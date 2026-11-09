// HTTP-level tests of the router, sign-in, listings, orders, support, admin and payouts endpoints.
import test from 'node:test';
import assert from 'node:assert/strict';
import { api, enableSeller, harness, makeUser, manifestJson, PNG_1x1, sceneItem, sceneJson, serve, writePackage, type Harness, type Server } from './helpers.ts';
import { RateLimiter } from '../src/http/rateLimit.ts';
import { clientIp } from '../src/http/router.ts';

async function withServer(fn: (h: Harness, server: Server) => Promise<void>, env: Record<string, string> = {}): Promise<void> {
  const h = await harness(env);
  const server = await serve(h.ctx);
  try {
    await fn(h, server);
  } finally {
    await server.close();
    await h.close();
  }
}

function paidPackage(amount = 1000, extra: Record<string, unknown> = {}) {
  const scene = sceneJson();
  return writePackage({ scene, manifest: manifestJson({ price: { credits: 0, realMoney: { amount, currency: 'USD' } }, ...extra }, scene) });
}

test('router: unknown paths, wrong methods, security headers, request ids, error shape', async () => {
  await withServer(async (_h, server) => {
    const missing = await api(server, 'GET', '/nope');
    assert.equal(missing.status, 404);
    assert.deepEqual(missing.json, { error: { code: 'NotFound', message: 'no such endpoint' } });
    assert.equal((await api(server, 'PUT', '/v1/listings')).json.error.code, 'MethodNotAllowed');
    const health = await api(server, 'GET', '/healthz');
    assert.equal(health.status, 200);
    for (const header of ['x-content-type-options', 'x-frame-options', 'strict-transport-security', 'content-security-policy', 'referrer-policy']) {
      assert.ok(health.headers.get(header), header);
    }
    assert.match(health.headers.get('x-request-id')!, /^[0-9a-f-]{36}$/);
    assert.equal((await api(server, 'GET', '/readyz')).json.status, 'ready');
  });
});

test('auth: missing, malformed, unknown, expired and revoked sessions; admin role from the database only', async () => {
  await withServer(async (h, server) => {
    assert.equal((await api(server, 'GET', '/v1/me')).json.error.code, 'Unauthenticated');
    assert.equal((await api(server, 'GET', '/v1/me', { headers: { Authorization: 'Basic abc' } })).status, 401);
    assert.equal((await api(server, 'GET', '/v1/me', { token: 'scs_unknownunknownunknown' })).status, 401);
    assert.equal((await api(server, 'GET', '/v1/me', { token: 'not_prefixed_token_value' })).status, 401);
    const user = await makeUser(h.ctx, 'alice');
    const me = await api(server, 'GET', '/v1/me', { token: user.token });
    assert.equal(me.status, 200);
    assert.deepEqual(me.json.roles, []);
    assert.deepEqual(me.json.seller, { status: 'none', payoutMethod: null, payoutsEnabled: false, country: null });
    assert.equal((await api(server, 'GET', '/v1/admin/summary', { token: user.token })).json.error.code, 'Forbidden');
    const admin = await makeUser(h.ctx, 'root', ['admin']);
    assert.equal((await api(server, 'GET', '/v1/admin/summary', { token: admin.token })).status, 200);
    // Logout revokes; the token stops working.
    assert.equal((await api(server, 'POST', '/v1/auth/logout', { token: user.token })).status, 204);
    assert.equal((await api(server, 'GET', '/v1/me', { token: user.token })).status, 401);
    // Expiry after 30 days.
    h.clock.advance(31 * 24 * 3600 * 1000);
    assert.equal((await api(server, 'GET', '/v1/me', { token: admin.token })).status, 401);
    // Tokens never reach the logs.
    assert.ok(!h.log.lines.join('\n').includes(admin.token));
  });
});

