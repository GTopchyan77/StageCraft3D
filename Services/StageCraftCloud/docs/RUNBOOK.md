# StageCraft Cloud runbook

Operations for the service in `Services/StageCraftCloud` (ADR 0032). Nothing is deployed yet; this is the plan for when it is.

## 1. Decisions Gevor must make first (ADR 0032 §2)

| # | Decision | Where it lands in the service |
|---|---|---|
| D1 | Legal entity that sells: a **US or UK** company (Stripe cannot open an Armenian platform; Global Payouts to Armenia need a US/UK platform). | `STRIPE_PLATFORM_COUNTRY` (US or GB); the Stripe account the keys belong to. |
| D2 | Provider. Recommendation: Stripe (Checkout + separate charges and transfers + Connect). | `PAYMENT_PROVIDER=stripe`. |
| D3 | Tax (Stripe Tax or an adviser) before launch. Prices are tax-exclusive. | `STRIPE_AUTOMATIC_TAX` (default false). |
| D4 | Who pays the card fee. Assumed: the platform, out of its 15 %. | The split is `SELLER_SHARE_BPS` (8500); card fees are not yet posted to the ledger (follow-up in §8). |
| D5 | Hold period before payout. | `PAYOUT_HOLD_DAYS` (14). |
| D6 | Identity provider (Cognito, Auth0, Google, …). Any OIDC provider with discovery, PKCE and RS256 ID tokens works. | `OIDC_ISSUER`, `OIDC_CLIENT_ID`, `OIDC_CLIENT_SECRET`; redirect URI `https://<host>/auth/callback`. |
| D7 | AWS account, region, domain, cluster. | `deploy/k8s` placeholders `<ACCOUNT_ID>`, `<REGION>`, `<CERT_ARN>`, `cloud.example.com`. |
| D8 | Refund policy shown to buyers. Assumed: admin-initiated; a full refund revokes future downloads. | `POST /v1/admin/orders/{id}/refund`. |

## 2. Local development

```sh
npm ci && npm start          # SQLite, dev sign-in, fake payments; see README.md
npm test                     # SQLite
npm run test:pg              # Postgres 16 in Docker
```

## 3. Stripe test mode end to end (on a developer machine)

1. In the Stripe dashboard (test mode) copy the secret key `sk_test_…`. Enable Connect (Express) for the platform.
2. Install the Stripe CLI and forward webhooks:
   ```sh
   stripe listen --forward-to localhost:8080/v1/webhooks/stripe \
     --events checkout.session.completed,checkout.session.async_payment_succeeded,checkout.session.async_payment_failed,checkout.session.expired,charge.refunded,charge.dispute.created,charge.dispute.closed,account.updated
   stripe listen --forward-connect-to localhost:8080/v1/webhooks/stripe --events account.updated   # Connect events
   ```
   Each prints a `whsec_…` secret.
3. Start the service with `PAYMENT_PROVIDER=stripe STRIPE_SECRET_KEY=sk_test_… STRIPE_WEBHOOK_SECRETS=whsec_platform,whsec_connect npm start`.
4. Buy with test card `4242 4242 4242 4242`; try `4000 0000 0000 0259` (dispute) and a delayed method (e.g. SEPA debit) for the
   async path. Refund from `POST /v1/admin/orders/{id}/refund`; the ledger posts when `charge.refunded` arrives.
5. Check `GET /v1/admin/summary`: `grossSales - refunds = platformRevenue + sellerEarnings` must hold.

## 4. Deploy (EKS)

One-time:
1. **Postgres:** RDS for PostgreSQL 16, Multi-AZ, encryption on, in private subnets. Create database `stagecraft` and a user.
2. **S3:** bucket `stagecraft-cloud-packages`, Block Public Access on, SSE-S3 or SSE-KMS, versioning on. No bucket policy for public access: downloads use 5-minute pre-signed URLs.
3. **IAM:** role `stagecraft-cloud` with `s3:GetObject`, `s3:PutObject`, `s3:DeleteObject` on `arn:aws:s3:::stagecraft-cloud-packages/*`. Bind it to the `stagecraft/stagecraft-cloud` service account with an **EKS Pod Identity association** (preferred) or IRSA (the annotation in the manifest).
4. **Secrets Manager:** secret `stagecraft/cloud`, a JSON object with `DATABASE_URL` (`postgres://user:pass@host:5432/stagecraft?sslmode=require`), `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRETS`, `OIDC_ISSUER`, `OIDC_CLIENT_ID`, `OIDC_CLIENT_SECRET`.
5. **Cluster add-ons:** AWS Load Balancer Controller, External Secrets Operator with a `ClusterSecretStore` named `aws-secrets-manager`, metrics-server, a NetworkPolicy-enforcing CNI (VPC CNI network policy agent or Calico). Kubernetes >= 1.30 for the `preStop.sleep` hook.
6. **ACM certificate** for the host; DNS record to the ALB once it exists.
7. **Stripe:** live webhook endpoint `https://<host>/v1/webhooks/stripe` with the events listed in §3, plus a Connect endpoint for `account.updated`; pin its API version and set `STRIPE_API_VERSION` to the same value.

