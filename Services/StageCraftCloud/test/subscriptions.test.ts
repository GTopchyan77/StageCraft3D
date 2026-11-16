// StageCraft Pro subscriptions and licences (ADR 0034 §17): checkout, webhook-only settlement with out-of-order events, signed
// licence tokens, the machine limit, the long poll, and the development checkout and portal pages.
import test from 'node:test';
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { api, harness, makeUser, serve, signature, type Harness } from './helpers.ts';
import { handleStripeWebhook } from '../src/webhooks.ts';
import { createSubscriptionCheckout, currentSubscription, isEntitled } from '../src/subscriptions.ts';
import { issueLicence, releaseLicence } from '../src/licences.ts';
import { createSigner, DEV_KEY_ID, DEV_SIGNING_KEY_PKCS8, loadSigningKey, PRO_ENTITLEMENTS, verifyToken } from '../src/licence/token.ts';
import { ApiError } from '../src/errors.ts';

const MACHINE_A = 'a'.repeat(64);
const MACHINE_B = 'b'.repeat(64);
const MACHINE_C = 'c'.repeat(64);
const MACHINE_D = 'd'.repeat(64);

/** The development public key, as StageLicenceToken.cpp's development entry spells it. */
const DEV_PUBLIC_KEY_HEX = 'd02da7fbe4e12f2d1435125b3cbf0961cfa8ce4269adf910cb937dc0e82619f0';

async function deliver(h: Harness, type: string, object: Record<string, unknown>, created: number, id = `evt_${randomUUID().replace(/-/g, '')}`) {
  const raw = Buffer.from(JSON.stringify({ id, object: 'event', type, created, data: { object } }), 'utf8');
  return handleStripeWebhook(h.ctx, raw, signature(raw, h.clock));
}

function subscriptionObject(accountId: string, status: string, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: `sub_${accountId.replace(/-/g, '').slice(0, 20)}`,
    object: 'subscription',
    status,
    customer: `cus_${accountId.replace(/-/g, '').slice(0, 12)}`,
    metadata: { accountId, plan: 'monthly' },
    cancel_at_period_end: false,
    current_period_end: 1_900_000_000,
    ...extra,
  };
}

async function withHarness(fn: (h: Harness) => Promise<void>): Promise<void> {
  const h = await harness();
  try {
    await fn(h);
  } finally {
    await h.close();
  }
}

async function rejectsWith(promise: Promise<unknown>, code: string): Promise<void> {
  await assert.rejects(promise, (error: unknown) => error instanceof ApiError && error.code === code);
}

test('licence tokens: signed with Ed25519, verified against the public key, any change refused', () => {
  const signer = createSigner(DEV_KEY_ID, loadSigningKey(DEV_SIGNING_KEY_PKCS8));
  assert.equal(signer.publicKeyRaw().toString('hex'), DEV_PUBLIC_KEY_HEX, 'the client trusts this exact development key');
  const token = signer.sign({ sub: 'acct', mid: MACHINE_A, plan: 'monthly', ent: [...PRO_ENTITLEMENTS], iat: 100, exp: 200 });
  assert.match(token, /^SC1\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/);
  const payload = verifyToken(token, signer.publicKeyRaw());
  assert.deepEqual(payload, { v: 1, kid: DEV_KEY_ID, sub: 'acct', mid: MACHINE_A, plan: 'monthly', ent: [...PRO_ENTITLEMENTS], iat: 100, exp: 200 });

  const [prefix, body, sig] = token.split('.');
  const forgedBody = Buffer.from(JSON.stringify({ ...payload, exp: 9_999_999_999 })).toString('base64url');
  assert.equal(verifyToken(`${prefix}.${forgedBody}.${sig}`, signer.publicKeyRaw()), null, 'an edited payload fails');
  assert.equal(verifyToken(`${prefix}.${body}.${sig!.slice(0, -2)}AA`, signer.publicKeyRaw()), null, 'an edited signature fails');
  assert.equal(verifyToken(token, Buffer.alloc(32, 7)), null, 'another key fails');
  assert.equal(verifyToken('SC2.x.y', signer.publicKeyRaw()), null);
});