test('device flow (RFC 8628) with the dev identity: pending, slow_down, approval, single use, expiry, denial', async () => {
  await withServer(async (h, server) => {
    const start = await api(server, 'POST', '/v1/auth/device');
    assert.equal(start.status, 200);
    const { deviceCode, userCode, verificationUri, verificationUriComplete, interval, expiresIn } = start.json;
    assert.match(userCode, /^[BCDFGHJKLMNPQRSTVWXZ]{4}-[BCDFGHJKLMNPQRSTVWXZ]{4}$/);
    assert.equal(verificationUri, 'http://cloud.test/auth/verify');
    assert.equal(verificationUriComplete, `http://cloud.test/auth/verify?user_code=${userCode}`);
    assert.equal(interval, 5);
    assert.equal(expiresIn, 600);
    const poll = () => api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode } });
    assert.equal((await poll()).json.error.code, 'authorization_pending');
    assert.equal((await poll()).json.error.code, 'slow_down');
    h.clock.advance(11_000);
    assert.equal((await poll()).json.error.code, 'authorization_pending');
    // The verification page, then approval as a dev user.
    const page = await api(server, 'GET', `/auth/verify?user_code=${userCode}`);
    assert.match(page.text, /Development user/);
    assert.match((await api(server, 'GET', `/auth/verify/start?user_code=${userCode}`)).text, /Enter a development user name/);
    assert.match((await api(server, 'GET', '/auth/verify/start?user_code=BBBB-BBBB&dev_user=x')).text, /unknown or has expired/);
    const approved = await api(server, 'GET', `/auth/verify/start?user_code=${userCode.toLowerCase().replace('-', ' ')}&dev_user=alice`);
    assert.match(approved.text, /Signed in/);
    h.clock.advance(11_000);
    const token = await poll();
    assert.equal(token.status, 200);
    assert.match(token.json.sessionToken, /^scs_/);
    assert.equal((await api(server, 'GET', '/v1/me', { token: token.json.sessionToken })).json.displayName, 'alice');
    assert.equal((await poll()).json.error.code, 'invalid_grant');
    assert.equal((await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: 'scd_nope' } })).json.error.code, 'invalid_grant');
    assert.equal((await api(server, 'POST', '/v1/auth/device/token', { json: {} })).json.error.code, 'InvalidRequest');
    // Expiry.
    const second = (await api(server, 'POST', '/v1/auth/device')).json;
    h.clock.advance(601_000);
    assert.equal((await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: second.deviceCode } })).json.error.code, 'expired_token');
    // Denial.
    const third = (await api(server, 'POST', '/v1/auth/device')).json;
    await h.ctx.db.query("UPDATE device_codes SET status = 'denied' WHERE user_code = $1", [third.userCode]);
    assert.equal((await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: third.deviceCode } })).json.error.code, 'access_denied');
    // In dev mode the OIDC callback does not exist.
    assert.equal((await api(server, 'GET', '/auth/callback?state=x&code=y')).status, 404);
  });
});

test('body limits, content types and rate limits', async () => {
  await withServer(async (h, server) => {
    const user = await makeUser(h.ctx, 'bob');
    // JSON route with the wrong content type, invalid JSON, a non-object.
    assert.equal((await api(server, 'POST', '/v1/orders', { token: user.token, body: '{}', headers: { 'Content-Type': 'text/plain' } })).json.error.code, 'UnsupportedMediaType');
    assert.equal((await api(server, 'POST', '/v1/orders', { token: user.token, body: '{', headers: { 'Content-Type': 'application/json' } })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'POST', '/v1/orders', { token: user.token, json: [1] })).json.error.code, 'InvalidRequest');
    // Over the 64 KiB default limit.
    const big = await api(server, 'POST', '/v1/orders', { token: user.token, json: { pad: 'x'.repeat(70 * 1024) } });
    assert.equal(big.status, 413);
    assert.equal(big.json.error.code, 'TooLarge');
    // Publishing needs octet-stream.
    assert.equal((await api(server, 'POST', '/v1/listings', { token: user.token, json: {} })).json.error.code, 'UnsupportedMediaType');
    // Device sign-in starts are limited per IP (10 per hour).
    let limited = null;
    for (let index = 0; index < 12 && !limited; index++) {
      const response = await api(server, 'POST', '/v1/auth/device');
      if (response.status === 429) limited = response;
    }
    assert.ok(limited);
    assert.equal(limited.json.error.code, 'RateLimited');
    assert.ok(Number(limited.headers.get('retry-after')) >= 1);
  });
});

