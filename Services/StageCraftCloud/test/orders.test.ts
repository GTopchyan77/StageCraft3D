import test from 'node:test';
import assert from 'node:assert/strict';
import { applyTransition, getOrder, IllegalTransitionError, isOrderState, ORDER_STATES, rowToOrder, transition, type OrderEvent, type OrderState } from '../src/orders.ts';
import { freshDatabase } from './helpers.ts';

const EVENTS: OrderEvent[] = [
  'checkout_opened',
  'checkout_failed',
  'payment_succeeded',
  'payment_failed',
  'checkout_expired',
  'amount_mismatch',
  'refunded_partially',
  'refunded_fully',
  'dispute_opened',
  'dispute_won',
  'dispute_lost_partially',
  'dispute_lost_fully',
];

// The complete legal table from ADR 0032 §3.4 (plus OnHold for amount mismatches). Everything else must be refused.
const LEGAL: Partial<Record<OrderState, Partial<Record<OrderEvent, OrderState>>>> = {
  Created: { checkout_opened: 'CheckoutOpen', checkout_failed: 'Failed' },
  CheckoutOpen: { payment_succeeded: 'Paid', payment_failed: 'Failed', checkout_expired: 'Expired', amount_mismatch: 'OnHold' },
  Paid: { refunded_partially: 'PartiallyRefunded', refunded_fully: 'Refunded', dispute_opened: 'Disputed' },
  PartiallyRefunded: { refunded_partially: 'PartiallyRefunded', refunded_fully: 'Refunded', dispute_opened: 'Disputed' },
  Disputed: { dispute_won: 'Paid', dispute_lost_partially: 'PartiallyRefunded', dispute_lost_fully: 'Refunded' },
  OnHold: { refunded_fully: 'Refunded' },
};

test('transition table: every state x event pair', () => {
  for (const state of ORDER_STATES) {
    for (const event of EVENTS) {
      assert.equal(transition(state, event), LEGAL[state]?.[event] ?? null, `${state} + ${event}`);
    }
  }
  // A won dispute returns a previously partially refunded order to PartiallyRefunded.
  assert.equal(transition('Disputed', 'dispute_won', { refundedBefore: true }), 'PartiallyRefunded');
  // Terminal states accept nothing; for example a refund of an unpaid order is refused.
  assert.equal(transition('CheckoutOpen', 'refunded_fully'), null);
  assert.equal(transition('Expired', 'payment_succeeded'), null);
  assert.equal(isOrderState('Paid'), true);
  assert.equal(isOrderState('Bogus'), false);
});

async function seedOrder(db: Awaited<ReturnType<typeof freshDatabase>>, state: OrderState): Promise<string> {
  const now = 1;
  await db.query("INSERT INTO accounts (id, issuer, subject, display_name, created_at) VALUES ('a1', 't', 's1', 'A', 1), ('a2', 't', 's2', 'B', 1)");
  await db.query(
    `INSERT INTO listings (id, author_id, author_name, title, description, kind, category, keywords_json, credits, real_money_minor, currency,
       item_count, fixture_count, cue_count, package_bytes, package_sha256, has_thumbnail, allow_export, manifest_version, required_items_json,
       search_text, title_key, status, created_at, package_key)
     VALUES ('L1', 'a2', 'B', 'T', '', 'StageDesign', 'StageCraft.Marketplace.Category.Other', '[]', 0, 500, 'USD', 0, 0, 0, 1, 'h', 0, 0, 2, '[]', 't', '0074', 'Approved', 1, 'packages/l1.stage')`,
  );
  await db.query(
    `INSERT INTO orders (id, buyer_id, listing_id, seller_id, idempotency_key, state, amount_minor, currency, share_bps, seller_amount, platform_amount, created_at, updated_at)
     VALUES ('00000000-0000-4000-8000-000000000001', 'a1', 'L1', 'a2', 'key-00000001', $1, 500, 'USD', 8500, 425, 75, $2, $2)`,
    [state, now],
  );
  return '00000000-0000-4000-8000-000000000001';
}

test('applyTransition persists the state, records an event, and refuses illegal or stale transitions', async () => {
  const db = await freshDatabase();
  try {
    const id = await seedOrder(db, 'Created');
    const created = (await getOrder(db, id))!;
    const open = await db.withTransaction((tx) => applyTransition(tx, { order: created, event: 'checkout_opened', source: 'test', at: 5, set: { checkout_url: 'https://pay', checkout_session_id: 'cs_1' } }));
    assert.equal(open.state, 'CheckoutOpen');
    assert.equal(open.checkoutUrl, 'https://pay');
    const events = await db.query('SELECT from_state, to_state, event FROM order_events WHERE order_id = $1', [id]);
    assert.deepEqual(events.map((row) => [row.from_state, row.to_state, row.event]), [['Created', 'CheckoutOpen', 'checkout_opened']]);
    // Illegal in the current state.
    await assert.rejects(db.withTransaction((tx) => applyTransition(tx, { order: open, event: 'refunded_fully', source: 'test', at: 6 })), IllegalTransitionError);
    // Stale read: the caller thinks the order is still Created.
    await assert.rejects(db.withTransaction((tx) => applyTransition(tx, { order: created, event: 'checkout_opened', source: 'test', at: 6 })), IllegalTransitionError);
    // Only whitelisted columns can be set.
    await assert.rejects(
      db.withTransaction((tx) => applyTransition(tx, { order: open, event: 'checkout_expired', source: 'test', at: 6, set: { amount_minor: 1 } as never })),
      /cannot be set/,
    );
    assert.equal((await getOrder(db, id))!.state, 'CheckoutOpen');
    assert.equal(await getOrder(db, '00000000-0000-4000-8000-00000000ffff'), null);
    const row = (await db.query('SELECT * FROM orders'))[0]!;
    assert.throws(() => rowToOrder({ ...row, state: 'Weird' }), /unknown order state/);
  } finally {
    await db.close();
  }
});