test('checkout: idempotent per key, one open checkout per account, plan and Price required, provider failure is retryable', async () => {
  await withHarness(async (h) => {
    const user = await makeUser(h.ctx, 'pro');
    await rejectsWith(createSubscriptionCheckout(h.ctx, user.accountId, 'weekly', 'key-00000001'), 'InvalidRequest');
    await rejectsWith(createSubscriptionCheckout(h.ctx, user.accountId, 'monthly', 'short'), 'InvalidRequest');

    const first = await createSubscriptionCheckout(h.ctx, user.accountId, 'monthly', 'key-00000001');
    assert.equal(first.state, 'Open');
    assert.match(first.url ?? '', /\/dev\/fake-subscription\/cs_fake_sub_/);
    assert.equal(h.provider.subscriptionCheckouts.length, 1);
    assert.equal(h.provider.subscriptionCheckouts[0]!.priceId, 'price_fake_pro_monthly', 'the Price comes from the server config');
    assert.equal(h.provider.subscriptionCheckouts[0]!.accountId, user.accountId);

    const repeat = await createSubscriptionCheckout(h.ctx, user.accountId, 'monthly', 'key-00000001');
    assert.equal(repeat.id, first.id, 'the same key returns the same checkout');
    const again = await createSubscriptionCheckout(h.ctx, user.accountId, 'monthly', 'key-00000002');
    assert.equal(again.id, first.id, 'an open checkout of the same plan is reused');
    assert.equal(h.provider.subscriptionCheckouts.length, 1, 'and Stripe is asked once');
    await rejectsWith(createSubscriptionCheckout(h.ctx, user.accountId, 'yearly', 'key-00000003'), 'PurchasePending');
    await rejectsWith(createSubscriptionCheckout(h.ctx, user.accountId, 'yearly', 'key-00000001'), 'IdempotencyConflict');

    h.ctx.config.subscriptions.prices.yearly = null;
    const other = await makeUser(h.ctx, 'other');
    await rejectsWith(createSubscriptionCheckout(h.ctx, other.accountId, 'yearly', 'key-00000004'), 'PlanNotAvailable');

    h.provider.failNext.subscription = true;
    await rejectsWith(createSubscriptionCheckout(h.ctx, other.accountId, 'monthly', 'key-00000005'), 'ProviderError');
    const retried = await createSubscriptionCheckout(h.ctx, other.accountId, 'monthly', 'key-00000006');
    assert.equal(retried.state, 'Open', 'a failed checkout does not block the next attempt');

    // Once Pro is active, a second subscription is refused.
    await deliver(h, 'customer.subscription.created', subscriptionObject(user.accountId, 'active'), 1000);
    h.clock.advance(31 * 60_000);
    await rejectsWith(createSubscriptionCheckout(h.ctx, user.accountId, 'monthly', 'key-00000007'), 'AlreadySubscribed');
  });
});

