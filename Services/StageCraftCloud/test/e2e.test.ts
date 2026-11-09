// End to end over HTTP on an ephemeral port: SQLite (or Postgres with TEST_DATABASE_URL) + FakeProvider, injected clock.
// sign in -> onboard -> publish -> approve -> order (idempotent) -> signed webhook -> entitlement -> download -> refund ->
// webhook -> ledger exact -> payout after the hold period -> manual payout -> admin summary exact.
import test from 'node:test';
import assert from 'node:assert/strict';
import { api, completedSession, harness, manifestJson, sceneJson, serve, signature, stripeEvent, writePackage, type Server } from './helpers.ts';
import { grantRole } from '../src/auth/sessions.ts';
import { Accounts, balance, trialBalance } from '../src/ledger.ts';

const DAY = 24 * 3600 * 1000;

async function signIn(server: Server, name: string): Promise<{ token: string; accountId: string }> {
  const device = (await api(server, 'POST', '/v1/auth/device')).json;
  const page = await api(server, 'GET', `/auth/verify/start?user_code=${device.userCode}&dev_user=${encodeURIComponent(name)}`);
  assert.match(page.text, /Signed in/);
  const session = await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: device.deviceCode } });
  assert.equal(session.status, 200, JSON.stringify(session.json));
  return { token: session.json.sessionToken, accountId: session.json.accountId };
}

