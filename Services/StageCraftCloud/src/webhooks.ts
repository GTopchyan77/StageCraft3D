// Stripe webhooks (ADR 0032 §3.5): the only path that settles money. The signature is verified on the raw bytes before parsing;
// each event id is stored once; the event row, the order transition, the journal entry and the entitlement commit in one
// transaction or not at all. Business refusals (unknown order, illegal transition, amount mismatch) are stored, answered 200 and
// flagged for the admin; unexpected errors roll everything back and answer 500 so Stripe retries.
import { ApiError } from './errors.ts';
import { big, num, str, type Queryable } from './db/driver.ts';
import type { AppContext } from './context.ts';
import {
  Accounts,
  disputeOpenedLines,
  disputeWonLines,
  feeLines,
  postEntry,
  refundLines,
  saleLines,
} from './ledger.ts';
import { apportionRefund } from './money.ts';
import { applyTransition, findOrderBy, getOrder, IllegalTransitionError, refundedTotal, type Order } from './orders.ts';
import { verifyStripeSignature } from './payments/webhookSignature.ts';

export type WebhookStatus = 'processed' | 'ignored' | 'needs_attention';

export interface WebhookOutcome {
  status: WebhookStatus | 'duplicate';
  detail: string;
  orderIds: string[];
}

interface HandlerResult {
  status: WebhookStatus;
  detail: string;
  orderIds?: string[];
}

type StripeObject = Record<string, unknown>;