test('webhooks: only signed subscription events grant Pro; late events never overwrite newer state', async () => {
  await withHarness(async (h) => {
    const user = await makeUser(h.ctx, 'pro');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), false);

    const created = await deliver(h, 'customer.subscription.created', subscriptionObject(user.accountId, 'incomplete'), 1000);
    assert.equal(created.status, 'processed');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), false, 'incomplete is not Pro');

    // Newer API versions put the period end on the item.
    const active = subscriptionObject(user.accountId, 'active', { current_period_end: undefined, items: { data: [{ current_period_end: 1_950_000_000, price: { id: 'price_x' } }] } });
    assert.equal((await deliver(h, 'customer.subscription.updated', active, 1010)).status, 'processed');
    const now = await currentSubscription(h.ctx.db, user.accountId);
    assert.equal(now?.status, 'active');
    assert.equal(now?.currentPeriodEnd, 1_950_000_000_000);
    assert.equal(now?.plan, 'monthly');
    assert.equal(now?.customerId, `cus_${user.accountId.replace(/-/g, '').slice(0, 12)}`);

    // Stripe delivers the older "incomplete" again, later: ignored.
    const late = await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'incomplete'), 1005);
    assert.equal(late.status, 'ignored');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), true);

    // The same event id twice is stored once.
    const twice = `evt_${randomUUID().replace(/-/g, '')}`;
    await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'active', { cancel_at_period_end: true }), 1020, twice);
    assert.equal((await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'active'), 1020, twice)).status, 'duplicate');
    assert.equal((await currentSubscription(h.ctx.db, user.accountId))?.cancelAtPeriodEnd, true);

    // Unknown accounts and a subscription that changes account are kept for the admin, not applied.
    assert.equal((await deliver(h, 'customer.subscription.updated', subscriptionObject(randomUUID(), 'active'), 1030)).status, 'needs_attention');
    const intruder = await makeUser(h.ctx, 'intruder');
    const moved = { ...subscriptionObject(user.accountId, 'active'), metadata: { accountId: intruder.accountId } };
    assert.equal((await deliver(h, 'customer.subscription.updated', moved, 1040)).status, 'needs_attention');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, intruder.accountId)), false);

    // Same second: an earlier stage never undoes a later one (Stripe's created is whole seconds).
    assert.equal((await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'incomplete'), 1020)).status, 'ignored');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), true, 'a same-second incomplete does not undo active');

    // Past due keeps Pro while Stripe retries the card; unpaid ends it; deletion is final.
    await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'past_due'), 1050);
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), true, 'past_due keeps Pro during retries');
    await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'unpaid'), 1055);
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, user.accountId)), false, 'unpaid is not Pro');
    await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'active'), 1060);
    await deliver(h, 'customer.subscription.deleted', subscriptionObject(user.accountId, 'active'), 1070);
    assert.equal((await currentSubscription(h.ctx.db, user.accountId))?.status, 'canceled', 'a deletion cancels whatever the snapshot says');
    assert.equal((await deliver(h, 'customer.subscription.updated', subscriptionObject(user.accountId, 'active'), 1075)).status, 'ignored', 'canceled is final');

    // The price decides the plan: a switch in the Billing Portal keeps the old metadata.
    const switcher = await makeUser(h.ctx, 'switcher');
    const switched = subscriptionObject(switcher.accountId, 'active', { items: { data: [{ price: { id: 'price_fake_pro_yearly' } }] } });
    await deliver(h, 'customer.subscription.updated', switched, 1076);
    assert.equal((await currentSubscription(h.ctx.db, switcher.accountId))?.plan, 'yearly');

    // A subscription checkout's completion only closes the checkout record.
    const checkout = await createSubscriptionCheckout(h.ctx, intruder.accountId, 'yearly', 'key-00000010');
    const done = await deliver(h, 'checkout.session.completed', { id: checkout.sessionId, object: 'checkout.session', mode: 'subscription', client_reference_id: checkout.id, payment_status: 'paid' }, 1080);
    assert.equal(done.status, 'processed');
    assert.equal(isEntitled(await currentSubscription(h.ctx.db, intruder.accountId)), false, 'Pro comes from the subscription event');
    const row = await h.ctx.db.query('SELECT state FROM subscription_checkouts WHERE id = $1', [checkout.id]);
    assert.equal(row[0]!.state, 'Completed');
  });
});

test('licences: signed for one account and machine, 14 days, at most three machines, release bounded', async () => {
  await withHarness(async (h) => {
    const user = await makeUser(h.ctx, 'pro');
    await rejectsWith(issueLicence(h.ctx, user.accountId, MACHINE_A), 'NoActiveSubscription');
    await deliver(h, 'customer.subscription.created', subscriptionObject(user.accountId, 'active'), 1000);

    const licence = await issueLicence(h.ctx, user.accountId, MACHINE_A);
    const payload = verifyToken(licence.token, h.ctx.signer.publicKeyRaw());
    assert.ok(payload, 'the token verifies with the server key');
    assert.equal(payload.sub, user.accountId);
    assert.equal(payload.mid, MACHINE_A);
    assert.equal(payload.plan, 'monthly');
    assert.deepEqual(payload.ent, [...PRO_ENTITLEMENTS]);
    assert.equal(payload.iat, Math.floor(h.clock.now() / 1000));
    assert.equal(payload.exp - payload.iat, 14 * 86_400, 'offline for 14 days');
    assert.equal(licence.expiresAt, payload.exp * 1000);

    await issueLicence(h.ctx, user.accountId, MACHINE_B);
    await issueLicence(h.ctx, user.accountId, MACHINE_C);
    await rejectsWith(issueLicence(h.ctx, user.accountId, MACHINE_D), 'MachineLimitReached');
    await issueLicence(h.ctx, user.accountId, MACHINE_A);

    assert.deepEqual(await releaseLicence(h.ctx, user.accountId, MACHINE_C), { released: true });
    assert.deepEqual(await releaseLicence(h.ctx, user.accountId, MACHINE_C), { released: false }, 'releasing twice changes nothing');
    await issueLicence(h.ctx, user.accountId, MACHINE_D);

    await releaseLicence(h.ctx, user.accountId, MACHINE_A);
    await releaseLicence(h.ctx, user.accountId, MACHINE_B);
    await rejectsWith(releaseLicence(h.ctx, user.accountId, MACHINE_D), 'MachineLimitReached');

    await deliver(h, 'customer.subscription.deleted', subscriptionObject(user.accountId, 'canceled'), 2000);
    await rejectsWith(issueLicence(h.ctx, user.accountId, MACHINE_D), 'NoActiveSubscription');
  });
});