test('the whole money path over HTTP', async () => {
  const h = await harness({ PAYOUT_HOLD_DAYS: '14' });
  const server = await serve(h.ctx);
  try {
    const seller = await signIn(server, 'seller');
    const armenian = await signIn(server, 'armenian seller');
    const buyer = await signIn(server, 'buyer');
    const buyer2 = await signIn(server, 'buyer two');
    const adminUser = await signIn(server, 'admin');
    await grantRole(h.ctx.db, adminUser.accountId, 'admin', h.clock.now());
    const admin = adminUser.token;

    // Seller onboarding: Stripe (US) through the fake onboarding; manual (AM) enabled by an admin.
    const onboarding = await api(server, 'POST', '/v1/me/seller/onboarding', { token: seller.token, json: { country: 'US' } });
    await api(server, 'GET', new URL(onboarding.json.onboardingUrl).pathname);
    assert.equal((await api(server, 'GET', '/v1/me', { token: seller.token })).json.seller.payoutsEnabled, true);
    await api(server, 'POST', '/v1/me/seller/onboarding', { token: armenian.token, json: { country: 'AM' } });
    await api(server, 'POST', `/v1/admin/sellers/${armenian.accountId}/manual-payouts`, { token: admin, json: { enabled: true } });

    // Publish two paid listings and approve them.
    const publish = async (token: string, title: string, amount: number) => {
      const scene = sceneJson();
      const { bytes } = writePackage({ scene, manifest: manifestJson({ title, price: { credits: 0, realMoney: { amount, currency: 'USD' } } }, scene) });
      const listing = await api(server, 'POST', '/v1/listings', { token, body: bytes, headers: { 'Content-Type': 'application/octet-stream' } });
      assert.equal(listing.status, 201, JSON.stringify(listing.json));
      assert.equal((await api(server, 'POST', `/v1/admin/moderation/${listing.json.listingId}`, { token: admin, json: { decision: 'approve' } })).json.status, 'Approved');
      return listing.json.listingId as string;
    };
    const rigId = await publish(seller.token, 'Arena Rig', 1999);
    const lampId = await publish(armenian.token, 'Yerevan Lamps', 1000);

    // Orders: the server reads the price; a repeat with the same key returns the same order.
    const create = (token: string, listingId: string, key: string) => api(server, 'POST', '/v1/orders', { token, json: { listingId, idempotencyKey: key } });
    const order = (await create(buyer.token, rigId, 'tx-0000000000000001')).json;
    assert.equal(order.state, 'CheckoutOpen');
    assert.equal(order.amountMinor, 1999);
    assert.deepEqual((await create(buyer.token, rigId, 'tx-0000000000000001')).json, order);
    const order2 = (await create(buyer2.token, rigId, 'tx-0000000000000002')).json;
    const order3 = (await create(buyer.token, lampId, 'tx-0000000000000003')).json;

    // Not paid until a verified webhook says so; a forged one is refused.
    assert.equal((await api(server, 'GET', `/v1/listings/${rigId}/package?purpose=acquire`, { token: buyer.token })).json.error.code, 'NotOwned');
    const webhook = async (type: string, object: Record<string, unknown>, sign = true) => {
      const raw = stripeEvent(type, object);
      return api(server, 'POST', '/v1/webhooks/stripe', { body: raw, headers: { 'Content-Type': 'application/json', 'Stripe-Signature': sign ? signature(raw, h.clock) : 't=1,v1=00' } });
    };
    const forged = await webhook('checkout.session.completed', completedSession({ id: order.orderId, amountMinor: 1999n, currency: 'USD', checkoutSessionId: `cs_fake_${order.orderId}` }), false);
    assert.equal(forged.status, 400);
    assert.equal(forged.json.error.code, 'InvalidSignature');
    for (const o of [order, order2, order3]) {
      const paid = await webhook('checkout.session.completed', completedSession({ id: o.orderId, amountMinor: BigInt(o.amountMinor), currency: 'USD', checkoutSessionId: `cs_fake_${o.orderId}` }));
      assert.equal(paid.json.status, 'processed');
    }
    assert.equal((await api(server, 'GET', `/v1/orders/${order.orderId}`, { token: buyer.token })).json.state, 'Paid');
    const entitlements = (await api(server, 'GET', '/v1/me/entitlements', { token: buyer.token })).json.entitlements;
    assert.deepEqual(entitlements.map((entry: { listingId: string }) => entry.listingId).sort(), [rigId, lampId].sort());
    const download = await api(server, 'GET', `/v1/listings/${rigId}/package?purpose=acquire`, { token: buyer.token });
    assert.equal(download.status, 302);
    const bytes = await fetch(`${server.url}${new URL(download.json.url).pathname}${new URL(download.json.url).search}`);
    assert.equal(bytes.status, 200);

    // Admin refund of 500 on order 1: the provider is asked; nothing posts until its webhook.
    assert.equal((await api(server, 'POST', `/v1/admin/orders/${order.orderId}/refund`, { token: admin, json: { amountMinor: 5000 } })).json.error.code, 'RefundExceedsCharge');
    assert.equal((await api(server, 'POST', `/v1/admin/orders/${order.orderId}/refund`, { token: admin, json: { amountMinor: 1.5 } })).json.error.code, 'InvalidRequest');
    const refund = await api(server, 'POST', `/v1/admin/orders/${order.orderId}/refund`, { token: admin, json: { amountMinor: 500, reason: 'customer request' } });
    assert.equal(refund.status, 202);
    assert.equal(h.provider.refunds.at(-1)!.amountMinor, 500n);
    assert.equal(h.provider.refunds.at(-1)!.idempotencyKey, refund.json.refundRequestId);
    // An in-flight refund counts against what is left.
    assert.equal((await api(server, 'POST', `/v1/admin/orders/${order.orderId}/refund`, { token: admin, json: { amountMinor: 1500 } })).json.error.code, 'RefundExceedsCharge');
    assert.equal((await api(server, 'GET', `/v1/orders/${order.orderId}`, { token: buyer.token })).json.state, 'Paid');
    const pi = h.provider.refunds.at(-1)!.paymentIntentId;
    await webhook('charge.refunded', { id: 'ch_e2e_1', payment_intent: pi, amount_refunded: 500, currency: 'usd' });
    assert.equal((await api(server, 'GET', `/v1/orders/${order.orderId}`, { token: buyer.token })).json.state, 'PartiallyRefunded');
    assert.equal((await h.ctx.db.query("SELECT status FROM refund_requests"))[0]!.status, 'settled');
    // A provider refusal leaves nothing changed.
    h.provider.failNext.refund = true;
    assert.equal((await api(server, 'POST', `/v1/admin/orders/${order2.orderId}/refund`, { token: admin, json: {} })).json.error.code, 'ProviderError');
    assert.equal((await api(server, 'POST', '/v1/admin/orders/00000000-0000-4000-8000-000000000000/refund', { token: admin, json: {} })).status, 404);

    // Ledger, exactly. Order 1: 1999 -> seller 1699, platform 300; refund 500 -> seller 425, platform 75.
    // Order 2: 1999 -> 1699 / 300. Order 3: 1000 -> 850 / 150.
    const db = h.ctx.db;
    assert.equal(await balance(db, Accounts.cash, 'USD'), 1999n + 1999n + 1000n - 500n);
    assert.equal(await balance(db, Accounts.sellerPending(seller.accountId), 'USD'), -(1699n - 425n + 1699n));
    assert.equal(await balance(db, Accounts.sellerPending(armenian.accountId), 'USD'), -850n);
    assert.equal(await balance(db, Accounts.revenue, 'USD'), -(300n + 300n + 150n));
    assert.equal(await balance(db, Accounts.refunds, 'USD'), 75n);
    assert.deepEqual([...(await trialBalance(db))], [['USD', 0n]]);

    // Before the hold period nothing is paid out.
    assert.deepEqual((await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts, []);
    h.clock.advance(15 * DAY);
    const due = (await api(server, 'GET', '/v1/admin/payouts', { token: admin })).json.due;
    assert.ok(due.length >= 2);
    const run = (await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts;
    const stripePayout = run.find((payout: { method: string }) => payout.method === 'stripe');
    const manualPayout = run.find((payout: { method: string }) => payout.method === 'manual');
    assert.equal(stripePayout.amountMinor, 1699 - 425 + 1699);
    assert.equal(stripePayout.status, 'paid');
    assert.equal(h.provider.transfers.at(-1)!.idempotencyKey, stripePayout.payoutId);
    assert.equal(manualPayout.amountMinor, 850);
    assert.equal(manualPayout.status, 'pending');
    assert.deepEqual((await api(server, 'GET', '/v1/me/seller/balance', { token: seller.token })).json.balances, [
      { currency: 'USD', pending: 0, available: 0, inTransit: 0, paidOut: 2973 },
    ]);
    assert.deepEqual((await api(server, 'GET', '/v1/me/seller/balance', { token: armenian.token })).json.balances, [
      { currency: 'USD', pending: 0, available: 0, inTransit: 850, paidOut: 0 },
    ]);
    // Manual payout confirmed by the admin with a bank reference.
    assert.equal((await api(server, 'POST', `/v1/admin/payouts/${manualPayout.payoutId}/mark-paid`, { token: admin, json: {} })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'POST', `/v1/admin/payouts/${manualPayout.payoutId}/mark-paid`, { token: admin, json: { reference: 'AMD-WIRE-1' } })).json.status, 'paid');
    assert.equal((await api(server, 'POST', `/v1/admin/payouts/${manualPayout.payoutId}/mark-paid`, { token: admin, json: { reference: 'again' } })).json.error.code, 'IllegalTransition');
    assert.equal((await api(server, 'POST', `/v1/admin/payouts/${stripePayout.payoutId}/mark-paid`, { token: admin, json: { reference: 'x' } })).json.error.code, 'IllegalTransition');
    assert.equal((await api(server, 'POST', '/v1/admin/payouts/00000000-0000-4000-8000-000000000000/mark-paid', { token: admin, json: { reference: 'x' } })).status, 404);

    // A refund after the payout: the seller's available balance goes negative; the next sale recovers it.
    await webhook('charge.refunded', { id: 'ch_e2e_2', payment_intent: `pi_${order2.orderId.replace(/-/g, '')}`, amount_refunded: 1999, currency: 'usd' });
    assert.equal((await api(server, 'GET', '/v1/me/seller/balance', { token: seller.token })).json.balances[0].available, -1699);
    assert.deepEqual((await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts, []);
    assert.equal((await api(server, 'GET', `/v1/listings/${rigId}/package`, { token: buyer2.token })).json.error.code, 'NotOwned');

    // Admin summary, exactly.
    const summary = (await api(server, 'GET', '/v1/admin/summary', { token: admin })).json;
    assert.deepEqual(summary.currencies, [
      {
        currency: 'USD',
        grossSales: 1999 + 1999 + 1000,
        refunds: 500 + 1999,
        platformRevenue: 300 + 300 + 150 - 75 - 300,
        sellerEarnings: 1699 + 1699 + 850 - 425 - 1699,
        disputesOpened: 0,
        disputesWon: 0,
        disputesLost: 0,
        fees: 0,
        payouts: 2973 + 850,
        salesCount: 3,
      },
    ]);
    const usd = summary.currencies[0];
    assert.equal(usd.grossSales - usd.refunds, usd.platformRevenue + usd.sellerEarnings);
    assert.equal(summary.openModeration, 0);
    // A window that ends before the sales shows nothing.
    assert.deepEqual((await api(server, 'GET', '/v1/admin/summary?to=1000', { token: admin })).json.currencies, []);
    assert.equal((await api(server, 'GET', '/v1/admin/summary?from=yesterday', { token: admin })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'GET', `/v1/admin/summary?from=${new Date(h.clock.now() - 30 * DAY).toISOString()}`, { token: admin })).json.currencies[0].grossSales, 4998);

    // Admin order search and webhook attention list; every admin mutation is audited.
    assert.equal((await api(server, 'GET', '/v1/admin/orders?state=Refunded', { token: admin })).json.orders.length, 1);
    assert.equal((await api(server, 'GET', `/v1/admin/orders?buyerId=${buyer.accountId}&limit=1`, { token: admin })).json.orders.length, 1);
    assert.equal((await api(server, 'GET', '/v1/admin/orders?state=Bogus', { token: admin })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'GET', '/v1/admin/payouts?status=paid', { token: admin })).json.payouts.length, 2);
    assert.ok(Array.isArray((await api(server, 'GET', '/v1/admin/webhooks', { token: admin })).json.events));
    const actions = (await h.ctx.db.query('SELECT action FROM audit_log')).map((row) => row.action);
    for (const action of ['listing.approve', 'order.refund', 'payout.create', 'payout.mark_paid', 'seller.manual_payouts']) {
      assert.ok(actions.includes(action), action);
    }
    assert.deepEqual([...(await trialBalance(db))], [['USD', 0n]]);
  } finally {
    await server.close();
    await h.close();
  }
});

