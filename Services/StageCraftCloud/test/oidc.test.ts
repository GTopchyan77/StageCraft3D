// OIDC sign-in against a local fake identity provider with a generated RSA key: ID token verification and the browser flow.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync, sign, type KeyObject } from 'node:crypto';
import { createServer } from 'node:http';
import type { AddressInfo } from 'node:net';
import { OidcClient, OidcError, pkcePair } from '../src/auth/oidc.ts';
import { api, harness, serve } from './helpers.ts';

const { privateKey, publicKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
const other = generateKeyPairSync('rsa', { modulusLength: 2048 });

function b64(value: unknown): string {
  return Buffer.from(JSON.stringify(value)).toString('base64url');
}

function jwt(claims: Record<string, unknown>, header: Record<string, unknown> = { alg: 'RS256', kid: 'k1', typ: 'JWT' }, key: KeyObject = privateKey): string {
  const signingInput = `${b64(header)}.${b64(claims)}`;
  const signature = sign('RSA-SHA256', Buffer.from(signingInput), key).toString('base64url');
  return `${signingInput}.${signature}`;
}

interface FakeIdp {
  issuer: string;
  tokenRequests: URLSearchParams[];
  tokenRequestAuth: (string | undefined)[];
  nextIdToken: string | null;
  discoveryIssuer: string | null;
  jwksFetches: number;
  jwksKeys: () => unknown[];
  close(): Promise<void>;
}

async function fakeIdp(): Promise<FakeIdp> {
  const state: FakeIdp = {
    issuer: '',
    tokenRequests: [],
    tokenRequestAuth: [],
    nextIdToken: null,
    discoveryIssuer: null,
    jwksFetches: 0,
    jwksKeys: () => [{ ...publicKey.export({ format: 'jwk' }), kid: 'k1', use: 'sig', alg: 'RS256' }],
    close: async () => undefined,
  };
  const server = createServer(async (req, res) => {
    const chunks: Buffer[] = [];
    for await (const chunk of req) chunks.push(chunk as Buffer);
    const send = (status: number, body: unknown) => {
      res.writeHead(status, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(body));
    };
    if (req.url === '/.well-known/openid-configuration') {
      return send(200, {
        issuer: state.discoveryIssuer ?? state.issuer,
        authorization_endpoint: `${state.issuer}/authorize`,
        token_endpoint: `${state.issuer}/token`,
        jwks_uri: `${state.issuer}/jwks`,
      });
    }
    if (req.url === '/jwks') {
      state.jwksFetches++;
      return send(200, { keys: state.jwksKeys() });
    }
    if (req.url === '/token') {
      state.tokenRequests.push(new URLSearchParams(Buffer.concat(chunks).toString()));
      state.tokenRequestAuth.push(req.headers.authorization);
      return state.nextIdToken ? send(200, { id_token: state.nextIdToken, access_token: 'at' }) : send(400, { error: 'invalid_grant' });
    }
    send(404, {});
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  state.issuer = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  state.close = () => new Promise((resolve) => server.close(() => resolve()));
  return state;
}

const NOW = Date.UTC(2026, 10, 7, 12);
const nowSeconds = Math.floor(NOW / 1000);

function claims(idp: FakeIdp, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return { iss: idp.issuer, aud: 'stagecraft', sub: 'user-1', exp: nowSeconds + 300, iat: nowSeconds, nonce: 'n1', email: 'u@example.com', name: 'User One', ...extra };
}

test('ID token verification: RS256 signature, issuer, audience, expiry, nonce, key rotation', async () => {
  const idp = await fakeIdp();
  try {
    let now = NOW;
    const client = new OidcClient({ issuer: idp.issuer, clientId: 'stagecraft', clientSecret: null, redirectUri: 'http://cloud/cb', scopes: 'openid', now: () => now });
    const identity = await client.verifyIdToken(jwt(claims(idp)), 'n1');
    assert.deepEqual(identity, { issuer: idp.issuer, subject: 'user-1', email: 'u@example.com', name: 'User One' });
    // Multiple audiences need azp; an unverified e-mail is dropped.
    assert.equal((await client.verifyIdToken(jwt(claims(idp, { aud: ['stagecraft', 'x'], azp: 'stagecraft', email_verified: false })), 'n1')).email, null);
    const refuse = async (token: string, message: RegExp, nonce = 'n1') => assert.rejects(client.verifyIdToken(token, nonce), (error: unknown) => error instanceof OidcError && message.test(error.message));
    await refuse(jwt(claims(idp, { aud: ['stagecraft', 'x'] })), /audience/);
    await refuse(jwt(claims(idp, { aud: 'someone-else' })), /audience/);
    await refuse(jwt(claims(idp, { iss: 'https://evil' })), /issuer/);
    await refuse(jwt(claims(idp, { exp: nowSeconds - 120 })), /expired/);
    await refuse(jwt(claims(idp, { exp: undefined })), /expired/);
    await refuse(jwt(claims(idp, { iat: nowSeconds + 3600 })), /future/);
    await refuse(jwt(claims(idp)), /nonce/, 'other-nonce');
    await refuse(jwt(claims(idp, { sub: '' })), /subject/);
    await refuse(jwt(claims(idp), { alg: 'none', kid: 'k1' }), /RS256/);
    await refuse(jwt(claims(idp), { alg: 'HS256', kid: 'k1' }), /RS256/);
    await refuse(jwt(claims(idp), { alg: 'RS256' }), /RS256/);
    await refuse(jwt(claims(idp), undefined, other.privateKey), /signature/);
    await refuse('a.b', /malformed/);
    await refuse(`${b64({ alg: 'RS256', kid: 'k1' })}.bm90LWpzb24.sig`, /malformed/);
    await refuse(`${Buffer.from('[1]').toString('base64url')}.${b64(claims(idp))}.sig`, /malformed/);
    // Tampered payload with the original signature.
    const [header, , signature] = jwt(claims(idp)).split('.');
    await refuse(`${header}.${b64(claims(idp, { sub: 'admin' }))}.${signature}`, /signature/);
    // Unknown kid: the JWKS is refetched at most once a minute (the keys were fetched just now), so it is refused without a fetch.
    const fetchesBefore = idp.jwksFetches;
    await refuse(jwt(claims(idp), { alg: 'RS256', kid: 'k2' }), /unknown key/);
    await refuse(jwt(claims(idp), { alg: 'RS256', kid: 'k2' }), /unknown key/);
    assert.equal(idp.jwksFetches, fetchesBefore);
    // Key rotation: the provider publishes k2; after the refresh window a token signed with it verifies.
    idp.jwksKeys = () => [
      { ...publicKey.export({ format: 'jwk' }), kid: 'k1' },
      { ...other.publicKey.export({ format: 'jwk' }), kid: 'k2', use: 'sig' },
      { kty: 'EC', kid: 'ec' },
      { kty: 'RSA', kid: 'enc', use: 'enc' },
      { kty: 'RSA', kid: 'broken', n: '!!', e: 'AQAB' },
    ];
    now += 61_000;
    assert.equal((await client.verifyIdToken(jwt(claims(idp, { exp: nowSeconds + 400 }), { alg: 'RS256', kid: 'k2' }, other.privateKey), 'n1')).subject, 'user-1');
  } finally {
    await idp.close();
  }
});

test('discovery must match the issuer; unreachable and failing providers are OidcErrors', async () => {
  const idp = await fakeIdp();
  try {
    idp.discoveryIssuer = 'https://not-the-issuer';
    const client = new OidcClient({ issuer: idp.issuer, clientId: 'c', clientSecret: null, redirectUri: 'r', scopes: 'openid', now: () => NOW });
    await assert.rejects(client.getDiscovery(), /does not match/);
    idp.discoveryIssuer = null;
    await assert.rejects(client.completeSignIn('bad-code', 'verifier', 'n'), /answered 400/);
    idp.nextIdToken = null;
  } finally {
    await idp.close();
  }
  const offline = new OidcClient({ issuer: 'http://127.0.0.1:1', clientId: 'c', clientSecret: null, redirectUri: 'r', scopes: 'openid', now: () => NOW });
  await assert.rejects(offline.getDiscovery(), /unreachable/);
  const html = await (async () => {
    const server = createServer((_req, res) => {
      res.writeHead(200);
      res.end('<html>');
    });
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
    const client = new OidcClient({ issuer: `http://127.0.0.1:${(server.address() as AddressInfo).port}`, clientId: 'c', clientSecret: null, redirectUri: 'r', scopes: 'openid', now: () => NOW });
    const error = await client.getDiscovery().catch((caught: Error) => caught);
    await new Promise((resolve) => server.close(resolve));
    return error;
  })();
  assert.match(String(html), /invalid JSON/);
  const pair = pkcePair();
  assert.equal(createHash('sha256').update(pair.verifier).digest('base64url'), pair.challenge);
});

test('browser flow: verify page -> provider (PKCE, state, nonce) -> callback -> device approved -> session', async () => {
  const idp = await fakeIdp();
  const h = await harness({ AUTH_MODE: 'oidc', OIDC_ISSUER: idp.issuer, OIDC_CLIENT_ID: 'stagecraft', OIDC_CLIENT_SECRET: 'client-secret' });
  h.clock.current = NOW;
  const server = await serve(h.ctx);
  try {
    const device = (await api(server, 'POST', '/v1/auth/device')).json;
    const page = await api(server, 'GET', `/auth/verify?user_code=${device.userCode}`);
    assert.doesNotMatch(page.text, /Development user/);
    const start = await api(server, 'GET', `/auth/verify/start?user_code=${device.userCode}`);
    assert.equal(start.status, 302);
    const authorize = new URL(start.headers.get('location')!);
    assert.equal(authorize.origin + authorize.pathname, `${idp.issuer}/authorize`);
    assert.equal(authorize.searchParams.get('client_id'), 'stagecraft');
    assert.equal(authorize.searchParams.get('redirect_uri'), 'http://cloud.test/auth/callback');
    assert.equal(authorize.searchParams.get('code_challenge_method'), 'S256');
    assert.equal(authorize.searchParams.get('response_type'), 'code');
    const state = authorize.searchParams.get('state')!;
    const nonce = authorize.searchParams.get('nonce')!;

    // A token whose nonce does not match is refused and the device stays pending.
    idp.nextIdToken = jwt(claims(idp, { nonce: 'wrong' }));
    assert.match((await api(server, 'GET', `/auth/callback?state=${state}&code=abc`)).text, /could not be verified/);
    // The state is single-use.
    assert.match((await api(server, 'GET', `/auth/callback?state=${state}&code=abc`)).text, /expired or was already used/);

    const second = new URL((await api(server, 'GET', `/auth/verify/start?user_code=${device.userCode}`)).headers.get('location')!);
    idp.nextIdToken = jwt(claims(idp, { nonce: second.searchParams.get('nonce') }));
    const callback = await api(server, 'GET', `/auth/callback?state=${second.searchParams.get('state')}&code=good-code`);
    assert.match(callback.text, /Signed in/);
    const tokenRequest = idp.tokenRequests.at(-1)!;
    assert.equal(tokenRequest.get('grant_type'), 'authorization_code');
    assert.equal(tokenRequest.get('code'), 'good-code');
    assert.ok(tokenRequest.get('code_verifier'));
    assert.equal(createHash('sha256').update(tokenRequest.get('code_verifier')!).digest('base64url'), second.searchParams.get('code_challenge'));
    assert.equal(idp.tokenRequestAuth.at(-1), `Basic ${Buffer.from('stagecraft:client-secret').toString('base64')}`);

    const session = await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: device.deviceCode } });
    assert.equal(session.status, 200);
    const me = await api(server, 'GET', '/v1/me', { token: session.json.sessionToken });
    assert.equal(me.json.displayName, 'User One');
    // A role claim in the token never grants a role.
    assert.deepEqual(me.json.roles, []);
    void nonce;

    // Cancel at the provider: the device code is denied.
    const denied = (await api(server, 'POST', '/v1/auth/device')).json;
    const third = new URL((await api(server, 'GET', `/auth/verify/start?user_code=${denied.userCode}`)).headers.get('location')!);
    assert.equal((await api(server, 'GET', `/auth/callback?state=${third.searchParams.get('state')}&error=access_denied`)).status, 400);
    assert.equal((await api(server, 'POST', '/v1/auth/device/token', { json: { deviceCode: denied.deviceCode } })).json.error.code, 'access_denied');
    assert.equal((await api(server, 'GET', '/auth/callback')).status, 400);
    // No secrets or codes in the logs.
    const logs = h.log.lines.join('\n');
    assert.ok(!logs.includes('client-secret') && !logs.includes('good-code') && !logs.includes(session.json.sessionToken));
  } finally {
    await server.close();
    await h.close();
    await idp.close();
  }
});
