// Order states and their transitions (ADR 0032 §3.4). The transition function is pure; persistence applies it with a compare-and-set
// on the current state, so a transition decided on a stale read can never be forced.
import { randomUUID } from 'node:crypto';
import { big, num, str, strOrNull, type Queryable, type Row } from './db/driver.ts';

export const ORDER_STATES = ['Created', 'CheckoutOpen', 'Paid', 'PartiallyRefunded', 'Refunded', 'Expired', 'Failed', 'Disputed', 'OnHold'] as const;
export type OrderState = (typeof ORDER_STATES)[number];

export type OrderEvent =
  | 'checkout_opened'
  | 'checkout_failed'
  | 'payment_succeeded'
  | 'payment_failed'
  | 'checkout_expired'
  | 'amount_mismatch'
  | 'refunded_partially'
  | 'refunded_fully'
  | 'dispute_opened'
  | 'dispute_won'
  | 'dispute_lost_partially'
  | 'dispute_lost_fully';

export interface TransitionContext {
  /** Whether the order had been partially refunded before the dispute (a won dispute returns it to that state). */
  refundedBefore?: boolean;
}

const TABLE: Record<OrderState, Partial<Record<OrderEvent, OrderState | ((context: TransitionContext) => OrderState)>>> = {
  Created: { checkout_opened: 'CheckoutOpen', checkout_failed: 'Failed' },
  CheckoutOpen: { payment_succeeded: 'Paid', payment_failed: 'Failed', checkout_expired: 'Expired', amount_mismatch: 'OnHold' },
  Paid: { refunded_partially: 'PartiallyRefunded', refunded_fully: 'Refunded', dispute_opened: 'Disputed' },
  PartiallyRefunded: { refunded_partially: 'PartiallyRefunded', refunded_fully: 'Refunded', dispute_opened: 'Disputed' },
  Disputed: {
    dispute_won: (context) => (context.refundedBefore ? 'PartiallyRefunded' : 'Paid'),
    dispute_lost_partially: 'PartiallyRefunded',
    dispute_lost_fully: 'Refunded',
  },
  // A held order's money is returned in full by an admin refund; nothing was ever posted or granted for it.
  OnHold: { refunded_fully: 'Refunded' },
  Refunded: {},
  Expired: {},
  Failed: {},
};

/** The next state, or null when the event is illegal in this state (refused and logged, never forced). */
export function transition(state: OrderState, event: OrderEvent, context: TransitionContext = {}): OrderState | null {
  const next = TABLE[state]?.[event];
  if (next === undefined) {
    return null;
  }
  return typeof next === 'function' ? next(context) : next;
}

export function isOrderState(value: string): value is OrderState {
  return (ORDER_STATES as readonly string[]).includes(value);
}

export interface Order {
  id: string;
  buyerId: string;
  listingId: string;
  sellerId: string;
  idempotencyKey: string;
  state: OrderState;
  amountMinor: bigint;
  currency: string;
  shareBps: bigint;
  sellerAmount: bigint;
  platformAmount: bigint;
  sellerRefunded: bigint;
  platformRefunded: bigint;
  checkoutSessionId: string | null;
  checkoutUrl: string | null;
  checkoutExpiresAt: number | null;
  paymentIntentId: string | null;
  holdReason: string | null;
  createdAt: number;
  updatedAt: number;
  paidAt: number | null;
  releasedAt: number | null;
}

export function refundedTotal(order: Order): bigint {
  return order.sellerRefunded + order.platformRefunded;
}

function numOrNull(value: unknown): number | null {
  return value === null || value === undefined ? null : num(value);
}

export function rowToOrder(row: Row): Order {
  const state = str(row.state);
  if (!isOrderState(state)) {
    throw new Error(`unknown order state ${state}`);
  }
  return {
    id: str(row.id),
    buyerId: str(row.buyer_id),
    listingId: str(row.listing_id),
    sellerId: str(row.seller_id),
    idempotencyKey: str(row.idempotency_key),
    state,
    amountMinor: big(row.amount_minor),
    currency: str(row.currency),
    shareBps: big(row.share_bps),
    sellerAmount: big(row.seller_amount),
    platformAmount: big(row.platform_amount),
    sellerRefunded: big(row.seller_refunded),
    platformRefunded: big(row.platform_refunded),
    checkoutSessionId: strOrNull(row.checkout_session_id),
    checkoutUrl: strOrNull(row.checkout_url),
    checkoutExpiresAt: numOrNull(row.checkout_expires_at),
    paymentIntentId: strOrNull(row.payment_intent_id),
    holdReason: strOrNull(row.hold_reason),
    createdAt: num(row.created_at),
    updatedAt: num(row.updated_at),
    paidAt: numOrNull(row.paid_at),
    releasedAt: numOrNull(row.released_at),
  };
}

