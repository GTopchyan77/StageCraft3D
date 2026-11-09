# ADR 0032: Real-money marketplace, StageCraft Cloud, support center and the Stage Inspector (image to BOM)

- **Status:** implemented by Claude in auto mode on Gevor's brief of 2026-11-07 (STATE.md #77), awaiting Gevor's approval. **Nothing here moves real money until Gevor makes the decisions in §2 and deploys the service with live keys.**
- **Related:**
  - ADR 0021 (marketplace, `.stage` packages, `UStageMarketplaceBackend`, listing purchases, §14 unanswered backends).
  - ADR 0022 §11 (exports and the credits top-up seam).
  - ADR 0023 (credential vault).
  - ADR 0017 §2.7 (Claude reading a drawing): the Inspector uses the same pattern.
  - ADR 0014 (shopping and assembly list).
  - ADR 0015 (sketch to stage build plans).
  - Rules.md: Economy security standards 1–10, §19–20 and §66 (trust boundary), §26 and §55 (untrusted data), §65 (Shipping).

## 1. Problem

Gevor's brief (2026-11-07) asks for six things:

1. **Real-money marketplace.**
   - Commission split: 15 % to the platform, 85 % to the seller.
   - Payment: hosted checkout, settled only by server-side webhooks. Card data never touches StageCraft.
   - Bookkeeping: a ledger, order history, refunds and disputes, seller balances.
   - Hosting: AWS and Kubernetes, in Docker.
2. **In-app purchasing.** Browse, buy and publish inside StageCraft. A purchase is one click, then downloads and lands in the open scene.
3. **Stage Inspector.** A reference photo of a stage becomes a bill of materials (trusses, LED walls, fixtures, audio) mapped to the catalog, and can be placed.
4. **Support.** An in-app support center and bug report (steps, logs, screenshot).
5. **Admin backend.** Endpoints for sales, platform revenue, seller payouts, refunds and moderation.
6. **Quality gates.** 0 warnings on every target, tests, STATE.md, todo.md.

**What exists.** The client was built for this seam from the start:
- `UStageCommerceBackend::CanSettleRealMoney`;
- the `ServerRequired` refusal;
- `UStageMarketplaceBackend` (query, fetch, publish, unpublish);
- the per-transaction `FGuid` as an idempotency key;
- late-answer reconciliation (ADR 0021 §14).

There is no server, no account system and no payment provider.

## 2. Decisions only Gevor can make (open)

| # | Decision | What the code assumes until then |
|---|---|---|
| D1 | **Legal entity that sells.** Stripe cannot open a platform account in Armenia. Global Payouts, the only Stripe route that pays sellers in Armenia (AMD), can be sent only from a **US or UK** platform account (docs.stripe.com/global-payouts/recipient-countries). Connect cross-border payouts work only between the US, UK, EEA, Canada and Switzerland (docs.stripe.com/connect/cross-border-payouts). So selling needs a US or UK company (for example Stripe Atlas or a UK Ltd). | Provider-neutral code; a Stripe adapter; a fake provider for development. |
| D2 | **Provider.** Lemon Squeezy and Paddle are merchants of record for *one* seller. They offer no seller onboarding or split payouts, so they cannot run an 85 % marketplace. **Recommendation: Stripe** (Checkout + separate charges and transfers + Connect / Global Payouts). | Stripe adapter. |
| D3 | **Tax.** VAT/GST on digital goods is the seller of record's job. With Stripe the platform is the seller of record, so Stripe Tax (or a tax adviser) must be decided before launch. | Prices are tax-exclusive. Checkout is created with automatic tax **off**, behind one flag (`STRIPE_AUTOMATIC_TAX`). |
| D4 | **Who pays the card fee.** | The platform pays it out of its 15 %. The seller always gets exactly 85 % of the price (§3.2). One config value changes this. |
| D5 | **Hold period** before a sale can be paid out (the refund and dispute window). | 14 days (`PAYOUT_HOLD_DAYS`). |
| D6 | **Identity provider** for sign-in (Cognito, Auth0, Google, email links). | The server signs users in through any OpenID Connect provider (§5). Development uses a dev provider that exists only when `AUTH_MODE=dev`. |
| D7 | **AWS account, domain, deployment.** | Manifests and a runbook only. Nothing is deployed. |
| D8 | **Refund policy** shown to buyers. | Admin-initiated refunds; a full refund revokes future downloads. |

