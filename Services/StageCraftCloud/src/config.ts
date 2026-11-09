// Configuration from environment variables, validated at startup. Every problem is reported at once, then the process refuses
// to start. Secrets are kept in the config object but never logged (see describeConfig).
import { isCurrencyCode } from './money.ts';

export interface Config {
  nodeEnv: 'production' | 'development' | 'test';
  port: number;
  publicBaseUrl: string;
  logLevel: 'debug' | 'info' | 'warn' | 'error';
  database: { kind: 'postgres'; url: string; maxConnections: number } | { kind: 'sqlite'; path: string };
  migrateOnStart: boolean;
  trustProxyHops: number;
  auth:
    | { mode: 'dev' }
    | { mode: 'oidc'; issuer: string; clientId: string; clientSecret: string | null; scopes: string };
  payments:
    | { provider: 'fake'; webhookSecrets: string[] }
    | {
        provider: 'stripe';
        secretKey: string;
        webhookSecrets: string[];
        apiBase: string;
        apiVersion: string | null;
        automaticTax: boolean;
        platformCountry: string;
      };
  checkoutSuccessUrl: string;
  checkoutCancelUrl: string;
  checkoutExpiresMinutes: number;
  settlementCurrencies: string[];
  sellerShareBps: bigint;
  payoutHoldDays: number;
  payoutMinimumMinor: bigint;
  disputeFeeFallbackMinor: bigint;
  blob:
    | { kind: 'local'; dir: string; urlSecret: string }
    | { kind: 's3'; bucket: string; region: string; endpoint: string | null; prefix: string };
  downloadUrlTtlSeconds: number;
}

export class ConfigError extends Error {
  readonly problems: string[];
  constructor(problems: string[]) {
    super(`invalid configuration:\n  - ${problems.join('\n  - ')}`);
    this.name = 'ConfigError';
    this.problems = problems;
  }
}

type Env = Record<string, string | undefined>;

