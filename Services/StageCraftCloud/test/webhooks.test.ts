import test from 'node:test';
import assert from 'node:assert/strict';
import { completedSession, enableSeller, harness, makeUser, manifestJson, sceneJson, signature, stripeEvent, writePackage, type Harness } from './helpers.ts';
import { publishListing, hasActiveEntitlement } from '../src/listings.ts';
import { moderate } from '../src/admin.ts';
import { createOrder } from '../src/commerce.ts';
import { handleStripeWebhook } from '../src/webhooks.ts';
import { getOrder, type Order } from '../src/orders.ts';
import { Accounts, balance, countEntries, trialBalance } from '../src/ledger.ts';
import { releaseMaturedSales } from '../src/payouts.ts';
import { ApiError } from '../src/errors.ts';

interface Setup {
  h: Harness;
  sellerId: string;
  buyerId: string;
  listingId: string;
  order: Order;
}

async function setup(price = 1000, env: Record<string, string> = {}): Promise<Setup> {
  const h = await harness(env);
  const seller = await makeUser(h.ctx, 'seller');
  const buyer = await makeUser(h.ctx, 'buyer');
  const admin = await makeUser(h.ctx, 'admin', ['admin']);
  await enableSeller(h.ctx, seller.accountId);
  const scene = sceneJson();
  const { bytes } = writePackage({ scene, manifest: manifestJson({ price: { credits: 0, realMoney: { amount: price, currency: 'USD' } } }, scene) });
  const listing = await publishListing(h.ctx, { accountId: seller.accountId, roles: [], sessionHash: 'x' }, bytes);
  await moderate(h.ctx, admin.accountId, listing.id, { decision: 'approve' });
  const order = await createOrder(h.ctx, buyer.accountId, listing.id, 'key-0000000001');
  return { h, sellerId: seller.accountId, buyerId: buyer.accountId, listingId: listing.id, order };
}

async function deliver(h: Harness, type: string, object: Record<string, unknown>, id?: string) {
  const raw = stripeEvent(type, object, id);
  return handleStripeWebhook(h.ctx, raw, signature(raw, h.clock));
}

async function reload(s: Setup): Promise<Order> {
  return (await getOrder(s.h.ctx.db, s.order.id))!;
}

async function assertLedgerBalanced(h: Harness): Promise<void> {
  for (const [, total] of await trialBalance(h.ctx.db)) {
    assert.equal(total, 0n);
  }
}

test('checkout.session.completed (paid): order Paid, sale posted, entitlement granted; replay does nothing', async () => {
  const s = await setup();
  try {
    assert.equal(s.order.state, 'CheckoutOpen');
    const eventId = 'evt_paid_1';
    const outcome = await deliver(s.h, 'checkout.session.completed', completedSession(s.order), eventId);
    assert.equal(outcome.status, 'processed');
    const paid = await reload(s);
    assert.equal(paid.state, 'Paid');
    assert.equal(await hasActiveEntitlement(s.h.ctx.db, s.buyerId, s.listingId), true);
    assert.equal(await balance(s.h.ctx.db, Accounts.cash, 'USD'), 1000n);
    assert.equal(await balance(s.h.ctx.db, Accounts.sellerPending(s.sellerId), 'USD'), -850n);
    assert.equal(await balance(s.h.ctx.db, Accounts.revenue, 'USD'), -150n);
    // Replay of the same event id.
    const replay = await deliver(s.h, 'checkout.session.completed', completedSession(s.order), eventId);
    assert.equal(replay.status, 'duplicate');
    // A different event for the same, already settled payment.
    const again = await deliver(s.h, 'checkout.session.async_payment_succeeded', completedSession(s.order));
    assert.equal(again.status, 'ignored');
    assert.equal(await countEntries(s.h.ctx.db, 'sale'), 1);
    await assertLedgerBalanced(s.h);
  } finally {
    await s.h.close();
  }
});