## 3. Money

### 3.1 Funds flow: separate charges and transfers

1. **Order.** The buyer asks the server for an order. The **server** reads the price from its own listing; the client never sends an amount.
2. **Checkout.** The server creates a Stripe Checkout Session for that amount on the **platform** account. The client opens Stripe's hosted page in the system browser. Card numbers and CVV go only to Stripe; StageCraft never sees or stores them (PCI SAQ A scope).
3. **Settlement.** Only the signed webhook `checkout.session.completed` (with `payment_status = paid`), or `checkout.session.async_payment_succeeded`, marks the order paid.
   - The ledger posts the sale and the buyer gets the entitlement.
   - The return page and the client's view of the order are display only.
4. **Payout.** After the hold period, a payout run moves each seller's available balance:
   - by a Stripe transfer to the seller's connected account where cross-border payouts allow it (US, UK, EEA, CA, CH);
   - otherwise as a **manual** payout batch. This is where Global Payouts to Armenia goes until its adapter is written against Gevor's account (follow-up §11).

Destination charges were rejected. They pay the seller at charge time, so the 14-day hold, Armenian sellers and the reversal of a refund after payout would all need workarounds.

### 3.2 Amounts and the split

- All money is `bigint` minor units with an ISO 4217 currency. Floats never appear.
- Only currencies in `SETTLEMENT_CURRENCIES` (default `USD`) may price a listing. The ledger never converts currencies.
- **The split, for price P:**
  - `seller = floor(P * 8500 / 10000)`;
  - `platform = P - seller`;
  - so the parts always sum to P, and any rounding goes to the platform.
  - The share is stored on the order when it is created. Changing the configuration never re-prices old orders.
- **Partial refund R of an order with share S:**
  - the seller's part is `floor(R * S / 10000)`, capped so that the cumulative seller refund never exceeds the seller's share;
  - the platform's part is the rest, capped the same way;
  - the cumulative refund never exceeds P.
- **Card fees** (D4) are a platform expense. They are posted when known from the balance transaction, and never reduce the seller's part.

### 3.3 Double-entry ledger

- **Balanced entries.** Every money event is one journal entry whose lines sum to zero per currency. The database checks this before commit.
- **Accounts:**
  - `cash:stripe`: the money held at the provider;
  - `platform:revenue`;
  - `platform:fees`;
  - `seller:<id>:pending`: within the hold period;
  - `seller:<id>:available`;
  - `seller:<id>:paid`;
  - `refunds`, `disputes`.
- **Events and their entries:**

| Event | Entry |
|---|---|
| Sale | cash +P; seller pending −seller; platform revenue −platform (credit balances are negative) |
| Hold elapsed | seller pending → seller available |
| Payout | seller available → seller paid, cash −amount |
| Refund | the sale reversed in the proportions of §3.2. If the seller was already paid, the seller's available balance goes negative and future sales recover it; nothing is clawed back automatically. |
| Dispute opened | the disputed amount is moved to `disputes` |
| Dispute won | the disputed amount is moved back |
| Dispute lost | the dispute is posted as a refund; the dispute fee is posted to `platform:fees` |

- **Balances are only ever sums of journal lines.** No mutable "balance" column exists that could drift (one source of truth, Rules §36).

### 3.4 Orders

- **States:**
  - Created → CheckoutOpen → Paid → (PartiallyRefunded | Refunded);
  - CheckoutOpen → Expired | Failed;
  - Paid → Disputed → (Paid | Refunded).
- **Transitions are a pure function.** An illegal transition (for example a refund of an unpaid order) is refused and logged, never forced.
- **Idempotency.** The client's `TransactionId` is the order's idempotency key, unique per buyer. Repeating a create returns the same order and the same checkout URL.
- **Owned listings.** A buyer who already owns the listing is refused (`AlreadyOwned`). An open checkout for the same listing is returned rather than duplicated.

