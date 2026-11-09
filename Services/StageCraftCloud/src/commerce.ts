// Buying a listing (ADR 0032 §3.1, §3.4): the server reads the price from its own listing, creates the order with the share
// stored on it, and opens a hosted checkout. Nothing here marks an order paid; only a verified webhook does (webhooks.ts).
import { randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { UniqueViolation, type Queryable } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { getListing, hasActiveEntitlement } from './listings.ts';
import { splitSale } from './money.ts';
import { applyTransition, getOrder, rowToOrder, type Order } from './orders.ts';
import { ProviderFailure } from './payments/provider.ts';

const IDEMPOTENCY_KEY = /^[A-Za-z0-9_-]{8,128}$/;

async function findByIdempotencyKey(q: Queryable, buyerId: string, key: string): Promise<Order | null> {
  const rows = await q.query('SELECT * FROM orders WHERE buyer_id = $1 AND idempotency_key = $2', [buyerId, key]);
  return rows.length > 0 ? rowToOrder(rows[0]!) : null;
}

async function findOpenOrder(q: Queryable, buyerId: string, listingId: string): Promise<Order | null> {
  const rows = await q.query("SELECT * FROM orders WHERE buyer_id = $1 AND listing_id = $2 AND state IN ('Created', 'CheckoutOpen')", [
    buyerId,
    listingId,
  ]);
  return rows.length > 0 ? rowToOrder(rows[0]!) : null;
}

type Decision = { kind: 'existing'; order: Order } | { kind: 'created'; order: Order; title: string };

/**
 * POST /v1/orders. Idempotent per buyer and key: a repeat returns the same order and checkout URL. An open checkout for the same
 * listing is returned rather than duplicated. Refuses free and credit-priced listings (NotForSale), owned ones (AlreadyOwned), and
 * a key reused for another listing (IdempotencyConflict).
 */
export async function createOrder(ctx: AppContext, buyerId: string, listingId: string, idempotencyKey: string): Promise<Order> {
  if (!IDEMPOTENCY_KEY.test(idempotencyKey)) {
    throw new ApiError('InvalidRequest', 'idempotencyKey must be 8-128 letters, digits, - or _');
  }
  const now = ctx.clock.now();
  const decision = await ctx.db.withTransaction(async (tx): Promise<Decision> => {
    const repeat = await findByIdempotencyKey(tx, buyerId, idempotencyKey);
    if (repeat) {
      if (repeat.listingId !== listingId) {
        throw new ApiError('IdempotencyConflict', 'this idempotencyKey was already used for another listing');
      }
      return { kind: 'existing', order: repeat };
    }
    const listing = await getListing(tx, listingId);
    if (!listing || listing.status !== 'Approved') {
      throw new ApiError('NotFound', 'no such listing');
    }
    if (listing.realMoneyMinor <= 0n) {
      throw new ApiError('NotForSale', 'this listing has no real-money price; free listings are acquired directly');
    }
    if (!ctx.config.settlementCurrencies.includes(listing.currency)) {
      throw new ApiError('CurrencyNotSupported', 'this listing is priced in a currency the store no longer settles');
    }
    if (listing.authorId === buyerId || (await hasActiveEntitlement(tx, buyerId, listing.id))) {
      throw new ApiError('AlreadyOwned', 'you already own this listing');
    }
    const open = await findOpenOrder(tx, buyerId, listing.id);
    if (open) {
      if (open.state === 'CheckoutOpen' && open.checkoutExpiresAt !== null && open.checkoutExpiresAt <= now) {
        // The provider has not reported the expiry yet; a new checkout now could be paid twice.
        throw new ApiError('PurchasePending', 'the previous checkout for this listing is closing; try again in a minute');
      }
      return { kind: 'existing', order: open };
    }
    const shareBps = ctx.config.sellerShareBps;
    const split = splitSale(listing.realMoneyMinor, shareBps);
    const id = randomUUID();
    try {
      await tx.query(
        `INSERT INTO orders (id, buyer_id, listing_id, seller_id, idempotency_key, state, amount_minor, currency, share_bps, seller_amount,
           platform_amount, created_at, updated_at)
         VALUES ($1, $2, $3, $4, $5, 'Created', $6, $7, $8, $9, $10, $11, $11)`,
        [id, buyerId, listing.id, listing.authorId, idempotencyKey, listing.realMoneyMinor, listing.currency, shareBps, split.seller, split.platform, now],
      );
    } catch (error) {
      if (error instanceof UniqueViolation) {
        throw new ApiError('PurchasePending', 'another order for this listing is being created; try again');
      }
      throw error;
    }
    return { kind: 'created', order: (await getOrder(tx, id))!, title: listing.title };
  });

  if (decision.kind === 'existing' && decision.order.state !== 'Created') {
    return decision.order;
  }
  // A new order, or one left in Created by a crash between the two transactions: (re)open its checkout. The provider call uses the
  // order id as its idempotency key, so a retry returns the same session.
  const order = decision.order;
  const title = decision.kind === 'created' ? decision.title : ((await getListing(ctx.db, order.listingId))?.title ?? 'StageCraft listing');
  return openCheckout(ctx, order, title);
}

async function openCheckout(ctx: AppContext, order: Order, title: string): Promise<Order> {
  const expiresAt = ctx.clock.now() + ctx.config.checkoutExpiresMinutes * 60_000;
  try {
    const session = await ctx.provider.createCheckout({
      orderId: order.id,
      amountMinor: order.amountMinor,
      currency: order.currency,
      title,
      expiresAt,
      successUrl: `${ctx.config.checkoutSuccessUrl}?order=${order.id}`,
      cancelUrl: `${ctx.config.checkoutCancelUrl}?order=${order.id}`,
    });
    const updated = await ctx.db.withTransaction(async (tx) => {
      const current = (await getOrder(tx, order.id))!;
      if (current.state !== 'Created') {
        return current;
      }
      return applyTransition(tx, {
        order: current,
        event: 'checkout_opened',
        source: 'api',
        at: ctx.clock.now(),
        set: { checkout_session_id: session.sessionId, checkout_url: session.url, checkout_expires_at: session.expiresAt },
      });
    });
    ctx.notifier.notify(order.id);
    return updated;
  } catch (error) {
    if (!(error instanceof ProviderFailure)) {
      throw error;
    }
    ctx.log.warn('checkout creation failed', { orderId: order.id, status: error.status, providerCode: error.providerCode });
    await ctx.db.withTransaction(async (tx) => {
      const current = (await getOrder(tx, order.id))!;
      if (current.state === 'Created') {
        await applyTransition(tx, { order: current, event: 'checkout_failed', source: 'api', at: ctx.clock.now() });
      }
    });
    ctx.notifier.notify(order.id);
    throw new ApiError('ProviderError', 'the payment provider is not available; nothing was charged, try again');
  }
}

export async function getBuyerOrder(ctx: AppContext, buyerId: string, orderId: string): Promise<Order> {
  const order = /^[0-9a-f-]{36}$/.test(orderId) ? await getOrder(ctx.db, orderId) : null;
  if (!order || order.buyerId !== buyerId) {
    throw new ApiError('NotFound', 'no such order');
  }
  return order;
}

/** Long poll: answers as soon as the order's state differs from the state at the start, or after waitSeconds. */
export async function waitForOrderChange(ctx: AppContext, buyerId: string, orderId: string, waitSeconds: number): Promise<Order> {
  const first = await getBuyerOrder(ctx, buyerId, orderId);
  const deadline = ctx.clock.now() + waitSeconds * 1000;
  let current = first;
  while (current.state === first.state) {
    const remaining = deadline - ctx.clock.now();
    if (remaining <= 0) {
      break;
    }
    // Woken by a change committed in this process; the 2 s cap picks up changes another replica committed.
    await ctx.notifier.wait(orderId, Math.min(remaining, 2000));
    current = await getBuyerOrder(ctx, buyerId, orderId);
  }
  return current;
}

export async function listBuyerOrders(ctx: AppContext, buyerId: string): Promise<Order[]> {
  const rows = await ctx.db.query('SELECT * FROM orders WHERE buyer_id = $1 ORDER BY created_at DESC, id LIMIT 500', [buyerId]);
  return rows.map(rowToOrder);
}

export async function listEntitlements(ctx: AppContext, accountId: string): Promise<{ listingId: string; orderId: string; grantedAt: number }[]> {
  const rows = await ctx.db.query(
    'SELECT listing_id, order_id, granted_at FROM entitlements WHERE account_id = $1 AND revoked_at IS NULL ORDER BY granted_at, listing_id',
    [accountId],
  );
  return rows.map((row) => ({ listingId: String(row.listing_id), orderId: String(row.order_id), grantedAt: Number(row.granted_at) }));
}