test('bad signatures are refused with 400 semantics and nothing is stored', async () => {
  const s = await setup();
  try {
    const raw = stripeEvent('checkout.session.completed', completedSession(s.order));
    await assert.rejects(handleStripeWebhook(s.h.ctx, raw, 't=1,v1=00'), (error: unknown) => error instanceof ApiError && error.code === 'InvalidSignature');
    const stale = signature(raw, s.h.clock);
    s.h.clock.advance(301_000);
    await assert.rejects(handleStripeWebhook(s.h.ctx, raw, stale), (error: unknown) => error instanceof ApiError && error.code === 'InvalidSignature');
    await assert.rejects(handleStripeWebhook(s.h.ctx, raw, undefined), ApiError);
    const events = await s.h.ctx.db.query('SELECT COUNT(*) AS n FROM webhook_events');
    assert.equal(Number(events[0]!.n), 0);
    assert.equal((await reload(s)).state, 'CheckoutOpen');
    // Signed but not an event.
    for (const body of ['not json', '{"id":"evt_x"}', '{"id":"","type":"x","data":{"object":{}}}']) {
      const bad = Buffer.from(body);
      await assert.rejects(handleStripeWebhook(s.h.ctx, bad, signature(bad, s.h.clock)), (error: unknown) => error instanceof ApiError && error.code === 'InvalidRequest');
    }
  } finally {
    await s.h.close();
  }
});

test('amount or currency mismatch puts the order on hold and grants nothing', async () => {
  for (const extra of [{ amount_total: 999 }, { currency: 'eur' }]) {
    const s = await setup();
    try {
      const outcome = await deliver(s.h, 'checkout.session.completed', completedSession(s.order, extra));
      assert.equal(outcome.status, 'needs_attention');
      const held = await reload(s);
      assert.equal(held.state, 'OnHold');
      assert.match(held.holdReason!, /expected 1000 USD/);
      assert.equal(await hasActiveEntitlement(s.h.ctx.db, s.buyerId, s.listingId), false);
      assert.equal(await countEntries(s.h.ctx.db, 'sale'), 0);
      const stored = await s.h.ctx.db.query('SELECT status FROM webhook_events');
      assert.equal(stored[0]!.status, 'needs_attention');
    } finally {
      await s.h.close();
    }
  }
});

test('a held order refunded in full becomes Refunded without ledger lines; a partial refund needs attention', async () => {
  const s = await setup();
  try {
    await deliver(s.h, 'checkout.session.completed', completedSession(s.order, { amount_total: 1 }));
    const pi = `pi_${s.order.id.replace(/-/g, '')}`;
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 500, currency: 'usd' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 1000, currency: 'usd' })).status, 'processed');
    assert.equal((await reload(s)).state, 'Refunded');
    assert.equal(await countEntries(s.h.ctx.db, 'refund'), 0);
  } finally {
    await s.h.close();
  }
});

test('delayed payment: unpaid completion, then async success or failure; expiry', async () => {
  const s = await setup();
  try {
    const unpaid = await deliver(s.h, 'checkout.session.completed', completedSession(s.order, { payment_status: 'unpaid' }));
    assert.equal(unpaid.detail, 'awaiting asynchronous payment');
    const open = await reload(s);
    assert.equal(open.state, 'CheckoutOpen');
    assert.ok(open.paymentIntentId);
    assert.equal((await deliver(s.h, 'checkout.session.async_payment_succeeded', completedSession(s.order))).status, 'processed');
    assert.equal((await reload(s)).state, 'Paid');
    // Expiry after payment is an illegal transition: kept for the admin, nothing changes.
    const late = await deliver(s.h, 'checkout.session.expired', { id: s.order.checkoutSessionId, client_reference_id: s.order.id });
    assert.equal(late.status, 'needs_attention');
    assert.equal((await reload(s)).state, 'Paid');
  } finally {
    await s.h.close();
  }

  const f = await setup();
  try {
    assert.equal((await deliver(f.h, 'checkout.session.async_payment_failed', completedSession(f.order))).status, 'processed');
    assert.equal((await reload(f)).state, 'Failed');
    assert.equal(await hasActiveEntitlement(f.h.ctx.db, f.buyerId, f.listingId), false);
  } finally {
    await f.h.close();
  }

  const e = await setup();
  try {
    assert.equal((await deliver(e.h, 'checkout.session.expired', { id: e.order.checkoutSessionId })).status, 'processed');
    assert.equal((await reload(e)).state, 'Expired');
    // A payment arriving for an expired order is refused (illegal), never forced.
    assert.equal((await deliver(e.h, 'checkout.session.completed', completedSession(e.order))).status, 'needs_attention');
    assert.equal((await reload(e)).state, 'Expired');
    assert.equal(await countEntries(e.h.ctx.db, 'sale'), 0);
  } finally {
    await e.h.close();
  }
});