### 3.5 Webhooks

- **Signature.** `Stripe-Signature` is verified as an HMAC-SHA256 of `t + "." + rawBody` with the endpoint secret. The comparison is constant-time, and the tolerance is 300 seconds.
  - Requests that fail are answered 400 and nothing is stored.
  - The raw body is verified before any JSON parsing.
- **Idempotent.** Each event id is inserted once (`webhook_events` primary key). A replay answers 200 and does nothing.
- **One transaction.** The event row, the order transition, the journal entry and the entitlement commit together, or none of them do.
- **Unknown order or event type:** stored, answered 200, and logged for the admin. Answering 200 stops Stripe from retrying something that can never succeed.
- **Amount check.** The amount and currency in the event must equal the order. A mismatch puts the order on hold for the admin and grants nothing.

## 4. StageCraft Cloud (the service) — `Services/StageCraftCloud/`

- **Runtime:** Node.js 22 and TypeScript, run with Node's built-in type stripping. HTTP comes from `node:http`, tests from `node:test`, and crypto from `node:crypto`.
- **Dependencies:** one runtime dependency, `pg`, the Postgres driver.
  - No web framework and no Stripe SDK; Stripe is plain HTTPS against its documented REST API.
  - This keeps the supply chain small for a service that handles money.
- **Storage:** PostgreSQL in production. Tests and local development use the built-in `node:sqlite`.
  - Both run the same SQL through one small driver interface.
  - The integration tests also run against Postgres in Docker.
- **Packages:** `.stage` files go to S3 in production (pre-signed download URLs, 5 minutes) and to a local folder in development.
- **Deployment (D7):**
  - a Dockerfile: non-root, read-only root filesystem, health checks;
  - Kubernetes manifests: Deployment, Service, Ingress for the AWS Load Balancer Controller, HPA, PodDisruptionBudget, NetworkPolicy, and secrets from AWS Secrets Manager through External Secrets;
  - a runbook.
- **Server re-validation (ADR 0021 §3 contract).** The server re-reads every uploaded `.stage` package with its own reader, which has the same rules as `StagePackage::Read`: magic, versions, section caps, codecs, SHA-1s, trailing bytes and the manifest rules. It never trusts the client's validation.
  - The author is the signed-in account, not the manifest's text.
  - A real-money price needs a seller account that has completed payout onboarding.
- **Moderation.** New listings are `PendingReview` and are visible only to their author until an admin approves them.

### 4.1 API (version 1, JSON, `Authorization: Bearer <session>`)

