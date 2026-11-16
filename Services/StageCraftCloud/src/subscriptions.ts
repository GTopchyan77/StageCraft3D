// StageCraft Pro subscriptions (ADR 0034 §17.2). The server reads the plan's Price from its own config, opens a Stripe Checkout in
// subscription mode and records it; the subscription's state is written only by verified webhooks (webhooks.ts), never by these calls.
import { randomUUID } from 'node:crypto';
import { ApiError } from './errors.ts';
import { num, str, strOrNull, type Queryable, type Row } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { ProviderFailure } from './payments/provider.ts';

export const PLANS = ['monthly', 'yearly'] as const;
export type Plan = (typeof PLANS)[number];

/**
 * Stripe statuses that grant Pro. past_due is Stripe's retry period after a failed renewal: a paying customer keeps Pro while the card
 * is retried, and loses it when Stripe gives up (unpaid or canceled). incomplete and the rest never grant it (ADR 0034 §17.2).
 */
const ENTITLED_STATUSES = new Set(['active', 'trialing', 'past_due']);

/**
 * How far along a subscription's life a status is, for two events stamped in the same second (Stripe's created is whole seconds): the
 * later stage wins, so "incomplete" delivered after "active" from the same checkout second cannot undo a payment.
 */
const STATUS_STAGE: Record<string, number> = { incomplete: 0, incomplete_expired: 1, trialing: 2, active: 2, past_due: 2, paused: 2, unpaid: 3, canceled: 4 };

function stageOf(status: string): number {
  return STATUS_STAGE[status] ?? 0;
}

const IDEMPOTENCY_KEY = /^[A-Za-z0-9_-]{8,128}$/;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

export function isPlan(value: unknown): value is Plan {
  return typeof value === 'string' && (PLANS as readonly string[]).includes(value);
}

export interface Subscription {
  providerSubscriptionId: string;
  accountId: string;
  customerId: string | null;
  plan: string;
  status: string;
  /** Epoch ms, or null when Stripe did not say. */
  currentPeriodEnd: number | null;
  cancelAtPeriodEnd: boolean;
  lastEventCreated: number;
  updatedAt: number;
}

export function isEntitled(subscription: Subscription | null): boolean {
  return subscription !== null && ENTITLED_STATUSES.has(subscription.status);
}

function rowToSubscription(row: Row): Subscription {
  return {
    providerSubscriptionId: str(row.provider_subscription_id),
    accountId: str(row.account_id),
    customerId: strOrNull(row.provider_customer_id),
    plan: str(row.plan),
    status: str(row.status),
    currentPeriodEnd: row.current_period_end === null || row.current_period_end === undefined ? null : num(row.current_period_end),
    cancelAtPeriodEnd: num(row.cancel_at_period_end) !== 0,
    lastEventCreated: num(row.last_event_created),
    updatedAt: num(row.updated_at),
  };
}

/** The account's subscription that grants Pro, else its most recently changed one, else null. */
export async function currentSubscription(q: Queryable, accountId: string): Promise<Subscription | null> {
  const rows = await q.query('SELECT * FROM subscriptions WHERE account_id = $1 ORDER BY updated_at DESC, provider_subscription_id LIMIT 50', [accountId]);
  const subscriptions = rows.map(rowToSubscription);
  return subscriptions.find(isEntitled) ?? subscriptions[0] ?? null;
}

export function subscriptionToJson(subscription: Subscription | null): Record<string, unknown> {
  return {
    status: subscription?.status ?? 'none',
    entitled: isEntitled(subscription),
    plan: subscription?.plan ?? null,
    currentPeriodEnd: subscription?.currentPeriodEnd ? new Date(subscription.currentPeriodEnd).toISOString() : null,
    cancelAtPeriodEnd: subscription?.cancelAtPeriodEnd ?? false,
  };
}

export function subscriptionNotifyKey(accountId: string): string {
  return `subscription:${accountId}`;
}

interface CheckoutRecord {
  id: string;
  accountId: string;
  plan: Plan;
  state: 'Created' | 'Open' | 'Completed' | 'Expired' | 'Failed';
  sessionId: string | null;
  url: string | null;
  expiresAt: number | null;
}

function rowToCheckout(row: Row): CheckoutRecord {
  return {
    id: str(row.id),
    accountId: str(row.account_id),
    plan: str(row.plan) as Plan,
    state: str(row.state) as CheckoutRecord['state'],
    sessionId: strOrNull(row.session_id),
    url: strOrNull(row.url),
    expiresAt: row.expires_at === null || row.expires_at === undefined ? null : num(row.expires_at),
  };
}

