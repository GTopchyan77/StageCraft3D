import test from 'node:test';
import assert from 'node:assert/strict';
import { apportionRefund, assertShareBps, isCurrencyCode, normalizeCurrency, parseAmount, splitSale, toJsonAmount } from '../src/money.ts';
import { ApiError } from '../src/errors.ts';

test('split: seller gets floor(P * share / 10000), platform the rest, parts sum to P', () => {
  assert.deepEqual(splitSale(1000n, 8500n), { seller: 850n, platform: 150n });
  assert.deepEqual(splitSale(999n, 8500n), { seller: 849n, platform: 150n }); // 849.15 -> rounding goes to the platform
  assert.deepEqual(splitSale(1n, 8500n), { seller: 0n, platform: 1n });
  assert.deepEqual(splitSale(0n, 8500n), { seller: 0n, platform: 0n });
  assert.deepEqual(splitSale(100n, 10000n), { seller: 100n, platform: 0n });
  assert.deepEqual(splitSale(100n, 0n), { seller: 0n, platform: 100n });
  for (let price = 0n; price < 2000n; price += 7n) {
    const { seller, platform } = splitSale(price, 8500n);
    assert.equal(seller + platform, price);
    assert.ok(seller * 10000n <= price * 8500n);
  }
  const huge = 9_000_000_000_000_000_000n;
  assert.equal(splitSale(huge, 8500n).seller, (huge * 8500n) / 10000n);
});

test('split refuses invalid shares and negative prices', () => {
  assert.throws(() => splitSale(100n, 10001n), RangeError);
  assert.throws(() => splitSale(100n, -1n), RangeError);
  assert.throws(() => splitSale(-1n, 8500n), RangeError);
  assert.throws(() => assertShareBps(20000n), RangeError);
});

test('refund apportioning: proportional, cumulative caps, never above P', () => {
  const base = { price: 999n, shareBps: 8500n, sellerRefunded: 0n, platformRefunded: 0n };
  // Full refund returns exactly the original parts.
  assert.deepEqual(apportionRefund(base, 999n), { seller: 849n, platform: 150n });
  // Partial: floor(R * S / 10000) to the seller.
  assert.deepEqual(apportionRefund(base, 100n), { seller: 85n, platform: 15n });
  // Many small refunds never exceed either share, and they sum to P exactly.
  let state = { ...base };
  let total = 0n;
  for (const part of [1n, 1n, 1n, 333n, 333n, 330n]) {
    const split = apportionRefund(state, part);
    assert.equal(split.seller + split.platform, part);
    state = { ...state, sellerRefunded: state.sellerRefunded + split.seller, platformRefunded: state.platformRefunded + split.platform };
    total += part;
  }
  assert.equal(total, 999n);
  assert.equal(state.sellerRefunded, 849n);
  assert.equal(state.platformRefunded, 150n);
});

test('refund caps: seller cap binds, then platform cap binds', () => {
  // Seller already got back almost all of its share: the rest goes to the platform part.
  assert.deepEqual(apportionRefund({ price: 1000n, shareBps: 8500n, sellerRefunded: 849n, platformRefunded: 0n }, 100n), { seller: 1n, platform: 99n });
  // Platform part exhausted: the seller part takes the remainder.
  assert.deepEqual(apportionRefund({ price: 1000n, shareBps: 8500n, sellerRefunded: 0n, platformRefunded: 150n }, 100n), { seller: 100n, platform: 0n });
});

test('refund refusals: zero, negative, over the remaining charge, inconsistent state', () => {
  const base = { price: 1000n, shareBps: 8500n, sellerRefunded: 0n, platformRefunded: 0n };
  for (const amount of [0n, -1n, 1001n]) {
    assert.throws(() => apportionRefund(base, amount), (error: unknown) => error instanceof ApiError && error.code === 'RefundExceedsCharge');
  }
  assert.throws(() => apportionRefund({ ...base, sellerRefunded: 500n, platformRefunded: 100n }, 401n), ApiError);
  assert.throws(() => apportionRefund({ ...base, sellerRefunded: 851n }, 1n), RangeError);
});

test('currency codes and amounts', () => {
  assert.equal(isCurrencyCode('USD'), true);
  assert.equal(isCurrencyCode('usd'), false);
  assert.equal(isCurrencyCode('US'), false);
  assert.equal(isCurrencyCode(5), false);
  assert.equal(normalizeCurrency(' eur '), 'EUR');
  assert.throws(() => normalizeCurrency('EURO'), ApiError);
  assert.throws(() => normalizeCurrency(null), ApiError);
  assert.equal(parseAmount(5), 5n);
  assert.equal(parseAmount('123'), 123n);
  assert.equal(parseAmount('0'), 0n);
  for (const bad of [-1, 1.5, '01', '-5', 'abc', null, Number.MAX_SAFE_INTEGER + 1, '1e3']) {
    assert.equal(parseAmount(bad), null);
  }
  assert.equal(toJsonAmount(42n), 42);
  assert.throws(() => toJsonAmount(BigInt(Number.MAX_SAFE_INTEGER) + 1n), RangeError);
  assert.throws(() => toJsonAmount(BigInt(Number.MIN_SAFE_INTEGER) - 1n), RangeError);
});
