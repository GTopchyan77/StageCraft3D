# ADR 0021: Community marketplace, `.stage` packages and listing purchases

- **Status:** accepted. Phase A implemented (STATE.md #50). Phases B and C are planned below.
- **Related:**
  - ADR 0004 §4 and ADR 0010 §4 (scene files): a package carries a scene document in the existing, versioned scene format. It does not use a second serializer.
  - STATE.md #18 (economy, profile, commerce backend): listing purchases are a new validated economy operation. The profile stays single-writer.
  - ADR 0016 (AI assistant): Phase C adds marketplace intents.
  - Rules.md: Architecture (state by lifetime, request bridge, push not poll), Economy security standards 1–10, Engineering Standard §26 (save/load contracts), §29 (async), §55 (trust boundary), §66 (security boundary).

## 1. Problem

Gevor's milestone (2026-11-01): a Community Marketplace and Cloud Showcase. Creators publish, show and sell stage designs, cue lists and lighting setups. Others browse, preview and acquire them.

1. A `.stage` package format for complete stage projects (fixtures, multi-cell channels, DMX patch, truss geometry, cues).
2. A marketplace panel with filters (category, popularity, price) and a read-only 3D preview.
3. Assistant intents to search, preview and import marketplace content.
4. `TObjectPtr`, encapsulation, no per-frame tick, zero warnings, all tests, STATE.md.

What existed already:

- **Scene files** (`FStageSceneStore`, format v3). Every placed item is stored with its catalog path, transform, id and SaveGame state. That state covers fixture cells and attributes (#49), the DMX patch (#40) and span/truss geometry (#27, #45). The files also hold the environment and the cue list. Parsing is hardened against untrusted input.
- **The economy.** It sells only Asset Manager-scanned `UStageProductData` assets, settles purchases through `UStageCommerceBackend` (offline: `UStageLocalCommerceBackend`), and protects the profile with a salted SHA-1 integrity hash.
- **No server.** There is no cloud service, account system or API.

Gevor's choices, asked before design:

| Question | Choice |
|---|---|
| Catalog source | **A backend seam plus a local backend**. The local backend is a "community folder" of `.stage` files. An HTTP backend plugs in later, once a server API exists. |
| Monetization | **Credits only.** Real-money prices exist in the data but are refused unless the commerce backend is an authoritative server. |
| Delivery | **Phased.** A: format, backend, subsystem, purchases, tests. B: panel and read-only preview. C: assistant navigator. |

## 2. The `.stage` package (Phase A)

### 2.1 Container

The file is a binary container with named, compressed and hashed sections. JSON alone would have been bigger, and a section cannot be read without reading all of it. Browsing reads only the manifest, so a query never inflates a 10,000-item scene.

```
"STAGEPKG"           8 bytes magic
uint32 Version       container version (1)
uint32 SectionCount  1..8
SectionCount x { uint32 Tag (FourCC), uint32 Codec (0 stored, 1 zlib), uint64 StoredSize, uint64 RawSize, uint8 Sha1[20] }
payloads, in header order
```

All fields are little-endian (`FArchive` byte order).

| Section | Content | Raw cap |
|---|---|---|
| `MNFT` (required) | Manifest JSON (§2.2) | 256 KiB |
| `SCEN` (required) | The scene document: `FStageSceneStore::Serialize`, UTF-8 | 64 MiB |
| `THMB` (optional) | PNG thumbnail, checked by its signature | 2 MiB |

The whole package is capped at 96 MiB.

**Reading is a trust boundary.** The reader rejects a package when any of these hold:

- the magic or the container version is wrong;
- there are too many sections, or a tag repeats;
- a size is over its cap, or a payload runs out of bounds;
- trailing bytes remain;
- a section uses an unknown codec, or decompresses to anything other than exactly `RawSize`;
- a SHA-1 does not match;
- a required section is missing.

Unknown optional section tags are skipped, so a later minor addition still opens. The SHA-1 detects damage, not tampering. Authenticity, such as a server signature, belongs to a server backend (§66).

### 2.2 Manifest

```json
{ "format": "StageCraftPackage", "version": 1, "id": "<guid>", "title": "...", "description": "...", "author": "...",
  "kind": "StageDesign|LightingRig|CueList", "category": "StageCraft.Marketplace.Category.Concert", "keywords": ["..."],
  "price": { "credits": 250, "realMoney": { "amount": 0, "currency": "" } },
  "createdAt": "<ISO 8601>", "sceneVersion": 3,
  "stats": { "items": 120, "fixtures": 48, "cues": 12 }, "requiredItems": ["/Game/StageCraft/Items/..."] }
```

- **Validation.** `StagePackage::ValidateManifest` checks the id; the title (1–80 characters after trimming); the description (≤ 2000); keywords (≤ 16, each ≤ 32); credits (0–1,000,000); real money (non-negative, with a 3-letter ISO code when non-zero); and the stats (non-negative, `items` equal to the scene's item count). An unknown category reads as `Other`.
- **`requiredItems`** lists the distinct catalog paths. A browser can say what is missing before a download, and the stats are computed from the scene, never trusted from the manifest.
- **Portability.** Items made from imported models (ADR 0006) live only on the machine that imported them. A scene containing any of them is refused with `NotPortable` at publish. Embedding the model files is a follow-up.

### 2.3 Kinds

A package is always a full scene. `Kind` is the creator's label for what it is mainly for (a stage design, a lighting rig or a cue list), and browsing filters on it. Phase A imports every kind as a whole new scene. Merging a rig or a cue list into the open stage is a follow-up, because cues reference fixture ids that must be remapped.

## 3. Backend seam and the local backend (Phase A)

`UStageMarketplaceBackend` (abstract, `UObject`, chosen by `UStageMarketplaceSubsystem::MarketplaceBackendClass` in DefaultGame.ini) has three calls:

- `Query(Query, Callback)` returns listings matching text, kinds, category and price filter, sorted by popularity, date, price or title, one page at a time.
- `FetchPackage(ListingId, Callback)` returns the package bytes.
- `Publish(PackageBytes, Callback)` returns the published listing.

**Contract.** Every call answers **exactly once, on the game thread**. A server backend must re-validate the package and the price itself, because the client is never trusted.

`UStageLocalMarketplaceBackend` runs `FStageLocalMarketplaceStore` on one `UE::Tasks::FPipe`, so operations on the folder never overlap. It answers through `AsyncTask(GameThread)` with a weak `this`.

- **The folder.** `Saved/StageCraft/Marketplace/Community/<id>.stage`.
- **Popularity.** Download counts are kept in `downloads.json` (atomic writes) and sort Popular.
- **Query.** It reads only manifests. The filtering, sorting and paging are pure (`StageMarketplaceQuery::Apply`) and tested.
- **Publish.** It re-reads the whole package through the reader. It refuses an id that already exists (`AlreadyExists`) and writes atomically.

Its folder can be a shared network drive, which is enough for a studio LAN showcase without a server.

## 4. `UStageMarketplaceSubsystem` (Phase A)

It is a `UGameInstanceSubsystem`, because the catalog view and in-flight requests outlive a level. It holds no rules about money.

- **`RequestQuery(Query)`.** Newer queries supersede older ones by serial number, and a stale answer is dropped. Results are pushed on `OnListingsUpdated`. `GetListings` and `FindListing` read them.
- **`GetListingOwnership(Id)`** returns one of four states:
  - Free: no price.
  - Owned: the listing is in the profile.
  - Purchasable: a credits price, and spending is allowed.
  - Locked: a real-money price without a server, or a frozen profile.
- **`RequestAcquire(Id)`** gets a listing.
  - A free or owned listing is downloaded and imported directly.
  - A paid listing goes through `UStageEconomySubsystem::RequestListingPurchase` (§5) and is imported when the purchase completes.
  - At most one acquisition per listing is in flight.
  - The result arrives on `OnAcquireCompleted(ListingId, Result, SceneName)`.
- **Importing reuses the scene path.** The bytes are verified on a background task: the full read, plus the manifest id must equal the listing id. The scene is then written into the scenes folder under a unique name made from the title ("Festival Rig", "Festival Rig (2)", ...). A copy of the package is kept in `Marketplace/Library/`. Opening the result goes through the existing scene load, so catalog membership, entitlement locks, placement rules, undo and the unsaved-changes prompt all apply unchanged. The marketplace never spawns actors.
- **`RequestPublish(Request)`** publishes a saved scene: what is on disk is what gets published.
  - On the game thread it collects the catalog's light items, for the fixture count, and the author name from the profile.
  - A background task loads the scene, refuses imported models, builds the manifest from the scene and writes the package. The backend then publishes it.
  - A real-money price is refused with `ServerRequired` unless the commerce backend settles real money.
  - One publish runs at a time.

## 5. Listing purchases (Phase A)

Listings are not Asset Manager products, and the economy's catalog-only rule (Economy security 4) still holds for products. A listing is sold through a **new, separate validated operation**. A runtime-built `UStageProductData` is never made to pass the catalog check.

- **Calls.** `UStageEconomySubsystem::CanPurchaseListing(Id)`, `RequestListingPurchase(Id)` and `OnListingPurchaseCompleted(Id, Result)`.
- **The price authority is the backend's listing.** The economy reads the offer from `UStageMarketplaceSubsystem::FindListing`, never from the caller. It snapshots the offer per transaction, so a newer query that replaces the visible listings cannot change or lose a pending purchase.
- **Validation** is pure (`StageMarketplaceRules::ValidateListingPurchase`), in this order:
  1. profile integrity;
  2. listing known (`UnknownProduct`);
  3. not a free listing (`InvalidRequest`: free listings need no purchase);
  4. not already owned (`AlreadyOwned`);
  5. not pending (`PurchasePending`);
  6. no real money unless the commerce backend `CanSettleRealMoney()` (new, false for the local backend), otherwise `ServerRequired` (new result, appended);
  7. enough credits (`InsufficientFunds`).
- **Settlement.**
  - The purchase goes to `UStageCommerceBackend::ProcessPurchase`. `ProductId` is `StageListing:<guid>` and `TransactionId` is a fresh `FGuid`, which is the idempotency key.
  - When the backend answers, the snapshot is validated again.
  - `CommitPurchase` then debits the credits, records the purchase and adds the id to `FStagePlayerProfile::OwnedListings` (new SaveGame field), all or nothing.
  - `OnEntitlementsChanged` fires, since what the player owns changed.
- **Save compatibility.** `OwnedListings` is a new tagged property, so version-1 saves load unchanged. The integrity hash adds one `O:<guid>` part per owned listing, sorted. An old save has none and hashes exactly as before, so there is no version bump and no wallet reset.
- **Seller payouts** need a server. The local backend pays no one. That is acceptable only because it handles credits, not money.

## 6. Read-only 3D preview (Phase B, planned)

The preview opens the package's scene in a **separate preview world**: `UWorld::CreateWorld(EWorldType::GamePreview)` with its own `FPreviewScene`-style lighting, drawn in an `SViewport` inside the Marketplace panel. It does not use the main stage world.

- **It is read-only by construction.** The preview world has no player controller, spawn system, session subsystem or edit history, so no request path exists to mutate it. The rules do not have to remember to refuse edits.
- **It spawns items directly** from validated snapshots, after entitlement checks: locked items show as grey bounding boxes. It also applies cues for a cue preview.
- **It is torn down** when the panel closes.
- The main stage, its dirty flag and its undo history are never touched.

This needs an ADR addendum before code, because it is the first second world in the app.

## 7. Marketplace panel (Phase B, planned)

A Slate panel registered in `FStageWorkspaceShell`. It floats by default, like Sketch (it is a big tool window).

- **Search and filters.** A search box, kind chips, a category dropdown, a Free/Paid/Any filter and sort by Popular/Newest/Price/Title.
- **Results.** A virtualized `STileView` of listing cards with the thumbnail, title, author, stats and price, and a details pane with Preview, Get/Buy and Open.
- **Publish dialog.** It publishes the current saved scene with title, description, kind, category, keywords, price and a viewport thumbnail.
- **Requests and updates.** Every write goes through new `AModularPlayerController` requests (`RequestMarketplaceAcquire`, `RequestMarketplacePublish`), which broadcast refusals on `OnRequestRejected`. The panel binds to `OnListingsUpdated`, `OnAcquireCompleted`, `OnBalanceChanged` and `OnEntitlementsChanged`, and never polls.

## 8. Assistant navigator (Phase C, planned)

New `EStageAIIntent` values: `SearchMarketplace` (text, kind, category, max price, sort), `PreviewListing` (index or title from the last results), `AcquireListing` and `OpenImported`. They go into the local grammar (English and Armenian) and the Claude tool schema. Purchases always need an explicit confirmation turn ("Buy 'Festival Rig' for 250 credits?"), because the assistant must never spend on its own. Every action goes through the same controller requests as the panel.

## 9. Console (development builds only)

`StageCraft.Marketplace.Query [text]`, `.Publish <kind> <credits> <scene name>`, `.Get <listing id>` and `.Status`. They call the subsystem, so a scripted run can check publish, browse and import from the logs. They are compiled out of Shipping.

## 10. Alternatives considered

- **One JSON file with base64 sections.** Simpler, but browsing would parse every full scene, and it would be about a third bigger. Rejected.
- **`.pak` / `.utoc` containers.** These are built for cooked engine content and need cooking tools. A package holds user data, not assets. Rejected.
- **Selling listings as runtime-built `UStageProductData`.** This would break the catalog-only rule that stops forged products. Rejected in favour of §5.
- **Previewing in the main world under a "locked" flag.** Every edit path would have to remember the flag, and the user's stage would be replaced. Rejected in favour of §6.
- **Entitlement tags per listing.** Gameplay tags cannot be registered safely at runtime for user content. Rejected in favour of `OwnedListings`.

## 11. Consequences and follow-ups

- **Merge import** for `LightingRig` and `CueList` packages, with fixture-id remapping.
- **Embedding imported models** in packages (lifting `NotPortable`).
- **An HTTP backend** once a server API exists, with authentication, server-side validation, signatures and payouts. Real-money listings only then.
- **Manifest caching** in the local backend if a folder grows beyond a few hundred packages. Measure first (§22). Today a query loads each whole file to read its manifest. A bounded prefix read (header, table and manifest only) is the first step if browsing gets slow.
- **The real-money request.** `FStagePurchaseRequest` carries only the credits price. When a server backend can settle real money, the request must also carry the listing's real-money price and currency, and the server must look them up itself, never trusting the client's copy.
- **Versioned republishing** of a listing by its author.