export function checkoutToJson(checkout: CheckoutRecord): Record<string, unknown> {
  return {
    checkoutId: checkout.id,
    plan: checkout.plan,
    state: checkout.state,
    url: checkout.state === 'Open' ? checkout.url : null,
    expiresAt: checkout.expiresAt ? new Date(checkout.expiresAt).toISOString() : null,
  };
}

async function getCheckout(q: Queryable, id: string): Promise<CheckoutRecord | null> {
  const rows = await q.query('SELECT * FROM subscription_checkouts WHERE id = $1', [id]);
  return rows.length > 0 ? rowToCheckout(rows[0]!) : null;
}

type Decision = { kind: 'existing'; checkout: CheckoutRecord } | { kind: 'open'; checkout: CheckoutRecord; customerId: string | null };

/**
 * POST /v1/subscriptions/checkout. Idempotent per account and key: a repeat returns the same checkout. An open checkout of the same
 * plan is returned rather than duplicated, and one of another plan refuses (PurchasePending), so one person cannot pay for two
 * subscriptions by clicking twice. Refuses an account that already has Pro (AlreadySubscribed) and a plan without a Price.
 */
export async function createSubscriptionCheckout(ctx: AppContext, accountId: string, plan: unknown, idempotencyKey: unknown): Promise<CheckoutRecord> {
  if (!isPlan(plan)) {
    throw new ApiError('InvalidRequest', `plan must be one of ${PLANS.join(', ')}`);
  }
  if (typeof idempotencyKey !== 'string' || !IDEMPOTENCY_KEY.test(idempotencyKey)) {
    throw new ApiError('InvalidRequest', 'idempotencyKey must be 8-128 letters, digits, - or _');
  }
  const priceId = ctx.config.subscriptions.prices[plan];
  if (!priceId) {
    throw new ApiError('PlanNotAvailable', 'this plan is not offered');
  }
  const now = ctx.clock.now();
  const decision = await ctx.db.withTransaction(async (tx): Promise<Decision> => {
    const repeatRows = await tx.query('SELECT * FROM subscription_checkouts WHERE account_id = $1 AND idempotency_key = $2', [accountId, idempotencyKey]);
    if (repeatRows.length > 0) {
      const repeat = rowToCheckout(repeatRows[0]!);
      if (repeat.plan !== plan) {
        throw new ApiError('IdempotencyConflict', 'this idempotencyKey was already used for another plan');
      }
      if (repeat.state !== 'Created') {
        return { kind: 'existing', checkout: repeat };
      }
      return { kind: 'open', checkout: repeat, customerId: (await currentSubscription(tx, accountId))?.customerId ?? null };
    }
    const subscription = await currentSubscription(tx, accountId);
    if (isEntitled(subscription)) {
      throw new ApiError('AlreadySubscribed', 'this account already has StageCraft Pro');
    }
    const openRows = await tx.query(
      "SELECT * FROM subscription_checkouts WHERE account_id = $1 AND state IN ('Created', 'Open') AND (expires_at IS NULL OR expires_at > $2) ORDER BY created_at DESC",
      [accountId, now],
    );
    if (openRows.length > 0) {
      const open = rowToCheckout(openRows[0]!);
      if (open.plan !== plan) {
        throw new ApiError('PurchasePending', 'another subscription checkout is open; finish it or wait for it to expire');
      }
      if (open.state === 'Open') {
        return { kind: 'existing', checkout: open };
      }
    }
    const id = randomUUID();
    await tx.query(
      `INSERT INTO subscription_checkouts (id, account_id, plan, idempotency_key, state, created_at, updated_at)
       VALUES ($1, $2, $3, $4, 'Created', $5, $5)`,
      [id, accountId, plan, idempotencyKey, now],
    );
    return { kind: 'open', checkout: (await getCheckout(tx, id))!, customerId: subscription?.customerId ?? null };
  });
  if (decision.kind === 'existing') {
    return decision.checkout;
  }
  return openCheckout(ctx, decision.checkout, priceId, decision.customerId);
}

async function openCheckout(ctx: AppContext, checkout: CheckoutRecord, priceId: string, customerId: string | null): Promise<CheckoutRecord> {
  try {
    const session = await ctx.provider.createSubscriptionCheckout({
      checkoutId: checkout.id,
      accountId: checkout.accountId,
      plan: checkout.plan,
      priceId,
      customerId,
      expiresAt: ctx.clock.now() + ctx.config.checkoutExpiresMinutes * 60_000,
      successUrl: `${ctx.config.publicBaseUrl}/subscription/done?checkout=${checkout.id}`,
      cancelUrl: `${ctx.config.checkoutCancelUrl}?checkout=${checkout.id}`,
    });
    await ctx.db.query(
      "UPDATE subscription_checkouts SET state = 'Open', session_id = $1, url = $2, expires_at = $3, updated_at = $4 WHERE id = $5 AND state = 'Created'",
      [session.sessionId, session.url, session.expiresAt, ctx.clock.now(), checkout.id],
    );
    return (await getCheckout(ctx.db, checkout.id))!;
  } catch (error) {
    if (!(error instanceof ProviderFailure)) {
      throw error;
    }
    ctx.log.warn('subscription checkout creation failed', { checkoutId: checkout.id, status: error.status, providerCode: error.providerCode });
    await ctx.db.query("UPDATE subscription_checkouts SET state = 'Failed', updated_at = $1 WHERE id = $2 AND state = 'Created'", [ctx.clock.now(), checkout.id]);
    throw new ApiError('ProviderError', 'the payment provider is not available; nothing was charged, try again');
  }
}

