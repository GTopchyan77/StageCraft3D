import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, type IncomingMessage } from 'node:http';
import type { AddressInfo } from 'node:net';
import { encodeForm, StripeProvider } from '../src/payments/stripe.ts';
import { ProviderFailure, payoutMethodFor } from '../src/payments/provider.ts';
import { FakeProvider } from '../src/payments/fake.ts';

interface Captured {
  method: string;
  path: string;
  headers: IncomingMessage['headers'];
  form: URLSearchParams;
}

/** A local stand-in for api.stripe.com that records every request and answers from a script. */
async function fakeStripe(answer: (request: Captured) => { status: number; body: unknown }): Promise<{ url: string; requests: Captured[]; close(): Promise<void> }> {
  const requests: Captured[] = [];
  const server = createServer(async (req, res) => {
    const chunks: Buffer[] = [];
    for await (const chunk of req) chunks.push(chunk as Buffer);
    const captured = { method: req.method!, path: req.url!, headers: req.headers, form: new URLSearchParams(Buffer.concat(chunks).toString()) };
    requests.push(captured);
    const { status, body } = answer(captured);
    res.writeHead(status, { 'Content-Type': 'application/json' });
    res.end(typeof body === 'string' ? body : JSON.stringify(body));
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  return {
    url: `http://127.0.0.1:${(server.address() as AddressInfo).port}`,
    requests,
    close: () => new Promise((resolve) => server.close(() => resolve())),
  };
}

function provider(url: string, extra: Partial<ConstructorParameters<typeof StripeProvider>[0]> = {}): StripeProvider {
  return new StripeProvider({ secretKey: 'sk_test_123', baseUrl: url, automaticTax: false, platformCountry: 'US', ...extra });
}

test('encodeForm uses Stripe bracket notation and skips null/undefined', () => {
  assert.equal(
    encodeForm({ a: 1, b: { c: 'x y', d: [{ e: true }, 5n] }, skip: undefined, none: null }),
    'a=1&b%5Bc%5D=x%20y&b%5Bd%5D%5B0%5D%5Be%5D=true&b%5Bd%5D%5B1%5D=5',
  );
});

test('checkout session: exact form fields, auth and idempotency headers', async () => {
  const stripe = await fakeStripe(() => ({ status: 200, body: { id: 'cs_test_1', url: 'https://checkout.stripe.com/c/pay/cs_test_1', expires_at: 1_800_000_000 } }));
  try {
    const session = await provider(stripe.url, { apiVersion: '2025-01-27.acacia' }).createCheckout({
      orderId: 'order-1',
      amountMinor: 1999n,
      currency: 'USD',
      title: 'Arena Rig',
      expiresAt: 1_800_000_000_500,
      successUrl: 'https://cloud/checkout/success?order=order-1',
      cancelUrl: 'https://cloud/checkout/cancel?order=order-1',
    });
    assert.deepEqual(session, { sessionId: 'cs_test_1', url: 'https://checkout.stripe.com/c/pay/cs_test_1', expiresAt: 1_800_000_000_000 });
    const [request] = stripe.requests;
    assert.equal(request!.method, 'POST');
    assert.equal(request!.path, '/v1/checkout/sessions');
    assert.equal(request!.headers.authorization, 'Bearer sk_test_123');
    assert.equal(request!.headers['idempotency-key'], 'checkout-order-1');
    assert.equal(request!.headers['content-type'], 'application/x-www-form-urlencoded');
    assert.equal(request!.headers['stripe-version'], '2025-01-27.acacia');
    assert.deepEqual(Object.fromEntries(request!.form), {
      mode: 'payment',
      client_reference_id: 'order-1',
      success_url: 'https://cloud/checkout/success?order=order-1',
      cancel_url: 'https://cloud/checkout/cancel?order=order-1',
      expires_at: '1800000000',
      'line_items[0][quantity]': '1',
      'line_items[0][price_data][currency]': 'usd',
      'line_items[0][price_data][unit_amount]': '1999',
      'line_items[0][price_data][product_data][name]': 'Arena Rig',
      'metadata[orderId]': 'order-1',
      'payment_intent_data[metadata][orderId]': 'order-1',
      'automatic_tax[enabled]': 'false',
    });
  } finally {
    await stripe.close();
  }
});

test('refunds, connected accounts, account links and transfers send the documented requests', async () => {
  const stripe = await fakeStripe((request) => {
    switch (request.path) {
      case '/v1/refunds':
        return { status: 200, body: { id: 're_1', status: 'pending' } };
      case '/v1/accounts':
        return { status: 200, body: { id: 'acct_1' } };
      case '/v1/account_links':
        return { status: 200, body: { url: 'https://connect.stripe.com/setup/e/acct_1', expires_at: 1_800_000_000 } };
      default:
        return { status: 200, body: { id: 'tr_1' } };
    }
  });
  try {
    const p = provider(stripe.url, { automaticTax: true });
    assert.deepEqual(await p.createRefund({ paymentIntentId: 'pi_1', amountMinor: 500n, idempotencyKey: 'rr-1', orderId: 'o1' }), { refundId: 're_1', status: 'pending' });
    assert.deepEqual(await p.createConnectedAccount({ accountId: 'acc-1', country: 'DE' }), { providerAccountId: 'acct_1' });
    assert.deepEqual(await p.createConnectedAccount({ accountId: 'acc-2', country: 'US' }), { providerAccountId: 'acct_1' });
    const link = await p.createOnboardingLink({ providerAccountId: 'acct_1', refreshUrl: 'https://cloud/r', returnUrl: 'https://cloud/d' });
    assert.equal(link.url, 'https://connect.stripe.com/setup/e/acct_1');
    assert.deepEqual(await p.createTransfer({ amountMinor: 850n, currency: 'USD', destination: 'acct_1', idempotencyKey: 'po-1', payoutId: 'po-1' }), { transferId: 'tr_1' });

    const [refund, crossBorder, domestic, accountLink, transfer] = stripe.requests;
    assert.deepEqual(Object.fromEntries(refund!.form), { payment_intent: 'pi_1', amount: '500', 'metadata[orderId]': 'o1' });
    assert.equal(refund!.headers['idempotency-key'], 'refund-rr-1');
    assert.deepEqual(Object.fromEntries(crossBorder!.form), {
      type: 'express',
      country: 'DE',
      'capabilities[transfers][requested]': 'true',
      'tos_acceptance[service_agreement]': 'recipient',
      'metadata[accountId]': 'acc-1',
    });
    assert.equal(domestic!.form.get('tos_acceptance[service_agreement]'), null);
    assert.equal(crossBorder!.headers['idempotency-key'], 'account-acc-1');
    assert.deepEqual(Object.fromEntries(accountLink!.form), { account: 'acct_1', refresh_url: 'https://cloud/r', return_url: 'https://cloud/d', type: 'account_onboarding' });
    assert.deepEqual(Object.fromEntries(transfer!.form), { amount: '850', currency: 'usd', destination: 'acct_1', 'metadata[payoutId]': 'po-1' });
    assert.equal(transfer!.headers['idempotency-key'], 'transfer-po-1');
    assert.equal(transfer!.headers['stripe-version'], undefined);
  } finally {
    await stripe.close();
  }
});

test('provider failures: HTTP errors with codes, non-JSON answers, missing ids, network failures', async () => {
  let mode = 'error';
  const stripe = await fakeStripe(() => {
    if (mode === 'error') return { status: 402, body: { error: { type: 'card_error', code: 'amount_too_small' } } };
    if (mode === 'typeonly') return { status: 400, body: { error: { type: 'invalid_request_error' } } };
    if (mode === 'html') return { status: 500, body: '<html>' };
    return { status: 200, body: { status: 'ok' } };
  });
  try {
    const p = provider(stripe.url);
    const input = { paymentIntentId: 'pi', amountMinor: 1n, idempotencyKey: 'k', orderId: 'o' };
    await assert.rejects(p.createRefund(input), (error: unknown) => error instanceof ProviderFailure && error.status === 402 && error.providerCode === 'amount_too_small');
    mode = 'typeonly';
    await assert.rejects(p.createRefund(input), (error: unknown) => error instanceof ProviderFailure && error.providerCode === 'invalid_request_error');
    mode = 'html';
    await assert.rejects(p.createRefund(input), (error: unknown) => error instanceof ProviderFailure && error.status === 500 && error.providerCode === null);
    mode = 'noid';
    await assert.rejects(p.createRefund(input), /no id/);
    const link = await p.createOnboardingLink({ providerAccountId: 'a', refreshUrl: 'r', returnUrl: 'u' }).catch((error: Error) => error);
    assert.ok(link instanceof ProviderFailure);
  } finally {
    await stripe.close();
  }
  const offline = provider('http://127.0.0.1:1', { timeoutMs: 2000 });
  await assert.rejects(offline.createTransfer({ amountMinor: 1n, currency: 'USD', destination: 'a', idempotencyKey: 'k', payoutId: 'p' }), (error: unknown) => error instanceof ProviderFailure && error.status === 0);
});

test('checkout falls back to the requested expiry when Stripe omits it; refund status unknown', async () => {
  const stripe = await fakeStripe((request) => ({ status: 200, body: request.path === '/v1/refunds' ? { id: 're_2' } : { id: 'cs_2', url: 'https://x' } }));
  try {
    const p = provider(stripe.url);
    const session = await p.createCheckout({ orderId: 'o', amountMinor: 1n, currency: 'USD', title: 't', expiresAt: 2_000_000_000_999, successUrl: 's', cancelUrl: 'c' });
    assert.equal(session.expiresAt, 2_000_000_000_000);
    assert.equal((await p.createRefund({ paymentIntentId: 'pi', amountMinor: 1n, idempotencyKey: 'k', orderId: 'o' })).status, 'unknown');
    assert.equal((await p.createOnboardingLink({ providerAccountId: 'a', refreshUrl: 'r', returnUrl: 'u' })).expiresAt, 0);
  } finally {
    await stripe.close();
  }
});

test('payout method by country and the fake provider', async () => {
  assert.equal(payoutMethodFor('US'), 'stripe');
  assert.equal(payoutMethodFor('DE'), 'stripe');
  assert.equal(payoutMethodFor('NO'), 'stripe');
  assert.equal(payoutMethodFor('AM'), 'manual');
  const fake = new FakeProvider('http://dev');
  fake.failNext.transfer = true;
  await assert.rejects(fake.createTransfer({ amountMinor: 1n, currency: 'USD', destination: 'a', idempotencyKey: 'k', payoutId: 'p' }), ProviderFailure);
  assert.equal((await fake.createTransfer({ amountMinor: 1n, currency: 'USD', destination: 'a', idempotencyKey: 'k', payoutId: 'p' })).transferId, 'tr_fake_p');
  assert.match((await fake.createOnboardingLink({ providerAccountId: 'acct', refreshUrl: '', returnUrl: '' })).url, /fake-onboarding\/acct/);
});
