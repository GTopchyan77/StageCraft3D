// Stripe over its documented REST API: form-encoded POSTs with an Idempotency-Key, no SDK (ADR 0032 §4, §10).
import {
  ProviderFailure,
  type CheckoutInput,
  type CheckoutSession,
  type ConnectedAccountInput,
  type OnboardingLinkInput,
  type PaymentProvider,
  type RefundInput,
  type TransferInput,
} from './provider.ts';

export interface StripeOptions {
  secretKey: string;
  /** https://api.stripe.com in production; tests point it at a local fake. */
  baseUrl: string;
  automaticTax: boolean;
  /** The platform account's country; connected accounts elsewhere use the recipient service agreement. */
  platformCountry: string;
  apiVersion?: string;
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
}

type FormValue = string | number | bigint | boolean | null | undefined | FormObject | FormValue[];
interface FormObject {
  [key: string]: FormValue;
}

/** Stripe's bracket encoding: a[b][0][c]=v. Undefined and null values are left out. */
export function encodeForm(object: FormObject): string {
  const pairs: string[] = [];
  const add = (prefix: string, value: FormValue): void => {
    if (value === undefined || value === null) {
      return;
    }
    if (Array.isArray(value)) {
      value.forEach((entry, index) => add(`${prefix}[${index}]`, entry));
    } else if (typeof value === 'object') {
      for (const [key, entry] of Object.entries(value)) {
        add(prefix ? `${prefix}[${key}]` : key, entry);
      }
    } else {
      pairs.push(`${encodeURIComponent(prefix)}=${encodeURIComponent(String(value))}`);
    }
  };
  add('', object);
  return pairs.join('&');
}

export class StripeProvider implements PaymentProvider {
  readonly name = 'stripe';
  private readonly options: StripeOptions;

  constructor(options: StripeOptions) {
    this.options = options;
  }

  private async post(path: string, form: FormObject, idempotencyKey: string): Promise<Record<string, unknown>> {
    const headers: Record<string, string> = {
      Authorization: `Bearer ${this.options.secretKey}`,
      'Content-Type': 'application/x-www-form-urlencoded',
      'Idempotency-Key': idempotencyKey,
    };
    if (this.options.apiVersion) {
      headers['Stripe-Version'] = this.options.apiVersion;
    }
    let response: Response;
    try {
      response = await (this.options.fetchImpl ?? fetch)(new URL(path, this.options.baseUrl), {
        method: 'POST',
        headers,
        body: encodeForm(form),
        signal: AbortSignal.timeout(this.options.timeoutMs ?? 20_000),
      });
    } catch (error) {
      throw new ProviderFailure(`stripe request failed: ${(error as Error).name}`, 0, null);
    }
    let body: Record<string, unknown> = {};
    try {
      body = (await response.json()) as Record<string, unknown>;
    } catch {
      // A non-JSON answer is handled as a failure below.
    }
    if (!response.ok) {
      const error = (body.error ?? {}) as Record<string, unknown>;
      const code = typeof error.code === 'string' ? error.code : typeof error.type === 'string' ? error.type : null;
      throw new ProviderFailure(`stripe ${path} answered ${response.status}`, response.status, code);
    }
    return body;
  }

  private static requireString(body: Record<string, unknown>, field: string): string {
    const value = body[field];
    if (typeof value !== 'string' || value.length === 0) {
      throw new ProviderFailure(`stripe answer has no ${field}`, 200, null);
    }
    return value;
  }

  async createCheckout(input: CheckoutInput): Promise<CheckoutSession> {
    const expiresAtSeconds = Math.floor(input.expiresAt / 1000);
    const body = await this.post(
      '/v1/checkout/sessions',
      {
        mode: 'payment',
        client_reference_id: input.orderId,
        success_url: input.successUrl,
        cancel_url: input.cancelUrl,
        expires_at: expiresAtSeconds,
        line_items: [
          {
            quantity: 1,
            price_data: {
              currency: input.currency.toLowerCase(),
              unit_amount: input.amountMinor,
              product_data: { name: input.title },
            },
          },
        ],
        metadata: { orderId: input.orderId },
        payment_intent_data: { metadata: { orderId: input.orderId } },
        automatic_tax: { enabled: this.options.automaticTax },
      },
      `checkout-${input.orderId}`,
    );
    return {
      sessionId: StripeProvider.requireString(body, 'id'),
      url: StripeProvider.requireString(body, 'url'),
      expiresAt: typeof body.expires_at === 'number' ? body.expires_at * 1000 : expiresAtSeconds * 1000,
    };
  }

  async createRefund(input: RefundInput): Promise<{ refundId: string; status: string }> {
    const body = await this.post(
      '/v1/refunds',
      { payment_intent: input.paymentIntentId, amount: input.amountMinor, metadata: { orderId: input.orderId } },
      `refund-${input.idempotencyKey}`,
    );
    return { refundId: StripeProvider.requireString(body, 'id'), status: typeof body.status === 'string' ? body.status : 'unknown' };
  }

  async createConnectedAccount(input: ConnectedAccountInput): Promise<{ providerAccountId: string }> {
    const crossBorder = input.country !== this.options.platformCountry;
    const body = await this.post(
      '/v1/accounts',
      {
        type: 'express',
        country: input.country,
        capabilities: { transfers: { requested: true } },
        // A connected account outside the platform's country can only receive transfers under the recipient agreement.
        tos_acceptance: crossBorder ? { service_agreement: 'recipient' } : undefined,
        metadata: { accountId: input.accountId },
      },
      `account-${input.accountId}`,
    );
    return { providerAccountId: StripeProvider.requireString(body, 'id') };
  }

  async createOnboardingLink(input: OnboardingLinkInput): Promise<{ url: string; expiresAt: number }> {
    const body = await this.post(
      '/v1/account_links',
      { account: input.providerAccountId, refresh_url: input.refreshUrl, return_url: input.returnUrl, type: 'account_onboarding' },
      // Account links are single-use; a fresh key per call is intended.
      `account-link-${input.providerAccountId}-${Date.now()}`,
    );
    return {
      url: StripeProvider.requireString(body, 'url'),
      expiresAt: typeof body.expires_at === 'number' ? body.expires_at * 1000 : 0,
    };
  }

  async createTransfer(input: TransferInput): Promise<{ transferId: string }> {
    const body = await this.post(
      '/v1/transfers',
      {
        amount: input.amountMinor,
        currency: input.currency.toLowerCase(),
        destination: input.destination,
        metadata: { payoutId: input.payoutId },
      },
      `transfer-${input.idempotencyKey}`,
    );
    return { transferId: StripeProvider.requireString(body, 'id') };
  }
}
