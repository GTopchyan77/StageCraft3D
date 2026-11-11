// Shared test fixtures: config, clock, database (SQLite in memory, or Postgres when TEST_DATABASE_URL is set), an HTTP server on an
// ephemeral port, a .stage writer that mirrors StagePackage::Write, and signed webhook deliveries.
import { createHash, randomUUID } from 'node:crypto';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { deflateSync } from 'node:zlib';
import type { AddressInfo } from 'node:net';
import { loadConfig, type Config } from '../src/config.ts';
import { buildContext, createHttpServer, type Overrides } from '../src/bootstrap.ts';
import type { AppContext, Clock } from '../src/context.ts';
import type { Database } from '../src/db/driver.ts';
import { SqliteDatabase } from '../src/db/sqlite.ts';
import { PostgresDatabase } from '../src/db/postgres.ts';
import { migrate } from '../src/db/migrate.ts';
import { createMemoryLogger } from '../src/log.ts';
import { FakeProvider } from '../src/payments/fake.ts';
import { signStripePayload } from '../src/payments/webhookSignature.ts';
import { grantRole, upsertAccount, createSession } from '../src/auth/sessions.ts';
import { MAGIC, makeTag, CODEC_STORED, CODEC_ZLIB } from '../src/package/stagePackage.ts';

export const WEBHOOK_SECRET = 'whsec_test_secret_for_unit_tests';

export class TestClock implements Clock {
  current: number;
  constructor(start = Date.UTC(2026, 10, 7, 12, 0, 0)) {
    this.current = start;
  }
  now(): number {
    return this.current;
  }
  advance(ms: number): void {
    this.current += ms;
  }
}

export function testEnv(extra: Record<string, string> = {}): Record<string, string> {
  return {
    NODE_ENV: 'test',
    PUBLIC_BASE_URL: 'http://cloud.test',
    SQLITE_PATH: ':memory:',
    BLOB_LOCAL_DIR: mkdtempSync(join(tmpdir(), 'scc-blobs-')),
    BLOB_URL_SECRET: 'test-blob-url-secret-0123456789',
    STRIPE_WEBHOOK_SECRETS: WEBHOOK_SECRET,
    LOG_LEVEL: 'debug',
    MIGRATE_ON_START: 'true',
    ...extra,
  };
}

/** A fresh, migrated database: in-memory SQLite, or a dropped-and-recreated schema on TEST_DATABASE_URL. */
export async function freshDatabase(now = Date.now()): Promise<Database> {
  const url = process.env.TEST_DATABASE_URL;
  if (url) {
    const db = new PostgresDatabase(url, 4);
    await db.execScript('DROP SCHEMA public CASCADE; CREATE SCHEMA public;');
    await migrate(db, undefined, now);
    return db;
  }
  const db = new SqliteDatabase(':memory:');
  await migrate(db, undefined, now);
  return db;
}

export interface Harness {
  ctx: AppContext;
  config: Config;
  clock: TestClock;
  provider: FakeProvider;
  log: ReturnType<typeof createMemoryLogger>;
  close(): Promise<void>;
}

export async function harness(env: Record<string, string> = {}, overrides: Overrides = {}): Promise<Harness> {
  const config = loadConfig(testEnv({ ...env, MIGRATE_ON_START: 'false' }));
  const clock = new TestClock();
  const log = createMemoryLogger();
  const provider = new FakeProvider(config.publicBaseUrl);
  const db = overrides.db ?? (await freshDatabase(clock.now()));
  const ctx = await buildContext(config, { clock, log, provider, db, ...overrides });
  return { ctx, config, clock, provider, log, close: () => ctx.db.close() };
}

export interface Server {
  url: string;
  close(): Promise<void>;
}

export async function serve(ctx: AppContext): Promise<Server> {
  const server = createHttpServer(ctx);
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address() as AddressInfo;
  return {
    url: `http://127.0.0.1:${port}`,
    close: () =>
      new Promise((resolve) => {
        server.closeAllConnections();
        server.close(() => resolve());
      }),
  };
}

/** Creates an account and a session directly (unit tests); the e2e test signs in through the device flow instead. */
export async function makeUser(ctx: AppContext, name: string, roles: 'admin'[] = []): Promise<{ accountId: string; token: string }> {
  const now = ctx.clock.now();
  const account = await ctx.db.withTransaction((tx) => upsertAccount(tx, { issuer: 'test', subject: name, email: `${name}@example.com`, displayName: name }, now));
  for (const role of roles) {
    await grantRole(ctx.db, account.id, role, now);
  }
  const session = await ctx.db.withTransaction((tx) => createSession(tx, account.id, now));
  return { accountId: account.id, token: session.token };
}

export async function enableSeller(ctx: AppContext, accountId: string, method: 'stripe' | 'manual' = 'stripe', country = 'US'): Promise<void> {
  const now = ctx.clock.now();
  await ctx.db.query(
    `INSERT INTO sellers (account_id, country, payout_method, provider_account_id, payouts_enabled, created_at, updated_at)
     VALUES ($1, $2, $3, $4, 1, $5, $5)`,
    [accountId, country, method, method === 'stripe' ? `acct_${accountId.replace(/-/g, '').slice(0, 12)}` : null, now],
  );
}