| Method and path | Who | Purpose |
|---|---|---|
| `POST /v1/auth/device` | anyone | Starts a device sign-in. Returns `deviceCode`, `userCode` and `verificationUri` (RFC 8628 shape). |
| `POST /v1/auth/device/token` | anyone | Polled by the client with `deviceCode`. Answers `authorization_pending`, `slow_down`, `expired_token`, or a session token. |
| `GET /auth/verify` → OIDC → `/auth/callback` | browser | The user signs in with the provider (D6), and the device code is approved. |
| `POST /v1/auth/logout` | user | Revokes the session. |
| `GET /v1/me` | user | Account, roles, seller status. |
| `GET /v1/listings` | anyone | Query (the `FStageMarketplaceQuery` fields). Shows approved listings only. |
| `GET /v1/listings/{id}` | anyone | One listing. |
| `GET /v1/listings/{id}/package?purpose=acquire\|preview` | user | Free listings, or paid listings the user owns. Redirects to a short-lived URL. A preview of a paid listing that is not owned is refused (`NotOwned`); the thumbnail is public. |
| `GET /v1/listings/{id}/thumbnail` | anyone | PNG. |
| `POST /v1/listings` | seller | Body: the `.stage` package (octet-stream, ≤ 96 MiB). It is validated, stored and queued for review. |
| `DELETE /v1/listings/{id}` | author | Unpublish. |
| `POST /v1/orders` | user | `{listingId, idempotencyKey}` → `{orderId, state, checkoutUrl}`. Free listings are refused here; they are acquired directly. |
| `GET /v1/orders/{id}?waitSeconds=0..25` | buyer | Order state. A long poll answers as soon as the state changes, so the client is told rather than polling every second. |
| `GET /v1/me/orders`, `GET /v1/me/entitlements` | user | History, and "restore purchases" on a new machine. |
| `POST /v1/me/seller/onboarding` | user | Returns a provider onboarding link (Stripe Account Link). |
| `GET /v1/me/seller/balance`, `GET /v1/me/seller/sales` | seller | Pending, available and paid balances; sales lines. |
| `POST /v1/webhooks/stripe` | Stripe | §3.5. |
| `POST /v1/support/tickets` | anyone (rate-limited) or user | §7. |
| `GET /v1/admin/summary?from&to` | admin | Gross sales, platform revenue, seller earnings, refunds, disputes, payouts, open moderation, open tickets. |
| `GET /v1/admin/orders`, `POST /v1/admin/orders/{id}/refund` | admin | Order search; a full or partial refund through the provider (the ledger posts when the webhook arrives). |
| `GET /v1/admin/payouts`, `POST /v1/admin/payouts/run`, `POST /v1/admin/payouts/{id}/mark-paid` | admin | Balances that are due; run a payout batch; confirm a manual payout. |
| `GET /v1/admin/moderation`, `POST /v1/admin/moderation/{id}` | admin | Review queue; `approve` or `reject` with a reason. |
| `GET /v1/admin/support`, `POST /v1/admin/support/{id}` | admin | Tickets and their status. |
| `GET /healthz`, `GET /readyz` | probes | Liveness, and readiness (database reachable). |

**Common rules for every endpoint:**
- **Errors** are `{error: {code, message}}`, with stable codes the client maps to `EStageMarketplaceResult` or `EStageEconomyResult`.
- **Limits.** Body limits apply per route. Rate limits apply per account and per IP.
- **Responses** carry security headers and a request id.
- **Auditing.** Every admin mutation writes `audit_log` with who, what and before/after.

## 5. Accounts and sign-in

- **Sessions.** Opaque 256-bit tokens. Only their SHA-256 is stored. They last 30 days, and logout revokes them.
- **Device flow.** The desktop app never handles a password and never runs a local web server.
  1. The app shows a short code and opens the verification page in the browser.
  2. The user signs in with the OIDC provider. The server uses authorization code with PKCE, and verifies the ID token's RS256 signature against the provider's JWKS, its issuer, audience, expiry and nonce.
  3. The app's long poll receives its session.
- **Roles:** `admin` is granted only by a database row, never by a token claim.
- **Storage on the client.** The session token is stored in the credential vault (ADR 0023, Windows DPAPI) as the `StageCraftCloud` credential. It is never written to config, logs or saves.

## 6. Client (UE)

- **`UStageCloudSubsystem`** (GameInstance) owns the connection: base URL (`[StageCraft.Cloud] BaseUrl`, empty = offline), sign-in state, the session token from the vault, `/v1/me`.
  - It pushes `OnCloudStateChanged`.
  - Requests are the engine HTTP module, answered on the game thread with weak `this`.
- **`UStageCloudMarketplaceBackend`** implements `UStageMarketplaceBackend` over `/v1/listings`. The JSON is parsed by pure, tested `StageCloudJson` functions; the server's answers are untrusted (§55).
- **`UStageCloudCommerceBackend`** implements `UStageCommerceBackend`. It is the only class for which `CanSettleRealMoney()` is true, and only while signed in.
  - **Real-money listing:**
    1. create the order;
    2. open `checkoutUrl` in the system browser;
    3. long-poll the order until it is Paid, Expired or Failed;
    4. answer exactly once.
  - **Credit-priced listings** are refused as `ServerRequired`. The cloud does not sell credits yet (ADR 0022 §11.4 stays closed).