test('payout transfer failures: a refusal returns the balance; a timeout keeps the payout pending and retries with the same key', async () => {
  const h = await harness({ PAYOUT_HOLD_DAYS: '0' });
  const server = await serve(h.ctx);
  try {
    const seller = await signIn(server, 'seller');
    const buyer = await signIn(server, 'buyer');
    const adminUser = await signIn(server, 'admin');
    await grantRole(h.ctx.db, adminUser.accountId, 'admin', h.clock.now());
    const admin = adminUser.token;
    const onboarding = await api(server, 'POST', '/v1/me/seller/onboarding', { token: seller.token, json: { country: 'GB' } });
    await api(server, 'GET', new URL(onboarding.json.onboardingUrl).pathname);
    const scene = sceneJson();
    const { bytes } = writePackage({ scene, manifest: manifestJson({ price: { credits: 0, realMoney: { amount: 1000, currency: 'USD' } } }, scene) });
    const listingId = (await api(server, 'POST', '/v1/listings', { token: seller.token, body: bytes, headers: { 'Content-Type': 'application/octet-stream' } })).json.listingId;
    await api(server, 'POST', `/v1/admin/moderation/${listingId}`, { token: admin, json: { decision: 'approve' } });
    const order = (await api(server, 'POST', '/v1/orders', { token: buyer.token, json: { listingId, idempotencyKey: 'tx-payout-000001' } })).json;
    await fetch(`${server.url}/dev/fake-checkout/cs_fake_${order.orderId}/pay`, { method: 'POST' });

    // A 4xx refusal: the payout fails and the money returns to available.
    const realCreate = h.provider.createTransfer.bind(h.provider);
    const { ProviderFailure } = await import('../src/payments/provider.ts');
    h.provider.createTransfer = async () => {
      throw new ProviderFailure('refused', 400, 'account_invalid');
    };
    const failed = (await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts;
    assert.equal(failed[0].status, 'failed');
    assert.match(failed[0].failure, /account_invalid/);
    assert.equal((await api(server, 'GET', '/v1/me/seller/balance', { token: seller.token })).json.balances[0].available, 850);

    // A timeout: the payout stays pending (the transfer may have reached Stripe) and is retried with the same idempotency key.
    h.provider.createTransfer = async () => {
      throw new ProviderFailure('timeout', 0, null);
    };
    const pending = (await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts;
    assert.equal(pending[0].status, 'pending');
    assert.equal((await api(server, 'GET', '/v1/me/seller/balance', { token: seller.token })).json.balances[0].inTransit, 850);
    h.provider.createTransfer = realCreate;
    const retried = (await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts;
    assert.equal(retried.length, 1);
    assert.equal(retried[0].payoutId, pending[0].payoutId);
    assert.equal(retried[0].status, 'paid');
    assert.equal(h.provider.transfers.at(-1)!.idempotencyKey, pending[0].payoutId);
    // A seller whose connected account is gone: the payout fails without calling the provider.
    await h.ctx.db.query('UPDATE sellers SET provider_account_id = NULL');
    const order2 = (await api(server, 'POST', '/v1/orders', { token: (await signIn(server, 'buyer2')).token, json: { listingId, idempotencyKey: 'tx-payout-000002' } })).json;
    await fetch(`${server.url}/dev/fake-checkout/cs_fake_${order2.orderId}/pay`, { method: 'POST' });
    const orphan = (await api(server, 'POST', '/v1/admin/payouts/run', { token: admin })).json.payouts;
    assert.match(orphan[0].failure, /no connected account/);
    assert.deepEqual([...(await trialBalance(h.ctx.db))], [['USD', 0n]]);
  } finally {
    await server.close();
    await h.close();
  }
});
