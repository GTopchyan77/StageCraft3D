// Double-entry ledger (ADR 0032 §3.3). Every money event is one journal entry whose lines sum to zero per currency. Debits are
// positive, credits negative, so liabilities and revenue carry negative balances. Balances are only ever sums of lines.
import { randomUUID } from 'node:crypto';
import { big, num, str, type Queryable } from './db/driver.ts';
import { isCurrencyCode } from './money.ts';

export const Accounts = {
  cash: 'cash:stripe',
  revenue: 'platform:revenue',
  fees: 'platform:fees',
  refunds: 'refunds',
  disputes: 'disputes',
  sellerPending: (sellerId: string) => `seller:${checkId(sellerId)}:pending`,
  sellerAvailable: (sellerId: string) => `seller:${checkId(sellerId)}:available`,
  /** Payouts created and not yet confirmed. A confirmed payout clears it against cash. */
  sellerPaid: (sellerId: string) => `seller:${checkId(sellerId)}:paid`,
} as const;

const ACCOUNT_PATTERN = /^(cash:stripe|platform:revenue|platform:fees|refunds|disputes|seller:[A-Za-z0-9-]{1,64}:(pending|available|paid))$/;

function checkId(id: string): string {
  if (!/^[A-Za-z0-9-]{1,64}$/.test(id)) {
    throw new RangeError('invalid ledger account id');
  }
  return id;
}

export interface LedgerLine {
  account: string;
  currency: string;
  amount: bigint;
}

export type EntryKind =
  | 'sale'
  | 'release'
  | 'refund'
  | 'dispute_opened'
  | 'dispute_won'
  | 'dispute_lost'
  | 'dispute_fee'
  | 'payout_reserved'
  | 'payout_settled'
  | 'payout_failed';

export class UnbalancedEntryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'UnbalancedEntryError';
  }
}

/** Refuses an entry unless it has at least two non-zero lines on known accounts and sums to zero in every currency. */
export function assertBalanced(lines: readonly LedgerLine[]): void {
  if (lines.length < 2) {
    throw new UnbalancedEntryError('an entry needs at least two lines');
  }
  const sums = new Map<string, bigint>();
  for (const line of lines) {
    if (!ACCOUNT_PATTERN.test(line.account)) {
      throw new UnbalancedEntryError(`unknown account ${line.account}`);
    }
    if (!isCurrencyCode(line.currency)) {
      throw new UnbalancedEntryError(`invalid currency ${line.currency}`);
    }
    if (typeof line.amount !== 'bigint' || line.amount === 0n) {
      throw new UnbalancedEntryError('every line needs a non-zero bigint amount');
    }
    sums.set(line.currency, (sums.get(line.currency) ?? 0n) + line.amount);
  }
  for (const [currency, sum] of sums) {
    if (sum !== 0n) {
      throw new UnbalancedEntryError(`entry does not balance in ${currency}: ${sum}`);
    }
  }
}

/** Drops zero lines (a refund whose seller part rounds to zero, for example) before the balance check. */
function nonZero(lines: LedgerLine[]): LedgerLine[] {
  return lines.filter((line) => line.amount !== 0n);
}

export interface EntryInput {
  /** Business key; an entry with the same ref is never posted twice. */
  ref: string;
  kind: EntryKind;
  lines: LedgerLine[];
  at: number;
  orderId?: string | null;
  payoutId?: string | null;
  memo?: string | null;
}

/**
 * Posts one balanced entry inside the caller's transaction. Returns the entry id, or null when an entry with this ref already
 * exists (an idempotent replay). The balance is checked in code before writing and again by the database before commit, so an
 * unbalanced entry throws and the caller's transaction rolls back.
 */
export async function postEntry(tx: Queryable, input: EntryInput): Promise<string | null> {
  const lines = nonZero(input.lines);
  assertBalanced(lines);
  const id = randomUUID();
  const inserted = await tx.query(
    `INSERT INTO journal_entries (id, ref, kind, order_id, payout_id, memo, created_at)
     VALUES ($1, $2, $3, $4, $5, $6, $7) ON CONFLICT (ref) DO NOTHING RETURNING id`,
    [id, input.ref, input.kind, input.orderId ?? null, input.payoutId ?? null, input.memo ?? null, input.at],
  );
  if (inserted.length === 0) {
    return null;
  }
  for (const [index, line] of lines.entries()) {
    await tx.query('INSERT INTO journal_lines (entry_id, line_no, account, currency, amount) VALUES ($1, $2, $3, $4, $5)', [
      id,
      index,
      line.account,
      line.currency,
      line.amount,
    ]);
  }
  const unbalanced = await tx.query(
    'SELECT currency, SUM(amount) AS total FROM journal_lines WHERE entry_id = $1 GROUP BY currency HAVING SUM(amount) <> 0',
    [id],
  );
  if (unbalanced.length > 0) {
    throw new UnbalancedEntryError(`database check refused entry ${input.ref}`);
  }
  return id;
}

