// Routes of API version 1 (ADR 0032 §4.1) wired to the services. Every handler validates its input and calls one service function.
import { randomBytes, randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import type { AppContext } from './context.ts';
import { Router, type Reply, type RequestContext } from './http/router.ts';
import { authenticate, getAccount, getRoles, revokeSession, sha256Hex, upsertAccount } from './auth/sessions.ts';
import { decideDevice, findPendingDevice, normalizeUserCode, pollDeviceToken, startDeviceAuthorization } from './auth/device.ts';
import { pkcePair, OidcError } from './auth/oidc.ts';
import { LocalBlobStore } from './blob/localBlobStore.ts';
import {
  authorizeDownload,
  getThumbnail,
  getVisibleListing,
  listingToJson,
  listOwnListings,
  parseListingId,
  publishListing,
  queryListings,
  unpublishListing,
} from './listings.ts';
import { parseListingQuery } from './query.ts';
import { createOrder, listBuyerOrders, listEntitlements, waitForOrderChange } from './commerce.ts';
import { getOrder, orderToJson } from './orders.ts';
import { handleStripeWebhook } from './webhooks.ts';
import { getSeller, sellerToJson, setManualPayouts, startOnboarding } from './sellers.ts';
import { dueBalances, listPayouts, markPayoutPaid, payoutToJson, runPayouts, sellerBalances, sellerSales } from './payouts.ts';
import { createTicket, listTickets, parseTicket, SupportBodyLimit, ticketToJson, updateTicket } from './support.ts';
import { moderate, moderationQueue, requestRefund, searchOrders, summary } from './admin.ts';
import { escapeHtml, page } from './pages.ts';
import { signStripePayload } from './payments/webhookSignature.ts';
import { MaxPackageBytes } from './package/stagePackage.ts';
import { maskEmail } from './redaction.ts';
import { num, str } from './db/driver.ts';

const ok = (json: unknown, status = 200): Reply => ({ status, json });
const html = (markup: string, status = 200): Reply => ({ status, body: markup, contentType: 'text/html; charset=utf-8' });
const redirect = (location: string): Reply => ({ status: 302, headers: { Location: location }, json: { url: location } });

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
function uuidParam(context: RequestContext, name: string, what: string): string {
  const value = context.params[name]!;
  if (!UUID.test(value)) {
    throw new ApiError('NotFound', `no such ${what}`);
  }
  return value;
}

const OIDC_STATE_TTL_MS = 10 * 60_000;

export function createRouter(ctx: AppContext): Router {
  const router = new Router({
    log: ctx.log,
    limiter: ctx.limiter,
    authenticate: (token) => authenticate(ctx.db, token, ctx.clock.now()),
    trustProxyHops: ctx.config.trustProxyHops,
  });

  // Probes
  router.add('GET', '/healthz', async () => ok({ status: 'ok' }), { skipIpLimit: true });
  router.add(
    'GET',
    '/readyz',
    async () => {
      try {
        await ctx.db.ping();
        await ctx.blob.check();
        return ok({ status: 'ready' });
      } catch (error) {
        ctx.log.warn('readiness check failed', { error });
        return { status: 503, json: { error: { code: 'BackendUnavailable', message: 'not ready' } } };
      }
    },
    { skipIpLimit: true },
  );

  // Sign-in (§5)
  router.add(
    'POST',
    '/v1/auth/device',
    async () => ok(await startDeviceAuthorization(ctx.db, ctx.config.publicBaseUrl, ctx.clock.now())),
    { limits: [{ policy: 'deviceStart', by: 'ip' }] },
  );
  router.add('POST', '/v1/auth/device/token', async (context) => {
    const body = await context.json();
    if (typeof body.deviceCode !== 'string' || body.deviceCode.length > 128) {
      throw new ApiError('InvalidRequest', 'deviceCode is required');
    }
    const result = await pollDeviceToken(ctx.db, body.deviceCode, ctx.clock.now());
    return ok({ sessionToken: result.sessionToken, expiresAt: new Date(result.expiresAt).toISOString(), accountId: result.accountId });
  });
  router.add(
    'POST',
    '/v1/auth/logout',
    async (context) => {
      await revokeSession(ctx.db, context.requireUser().sessionHash, ctx.clock.now());
      return { status: 204 };
    },
    { auth: 'user' },
  );

  const verifyForm = (userCode: string, message = ''): Reply => {
    const dev = ctx.config.auth.mode === 'dev';
    return html(
      page(
        'Sign in to StageCraft',
        `${message ? `<p><strong>${escapeHtml(message)}</strong></p>` : ''}
<p>Check that this code matches the one StageCraft shows, then continue.</p>
<form method="get" action="/auth/verify/start">
<label>Code<br><input name="user_code" value="${escapeHtml(userCode)}" autocomplete="off" required></label><br>
${dev ? '<label>Development user (AUTH_MODE=dev only)<br><input name="dev_user" pattern="[A-Za-z0-9 ._-]{1,64}" required></label><br>' : ''}
<button type="submit">Continue</button></form>`,
      ),
    );
  };
  router.add('GET', '/auth/verify', async (context) => verifyForm(context.query.get('user_code') ?? ''), {
    limits: [{ policy: 'authBrowser', by: 'ip' }],
  });
  router.add(
    'GET',
    '/auth/verify/start',
    async (context) => {
      const userCode = normalizeUserCode(context.query.get('user_code') ?? '');
      const now = ctx.clock.now();
      const device = userCode ? await findPendingDevice(ctx.db, userCode, now) : null;
      if (!device) {
        return verifyForm(context.query.get('user_code') ?? '', 'That code is unknown or has expired. Start the sign-in again in StageCraft.');
      }
      if (ctx.config.auth.mode === 'dev') {
        const devUser = (context.query.get('dev_user') ?? '').trim();
        if (!/^[A-Za-z0-9 ._-]{1,64}$/.test(devUser)) {
          return verifyForm(userCode!, 'Enter a development user name.');
        }
        await ctx.db.withTransaction(async (tx) => {
          const account = await upsertAccount(tx, { issuer: 'dev', subject: devUser, email: null, displayName: devUser }, now);
          await decideDevice(tx, device.id, { accountId: account.id }, now);
        });
        return html(page('Signed in', '<p>You can return to StageCraft.</p>'));
      }
      const state = randomBytes(32).toString('base64url');
      const nonce = randomBytes(16).toString('base64url');
      const pkce = pkcePair();
      await ctx.db.query(
        'INSERT INTO oidc_states (state_hash, device_code_id, nonce, code_verifier, created_at, expires_at) VALUES ($1, $2, $3, $4, $5, $6)',
        [sha256Hex(state), device.id, nonce, pkce.verifier, now, now + OIDC_STATE_TTL_MS],
      );
      return redirect(await ctx.oidc!.authorizationUrl({ state, nonce, codeChallenge: pkce.challenge }));
    },
    { limits: [{ policy: 'authBrowser', by: 'ip' }] },
  );
  router.add(
    'GET',
    '/auth/callback',
    async (context) => {
      if (ctx.config.auth.mode !== 'oidc' || !ctx.oidc) {
        throw new ApiError('NotFound', 'no such endpoint');
      }
      const state = context.query.get('state') ?? '';
      const now = ctx.clock.now();
      const rows = state ? await ctx.db.query('UPDATE oidc_states SET used_at = $1 WHERE state_hash = $2 AND used_at IS NULL AND expires_at > $1 RETURNING *', [now, sha256Hex(state)]) : [];
      const record = rows[0];
      if (!record) {
        return html(page('Sign-in failed', '<p>This sign-in link has expired or was already used. Start again in StageCraft.</p>'), 400);
      }
      const deviceId = str(record.device_code_id);
      const code = context.query.get('code');
      if (context.query.get('error') || !code) {
        await decideDevice(ctx.db, deviceId, 'denied', now);
        return html(page('Sign-in cancelled', '<p>Nothing was signed in. You can close this page.</p>'), 400);
      }
      try {
        const identity = await ctx.oidc.completeSignIn(code, str(record.code_verifier), str(record.nonce));
        await ctx.db.withTransaction(async (tx) => {
          const displayName = identity.name ?? identity.email?.split('@')[0] ?? 'StageCraft user';
          const account = await upsertAccount(tx, { issuer: identity.issuer, subject: identity.subject, email: identity.email, displayName }, now);
          await decideDevice(tx, deviceId, { accountId: account.id }, now);
        });
      } catch (error) {
        if (error instanceof OidcError) {
          ctx.log.warn('OIDC sign-in refused', { reason: error.message });
          return html(page('Sign-in failed', '<p>The identity provider\'s answer could not be verified. Start again in StageCraft.</p>'), 400);
        }
        throw error;
      }
      return html(page('Signed in', '<p>You can return to StageCraft.</p>'));
    },
    { limits: [{ policy: 'authBrowser', by: 'ip' }] },
  );

  router.add(
    'GET',
    '/v1/me',
    async (context) => {
      const principal = context.requireUser();
      const account = await getAccount(ctx.db, principal.accountId);
      return ok({
        accountId: principal.accountId,
        displayName: account?.displayName ?? '',
        email: account?.email ?? null,
        roles: await getRoles(ctx.db, principal.accountId),
        seller: sellerToJson(await getSeller(ctx.db, principal.accountId)),
      });
    },
    { auth: 'user' },
  );

  // Listings
  router.add('GET', '/v1/listings', async (context) => {
    const result = await queryListings(ctx, parseListingQuery(context.query));
    return ok({ listings: result.listings.map((listing) => listingToJson(listing)), totalMatches: result.totalMatches });
  });
  router.add(
    'GET',
    '/v1/listings/:id',
    async (context) => {
      const listing = await getVisibleListing(ctx, parseListingId(context.params.id!), context.principal);
      const reviewer = context.principal !== null && (context.principal.accountId === listing.authorId || context.principal.roles.includes('admin'));
      return ok(listingToJson(listing, reviewer));
    },
    { auth: 'optional' },
  );
  router.add(
    'GET',
    '/v1/listings/:id/package',
    async (context) => {
      const purpose = context.query.get('purpose') ?? 'acquire';
      if (purpose !== 'acquire' && purpose !== 'preview') {
        throw new ApiError('InvalidRequest', 'purpose must be acquire or preview');
      }
      const redirectParam = context.query.get('redirect') ?? 'true';
      if (redirectParam !== 'true' && redirectParam !== 'false') {
        throw new ApiError('InvalidRequest', 'redirect must be true or false');
      }
      const download = await authorizeDownload(ctx, context.requireUser(), parseListingId(context.params.id!), purpose);
      const json = { url: download.url, expiresAt: new Date(download.expiresAt).toISOString() };
      // redirect=false: the client fetches url itself without its Authorization header (a followed 302 could forward the bearer
      // token to the storage host).
      return redirectParam === 'false' ? ok(json) : { status: 302, headers: { Location: download.url }, json };
    },
    { auth: 'user' },
  );
  router.add(
    'GET',
    '/v1/listings/:id/thumbnail',
    async (context) => {
      const png = await getThumbnail(ctx, parseListingId(context.params.id!), context.principal);
      return { status: 200, body: png, contentType: 'image/png', headers: { 'Cache-Control': 'public, max-age=300', 'Cross-Origin-Resource-Policy': 'cross-origin' } };
    },
    { auth: 'optional' },
  );
  router.add(
    'POST',
    '/v1/listings',
    async (context) => {
      if (!/^application\/octet-stream\b/i.test(context.req.headers['content-type'] ?? '')) {
        throw new ApiError('UnsupportedMediaType', 'send the .stage package as application/octet-stream');
      }
      const listing = await publishListing(ctx, context.requireUser(), await context.body());
      return ok(listingToJson(listing, true), 201);
    },
    { auth: 'user', bodyLimit: MaxPackageBytes, limits: [{ policy: 'publish', by: 'account' }] },
  );
  router.add(
    'DELETE',
    '/v1/listings/:id',
    async (context) => ok(listingToJson(await unpublishListing(ctx, context.requireUser(), parseListingId(context.params.id!)), true)),
    { auth: 'user' },
  );
  router.add(
    'GET',
    '/v1/me/listings',
    async (context) => ok({ listings: (await listOwnListings(ctx, context.requireUser().accountId)).map((listing) => listingToJson(listing, true)) }),
    { auth: 'user' },
  );

  // Orders
  router.add(
    'POST',
    '/v1/orders',
    async (context) => {
      const body = await context.json();
      if (typeof body.listingId !== 'string' || typeof body.idempotencyKey !== 'string') {
        throw new ApiError('InvalidRequest', 'listingId and idempotencyKey are required');
      }
      const order = await createOrder(ctx, context.requireUser().accountId, parseListingId(body.listingId), body.idempotencyKey);
      return ok(orderToJson(order));
    },
    { auth: 'user', limits: [{ policy: 'orderCreate', by: 'account' }] },
  );
  router.add(
    'GET',
    '/v1/orders/:id',
    async (context) => {
      const raw = context.query.get('waitSeconds') ?? '0';
      if (!/^[0-9]{1,2}$/.test(raw) || Number(raw) > 25) {
        throw new ApiError('InvalidRequest', 'waitSeconds must be 0..25');
      }
      const order = await waitForOrderChange(ctx, context.requireUser().accountId, context.params.id!, Number(raw));
      return ok(orderToJson(order));
    },
    { auth: 'user' },
  );
  router.add('GET', '/v1/me/orders', async (context) => ok({ orders: (await listBuyerOrders(ctx, context.requireUser().accountId)).map(orderToJson) }), { auth: 'user' });
  router.add(
    'GET',
    '/v1/me/entitlements',
    async (context) => ok({
      entitlements: (await listEntitlements(ctx, context.requireUser().accountId)).map((entry) => ({ ...entry, grantedAt: new Date(entry.grantedAt).toISOString() })),
    }),
    { auth: 'user' },
  );

  // Selling
  router.add(
    'POST',
    '/v1/me/seller/onboarding',
    async (context) => ok(await startOnboarding(ctx, context.requireUser().accountId, (await context.json()).country)),
    { auth: 'user', limits: [{ policy: 'onboarding', by: 'account' }] },
  );
  router.add('GET', '/v1/me/seller/balance', async (context) => ok({ balances: await sellerBalances(ctx.db, context.requireUser().accountId) }), { auth: 'user' });
  router.add('GET', '/v1/me/seller/sales', async (context) => ok({ sales: await sellerSales(ctx.db, context.requireUser().accountId) }), { auth: 'user' });

  // Webhooks (§3.5)
  router.add(
    'POST',
    '/v1/webhooks/stripe',
    async (context) => {
      const header = context.req.headers['stripe-signature'];
      const outcome = await handleStripeWebhook(ctx, await context.body(), Array.isArray(header) ? header.join(',') : header);
      return ok({ received: true, status: outcome.status });
    },
    { bodyLimit: 1024 * 1024, skipIpLimit: true },
  );

  // Support (§7)
  router.add(
    'POST',
    '/v1/support/tickets',
    async (context) => {
      const result = await createTicket(ctx, context.principal?.accountId ?? null, context.ip, parseTicket(await context.json()));
      return ok({ ticketId: result.ticketId, status: 'open', created: result.created }, result.created ? 201 : 200);
    },
    {
      auth: 'optional',
      bodyLimit: SupportBodyLimit,
      limits: [
        { policy: 'supportAnonymous', by: 'ip' },
        { policy: 'supportAccount', by: 'account' },
      ],
    },
  );
  router.add(
    'GET',
    '/v1/me/support',
    async (context) => ok({ tickets: (await listTickets(ctx, { accountId: context.requireUser().accountId })).map((row) => ticketToJson(row, false)) }),
    { auth: 'user' },
  );

  // Admin (§4.1)
  const admin = { auth: 'admin' as const };
  const adminMutation = { auth: 'admin' as const, limits: [{ policy: 'adminMutation' as const, by: 'account' as const }] };
  router.add('GET', '/v1/admin/summary', async (context) => ok(await summary(ctx, context.query.get('from'), context.query.get('to'))), admin);
  router.add('GET', '/v1/admin/orders', async (context) => ok({ orders: await searchOrders(ctx, context.query) }), admin);
  router.add(
    'POST',
    '/v1/admin/orders/:id/refund',
    async (context) => ok(await requestRefund(ctx, context.requireUser().accountId, uuidParam(context, 'id', 'order'), await context.json()), 202),
    adminMutation,
  );
  router.add(
    'GET',
    '/v1/admin/payouts',
    async (context) => ok({ due: await dueBalances(ctx.db), payouts: (await listPayouts(ctx.db, context.query.get('status'))).map(payoutToJson) }),
    admin,
  );
  router.add('POST', '/v1/admin/payouts/run', async (context) => ok({ payouts: (await runPayouts(ctx, context.requireUser().accountId)).map(payoutToJson) }), adminMutation);
  router.add(
    'POST',
    '/v1/admin/payouts/:id/mark-paid',
    async (context) => {
      const body = await context.json();
      const payout = await markPayoutPaid(ctx, context.requireUser().accountId, uuidParam(context, 'id', 'payout'), typeof body.reference === 'string' ? body.reference.trim() : '');
      return ok(payoutToJson(payout));
    },
    adminMutation,
  );
  router.add(
    'POST',
    '/v1/admin/sellers/:id/manual-payouts',
    async (context) => ok(sellerToJson(await setManualPayouts(ctx, context.requireUser().accountId, uuidParam(context, 'id', 'seller'), (await context.json()).enabled))),
    adminMutation,
  );
  router.add('GET', '/v1/admin/moderation', async () => ok({ listings: await moderationQueue(ctx) }), admin);
  router.add(
    'POST',
    '/v1/admin/moderation/:id',
    async (context) => ok(listingToJson(await moderate(ctx, context.requireUser().accountId, parseListingId(context.params.id!), await context.json()), true)),
    adminMutation,
  );
  router.add('GET', '/v1/admin/support', async (context) => ok({ tickets: (await listTickets(ctx, { status: context.query.get('status') })).map((row) => ticketToJson(row, true)) }), admin);
  router.add(
    'POST',
    '/v1/admin/support/:id',
    async (context) => ok(ticketToJson(await updateTicket(ctx, context.requireUser().accountId, uuidParam(context, 'id', 'ticket'), await context.json()), true)),
    adminMutation,
  );
  router.add(
    'GET',
    '/v1/admin/support/:id/screenshot',
    async (context) => {
      const rows = await ctx.db.query('SELECT screenshot_key FROM support_tickets WHERE id = $1', [uuidParam(context, 'id', 'ticket')]);
      const key = rows[0]?.screenshot_key;
      const png = typeof key === 'string' ? await ctx.blob.get(key) : null;
      if (!png) {
        throw new ApiError('NotFound', 'this ticket has no screenshot');
      }
      return { status: 200, body: png, contentType: 'image/png' };
    },
    admin,
  );
  router.add(
    'GET',
    '/v1/admin/webhooks',
    async (context) => {
      const status = context.query.get('status') ?? 'needs_attention';
      const rows = await ctx.db.query('SELECT id, type, status, detail, received_at FROM webhook_events WHERE status = $1 ORDER BY received_at DESC LIMIT 500', [status]);
      return ok({ events: rows.map((row) => ({ eventId: str(row.id), type: str(row.type), status: str(row.status), detail: row.detail, receivedAt: new Date(num(row.received_at)).toISOString() })) });
    },
    admin,
  );
  router.add(
    'GET',
    '/v1/admin/accounts/:id',
    async (context) => {
      const account = await getAccount(ctx.db, uuidParam(context, 'id', 'account'));
      if (!account) throw new ApiError('NotFound', 'no such account');
      return ok({ accountId: account.id, displayName: account.displayName, email: maskEmail(account.email), roles: await getRoles(ctx.db, account.id), seller: sellerToJson(await getSeller(ctx.db, account.id)) });
    },
    admin,
  );

  // Checkout return pages: display only; the order state comes from webhooks.
  router.add('GET', '/checkout/success', async () => html(page('Payment received', '<p>Return to StageCraft: your purchase appears there as soon as the payment is confirmed.</p>')));
  router.add('GET', '/checkout/cancel', async () => html(page('Checkout cancelled', '<p>Nothing was charged. You can close this page.</p>')));
  router.add('GET', '/seller/onboarding/done', async () => html(page('Payout setup', '<p>Return to StageCraft. Your payout status updates as soon as the payment provider confirms it.</p>')));
  router.add('GET', '/seller/onboarding/refresh', async () => html(page('Payout setup', '<p>The setup link expired. Start payout setup again in StageCraft.</p>')));

  if (ctx.blob instanceof LocalBlobStore) {
    const store = ctx.blob;
    router.add('GET', '/v1/blobs/*', async (context) => {
      const key = context.params.rest!;
      if (!store.verify(key, context.query.get('expires'), context.query.get('sig'))) {
        throw new ApiError('Forbidden', 'this download link is invalid or expired');
      }
      const bytes = await store.get(key);
      if (!bytes) {
        throw new ApiError('NotFound', 'no such file');
      }
      return { status: 200, body: bytes, contentType: key.endsWith('.png') ? 'image/png' : 'application/octet-stream' };
    });
  }

  if (ctx.config.payments.provider === 'fake' && ctx.config.nodeEnv !== 'production') {
    registerFakeCheckout(router, ctx);
  }
  return router;
}

/** Development only: a stand-in for Stripe's hosted page that settles through the same signed-webhook path as production. */
function registerFakeCheckout(router: Router, ctx: AppContext): void {
  const orderForSession = async (sessionId: string) => {
    const orderId = sessionId.startsWith('cs_fake_') ? sessionId.slice('cs_fake_'.length) : '';
    const order = UUID.test(orderId) ? await getOrder(ctx.db, orderId) : null;
    if (!order || order.checkoutSessionId !== sessionId) {
      throw new ApiError('NotFound', 'no such checkout');
    }
    return order;
  };
  router.add('GET', '/dev/fake-checkout/:session', async (context) => {
    const order = await orderForSession(context.params.session!);
    const amount = `${order.amountMinor} ${order.currency} (minor units)`;
    return html(
      page(
        'Fake checkout (development)',
        `<p>Order <code>${escapeHtml(order.id)}</code>: ${escapeHtml(amount)}, state ${escapeHtml(order.state)}.</p>
<form method="post" action="/dev/fake-checkout/${escapeHtml(order.checkoutSessionId!)}/pay"><button type="submit">Pay</button></form>`,
      ),
    );
  });
  router.add('POST', '/dev/fake-checkout/:session/pay', async (context) => {
    const order = await orderForSession(context.params.session!);
    const raw = Buffer.from(
      JSON.stringify({
        id: `evt_fake_${randomUUID()}`,
        type: 'checkout.session.completed',
        data: {
          object: {
            id: order.checkoutSessionId,
            object: 'checkout.session',
            client_reference_id: order.id,
            metadata: { orderId: order.id },
            payment_status: 'paid',
            amount_total: Number(order.amountMinor),
            currency: order.currency.toLowerCase(),
            payment_intent: `pi_fake_${order.id}`,
          },
        },
      }),
    );
    const signature = signStripePayload(raw, ctx.config.payments.webhookSecrets[0]!, Math.floor(ctx.clock.now() / 1000));
    const outcome = await handleStripeWebhook(ctx, raw, signature);
    return html(page('Fake payment sent', `<p>Webhook result: ${escapeHtml(outcome.status)} (${escapeHtml(outcome.detail)}). Return to StageCraft.</p>`));
  });
  router.add('GET', '/dev/fake-onboarding/:account', async (context) => {
    const providerAccountId = context.params.account!;
    const raw = Buffer.from(
      JSON.stringify({ id: `evt_fake_${randomUUID()}`, type: 'account.updated', data: { object: { id: providerAccountId, object: 'account', payouts_enabled: true } } }),
    );
    const signature = signStripePayload(raw, ctx.config.payments.webhookSecrets[0]!, Math.floor(ctx.clock.now() / 1000));
    const outcome = await handleStripeWebhook(ctx, raw, signature);
    return html(page('Fake onboarding (development)', `<p>Payouts enabled: ${escapeHtml(outcome.detail)}. Return to StageCraft.</p>`));
  });
}