- **Timeout.** The economy's 30 s answer deadline (ADR 0021 §14) cannot cover a person typing a card. A new virtual `UStageCommerceBackend::GetSettlementTimeoutSeconds(Request)` lets the cloud backend ask for the checkout's own lifetime (31 minutes).
  - Every other backend keeps the economy's value.
  - A late paid answer still reconciles through ADR 0021 §14. "Restore purchases" (`/v1/me/entitlements`) recovers anything else.
- **One click, straight into the scene.**
  - After the purchase settles, the existing acquire flow downloads the package, verifies it with `StagePackage::Read` and adds it to the library.
  - With "Open after purchase" (default on), the marketplace then opens it as the current scene through the existing scene-load request.
  - Merging into the open scene is still the ADR 0021 §2.3 follow-up (cue and fixture ids must be remapped).

## 7. Support center and bug report

- **`UStageSupportSubsystem`** with two pure, tested parts:
  - **`StageSupportReport`** builds the report: category, title, steps, expected and actual results, app version, OS, GPU, scene statistics, the last 256 KiB of the log, and an optional window screenshot (PNG ≤ 4 MiB).
  - **`StageSupportRedaction`** scrubs before anything leaves the machine:
    - API keys (`sk-ant-…`, `sk_live_…`, `whsec_…`, `Bearer …`);
    - e-mail addresses;
    - the Windows user name in paths.

  The server scrubs again.
- **The user sees exactly what will be sent:** the log and screenshot are opt-in checkboxes with previews.
- **Sending.**
  - Signed in: the report goes to `/v1/support/tickets`.
  - Offline or on failure: it is saved as a folder under `Saved/StageCraft/Support/<timestamp>/`, which the panel opens in Explorer for e-mail.
- **The Support panel** (Help menu) holds the form, links to the offline help articles (ADR 0030 H1) and the user's ticket status.

## 8. Stage Inspector (reference photo → bill of materials)

1. **Input.** The user drops or picks a photo (JPEG or PNG, ≤ 20 MB). It is resized to at most 1568 px on the long side and re-encoded as JPEG, like the sketch photo import.
2. **Reading (online, opt-in, costs API usage).** `UStageClaudeStageInspectorBackend` sends the image to the Claude Messages API with one strict tool, `stage_inventory`. The model only describes what it sees, never placing anything. Each line has:
   - `kind`: from a closed enum (truss straight, truss corner, truss tower, ground support, deck, LED wall panel, moving head spot/wash/beam, PAR/wash, strobe, blinder, followspot, line-array box, subwoofer, speaker stack, monitor wedge, mixing desk, lighting desk, barricade, haze machine, other);
   - `quantity`: integer 1..500;
   - `confidence`: 0..1;
   - `dimensions`: optional (width, height, length in metres, for trusses, LED walls and decks);
   - `notes`: optional;
   - `position`: a coarse region (stage left / centre / right, upstage / downstage, flown / floor).
3. **Validation (pure, `StageInspector::ParseResponse`).**
   - Unknown kinds are dropped and counted.
   - Quantities are clamped. At most 128 lines.
   - Text fields are length-capped.
   - A malformed answer is refused with a reason.
   - Truss and LED walls are converted to catalog units: truss length in standard 1, 2 and 3 m sections; LED area in 0.5 × 0.5 m panels when the catalog item is a panel.
4. **Catalog matching (pure, `StageInspector::MatchCatalog`).**
   - Each kind maps to catalog items by gameplay tag (the item categories and fixture types already in the catalog).
   - Unmatched kinds stay on the bill as "not in the catalog", so the real-world list is still complete.
5. **The Inspector panel shows the BOM:** kind, quantity, catalog match, confidence and real-world spec (weight and power from the catalog data when matched).
   - Editable quantities.
   - **Copy as CSV** and **Save CSV**. The ADR 0014 shopping list is computed from the built scene, so it is not something to add to; once the bill is built, the shopping list and reports cover it like any stage.
   - **Build in scene.** A layout from the coarse positions is converted to an `FStageSketchBuildPlan` (`Items`, ADR 0015) and applied as one undo step through the existing sketch builder path.
