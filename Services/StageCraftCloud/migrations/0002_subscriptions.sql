-- StageCraft Pro subscriptions and licences (ADR 0034 §17). Additive: nothing in 0001 changes.

-- A Stripe Checkout session in subscription mode, kept so a repeated request with the same key returns the same page.
CREATE TABLE subscription_checkouts (
  id TEXT PRIMARY KEY,
  account_id TEXT NOT NULL REFERENCES accounts (id),
  plan TEXT NOT NULL CHECK (plan IN ('monthly', 'yearly')),
  idempotency_key TEXT NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('Created', 'Open', 'Completed', 'Expired', 'Failed')),
  session_id TEXT UNIQUE,
  url TEXT,
  expires_at BIGINT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  UNIQUE (account_id, idempotency_key)
);
CREATE INDEX subscription_checkouts_account ON subscription_checkouts (account_id);

-- One row per Stripe subscription, written only by verified webhooks. last_event_created is the Stripe event time (seconds) of the
-- newest event applied, so an event delivered late never overwrites a newer state.
CREATE TABLE subscriptions (
  provider_subscription_id TEXT PRIMARY KEY,
  account_id TEXT NOT NULL REFERENCES accounts (id),
  provider_customer_id TEXT,
  plan TEXT NOT NULL,
  status TEXT NOT NULL,
  current_period_end BIGINT,
  cancel_at_period_end BIGINT NOT NULL DEFAULT 0,
  last_event_created BIGINT NOT NULL,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL
);
CREATE INDEX subscriptions_account ON subscriptions (account_id);

-- Machines an account's licence was issued to; at most LICENCE_MAX_MACHINES unreleased per account.
CREATE TABLE licence_machines (
  account_id TEXT NOT NULL REFERENCES accounts (id),
  machine_id TEXT NOT NULL,
  first_issued_at BIGINT NOT NULL,
  last_issued_at BIGINT NOT NULL,
  released_at BIGINT,
  PRIMARY KEY (account_id, machine_id)
);