test('rate limiter refills, evicts and computes client IPs behind proxies', () => {
  let now = 0;
  const limiter = new RateLimiter(() => now, 4);
  const policy = { capacity: 2, refillPerSecond: 1 };
  assert.equal(limiter.take('p', policy, 'a').ok, true);
  assert.equal(limiter.take('p', policy, 'a').ok, true);
  const refused = limiter.take('p', policy, 'a');
  assert.equal(refused.ok, false);
  assert.equal(refused.retryAfterSeconds, 1);
  now += 1000;
  assert.equal(limiter.take('p', policy, 'a').ok, true);
  for (const key of ['b', 'c', 'd', 'e', 'f']) limiter.take('p', policy, key);
  assert.ok(limiter.size <= 4);
  now += 4_000_000;
  limiter.take('p', policy, 'g');
  assert.ok(limiter.size <= 4);
  const req = (xff: string | undefined) => ({ socket: { remoteAddress: '10.0.0.1' }, headers: { 'x-forwarded-for': xff } }) as never;
  assert.equal(clientIp(req('1.1.1.1, 2.2.2.2'), 0), '10.0.0.1');
  assert.equal(clientIp(req('1.1.1.1, 2.2.2.2'), 1), '2.2.2.2');
  assert.equal(clientIp(req('1.1.1.1, 2.2.2.2'), 2), '1.1.1.1');
  assert.equal(clientIp(req(undefined), 1), '10.0.0.1');
});

