// Stripe-Signature verification (ADR 0032 §3.5): HMAC-SHA256 of `${t}.${rawBody}` with an endpoint secret, any v1 may match,
// constant-time compare, 300 s tolerance. Runs on the raw bytes before anything parses them.
import { createHmac, timingSafeEqual } from 'node:crypto';

export const DEFAULT_TOLERANCE_SECONDS = 300;

export type SignatureFailure = 'missing_header' | 'malformed_header' | 'stale_timestamp' | 'no_match';

export interface SignatureCheck {
  ok: boolean;
  failure?: SignatureFailure;
}

function hmacHex(secret: string, timestamp: string, rawBody: Buffer): Buffer {
  return createHmac('sha256', secret).update(`${timestamp}.`, 'utf8').update(rawBody).digest();
}

/**
 * secrets: every currently valid endpoint secret (more than one during rotation, and the Connect endpoint's secret).
 * The timestamp must be within tolerance in either direction of nowMs.
 */
export function verifyStripeSignature(
  rawBody: Buffer,
  header: string | undefined,
  secrets: readonly string[],
  nowMs: number,
  toleranceSeconds = DEFAULT_TOLERANCE_SECONDS,
): SignatureCheck {
  if (!header) {
    return { ok: false, failure: 'missing_header' };
  }
  let timestamp: string | null = null;
  const candidates: Buffer[] = [];
  for (const part of header.split(',')) {
    const separator = part.indexOf('=');
    if (separator <= 0) {
      continue;
    }
    const key = part.slice(0, separator).trim();
    const value = part.slice(separator + 1).trim();
    if (key === 't' && timestamp === null) {
      timestamp = value;
    } else if (key === 'v1' && /^[0-9a-f]{64}$/.test(value)) {
      candidates.push(Buffer.from(value, 'hex'));
    }
  }
  if (timestamp === null || !/^[0-9]{1,12}$/.test(timestamp) || candidates.length === 0) {
    return { ok: false, failure: 'malformed_header' };
  }
  if (Math.abs(Math.floor(nowMs / 1000) - Number(timestamp)) > toleranceSeconds) {
    return { ok: false, failure: 'stale_timestamp' };
  }
  for (const secret of secrets) {
    const expected = hmacHex(secret, timestamp, rawBody);
    // Every candidate is compared (no early exit on the first secret's result) so timing does not reveal which part matched.
    let matched = false;
    for (const candidate of candidates) {
      matched = timingSafeEqual(expected, candidate) || matched;
    }
    if (matched) {
      return { ok: true };
    }
  }
  return { ok: false, failure: 'no_match' };
}

/** Builds a Stripe-Signature header; used by the fake checkout and by tests. */
export function signStripePayload(rawBody: Buffer | string, secret: string, timestampSeconds: number): string {
  const body = typeof rawBody === 'string' ? Buffer.from(rawBody, 'utf8') : rawBody;
  return `t=${timestampSeconds},v1=${hmacHex(secret, String(timestampSeconds), body).toString('hex')}`;
}