test('unknown orders, unknown events and odd payloads are stored and answered without changes', async () => {
  const s = await setup();
  try {
    assert.equal((await deliver(s.h, 'checkout.session.completed', { id: 'cs_unknown', payment_status: 'paid' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'checkout.session.completed', { id: 'cs_x', client_reference_id: 'not-a-uuid' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'checkout.session.async_payment_succeeded', { id: 'cs_unknown' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'checkout.session.expired', { id: 'cs_unknown' })).status, 'ignored');
    assert.equal((await deliver(s.h, 'customer.created', { id: 'cus_1' })).status, 'ignored');
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_x', payment_intent: 'pi_unknown', amount_refunded: 1 })).status, 'ignored');
    assert.equal((await deliver(s.h, 'charge.dispute.created', { id: 'dp_x', payment_intent: 'pi_unknown', amount: 1 })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'charge.dispute.closed', { id: 'dp_unknown', status: 'won' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'account.updated', { id: 'acct_unknown', payouts_enabled: true })).status, 'ignored');
    assert.equal((await deliver(s.h, 'account.updated', { id: 'acct_unknown' })).status, 'ignored');
    assert.equal((await deliver(s.h, 'checkout.session.completed', completedSession(s.order, { payment_status: 'no_payment_required' }))).status, 'needs_attention');
    // A session id that is not the one opened for the order.
    assert.equal((await deliver(s.h, 'checkout.session.completed', completedSession(s.order, { id: 'cs_other' }))).status, 'needs_attention');
    const rows = await s.h.ctx.db.query('SELECT COUNT(*) AS n FROM webhook_events');
    assert.equal(Number(rows[0]!.n), 12);
    assert.equal((await reload(s)).state, 'CheckoutOpen');
  } finally {
    await s.h.close();
  }
});

test('refunds: partial then full, entitlement revoked on full, over-refund and currency mismatch need attention', async () => {
  const s = await setup(999);
  try {
    await deliver(s.h, 'checkout.session.completed', completedSession(s.order));
    const pi = `pi_${s.order.id.replace(/-/g, '')}`;
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 100, currency: 'usd' })).detail, 'order partially refunded');
    let order = await reload(s);
    assert.equal(order.state, 'PartiallyRefunded');
    assert.equal(order.sellerRefunded, 85n);
    assert.equal(order.platformRefunded, 15n);
    assert.equal(await hasActiveEntitlement(s.h.ctx.db, s.buyerId, s.listingId), true);
    // The same cumulative amount again (a different event id) posts nothing.
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 100, currency: 'usd' })).status, 'ignored');
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 1000, currency: 'usd' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: pi, amount_refunded: 200, currency: 'eur' })).status, 'needs_attention');
    assert.equal((await deliver(s.h, 'charge.refunded', { id: 'ch_1', payment_intent: { id: pi }, amount_refunded: 999, currency: 'usd' })).detail, 'order refunded');
    order = await reload(s);
    assert.equal(order.state, 'Refunded');
    assert.equal(order.sellerRefunded, 849n);
    assert.equal(order.platformRefunded, 150n);
    assert.equal(await hasActiveEntitlement(s.h.ctx.db, s.buyerId, s.listingId), false);
    assert.equal(await balance(s.h.ctx.db, Accounts.cash, 'USD'), 0n);
    assert.equal(await balance(s.h.ctx.db, Accounts.sellerPending(s.sellerId), 'USD'), 0n);
    assert.equal(await balance(s.h.ctx.db, Accounts.refunds, 'USD'), 150n);
    await assertLedgerBalanced(s.h);
  } finally {
    await s.h.close();
  }
});

test('a refund after the hold period comes out of available, which may go negative', async () => {
  const s = await setup(1000);
  try {
    await deliver(s.h, 'checkout.session.completed', completedSession(s.order));
    s.h.clock.advance(15 * 24 * 3600 * 1000);
    assert.equal(await releaseMaturedSales(s.h.ctx), 1);
    assert.equal(await balance(s.h.ctx.db, Accounts.sellerAvailable(s.sellerId), 'USD'), -850n);
    // Simulate the seller having been paid out already: the available balance moved to paid.
    await s.h.ctx.db.query("UPDATE orders SET released_at = released_at WHERE id = $1", [s.order.id]);
    const pi = `pi_${s.order.id.replace(/-/g, '')}`;
    await deliver(s.h, 'charge.refunded', { id: 'ch_2', payment_intent: pi, amount_refunded: 1000, currency: 'usd' });
    assert.equal(await balance(s.h.ctx.db, Accounts.sellerAvailable(s.sellerId), 'USD'), 0n);
    assert.equal(await balance(s.h.ctx.db, Accounts.sellerPending(s.sellerId), 'USD'), 0n);
    await assertLedgerBalanced(s.h);
  } finally {
    await s.h.close();
  }
});

