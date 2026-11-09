import pg from 'pg';
import { UniqueViolation, type Database, type Queryable, type Row, type SqlValue } from './driver.ts';

const INT8_OID = 20;
const NUMERIC_OID = 1700;
const MAX_SERIALIZATION_RETRIES = 5;

/** Integers come back as bigint (never a double), like node:sqlite with readBigInts. */
function getTypeParser(oid: number, format?: string): (value: string) => unknown {
  if (oid === INT8_OID || oid === NUMERIC_OID) {
    // A fractional numeric throws: this schema never stores one, and silently truncating money is worse than failing.
    return (value: string) => BigInt(value);
  }
  return pg.types.getTypeParser(oid, format as 'text');
}

function toParams(params: readonly SqlValue[]): unknown[] {
  return params.map((value) => (typeof value === 'bigint' ? value.toString() : value));
}

function mapError(error: unknown): unknown {
  return (error as { code?: string }).code === '23505' ? new UniqueViolation((error as Error).message) : error;
}

function isRetryable(error: unknown): boolean {
  const code = (error as { code?: string }).code;
  return code === '40001' || code === '40P01';
}

export class PostgresDatabase implements Database {
  readonly dialect = 'postgres' as const;
  private readonly pool: pg.Pool;

  constructor(connectionString: string, maxConnections = 10) {
    this.pool = new pg.Pool({ connectionString, max: maxConnections, types: { getTypeParser } as never });
  }

  async query(sql: string, params: readonly SqlValue[] = []): Promise<Row[]> {
    try {
      const result = await this.pool.query(sql, toParams(params));
      return result.rows as Row[];
    } catch (error) {
      throw mapError(error);
    }
  }

  async withTransaction<T>(fn: (tx: Queryable) => Promise<T>): Promise<T> {
    for (let attempt = 1; ; attempt++) {
      const client = await this.pool.connect();
      const tx: Queryable = {
        query: async (sql, params = []) => {
          try {
            return (await client.query(sql, toParams(params))).rows as Row[];
          } catch (error) {
            throw mapError(error);
          }
        },
      };
      try {
        await client.query('BEGIN ISOLATION LEVEL SERIALIZABLE');
        const result = await fn(tx);
        await client.query('COMMIT');
        return result;
      } catch (error) {
        await client.query('ROLLBACK').catch(() => undefined);
        if (attempt < MAX_SERIALIZATION_RETRIES && isRetryable(error)) {
          continue;
        }
        throw error;
      } finally {
        client.release();
      }
    }
  }

  /** One dedicated connection, so a failed script's open transaction is rolled back before the connection goes back to the pool. */
  async execScript(sql: string): Promise<void> {
    const client = await this.pool.connect();
    try {
      await client.query(sql);
    } catch (error) {
      await client.query('ROLLBACK').catch(() => undefined);
      throw mapError(error);
    } finally {
      client.release();
    }
  }

  async ping(): Promise<void> {
    await this.pool.query('SELECT 1 AS ok');
  }

  async close(): Promise<void> {
    await this.pool.end();
  }
}
