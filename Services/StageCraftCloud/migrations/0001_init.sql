-- StageCraft Cloud schema v1 (ADR 0032). Written in the subset of SQL that SQLite and PostgreSQL both run unchanged:
-- TEXT ids, BIGINT for every integer (times are epoch milliseconds, money is minor units, flags are 0/1), no serial columns.
-- Balances are never stored: they are sums of journal_lines (ADR 0032 §3.3, Rules §36).

CREATE TABLE accounts (
  id TEXT PRIMARY KEY,
  issuer TEXT NOT NULL,
  subject TEXT NOT NULL,
  email TEXT,
  display_name TEXT NOT NULL,
  created_at BIGINT NOT NULL,
  UNIQUE (issuer, subject)
);

-- Roles come only from this table, never from a token claim (ADR 0032 §5).
CREATE TABLE account_roles (
  account_id TEXT NOT NULL REFERENCES accounts (id),
  role TEXT NOT NULL CHECK (role IN ('admin')),
  granted_at BIGINT NOT NULL,
  PRIMARY KEY (account_id, role)
);

-- Only the SHA-256 of a session token is stored.
CREATE TABLE sessions (
  token_hash TEXT PRIMARY KEY,
  account_id TEXT NOT NULL REFERENCES accounts (id),
  created_at BIGINT NOT NULL,
  expires_at BIGINT NOT NULL,
  revoked_at BIGINT
);
CREATE INDEX sessions_account ON sessions (account_id);

CREATE TABLE device_codes (
  id TEXT PRIMARY KEY,
  device_code_hash TEXT NOT NULL UNIQUE,
  user_code TEXT NOT NULL UNIQUE,
  status TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'denied', 'consumed')),
  account_id TEXT REFERENCES accounts (id),
  interval_seconds BIGINT NOT NULL,
  last_polled_at BIGINT,
  created_at BIGINT NOT NULL,
  expires_at BIGINT NOT NULL
);

CREATE TABLE oidc_states (
  state_hash TEXT PRIMARY KEY,
  device_code_id TEXT NOT NULL REFERENCES device_codes (id),
  nonce TEXT NOT NULL,
  code_verifier TEXT NOT NULL,
  created_at BIGINT NOT NULL,
  expires_at BIGINT NOT NULL,
  used_at BIGINT
);

CREATE TABLE sellers (
  account_id TEXT PRIMARY KEY REFERENCES accounts (id),
  country TEXT NOT NULL,
  payout_method TEXT NOT NULL CHECK (payout_method IN ('stripe', 'manual')),
  provider_account_id TEXT UNIQUE,
  payouts_enabled BIGINT NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL
);

CREATE TABLE listings (
  id TEXT PRIMARY KEY,
  author_id TEXT NOT NULL REFERENCES accounts (id),
  author_name TEXT NOT NULL,
  title TEXT NOT NULL,
  description TEXT NOT NULL,
  kind TEXT NOT NULL,
  category TEXT NOT NULL,
  keywords_json TEXT NOT NULL,
  credits BIGINT NOT NULL,
  real_money_minor BIGINT NOT NULL,
  currency TEXT NOT NULL,
  item_count BIGINT NOT NULL,
  fixture_count BIGINT NOT NULL,
  cue_count BIGINT NOT NULL,
  package_bytes BIGINT NOT NULL,
  package_sha256 TEXT NOT NULL,
  has_thumbnail BIGINT NOT NULL,
  allow_export BIGINT NOT NULL,
  manifest_version BIGINT NOT NULL,
  required_items_json TEXT NOT NULL,
  -- Lower-cased title, description, author and keywords joined by U+0001, for word search with the same result on both databases.
  search_text TEXT NOT NULL,
  -- The lower-cased title as 4 hex digits per UTF-16 unit: sorts like the client's case-insensitive compare under any collation.
  title_key TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('PendingReview', 'Approved', 'Rejected', 'Unpublished')),
  review_reason TEXT,
  downloads BIGINT NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  published_at BIGINT,
  package_key TEXT NOT NULL,
  thumbnail_key TEXT
);
CREATE INDEX listings_status ON listings (status);
CREATE INDEX listings_author ON listings (author_id);

CREATE TABLE listing_downloads (
  account_id TEXT NOT NULL REFERENCES accounts (id),
  listing_id TEXT NOT NULL REFERENCES listings (id),
  first_at BIGINT NOT NULL,
  PRIMARY KEY (account_id, listing_id)
);

CREATE TABLE orders (
  id TEXT PRIMARY KEY,
  buyer_id TEXT NOT NULL REFERENCES accounts (id),
  listing_id TEXT NOT NULL REFERENCES listings (id),
  seller_id TEXT NOT NULL REFERENCES accounts (id),
  idempotency_key TEXT NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('Created', 'CheckoutOpen', 'Paid', 'PartiallyRefunded', 'Refunded', 'Expired', 'Failed', 'Disputed', 'OnHold')),
  amount_minor BIGINT NOT NULL CHECK (amount_minor > 0),
  currency TEXT NOT NULL,
  share_bps BIGINT NOT NULL CHECK (share_bps >= 0 AND share_bps <= 10000),
  seller_amount BIGINT NOT NULL,
  platform_amount BIGINT NOT NULL,
  seller_refunded BIGINT NOT NULL DEFAULT 0,
  platform_refunded BIGINT NOT NULL DEFAULT 0,
  checkout_session_id TEXT UNIQUE,
  checkout_url TEXT,
  checkout_expires_at BIGINT,
  payment_intent_id TEXT UNIQUE,
  hold_reason TEXT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  paid_at BIGINT,
  released_at BIGINT,
  UNIQUE (buyer_id, idempotency_key),
  CHECK (seller_amount + platform_amount = amount_minor),
  CHECK (seller_refunded >= 0 AND seller_refunded <= seller_amount),
  CHECK (platform_refunded >= 0 AND platform_refunded <= platform_amount)
);
-- At most one checkout in flight per buyer and listing: a second create reuses it (ADR 0032 §3.4).
CREATE UNIQUE INDEX orders_one_open_checkout ON orders (buyer_id, listing_id) WHERE state IN ('Created', 'CheckoutOpen');
CREATE INDEX orders_listing ON orders (listing_id);
CREATE INDEX orders_seller ON orders (seller_id);
CREATE INDEX orders_state ON orders (state);