test('HTTP: Pro routes need a session, validate input and long-poll; the development checkout and portal settle through webhooks', async () => {
  await withHarness(async (h) => {
    const server = await serve(h.ctx);
    try {
      const user = await makeUser(h.ctx, 'pro');
      assert.equal((await api(server, 'POST', '/v1/licences', { json: { machineId: MACHINE_A } })).status, 401);
      assert.equal((await api(server, 'GET', '/v1/me/subscription')).status, 401);
      const badMachine = await api(server, 'POST', '/v1/licences', { token: user.token, json: { machineId: 'ABC' } });
      assert.equal(badMachine.json.error.code, 'InvalidRequest');
      const refused = await api(server, 'POST', '/v1/licences', { token: user.token, json: { machineId: MACHINE_A } });
      assert.equal(refused.status, 403);
      assert.equal(refused.json.error.code, 'NoActiveSubscription');
      assert.equal((await api(server, 'GET', '/v1/me/subscription?waitSeconds=26', { token: user.token })).status, 400);
      assert.deepEqual((await api(server, 'GET', '/v1/me/subscription', { token: user.token })).json, {
        status: 'none',
        entitled: false,
        plan: null,
        currentPeriodEnd: null,
        cancelAtPeriodEnd: false,
      });
      assert.equal((await api(server, 'POST', '/v1/subscriptions/portal', { token: user.token })).json.error.code, 'NoActiveSubscription');

      const checkout = await api(server, 'POST', '/v1/subscriptions/checkout', { token: user.token, json: { plan: 'yearly', idempotencyKey: 'key-00000020' } });
      assert.equal(checkout.status, 200);
      assert.equal(checkout.json.state, 'Open');
      const sessionPath = new URL(checkout.json.url).pathname;

      // The long poll answers when the subscription changes, not at its deadline.
      const started = Date.now();
      const waiting = api(server, 'GET', '/v1/me/subscription?waitSeconds=20', { token: user.token });
      await new Promise((resolve) => setTimeout(resolve, 100));
      assert.equal((await api(server, 'POST', `${sessionPath}/pay`)).status, 200);
      const changed = await waiting;
      assert.ok(Date.now() - started < 10_000, 'woken by the webhook');
      assert.equal(changed.json.entitled, true);
      assert.equal(changed.json.plan, 'yearly');

      const issued = await api(server, 'POST', '/v1/licences', { token: user.token, json: { machineId: MACHINE_A } });
      assert.equal(issued.status, 200);
      assert.deepEqual(issued.json.entitlements, [...PRO_ENTITLEMENTS]);
      assert.ok(verifyToken(issued.json.token, h.ctx.signer.publicKeyRaw()));

      const portal = await api(server, 'POST', '/v1/subscriptions/portal', { token: user.token });
      assert.match(portal.json.url, /\/dev\/fake-portal\/cus_fake_/);
      assert.equal((await api(server, 'POST', `${new URL(portal.json.url).pathname}/cancel`)).status, 200);
      assert.equal((await api(server, 'POST', '/v1/licences', { token: user.token, json: { machineId: MACHINE_A } })).json.error.code, 'NoActiveSubscription');
      assert.equal((await api(server, 'POST', '/v1/licences/release', { token: user.token, json: { machineId: MACHINE_A } })).json.released, true);
    } finally {
      await server.close();
    }
  });
});
