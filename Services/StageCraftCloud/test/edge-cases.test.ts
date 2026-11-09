// Edge paths not reached by the scenario tests: unknown section caps, odd webhook payloads, query parameter errors, support races.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readSection, readDirectory, makeTag, PackageError } from '../src/package/stagePackage.ts';
import { parseListingQuery } from '../src/query.ts';
import { ApiError } from '../src/errors.ts';
import { createTicket, parseTicket } from '../src/support.ts';
import { publishListing } from '../src/listings.ts';
import { moderate } from '../src/admin.ts';
import { createOrder } from '../src/commerce.ts';
import { handleStripeWebhook } from '../src/webhooks.ts';
import { getOrder } from '../src/orders.ts';
import { completedSession, enableSeller, harness, makeUser, manifestJson, PNG_1x1, sceneJson, signature, stripeEvent, writeContainer, writePackage } from './helpers.ts';

test('unknown sections have a raw-size cap of zero', () => {
  const bytes = writeContainer([{ tag: 'XTRA', raw: Buffer.from('abc') }]);
  const [section] = readDirectory(bytes);
  assert.equal(section!.tag, makeTag('XTRA'));
  assert.throws(() => readSection(bytes, section!), (error: unknown) => error instanceof PackageError && error.result === 'TooLarge');
  const empty = writeContainer([{ tag: 'XTRA', raw: Buffer.alloc(0) }]);
  assert.equal(readSection(empty, readDirectory(empty)[0]!).length, 0);
});

test('listing query parameter errors', () => {
  const invalid = (query: string) => assert.throws(() => parseListingQuery(new URLSearchParams(query)), (error: unknown) => error instanceof ApiError && error.code === 'InvalidRequest');
  invalid('offset=abc');
  invalid('limit=1e3');
  invalid('kinds=StageDesign,Nope');
  invalid('category=StageCraft.Marketplace.Category.Nope');
  invalid('priceFilter=Cheap');
  const query = parseListingQuery(new URLSearchParams('kinds=stagedesign&kinds=StageDesign,,cuelist&limit=999&offset=-5&maxCredits=-1&searchText=%20a%09b%20'));
  assert.deepEqual(query.kinds, ['StageDesign', 'CueList']);
  assert.equal(query.limit, 200);
  assert.equal(query.offset, 0);
  assert.equal(query.maxCredits, 0);
  assert.equal(query.searchText, 'a b');
  assert.equal(parseListingQuery(new URLSearchParams('limit=0')).limit, 1);
  assert.equal(parseListingQuery(new URLSearchParams(`searchText=${'x'.repeat(200)}`)).searchText.length, 128);
});

test('support: optional fields, a screenshot just over 4 MiB, and concurrent resends of one report', async () => {
  const minimal = parseTicket({ category: 'other', title: 'Hi', description: 'd' });
  assert.equal(minimal.steps, '');
  assert.equal(minimal.screenshot, null);
  assert.equal(minimal.clientReportId, null);
  const over = Buffer.concat([PNG_1x1, Buffer.alloc(4 * 1024 * 1024 + 1 - PNG_1x1.length)]).toString('base64');
  assert.throws(() => parseTicket({ category: 'bug', title: 't', description: 'd', screenshotPngBase64: over }), (error: unknown) => error instanceof ApiError && error.code === 'TooLarge');

  const h = await harness();
  try {
    const input = parseTicket({ category: 'bug', title: 't', description: 'd', clientReportId: '3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b', screenshotPngBase64: PNG_1x1.toString('base64') });
    const results = await Promise.all([createTicket(h.ctx, null, '1.2.3.4', input), createTicket(h.ctx, null, '1.2.3.4', input), createTicket(h.ctx, null, '1.2.3.4', input)]);
    assert.equal(new Set(results.map((result) => result.ticketId)).size, 1);
    assert.equal(results.filter((result) => result.created).length, 1);
    assert.equal(Number((await h.ctx.db.query('SELECT COUNT(*) AS n FROM support_tickets'))[0]!.n), 1);
    // Another IP is another anonymous reporter.
    assert.equal((await createTicket(h.ctx, null, '5.6.7.8', input)).created, true);
  } finally {
    await h.close();
  }
});

test('commerce and webhook edge paths: currency no longer settled, refund of a disputed order, charge without payment intent', async () => {
  const h = await harness();
  try {
    const seller = await makeUser(h.ctx, 'seller');
    const buyer = await makeUser(h.ctx, 'buyer');
    const admin = await makeUser(h.ctx, 'admin', ['admin']);
    await enableSeller(h.ctx, seller.accountId);
    const scene = sceneJson();
    const { bytes } = writePackage({ scene, manifest: manifestJson({ price: { credits: 0, realMoney: { amount: 700, currency: 'USD' } } }, scene) });
    const listing = await publishListing(h.ctx, { accountId: seller.accountId, roles: [], sessionHash: 'x' }, bytes);
    await moderate(h.ctx, admin.accountId, listing.id, { decision: 'approve' });

    const order = await createOrder(h.ctx, buyer.accountId, listing.id, 'key-edge-00001');
    const deliver = async (type: string, object: Record<string, unknown>) => {
      const raw = stripeEvent(type, object);
      return handleStripeWebhook(h.ctx, raw, signature(raw, h.clock));
    };
    await deliver('checkout.session.completed', completedSession(order));
    const pi = (await getOrder(h.ctx.db, order.id))!.paymentIntentId!;
    await deliver('charge.dispute.created', { id: 'dp_e', payment_intent: pi, amount: 700, currency: 'usd', status: 'needs_response' });
    assert.equal((await deliver('charge.refunded', { id: 'ch_e', payment_intent: pi, amount_refunded: 100, currency: 'usd' })).status, 'needs_attention');
    assert.equal((await deliver('charge.refunded', { id: 'ch_e', amount_refunded: 100, currency: 'usd' })).status, 'ignored');
    assert.equal((await deliver('charge.refunded', { id: 'ch_e', payment_intent: 5, amount_refunded: 100 })).status, 'ignored');

    h.ctx.config.settlementCurrencies = ['EUR'];
    const other = await makeUser(h.ctx, 'other');
    await assert.rejects(createOrder(h.ctx, other.accountId, listing.id, 'key-edge-00002'), (error: unknown) => error instanceof ApiError && error.code === 'CurrencyNotSupported');
  } finally {
    await h.close();
  }
});