CREATE TABLE order_events (
  id TEXT PRIMARY KEY,
  order_id TEXT NOT NULL REFERENCES orders (id),
  from_state TEXT NOT NULL,
  to_state TEXT NOT NULL,
  event TEXT NOT NULL,
  source TEXT NOT NULL,
  created_at BIGINT NOT NULL
);
CREATE INDEX order_events_order ON order_events (order_id);

CREATE TABLE entitlements (
  account_id TEXT NOT NULL REFERENCES accounts (id),
  listing_id TEXT NOT NULL REFERENCES listings (id),
  order_id TEXT NOT NULL REFERENCES orders (id),
  granted_at BIGINT NOT NULL,
  revoked_at BIGINT,
  PRIMARY KEY (account_id, listing_id)
);

CREATE TABLE journal_entries (
  id TEXT PRIMARY KEY,
  -- Business key of the money event (sale:<order>, refund:<charge>:<cumulative>, ...): a replay can never post twice.
  ref TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL,
  order_id TEXT REFERENCES orders (id),
  payout_id TEXT,
  memo TEXT,
  created_at BIGINT NOT NULL
);
CREATE INDEX journal_entries_created ON journal_entries (created_at);

CREATE TABLE journal_lines (
  entry_id TEXT NOT NULL REFERENCES journal_entries (id),
  line_no BIGINT NOT NULL,
  account TEXT NOT NULL,
  currency TEXT NOT NULL,
  amount BIGINT NOT NULL CHECK (amount <> 0),
  PRIMARY KEY (entry_id, line_no)
);
CREATE INDEX journal_lines_account ON journal_lines (account, currency);

CREATE TABLE refund_requests (
  id TEXT PRIMARY KEY,
  order_id TEXT NOT NULL REFERENCES orders (id),
  amount BIGINT NOT NULL CHECK (amount > 0),
  status TEXT NOT NULL CHECK (status IN ('submitted', 'failed', 'settled')),
  provider_refund_id TEXT,
  requested_by TEXT NOT NULL REFERENCES accounts (id),
  reason TEXT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL
);
CREATE INDEX refund_requests_order ON refund_requests (order_id);

CREATE TABLE disputes (
  id TEXT PRIMARY KEY,
  order_id TEXT NOT NULL REFERENCES orders (id),
  amount BIGINT NOT NULL,
  currency TEXT NOT NULL,
  status TEXT NOT NULL,
  is_inquiry BIGINT NOT NULL,
  created_at BIGINT NOT NULL,
  closed_at BIGINT
);

CREATE TABLE payouts (
  id TEXT PRIMARY KEY,
  seller_id TEXT NOT NULL REFERENCES accounts (id),
  currency TEXT NOT NULL,
  amount BIGINT NOT NULL CHECK (amount > 0),
  method TEXT NOT NULL CHECK (method IN ('stripe', 'manual')),
  status TEXT NOT NULL CHECK (status IN ('pending', 'paid', 'failed')),
  provider_transfer_id TEXT,
  reference TEXT,
  failure TEXT,
  created_by TEXT NOT NULL REFERENCES accounts (id),
  created_at BIGINT NOT NULL,
  completed_at BIGINT
);
CREATE INDEX payouts_seller ON payouts (seller_id);

CREATE TABLE webhook_events (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('processed', 'ignored', 'needs_attention')),
  detail TEXT,
  payload TEXT NOT NULL,
  received_at BIGINT NOT NULL
);
CREATE INDEX webhook_events_status ON webhook_events (status);

CREATE TABLE support_tickets (
  id TEXT PRIMARY KEY,
  account_id TEXT REFERENCES accounts (id),
  category TEXT NOT NULL,
  title TEXT NOT NULL,
  description TEXT NOT NULL,
  -- Only for anonymous reports; never redacted (it is how support answers), never logged in full.
  contact_email TEXT,
  -- acct:<accountId>:<clientReportId> or ip:<sha256(ip)>:<clientReportId>: a resend returns the same ticket.
  dedupe_key TEXT UNIQUE,
  steps TEXT NOT NULL,
  expected TEXT NOT NULL,
  actual TEXT NOT NULL,
  app_version TEXT NOT NULL,
  os TEXT NOT NULL,
  gpu TEXT NOT NULL,
  scene_stats TEXT NOT NULL,
  log_tail TEXT NOT NULL,
  screenshot_key TEXT,
  status TEXT NOT NULL CHECK (status IN ('open', 'in_progress', 'resolved', 'closed')),
  admin_note TEXT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL
);
CREATE INDEX support_tickets_account ON support_tickets (account_id);
CREATE INDEX support_tickets_status ON support_tickets (status);

CREATE TABLE audit_log (
  id TEXT PRIMARY KEY,
  actor_id TEXT NOT NULL REFERENCES accounts (id),
  action TEXT NOT NULL,
  target TEXT NOT NULL,
  before_json TEXT,
  after_json TEXT,
  created_at BIGINT NOT NULL
);
CREATE INDEX audit_log_created ON audit_log (created_at);
