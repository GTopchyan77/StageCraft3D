// Builds the application context and HTTP server from a validated config. main.ts and the tests share it.
import { createServer, type Server } from 'node:http';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import type { Config } from './config.ts';
import { OrderNotifier, systemClock, type AppContext, type Clock } from './context.ts';
import type { Database } from './db/driver.ts';
import { SqliteDatabase } from './db/sqlite.ts';
import { PostgresDatabase } from './db/postgres.ts';
import { migrate } from './db/migrate.ts';
import { createLogger, type Logger } from './log.ts';
import { RateLimiter } from './http/rateLimit.ts';
import { OidcClient } from './auth/oidc.ts';
import { LocalBlobStore } from './blob/localBlobStore.ts';
import { S3BlobStore, defaultCredentialProvider } from './blob/s3BlobStore.ts';
import type { BlobStore } from './blob/blobStore.ts';
import { FakeProvider } from './payments/fake.ts';
import { StripeProvider } from './payments/stripe.ts';
import type { PaymentProvider } from './payments/provider.ts';
import { createRouter } from './app.ts';

export interface Overrides {
  db?: Database;
  clock?: Clock;
  log?: Logger;
  provider?: PaymentProvider;
  blob?: BlobStore;
  fetchImpl?: typeof fetch;
  env?: Record<string, string | undefined>;
}

export function openDatabase(config: Config): Database {
  if (config.database.kind === 'postgres') {
    return new PostgresDatabase(config.database.url, config.database.maxConnections);
  }
  if (config.database.path !== ':memory:') {
    mkdirSync(dirname(config.database.path), { recursive: true });
  }
  return new SqliteDatabase(config.database.path);
}

export async function buildContext(config: Config, overrides: Overrides = {}): Promise<AppContext> {
  const clock = overrides.clock ?? systemClock;
  const log = overrides.log ?? createLogger(config.logLevel);
  const db = overrides.db ?? openDatabase(config);
  if (config.migrateOnStart) {
    const applied = await migrate(db, undefined, clock.now());
    if (applied.length > 0) {
      log.info('migrations applied', { applied });
    }
  }
  const payments = config.payments;
  const provider =
    overrides.provider ??
    (payments.provider === 'stripe'
      ? new StripeProvider({
          secretKey: payments.secretKey,
          baseUrl: payments.apiBase,
          automaticTax: payments.automaticTax,
          platformCountry: payments.platformCountry,
          apiVersion: payments.apiVersion ?? undefined,
          fetchImpl: overrides.fetchImpl,
        })
      : new FakeProvider(config.publicBaseUrl));
  const blob =
    overrides.blob ??
    (config.blob.kind === 's3'
      ? new S3BlobStore({
          bucket: config.blob.bucket,
          region: config.blob.region,
          endpoint: config.blob.endpoint,
          prefix: config.blob.prefix,
          credentials: defaultCredentialProvider(overrides.env ?? process.env, () => clock.now(), overrides.fetchImpl),
          now: () => clock.now(),
          fetchImpl: overrides.fetchImpl,
        })
      : new LocalBlobStore(config.blob.dir, config.publicBaseUrl, config.blob.urlSecret, () => clock.now()));
  const oidc =
    config.auth.mode === 'oidc'
      ? new OidcClient({
          issuer: config.auth.issuer,
          clientId: config.auth.clientId,
          clientSecret: config.auth.clientSecret,
          redirectUri: `${config.publicBaseUrl}/auth/callback`,
          scopes: config.auth.scopes,
          now: () => clock.now(),
          fetchImpl: overrides.fetchImpl,
        })
      : null;
  return { config, db, clock, provider, blob, log, notifier: new OrderNotifier(), oidc, limiter: new RateLimiter(() => clock.now()) };
}

export function createHttpServer(ctx: AppContext): Server {
  const router = createRouter(ctx);
  const server = createServer({ requestTimeout: 300_000, headersTimeout: 30_000 }, (req, res) => {
    router.handle(req, res).catch((error: unknown) => {
      ctx.log.error('request handler crashed', { error });
      if (!res.headersSent) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
      }
      res.end('{"error":{"code":"Internal","message":"internal error"}}');
    });
  });
  // Behind the ALB (idle timeout 60 s): keep-alive longer than the balancer's, so the balancer always closes first.
  server.keepAliveTimeout = 65_000;
  return server;
}