// ---------- .stage writer mirroring StagePackage::Write ----------

export interface ManifestFields {
  format?: unknown;
  version?: unknown;
  id?: unknown;
  title?: unknown;
  description?: unknown;
  author?: unknown;
  kind?: unknown;
  category?: unknown;
  keywords?: unknown;
  price?: unknown;
  createdAt?: unknown;
  sceneVersion?: unknown;
  stats?: unknown;
  requiredItems?: unknown;
  allowExport?: unknown;
  [extra: string]: unknown;
}

export function sceneItem(path = '/Game/StageCraft/Items/DA_Truss.DA_Truss', id: string = randomUUID().toUpperCase()): Record<string, unknown> {
  return { id, item: path, label: 'Item', location: [0, 0, 0], rotation: [0, 0, 0, 1], scale: [1, 1, 1], state: 'AAAA' };
}

export function sceneJson(items: unknown[] = [sceneItem()], extra: Record<string, unknown> = {}): Record<string, unknown> {
  return { format: 'StageCraftScene', version: 12, name: 'Test', level: 'L_Test', savedAt: '2026-11-01T00:00:00.000Z', items, cues: [], ...extra };
}

export function manifestJson(fields: ManifestFields = {}, scene: Record<string, unknown> = sceneJson()): ManifestFields {
  const items = Array.isArray(scene.items) ? (scene.items as Record<string, unknown>[]) : [];
  const required = [...new Set(items.map((item) => item?.item as string))].sort();
  return {
    format: 'StageCraftPackage',
    version: 2,
    id: randomUUID().toUpperCase(),
    title: 'Arena Rig',
    description: 'A rig',
    author: 'Someone',
    kind: 'StageDesign',
    category: 'StageCraft.Marketplace.Category.Concert',
    keywords: ['arena'],
    price: { credits: 0, realMoney: { amount: 0, currency: '' } },
    createdAt: '2026-11-02T13:43:11.360Z',
    sceneVersion: 12,
    stats: { items: items.length, fixtures: 0, cues: Array.isArray(scene.cues) ? scene.cues.length : 0 },
    requiredItems: required,
    allowExport: false,
    ...fields,
  };
}

export interface SectionSpec {
  tag: string | number;
  raw: Buffer;
  codec?: number;
  /** Override the stored bytes (hostile tests). */
  stored?: Buffer;
  rawSize?: bigint;
  storedSize?: bigint;
  hash?: Buffer;
}

/** Zlib when it saves space, stored otherwise, exactly like StagePackageFormat::Compress. */
export function compress(raw: Buffer): { codec: number; stored: Buffer } {
  if (raw.length > 0) {
    const deflated = deflateSync(raw);
    if (deflated.length < raw.length) {
      return { codec: CODEC_ZLIB, stored: deflated };
    }
  }
  return { codec: CODEC_STORED, stored: raw };
}

export function writeContainer(sections: SectionSpec[], options: { magic?: Buffer; version?: number; count?: number; trailing?: Buffer } = {}): Buffer {
  const headers: Buffer[] = [];
  const payloads: Buffer[] = [];
  for (const section of sections) {
    const packed = section.stored !== undefined ? { codec: section.codec ?? CODEC_STORED, stored: section.stored } : compress(section.raw);
    const codec = section.codec ?? packed.codec;
    const header = Buffer.alloc(44);
    header.writeUInt32LE(typeof section.tag === 'number' ? section.tag : makeTag(section.tag), 0);
    header.writeUInt32LE(codec, 4);
    header.writeBigUInt64LE(section.storedSize ?? BigInt(packed.stored.length), 8);
    header.writeBigUInt64LE(section.rawSize ?? BigInt(section.raw.length), 16);
    (section.hash ?? createHash('sha1').update(section.raw).digest()).copy(header, 24);
    headers.push(header);
    payloads.push(packed.stored);
  }
  const fixed = Buffer.alloc(16);
  (options.magic ?? MAGIC).copy(fixed, 0);
  fixed.writeUInt32LE(options.version ?? 1, 8);
  fixed.writeUInt32LE(options.count ?? sections.length, 12);
  return Buffer.concat([fixed, ...headers, ...payloads, options.trailing ?? Buffer.alloc(0)]);
}

export const PNG_1x1 = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==',
  'base64',
);