6. **Honest limit.** Counting from one photo is an estimate.
   - Every line carries its confidence, and lines below 0.5 are marked.
   - The panel says the BOM must be checked by a person before anything is ordered or rigged.
   - The Inspector never sets a price and never orders.

## 9. Security summary

| Threat | Control |
|---|---|
| A forged "paid" from the client | Settlement only via verified webhooks (§3.5). The client's view is display only. |
| A tampered price | The server reads the price from its own listing (§3.4). The client sends no amounts. |
| Webhook replay or forgery | HMAC with a time tolerance; event id uniqueness. |
| Double charge from retries | The idempotency key is unique per buyer. An open checkout is reused. |
| Card data exposure | Hosted checkout only; no card fields in any API or log. |
| Session theft | Only a hash is stored server-side; the client token lives in the DPAPI vault; logout revokes; 30-day expiry. |
| A malicious package | The server re-validates the container and manifest; caps; the author comes from the session; moderation before listing. |
| Personal data in bug reports | Redaction on both sides; opt-in log and screenshot; the user sees a preview. |
| Admin abuse | A database role (not token claims); audit log of every admin mutation. |
| Dev shortcuts shipping | The dev identity exists only with `AUTH_MODE=dev`, and the server refuses to start with it when `NODE_ENV=production`. On the client, cloud console commands are `#if !UE_BUILD_SHIPPING`. |

## 10. Alternatives considered

- **Lemon Squeezy or Paddle (merchant of record):** no multi-seller payouts (D2).
- **Destination charges:** no hold period and no Armenian payouts (§3.1).
- **A client-side Stripe SDK or embedded browser card form:** puts card data in the app's scope. Rejected.
- **A web framework, an ORM or the Stripe SDK on the server:** more supply chain for little gain. The service is small, and the ledger needs hand-written SQL anyway.
- **Running the Inspector on the server:** the photo would leave the user's machine through StageCraft. Today it goes from the user's machine straight to Anthropic with their own key, as the sketch photo import does (ADR 0017). A server proxy (with billing) is a follow-up once accounts exist.

## 11. Consequences and follow-ups

- **Deployment.** Gevor decides D1–D8. Then: Stripe test-mode keys → a staging deployment → end-to-end test purchases with Stripe test cards → live keys.
- **A Global Payouts adapter** (Armenia, AMD), written against Gevor's US or UK account once it has access. Until then, those payouts are manual batches the admin marks paid.
- **An admin web UI** over the admin endpoints. This phase delivers the endpoints.
- **Selling credits** for real money (ADR 0022 §11.4) through the same checkout.
- **Package signatures.** A server signature lets the client verify that a package came from the cloud.
- **Ratings and reviews,** now that accounts exist.
- **Merging** a purchased rig or cue list into the open scene (ADR 0021 §2.3).

## 12. As built (2026-11-07, STATE.md #77)

### 12.1 Names

- **Vision Inspector.** "Inspector" was already the property panel's name, so the photo-to-BOM feature is `StageVisionBom*` in code (panel `UStageVisionInspectorPanel`, tag `StageCraft.Panel.VisionInspector`, menu Edit › Draw › Vision Inspector).
- **The tool** is `stage_inventory`.
- **Support** is `UStageSupportSubsystem`, `UStageSupportPanel` and `StageCraft.Panel.Support` (menu: Community › Support & Bug Report).

### 12.2 Client classes

