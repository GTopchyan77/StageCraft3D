// StageCraft Pro licences (ADR 0034 §17.2): a signed token for one account on one machine, valid offline for LICENCE_OFFLINE_DAYS.
// Issued only while the account's subscription is entitled; at most LICENCE_MAX_MACHINES machines per account at a time.
import { ApiError } from './errors.ts';
import { num } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { PRO_ENTITLEMENTS } from './licence/token.ts';
import { currentSubscription, isEntitled } from './subscriptions.ts';

/** What the client sends: SHA-256 of its salted install id, as lowercase hex (StageLicenceToken::MakeMachineId). */
const MACHINE_ID = /^[0-9a-f]{64}$/;

export function parseMachineId(value: unknown): string {
  if (typeof value !== 'string' || !MACHINE_ID.test(value)) {
    throw new ApiError('InvalidRequest', 'machineId must be 64 lowercase hex characters');
  }
  return value;
}

export interface IssuedLicence {
  token: string;
  /** Epoch ms. */
  expiresAt: number;
  plan: string;
  entitlements: string[];
}

/** POST /v1/licences. Refuses NoActiveSubscription and MachineLimitReached; reissuing to a machine already counted is always allowed. */
export async function issueLicence(ctx: AppContext, accountId: string, machineId: string): Promise<IssuedLicence> {
  const now = ctx.clock.now();
  const plan = await ctx.db.withTransaction(async (tx) => {
    const subscription = await currentSubscription(tx, accountId);
    if (!isEntitled(subscription)) {
      throw new ApiError('NoActiveSubscription', 'this account has no active StageCraft Pro subscription');
    }
    const known = await tx.query('SELECT released_at FROM licence_machines WHERE account_id = $1 AND machine_id = $2', [accountId, machineId]);
    const counted = known.length > 0 && (known[0]!.released_at === null || known[0]!.released_at === undefined);
    if (!counted) {
      const active = await tx.query('SELECT COUNT(*) AS n FROM licence_machines WHERE account_id = $1 AND released_at IS NULL', [accountId]);
      if (num(active[0]!.n) >= ctx.config.licence.maxMachines) {
        throw new ApiError(
          'MachineLimitReached',
          `StageCraft Pro is active on ${ctx.config.licence.maxMachines} computers already; release one of them first`,
        );
      }
    }
    await tx.query(
      `INSERT INTO licence_machines (account_id, machine_id, first_issued_at, last_issued_at, released_at) VALUES ($1, $2, $3, $3, NULL)
       ON CONFLICT (account_id, machine_id) DO UPDATE SET last_issued_at = excluded.last_issued_at, released_at = NULL`,
      [accountId, machineId, now],
    );
    return subscription!.plan;
  });
  const issuedAt = Math.floor(now / 1000);
  const expiresAt = issuedAt + ctx.config.licence.offlineDays * 86_400;
  const entitlements = [...PRO_ENTITLEMENTS];
  const token = ctx.signer.sign({ sub: accountId, mid: machineId, plan, ent: entitlements, iat: issuedAt, exp: expiresAt });
  ctx.log.info('licence issued', { accountId, plan, expiresAt: new Date(expiresAt * 1000).toISOString() });
  return { token, expiresAt: expiresAt * 1000, plan, entitlements };
}

const RELEASE_WINDOW_MS = 30 * 86_400_000;

/**
 * POST /v1/licences/release: frees the machine's place. A released machine's token keeps working offline until it expires, so
 * releasing and reissuing could otherwise put Pro on any number of computers: at most LICENCE_MAX_MACHINES releases per 30 days.
 */
export async function releaseLicence(ctx: AppContext, accountId: string, machineId: string): Promise<{ released: boolean }> {
  const recent = await ctx.db.query('SELECT COUNT(*) AS n FROM licence_machines WHERE account_id = $1 AND released_at > $2', [
    accountId,
    ctx.clock.now() - RELEASE_WINDOW_MS,
  ]);
  if (num(recent[0]!.n) >= ctx.config.licence.maxMachines) {
    throw new ApiError('MachineLimitReached', 'too many computers were released in the last 30 days; try again later or contact support');
  }
  const rows = await ctx.db.query(
    'UPDATE licence_machines SET released_at = $1 WHERE account_id = $2 AND machine_id = $3 AND released_at IS NULL RETURNING machine_id',
    [ctx.clock.now(), accountId, machineId],
  );
  return { released: rows.length > 0 };
}