Each release:
```sh
docker build -t <ACCOUNT_ID>.dkr.ecr.<REGION>.amazonaws.com/stagecraft-cloud:<version> .
docker push <ACCOUNT_ID>.dkr.ecr.<REGION>.amazonaws.com/stagecraft-cloud:<version>
# set the tag in deploy/k8s/kustomization.yaml (images.newTag) and replace the placeholders (or use an overlay)
kubectl delete job -n stagecraft stagecraft-cloud-migrate --ignore-not-found
kubectl apply -k deploy/k8s                    # creates the migration Job and rolls the Deployment
kubectl wait -n stagecraft --for=condition=complete job/stagecraft-cloud-migrate --timeout=300s
kubectl rollout status -n stagecraft deploy/stagecraft-cloud
```
Migrations must stay backward compatible with the previous release (add columns and tables; drop only in a later release), because
the Job and the old pods run at the same time. `MIGRATE_ON_START` is false in production; the Job is the only migrator.

First admin: sign in once from the app, then `kubectl exec -n stagecraft deploy/stagecraft-cloud -- node src/cli.ts grant-admin <accountId>`.

Rollback: `kubectl rollout undo -n stagecraft deploy/stagecraft-cloud` (schema changes are additive, so the old version still runs).

## 5. Key rotation

- **Stripe webhook secret:** create the new endpoint secret (roll it in the dashboard with an overlap period), set `STRIPE_WEBHOOK_SECRETS=whsec_new,whsec_old` in Secrets Manager, wait for External Secrets (1 h, or annotate the ExternalSecret to force), restart the Deployment, then remove the old secret after the overlap ends.
- **Stripe secret key:** create a new restricted key (Checkout Sessions write, Refunds write, Accounts write, Account Links write, Transfers write), update `STRIPE_SECRET_KEY`, restart, then revoke the old key.
- **OIDC client secret:** rotate at the provider, update `OIDC_CLIENT_SECRET`, restart. Signing keys of the provider rotate by themselves (JWKS is re-read, at most once a minute, when an unknown `kid` appears).
- **Database password:** rotate in RDS / Secrets Manager, update `DATABASE_URL`, restart.
- **Sessions:** to sign everyone out, `UPDATE sessions SET revoked_at = <epoch ms> WHERE revoked_at IS NULL;`.

## 6. Incidents

**Webhook backlog or outage.** Stripe retries failed deliveries for up to 3 days with backoff. Every delivery is idempotent (event id
primary key; ledger entries keyed by business ref), so replays are safe.
1. Check `/readyz` and the logs for `webhook` lines with status 500 (`unhandled error`).
2. Fix the cause; Stripe redelivers. To speed it up, resend from the dashboard (Developers → Events → Resend) or `stripe events resend evt_…`.
3. Reconcile: `GET /v1/admin/webhooks?status=needs_attention` lists events stored but not applied (unknown order, amount mismatch, illegal transition). `GET /v1/admin/orders?state=OnHold` lists held orders; refund them in full with `POST /v1/admin/orders/{id}/refund` or investigate.
4. Cross-check with Stripe: every Checkout Session with `payment_status=paid` in the dashboard must have an order in `Paid`, `PartiallyRefunded`, `Refunded` or `Disputed`. A paid session whose order is still `CheckoutOpen` means its webhook never arrived: resend the event.
5. Ledger self-check: `SELECT currency, SUM(amount) FROM journal_lines GROUP BY currency;` must be 0 for every currency.

**Payout transfer stuck.** A payout stays `pending` when the transfer call timed out; the next `POST /v1/admin/payouts/run` retries it with the same idempotency key, so Stripe never pays twice. A `failed` payout returned its money to the seller's available balance.

**Suspected token leak.** Revoke the account's sessions (`UPDATE sessions SET revoked_at = … WHERE account_id = …`). Logs never contain tokens; tokens start with `scs_`, so `grep scs_` across logs must find nothing.

## 7. Rate limits

Per pod, in memory: 120 requests/minute per IP and per account, plus tighter limits on sign-in starts, publishing, orders,
onboarding and support. With N replicas the effective limit is N times higher; attach an AWS WAF rate-based rule to the ALB for a
cluster-wide limit.

## 8. Known gaps (follow-ups)

- **Card fees** are not posted yet (they need the charge's balance transaction from Stripe). Dispute fees are posted when a dispute is lost.
- **Global Payouts** adapter for Armenia (manual payouts until then).
- The server checks the scene's items, cue numbers and origin, but not the deeper show data (cue fixture states, effects, palettes, playbacks); the client's `StagePackage::Read` checks those when it acquires the package.
- The S3 store and AWS credential providers are written to the documented APIs and tested against a fake S3 and AWS's published SigV4 example, not yet against real AWS.