interface StripeEvent {
  id: string;
  type: string;
  object: StripeObject;
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function parseEvent(rawBody: Buffer): StripeEvent {
  let event: unknown;
  try {
    event = JSON.parse(rawBody.toString('utf8'));
  } catch {
    throw new ApiError('InvalidRequest', 'webhook body is not JSON');
  }
  const record = event as Record<string, unknown> | null;
  const data = record?.data as Record<string, unknown> | undefined;
  if (
    typeof record?.id !== 'string' ||
    record.id.length === 0 ||
    record.id.length > 255 ||
    typeof record.type !== 'string' ||
    typeof data?.object !== 'object' ||
    data.object === null
  ) {
    throw new ApiError('InvalidRequest', 'webhook body is not a Stripe event');
  }
  return { id: record.id, type: record.type, object: data.object as StripeObject };
}

function text(object: StripeObject, field: string): string | null {
  const value = object[field];
  return typeof value === 'string' && value.length > 0 ? value : null;
}

/** A Stripe integer amount (minor units), or null. */
function amount(object: StripeObject, field: string): bigint | null {
  const value = object[field];
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 ? BigInt(value) : null;
}

function currency(object: StripeObject): string | null {
  const value = text(object, 'currency');
  return value ? value.toUpperCase() : null;
}

/** Stripe sometimes sends an expanded object where an id is expected. */
function idOf(object: StripeObject, field: string): string | null {
  const value = object[field];
  if (typeof value === 'string') return value;
  if (typeof value === 'object' && value !== null && typeof (value as StripeObject).id === 'string') return (value as StripeObject).id as string;
  return null;
}

async function grantEntitlement(tx: Queryable, order: Order, at: number): Promise<void> {
  await tx.query(
    `INSERT INTO entitlements (account_id, listing_id, order_id, granted_at) VALUES ($1, $2, $3, $4)
     ON CONFLICT (account_id, listing_id) DO UPDATE SET order_id = excluded.order_id, granted_at = excluded.granted_at, revoked_at = NULL`,
    [order.buyerId, order.listingId, order.id, at],
  );
}

async function revokeEntitlement(tx: Queryable, order: Order, at: number): Promise<void> {
  await tx.query('UPDATE entitlements SET revoked_at = $1 WHERE order_id = $2 AND revoked_at IS NULL', [at, order.id]);
}

function sellerAccountFor(order: Order): string {
  return order.releasedAt === null ? Accounts.sellerPending(order.sellerId) : Accounts.sellerAvailable(order.sellerId);
}

async function orderForSession(tx: Queryable, session: StripeObject): Promise<Order | null> {
  const metadata = (session.metadata ?? {}) as StripeObject;
  const orderId = text(session, 'client_reference_id') ?? text(metadata, 'orderId');
  if (orderId && UUID.test(orderId)) {
    const order = await getOrder(tx, orderId);
    if (order) return order;
  }
  const sessionId = text(session, 'id');
  return sessionId ? findOrderBy(tx, 'checkout_session_id', sessionId) : null;
}

async function orderForCharge(tx: Queryable, object: StripeObject): Promise<Order | null> {
  const paymentIntent = idOf(object, 'payment_intent');
  return paymentIntent ? findOrderBy(tx, 'payment_intent_id', paymentIntent) : null;
}

/** Marks a checkout's order paid: amount and currency must equal the order, else the order is held and nothing is granted. */
async function settlePaid(ctx: AppContext, tx: Queryable, order: Order, session: StripeObject, at: number): Promise<HandlerResult> {
  const paymentIntent = idOf(session, 'payment_intent');
  if (order.state !== 'CheckoutOpen') {
    if (order.paymentIntentId !== null && order.paymentIntentId === paymentIntent && order.paidAt !== null) {
      return { status: 'ignored', detail: 'order already settled', orderIds: [order.id] };
    }
    return { status: 'needs_attention', detail: `payment for an order in state ${order.state}`, orderIds: [order.id] };
  }
  if (order.checkoutSessionId !== null && order.checkoutSessionId !== text(session, 'id')) {
    return { status: 'needs_attention', detail: 'checkout session does not belong to this order', orderIds: [order.id] };
  }
  const paid = amount(session, 'amount_total');
  const paidCurrency = currency(session);
  if (paid !== order.amountMinor || paidCurrency !== order.currency) {
    await applyTransition(tx, {
      order,
      event: 'amount_mismatch',
      source: 'webhook',
      at,
      set: { hold_reason: `paid ${paid ?? '?'} ${paidCurrency ?? '?'}, expected ${order.amountMinor} ${order.currency}`, payment_intent_id: paymentIntent },
    });
    ctx.log.error('payment amount mismatch; order on hold', { orderId: order.id });
    return { status: 'needs_attention', detail: 'amount or currency mismatch; order on hold', orderIds: [order.id] };
  }
  const updated = await applyTransition(tx, {
    order,
    event: 'payment_succeeded',
    source: 'webhook',
    at,
    set: { paid_at: at, payment_intent_id: paymentIntent },
  });
  await postEntry(tx, {
    ref: `sale:${order.id}`,
    kind: 'sale',
    orderId: order.id,
    at,
    lines: saleLines(order.sellerId, order.currency, order.sellerAmount, order.platformAmount),
  });
  await grantEntitlement(tx, updated, at);
  return { status: 'processed', detail: 'order paid', orderIds: [order.id] };
}

async function onCheckoutCompleted(ctx: AppContext, tx: Queryable, session: StripeObject, at: number): Promise<HandlerResult> {
  const order = await orderForSession(tx, session);
  if (!order) {
    return { status: 'needs_attention', detail: 'checkout for an unknown order' };
  }
  const paymentStatus = text(session, 'payment_status');
  if (paymentStatus === 'paid') {
    return settlePaid(ctx, tx, order, session, at);
  }
  if (paymentStatus === 'unpaid') {
    // A delayed payment method: async_payment_succeeded or _failed follows. Remember the payment intent for refunds and disputes.
    await tx.query("UPDATE orders SET payment_intent_id = $1, updated_at = $2 WHERE id = $3 AND state = 'CheckoutOpen'", [
      idOf(session, 'payment_intent'),
      at,
      order.id,
    ]);
    return { status: 'processed', detail: 'awaiting asynchronous payment', orderIds: [order.id] };
  }
  return { status: 'needs_attention', detail: `unexpected payment_status ${paymentStatus ?? 'missing'}`, orderIds: [order.id] };
}

async function onAsyncPayment(ctx: AppContext, tx: Queryable, session: StripeObject, succeeded: boolean, at: number): Promise<HandlerResult> {
  const order = await orderForSession(tx, session);
  if (!order) {
    return { status: 'needs_attention', detail: 'asynchronous payment for an unknown order' };
  }
  if (succeeded) {
    return settlePaid(ctx, tx, order, session, at);
  }
  await applyTransition(tx, { order, event: 'payment_failed', source: 'webhook', at });
  return { status: 'processed', detail: 'asynchronous payment failed', orderIds: [order.id] };
}

async function onCheckoutExpired(tx: Queryable, session: StripeObject, at: number): Promise<HandlerResult> {
  const order = await orderForSession(tx, session);
  if (!order) {
    return { status: 'ignored', detail: 'expiry of an unknown checkout' };
  }
  await applyTransition(tx, { order, event: 'checkout_expired', source: 'webhook', at });
  return { status: 'processed', detail: 'checkout expired', orderIds: [order.id] };
}

/** charge.refunded carries the cumulative amount_refunded; the difference to what is posted is the new refund. */
async function onChargeRefunded(ctx: AppContext, tx: Queryable, charge: StripeObject, at: number): Promise<HandlerResult> {
  const order = await orderForCharge(tx, charge);
  if (!order) {
    return { status: 'ignored', detail: 'refund of an unknown charge' };
  }
  const cumulative = amount(charge, 'amount_refunded');
  if (cumulative === null || currency(charge) !== order.currency || cumulative > order.amountMinor) {
    return { status: 'needs_attention', detail: 'refund amount or currency does not fit the order', orderIds: [order.id] };
  }
  if (order.state === 'OnHold') {
    if (cumulative !== order.amountMinor) {
      return { status: 'needs_attention', detail: 'partial refund of a held order', orderIds: [order.id] };
    }
    // Nothing was posted or granted for a held order, so its full refund moves no ledger money.
    await applyTransition(tx, { order, event: 'refunded_fully', source: 'webhook', at });
    await settleRefundRequests(tx, order.id, at);
    return { status: 'processed', detail: 'held order refunded', orderIds: [order.id] };
  }
  const delta = cumulative - refundedTotal(order);
  if (delta <= 0n) {
    return { status: 'ignored', detail: 'refund already posted', orderIds: [order.id] };
  }
  if (order.state !== 'Paid' && order.state !== 'PartiallyRefunded') {
    return { status: 'needs_attention', detail: `refund of an order in state ${order.state}`, orderIds: [order.id] };
  }
  const split = apportionRefund(
    { price: order.amountMinor, shareBps: order.shareBps, sellerRefunded: order.sellerRefunded, platformRefunded: order.platformRefunded },
    delta,
  );
  const chargeId = text(charge, 'id') ?? 'unknown';
  const full = cumulative === order.amountMinor;
  await applyTransition(tx, {
    order,
    event: full ? 'refunded_fully' : 'refunded_partially',
    source: 'webhook',
    at,
    set: { seller_refunded: order.sellerRefunded + split.seller, platform_refunded: order.platformRefunded + split.platform },
  });
  await postEntry(tx, {
    ref: `refund:${chargeId}:${cumulative}`,
    kind: 'refund',
    orderId: order.id,
    at,
    lines: refundLines(sellerAccountFor(order), order.currency, split.seller, split.platform, Accounts.cash),
  });
  if (full) {
    await revokeEntitlement(tx, order, at);
  }
  await settleRefundRequests(tx, order.id, at);
  return { status: 'processed', detail: full ? 'order refunded' : 'order partially refunded', orderIds: [order.id] };
}

async function settleRefundRequests(tx: Queryable, orderId: string, at: number): Promise<void> {
  await tx.query("UPDATE refund_requests SET status = 'settled', updated_at = $1 WHERE order_id = $2 AND status = 'submitted'", [at, orderId]);
}

function isInquiry(status: string | null): boolean {
  return status !== null && status.startsWith('warning_');
}

async function onDisputeCreated(ctx: AppContext, tx: Queryable, dispute: StripeObject, at: number): Promise<HandlerResult> {
  const order = await orderForCharge(tx, dispute);
  const disputeId = text(dispute, 'id');
  const disputed = amount(dispute, 'amount');
  if (!order || !disputeId || disputed === null) {
    return { status: 'needs_attention', detail: 'dispute of an unknown charge' };
  }
  const status = text(dispute, 'status');
  const inquiry = isInquiry(status);
  // ON CONFLICT rather than catching a unique violation: on Postgres a failed statement would abort the whole transaction.
  const inserted = await tx.query(
    `INSERT INTO disputes (id, order_id, amount, currency, status, is_inquiry, created_at) VALUES ($1, $2, $3, $4, $5, $6, $7)
     ON CONFLICT (id) DO NOTHING RETURNING id`,
    [disputeId, order.id, disputed, currency(dispute) ?? order.currency, status ?? 'unknown', inquiry ? 1 : 0, at],
  );
  if (inserted.length === 0) {
    return { status: 'ignored', detail: 'dispute already recorded', orderIds: [order.id] };
  }
  if (inquiry) {
    // An inquiry withdraws no money; only a real dispute does.
    return { status: 'processed', detail: 'inquiry recorded', orderIds: [order.id] };
  }
  if (currency(dispute) !== order.currency || disputed > order.amountMinor - refundedTotal(order) || disputed === 0n) {
    return { status: 'needs_attention', detail: 'dispute amount does not fit the order', orderIds: [order.id] };
  }
  await applyTransition(tx, { order, event: 'dispute_opened', source: 'webhook', at });
  await postEntry(tx, { ref: `dispute_opened:${disputeId}`, kind: 'dispute_opened', orderId: order.id, at, lines: disputeOpenedLines(order.currency, disputed) });
  ctx.log.warn('dispute opened', { orderId: order.id });
  return { status: 'processed', detail: 'dispute opened', orderIds: [order.id] };
}

/** The dispute fee from the dispute's balance transactions, else the configured fallback. */
function disputeFee(ctx: AppContext, dispute: StripeObject): bigint {
  const transactions = Array.isArray(dispute.balance_transactions) ? (dispute.balance_transactions as StripeObject[]) : [];
  let fee = 0n;
  for (const transaction of transactions) {
    const value = typeof transaction === 'object' && transaction !== null ? transaction.fee : undefined;
    if (typeof value === 'number' && Number.isSafeInteger(value)) {
      fee += BigInt(value);
    }
  }
  return fee > 0n ? fee : ctx.config.disputeFeeFallbackMinor;
}

async function onDisputeClosed(ctx: AppContext, tx: Queryable, dispute: StripeObject, at: number): Promise<HandlerResult> {
  const disputeId = text(dispute, 'id');
  const rows = disputeId ? await tx.query('SELECT * FROM disputes WHERE id = $1', [disputeId]) : [];
  const record = rows[0];
  if (!disputeId || !record) {
    return { status: 'needs_attention', detail: 'closure of an unknown dispute' };
  }
  const order = (await getOrder(tx, str(record.order_id)))!;
  const status = text(dispute, 'status');
  if (record.closed_at !== null) {
    return { status: 'ignored', detail: 'dispute already closed', orderIds: [order.id] };
  }
  await tx.query('UPDATE disputes SET status = $1, closed_at = $2 WHERE id = $3', [status ?? 'unknown', at, disputeId]);
  if (num(record.is_inquiry) === 1) {
    return { status: 'processed', detail: 'inquiry closed', orderIds: [order.id] };
  }
  const disputed = big(record.amount);
  if (status === 'won') {
    await applyTransition(tx, { order, event: 'dispute_won', source: 'webhook', at, context: { refundedBefore: refundedTotal(order) > 0n } });
    await postEntry(tx, { ref: `dispute_won:${disputeId}`, kind: 'dispute_won', orderId: order.id, at, lines: disputeWonLines(order.currency, disputed) });
    return { status: 'processed', detail: 'dispute won', orderIds: [order.id] };
  }
  if (status === 'lost') {
    const split = apportionRefund(
      { price: order.amountMinor, shareBps: order.shareBps, sellerRefunded: order.sellerRefunded, platformRefunded: order.platformRefunded },
      disputed,
    );
    const full = refundedTotal(order) + disputed === order.amountMinor;
    await applyTransition(tx, {
      order,
      event: full ? 'dispute_lost_fully' : 'dispute_lost_partially',
      source: 'webhook',
      at,
      set: { seller_refunded: order.sellerRefunded + split.seller, platform_refunded: order.platformRefunded + split.platform },
    });
    await postEntry(tx, {
      ref: `dispute_lost:${disputeId}`,
      kind: 'dispute_lost',
      orderId: order.id,
      at,
      lines: refundLines(sellerAccountFor(order), order.currency, split.seller, split.platform, Accounts.disputes),
    });
    const fee = disputeFee(ctx, dispute);
    if (fee > 0n) {
      await postEntry(tx, { ref: `dispute_fee:${disputeId}`, kind: 'dispute_fee', orderId: order.id, at, lines: feeLines(order.currency, fee) });
    }
    if (full) {
      await revokeEntitlement(tx, order, at);
    }
    return { status: 'processed', detail: 'dispute lost', orderIds: [order.id] };
  }
  return { status: 'needs_attention', detail: `dispute closed with status ${status ?? 'missing'}`, orderIds: [order.id] };
}

async function onAccountUpdated(tx: Queryable, account: StripeObject, at: number): Promise<HandlerResult> {
  const providerAccountId = text(account, 'id');
  if (!providerAccountId || typeof account.payouts_enabled !== 'boolean') {
    return { status: 'ignored', detail: 'account update without payouts_enabled' };
  }
  const rows = await tx.query(
    "UPDATE sellers SET payouts_enabled = $1, updated_at = $2 WHERE provider_account_id = $3 AND payout_method = 'stripe' RETURNING account_id",
    [account.payouts_enabled ? 1 : 0, at, providerAccountId],
  );
  return rows.length > 0 ? { status: 'processed', detail: 'seller payout status updated' } : { status: 'ignored', detail: 'unknown connected account' };
}

async function dispatch(ctx: AppContext, tx: Queryable, event: StripeEvent, at: number): Promise<HandlerResult> {
  try {
    switch (event.type) {
      case 'checkout.session.completed':
        return await onCheckoutCompleted(ctx, tx, event.object, at);
      case 'checkout.session.async_payment_succeeded':
        return await onAsyncPayment(ctx, tx, event.object, true, at);
      case 'checkout.session.async_payment_failed':
        return await onAsyncPayment(ctx, tx, event.object, false, at);
      case 'checkout.session.expired':
        return await onCheckoutExpired(tx, event.object, at);
      case 'charge.refunded':
        return await onChargeRefunded(ctx, tx, event.object, at);
      case 'charge.dispute.created':
        return await onDisputeCreated(ctx, tx, event.object, at);
      case 'charge.dispute.closed':
        return await onDisputeClosed(ctx, tx, event.object, at);
      case 'account.updated':
        return await onAccountUpdated(tx, event.object, at);
      default:
        return { status: 'ignored', detail: 'event type not handled' };
    }
  } catch (error) {
    if (error instanceof IllegalTransitionError) {
      ctx.log.warn('webhook refused an illegal order transition', { eventId: event.id, type: event.type, from: error.from, event: error.event });
      throw new Refusal(error.message);
    }
    if (error instanceof ApiError && error.code === 'RefundExceedsCharge') {
      throw new Refusal(error.message);
    }
    throw error;
  }
}

/** A business refusal discovered part-way through a handler: its writes are rolled back, the event is kept for the admin. */
class Refusal extends Error {}

/** Runs the handler under a savepoint, so a refusal keeps the event row but none of the handler's partial writes. */
async function dispatchAtomically(ctx: AppContext, tx: Queryable, event: StripeEvent, at: number): Promise<HandlerResult> {
  await tx.query('SAVEPOINT webhook_handler');
  try {
    const result = await dispatch(ctx, tx, event, at);
    await tx.query('RELEASE SAVEPOINT webhook_handler');
    return result;
  } catch (error) {
    if (!(error instanceof Refusal)) {
      throw error;
    }
    await tx.query('ROLLBACK TO SAVEPOINT webhook_handler');
    await tx.query('RELEASE SAVEPOINT webhook_handler');
    return { status: 'needs_attention', detail: error.message };
  }
}

/** Verifies, stores and applies one webhook delivery. Throws InvalidSignature (400, nothing stored) for a bad signature. */
export async function handleStripeWebhook(ctx: AppContext, rawBody: Buffer, signatureHeader: string | undefined): Promise<WebhookOutcome> {
  const check = verifyStripeSignature(rawBody, signatureHeader, ctx.config.payments.webhookSecrets, ctx.clock.now());
  if (!check.ok) {
    ctx.log.warn('webhook signature refused', { failure: check.failure });
    throw new ApiError('InvalidSignature', 'webhook signature verification failed');
  }
  const event = parseEvent(rawBody);
  const at = ctx.clock.now();
  const outcome = await ctx.db.withTransaction(async (tx): Promise<WebhookOutcome> => {
    const inserted = await tx.query(
      `INSERT INTO webhook_events (id, type, status, payload, received_at) VALUES ($1, $2, 'processed', $3, $4)
       ON CONFLICT (id) DO NOTHING RETURNING id`,
      [event.id, event.type, rawBody.toString('utf8'), at],
    );
    if (inserted.length === 0) {
      return { status: 'duplicate', detail: 'event already received', orderIds: [] };
    }
    const result = await dispatchAtomically(ctx, tx, event, at);
    await tx.query('UPDATE webhook_events SET status = $1, detail = $2 WHERE id = $3', [result.status, result.detail, event.id]);
    return { status: result.status, detail: result.detail, orderIds: result.orderIds ?? [] };
  });
  for (const orderId of outcome.orderIds) {
    ctx.notifier.notify(orderId);
  }
  const level = outcome.status === 'needs_attention' ? 'warn' : 'info';
  ctx.log[level]('webhook', { eventId: event.id, type: event.type, status: outcome.status, detail: outcome.detail, orders: outcome.orderIds });
  return outcome;
}
