// Accounts and sessions (ADR 0032 §5). Session tokens are opaque 256-bit values; only their SHA-256 is stored. Roles come only from
// the account_roles table.
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { num, str, strOrNull, type Queryable } from '../db/driver.ts';
import type { Principal } from '../http/router.ts';

export const SESSION_TTL_MS = 30 * 24 * 3600 * 1000;
/** A recognisable prefix lets log and report redaction find a leaked token (redaction.ts). */
export const SESSION_TOKEN_PREFIX = 'scs_';

export function sha256Hex(text: string): string {
  return createHash('sha256').update(text, 'utf8').digest('hex');
}

export function newOpaqueToken(prefix = ''): string {
  return `${prefix}${randomBytes(32).toString('base64url')}`;
}

export interface Account {
  id: string;
  issuer: string;
  subject: string;
  email: string | null;
  displayName: string;
  createdAt: number;
}

export async function upsertAccount(
  tx: Queryable,
  identity: { issuer: string; subject: string; email: string | null; displayName: string },
  now: number,
): Promise<Account> {
  const existing = await tx.query('SELECT * FROM accounts WHERE issuer = $1 AND subject = $2', [identity.issuer, identity.subject]);
  if (existing.length > 0) {
    const row = existing[0]!;
    await tx.query('UPDATE accounts SET email = $1, display_name = $2 WHERE id = $3', [identity.email, identity.displayName, str(row.id)]);
    return { id: str(row.id), issuer: identity.issuer, subject: identity.subject, email: identity.email, displayName: identity.displayName, createdAt: num(row.created_at) };
  }
  const id = randomUUID();
  await tx.query('INSERT INTO accounts (id, issuer, subject, email, display_name, created_at) VALUES ($1, $2, $3, $4, $5, $6)', [
    id,
    identity.issuer,
    identity.subject,
    identity.email,
    identity.displayName,
    now,
  ]);
  return { id, ...identity, createdAt: now };
}

export async function getAccount(q: Queryable, id: string): Promise<Account | null> {
  const rows = await q.query('SELECT * FROM accounts WHERE id = $1', [id]);
  if (rows.length === 0) {
    return null;
  }
  const row = rows[0]!;
  return {
    id: str(row.id),
    issuer: str(row.issuer),
    subject: str(row.subject),
    email: strOrNull(row.email),
    displayName: str(row.display_name),
    createdAt: num(row.created_at),
  };
}

export async function getRoles(q: Queryable, accountId: string): Promise<string[]> {
  const rows = await q.query('SELECT role FROM account_roles WHERE account_id = $1 ORDER BY role', [accountId]);
  return rows.map((row) => str(row.role));
}

export async function grantRole(q: Queryable, accountId: string, role: 'admin', now: number): Promise<void> {
  await q.query('INSERT INTO account_roles (account_id, role, granted_at) VALUES ($1, $2, $3) ON CONFLICT (account_id, role) DO NOTHING', [
    accountId,
    role,
    now,
  ]);
}

export async function createSession(tx: Queryable, accountId: string, now: number): Promise<{ token: string; expiresAt: number }> {
  const token = newOpaqueToken(SESSION_TOKEN_PREFIX);
  const expiresAt = now + SESSION_TTL_MS;
  await tx.query('INSERT INTO sessions (token_hash, account_id, created_at, expires_at) VALUES ($1, $2, $3, $4)', [
    sha256Hex(token),
    accountId,
    now,
    expiresAt,
  ]);
  return { token, expiresAt };
}

export async function authenticate(q: Queryable, token: string, now: number): Promise<Principal | null> {
  if (!token.startsWith(SESSION_TOKEN_PREFIX)) {
    return null;
  }
  const hash = sha256Hex(token);
  const rows = await q.query('SELECT account_id, expires_at, revoked_at FROM sessions WHERE token_hash = $1', [hash]);
  const row = rows[0];
  if (!row || row.revoked_at !== null || num(row.expires_at) <= now) {
    return null;
  }
  const accountId = str(row.account_id);
  return { accountId, roles: await getRoles(q, accountId), sessionHash: hash };
}

export async function revokeSession(q: Queryable, sessionHash: string, now: number): Promise<void> {
  await q.query('UPDATE sessions SET revoked_at = $1 WHERE token_hash = $2 AND revoked_at IS NULL', [now, sessionHash]);
}