export function loadConfig(env: Env): Config {
  const problems: string[] = [];
  const get = (name: string): string | undefined => {
    const value = env[name];
    return value === undefined || value.trim() === '' ? undefined : value.trim();
  };
  const required = (name: string): string => {
    const value = get(name);
    if (value === undefined) {
      problems.push(`${name} is required`);
      return '';
    }
    return value;
  };
  const integer = (name: string, fallback: number, min: number, max: number): number => {
    const raw = get(name);
    if (raw === undefined) {
      return fallback;
    }
    if (!/^-?[0-9]+$/.test(raw) || Number(raw) < min || Number(raw) > max) {
      problems.push(`${name} must be an integer in ${min}..${max}`);
      return fallback;
    }
    return Number(raw);
  };
  const flag = (name: string, fallback: boolean): boolean => {
    const raw = get(name);
    if (raw === undefined) {
      return fallback;
    }
    if (raw !== 'true' && raw !== 'false') {
      problems.push(`${name} must be true or false`);
      return fallback;
    }
    return raw === 'true';
  };
  const url = (name: string, fallback?: string): string => {
    const raw = get(name) ?? fallback;
    if (raw === undefined) {
      problems.push(`${name} is required`);
      return '';
    }
    try {
      const parsed = new URL(raw);
      if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
        throw new Error('scheme');
      }
    } catch {
      problems.push(`${name} must be an http(s) URL`);
    }
    return raw.replace(/\/+$/, '');
  };
  const oneOf = <T extends string>(name: string, allowed: readonly T[], fallback: T): T => {
    const raw = get(name) ?? fallback;
    if (!(allowed as readonly string[]).includes(raw)) {
      problems.push(`${name} must be one of ${allowed.join(', ')}`);
      return fallback;
    }
    return raw as T;
  };

  const nodeEnv = oneOf('NODE_ENV', ['production', 'development', 'test'] as const, 'development');
  const production = nodeEnv === 'production';
  const publicBaseUrl = url('PUBLIC_BASE_URL', production ? undefined : 'http://localhost:8080');
  if (production && publicBaseUrl.startsWith('http:')) {
    problems.push('PUBLIC_BASE_URL must be https in production');
  }

  const databaseUrl = get('DATABASE_URL');
  const database: Config['database'] = databaseUrl
    ? { kind: 'postgres', url: databaseUrl, maxConnections: integer('DATABASE_MAX_CONNECTIONS', 10, 1, 100) }
    : { kind: 'sqlite', path: get('SQLITE_PATH') ?? './data/stagecraft-cloud.sqlite' };
  if (production && database.kind !== 'postgres') {
    problems.push('DATABASE_URL (Postgres) is required in production');
  }

  const authMode = oneOf('AUTH_MODE', ['oidc', 'dev'] as const, production ? 'oidc' : 'dev');
  if (authMode === 'dev' && production) {
    problems.push('AUTH_MODE=dev is refused when NODE_ENV=production');
  }
  const auth: Config['auth'] =
    authMode === 'dev'
      ? { mode: 'dev' }
      : {
          mode: 'oidc',
          issuer: url('OIDC_ISSUER'),
          clientId: required('OIDC_CLIENT_ID'),
          clientSecret: get('OIDC_CLIENT_SECRET') ?? null,
          scopes: get('OIDC_SCOPES') ?? 'openid email profile',
        };

  const providerName = oneOf('PAYMENT_PROVIDER', ['stripe', 'fake'] as const, production ? 'stripe' : 'fake');
  if (providerName === 'fake' && production) {
    problems.push('PAYMENT_PROVIDER=fake is refused when NODE_ENV=production');
  }
  const webhookSecrets = (get('STRIPE_WEBHOOK_SECRETS') ?? (production ? '' : 'whsec_dev_only_secret'))
    .split(',')
    .map((secret) => secret.trim())
    .filter((secret) => secret.length > 0);
  if (webhookSecrets.length === 0) {
    problems.push('STRIPE_WEBHOOK_SECRETS is required');
  }
  let payments: Config['payments'];
  if (providerName === 'stripe') {
    const secretKey = required('STRIPE_SECRET_KEY');
    if (secretKey && !/^(sk|rk)_(live|test)_/.test(secretKey)) {
      problems.push('STRIPE_SECRET_KEY does not look like a Stripe secret or restricted key');
    }
    const platformCountry = get('STRIPE_PLATFORM_COUNTRY') ?? 'US';
    if (!/^[A-Z]{2}$/.test(platformCountry)) {
      problems.push('STRIPE_PLATFORM_COUNTRY must be a two-letter country code');
    }
    payments = {
      provider: 'stripe',
      secretKey,
      webhookSecrets,
      apiBase: url('STRIPE_API_BASE', 'https://api.stripe.com'),
      apiVersion: get('STRIPE_API_VERSION') ?? null,
      automaticTax: flag('STRIPE_AUTOMATIC_TAX', false),
      platformCountry,
    };
  } else {
    payments = { provider: 'fake', webhookSecrets };
  }

  const settlementCurrencies = (get('SETTLEMENT_CURRENCIES') ?? 'USD').split(',').map((code) => code.trim().toUpperCase());
  if (!settlementCurrencies.every(isCurrencyCode)) {
    problems.push('SETTLEMENT_CURRENCIES must be comma-separated ISO 4217 codes');
  }

  const blobKind = oneOf('BLOB_STORE', ['local', 's3'] as const, production ? 's3' : 'local');
  let blob: Config['blob'];
  // Download URLs go to clients without a session; in production they must be https.
  if (production && get('S3_ENDPOINT')?.startsWith('http:')) {
    problems.push('S3_ENDPOINT must be https in production');
  }
  if (blobKind === 's3') {
    blob = {
      kind: 's3',
      bucket: required('S3_BUCKET'),
      region: required('S3_REGION'),
      endpoint: get('S3_ENDPOINT') ? url('S3_ENDPOINT') : null,
      prefix: get('S3_PREFIX') ?? '',
    };
  } else {
    const urlSecret = get('BLOB_URL_SECRET') ?? (production ? '' : 'dev-only-blob-url-secret');
    if (production || urlSecret.length < 16) {
      if (production) problems.push('BLOB_STORE=local is refused when NODE_ENV=production');
      else problems.push('BLOB_URL_SECRET must be at least 16 characters');
    }
    blob = { kind: 'local', dir: get('BLOB_LOCAL_DIR') ?? './data/blobs', urlSecret };
  }

  const config: Config = {
    nodeEnv,
    port: integer('PORT', 8080, 1, 65535),
    publicBaseUrl,
    logLevel: oneOf('LOG_LEVEL', ['debug', 'info', 'warn', 'error'] as const, 'info'),
    database,
    migrateOnStart: flag('MIGRATE_ON_START', !production),
    trustProxyHops: integer('TRUST_PROXY_HOPS', 0, 0, 5),
    auth,
    payments,
    checkoutSuccessUrl: url('CHECKOUT_SUCCESS_URL', `${publicBaseUrl}/checkout/success`),
    checkoutCancelUrl: url('CHECKOUT_CANCEL_URL', `${publicBaseUrl}/checkout/cancel`),
    // Stripe accepts 30 minutes to 24 hours; the client waits for the checkout's lifetime plus one minute (ADR 0032 §6).
    checkoutExpiresMinutes: integer('CHECKOUT_EXPIRES_MINUTES', 30, 30, 1440),
    settlementCurrencies,
    sellerShareBps: BigInt(integer('SELLER_SHARE_BPS', 8500, 0, 10000)),
    payoutHoldDays: integer('PAYOUT_HOLD_DAYS', 14, 0, 365),
    payoutMinimumMinor: BigInt(integer('PAYOUT_MINIMUM_MINOR', 100, 1, 100_000_000)),
    disputeFeeFallbackMinor: BigInt(integer('DISPUTE_FEE_FALLBACK_MINOR', 1500, 0, 100_000)),
    blob,
    downloadUrlTtlSeconds: integer('DOWNLOAD_URL_TTL_SECONDS', 300, 30, 3600),
  };
  if (problems.length > 0) {
    throw new ConfigError(problems);
  }
  return config;
}

/** What the startup log may say about the configuration: no secrets, no connection strings. */
export function describeConfig(config: Config): Record<string, unknown> {
  return {
    nodeEnv: config.nodeEnv,
    port: config.port,
    publicBaseUrl: config.publicBaseUrl,
    database: config.database.kind,
    authMode: config.auth.mode,
    paymentProvider: config.payments.provider,
    blobStore: config.blob.kind,
    settlementCurrencies: config.settlementCurrencies,
    sellerShareBps: Number(config.sellerShareBps),
    payoutHoldDays: config.payoutHoldDays,
  };
}
