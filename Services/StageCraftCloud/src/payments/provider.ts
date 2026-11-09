// The payment provider seam (ADR 0032 §3.1, D2). Settlement never comes from these calls: only verified webhooks mark an order paid
// or post a refund. These calls start things (a checkout, a refund, a transfer) and return the provider's ids.

export interface CheckoutInput {
  orderId: string;
  amountMinor: bigint;
  currency: string;
  title: string;
  /** Epoch ms; the provider refuses the session after it. */
  expiresAt: number;
  successUrl: string;
  cancelUrl: string;
}

export interface CheckoutSession {
  sessionId: string;
  url: string;
  expiresAt: number;
}

export interface RefundInput {
  paymentIntentId: string;
  amountMinor: bigint;
  idempotencyKey: string;
  orderId: string;
}

export interface ConnectedAccountInput {
  accountId: string;
  country: string;
}

export interface OnboardingLinkInput {
  providerAccountId: string;
  refreshUrl: string;
  returnUrl: string;
}

export interface TransferInput {
  amountMinor: bigint;
  currency: string;
  destination: string;
  idempotencyKey: string;
  payoutId: string;
}

export interface PaymentProvider {
  readonly name: string;
  createCheckout(input: CheckoutInput): Promise<CheckoutSession>;
  createRefund(input: RefundInput): Promise<{ refundId: string; status: string }>;
  createConnectedAccount(input: ConnectedAccountInput): Promise<{ providerAccountId: string }>;
  createOnboardingLink(input: OnboardingLinkInput): Promise<{ url: string; expiresAt: number }>;
  createTransfer(input: TransferInput): Promise<{ transferId: string }>;
}

export class ProviderFailure extends Error {
  /** HTTP status from the provider, or 0 for a network failure or timeout. */
  readonly status: number;
  /** The provider's own error code, safe to log (never the request body). */
  readonly providerCode: string | null;
  constructor(message: string, status: number, providerCode: string | null) {
    super(message);
    this.name = 'ProviderFailure';
    this.status = status;
    this.providerCode = providerCode;
  }
}

/** Countries a Stripe platform can pay by Connect cross-border transfers (ADR 0032 §3.1): US, UK, EEA, Canada and Switzerland. */
export const CONNECT_PAYOUT_COUNTRIES = new Set([
  'US', 'GB', 'CA', 'CH',
  // EU
  'AT', 'BE', 'BG', 'HR', 'CY', 'CZ', 'DK', 'EE', 'FI', 'FR', 'DE', 'GR', 'HU', 'IE', 'IT', 'LV', 'LT', 'LU', 'MT', 'NL', 'PL',
  'PT', 'RO', 'SK', 'SI', 'ES', 'SE',
  // EEA outside the EU
  'IS', 'LI', 'NO',
]);

export function payoutMethodFor(country: string): 'stripe' | 'manual' {
  return CONNECT_PAYOUT_COUNTRIES.has(country) ? 'stripe' : 'manual';
}
