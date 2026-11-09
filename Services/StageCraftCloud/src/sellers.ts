// Seller payout onboarding (ADR 0032 §3.1, §4): Connect Express for US/UK/EEA/CA/CH sellers, a manual payout method elsewhere
// (enabled by an admin after checking the seller's payout details off-platform, until the Global Payouts adapter exists).
import { ApiError } from './errors.ts';
import { num, str, strOrNull, type Queryable } from './db/driver.ts';
import type { AppContext } from './context.ts';
import { writeAudit } from './audit.ts';
import { payoutMethodFor, ProviderFailure } from './payments/provider.ts';

export interface Seller {
  accountId: string;
  country: string;
  payoutMethod: 'stripe' | 'manual';
  providerAccountId: string | null;
  payoutsEnabled: boolean;
}

export async function getSeller(q: Queryable, accountId: string): Promise<Seller | null> {
  const rows = await q.query('SELECT * FROM sellers WHERE account_id = $1', [accountId]);
  const row = rows[0];
  return row
    ? {
        accountId: str(row.account_id),
        country: str(row.country),
        payoutMethod: str(row.payout_method) as Seller['payoutMethod'],
        providerAccountId: strOrNull(row.provider_account_id),
        payoutsEnabled: num(row.payouts_enabled) === 1,
      }
    : null;
}

export function sellerToJson(seller: Seller | null): Record<string, unknown> {
  if (!seller) {
    return { status: 'none', payoutMethod: null, payoutsEnabled: false, country: null };
  }
  return {
    status: seller.payoutsEnabled ? 'enabled' : 'pending',
    payoutMethod: seller.payoutMethod,
    payoutsEnabled: seller.payoutsEnabled,
    country: seller.country,
  };
}

/**
 * POST /v1/me/seller/onboarding {country}. Stripe sellers get a fresh Account Link (the connected account is created once);
 * manual-payout sellers are registered and wait for an admin. The country is fixed by the first call.
 */
export async function startOnboarding(ctx: AppContext, accountId: string, countryInput: unknown): Promise<Record<string, unknown>> {
  const existing = await getSeller(ctx.db, accountId);
  const country = existing?.country ?? (typeof countryInput === 'string' ? countryInput.trim().toUpperCase() : '');
  if (!/^[A-Z]{2}$/.test(country)) {
    throw new ApiError('InvalidRequest', 'country must be an ISO 3166-1 alpha-2 code');
  }
  const method = existing?.payoutMethod ?? payoutMethodFor(country);
  const now = ctx.clock.now();
  if (!existing) {
    await ctx.db.query(
      `INSERT INTO sellers (account_id, country, payout_method, payouts_enabled, created_at, updated_at) VALUES ($1, $2, $3, 0, $4, $4)
       ON CONFLICT (account_id) DO NOTHING`,
      [accountId, country, method, now],
    );
  }
  if (method === 'manual') {
    const seller = (await getSeller(ctx.db, accountId))!;
    return { ...sellerToJson(seller), onboardingUrl: null, message: 'Payouts to your country are sent manually; an admin will contact you to confirm your payout details.' };
  }
  try {
    let providerAccountId = existing?.providerAccountId ?? null;
    if (!providerAccountId) {
      providerAccountId = (await ctx.provider.createConnectedAccount({ accountId, country })).providerAccountId;
      await ctx.db.query('UPDATE sellers SET provider_account_id = $1, updated_at = $2 WHERE account_id = $3 AND provider_account_id IS NULL', [
        providerAccountId,
        now,
        accountId,
      ]);
      providerAccountId = (await getSeller(ctx.db, accountId))!.providerAccountId!;
    }
    const link = await ctx.provider.createOnboardingLink({
      providerAccountId,
      refreshUrl: `${ctx.config.publicBaseUrl}/seller/onboarding/refresh`,
      returnUrl: `${ctx.config.publicBaseUrl}/seller/onboarding/done`,
    });
    const seller = (await getSeller(ctx.db, accountId))!;
    return { ...sellerToJson(seller), onboardingUrl: link.url };
  } catch (error) {
    if (error instanceof ProviderFailure) {
      ctx.log.warn('seller onboarding failed at the provider', { accountId, status: error.status, providerCode: error.providerCode });
      throw new ApiError('ProviderError', 'the payment provider is not available; try again');
    }
    throw error;
  }
}

/** POST /v1/admin/sellers/{id}/manual-payouts {enabled}: an admin confirms (or withdraws) a manual-payout seller. */
export async function setManualPayouts(ctx: AppContext, adminId: string, sellerId: string, enabled: unknown): Promise<Seller> {
  if (typeof enabled !== 'boolean') {
    throw new ApiError('InvalidRequest', 'enabled must be true or false');
  }
  const now = ctx.clock.now();
  return ctx.db.withTransaction(async (tx) => {
    const before = await getSeller(tx, sellerId);
    if (!before) {
      throw new ApiError('NotFound', 'no such seller');
    }
    if (before.payoutMethod !== 'manual') {
      throw new ApiError('IllegalTransition', 'Stripe sellers are enabled by Stripe onboarding, not by an admin');
    }
    await tx.query('UPDATE sellers SET payouts_enabled = $1, updated_at = $2 WHERE account_id = $3', [enabled ? 1 : 0, now, sellerId]);
    const after = (await getSeller(tx, sellerId))!;
    await writeAudit(tx, { actorId: adminId, action: 'seller.manual_payouts', target: `seller:${sellerId}`, before, after, at: now });
    return after;
  });
}