test('listings: publish, moderation, query, get, thumbnail, download entitlement checks, unpublish', async () => {
  await withServer(async (h, server) => {
    const author = await makeUser(h.ctx, 'author');
    const other = await makeUser(h.ctx, 'other');
    const admin = await makeUser(h.ctx, 'admin', ['admin']);
    const publish = (token: string, bytes: Buffer) => api(server, 'POST', '/v1/listings', { token, body: bytes, headers: { 'Content-Type': 'application/octet-stream' } });

    // Unauthenticated publish, corrupt package, real-money without onboarding, credits, unsupported currency, NotOriginal.
    const free = writePackage({ manifest: manifestJson({ title: 'Free Stage', keywords: ['festival'], author: 'Forged Name' }) });
    assert.equal((await api(server, 'POST', '/v1/listings', { body: free.bytes, headers: { 'Content-Type': 'application/octet-stream' } })).status, 401);
    assert.equal((await publish(author.token, Buffer.from('garbage-bytes-garbage'))).json.error.code, 'Corrupt');
    assert.equal((await publish(author.token, paidPackage().bytes)).json.error.code, 'SellerNotOnboarded');
    assert.equal((await publish(author.token, writePackage({ manifest: manifestJson({ price: { credits: 5 } }) }).bytes)).json.error.code, 'InvalidRequest');
    assert.equal((await publish(author.token, writePackage({ manifest: manifestJson({ price: { credits: 0, realMoney: { amount: 5, currency: 'EUR' } } }) }).bytes)).json.error.code, 'CurrencyNotSupported');
    const originScene = sceneJson([sceneItem()], { originListingId: '505EF34C-47A1-9708-76AD-7BA25769CB0E' });
    assert.equal((await publish(author.token, writePackage({ scene: originScene, manifest: manifestJson({}, originScene) }).bytes)).json.error.code, 'NotOriginal');
    const imported = sceneJson([sceneItem('/Temp/StageCraft/Imported/IMP_1.IMP_1')]);
    assert.equal((await publish(author.token, writePackage({ scene: imported, manifest: manifestJson({}, imported) }).bytes)).json.error.code, 'NotPortable');

    const created = await publish(author.token, free.bytes);
    assert.equal(created.status, 201);
    assert.equal(created.json.status, 'PendingReview');
    assert.equal(created.json.authorName, 'author'); // stamped from the account, not the manifest
    const freeId = created.json.listingId;
    assert.equal((await publish(author.token, free.bytes)).json.error.code, 'AlreadyExists');

    // Pending: invisible to others, visible to the author and admins.
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}`)).status, 404);
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}`, { token: other.token })).status, 404);
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}`, { token: author.token })).json.status, 'PendingReview');
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}/thumbnail`)).status, 404);
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}/package`, { token: other.token })).status, 404);
    assert.equal((await api(server, 'GET', '/v1/listings')).json.totalMatches, 0);
    assert.equal((await api(server, 'GET', '/v1/me/listings', { token: author.token })).json.listings.length, 1);

    // Moderation.
    const queue = await api(server, 'GET', '/v1/admin/moderation', { token: admin.token });
    assert.equal(queue.json.listings.length, 1);
    assert.equal((await api(server, 'POST', `/v1/admin/moderation/${freeId}`, { token: admin.token, json: { decision: 'reject' } })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'POST', `/v1/admin/moderation/${freeId}`, { token: admin.token, json: { decision: 'maybe' } })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'POST', `/v1/admin/moderation/${freeId}`, { token: admin.token, json: { decision: 'reject', reason: 'blurry' } })).json.status, 'Rejected');
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}`, { token: author.token })).json.reviewReason, 'blurry');
    assert.equal((await api(server, 'POST', `/v1/admin/moderation/${freeId}`, { token: admin.token, json: { decision: 'approve' } })).json.status, 'Approved');
    assert.equal((await api(server, 'POST', `/v1/admin/moderation/${freeId}`, { token: admin.token, json: { decision: 'approve' } })).json.error.code, 'IllegalTransition');
    assert.equal((await api(server, 'POST', '/v1/admin/moderation/00000000-0000-0000-0000-000000000001', { token: admin.token, json: { decision: 'approve' } })).status, 404);
    const audits = await h.ctx.db.query("SELECT action FROM audit_log WHERE target = $1 ORDER BY created_at", [`listing:${freeId}`]);
    assert.deepEqual(audits.map((row) => row.action), ['listing.reject', 'listing.approve']);

    // Public views.
    const listed = await api(server, 'GET', '/v1/listings?searchText=FESTIVAL');
    assert.equal(listed.json.totalMatches, 1);
    assert.equal(listed.json.listings[0].status, undefined);
    assert.deepEqual(listed.json.listings[0].price, { credits: 0, realMoneyMinorUnits: 0, realMoneyCurrency: '' });
    const thumb = await api(server, 'GET', `/v1/listings/${freeId.replace(/-/g, '').toLowerCase()}/thumbnail`);
    assert.equal(thumb.status, 200);
    assert.equal(thumb.headers.get('content-type'), 'image/png');
    assert.equal((await api(server, 'GET', '/v1/listings/not-a-guid')).status, 404);

    // Free download: any signed-in user; acquire counts once per account.
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}/package`)).status, 401);
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}/package?purpose=steal`, { token: other.token })).json.error.code, 'InvalidRequest');
    const download = await api(server, 'GET', `/v1/listings/${freeId}/package?purpose=acquire`, { token: other.token });
    assert.equal(download.status, 302);
    const location = download.headers.get('location')!;
    assert.match(location, /\/v1\/blobs\/packages\//);
    await api(server, 'GET', `/v1/listings/${freeId}/package?purpose=acquire`, { token: other.token });
    await api(server, 'GET', `/v1/listings/${freeId}/package?purpose=preview`, { token: author.token });
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}`)).json.downloads, 1);
    // The signed blob URL works, a tampered one does not, an expired one does not.
    const path = location.replace('http://cloud.test', '');
    const blob = await fetch(`${server.url}${path}`);
    assert.equal(blob.status, 200);
    assert.ok(Buffer.from(await blob.arrayBuffer()).equals(free.bytes));
    assert.equal((await api(server, 'GET', path.replace(/sig=(.)/, (_match, first: string) => `sig=${first === '0' ? '1' : '0'}`))).status, 403);
    assert.equal((await api(server, 'GET', path.replace(/expires=\d+/, 'expires=1'))).status, 403);
    h.clock.advance(301_000);
    assert.equal((await api(server, 'GET', path)).status, 403);

    // Paid listing: preview and acquire need ownership; the author always may.
    await enableSeller(h.ctx, author.accountId);
    const paid = await publish(author.token, paidPackage(1500, { title: 'Paid Stage' }).bytes);
    const paidId = paid.json.listingId;
    await api(server, 'POST', `/v1/admin/moderation/${paidId}`, { token: admin.token, json: { decision: 'approve' } });
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package?purpose=preview`, { token: other.token })).json.error.code, 'NotOwned');
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package?purpose=acquire`, { token: other.token })).json.error.code, 'NotOwned');
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package?purpose=preview`, { token: author.token })).status, 302);
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package`, { token: admin.token })).status, 302);
    // Ownership as a settled order would leave it (the order flow itself is covered by the orders and e2e tests).
    await h.ctx.db.query(
      `INSERT INTO orders (id, buyer_id, listing_id, seller_id, idempotency_key, state, amount_minor, currency, share_bps, seller_amount, platform_amount, created_at, updated_at)
       VALUES ('00000000-0000-4000-8000-0000000000aa', $1, $2, $3, 'key-owned-001', 'Paid', 1500, 'USD', 8500, 1275, 225, 1, 1)`,
      [other.accountId, paidId, author.accountId],
    );
    await h.ctx.db.query("INSERT INTO entitlements (account_id, listing_id, order_id, granted_at) VALUES ($1, $2, '00000000-0000-4000-8000-0000000000aa', 1)", [other.accountId, paidId]);
    // redirect=false: 200 with the same JSON and no Location header (the client fetches url without its bearer token).
    const noRedirect = await api(server, 'GET', `/v1/listings/${paidId}/package?purpose=acquire&redirect=false`, { token: other.token });
    assert.equal(noRedirect.status, 200);
    assert.equal(noRedirect.headers.get('location'), null);
    assert.match(noRedirect.json.url, /^http:\/\/cloud\.test\/v1\/blobs\/packages\//);
    assert.ok(Date.parse(noRedirect.json.expiresAt) > h.clock.now());
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package?redirect=maybe`, { token: other.token })).json.error.code, 'InvalidRequest');

    // Filters and sorts.
    assert.equal((await api(server, 'GET', '/v1/listings?priceFilter=Paid')).json.listings[0].title, 'Paid Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?priceFilter=free')).json.listings[0].title, 'Free Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?maxCredits=10')).json.totalMatches, 1);
    assert.equal((await api(server, 'GET', '/v1/listings?sort=PriceHighToLow')).json.listings[0].title, 'Paid Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?sort=PriceLowToHigh')).json.listings[0].title, 'Free Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?sort=Title&limit=1&offset=1')).json.listings[0].title, 'Paid Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?sort=Newest')).json.listings[0].title, 'Paid Stage');
    assert.equal((await api(server, 'GET', '/v1/listings?kinds=LightingRig,CueList')).json.totalMatches, 0);
    assert.equal((await api(server, 'GET', '/v1/listings?category=StageCraft.Marketplace.Category.Concert')).json.totalMatches, 2);
    assert.equal((await api(server, 'GET', '/v1/listings?sort=Cheapest')).json.error.code, 'InvalidRequest');

    // Unpublish: only the author; owners keep access; free listings disappear.
    assert.equal((await api(server, 'DELETE', `/v1/listings/${paidId}`, { token: other.token })).json.error.code, 'NotYours');
    assert.equal((await api(server, 'DELETE', `/v1/listings/${paidId}`, { token: author.token })).json.status, 'Unpublished');
    assert.equal((await api(server, 'DELETE', `/v1/listings/${paidId}`, { token: author.token })).json.status, 'Unpublished');
    assert.equal((await api(server, 'GET', `/v1/listings/${paidId}/package`, { token: other.token })).status, 302);
    await api(server, 'DELETE', `/v1/listings/${freeId}`, { token: author.token });
    assert.equal((await api(server, 'GET', `/v1/listings/${freeId}/package`, { token: other.token })).status, 404);
    assert.equal((await api(server, 'DELETE', `/v1/listings/${freeId}`, { token: other.token })).status, 404);
    assert.equal((await api(server, 'GET', '/v1/listings')).json.totalMatches, 0);
  });
});