test('disputes: opened moves money to disputes, won moves it back, lost reverses the sale and posts the fee', async () => {
  const won = await setup(1000);
  try {
    await deliver(won.h, 'checkout.session.completed', completedSession(won.order));
    const pi = `pi_${won.order.id.replace(/-/g, '')}`;
    assert.equal((await deliver(won.h, 'charge.dispute.created', { id: 'dp_1', payment_intent: pi, amount: 1000, currency: 'usd', status: 'needs_response' })).status, 'processed');
    assert.equal((await reload(won)).state, 'Disputed');
    assert.equal(await balance(won.h.ctx.db, Accounts.disputes, 'USD'), 1000n);
    assert.equal(await balance(won.h.ctx.db, Accounts.cash, 'USD'), 0n);
    assert.equal((await deliver(won.h, 'charge.dispute.created', { id: 'dp_1', payment_intent: pi, amount: 1000, currency: 'usd' })).status, 'ignored');
    // A disputed order is not released at the end of its hold period.
    won.h.clock.advance(20 * 24 * 3600 * 1000);
    assert.equal(await releaseMaturedSales(won.h.ctx), 0);
    assert.equal((await deliver(won.h, 'charge.dispute.closed', { id: 'dp_1', status: 'won' })).status, 'processed');
    assert.equal((await reload(won)).state, 'Paid');
    assert.equal(await balance(won.h.ctx.db, Accounts.disputes, 'USD'), 0n);
    assert.equal(await balance(won.h.ctx.db, Accounts.cash, 'USD'), 1000n);
    assert.equal((await deliver(won.h, 'charge.dispute.closed', { id: 'dp_1', status: 'won' })).status, 'ignored');
    assert.equal(await hasActiveEntitlement(won.h.ctx.db, won.buyerId, won.listingId), true);
    await assertLedgerBalanced(won.h);
  } finally {
    await won.h.close();
  }

  const lost = await setup(1000);
  try {
    await deliver(lost.h, 'checkout.session.completed', completedSession(lost.order));
    const pi = `pi_${lost.order.id.replace(/-/g, '')}`;
    await deliver(lost.h, 'charge.dispute.created', { id: 'dp_2', payment_intent: pi, amount: 1000, currency: 'usd', status: 'needs_response' });
    const closed = await deliver(lost.h, 'charge.dispute.closed', { id: 'dp_2', status: 'lost', balance_transactions: [{ fee: 1500 }, { fee: 0 }, 'x'] });
    assert.equal(closed.status, 'processed');
    const order = await reload(lost);
    assert.equal(order.state, 'Refunded');
    assert.equal(await hasActiveEntitlement(lost.h.ctx.db, lost.buyerId, lost.listingId), false);
    assert.equal(await balance(lost.h.ctx.db, Accounts.disputes, 'USD'), 0n);
    assert.equal(await balance(lost.h.ctx.db, Accounts.fees, 'USD'), 1500n);
    assert.equal(await balance(lost.h.ctx.db, Accounts.cash, 'USD'), -1500n);
    assert.equal(await balance(lost.h.ctx.db, Accounts.sellerPending(lost.sellerId), 'USD'), 0n);
    await assertLedgerBalanced(lost.h);
  } finally {
    await lost.h.close();
  }
});

test('disputes: partial loss after a partial refund, fallback fee, inquiries, unexpected status, oversize dispute', async () => {
  const s = await setup(1000);
  try {
    await deliver(s.h, 'checkout.session.completed', completedSession(s.order));
    const pi = `pi_${s.order.id.replace(/-/g, '')}`;
    await deliver(s.h, 'charge.refunded', { id: 'ch_3', payment_intent: pi, amount_refunded: 200, currency: 'usd' });
    // An inquiry moves no money and changes no state.
    assert.equal((await deliver(s.h, 'charge.dispute.created', { id: 'dp_i', payment_intent: pi, amount: 800, currency: 'usd', status: 'warning_needs_response' })).detail, 'inquiry recorded');
    assert.equal((await reload(s)).state, 'PartiallyRefunded');
    assert.equal((await deliver(s.h, 'charge.dispute.closed', { id: 'dp_i', status: 'warning_closed' })).detail, 'inquiry closed');
    // A dispute for more than is left of the charge needs attention.
    assert.equal((await deliver(s.h, 'charge.dispute.created', { id: 'dp_big', payment_intent: pi, amount: 900, currency: 'usd', status: 'needs_response' })).status, 'needs_attention');
    assert.equal((await reload(s)).state, 'PartiallyRefunded');
    // A partial dispute that is lost, with no fee information: the fallback fee is posted.
    await deliver(s.h, 'charge.dispute.created', { id: 'dp_3', payment_intent: pi, amount: 300, currency: 'usd', status: 'needs_response' });
    assert.equal((await reload(s)).state, 'Disputed');
    await deliver(s.h, 'charge.dispute.closed', { id: 'dp_3', status: 'lost' });
    const order = await reload(s);
    assert.equal(order.state, 'PartiallyRefunded');
    assert.equal(order.sellerRefunded + order.platformRefunded, 500n);
    assert.equal(await balance(s.h.ctx.db, Accounts.fees, 'USD'), 1500n);
    assert.equal(await hasActiveEntitlement(s.h.ctx.db, s.buyerId, s.listingId), true);
    // Unexpected closing status.
    await deliver(s.h, 'charge.dispute.created', { id: 'dp_4', payment_intent: pi, amount: 100, currency: 'usd', status: 'needs_response' });
    assert.equal((await deliver(s.h, 'charge.dispute.closed', { id: 'dp_4', status: 'under_review' })).status, 'needs_attention');
    await assertLedgerBalanced(s.h);
  } finally {
    await s.h.close();
  }
});

