// A deterministic provider for development and tests. It never moves money; settlement still only happens through a signed webhook
// (the dev fake-checkout page builds and signs one with the configured webhook secret).
import { randomUUID } from 'node:crypto';
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

export class FakeProvider implements PaymentProvider {
  readonly name = 'fake';
  readonly checkouts: CheckoutInput[] = [];
  readonly refunds: RefundInput[] = [];
  readonly transfers: TransferInput[] = [];
  readonly accounts: ConnectedAccountInput[] = [];
  /** Set by tests to make the next call of that kind fail. */
  failNext: Partial<Record<'checkout' | 'refund' | 'transfer' | 'account', boolean>> = {};
  private readonly publicBaseUrl: string;

  constructor(publicBaseUrl: string) {
    this.publicBaseUrl = publicBaseUrl;
  }

  private maybeFail(kind: 'checkout' | 'refund' | 'transfer' | 'account'): void {
    if (this.failNext[kind]) {
      this.failNext[kind] = false;
      throw new ProviderFailure(`fake ${kind} failure`, 502, 'fake_failure');
    }
  }

  async createCheckout(input: CheckoutInput): Promise<CheckoutSession> {
    this.maybeFail('checkout');
    this.checkouts.push(input);
    const sessionId = `cs_fake_${input.orderId}`;
    return { sessionId, url: `${this.publicBaseUrl}/dev/fake-checkout/${sessionId}`, expiresAt: input.expiresAt };
  }

  async createRefund(input: RefundInput): Promise<{ refundId: string; status: string }> {
    this.maybeFail('refund');
    this.refunds.push(input);
    return { refundId: `re_fake_${randomUUID()}`, status: 'pending' };
  }

  async createConnectedAccount(input: ConnectedAccountInput): Promise<{ providerAccountId: string }> {
    this.maybeFail('account');
    this.accounts.push(input);
    return { providerAccountId: `acct_fake_${input.accountId.replace(/-/g, '').slice(0, 16)}` };
  }

  async createOnboardingLink(input: OnboardingLinkInput): Promise<{ url: string; expiresAt: number }> {
    return { url: `${this.publicBaseUrl}/dev/fake-onboarding/${input.providerAccountId}`, expiresAt: 0 };
  }

  async createTransfer(input: TransferInput): Promise<{ transferId: string }> {
    this.maybeFail('transfer');
    this.transfers.push(input);
    return { transferId: `tr_fake_${input.payoutId}` };
  }
}
