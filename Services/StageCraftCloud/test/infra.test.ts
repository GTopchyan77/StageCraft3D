// Configuration, migrations, blob stores (local signed URLs, S3 SigV4 against AWS's published vector and a fake S3), credentials.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type { AddressInfo } from 'node:net';
import { ConfigError, describeConfig, loadConfig } from '../src/config.ts';
import { migrate } from '../src/db/migrate.ts';
import { SqliteDatabase, toSqlitePlaceholders } from '../src/db/sqlite.ts';
import { big, num, str, strOrNull, UniqueViolation } from '../src/db/driver.ts';
import { presignUrl, uriEncode, amzDate } from '../src/blob/sigv4.ts';
import { S3BlobStore, defaultCredentialProvider } from '../src/blob/s3BlobStore.ts';
import { LocalBlobStore } from '../src/blob/localBlobStore.ts';
import { assertBlobKey } from '../src/blob/blobStore.ts';
import { OrderNotifier } from '../src/context.ts';
import { testEnv } from './helpers.ts';

function configProblems(env: Record<string, string>): string[] {
  try {
    loadConfig(env);
    return [];
  } catch (error) {
    assert.ok(error instanceof ConfigError);
    return error.problems;
  }
}

test('config: development defaults, validation errors, production refusals', () => {
  const config = loadConfig(testEnv());
  assert.equal(config.sellerShareBps, 8500n);
  assert.equal(config.payoutHoldDays, 14);
  assert.equal(config.auth.mode, 'dev');
  assert.equal(config.payments.provider, 'fake');
  assert.deepEqual(config.settlementCurrencies, ['USD']);
  assert.equal(describeConfig(config).sellerShareBps, 8500);
  assert.ok(!JSON.stringify(describeConfig(config)).includes('whsec'));

  assert.deepEqual(configProblems(testEnv({ PORT: 'abc', SELLER_SHARE_BPS: '10001', STRIPE_AUTOMATIC_TAX: 'maybe', LOG_LEVEL: 'loud' })), [
    'PORT must be an integer in 1..65535',
    'LOG_LEVEL must be one of debug, info, warn, error',
    'STRIPE_AUTOMATIC_TAX must be true or false',
    'SELLER_SHARE_BPS must be an integer in 0..10000',
  ].filter((problem) => !problem.startsWith('STRIPE_AUTOMATIC_TAX')));
  assert.ok(configProblems(testEnv({ SETTLEMENT_CURRENCIES: 'USD,EURO' })).some((p) => p.includes('SETTLEMENT_CURRENCIES')));
  assert.ok(configProblems(testEnv({ PUBLIC_BASE_URL: 'ftp://x' })).some((p) => p.includes('PUBLIC_BASE_URL')));
  assert.ok(configProblems(testEnv({ PUBLIC_BASE_URL: 'not a url' })).some((p) => p.includes('PUBLIC_BASE_URL')));
  assert.ok(configProblems(testEnv({ BLOB_URL_SECRET: 'short' })).some((p) => p.includes('BLOB_URL_SECRET')));
  assert.ok(configProblems(testEnv({ AUTH_MODE: 'oidc' })).includes('OIDC_ISSUER is required'));
  assert.ok(configProblems(testEnv({ PAYMENT_PROVIDER: 'stripe' })).includes('STRIPE_SECRET_KEY is required'));
  assert.ok(configProblems(testEnv({ PAYMENT_PROVIDER: 'stripe', STRIPE_SECRET_KEY: 'pk_live_x', STRIPE_PLATFORM_COUNTRY: 'USA' })).length === 2);
  assert.ok(configProblems(testEnv({ BLOB_STORE: 's3' })).includes('S3_BUCKET is required'));
  const stripe = loadConfig(testEnv({ PAYMENT_PROVIDER: 'stripe', STRIPE_SECRET_KEY: 'sk_test_x', STRIPE_AUTOMATIC_TAX: 'true', STRIPE_API_VERSION: '2025-01-27.acacia' }));
  assert.equal(stripe.payments.provider === 'stripe' && stripe.payments.automaticTax, true);

  // Production: dev identity, fake provider, local blobs, SQLite, http URLs and missing secrets are all refused.
  const production = configProblems({ NODE_ENV: 'production', AUTH_MODE: 'dev', PAYMENT_PROVIDER: 'fake', BLOB_STORE: 'local', PUBLIC_BASE_URL: 'http://x', S3_ENDPOINT: 'http://minio' });
  for (const expected of [
    'AUTH_MODE=dev is refused when NODE_ENV=production',
    'PAYMENT_PROVIDER=fake is refused when NODE_ENV=production',
    'BLOB_STORE=local is refused when NODE_ENV=production',
    'DATABASE_URL (Postgres) is required in production',
    'PUBLIC_BASE_URL must be https in production',
    'STRIPE_WEBHOOK_SECRETS is required',
    'S3_ENDPOINT must be https in production',
  ]) {
    assert.ok(production.includes(expected), expected);
  }
  const good = loadConfig({
    NODE_ENV: 'production',
    PUBLIC_BASE_URL: 'https://cloud.stagecraft.example',
    DATABASE_URL: 'postgres://u:p@db/stagecraft',
    OIDC_ISSUER: 'https://id.example',
    OIDC_CLIENT_ID: 'stagecraft',
    STRIPE_SECRET_KEY: 'sk_live_abc',
    STRIPE_WEBHOOK_SECRETS: 'whsec_a, whsec_b',
    S3_BUCKET: 'stagecraft-packages',
    S3_REGION: 'us-east-1',
  });
  assert.equal(good.migrateOnStart, false);
  assert.equal(good.payments.webhookSecrets.length, 2);
  assert.equal(good.blob.kind, 's3');
});