export async function balance(q: Queryable, account: string, currency: string): Promise<bigint> {
  const rows = await q.query('SELECT COALESCE(SUM(amount), 0) AS total FROM journal_lines WHERE account = $1 AND currency = $2', [
    account,
    currency,
  ]);
  return big(rows[0]?.total);
}

/** Sum per account and currency for every account starting with prefix (e.g. "seller:"). */
export async function balancesByPrefix(q: Queryable, prefix: string): Promise<{ account: string; currency: string; total: bigint }[]> {
  const rows = await q.query(
    `SELECT account, currency, SUM(amount) AS total FROM journal_lines WHERE substr(account, 1, $2) = $1
     GROUP BY account, currency ORDER BY account, currency`,
    [prefix, prefix.length],
  );
  return rows.map((row) => ({ account: str(row.account), currency: str(row.currency), total: big(row.total) }));
}

/** Total of all lines; zero in every currency for a consistent ledger (used by tests and the readiness self-check). */
export async function trialBalance(q: Queryable): Promise<Map<string, bigint>> {
  const rows = await q.query('SELECT currency, SUM(amount) AS total FROM journal_lines GROUP BY currency');
  return new Map(rows.map((row) => [str(row.currency), big(row.total)]));
}

export async function countEntries(q: Queryable, kind: EntryKind): Promise<number> {
  const rows = await q.query('SELECT COUNT(*) AS n FROM journal_entries WHERE kind = $1', [kind]);
  return num(rows[0]?.n);
}

// Line builders for each event of §3.3. Amounts are positive inputs; signs are applied here.

export function saleLines(sellerId: string, currency: string, seller: bigint, platform: bigint): LedgerLine[] {
  return [
    { account: Accounts.cash, currency, amount: seller + platform },
    { account: Accounts.sellerPending(sellerId), currency, amount: -seller },
    { account: Accounts.revenue, currency, amount: -platform },
  ];
}

export function releaseLines(sellerId: string, currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.sellerPending(sellerId), currency, amount },
    { account: Accounts.sellerAvailable(sellerId), currency, amount: -amount },
  ];
}

/**
 * A refund (or a lost dispute) reverses the sale in the §3.2 proportions. The seller's part comes back from pending while the sale
 * is in its hold period, from available after it (which may go negative: future sales recover it). The platform's part is posted to
 * the refunds contra-revenue account. The money leaves cash, or the disputes account when the provider already withdrew it.
 */
export function refundLines(
  sellerAccount: string,
  currency: string,
  seller: bigint,
  platform: bigint,
  source: typeof Accounts.cash | typeof Accounts.disputes,
): LedgerLine[] {
  return [
    { account: source, currency, amount: -(seller + platform) },
    { account: sellerAccount, currency, amount: seller },
    { account: Accounts.refunds, currency, amount: platform },
  ];
}

export function disputeOpenedLines(currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.disputes, currency, amount },
    { account: Accounts.cash, currency, amount: -amount },
  ];
}

export function disputeWonLines(currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.cash, currency, amount },
    { account: Accounts.disputes, currency, amount: -amount },
  ];
}

export function feeLines(currency: string, fee: bigint): LedgerLine[] {
  return [
    { account: Accounts.fees, currency, amount: fee },
    { account: Accounts.cash, currency, amount: -fee },
  ];
}

export function payoutReservedLines(sellerId: string, currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.sellerAvailable(sellerId), currency, amount },
    { account: Accounts.sellerPaid(sellerId), currency, amount: -amount },
  ];
}

export function payoutSettledLines(sellerId: string, currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.sellerPaid(sellerId), currency, amount },
    { account: Accounts.cash, currency, amount: -amount },
  ];
}

export function payoutFailedLines(sellerId: string, currency: string, amount: bigint): LedgerLine[] {
  return [
    { account: Accounts.sellerPaid(sellerId), currency, amount },
    { account: Accounts.sellerAvailable(sellerId), currency, amount: -amount },
  ];
}