test('orders: validation, idempotent repeat, reused open checkout, owned and free listings, provider failure, long poll', async () => {
  await withServer(async (h, server) => {
    const seller = await makeUser(h.ctx, 'seller');
    const buyer = await makeUser(h.ctx, 'buyer');
    const admin = await makeUser(h.ctx, 'admin', ['admin']);
    await enableSeller(h.ctx, seller.accountId);
    const publish = async (bytes: Buffer) => {
      const listing = (await api(server, 'POST', '/v1/listings', { token: seller.token, body: bytes, headers: { 'Content-Type': 'application/octet-stream' } })).json;
      await api(server, 'POST', `/v1/admin/moderation/${listing.listingId}`, { token: admin.token, json: { decision: 'approve' } });
      return listing.listingId as string;
    };
    const paidId = await publish(paidPackage(1999).bytes);
    const freeId = await publish(writePackage().bytes);
    const order = (json: unknown, token = buyer.token) => api(server, 'POST', '/v1/orders', { token, json });

    assert.equal((await order({ listingId: paidId })).json.error.code, 'InvalidRequest');
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'short' })).json.error.code, 'InvalidRequest');
    assert.equal((await order({ listingId: freeId, idempotencyKey: 'key-free-0001' })).json.error.code, 'NotForSale');
    assert.equal((await order({ listingId: '00000000-0000-0000-0000-00000000abcd', idempotencyKey: 'key-none-0001' })).json.error.code, 'NotFound');
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'key-self-0001' }, seller.token)).json.error.code, 'AlreadyOwned');

    // The provider fails: nothing charged, the order is Failed, the caller sees ProviderError.
    h.provider.failNext.checkout = true;
    const failed = await order({ listingId: paidId, idempotencyKey: 'key-fail-0001' });
    assert.equal(failed.json.error.code, 'ProviderError');
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'key-fail-0001' })).json.state, 'Failed');

    const first = await order({ listingId: paidId, idempotencyKey: 'key-good-0001' });
    assert.equal(first.status, 200);
    assert.equal(first.json.state, 'CheckoutOpen');
    assert.equal(first.json.amountMinor, 1999);
    assert.match(first.json.checkoutUrl, /\/dev\/fake-checkout\/cs_fake_/);
    assert.ok(first.json.checkoutExpiresAt);
    // Same key: same order and URL. Another key while a checkout is open: the open one is returned.
    assert.deepEqual((await order({ listingId: paidId, idempotencyKey: 'key-good-0001' })).json, first.json);
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'key-good-0002' })).json.orderId, first.json.orderId);
    assert.equal((await order({ listingId: freeId, idempotencyKey: 'key-good-0001' })).json.error.code, 'IdempotencyConflict');
    assert.equal(h.provider.checkouts.filter((checkout) => checkout.orderId === first.json.orderId).length, 1);
    // The checkout's own lifetime passed without an expiry webhook: refuse a second checkout for now.
    h.clock.advance(31 * 60_000);
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'key-good-0003' })).json.error.code, 'PurchasePending');
    h.clock.advance(-31 * 60_000);

    // Order reads: owner only; long poll answers when the state changes.
    assert.equal((await api(server, 'GET', `/v1/orders/${first.json.orderId}`, { token: seller.token })).status, 404);
    assert.equal((await api(server, 'GET', '/v1/orders/abc', { token: buyer.token })).status, 404);
    assert.equal((await api(server, 'GET', `/v1/orders/${first.json.orderId}?waitSeconds=26`, { token: buyer.token })).json.error.code, 'InvalidRequest');
    assert.equal((await api(server, 'GET', `/v1/orders/${first.json.orderId}?waitSeconds=0`, { token: buyer.token })).json.state, 'CheckoutOpen');
    const realNow = Date.now();
    const poll = api(server, 'GET', `/v1/orders/${first.json.orderId}?waitSeconds=10`, { token: buyer.token });
    await new Promise((resolve) => setTimeout(resolve, 100));
    const pay = await fetch(`${server.url}/dev/fake-checkout/${first.json.checkoutUrl.split('/').pop()}/pay`, { method: 'POST' });
    assert.match(await pay.text(), /processed/);
    const polled = await poll;
    assert.equal(polled.json.state, 'Paid');
    assert.ok(Date.now() - realNow < 5000);
    assert.equal(polled.json.checkoutUrl, null);
    assert.equal((await order({ listingId: paidId, idempotencyKey: 'key-good-0004' })).json.error.code, 'AlreadyOwned');
    assert.equal((await api(server, 'GET', '/v1/me/entitlements', { token: buyer.token })).json.entitlements[0].listingId, paidId);
    assert.equal((await api(server, 'GET', '/v1/me/orders', { token: buyer.token })).json.orders.length, 2);
    assert.match((await api(server, 'GET', `/dev/fake-checkout/${first.json.checkoutUrl.split('/').pop()}`)).text, /Paid/);
    assert.equal((await api(server, 'GET', '/dev/fake-checkout/cs_fake_nope')).status, 404);
    const sales = await api(server, 'GET', '/v1/me/seller/sales', { token: seller.token });
    assert.equal(sales.json.sales[0].sellerAmountMinor, 1699);
    const balances = await api(server, 'GET', '/v1/me/seller/balance', { token: seller.token });
    assert.deepEqual(balances.json.balances, [{ currency: 'USD', pending: 1699, available: 0, inTransit: 0, paidOut: 0 }]);
  });
});