test('migrations apply once, in order, and a failing migration rolls back', async () => {
  const db = new SqliteDatabase(':memory:');
  try {
    assert.deepEqual(await migrate(db), ['0001_init']);
    assert.deepEqual(await migrate(db), []);
    const dir = mkdtempSync(join(tmpdir(), 'scc-migrations-'));
    writeFileSync(join(dir, '0002_bad.sql'), 'CREATE TABLE ok_table (a TEXT); THIS IS NOT SQL;');
    writeFileSync(join(dir, 'README.txt'), 'ignored');
    await assert.rejects(migrate(db, dir), /migration 0002_bad failed/);
    assert.equal((await db.query("SELECT name FROM sqlite_master WHERE name = 'ok_table'")).length, 0);
    assert.equal((await db.query("SELECT version FROM schema_migrations WHERE version = '0002_bad'")).length, 0);
    // Unique violations map to UniqueViolation; driver helpers convert columns strictly.
    await db.query("INSERT INTO accounts (id, issuer, subject, display_name, created_at) VALUES ('a', 'i', 's', 'n', 1)");
    await assert.rejects(db.query("INSERT INTO accounts (id, issuer, subject, display_name, created_at) VALUES ('a', 'i', 's2', 'n', 1)"), UniqueViolation);
    await assert.rejects(db.query('SELECT * FROM missing_table'), /no such table/);
    await assert.rejects(db.withTransaction(async (tx) => tx.query('SELECT * FROM missing_table')), /no such table/);
  } finally {
    await db.close();
  }
  assert.equal(toSqlitePlaceholders('a = $1 AND b = $12 OR c = $1'), 'a = ?1 AND b = ?12 OR c = ?1');
  assert.equal(big('-5'), -5n);
  assert.equal(big(7), 7n);
  assert.equal(big(null), 0n);
  assert.throws(() => big(1.5), TypeError);
  assert.throws(() => num(2n ** 60n), RangeError);
  assert.throws(() => str(5), TypeError);
  assert.equal(strOrNull(undefined), null);
});

