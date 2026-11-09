// Every admin mutation writes one audit_log row (who, what, target, before/after) in the same transaction as the change
// (ADR 0032 §4.1, §9).
import { randomUUID } from 'node:crypto';
import type { Queryable } from './db/driver.ts';

function toJson(value: unknown): string | null {
  if (value === undefined || value === null) {
    return null;
  }
  return JSON.stringify(value, (_key, entry) => (typeof entry === 'bigint' ? entry.toString() : entry));
}

export async function writeAudit(
  tx: Queryable,
  entry: { actorId: string; action: string; target: string; before?: unknown; after?: unknown; at: number },
): Promise<void> {
  await tx.query(
    'INSERT INTO audit_log (id, actor_id, action, target, before_json, after_json, created_at) VALUES ($1, $2, $3, $4, $5, $6, $7)',
    [randomUUID(), entry.actorId, entry.action, entry.target, toJson(entry.before), toJson(entry.after), entry.at],
  );
}
