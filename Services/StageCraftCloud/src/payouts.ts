// Seller balances, the hold period and payouts (ADR 0032 §3.1 step 4, §3.3). A payout run first releases sales whose hold period
// has passed (pending -> available), then reserves each seller's available balance (available -> paid) and pays it: by a Stripe
// transfer for Connect sellers, or as a manual batch an admin confirms (Armenia and other countries outside Connect).
import { randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { big, num, str, strOrNull, type Queryable } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { writeAudit } from './audit.ts';
import {
  Accounts,
  balance,
  payoutFailedLines,
  payoutReservedLines,
  payoutSettledLines,
  postEntry,
  releaseLines,
} from './ledger.ts';
import { rowToOrder } from './orders.ts';
import { ProviderFailure } from './payments/provider.ts';

const DAY_MS = 24 * 3600 * 1000;

/**
 * Moves the seller's remaining share of every sale older than the hold period from pending to available. A disputed order waits
 * until its dispute closes. Returns how many orders were released.
 */
export async function releaseMaturedSales(ctx: AppContext): Promise<number> {
  const now = ctx.clock.now();
  const cutoff = now - ctx.config.payoutHoldDays * DAY_MS;
  return ctx.db.withTransaction(async (tx) => {
    const rows = await tx.query(
      `SELECT * FROM orders WHERE released_at IS NULL AND paid_at IS NOT NULL AND paid_at <= $1
       AND state IN ('Paid', 'PartiallyRefunded', 'Refunded') ORDER BY paid_at, id`,
      [cutoff],
    );
    for (const row of rows) {
      const order = rowToOrder(row);
      const remaining = order.sellerAmount - order.sellerRefunded;
      if (remaining > 0n) {
        await postEntry(tx, {
          ref: `release:${order.id}`,
          kind: 'release',
          orderId: order.id,
          at: now,
          lines: releaseLines(order.sellerId, order.currency, remaining),
        });
      }
      await tx.query('UPDATE orders SET released_at = $1 WHERE id = $2 AND released_at IS NULL', [now, order.id]);
    }
    return rows.length;
  });
}

export interface SellerBalance {
  currency: string;
  pending: bigint;
  available: bigint;
  inTransit: bigint;
  paidOut: bigint;
}

/** Liabilities are credit (negative) balances in the ledger; shown to the seller as positive amounts. */
export async function sellerBalances(q: Queryable, sellerId: string): Promise<SellerBalance[]> {
  const rows = await q.query(
    `SELECT l.currency AS currency, l.account AS account, SUM(l.amount) AS total,
       SUM(CASE WHEN e.kind = 'payout_settled' THEN l.amount ELSE 0 END) AS settled
     FROM journal_lines l JOIN journal_entries e ON e.id = l.entry_id
     WHERE l.account IN ($1, $2, $3) GROUP BY l.currency, l.account ORDER BY l.currency`,
    [Accounts.sellerPending(sellerId), Accounts.sellerAvailable(sellerId), Accounts.sellerPaid(sellerId)],
  );
  const byCurrency = new Map<string, SellerBalance>();
  for (const row of rows) {
    const currency = str(row.currency);
    const entry = byCurrency.get(currency) ?? { currency, pending: 0n, available: 0n, inTransit: 0n, paidOut: 0n };
    const total = -big(row.total);
    const account = str(row.account);
    if (account === Accounts.sellerPending(sellerId)) entry.pending = total;
    else if (account === Accounts.sellerAvailable(sellerId)) entry.available = total;
    else {
      entry.inTransit = total;
      entry.paidOut = big(row.settled);
    }
    byCurrency.set(currency, entry);
  }
  return [...byCurrency.values()];
}

export async function sellerSales(q: Queryable, sellerId: string): Promise<Record<string, unknown>[]> {
  const rows = await q.query(
    `SELECT o.*, l.title AS title FROM orders o JOIN listings l ON l.id = o.listing_id
     WHERE o.seller_id = $1 AND o.paid_at IS NOT NULL ORDER BY o.paid_at DESC, o.id LIMIT 1000`,
    [sellerId],
  );
  return rows.map((row) => {
    const order = rowToOrder(row);
    return {
      orderId: order.id,
      listingId: order.listingId,
      title: str(row.title),
      state: order.state,
      currency: order.currency,
      amountMinor: order.amountMinor,
      sellerAmountMinor: order.sellerAmount,
      sellerRefundedMinor: order.sellerRefunded,
      paidAt: new Date(order.paidAt!).toISOString(),
      releasedAt: order.releasedAt === null ? null : new Date(order.releasedAt).toISOString(),
    };
  });
}

export interface Payout {
  id: string;
  sellerId: string;
  currency: string;
  amount: bigint;
  method: 'stripe' | 'manual';
  status: 'pending' | 'paid' | 'failed';
  providerTransferId: string | null;
  reference: string | null;
  failure: string | null;
  createdAt: number;
  completedAt: number | null;
}

function rowToPayout(row: Record<string, unknown>): Payout {
  return {
    id: str(row.id),
    sellerId: str(row.seller_id),
    currency: str(row.currency),
    amount: big(row.amount),
    method: str(row.method) as Payout['method'],
    status: str(row.status) as Payout['status'],
    providerTransferId: strOrNull(row.provider_transfer_id),
    reference: strOrNull(row.reference),
    failure: strOrNull(row.failure),
    createdAt: num(row.created_at),
    completedAt: row.completed_at === null ? null : num(row.completed_at),
  };
}

export function payoutToJson(payout: Payout): Record<string, unknown> {
  return {
    payoutId: payout.id,
    sellerId: payout.sellerId,
    currency: payout.currency,
    amountMinor: payout.amount,
    method: payout.method,
    status: payout.status,
    providerTransferId: payout.providerTransferId,
    reference: payout.reference,
    failure: payout.failure,
    createdAt: new Date(payout.createdAt).toISOString(),
    completedAt: payout.completedAt === null ? null : new Date(payout.completedAt).toISOString(),
  };
}

export async function listPayouts(q: Queryable, status: string | null): Promise<Payout[]> {
  const rows = status
    ? await q.query('SELECT * FROM payouts WHERE status = $1 ORDER BY created_at DESC, id LIMIT 500', [status])
    : await q.query('SELECT * FROM payouts ORDER BY created_at DESC, id LIMIT 500');
  return rows.map(rowToPayout);
}

export interface DueBalance {
  sellerId: string;
  currency: string;
  available: bigint;
  pending: bigint;
  method: 'stripe' | 'manual' | null;
  payoutsEnabled: boolean;
  country: string | null;
}

/** Every seller balance (available and pending) with the seller's payout method; what an admin sees before a run. */
export async function dueBalances(q: Queryable): Promise<DueBalance[]> {
  const rows = await q.query(
    `SELECT l.account AS account, l.currency AS currency, SUM(l.amount) AS total FROM journal_lines l
     WHERE substr(l.account, 1, 7) = 'seller:' GROUP BY l.account, l.currency ORDER BY l.account, l.currency`,
  );
  const map = new Map<string, DueBalance>();
  for (const row of rows) {
    const [, sellerId, bucket] = str(row.account).split(':');
    const key = `${sellerId}|${str(row.currency)}`;
    const entry = map.get(key) ?? { sellerId: sellerId!, currency: str(row.currency), available: 0n, pending: 0n, method: null, payoutsEnabled: false, country: null };
    if (bucket === 'available') entry.available = -big(row.total);
    if (bucket === 'pending') entry.pending = -big(row.total);
    map.set(key, entry);
  }
  const sellers = await q.query('SELECT account_id, payout_method, payouts_enabled, country FROM sellers');
  for (const seller of sellers) {
    for (const entry of map.values()) {
      if (entry.sellerId === str(seller.account_id)) {
        entry.method = str(seller.payout_method) as 'stripe' | 'manual';
        entry.payoutsEnabled = num(seller.payouts_enabled) === 1;
        entry.country = str(seller.country);
      }
    }
  }
  return [...map.values()].filter((entry) => entry.available !== 0n || entry.pending !== 0n);
}

/**
 * POST /v1/admin/payouts/run. Releases matured sales, then for each seller whose available balance reaches the minimum and whose
 * payouts are enabled, reserves the balance in its own transaction and pays it. Stripe payouts that were reserved but never
 * confirmed (a crash between reserve and transfer) are retried with the same idempotency key, so Stripe never pays twice.
 */
export async function runPayouts(ctx: AppContext, adminId: string): Promise<Payout[]> {
  await releaseMaturedSales(ctx);
  const created: Payout[] = [];
  for (const due of await dueBalances(ctx.db)) {
    if (!due.payoutsEnabled || due.method === null || due.available < ctx.config.payoutMinimumMinor) {
      continue;
    }
    const payout = await ctx.db.withTransaction(async (tx) => {
      // Re-read inside the transaction: the reserve must use the balance as it is now, not as listed above.
      const available = -(await balance(tx, Accounts.sellerAvailable(due.sellerId), due.currency));
      if (available < ctx.config.payoutMinimumMinor) {
        return null;
      }
      const id = randomUUID();
      const now = ctx.clock.now();
      await tx.query(
        `INSERT INTO payouts (id, seller_id, currency, amount, method, status, created_by, created_at)
         VALUES ($1, $2, $3, $4, $5, 'pending', $6, $7)`,
        [id, due.sellerId, due.currency, available, due.method, adminId, now],
      );
      await postEntry(tx, {
        ref: `payout_reserved:${id}`,
        kind: 'payout_reserved',
        payoutId: id,
        at: now,
        lines: payoutReservedLines(due.sellerId, due.currency, available),
      });
      await writeAudit(tx, { actorId: adminId, action: 'payout.create', target: `payout:${id}`, after: { sellerId: due.sellerId, currency: due.currency, amount: available, method: due.method }, at: now });
      return rowToPayout((await tx.query('SELECT * FROM payouts WHERE id = $1', [id]))[0]!);
    });
    if (payout) {
      created.push(payout);
    }
  }
  const pendingStripe = (await listPayouts(ctx.db, 'pending')).filter((payout) => payout.method === 'stripe');
  const results: Payout[] = [];
  for (const payout of pendingStripe) {
    results.push(await sendStripePayout(ctx, payout));
  }
  const manual = created.filter((payout) => payout.method === 'manual');
  return [...results, ...manual];
}

async function sendStripePayout(ctx: AppContext, payout: Payout): Promise<Payout> {
  const sellerRows = await ctx.db.query('SELECT provider_account_id FROM sellers WHERE account_id = $1', [payout.sellerId]);
  const destination = strOrNull(sellerRows[0]?.provider_account_id);
  let transferId: string | null = null;
  let failure: string | null = destination ? null : 'seller has no connected account';
  if (destination) {
    try {
      transferId = (
        await ctx.provider.createTransfer({ amountMinor: payout.amount, currency: payout.currency, destination, idempotencyKey: payout.id, payoutId: payout.id })
      ).transferId;
    } catch (error) {
      if (!(error instanceof ProviderFailure)) throw error;
      // A network failure or 5xx may have reached Stripe: keep the payout pending and retry it with the same key on the next run.
      if (error.status === 0 || error.status >= 500) {
        ctx.log.warn('payout transfer not confirmed; will retry', { payoutId: payout.id, status: error.status });
        return payout;
      }
      failure = `provider refused the transfer (${error.providerCode ?? error.status})`;
    }
  }
  const now = ctx.clock.now();
  await ctx.db.withTransaction(async (tx) => {
    if (transferId) {
      await tx.query("UPDATE payouts SET status = 'paid', provider_transfer_id = $1, completed_at = $2 WHERE id = $3 AND status = 'pending'", [transferId, now, payout.id]);
      await postEntry(tx, { ref: `payout_settled:${payout.id}`, kind: 'payout_settled', payoutId: payout.id, at: now, lines: payoutSettledLines(payout.sellerId, payout.currency, payout.amount) });
    } else {
      await tx.query("UPDATE payouts SET status = 'failed', failure = $1, completed_at = $2 WHERE id = $3 AND status = 'pending'", [failure, now, payout.id]);
      await postEntry(tx, { ref: `payout_failed:${payout.id}`, kind: 'payout_failed', payoutId: payout.id, at: now, lines: payoutFailedLines(payout.sellerId, payout.currency, payout.amount) });
    }
  });
  if (failure) {
    ctx.log.error('payout failed; balance returned to available', { payoutId: payout.id });
  }
  return rowToPayout((await ctx.db.query('SELECT * FROM payouts WHERE id = $1', [payout.id]))[0]!);
}

/** POST /v1/admin/payouts/{id}/mark-paid: confirms a manual payout after the money was sent outside Stripe. */
export async function markPayoutPaid(ctx: AppContext, adminId: string, payoutId: string, reference: string): Promise<Payout> {
  if (reference.length < 1 || reference.length > 200) {
    throw new ApiError('InvalidRequest', 'reference (the bank or Global Payouts reference) is required, at most 200 characters');
  }
  const now = ctx.clock.now();
  return ctx.db.withTransaction(async (tx) => {
    const rows = await tx.query('SELECT * FROM payouts WHERE id = $1', [payoutId]);
    if (rows.length === 0) {
      throw new ApiError('NotFound', 'no such payout');
    }
    const before = rowToPayout(rows[0]!);
    if (before.method !== 'manual' || before.status !== 'pending') {
      throw new ApiError('IllegalTransition', 'only a pending manual payout can be marked paid');
    }
    await tx.query("UPDATE payouts SET status = 'paid', reference = $1, completed_at = $2 WHERE id = $3", [reference, now, payoutId]);
    await postEntry(tx, { ref: `payout_settled:${payoutId}`, kind: 'payout_settled', payoutId, at: now, lines: payoutSettledLines(before.sellerId, before.currency, before.amount) });
    const after = rowToPayout((await tx.query('SELECT * FROM payouts WHERE id = $1', [payoutId]))[0]!);
    await writeAudit(tx, { actorId: adminId, action: 'payout.mark_paid', target: `payout:${payoutId}`, before: payoutToJson(before), after: payoutToJson(after), at: now });
    return after;
  });
}