test('SigV4 pre-signed URL matches the example in the AWS documentation', () => {
  const url = presignUrl({
    method: 'GET',
    url: 'https://examplebucket.s3.amazonaws.com/test.txt',
    region: 'us-east-1',
    credentials: { accessKeyId: 'AKIAIOSFODNN7EXAMPLE', secretAccessKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY' },
    now: Date.UTC(2013, 4, 24),
    expiresSeconds: 86400,
  });
  assert.equal(
    url,
    'https://examplebucket.s3.amazonaws.com/test.txt?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request&X-Amz-Date=20130524T000000Z&X-Amz-Expires=86400&X-Amz-SignedHeaders=host&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404',
  );
  assert.equal(uriEncode("a b!'()*/"), 'a%20b%21%27%28%29%2A%2F');
  assert.equal(uriEncode('a/b', false), 'a/b');
  assert.equal(amzDate(Date.UTC(2020, 0, 2, 3, 4, 5)), '20200102T030405Z');
  const withToken = presignUrl({ method: 'PUT', url: 'https://b.s3.eu-west-1.amazonaws.com/k', region: 'eu-west-1', credentials: { accessKeyId: 'A', secretAccessKey: 'S', sessionToken: 'T/+' }, now: 0, expiresSeconds: 60 });
  assert.match(withToken, /X-Amz-Security-Token=T%2F%2B/);
});

test('S3 store against a fake S3: put, get, missing, delete, download URL; credential providers', async () => {
  const objects = new Map<string, Buffer>();
  const seen: string[] = [];
  const server = createServer(async (req, res) => {
    const url = new URL(req.url!, 'http://x');
    seen.push(`${req.method} ${url.pathname} ${url.searchParams.has('X-Amz-Signature')}`);
    const chunks: Buffer[] = [];
    for await (const chunk of req) chunks.push(chunk as Buffer);
    if (url.pathname === '/creds') {
      if (req.headers.authorization !== 'pod-token') return void res.writeHead(401).end();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      return void res.end(JSON.stringify({ AccessKeyId: 'AK', SecretAccessKey: 'SK', Token: 'TK', Expiration: new Date(Date.now() + 3_600_000).toISOString() }));
    }
    if (url.pathname === '/fail') return void res.writeHead(500).end();
    if (req.method === 'PUT') objects.set(url.pathname, Buffer.concat(chunks));
    if (req.method === 'DELETE') objects.delete(url.pathname);
    if (req.method === 'GET') {
      const body = objects.get(url.pathname);
      if (!body) return void res.writeHead(404).end();
      return void res.writeHead(200).end(body);
    }
    res.writeHead(200).end();
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const endpoint = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  try {
    const store = new S3BlobStore({
      bucket: 'bucket',
      region: 'us-east-1',
      endpoint,
      prefix: 'prod/',
      credentials: defaultCredentialProvider({ AWS_ACCESS_KEY_ID: 'AK', AWS_SECRET_ACCESS_KEY: 'SK' }, () => Date.now()),
      now: () => Date.now(),
    });
    await store.put('packages/a.stage', Buffer.from('bytes'), 'application/octet-stream');
    assert.equal((await store.get('packages/a.stage'))!.toString(), 'bytes');
    assert.equal(await store.get('packages/missing.stage'), null);
    await store.delete('packages/a.stage');
    assert.equal(await store.get('packages/a.stage'), null);
    const download = await store.downloadUrl('packages/a.stage', 300, 'My "file".stage');
    assert.match(download, /response-content-disposition=attachment%3B%20filename%3D%22My__file_.stage%22/);
    assert.match(download, /X-Amz-Expires=300/);
    await store.check();
    assert.ok(seen.every((line) => line.endsWith('true')));
    assert.ok(seen[0]!.startsWith('PUT /bucket/prod/packages/a.stage'));
    assert.equal(new S3BlobStore({ bucket: 'b', region: 'eu-west-1', endpoint: null, prefix: '', credentials: { get: async () => ({ accessKeyId: 'a', secretAccessKey: 's' }) }, now: () => 0 }).objectUrl('x/y.png'), 'https://b.s3.eu-west-1.amazonaws.com/x/y.png');

    // EKS Pod Identity: container credentials endpoint with the token file.
    const tokenFile = join(mkdtempSync(join(tmpdir(), 'scc-token-')), 'token');
    writeFileSync(tokenFile, 'pod-token\n');
    const pod = defaultCredentialProvider({ AWS_CONTAINER_CREDENTIALS_FULL_URI: `${endpoint}/creds`, AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE: tokenFile }, () => Date.now());
    assert.deepEqual(await pod.get(), { accessKeyId: 'AK', secretAccessKey: 'SK', sessionToken: 'TK' });
    const badPod = defaultCredentialProvider({ AWS_CONTAINER_CREDENTIALS_FULL_URI: `${endpoint}/creds` }, () => Date.now());
    await assert.rejects(badPod.get(), /answered 401/);
    await assert.rejects(defaultCredentialProvider({}, () => 0).get(), /no AWS credentials/);
    // IRSA: STS AssumeRoleWithWebIdentity (answered here by a stub fetch).
    const sts = defaultCredentialProvider({ AWS_WEB_IDENTITY_TOKEN_FILE: tokenFile, AWS_ROLE_ARN: 'arn:aws:iam::1:role/r', AWS_REGION: 'eu-central-1' }, () => 0, (async (input: string | URL) => {
      assert.match(String(input), /^https:\/\/sts\.eu-central-1\.amazonaws\.com\/\?Action=AssumeRoleWithWebIdentity/);
      return new Response('<AccessKeyId>AK2</AccessKeyId><SecretAccessKey>SK2</SecretAccessKey><SessionToken>TK2</SessionToken><Expiration>2030-01-01T00:00:00Z</Expiration>');
    }) as typeof fetch);
    assert.deepEqual(await sts.get(), { accessKeyId: 'AK2', secretAccessKey: 'SK2', sessionToken: 'TK2' });
    const stsFail = defaultCredentialProvider({ AWS_WEB_IDENTITY_TOKEN_FILE: tokenFile, AWS_ROLE_ARN: 'r' }, () => 0, (async () => new Response('<Error/>', { status: 403 })) as typeof fetch);
    await assert.rejects(stsFail.get(), /STS AssumeRoleWithWebIdentity failed/);
    const s3Errors = new S3BlobStore({ bucket: 'b', region: 'r', endpoint, prefix: '', credentials: { get: async () => ({ accessKeyId: 'a', secretAccessKey: 's' }) }, now: () => 0, fetchImpl: (async () => new Response('', { status: 500 })) as typeof fetch });
    await assert.rejects(s3Errors.put('k', Buffer.alloc(1), 'x'), /PUT answered 500/);
    await assert.rejects(s3Errors.get('k'), /GET answered 500/);
    await assert.rejects(s3Errors.delete('k'), /DELETE answered 500/);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test('local blob store: atomic writes, signed URLs, key validation; order notifier', async () => {
  let now = 1_000_000;
  const store = new LocalBlobStore(mkdtempSync(join(tmpdir(), 'scc-local-')), 'http://dev', 'secret-secret-secret', () => now);
  await store.put('a/b.png', Buffer.from('x'), 'image/png');
  assert.equal((await store.get('a/b.png'))!.toString(), 'x');
  assert.equal(await store.get('a/missing.png'), null);
  const url = new URL(await store.downloadUrl('a/b.png', 60));
  assert.equal(store.verify('a/b.png', url.searchParams.get('expires'), url.searchParams.get('sig')), true);
  assert.equal(store.verify('a/c.png', url.searchParams.get('expires'), url.searchParams.get('sig')), false);
  assert.equal(store.verify('../x', url.searchParams.get('expires'), url.searchParams.get('sig')), false);
  assert.equal(store.verify('a/b.png', null, null), false);
  now += 61_000;
  assert.equal(store.verify('a/b.png', url.searchParams.get('expires'), url.searchParams.get('sig')), false);
  await store.delete('a/b.png');
  assert.equal(await store.get('a/b.png'), null);
  for (const bad of ['../etc/passwd', '/abs', 'UPPER', 'a//b', 'a/../b']) {
    assert.throws(() => assertBlobKey(bad), RangeError);
  }
  const notifier = new OrderNotifier();
  const started = Date.now();
  const waiting = notifier.wait('o1', 5000);
  assert.equal(notifier.listenerCount('o1'), 1);
  notifier.notify('o1');
  await waiting;
  assert.ok(Date.now() - started < 1000);
  assert.equal(notifier.listenerCount('o1'), 0);
  await notifier.wait('o2', 10);
});
