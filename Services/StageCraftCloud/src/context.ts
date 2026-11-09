// Everything a service function needs, built once at startup (main.ts) or per test, and passed explicitly: no globals.
import { EventEmitter } from 'node:events';
import type { OidcClient } from './auth/oidc.ts';
import type { BlobStore } from './blob/blobStore.ts';
import type { Config } from './config.ts';
import type { Database } from './db/driver.ts';
import type { RateLimiter } from './http/rateLimit.ts';
import type { Logger } from './log.ts';
import type { PaymentProvider } from './payments/provider.ts';

export interface Clock {
  now(): number;
}

export const systemClock: Clock = { now: () => Date.now() };

/**
 * Wakes long polls of GET /v1/orders/{id} when this process commits a change to that order. A change committed by another replica
 * is picked up by the poll's periodic re-read instead (see routes/orders.ts).
 */
export class OrderNotifier {
  private readonly emitter = new EventEmitter();

  constructor() {
    this.emitter.setMaxListeners(0);
  }

  notify(orderId: string): void {
    this.emitter.emit(orderId);
  }

  /** Resolves on the next notify(orderId) or after ms, whichever comes first. */
  wait(orderId: string, ms: number): Promise<void> {
    return new Promise((resolve) => {
      const done = (): void => {
        clearTimeout(timer);
        this.emitter.off(orderId, done);
        resolve();
      };
      const timer = setTimeout(done, ms);
      this.emitter.once(orderId, done);
    });
  }

  listenerCount(orderId: string): number {
    return this.emitter.listenerCount(orderId);
  }
}

export interface AppContext {
  config: Config;
  db: Database;
  clock: Clock;
  provider: PaymentProvider;
  blob: BlobStore;
  log: Logger;
  notifier: OrderNotifier;
  /** Null in AUTH_MODE=dev. */
  oidc: OidcClient | null;
  limiter: RateLimiter;
}
