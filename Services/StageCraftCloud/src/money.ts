// Money is bigint minor units with an ISO 4217 code. Floats never appear (ADR 0032 §3.2, Economy security 6).
import { ApiError } from './errors.ts';

export const BPS_DENOMINATOR = 10_000n;

const CURRENCY_PATTERN = /^[A-Z]{3}$/;

export function isCurrencyCode(code: unknown): code is string {
  return typeof code === 'string' && CURRENCY_PATTERN.test(code);
}

/** Upper-cases and validates; throws InvalidRequest for anything that is not three letters. */
export function normalizeCurrency(code: unknown): string {
  const upper = typeof code === 'string' ? code.trim().toUpperCase() : '';
  if (!isCurrencyCode(upper)) {
    throw new ApiError('InvalidRequest', 'currency must be an ISO 4217 code');
  }
  return upper;
}

export function assertShareBps(shareBps: bigint): void {
  if (shareBps < 0n || shareBps > BPS_DENOMINATOR) {
    throw new RangeError(`share must be 0..10000 basis points, got ${shareBps}`);
  }
}

export interface Split {
  seller: bigint;
  platform: bigint;
}

/** seller = floor(P * share / 10000); platform = the rest, so rounding always goes to the platform and the parts sum to P. */
export function splitSale(price: bigint, shareBps: bigint): Split {
  assertShareBps(shareBps);
  if (price < 0n) {
    throw new RangeError('price must not be negative');
  }
  const seller = (price * shareBps) / BPS_DENOMINATOR;
  return { seller, platform: price - seller };
}

export interface RefundState {
  price: bigint;
  shareBps: bigint;
  /** Seller and platform parts already refunded on this order. */
  sellerRefunded: bigint;
  platformRefunded: bigint;
}

/**
 * Splits a (partial) refund R of an order (§3.2): the seller's part is floor(R * S / 10000) capped so the cumulative seller refund
 * never exceeds the seller's share; the platform takes the rest, capped the same way. The cumulative refund never exceeds P.
 * Throws RefundExceedsCharge when R is not positive or would take the cumulative refund over the price.
 */
export function apportionRefund(state: RefundState, refund: bigint): Split {
  const { seller: sellerShare, platform: platformShare } = splitSale(state.price, state.shareBps);
  const sellerRemaining = sellerShare - state.sellerRefunded;
  const platformRemaining = platformShare - state.platformRefunded;
  if (sellerRemaining < 0n || platformRemaining < 0n) {
    throw new RangeError('order refund state is inconsistent');
  }
  if (refund <= 0n || refund > sellerRemaining + platformRemaining) {
    throw new ApiError('RefundExceedsCharge', 'refund must be positive and must not exceed what is left of the charge');
  }
  let seller = (refund * state.shareBps) / BPS_DENOMINATOR;
  if (seller > sellerRemaining) {
    seller = sellerRemaining;
  }
  let platform = refund - seller;
  if (platform > platformRemaining) {
    platform = platformRemaining;
    seller = refund - platform;
  }
  return { seller, platform };
}

/** A bigint as a JSON number; refuses values a double cannot hold exactly instead of silently rounding. */
export function toJsonAmount(value: bigint): number {
  if (value > BigInt(Number.MAX_SAFE_INTEGER) || value < BigInt(Number.MIN_SAFE_INTEGER)) {
    throw new RangeError('amount exceeds the JSON-safe integer range');
  }
  return Number(value);
}

/** Parses a JSON amount (a safe non-negative integer, or a decimal integer string) into minor units. */
export function parseAmount(value: unknown): bigint | null {
  if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) {
    return BigInt(value);
  }
  if (typeof value === 'string' && /^(0|[1-9][0-9]{0,17})$/.test(value)) {
    return BigInt(value);
  }
  return null;
}
