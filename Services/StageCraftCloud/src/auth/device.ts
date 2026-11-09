// OAuth 2.0 device authorization grant (RFC 8628) for the desktop app (ADR 0032 §5): the app never handles a password and never
// runs a local web server. Device codes are stored hashed; a code yields at most one session.
import { randomInt, randomUUID } from 'node:crypto';
import { ApiError } from '../errors.ts';
import { num, str, type Database, type Queryable } from '../db/driver.ts';
import { createSession, newOpaqueToken, sha256Hex } from './sessions.ts';

export const DEVICE_CODE_TTL_SECONDS = 600;
export const DEVICE_POLL_INTERVAL_SECONDS = 5;
export const SLOW_DOWN_INCREMENT_SECONDS = 5;
/** RFC 8628 §6.1: no vowels or easily confused characters. */
const USER_CODE_ALPHABET = 'BCDFGHJKLMNPQRSTVWXZ';

export function newUserCode(): string {
  let code = '';
  for (let index = 0; index < 8; index++) {
    code += USER_CODE_ALPHABET[randomInt(USER_CODE_ALPHABET.length)];
  }
  return `${code.slice(0, 4)}-${code.slice(4)}`;
}

/** Accepts what a person types: any case, with or without the dash or spaces. Null when it cannot be a user code. */
export function normalizeUserCode(input: string): string | null {
  const letters = input.toUpperCase().replace(/[^A-Z]/g, '');
  if (letters.length !== 8 || [...letters].some((char) => !USER_CODE_ALPHABET.includes(char))) {
    return null;
  }
  return `${letters.slice(0, 4)}-${letters.slice(4)}`;
}

export interface DeviceStart {
  deviceCode: string;
  userCode: string;
  verificationUri: string;
  verificationUriComplete: string;
  expiresIn: number;
  interval: number;
}

export async function startDeviceAuthorization(db: Database, publicBaseUrl: string, now: number): Promise<DeviceStart> {
  const deviceCode = newOpaqueToken('scd_');
  for (let attempt = 0; ; attempt++) {
    const userCode = newUserCode();
    const taken = await db.query('SELECT 1 AS taken FROM device_codes WHERE user_code = $1', [userCode]);
    if (taken.length > 0 && attempt < 5) {
      continue;
    }
    await db.query(
      `INSERT INTO device_codes (id, device_code_hash, user_code, status, interval_seconds, created_at, expires_at)
       VALUES ($1, $2, $3, 'pending', $4, $5, $6)`,
      [randomUUID(), sha256Hex(deviceCode), userCode, DEVICE_POLL_INTERVAL_SECONDS, now, now + DEVICE_CODE_TTL_SECONDS * 1000],
    );
    const verificationUri = `${publicBaseUrl}/auth/verify`;
    return {
      deviceCode,
      userCode,
      verificationUri,
      verificationUriComplete: `${verificationUri}?user_code=${userCode}`,
      expiresIn: DEVICE_CODE_TTL_SECONDS,
      interval: DEVICE_POLL_INTERVAL_SECONDS,
    };
  }
}

export interface DeviceTokenResult {
  sessionToken: string;
  expiresAt: number;
  accountId: string;
}

type PollOutcome = { kind: 'slow_down'; interval: number } | { kind: 'pending' } | ({ kind: 'session' } & DeviceTokenResult);

/** One poll. Throws the RFC 8628 errors as ApiError codes; returns the session exactly once after approval. */
export async function pollDeviceToken(db: Database, deviceCode: string, now: number): Promise<DeviceTokenResult> {
  const outcome = await db.withTransaction(async (tx): Promise<PollOutcome> => {
    const rows = await tx.query('SELECT * FROM device_codes WHERE device_code_hash = $1', [sha256Hex(deviceCode)]);
    const row = rows[0];
    if (!row || str(row.status) === 'consumed') {
      throw new ApiError('invalid_grant', 'unknown or already used device code');
    }
    if (num(row.expires_at) <= now) {
      throw new ApiError('expired_token', 'the device code expired; start again');
    }
    const status = str(row.status);
    if (status === 'denied') {
      throw new ApiError('access_denied', 'sign-in was refused');
    }
    const interval = num(row.interval_seconds);
    const lastPolled = row.last_polled_at === null ? null : num(row.last_polled_at);
    if (status === 'pending' && lastPolled !== null && now - lastPolled < interval * 1000) {
      // The slower interval must persist, so slow_down is returned (committed) rather than thrown (rolled back).
      await tx.query('UPDATE device_codes SET interval_seconds = $1, last_polled_at = $2 WHERE id = $3', [
        interval + SLOW_DOWN_INCREMENT_SECONDS,
        now,
        str(row.id),
      ]);
      return { kind: 'slow_down', interval: interval + SLOW_DOWN_INCREMENT_SECONDS };
    }
    await tx.query('UPDATE device_codes SET last_polled_at = $1 WHERE id = $2', [now, str(row.id)]);
    if (status === 'pending') {
      return { kind: 'pending' };
    }
    await tx.query("UPDATE device_codes SET status = 'consumed' WHERE id = $1", [str(row.id)]);
    const accountId = str(row.account_id);
    const session = await createSession(tx, accountId, now);
    return { kind: 'session', sessionToken: session.token, expiresAt: session.expiresAt, accountId };
  });
  if (outcome.kind === 'slow_down') {
    throw new ApiError('slow_down', `polling too fast; wait ${outcome.interval} seconds between polls`);
  }
  if (outcome.kind === 'pending') {
    throw new ApiError('authorization_pending', 'waiting for the user to sign in');
  }
  return { sessionToken: outcome.sessionToken, expiresAt: outcome.expiresAt, accountId: outcome.accountId };
}

export async function findPendingDevice(q: Queryable, userCode: string, now: number): Promise<{ id: string } | null> {
  const rows = await q.query("SELECT id, expires_at FROM device_codes WHERE user_code = $1 AND status = 'pending'", [userCode]);
  const row = rows[0];
  return row && num(row.expires_at) > now ? { id: str(row.id) } : null;
}

/** Approves (or denies) a pending, unexpired device code. False when it is no longer pending. */
export async function decideDevice(q: Queryable, deviceId: string, decision: { accountId: string } | 'denied', now: number): Promise<boolean> {
  const rows =
    decision === 'denied'
      ? await q.query("UPDATE device_codes SET status = 'denied' WHERE id = $1 AND status = 'pending' AND expires_at > $2 RETURNING id", [deviceId, now])
      : await q.query(
          "UPDATE device_codes SET status = 'approved', account_id = $1 WHERE id = $2 AND status = 'pending' AND expires_at > $3 RETURNING id",
          [decision.accountId, deviceId, now],
        );
  return rows.length > 0;
}