/** POST /v1/subscriptions/portal: a Stripe Billing Portal link for the account's customer (cancel, card, invoices). */
export async function createPortalLink(ctx: AppContext, accountId: string): Promise<{ url: string }> {
  const rows = await ctx.db.query(
    'SELECT provider_customer_id FROM subscriptions WHERE account_id = $1 AND provider_customer_id IS NOT NULL ORDER BY updated_at DESC LIMIT 1',
    [accountId],
  );
  const customerId = rows.length > 0 ? strOrNull(rows[0]!.provider_customer_id) : null;
  if (!customerId) {
    throw new ApiError('NoActiveSubscription', 'there is no subscription to manage');
  }
  try {
    return await ctx.provider.createBillingPortal({ customerId, returnUrl: ctx.config.subscriptions.portalReturnUrl });
  } catch (error) {
    if (!(error instanceof ProviderFailure)) {
      throw error;
    }
    ctx.log.warn('billing portal creation failed', { status: error.status, providerCode: error.providerCode });
    throw new ApiError('ProviderError', 'the payment provider is not available; try again');
  }
}

/** GET /v1/me/subscription?waitSeconds=n: answers when the account's subscription differs from the start, or after waitSeconds. */
export async function waitForSubscriptionChange(ctx: AppContext, accountId: string, waitSeconds: number): Promise<Record<string, unknown>> {
  const read = async (): Promise<Record<string, unknown>> => subscriptionToJson(await currentSubscription(ctx.db, accountId));
  const first = await read();
  const firstText = JSON.stringify(first);
  const deadline = ctx.clock.now() + waitSeconds * 1000;
  let current = first;
  while (JSON.stringify(current) === firstText) {
    const remaining = deadline - ctx.clock.now();
    if (remaining <= 0) {
      break;
    }
    // Woken by a webhook this process applied; the 2 s cap picks up one another replica applied.
    await ctx.notifier.wait(subscriptionNotifyKey(accountId), Math.min(remaining, 2000));
    current = await read();
  }
  return current;
}

// --- Webhooks (called inside handleStripeWebhook's transaction) ---

type StripeObject = Record<string, unknown>;

export interface SubscriptionWebhookResult {
  status: 'processed' | 'ignored' | 'needs_attention';
  detail: string;
  notifyKeys?: string[];
}

function text(object: StripeObject, field: string): string | null {
  const value = object[field];
  return typeof value === 'string' && value.length > 0 && value.length <= 255 ? value : null;
}

function idOf(object: StripeObject, field: string): string | null {
  const value = object[field];
  if (typeof value === 'string') return value.length <= 255 ? value : null;
  if (typeof value === 'object' && value !== null) return text(value as StripeObject, 'id');
  return null;
}

/** Seconds from Stripe as epoch ms, or null. */
function seconds(value: unknown): number | null {
  return typeof value === 'number' && Number.isSafeInteger(value) && value > 0 ? value * 1000 : null;
}

function firstItem(subscription: StripeObject): StripeObject | null {
  const items = subscription.items as StripeObject | undefined;
  const data = items?.data;
  return Array.isArray(data) && typeof data[0] === 'object' && data[0] !== null ? (data[0] as StripeObject) : null;
}

/** Newer Stripe API versions moved current_period_end from the subscription to its items; both are read. */
function periodEnd(subscription: StripeObject): number | null {
  return seconds(subscription.current_period_end) ?? seconds(firstItem(subscription)?.current_period_end);
}

/** The price decides (a plan switched in the Billing Portal keeps the checkout's metadata); the metadata only fills in. */
function planOf(ctx: AppContext, subscription: StripeObject): string {
  const price = firstItem(subscription)?.price;
  const priceId = typeof price === 'object' && price !== null ? text(price as StripeObject, 'id') : typeof price === 'string' ? price : null;
  const match = PLANS.find((plan) => priceId !== null && ctx.config.subscriptions.prices[plan] === priceId);
  if (match) {
    return match;
  }
  const fromMetadata = text((subscription.metadata ?? {}) as StripeObject, 'plan');
  return isPlan(fromMetadata) ? fromMetadata : 'unknown';
}

