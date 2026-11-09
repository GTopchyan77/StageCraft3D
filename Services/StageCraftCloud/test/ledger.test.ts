import test from 'node:test';
import assert from 'node:assert/strict';
import {
  Accounts,
  assertBalanced,
  balance,
  balancesByPrefix,
  countEntries,
  disputeOpenedLines,
  disputeWonLines,
  feeLines,
  payoutFailedLines,
  payoutReservedLines,
  payoutSettledLines,
  postEntry,
  refundLines,
  releaseLines,
  saleLines,
  trialBalance,
  UnbalancedEntryError,
} from '../src/ledger.ts';
import { freshDatabase } from './helpers.ts';

const SELLER = '11111111-2222-3333-4444-555555555555';

test('every line builder balances', () => {
  for (const lines of [
    saleLines(SELLER, 'USD', 850n, 150n),
    releaseLines(SELLER, 'USD', 850n),
    refundLines(Accounts.sellerPending(SELLER), 'USD', 85n, 15n, Accounts.cash),
    refundLines(Accounts.sellerAvailable(SELLER), 'USD', 85n, 15n, Accounts.disputes),
    disputeOpenedLines('USD', 100n),
    disputeWonLines('USD', 100n),
    feeLines('USD', 1500n),
    payoutReservedLines(SELLER, 'USD', 10n),
    payoutSettledLines(SELLER, 'USD', 10n),
    payoutFailedLines(SELLER, 'USD', 10n),
  ]) {
    assert.doesNotThrow(() => assertBalanced(lines));
  }
});

test('assertBalanced refuses unbalanced, single-line, zero, bad account and bad currency entries', () => {
  const bad = (lines: { account: string; currency: string; amount: bigint }[]) => assert.throws(() => assertBalanced(lines), UnbalancedEntryError);
  bad([{ account: Accounts.cash, currency: 'USD', amount: 1n }]);
  bad([
    { account: Accounts.cash, currency: 'USD', amount: 10n },
    { account: Accounts.revenue, currency: 'USD', amount: -9n },
  ]);
  // Balanced overall but not per currency.
  bad([
    { account: Accounts.cash, currency: 'USD', amount: 10n },
    { account: Accounts.revenue, currency: 'EUR', amount: -10n },
  ]);
  bad([
    { account: Accounts.cash, currency: 'USD', amount: 0n },
    { account: Accounts.revenue, currency: 'USD', amount: 0n },
  ]);
  bad([
    { account: 'nonsense', currency: 'USD', amount: 1n },
    { account: Accounts.revenue, currency: 'USD', amount: -1n },
  ]);
  bad([
    { account: Accounts.cash, currency: 'usd', amount: 1n },
    { account: Accounts.revenue, currency: 'usd', amount: -1n },
  ]);
  assert.throws(() => Accounts.sellerPending('bad:id'), RangeError);
});

test('postEntry: balanced entries post once per ref; balances are sums of lines', async () => {
  const db = await freshDatabase();
  try {
    const at = 1000;
    const first = await db.withTransaction((tx) => postEntry(tx, { ref: 'sale:a', kind: 'sale', at, lines: saleLines(SELLER, 'USD', 850n, 150n) }));
    assert.ok(first);
    const replay = await db.withTransaction((tx) => postEntry(tx, { ref: 'sale:a', kind: 'sale', at, lines: saleLines(SELLER, 'USD', 850n, 150n) }));
    assert.equal(replay, null);
    await db.withTransaction((tx) =>
      postEntry(tx, { ref: 'refund:a', kind: 'refund', at, lines: refundLines(Accounts.sellerPending(SELLER), 'USD', 0n, 1n, Accounts.cash) }),
    );
    assert.equal(await balance(db, Accounts.cash, 'USD'), 999n);
    assert.equal(await balance(db, Accounts.sellerPending(SELLER), 'USD'), -850n);
    assert.equal(await balance(db, Accounts.revenue, 'USD'), -150n);
    assert.equal(await balance(db, Accounts.refunds, 'USD'), 1n);
    assert.equal(await balance(db, Accounts.cash, 'EUR'), 0n);
    assert.deepEqual(await balancesByPrefix(db, 'seller:'), [{ account: Accounts.sellerPending(SELLER), currency: 'USD', total: -850n }]);
    assert.deepEqual([...(await trialBalance(db))], [['USD', 0n]]);
    assert.equal(await countEntries(db, 'sale'), 1);
  } finally {
    await db.close();
  }
});

test('an unbalanced entry throws and rolls back the whole transaction', async () => {
  const db = await freshDatabase();
  try {
    await assert.rejects(
      db.withTransaction(async (tx) => {
        await postEntry(tx, { ref: 'ok', kind: 'sale', at: 1, lines: saleLines(SELLER, 'USD', 85n, 15n) });
        await postEntry(tx, {
          ref: 'bad',
          kind: 'sale',
          at: 1,
          lines: [
            { account: Accounts.cash, currency: 'USD', amount: 5n },
            { account: Accounts.revenue, currency: 'USD', amount: -4n },
          ],
        });
      }),
      UnbalancedEntryError,
    );
    assert.equal(await countEntries(db, 'sale'), 0);
    assert.equal(await balance(db, Accounts.cash, 'USD'), 0n);
  } finally {
    await db.close();
  }
});

test('the database check refuses an entry whose stored lines do not balance', async () => {
  const db = await freshDatabase();
  try {
    // A line smuggled in before postEntry's own check runs (simulating a bug elsewhere in the transaction).
    await assert.rejects(
      db.withTransaction(async (tx) => {
        const lines = saleLines(SELLER, 'USD', 85n, 15n);
        const tamperingTx = {
          query: async (sql: string, params?: readonly (string | bigint | number | null)[]) => {
            const rows = await tx.query(sql, params);
            if (sql.startsWith('INSERT INTO journal_lines') && params?.[1] === 0) {
              await tx.query('INSERT INTO journal_lines (entry_id, line_no, account, currency, amount) VALUES ($1, 99, $2, $3, 7)', [params[0]!, Accounts.cash, 'USD']);
            }
            return rows;
          },
        };
        await postEntry(tamperingTx, { ref: 'x', kind: 'sale', at: 1, lines });
      }),
      UnbalancedEntryError,
    );
    assert.equal(await countEntries(db, 'sale'), 0);
  } finally {
    await db.close();
  }
});
