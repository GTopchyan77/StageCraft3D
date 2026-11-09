import { DatabaseSync, type StatementSync } from 'node:sqlite';
import { UniqueViolation, type Database, type Queryable, type Row, type SqlValue } from './driver.ts';

const SQLITE_CONSTRAINT_UNIQUE = 2067;
const SQLITE_CONSTRAINT_PRIMARYKEY = 1555;

/** $1 -> ?1: SQLite's numbered parameters bind by the same index, so a repeated $1 stays one value. */
export function toSqlitePlaceholders(sql: string): string {
  return sql.replace(/\$(\d+)/g, '?$1');
}

/**
 * node:sqlite is synchronous and single-connection, so every statement and transaction is serialised through one async lock: a
 * transaction that awaits between statements can never interleave with another request's statements.
 */
export class SqliteDatabase implements Database {
  readonly dialect = 'sqlite' as const;
  private readonly db: DatabaseSync;
  private readonly statements = new Map<string, StatementSync>();
  private lock: Promise<void> = Promise.resolve();

  constructor(path: string) {
    this.db = new DatabaseSync(path);
    this.db.exec('PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;');
    if (path !== ':memory:') {
      this.db.exec('PRAGMA journal_mode = WAL;');
    }
  }

  private run(sql: string, params: readonly SqlValue[]): Row[] {
    let statement = this.statements.get(sql);
    if (!statement) {
      statement = this.db.prepare(toSqlitePlaceholders(sql));
      statement.setReadBigInts(true);
      this.statements.set(sql, statement);
    }
    try {
      return statement.all(...(params as SqlValue[])) as Row[];
    } catch (error) {
      throw mapError(error);
    }
  }

  private async exclusive<T>(fn: () => Promise<T>): Promise<T> {
    const previous = this.lock;
    let release!: () => void;
    this.lock = new Promise<void>((resolve) => {
      release = resolve;
    });
    await previous;
    try {
      return await fn();
    } finally {
      release();
    }
  }

  query(sql: string, params: readonly SqlValue[] = []): Promise<Row[]> {
    return this.exclusive(async () => this.run(sql, params));
  }

  withTransaction<T>(fn: (tx: Queryable) => Promise<T>): Promise<T> {
    return this.exclusive(async () => {
      this.db.exec('BEGIN IMMEDIATE');
      const tx: Queryable = { query: async (sql, params = []) => this.run(sql, params) };
      try {
        const result = await fn(tx);
        this.db.exec('COMMIT');
        return result;
      } catch (error) {
        this.db.exec('ROLLBACK');
        throw error;
      }
    });
  }

  execScript(sql: string): Promise<void> {
    return this.exclusive(async () => {
      this.db.exec(sql);
    });
  }

  async ping(): Promise<void> {
    await this.query('SELECT 1 AS ok');
  }

  async close(): Promise<void> {
    await this.exclusive(async () => {
      this.statements.clear();
      this.db.close();
    });
  }
}

function mapError(error: unknown): unknown {
  const errcode = (error as { errcode?: number }).errcode;
  if (errcode === SQLITE_CONSTRAINT_UNIQUE || errcode === SQLITE_CONSTRAINT_PRIMARYKEY) {
    return new UniqueViolation((error as Error).message);
  }
  return error;
}