export function writePackage(
  options: {
    manifest?: ManifestFields;
    scene?: Record<string, unknown>;
    thumbnail?: Buffer | null;
    manifestRaw?: string;
    sceneRaw?: string;
    /** Raw FIXT payload (see fixtureSection), written as its own section when given. */
    fixtures?: Buffer;
  } = {},
): { bytes: Buffer; id: string } {
  const scene = options.scene ?? sceneJson();
  const manifest = options.manifest ?? manifestJson({}, scene);
  const sections: SectionSpec[] = [
    { tag: 'MNFT', raw: Buffer.from(options.manifestRaw ?? JSON.stringify(manifest), 'utf8') },
    { tag: 'SCEN', raw: Buffer.from(options.sceneRaw ?? JSON.stringify(scene, null, '\t'), 'utf8') },
  ];
  const thumbnail = options.thumbnail === undefined ? PNG_1x1 : options.thumbnail;
  if (thumbnail) {
    sections.push({ tag: 'THMB', raw: thumbnail });
  }
  if (options.fixtures) {
    sections.push({ tag: 'FIXT', raw: options.fixtures });
  }
  return { bytes: writeContainer(sections), id: String(manifest.id) };
}

// ---------- device-library fixtures (ADR 0033 §9) ----------

/** The four u32 words (FGuid A, B, C, D) of canonical GUID text. */
export function guidWords(id: string): [number, number, number, number] {
  const hex = id.replace(/-/g, '');
  return [0, 8, 16, 24].map((at) => parseInt(hex.slice(at, at + 8), 16)) as [number, number, number, number];
}

/** A library mode's item path: /Temp/StageCraftFixtures/<FGuid Digits>/<slug>.<slug>. */
export function fixturePath(typeId: string, slug = 'Mode_1'): string {
  return `/Temp/StageCraftFixtures/${typeId.replace(/-/g, '')}/${slug}.${slug}`;
}

/** A v12 item of a library mode with its "fixtureType" reference, as FStageSceneStore::Serialize writes it. */
export function fixtureItem(typeId: string, slug = 'Mode_1'): Record<string, unknown> {
  return { ...sceneItem(fixturePath(typeId, slug)), fixtureType: { id: typeId, manufacturer: 'StageCraft', name: 'Test Spot', mode: slug } };
}

/** Bytes that pass the reader's .gdtf check: the ZIP local header magic, padded to size. */
export function gdtfBytes(size = 64, fill = 0x41): Buffer {
  const bytes = Buffer.alloc(size, fill);
  Buffer.from([0x50, 0x4b, 0x03, 0x04]).copy(bytes, 0, 0, Math.min(4, size));
  return bytes;
}

export interface FixtureEntrySpec {
  id: string | [number, number, number, number];
  /** File bytes; when size is also given only the header claims it (hostile tests). */
  data?: Buffer;
  size?: number;
}

/** A raw FIXT payload: u32 count, then per entry four u32 id words, a u32 size and the bytes, all little-endian. */
export function fixtureSection(entries: FixtureEntrySpec[], options: { count?: number; trailing?: Buffer } = {}): Buffer {
  const parts: Buffer[] = [];
  const count = Buffer.alloc(4);
  count.writeUInt32LE(options.count ?? entries.length, 0);
  parts.push(count);
  for (const entry of entries) {
    const data = entry.data ?? gdtfBytes();
    const header = Buffer.alloc(20);
    const words = typeof entry.id === 'string' ? guidWords(entry.id) : entry.id;
    words.forEach((word, index) => header.writeUInt32LE(word >>> 0, index * 4));
    header.writeUInt32LE(entry.size ?? data.length, 16);
    parts.push(header, data);
  }
  parts.push(options.trailing ?? Buffer.alloc(0));
  return Buffer.concat(parts);
}

// ---------- webhooks ----------

export function stripeEvent(type: string, object: Record<string, unknown>, id = `evt_${randomUUID().replace(/-/g, '')}`): Buffer {
  return Buffer.from(JSON.stringify({ id, object: 'event', type, data: { object } }), 'utf8');
}

export function signature(raw: Buffer, clock: Clock, secret = WEBHOOK_SECRET): string {
  return signStripePayload(raw, secret, Math.floor(clock.now() / 1000));
}

export function completedSession(order: { id: string; amountMinor: bigint; currency: string; checkoutSessionId: string | null }, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: order.checkoutSessionId,
    object: 'checkout.session',
    client_reference_id: order.id,
    metadata: { orderId: order.id },
    payment_status: 'paid',
    amount_total: Number(order.amountMinor),
    currency: order.currency.toLowerCase(),
    payment_intent: `pi_${order.id.replace(/-/g, '')}`,
    ...extra,
  };
}

export async function api(
  server: Server,
  method: string,
  path: string,
  options: { token?: string; json?: unknown; body?: Buffer | string; headers?: Record<string, string>; redirect?: 'manual' } = {},
): Promise<{ status: number; json: any; headers: Headers; text: string }> {
  const headers: Record<string, string> = { ...(options.headers ?? {}) };
  let body: Buffer | string | undefined;
  if (options.token) headers.Authorization = `Bearer ${options.token}`;
  if (options.json !== undefined) {
    headers['Content-Type'] ??= 'application/json';
    body = JSON.stringify(options.json);
  } else if (options.body !== undefined) {
    body = options.body;
  }
  const response = await fetch(`${server.url}${path}`, { method, headers, body, redirect: options.redirect ?? 'manual' });
  const text = await response.text();
  let json: any = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    json = null;
  }
  return { status: response.status, json, headers: response.headers, text };
}