export function isSubscriptionCheckout(session: StripeObject): boolean {
  return text(session, 'mode') === 'subscription';
}

async function checkoutForSession(tx: Queryable, session: StripeObject): Promise<CheckoutRecord | null> {
  const metadata = (session.metadata ?? {}) as StripeObject;
  const checkoutId = text(session, 'client_reference_id') ?? text(metadata, 'checkoutId');
  if (checkoutId && UUID.test(checkoutId)) {
    const checkout = await getCheckout(tx, checkoutId);
    if (checkout) return checkout;
  }
  const sessionId = text(session, 'id');
  if (!sessionId) return null;
  const rows = await tx.query('SELECT * FROM subscription_checkouts WHERE session_id = $1', [sessionId]);
  return rows.length > 0 ? rowToCheckout(rows[0]!) : null;
}

/** checkout.session.completed / .expired in subscription mode: closes the checkout record. Pro itself comes from subscription events. */
export async function onSubscriptionCheckout(
  tx: Queryable,
  session: StripeObject,
  next: 'Completed' | 'Expired' | 'Failed',
  at: number,
): Promise<SubscriptionWebhookResult> {
  const checkout = await checkoutForSession(tx, session);
  if (!checkout) {
    return { status: 'needs_attention', detail: 'subscription checkout for an unknown checkout' };
  }
  await tx.query("UPDATE subscription_checkouts SET state = $1, updated_at = $2 WHERE id = $3 AND state IN ('Created', 'Open')", [next, at, checkout.id]);
  return { status: 'processed', detail: `subscription checkout ${next.toLowerCase()}`, notifyKeys: [subscriptionNotifyKey(checkout.accountId)] };
}

/**
 * customer.subscription.created / .updated / .deleted: writes the subscription row from the event's snapshot. An event older than
 * the one that last wrote the row is ignored, because Stripe does not deliver events in order (Rules.md §52).
 */
export async function onSubscriptionEvent(
  ctx: AppContext,
  tx: Queryable,
  subscription: StripeObject,
  eventCreated: number,
  deleted: boolean,
  at: number,
): Promise<SubscriptionWebhookResult> {
  const id = text(subscription, 'id');
  const metadata = (subscription.metadata ?? {}) as StripeObject;
  const accountId = text(metadata, 'accountId');
  if (!id || !accountId || !UUID.test(accountId)) {
    return { status: 'needs_attention', detail: 'subscription event without a subscription id or StageCraft account' };
  }
  const account = await tx.query('SELECT id FROM accounts WHERE id = $1', [accountId]);
  if (account.length === 0) {
    return { status: 'needs_attention', detail: 'subscription event for an unknown account' };
  }
  const status = deleted ? 'canceled' : text(subscription, 'status');
  if (!status) {
    return { status: 'needs_attention', detail: 'subscription event without a status' };
  }
  const existing = await tx.query('SELECT account_id, status, last_event_created FROM subscriptions WHERE provider_subscription_id = $1', [id]);
  if (existing.length > 0 && str(existing[0]!.account_id) !== accountId) {
    return { status: 'needs_attention', detail: 'subscription event names another account than the one recorded' };
  }
  if (existing.length > 0) {
    const appliedAt = num(existing[0]!.last_event_created);
    const appliedStatus = str(existing[0]!.status);
    // A canceled Stripe subscription never comes back; a new checkout makes a new one.
    if (appliedStatus === 'canceled') {
      return { status: 'ignored', detail: 'the subscription is already canceled' };
    }
    if (appliedAt > eventCreated || (appliedAt === eventCreated && stageOf(status) < stageOf(appliedStatus))) {
      return { status: 'ignored', detail: 'older than the subscription state already applied' };
    }
  }
  await tx.query(
    `INSERT INTO subscriptions (provider_subscription_id, account_id, provider_customer_id, plan, status, current_period_end,
       cancel_at_period_end, last_event_created, created_at, updated_at)
     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $9)
     ON CONFLICT (provider_subscription_id) DO UPDATE SET provider_customer_id = COALESCE(excluded.provider_customer_id, subscriptions.provider_customer_id),
       plan = excluded.plan, status = excluded.status, current_period_end = excluded.current_period_end,
       cancel_at_period_end = excluded.cancel_at_period_end, last_event_created = excluded.last_event_created, updated_at = excluded.updated_at`,
    [
      id,
      accountId,
      idOf(subscription, 'customer'),
      planOf(ctx, subscription),
      status,
      periodEnd(subscription),
      subscription.cancel_at_period_end === true ? 1 : 0,
      eventCreated,
      at,
    ],
  );
  return { status: 'processed', detail: `subscription ${status}`, notifyKeys: [subscriptionNotifyKey(accountId)] };
}
