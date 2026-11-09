import { readdir, readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { str, type Database } from './driver.ts';

export const MIGRATIONS_DIR = fileURLToPath(new URL('../../migrations/', import.meta.url));

const FILE_PATTERN = /^(\d{4})_[a-z0-9_]+\.sql$/;

/**
 * Applies every migration file not yet in schema_migrations, in file-name order, each in its own transaction together with its
 * schema_migrations row (the row is inserted first, so two migrators racing on Postgres fail cleanly on the primary key).
 * Returns the versions applied.
 */
export async function migrate(db: Database, dir: string = MIGRATIONS_DIR, now: number = Date.now()): Promise<string[]> {
  await db.execScript('CREATE TABLE IF NOT EXISTS schema_migrations (version TEXT PRIMARY KEY, applied_at BIGINT NOT NULL)');
  const appliedRows = await db.query('SELECT version FROM schema_migrations');
  const applied = new Set(appliedRows.map((row) => str(row.version)));

  const files = (await readdir(dir)).filter((name) => FILE_PATTERN.test(name)).sort();
  const done: string[] = [];
  for (const file of files) {
    const version = file.slice(0, -'.sql'.length);
    if (applied.has(version)) {
      continue;
    }
    const sql = await readFile(join(dir, file), 'utf8');
    // The version is checked against FILE_PATTERN above, so it is safe to inline.
    const script = `BEGIN;\nINSERT INTO schema_migrations (version, applied_at) VALUES ('${version}', ${Math.trunc(now)});\n${sql}\nCOMMIT;`;
    try {
      await db.execScript(script);
    } catch (error) {
      await db.execScript('ROLLBACK').catch(() => undefined);
      throw new Error(`migration ${version} failed: ${(error as Error).message}`);
    }
    done.push(version);
  }
  return done;
}