test('seller onboarding: Stripe link for Connect countries, manual for others, admin enables manual payouts', async () => {
  await withServer(async (h, server) => {
    const us = await makeUser(h.ctx, 'us');
    const am = await makeUser(h.ctx, 'am');
    const admin = await makeUser(h.ctx, 'admin', ['admin']);
    assert.equal((await api(server, 'POST', '/v1/me/seller/onboarding', { token: us.token, json: { country: 'usa' } })).json.error.code, 'InvalidRequest');
    h.provider.failNext.account = true;
    assert.equal((await api(server, 'POST', '/v1/me/seller/onboarding', { token: us.token, json: { country: 'us' } })).json.error.code, 'ProviderError');
    const link = await api(server, 'POST', '/v1/me/seller/onboarding', { token: us.token, json: { country: 'us' } });
    assert.equal(link.json.payoutMethod, 'stripe');
    assert.match(link.json.onboardingUrl, /fake-onboarding\/acct_fake_/);
    // Once onboarded (account.updated), payouts are enabled. The country is fixed by the first call.
    await api(server, 'GET', new URL(link.json.onboardingUrl).pathname);
    const again = await api(server, 'POST', '/v1/me/seller/onboarding', { token: us.token, json: { country: 'DE' } });
    assert.equal(again.json.country, 'US');
    assert.equal(again.json.status, 'enabled');
    assert.equal(h.provider.accounts.length, 1);

    const manual = await api(server, 'POST', '/v1/me/seller/onboarding', { token: am.token, json: { country: 'AM' } });
    assert.equal(manual.json.payoutMethod, 'manual');
    assert.equal(manual.json.onboardingUrl, null);
    assert.equal(manual.json.payoutsEnabled, false);
    const enable = (id: string, enabled: unknown) => api(server, 'POST', `/v1/admin/sellers/${id}/manual-payouts`, { token: admin.token, json: { enabled } });
    assert.equal((await enable(am.accountId, 'yes')).json.error.code, 'InvalidRequest');
    assert.equal((await enable(us.accountId, true)).json.error.code, 'IllegalTransition');
    assert.equal((await enable(admin.accountId, true)).status, 404);
    assert.equal((await enable(am.accountId, true)).json.payoutsEnabled, true);
    const audit = await h.ctx.db.query("SELECT action, before_json, after_json FROM audit_log WHERE action = 'seller.manual_payouts'");
    assert.equal(audit.length, 1);
    assert.match(String(audit[0]!.after_json), /"payoutsEnabled":true/);
    const account = await api(server, 'GET', `/v1/admin/accounts/${am.accountId}`, { token: admin.token });
    assert.equal(account.json.email, 'a***@example.com');
  });
});

