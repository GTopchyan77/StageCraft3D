// One small async driver interface over node:sqlite (tests, local dev) and pg (production). Both run the same SQL, written with
// $1-style placeholders. Integer columns come back as bigint from both drivers (SQLite readBigInts, pg int8/numeric parsers), so
// repositories never see a double for money.

export type SqlValue = string | bigint | number | null;
export type Row = Record<string, unknown>;

export interface Queryable {
  query(sql: string, params?: readonly SqlValue[]): Promise<Row[]>;
}

export interface Database extends Queryable {
  readonly dialect: 'sqlite' | 'postgres';
  /**
   * Runs fn in one transaction: commits when it resolves, rolls back when it throws. Postgres runs SERIALIZABLE and re-runs fn on a
   * serialization failure, so fn must only touch the database (no provider calls, no notifications) and must be safe to repeat.
   */
  withTransaction<T>(fn: (tx: Queryable) => Promise<T>): Promise<T>;
  /** Runs a multi-statement script without parameters (migrations). */
  execScript(sql: string): Promise<void>;
  ping(): Promise<void>;
  close(): Promise<void>;
}

/** A unique or primary-key constraint refused the write; callers turn it into AlreadyExists or an idempotent replay. */
export class UniqueViolation extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'UniqueViolation';
  }
}

export function big(value: unknown): bigint {
  if (typeof value === 'bigint') {
    return value;
  }
  if (typeof value === 'number' && Number.isSafeInteger(value)) {
    return BigInt(value);
  }
  if (typeof value === 'string' && /^-?[0-9]+$/.test(value)) {
    return BigInt(value);
  }
  if (value === null || value === undefined) {
    return 0n;
  }
  throw new TypeError(`not an integer column value: ${String(value)}`);
}

export function num(value: unknown): number {
  const asBig = big(value);
  if (asBig > BigInt(Number.MAX_SAFE_INTEGER) || asBig < BigInt(Number.MIN_SAFE_INTEGER)) {
    throw new RangeError('integer column out of the safe range');
  }
  return Number(asBig);
}

export function str(value: unknown): string {
  if (typeof value !== 'string') {
    throw new TypeError('expected a text column');
  }
  return value;
}

export function strOrNull(value: unknown): string | null {
  return value === null || value === undefined ? null : str(value);
}