test('a won dispute after a partial refund returns to PartiallyRefunded; a lost dispute that over-refunds is rolled back', async () => {
  const s = await setup(1000);
  try {
    await deliver(s.h, 'checkout.session.completed', completedSession(s.order));
    const pi = `pi_${s.order.id.replace(/-/g, '')}`;
    await deliver(s.h, 'charge.refunded', { id: 'ch_4', payment_intent: pi, amount_refunded: 100, currency: 'usd' });
    await deliver(s.h, 'charge.dispute.created', { id: 'dp_5', payment_intent: pi, amount: 900, currency: 'usd', status: 'needs_response' });
    // Corrupt the stored dispute amount so a "lost" would exceed the charge: the savepoint rolls the handler back.
    await s.h.ctx.db.query("UPDATE disputes SET amount = 5000 WHERE id = 'dp_5'");
    const refused = await deliver(s.h, 'charge.dispute.closed', { id: 'dp_5', status: 'lost' });
    assert.equal(refused.status, 'needs_attention');
    const still = await s.h.ctx.db.query("SELECT closed_at FROM disputes WHERE id = 'dp_5'");
    assert.equal(still[0]!.closed_at, null);
    await s.h.ctx.db.query("UPDATE disputes SET amount = 900 WHERE id = 'dp_5'");
    await deliver(s.h, 'charge.dispute.closed', { id: 'dp_5', status: 'won' });
    assert.equal((await reload(s)).state, 'PartiallyRefunded');
    await assertLedgerBalanced(s.h);
  } finally {
    await s.h.close();
  }
});

test('account.updated turns Stripe payouts on and off for a known connected account', async () => {
  const s = await setup();
  try {
    const rows = await s.h.ctx.db.query('SELECT provider_account_id FROM sellers WHERE account_id = $1', [s.sellerId]);
    const acct = String(rows[0]!.provider_account_id);
    assert.equal((await deliver(s.h, 'account.updated', { id: acct, payouts_enabled: false })).status, 'processed');
    assert.equal(Number((await s.h.ctx.db.query('SELECT payouts_enabled FROM sellers WHERE account_id = $1', [s.sellerId]))[0]!.payouts_enabled), 0);
  } finally {
    await s.h.close();
  }
});

test('an unexpected error rolls back the event row, so Stripe retries', async () => {
  const s = await setup();
  try {
    const raw = stripeEvent('checkout.session.completed', completedSession(s.order), 'evt_boom');
    const original = s.h.ctx.db.withTransaction.bind(s.h.ctx.db);
    // Break the ledger insert inside the webhook transaction.
    s.h.ctx.db.withTransaction = (fn) =>
      original((tx) =>
        fn({
          query: async (sql, params) => {
            if (sql.includes('INSERT INTO journal_lines')) throw new Error('disk full');
            return tx.query(sql, params);
          },
        }),
      );
    await assert.rejects(handleStripeWebhook(s.h.ctx, raw, signature(raw, s.h.clock)), /disk full/);
    s.h.ctx.db.withTransaction = original;
    const stored = await s.h.ctx.db.query("SELECT COUNT(*) AS n FROM webhook_events WHERE id = 'evt_boom'");
    assert.equal(Number(stored[0]!.n), 0);
    assert.equal((await reload(s)).state, 'CheckoutOpen');
    // The retry succeeds.
    assert.equal((await handleStripeWebhook(s.h.ctx, raw, signature(raw, s.h.clock))).status, 'processed');
  } finally {
    await s.h.close();
  }
});
