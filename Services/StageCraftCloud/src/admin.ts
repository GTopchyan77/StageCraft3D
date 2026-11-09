// Admin operations (ADR 0032 §4.1): summary, order search, refunds, moderation. Every mutation writes audit_log in its transaction.
import { randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { big, num, str, type Row } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { writeAudit } from './audit.ts';
import { getListing, listingToJson, rowToListing, type Listing } from './listings.ts';
import { getOrder, isOrderState, orderToJson, refundedTotal, rowToOrder } from './orders.ts';
import { parseAmount } from './money.ts';
import { ProviderFailure } from './payments/provider.ts';

function parseTime(value: string | null, fallback: number): number {
  if (value === null || value === '') {
    return fallback;
  }
  if (/^[0-9]{1,15}$/.test(value)) {
    return Number(value);
  }
  const parsed = Date.parse(value);
  if (Number.isNaN(parsed)) {
    throw new ApiError('InvalidRequest', 'from and to must be ISO 8601 times or epoch milliseconds');
  }
  return parsed;
}

export interface CurrencySummary {
  currency: string;
  grossSales: bigint;
  refunds: bigint;
  platformRevenue: bigint;
  sellerEarnings: bigint;
  disputesOpened: bigint;
  disputesWon: bigint;
  disputesLost: bigint;
  fees: bigint;
  payouts: bigint;
  salesCount: number;
}

/**
 * GET /v1/admin/summary?from&to. Money figures per currency from the journal entries posted in [from, to):
 * grossSales - refunds = platformRevenue + sellerEarnings always holds (refunds include lost disputes).
 */
export async function summary(ctx: AppContext, fromText: string | null, toText: string | null): Promise<Record<string, unknown>> {
  const from = parseTime(fromText, 0);
  const to = parseTime(toText, ctx.clock.now() + 1);
  const accountClass = "CASE WHEN substr(l.account, 1, 7) = 'seller:' THEN 'seller' ELSE l.account END";
  const rows = await ctx.db.query(
    `SELECT l.currency AS currency, e.kind AS kind, ${accountClass} AS account_class, SUM(l.amount) AS total, COUNT(DISTINCT e.id) AS entries
     FROM journal_lines l JOIN journal_entries e ON e.id = l.entry_id
     WHERE e.created_at >= $1 AND e.created_at < $2
     GROUP BY l.currency, e.kind, ${accountClass}`,
    [from, to],
  );
  const byCurrency = new Map<string, CurrencySummary>();
  for (const row of rows) {
    const currency = str(row.currency);
    const s =
      byCurrency.get(currency) ??
      { currency, grossSales: 0n, refunds: 0n, platformRevenue: 0n, sellerEarnings: 0n, disputesOpened: 0n, disputesWon: 0n, disputesLost: 0n, fees: 0n, payouts: 0n, salesCount: 0 };
    const kind = str(row.kind);
    const account = str(row.account_class);
    const total = big(row.total);
    if (kind === 'sale' && account === 'cash:stripe') {
      s.grossSales += total;
      s.salesCount += num(row.entries);
    }
    if (kind === 'refund' && account === 'cash:stripe') s.refunds -= total;
    if (kind === 'dispute_lost' && account === 'disputes') {
      s.refunds -= total;
      s.disputesLost -= total;
    }
    if (account === 'platform:revenue' || account === 'refunds') s.platformRevenue -= total;
    if (account === 'seller' && (kind === 'sale' || kind === 'refund' || kind === 'dispute_lost')) s.sellerEarnings -= total;
    if (kind === 'dispute_opened' && account === 'disputes') s.disputesOpened += total;
    if (kind === 'dispute_won' && account === 'disputes') s.disputesWon -= total;
    if (account === 'platform:fees') s.fees += total;
    if (kind === 'payout_settled' && account === 'cash:stripe') s.payouts -= total;
    byCurrency.set(currency, s);
  }
  const count = async (sql: string): Promise<number> => num((await ctx.db.query(sql))[0]!.n);
  return {
    from: new Date(from).toISOString(),
    to: new Date(to).toISOString(),
    currencies: [...byCurrency.values()].sort((a, b) => (a.currency < b.currency ? -1 : 1)),
    openModeration: await count("SELECT COUNT(*) AS n FROM listings WHERE status = 'PendingReview'"),
    openTickets: await count("SELECT COUNT(*) AS n FROM support_tickets WHERE status IN ('open', 'in_progress')"),
    ordersOnHold: await count("SELECT COUNT(*) AS n FROM orders WHERE state = 'OnHold'"),
    openDisputes: await count("SELECT COUNT(*) AS n FROM orders WHERE state = 'Disputed'"),
    webhooksNeedingAttention: await count("SELECT COUNT(*) AS n FROM webhook_events WHERE status = 'needs_attention'"),
  };
}

export async function searchOrders(ctx: AppContext, params: URLSearchParams): Promise<Record<string, unknown>[]> {
  const clauses: string[] = [];
  const values: (string | number)[] = [];
  const state = params.get('state');
  if (state) {
    if (!isOrderState(state)) {
      throw new ApiError('InvalidRequest', 'unknown order state');
    }
    values.push(state);
    clauses.push(`state = $${values.length}`);
  }
  for (const [param, column] of [
    ['buyerId', 'buyer_id'],
    ['sellerId', 'seller_id'],
    ['listingId', 'listing_id'],
    ['orderId', 'id'],
  ] as const) {
    const value = params.get(param);
    if (value) {
      values.push(value.slice(0, 64));
      clauses.push(`${column} = $${values.length}`);
    }
  }
  const limit = Math.min(500, Math.max(1, Number(params.get('limit') ?? 100) || 100));
  const offset = Math.max(0, Number(params.get('offset') ?? 0) || 0);
  values.push(limit, offset);
  const rows = await ctx.db.query(
    `SELECT * FROM orders ${clauses.length ? `WHERE ${clauses.join(' AND ')}` : ''} ORDER BY created_at DESC, id LIMIT $${values.length - 1} OFFSET $${values.length}`,
    values,
  );
  return rows.map((row) => {
    const order = rowToOrder(row);
    return {
      ...orderToJson(order),
      buyerId: order.buyerId,
      sellerId: order.sellerId,
      shareBps: Number(order.shareBps),
      sellerAmountMinor: order.sellerAmount,
      platformAmountMinor: order.platformAmount,
      sellerRefundedMinor: order.sellerRefunded,
      platformRefundedMinor: order.platformRefunded,
      paymentIntentId: order.paymentIntentId,
      holdReason: order.holdReason,
      releasedAt: order.releasedAt === null ? null : new Date(order.releasedAt).toISOString(),
    };
  });
}

/**
 * POST /v1/admin/orders/{id}/refund {amountMinor?, reason?}: asks the provider for a full or partial refund. The ledger, the order
 * state and the entitlement change only when the provider's charge.refunded webhook arrives. Refunds already requested but not yet
 * confirmed count against what is left, so two admins can never over-refund.
 */
export async function requestRefund(ctx: AppContext, adminId: string, orderId: string, body: Record<string, unknown>): Promise<Record<string, unknown>> {
  const reason = body.reason === undefined ? null : body.reason;
  if (reason !== null && (typeof reason !== 'string' || reason.length > 500)) {
    throw new ApiError('InvalidRequest', 'reason must be text of at most 500 characters');
  }
  const requested = body.amountMinor === undefined || body.amountMinor === null ? null : parseAmount(body.amountMinor);
  if (body.amountMinor !== undefined && body.amountMinor !== null && requested === null) {
    throw new ApiError('InvalidRequest', 'amountMinor must be a whole number of minor units');
  }
  const now = ctx.clock.now();
  const prepared = await ctx.db.withTransaction(async (tx) => {
    const order = await getOrder(tx, orderId);
    if (!order) {
      throw new ApiError('NotFound', 'no such order');
    }
    if (!['Paid', 'PartiallyRefunded', 'OnHold'].includes(order.state) || order.paymentIntentId === null) {
      throw new ApiError('IllegalTransition', `an order in state ${order.state} cannot be refunded`);
    }
    const pending = big((await tx.query("SELECT COALESCE(SUM(amount), 0) AS total FROM refund_requests WHERE order_id = $1 AND status = 'submitted'", [order.id]))[0]!.total);
    const remaining = order.amountMinor - refundedTotal(order) - pending;
    const amount = requested ?? remaining;
    if (amount <= 0n || amount > remaining || (order.state === 'OnHold' && amount !== order.amountMinor)) {
      throw new ApiError('RefundExceedsCharge', `at most ${remaining} can be refunded now (a held order only in full)`);
    }
    const id = randomUUID();
    await tx.query(
      "INSERT INTO refund_requests (id, order_id, amount, status, requested_by, reason, created_at, updated_at) VALUES ($1, $2, $3, 'submitted', $4, $5, $6, $6)",
      [id, order.id, amount, adminId, reason, now],
    );
    await writeAudit(tx, { actorId: adminId, action: 'order.refund', target: `order:${order.id}`, before: { state: order.state, refunded: refundedTotal(order) }, after: { refundRequestId: id, amount, reason }, at: now });
    return { id, amount, paymentIntentId: order.paymentIntentId, orderId: order.id };
  });
  try {
    const refund = await ctx.provider.createRefund({ paymentIntentId: prepared.paymentIntentId, amountMinor: prepared.amount, idempotencyKey: prepared.id, orderId: prepared.orderId });
    await ctx.db.query('UPDATE refund_requests SET provider_refund_id = $1, updated_at = $2 WHERE id = $3', [refund.refundId, ctx.clock.now(), prepared.id]);
    return { refundRequestId: prepared.id, status: 'submitted', amountMinor: prepared.amount, providerRefundId: refund.refundId };
  } catch (error) {
    if (!(error instanceof ProviderFailure)) throw error;
    await ctx.db.query("UPDATE refund_requests SET status = 'failed', updated_at = $1 WHERE id = $2", [ctx.clock.now(), prepared.id]);
    ctx.log.warn('refund refused by the provider', { orderId: prepared.orderId, status: error.status, providerCode: error.providerCode });
    throw new ApiError('ProviderError', 'the payment provider did not accept the refund; nothing changed');
  }
}

export async function moderationQueue(ctx: AppContext): Promise<Record<string, unknown>[]> {
  const rows = await ctx.db.query("SELECT * FROM listings WHERE status = 'PendingReview' ORDER BY created_at, id LIMIT 500");
  return rows.map((row: Row) => {
    const listing = rowToListing(row);
    return { ...listingToJson(listing, true), authorId: listing.authorId, requiredItems: JSON.parse(str(row.required_items_json)) as string[] };
  });
}

/** POST /v1/admin/moderation/{id} {decision: approve | reject, reason}. A rejection needs a reason the author is shown. */
export async function moderate(ctx: AppContext, adminId: string, listingId: string, body: Record<string, unknown>): Promise<Listing> {
  const decision = body.decision;
  const reason = typeof body.reason === 'string' ? body.reason.trim() : '';
  if (decision !== 'approve' && decision !== 'reject') {
    throw new ApiError('InvalidRequest', 'decision must be approve or reject');
  }
  if ((decision === 'reject' && reason.length === 0) || reason.length > 500) {
    throw new ApiError('InvalidRequest', 'a rejection needs a reason of at most 500 characters');
  }
  const now = ctx.clock.now();
  return ctx.db.withTransaction(async (tx) => {
    const before = await getListing(tx, listingId);
    if (!before) {
      throw new ApiError('NotFound', 'no such listing');
    }
    const allowed = decision === 'approve' ? ['PendingReview', 'Rejected'] : ['PendingReview', 'Approved'];
    if (!allowed.includes(before.status)) {
      throw new ApiError('IllegalTransition', `a listing in state ${before.status} cannot be ${decision}d`);
    }
    if (decision === 'approve') {
      await tx.query("UPDATE listings SET status = 'Approved', review_reason = NULL, published_at = COALESCE(published_at, $1) WHERE id = $2", [now, listingId]);
    } else {
      await tx.query("UPDATE listings SET status = 'Rejected', review_reason = $1 WHERE id = $2", [reason, listingId]);
    }
    const after = (await getListing(tx, listingId))!;
    await writeAudit(tx, {
      actorId: adminId,
      action: `listing.${decision}`,
      target: `listing:${listingId}`,
      before: { status: before.status, reviewReason: before.reviewReason },
      after: { status: after.status, reviewReason: after.reviewReason },
      at: now,
    });
    return after;
  });
}