| Class | Role |
|---|---|
| `UStageCloudSubsystem` (GameInstance, Config) | Holds `BaseUrl` (empty = offline). Runs the device sign-in, keeps the session in the vault bound to its server address, and sends every request through one `Send`. Accepts only `/v1/` paths, adds the session only when asked, and a 401 ends the session. `Download` fetches handed-out addresses without the session. A test transport replaces the network in tests. |
| `StageCloudJson` | Pure wire format. Every answer is validated and capped. Unknown error codes deny. Only safe addresses are allowed (https, or http to this computer). |
| `UStageCloudMarketplaceBackend` | `/v1/listings` while the cloud is configured. Otherwise the community folder, so offline behaviour is unchanged. Packages come from `?redirect=false`, then a session-free download. |
| `UStageCloudCommerceBackend` | Real-money listings: order, hosted checkout in the browser, long polls (25 s) until paid, expired, failed or on hold, or 31 minutes. Already owned on the server counts as approved. Credits stay local. Cancelling answers the purchase as declined, once. |
| `UStageCommerceBackend` | Two new virtuals. `GetSettlementTimeoutSeconds` lets the economy's 30 s deadline become the checkout's own, clamped to 1 s..1 h. `CancelPurchase` abandons a waiting purchase. |
| `UStageEconomySubsystem` | `RequestCancelListingPurchase`. |
| `AModularPlayerController` | `RequestMarketplaceCancelPayment` and `RequestBuildFromVisionBom`. |
| `UStageMarketplaceWidget` | Account bar (sign in / code / sign out). Prices are shown in money ("$15.00"). One click on BUY opens the checkout, the button becomes PAYING IN BROWSER · CANCEL, and the stage opens when paid; a stage with unsaved changes still asks. |
| `DefaultGame.ini` | The cloud backends are the configured classes. With an empty `BaseUrl` they behave exactly like the local ones. |

**One bug fixed on the way.** `UStageSketchSubsystem::RequestPlanAssets` now also streams a plan's `Items`. Before, items placed "as they are" relied on loading at spawn.

### 12.3 Server (`Services/StageCraftCloud`)

As specified in §3–§5, with these additions. The service README and `docs/RUNBOOK.md` hold the details.

- **Orders**
  - `OnHold` is a new order state (an amount mismatch).
  - `PurchasePending` (409) is returned while an expired checkout still waits for Stripe's expiry webhook.
- **Publishing**
  - Credit-priced packages are refused: the cloud does not sell credits.
  - Seller onboarding takes `{country}`, which is fixed by the first call. Outside the US, UK, EEA, Canada and Switzerland the payout method is manual.
- **Support:** `clientReportId` makes sending a report idempotent (the first send answers 201, a resend 200 with the same ticket).
- **Package download:** `?redirect=false` answers JSON. The 302 stays the default.
- **New endpoints:** `GET /v1/me/listings` and `GET /v1/admin/webhooks`. There are also admin account, support screenshot and manual-payout switch endpoints, and dev-only fake checkout and onboarding pages, which exist only with `PAYMENT_PROVIDER=fake`.
- **Refusing to start:** production refuses dev authentication, the fake provider, local blobs, SQLite and http addresses.

### 12.4 Verification

- **Server:**
  - 71 tests pass on SQLite and on Postgres 16 in Docker. Line coverage is 100 % for the money, ledger, orders, webhook signature and processing, package reader, redaction, query and support modules, and 96.9 % overall.
  - `docker build` succeeds and the container runs healthy with a read-only root filesystem.
  - The Kubernetes YAML parses with `kubectl kustomize`. There is no cluster to validate it against.
- **Client:** StageCraft.Vision.* (6), .Support.* (4) and .Cloud.* (4) tests with a fake server. They cover:
  - the sign-in and session rules;
  - the marketplace backend, real-money checkout, retries and cancel;
  - redaction, and the support send and offline save.
- **End to end:** `Scripts/Tests/RunCloudEndToEnd.ps1` runs the real game against the real server on this computer (dev mode, fake provider). The script plays the browser from the game's log (`StageCraft.Cloud.LogBrowserUrls`, development builds only). The run checks:
  - the device sign-in;
  - a one-click purchase of a $15.00 listing;
  - the paid order (the signed webhook);
  - the download and import of the stage;
  - the server's books: 1500 gross, 1275 to the seller, 225 to the platform;
  - that no session token appears in the log, and sign-out.
- **Not verified:**
  - real Stripe (test mode needs Gevor's account, D1–D2);
  - real AWS;
  - a real identity provider (D6);
  - a paid Claude reading of a real photo (needs Gevor's API key and a photo; the parser is tested with recorded answers);
  - card fees in the ledger (follow-up).
