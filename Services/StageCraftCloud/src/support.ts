// Support tickets (ADR 0032 §7). Anonymous or signed in; every text field is redacted again on the server; the log tail is capped at
// 256 KiB and the screenshot must be a PNG of at most 4 MiB.
import { createHash, randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { BlobKeys } from './blob/blobStore.ts';
import { num, str, strOrNull, type Row } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { writeAudit } from './audit.ts';
import { isStrictBase64 } from './base64.ts';
import { maskEmail, redact } from './redaction.ts';

export const MaxLogTailBytes = 256 * 1024;
export const MaxScreenshotBytes = 4 * 1024 * 1024;
/** Base64 of the screenshot, the log tail (JSON-escaped) and the form fields. */
export const SupportBodyLimit = 8 * 1024 * 1024;
export const TICKET_STATUSES = ['open', 'in_progress', 'resolved', 'closed'] as const;
export type TicketStatus = (typeof TICKET_STATUSES)[number];

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function textField(body: Record<string, unknown>, name: string, max: number, required: boolean): string {
  const value = body[name];
  if (value === undefined || value === null || value === '') {
    if (required) {
      throw new ApiError('InvalidRequest', `${name} is required`);
    }
    return '';
  }
  if (typeof value !== 'string' || value.length > max) {
    throw new ApiError('InvalidRequest', `${name} must be text of at most ${max} characters`);
  }
  return redact(value.replace(/\r\n/g, '\n'));
}

export interface TicketInput {
  category: string;
  title: string;
  description: string;
  contactEmail: string | null;
  clientReportId: string | null;
  steps: string;
  expected: string;
  actual: string;
  appVersion: string;
  os: string;
  gpu: string;
  sceneStats: string;
  logTail: string;
  screenshot: Buffer | null;
}

export function parseTicket(body: Record<string, unknown>): TicketInput {
  const category = body.category;
  if (typeof category !== 'string' || !/^[A-Za-z][A-Za-z0-9_-]{0,31}$/.test(category)) {
    throw new ApiError('InvalidRequest', 'category must be a short name such as Bug, Crash or Purchase');
  }
  let sceneStats = '';
  if (body.sceneStats !== undefined && body.sceneStats !== null) {
    sceneStats = typeof body.sceneStats === 'string' ? body.sceneStats : JSON.stringify(body.sceneStats);
    if (sceneStats.length > 4000) {
      throw new ApiError('InvalidRequest', 'sceneStats is too long');
    }
    sceneStats = redact(sceneStats);
  }
  let logTail = '';
  if (body.logTail !== undefined && body.logTail !== null) {
    if (typeof body.logTail !== 'string' || Buffer.byteLength(body.logTail, 'utf8') > MaxLogTailBytes) {
      throw new ApiError('TooLarge', 'logTail must be text of at most 256 KiB');
    }
    logTail = redact(body.logTail);
  }
  let screenshot: Buffer | null = null;
  const encoded = body.screenshotPngBase64;
  if (encoded !== undefined && encoded !== null && encoded !== '') {
    if (typeof encoded !== 'string' || !isStrictBase64(encoded)) {
      throw new ApiError('InvalidRequest', 'screenshotPngBase64 must be standard base64');
    }
    if ((encoded.length / 4) * 3 > MaxScreenshotBytes + 2) {
      throw new ApiError('TooLarge', 'the screenshot must be at most 4 MiB');
    }
    screenshot = Buffer.from(encoded, 'base64');
    if (screenshot.length > MaxScreenshotBytes) {
      throw new ApiError('TooLarge', 'the screenshot must be at most 4 MiB');
    }
    if (screenshot.length <= PNG_SIGNATURE.length || !screenshot.subarray(0, 8).equals(PNG_SIGNATURE)) {
      throw new ApiError('InvalidRequest', 'the screenshot must be a PNG');
    }
  }
  let contactEmail: string | null = null;
  if (body.contactEmail !== undefined && body.contactEmail !== null && body.contactEmail !== '') {
    const email = body.contactEmail;
    if (typeof email !== 'string' || email.length > 254 || !/^[^\s@<>()",;:\\]+@[^\s@<>()",;:\\]+\.[A-Za-z]{2,}$/.test(email.trim())) {
      throw new ApiError('InvalidRequest', 'contactEmail is not a plausible e-mail address');
    }
    contactEmail = email.trim();
  }
  let clientReportId: string | null = null;
  if (body.clientReportId !== undefined && body.clientReportId !== null && body.clientReportId !== '') {
    const id = body.clientReportId;
    if (typeof id !== 'string' || !/^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/.test(id)) {
      throw new ApiError('InvalidRequest', 'clientReportId must be a hyphenated GUID');
    }
    clientReportId = id.toLowerCase();
  }
  return {
    category: category.toLowerCase(),
    title: textField(body, 'title', 200, true),
    description: textField(body, 'description', 8000, true),
    contactEmail,
    clientReportId,
    steps: textField(body, 'steps', 8000, false),
    expected: textField(body, 'expected', 8000, false),
    actual: textField(body, 'actual', 8000, false),
    appVersion: textField(body, 'appVersion', 100, false),
    os: textField(body, 'os', 200, false),
    gpu: textField(body, 'gpu', 200, false),
    sceneStats,
    logTail,
    screenshot,
  };
}

/** acct:<id>:<report> or ip:<sha256(ip)>:<report>; null without a clientReportId. */
export function dedupeKey(accountId: string | null, ip: string, clientReportId: string | null): string | null {
  if (!clientReportId) {
    return null;
  }
  return accountId ? `acct:${accountId}:${clientReportId}` : `ip:${createHash('sha256').update(`support:${ip}`).digest('hex')}:${clientReportId}`;
}

async function findByDedupeKey(ctx: AppContext, key: string | null): Promise<string | null> {
  if (!key) return null;
  const rows = await ctx.db.query('SELECT id FROM support_tickets WHERE dedupe_key = $1', [key]);
  return rows.length > 0 ? str(rows[0]!.id) : null;
}

/**
 * Creates a ticket. A resend with the same clientReportId (per account, or per IP hash when anonymous) returns the existing ticket
 * (created = false) instead of creating a second one.
 */
export async function createTicket(ctx: AppContext, accountId: string | null, ip: string, input: TicketInput): Promise<{ ticketId: string; created: boolean }> {
  const key = dedupeKey(accountId, ip, input.clientReportId);
  const existing = await findByDedupeKey(ctx, key);
  if (existing) {
    return { ticketId: existing, created: false };
  }
  const id = randomUUID();
  const now = ctx.clock.now();
  const screenshotKey = input.screenshot ? BlobKeys.screenshot(id) : null;
  if (input.screenshot && screenshotKey) {
    await ctx.blob.put(screenshotKey, input.screenshot, 'image/png');
  }
  const inserted = await ctx.db.query(
    `INSERT INTO support_tickets (id, account_id, category, title, description, contact_email, dedupe_key, steps, expected, actual, app_version,
       os, gpu, scene_stats, log_tail, screenshot_key, status, created_at, updated_at)
     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, 'open', $17, $17)
     ON CONFLICT (dedupe_key) DO NOTHING RETURNING id`,
    [id, accountId, input.category, input.title, input.description, input.contactEmail, key, input.steps, input.expected, input.actual,
      input.appVersion, input.os, input.gpu, input.sceneStats, input.logTail, screenshotKey, now],
  );
  if (inserted.length === 0) {
    // A concurrent resend won the race.
    if (screenshotKey) await ctx.blob.delete(screenshotKey);
    return { ticketId: (await findByDedupeKey(ctx, key))!, created: false };
  }
  ctx.log.info('support ticket created', {
    ticketId: id,
    account: accountId,
    category: input.category,
    hasScreenshot: screenshotKey !== null,
    contact: maskEmail(input.contactEmail),
  });
  return { ticketId: id, created: true };
}

export function ticketToJson(row: Row, full: boolean): Record<string, unknown> {
  const json: Record<string, unknown> = {
    ticketId: str(row.id),
    category: str(row.category),
    title: str(row.title),
    status: str(row.status),
    createdAt: new Date(num(row.created_at)).toISOString(),
    updatedAt: new Date(num(row.updated_at)).toISOString(),
  };
  if (full) {
    Object.assign(json, {
      accountId: strOrNull(row.account_id),
      description: str(row.description),
      contactEmail: strOrNull(row.contact_email),
      steps: str(row.steps),
      expected: str(row.expected),
      actual: str(row.actual),
      appVersion: str(row.app_version),
      os: str(row.os),
      gpu: str(row.gpu),
      sceneStats: str(row.scene_stats),
      logTail: str(row.log_tail),
      hasScreenshot: row.screenshot_key !== null,
      adminNote: strOrNull(row.admin_note),
    });
  }
  return json;
}

export async function listTickets(ctx: AppContext, filter: { accountId?: string; status?: string | null }): Promise<Row[]> {
  if (filter.accountId) {
    return ctx.db.query('SELECT * FROM support_tickets WHERE account_id = $1 ORDER BY created_at DESC, id LIMIT 200', [filter.accountId]);
  }
  if (filter.status) {
    if (!(TICKET_STATUSES as readonly string[]).includes(filter.status)) {
      throw new ApiError('InvalidRequest', `status must be one of ${TICKET_STATUSES.join(', ')}`);
    }
    return ctx.db.query('SELECT * FROM support_tickets WHERE status = $1 ORDER BY created_at DESC, id LIMIT 500', [filter.status]);
  }
  return ctx.db.query('SELECT * FROM support_tickets ORDER BY created_at DESC, id LIMIT 500');
}

export async function updateTicket(ctx: AppContext, adminId: string, ticketId: string, body: Record<string, unknown>): Promise<Row> {
  const status = body.status;
  if (typeof status !== 'string' || !(TICKET_STATUSES as readonly string[]).includes(status)) {
    throw new ApiError('InvalidRequest', `status must be one of ${TICKET_STATUSES.join(', ')}`);
  }
  const note = body.note === undefined || body.note === null ? null : body.note;
  if (note !== null && (typeof note !== 'string' || note.length > 2000)) {
    throw new ApiError('InvalidRequest', 'note must be text of at most 2000 characters');
  }
  const now = ctx.clock.now();
  return ctx.db.withTransaction(async (tx) => {
    const rows = await tx.query('SELECT * FROM support_tickets WHERE id = $1', [ticketId]);
    if (rows.length === 0) {
      throw new ApiError('NotFound', 'no such ticket');
    }
    const before = rows[0]!;
    await tx.query('UPDATE support_tickets SET status = $1, admin_note = COALESCE($2, admin_note), updated_at = $3 WHERE id = $4', [status, note, now, ticketId]);
    const after = (await tx.query('SELECT * FROM support_tickets WHERE id = $1', [ticketId]))[0]!;
    await writeAudit(tx, {
      actorId: adminId,
      action: 'support.update',
      target: `ticket:${ticketId}`,
      before: { status: before.status, adminNote: before.admin_note },
      after: { status: after.status, adminNote: after.admin_note },
      at: now,
    });
    return after;
  });
}
