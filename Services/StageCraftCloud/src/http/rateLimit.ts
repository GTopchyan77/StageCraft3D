// In-process token buckets per key (an IP or an account id) and policy. With several replicas each pod enforces its own buckets,
// so the effective limit is replicas x capacity; the ALB/WAF rate rule is the cluster-wide backstop (docs/RUNBOOK.md).

export interface RatePolicy {
  /** Bucket size: the burst allowed. */
  capacity: number;
  /** Tokens added per second. */
  refillPerSecond: number;
}

interface Bucket {
  tokens: number;
  updatedAt: number;
}

export interface TakeResult {
  ok: boolean;
  retryAfterSeconds: number;
}

export class RateLimiter {
  private readonly buckets = new Map<string, Bucket>();
  private readonly now: () => number;
  private readonly maxBuckets: number;

  constructor(now: () => number, maxBuckets = 100_000) {
    this.now = now;
    this.maxBuckets = maxBuckets;
  }

  take(policyName: string, policy: RatePolicy, key: string, cost = 1): TakeResult {
    const id = `${policyName}|${key}`;
    const now = this.now();
    let bucket = this.buckets.get(id);
    if (!bucket) {
      if (this.buckets.size >= this.maxBuckets) {
        this.evictIdle(now);
      }
      bucket = { tokens: policy.capacity, updatedAt: now };
      this.buckets.set(id, bucket);
    }
    const elapsedSeconds = Math.max(0, now - bucket.updatedAt) / 1000;
    bucket.tokens = Math.min(policy.capacity, bucket.tokens + elapsedSeconds * policy.refillPerSecond);
    bucket.updatedAt = now;
    if (bucket.tokens >= cost) {
      bucket.tokens -= cost;
      return { ok: true, retryAfterSeconds: 0 };
    }
    return { ok: false, retryAfterSeconds: Math.max(1, Math.ceil((cost - bucket.tokens) / policy.refillPerSecond)) };
  }

  /** Drops full buckets (idle long enough to refill); if none are full, drops the oldest half. */
  private evictIdle(now: number): void {
    for (const [id, bucket] of this.buckets) {
      if (now - bucket.updatedAt > 3_600_000) {
        this.buckets.delete(id);
      }
    }
    if (this.buckets.size >= this.maxBuckets) {
      let drop = Math.floor(this.buckets.size / 2);
      for (const id of this.buckets.keys()) {
        if (drop-- <= 0) break;
        this.buckets.delete(id);
      }
    }
  }

  get size(): number {
    return this.buckets.size;
  }
}

export const RatePolicies = {
  /** Every request, per client IP. */
  ipDefault: { capacity: 120, refillPerSecond: 2 },
  /** Every authenticated request, per account. */
  accountDefault: { capacity: 120, refillPerSecond: 2 },
  deviceStart: { capacity: 10, refillPerSecond: 10 / 3600 },
  authBrowser: { capacity: 30, refillPerSecond: 30 / 3600 },
  supportAnonymous: { capacity: 5, refillPerSecond: 5 / 3600 },
  supportAccount: { capacity: 20, refillPerSecond: 20 / 3600 },
  publish: { capacity: 20, refillPerSecond: 20 / 3600 },
  orderCreate: { capacity: 20, refillPerSecond: 20 / 60 / 10 },
  onboarding: { capacity: 10, refillPerSecond: 10 / 3600 },
  adminMutation: { capacity: 60, refillPerSecond: 1 },
} as const satisfies Record<string, RatePolicy>;

export type RatePolicyName = keyof typeof RatePolicies;
