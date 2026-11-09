# StageCraft Cloud

The server side of StageCraft 3D's real-money marketplace, accounts and support (ADR 0032, `Docs/ADR/0032-real-money-marketplace-cloud-and-stage-inspector.md`).

- **Runtime:** Node.js 22 (>= 22.18) running TypeScript directly through Node's type stripping. No build step: `node src/main.ts`.
- **Dependencies:** one runtime dependency, `pg`. HTTP from `node:http`, crypto from `node:crypto`, tests from `node:test`.
- **Storage:** PostgreSQL in production, `node:sqlite` for tests and local development. Both run the same SQL.
- **Payments:** Stripe over its REST API (Checkout, refunds, Connect Express, transfers), settled only by signed webhooks. A fake provider for development.

Nothing here moves real money until Gevor makes decisions D1–D8 (see `docs/RUNBOOK.md`) and deploys it with live keys.

## Architecture

```
src/
  main.ts, cli.ts, bootstrap.ts   entry point, operator commands (migrate, grant-admin), wiring
  config.ts                       environment variables, validated at startup (.env.example)
  app.ts                          the routes of API v1 (ADR 0032 §4.1); handlers validate input and call one service function
  http/router.ts, rateLimit.ts    routing, body limits, auth levels, token buckets, security headers, request ids, access log
  money.ts                        bigint minor units, the 85/15 split, refund apportioning with cumulative caps (§3.2)
  ledger.ts                       double-entry journal: balanced per currency, checked in code and by the database (§3.3)
  orders.ts                       the order state machine (a pure transition table) and compare-and-set persistence (§3.4)
  commerce.ts                     creating orders and checkouts, idempotency, long poll, entitlements
  webhooks.ts                     Stripe webhooks: verify, store once, apply in one transaction (§3.5)
  payouts.ts, sellers.ts          hold period, seller balances, payout runs (Stripe transfers or manual), onboarding
  listings.ts, query.ts           publish (server re-validation), moderation states, query, download entitlement checks
  package/                        `.stage` reader, a port of StagePackage::Read (container, manifest, scene checks)
  auth/                           sessions, RFC 8628 device flow, OIDC (PKCE, RS256 ID token verification)
  support.ts, redaction.ts        support tickets, server-side scrubbing
  admin.ts, audit.ts              summary, order search, refunds, moderation; audit_log for every admin mutation
  blob/                           local folder (signed URLs) and S3 (SigV4 pre-signed URLs, Pod Identity / IRSA credentials)
  payments/                       provider interface, Stripe, fake, webhook signatures
migrations/                       ordered SQL files, applied with a schema_migrations table
deploy/k8s/                       EKS manifests (kustomize)
docs/RUNBOOK.md                   operations
```

**Money rules in one paragraph.** The client never sends an amount: the order takes the price from the server's listing and stores
the seller share (basis points) on the order. Only a verified webhook marks an order paid; in the same database transaction the
event id is stored, the order transitions, the sale is posted (cash +P, seller pending −seller, platform revenue −platform) and the
entitlement is granted. Refunds and lost disputes reverse the sale in the stored proportions, never more than was charged. After
the hold period a payout run moves each seller's available balance by Stripe transfer (US/UK/EEA/CA/CH) or as a manual payout the
admin confirms. Balances are always sums of journal lines; there is no balance column.

## Run locally

```sh
cd Services/StageCraftCloud
npm ci
npm start                      # http://localhost:8080 with SQLite (./data), dev sign-in, fake payments, local blobs
```

Development conveniences (refused when `NODE_ENV=production`):
- **Sign-in:** `POST /v1/auth/device`, open the returned `verificationUriComplete`, type any user name, then poll `/v1/auth/device/token`.
- **Payments:** the checkout URL is `/dev/fake-checkout/{session}`; its *Pay* button sends a signed `checkout.session.completed` through the real webhook path. `/dev/fake-onboarding/{account}` enables a seller's payouts.
- **Admin:** `node src/cli.ts grant-admin <accountId>` (roles live only in the database).

To run against Stripe test mode locally, see "Stripe test mode" in `docs/RUNBOOK.md`.

## Test

```sh
npm test                       # all tests on in-memory SQLite
npm run test:coverage          # the same, with Node's coverage table
npm run test:pg                # the same suite against a throwaway postgres:16-alpine container (Docker required)
```

The tests start real HTTP servers on ephemeral ports. Stripe and the OIDC provider are local fake HTTP servers that record and
assert the exact requests; nothing touches the network. `test/fixtures/real-writer-505EF34C.stage` was written by the UE client's
C++ writer and is read by the TS reader in `test/package.test.ts`.

## API

The routes, JSON field names and error codes follow ADR 0032 §4.1. Errors are always `{"error": {"code", "message"}}` with the
codes listed in `src/errors.ts`. Additions to the ADR table:
`GET /v1/me/listings`, `GET /v1/admin/support/{id}/screenshot`, `POST /v1/admin/sellers/{id}/manual-payouts`,
`GET /v1/admin/webhooks`, `GET /v1/admin/accounts/{id}`, `GET /v1/blobs/*` (local store only), and the browser pages
`/auth/verify/start`, `/checkout/success`, `/checkout/cancel`, `/seller/onboarding/done|refresh`.