export async function getOrder(q: Queryable, id: string): Promise<Order | null> {
  const rows = await q.query('SELECT * FROM orders WHERE id = $1', [id]);
  return rows.length > 0 ? rowToOrder(rows[0]!) : null;
}

export async function findOrderBy(q: Queryable, column: 'checkout_session_id' | 'payment_intent_id', value: string): Promise<Order | null> {
  const rows = await q.query(`SELECT * FROM orders WHERE ${column} = $1`, [value]);
  return rows.length > 0 ? rowToOrder(rows[0]!) : null;
}

export class IllegalTransitionError extends Error {
  readonly from: OrderState;
  readonly event: OrderEvent;
  constructor(from: OrderState, event: OrderEvent) {
    super(`illegal order transition: ${event} in ${from}`);
    this.name = 'IllegalTransitionError';
    this.from = from;
    this.event = event;
  }
}

export interface ApplyTransitionInput {
  order: Order;
  event: OrderEvent;
  source: string;
  at: number;
  context?: TransitionContext;
  /** Extra columns to set together with the state (whitelisted names only). */
  set?: Partial<Record<OrderColumn, string | bigint | number | null>>;
}

type OrderColumn =
  | 'checkout_session_id'
  | 'checkout_url'
  | 'checkout_expires_at'
  | 'payment_intent_id'
  | 'hold_reason'
  | 'paid_at'
  | 'released_at'
  | 'seller_refunded'
  | 'platform_refunded';

const ORDER_COLUMNS = new Set<string>([
  'checkout_session_id',
  'checkout_url',
  'checkout_expires_at',
  'payment_intent_id',
  'hold_reason',
  'paid_at',
  'released_at',
  'seller_refunded',
  'platform_refunded',
]);

/**
 * Applies a transition inside the caller's transaction. Throws IllegalTransitionError when the event is not allowed in the order's
 * state, or when the stored state is no longer the one the caller read (compare-and-set). Records an order_events row.
 */
export async function applyTransition(tx: Queryable, input: ApplyTransitionInput): Promise<Order> {
  const { order, event } = input;
  const next = transition(order.state, event, input.context);
  if (next === null) {
    throw new IllegalTransitionError(order.state, event);
  }
  const assignments: string[] = ['state = $1', 'updated_at = $2'];
  const params: (string | bigint | number | null)[] = [next, input.at];
  for (const [column, value] of Object.entries(input.set ?? {})) {
    if (!ORDER_COLUMNS.has(column)) {
      throw new Error(`column ${column} cannot be set by a transition`);
    }
    params.push(value ?? null);
    assignments.push(`${column} = $${params.length}`);
  }
  params.push(order.id, order.state);
  const rows = await tx.query(
    `UPDATE orders SET ${assignments.join(', ')} WHERE id = $${params.length - 1} AND state = $${params.length} RETURNING *`,
    params,
  );
  if (rows.length === 0) {
    throw new IllegalTransitionError(order.state, event);
  }
  await tx.query(
    'INSERT INTO order_events (id, order_id, from_state, to_state, event, source, created_at) VALUES ($1, $2, $3, $4, $5, $6, $7)',
    [randomUUID(), order.id, order.state, next, event, input.source, input.at],
  );
  return rowToOrder(rows[0]!);
}

export function orderToJson(order: Order): Record<string, unknown> {
  return {
    orderId: order.id,
    listingId: order.listingId,
    state: order.state,
    checkoutUrl: order.state === 'CheckoutOpen' ? order.checkoutUrl : null,
    checkoutExpiresAt: order.checkoutExpiresAt === null ? null : new Date(order.checkoutExpiresAt).toISOString(),
    amountMinor: Number(order.amountMinor),
    currency: order.currency,
    refundedMinor: Number(refundedTotal(order)),
    createdAt: new Date(order.createdAt).toISOString(),
    paidAt: order.paidAt === null ? null : new Date(order.paidAt).toISOString(),
  };
}