test('support tickets: redaction, limits, PNG check, idempotent resend, anonymous rate limit, own list, admin status with audit', async () => {
  await withServer(async (h, server) => {
    const user = await makeUser(h.ctx, 'reporter');
    const admin = await makeUser(h.ctx, 'admin', ['admin']);
    const base = {
      category: 'Bug',
      title: 'Crash on save',
      description: 'It crashed while saving; key sk-ant-secret123 in C:\\Users\\Gevor\\x',
      steps: 'Open C:\\Users\\Gevor\\scene, mail me at gevor@example.com',
      expected: 'saved',
      actual: 'crash',
      appVersion: '1.0',
      os: 'Windows 11',
      gpu: 'RTX',
      sceneStats: { items: 10 },
      logTail: 'Bearer abc.def token whsec_123',
    };
    const created = await api(server, 'POST', '/v1/support/tickets', { token: user.token, json: { ...base, screenshotPngBase64: PNG_1x1.toString('base64') } });
    assert.equal(created.status, 201);
    const ticketId = created.json.ticketId;
    const row = (await h.ctx.db.query('SELECT * FROM support_tickets WHERE id = $1', [ticketId]))[0]!;
    assert.equal(row.category, 'bug');
    for (const leaked of ['Gevor', 'gevor@example.com', 'Bearer abc', 'whsec_123', 'sk-ant-secret']) {
      assert.ok(!JSON.stringify(row, (_key, value) => (typeof value === 'bigint' ? String(value) : value)).includes(leaked), leaked);
    }
    assert.equal(row.scene_stats, '{"items":10}');

    // Validation.
    const bad = (json: Record<string, unknown>) => api(server, 'POST', '/v1/support/tickets', { token: user.token, json: { ...base, ...json } });
    assert.equal((await bad({ category: 'no spaces' })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ title: '' })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ description: undefined })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ description: 'd'.repeat(8001) })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ steps: 5 })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ sceneStats: 'x'.repeat(4001) })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ logTail: 'x'.repeat(256 * 1024 + 1) })).json.error.code, 'TooLarge');
    assert.equal((await bad({ screenshotPngBase64: '***' })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ screenshotPngBase64: Buffer.from('GIF89a-not-png').toString('base64') })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ screenshotPngBase64: Buffer.alloc(4 * 1024 * 1024 + 3).toString('base64') })).json.error.code, 'TooLarge');
    assert.equal((await bad({ contactEmail: 'not-an-email' })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ contactEmail: `${'a'.repeat(250)}@x.io` })).json.error.code, 'InvalidRequest');
    assert.equal((await bad({ clientReportId: 'nope' })).json.error.code, 'InvalidRequest');

    // clientReportId: a resend returns the same ticket (200) instead of creating another.
    const reportId = '3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b';
    const firstSend = await bad({ clientReportId: reportId });
    assert.equal(firstSend.status, 201);
    const resend = await bad({ clientReportId: reportId.toUpperCase(), title: 'changed' });
    assert.equal(resend.status, 200);
    assert.equal(resend.json.ticketId, firstSend.json.ticketId);
    assert.equal(resend.json.created, false);
    // The same report id from another account is a different ticket.
    const otherUser = await makeUser(h.ctx, 'other-reporter');
    const fromOther = await api(server, 'POST', '/v1/support/tickets', { token: otherUser.token, json: { ...base, clientReportId: reportId } });
    assert.equal(fromOther.status, 201);
    assert.notEqual(fromOther.json.ticketId, firstSend.json.ticketId);

    // Anonymous: allowed, contactEmail stored as given (not redacted), deduplicated per IP, rate-limited per IP.
    const anonymous = await api(server, 'POST', '/v1/support/tickets', { json: { ...base, contactEmail: 'help.me@example.org', clientReportId: reportId } });
    assert.equal(anonymous.status, 201);
    const anonRow = (await h.ctx.db.query('SELECT account_id, contact_email FROM support_tickets WHERE id = $1', [anonymous.json.ticketId]))[0]!;
    assert.equal(anonRow.account_id, null);
    assert.equal(anonRow.contact_email, 'help.me@example.org');
    assert.ok(!h.log.lines.join('\n').includes('help.me@example.org'));
    assert.equal((await api(server, 'POST', '/v1/support/tickets', { json: { ...base, clientReportId: reportId } })).json.ticketId, anonymous.json.ticketId);
    let limited = false;
    for (let index = 0; index < 6 && !limited; index++) {
      limited = (await api(server, 'POST', '/v1/support/tickets', { json: base })).status === 429;
    }
    assert.ok(limited);

    const mine = await api(server, 'GET', '/v1/me/support', { token: user.token });
    assert.equal(mine.json.tickets.length, 2);
    assert.equal(mine.json.tickets[0].logTail, undefined);

    const list = await api(server, 'GET', '/v1/admin/support?status=open', { token: admin.token });
    assert.ok(list.json.tickets.length >= 3);
    assert.equal((await api(server, 'GET', '/v1/admin/support?status=weird', { token: admin.token })).json.error.code, 'InvalidRequest');
    assert.ok((await api(server, 'GET', '/v1/admin/support', { token: admin.token })).json.tickets.length >= 3);
    const screenshot = await api(server, 'GET', `/v1/admin/support/${ticketId}/screenshot`, { token: admin.token });
    assert.equal(screenshot.status, 200);
    assert.equal((await api(server, 'GET', `/v1/admin/support/${firstSend.json.ticketId}/screenshot`, { token: admin.token })).status, 404);
    const update = (id: string, json: unknown) => api(server, 'POST', `/v1/admin/support/${id}`, { token: admin.token, json });
    assert.equal((await update(ticketId, { status: 'done' })).json.error.code, 'InvalidRequest');
    assert.equal((await update(ticketId, { status: 'resolved', note: 5 })).json.error.code, 'InvalidRequest');
    assert.equal((await update('00000000-0000-4000-8000-000000000000', { status: 'resolved' })).status, 404);
    assert.equal((await update('xyz', { status: 'resolved' })).status, 404);
    const resolved = await update(ticketId, { status: 'resolved', note: 'fixed in 1.1' });
    assert.equal(resolved.json.status, 'resolved');
    assert.equal(resolved.json.adminNote, 'fixed in 1.1');
    assert.equal((await update(ticketId, { status: 'closed' })).json.adminNote, 'fixed in 1.1');
    const audit = await h.ctx.db.query("SELECT COUNT(*) AS n FROM audit_log WHERE action = 'support.update'");
    assert.equal(Number(audit[0]!.n), 2);
  });
});
