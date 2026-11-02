# ADR 0022: DCC bridge — send stages to Blender, 3ds Max, Cinema 4D and Maya

- **Status:** accepted. Phase D1 implemented (STATE.md #53). Phase D3 implemented (STATE.md #54, §11 addendum); §4 is amended by §11.1: a publisher may allow buyers to export.
- **Related:**
  - ADR 0021: marketplace listings and `.stage` packages. This ADR adds the scene origin that keeps bought stages inside the app (§4).
  - ADR 0019 §3: the threading pattern of the DMX network receiver, reused for the bridge server (§7).
  - ADR 0004 §4 / ADR 0010 §4: the versioned scene format (now version 4).
  - Rules.md: Architecture (request bridge, push not poll, state by lifetime), Economy security standards 3 (deny by default) and 8 (entitlements), Engineering Standard §20/§55/§66 (trust boundaries), §29 (async), §59 (handles), §65 (Shipping safety).

## 1. Problem

Gevor's brief (2026-11-01) asked for four things:

1. A buy-and-send screen with live credits.
2. A universal bridge to Maya, Blender, 3ds Max and Cinema 4D, using USD, glTF and FBX.
3. Text- and image-to-3D generation in Armenian and English, with optimisation, retopology and LODs.
4. Bilingual semantic search.

These are four projects. Gevor's choices, asked before design:

| Question | Choice |
|---|---|
| First phase | **The DCC bridge.** The other three build on it (§8). |
| Which stages may leave the app as files | **The user's own scenes only.** Stages acquired on the marketplace stay inside StageCraft. |
| Outside models for search and generation | **Pluggable providers, starting offline.** This applies to D4 and D5. |

What existed:

- Saved scenes (`FStageSceneStore`, format v3).
- An isolated spawn path, `StageMarketplacePreview::SpawnScene`: a GamePreview world with no session, rules or play.
- `Sockets`/`Networking` (ADR 0019).
- A runtime model import path (ADR 0006).
- No exporter of any kind.

Findings that shaped the design:

- **glTF exporter.** UE's `GLTFExporter` plugin is a Runtime module, so `UGLTFExporter::ExportToGLTF` works in Game and Shipping builds.
  - Without `bAllowCPUAccess` it reads vertex and index buffers back from the GPU. That is slower, but not a failure.
  - It writes static, instanced and skeletal meshes, lights and cameras. It does not write `UProceduralMeshComponent`, which decks, stairs and arc trusses use.
- **No runtime FBX or USD writer.** UE has neither at runtime. FBX export is editor-only, and the FBX SDK has its own licence terms.
- **Native glTF import in each app:**
  - Blender: yes.
  - Cinema 4D: 2023 and later.
  - 3ds Max: depends on the version.
  - Maya: no native glTF import. It ships Maya USD.

## 2. Decision: formats (D1)

- **GLB (glTF 2.0 binary)** is what StageCraft writes, using the engine exporter.
  - Geometry and materials only. Lights are not exported, because they would arrive at intensities that mean nothing in the other app.
  - Material baking is disabled: it needs the editor.
- **USD (`.usda`)** comes in D2, from a writer of our own. That gives Maya a native path. The wire protocol already carries `"format"`.
- **FBX is not offered.** There is no runtime writer, and every target app imports GLB or USD.

## 3. Decision: what is exported

`FStageSceneExporter` (Private/Bridge) does this:

1. **Reads a saved scene.** It never reads the live stage, so the gizmo, ghosts, grid and unsaved edits are never part of an export, and the stage is never touched. The panel says when the open stage has unsaved changes.
2. **Refuses a marketplace stage** (§4).
3. **Exports only items the user may place.** An item is exportable when it is free or its exact entitlement is owned, the same rule a scene load applies. Items that are not owned, and items not in this build's Library, are counted and left out.
4. **Spawns into a private GamePreview world** with `StageMarketplacePreview::SpawnScene`.
5. **Turns procedural parts into transient static meshes.**
   - It uses the engine's `BuildMeshDescription(UProceduralMeshComponent*)` and `UStaticMesh::BuildFromMeshDescriptions` with fast build and CPU access.
   - Material slots follow the polygon groups that `BuildMeshDescription` creates.
6. **Writes `<name>.writing.glb` next to the target, then moves it into place.** A reader never sees a partial file. The world is destroyed before returning.

The export is synchronous on the game thread, because UObjects and the exporter require it.

- **Measured (R53):** R42_Sketch (15 items, 2 procedural parts, 200 KB) took 0.54 s on the first export and 0.02 s once warm.
- **Large scenes:** if they hitch noticeably, D2 moves the file write off the game thread.

## 4. Decision: own stages only (scene format v4)

`FStageSceneDocument::OriginListingId` records the listing a stage was acquired as.

- **Set:** by `ImportPackage`, whatever the package said.
- **Kept:** `UStageSceneComponent` keeps it while that stage is open. `CaptureStage` writes it into every save and autosave, so Save As produces a copy that is still refused.
- **Written:** only when set.
- **Read:**
  - Files older than version 4 load as the user's own.
  - A present but unreadable or zero id makes the file `Corrupt`. Deny by default.
- **Refusals:** export (`EStageBridgeResult::NotOriginal`) and publishing (`EStageMarketplaceResult::NotOriginal`) both refuse a stage that has an origin. Republishing someone else's stage under a new name was possible before this change.
- **Limits:**
  - Clear Stage keeps the origin.
  - Someone can rebuild a bought stage by hand, and a hand-edited file can drop the field. Like every client-side check, this deters casual copying and is not a security boundary.
  - Scenes imported before format v4 have no origin, because there was nothing to record it in.

## 5. Decision: the wire protocol

**Hub and spoke.** StageCraft listens. Each app's add-on connects as a client.

- **Transport:** TCP on **127.0.0.1 only**, port 47830. `-StageBridgePort=<n>` overrides it.
  - Other machines cannot reach the bridge, whatever the firewall says.
  - Browsers cannot open raw TCP connections, so a web page cannot talk to the bridge. That is the reason it is not a WebSocket.
- **Framing:** newline-delimited UTF-8 JSON, at most 64 KB per line, at most 4 connections.

| Direction | Message |
|---|---|
| add-on → StageCraft | `{"type":"hello","protocol":1,"token":"<64 hex>","app":"blender","version":"4.3.0"}` |
| StageCraft → add-on | `{"type":"welcome","protocol":1,"client":3}` |
| StageCraft → add-on | `{"type":"asset","id":"<hex>","format":"glb","name":"…","path":"<outbox file>","sha1":"<hex>","bytes":n}` |
| add-on → StageCraft | `{"type":"ack","id":"<hex>","ok":true}` or `{… "ok":false,"error":"…"}` |
| StageCraft → add-on | `{"type":"bye","reason":"…"}`, then the connection closes |

- **Files never travel over the socket.** The add-on reads the outbox file named in the message.
  - It checks that the path resolves inside the outbox, and that the size and SHA-1 match, before the app's importer sees it.
  - The outbox copy is deleted once the app answers.
- `Tools/DCCBridge/common/stagecraft_bridge_client.py` is the other end of `StageBridgeProtocol.h`. The two change together.

## 6. Decision: trust

- **Off by default.** The bridge listens only between the user switching on "Allow 3D apps to connect" and switching it off (or the app closing). The setting is not remembered.
- **Pairing token.**
  - Each start makes a fresh 256-bit token from two GUIDs (CoCreateGuid, cryptographically random on Windows).
  - It is written to `%LOCALAPPDATA%\StageCraft\Bridge\session.json` together with the port and outbox. Other Windows users cannot read that folder.
  - The file is deleted when the bridge stops.
  - Tokens are compared in constant time.
- **The handshake is checked on the socket thread.**
  - A connection that sends anything but a valid hello with the token within 5 s is told why (`bye`) and closed.
  - Nothing about an unauthenticated connection reaches the game thread. It is only logged, without the presented value.
- **After hello, only acks are accepted.** An unknown message type or an over-long line closes the connection.
  - Free text from the client (app version, error) is capped and stripped to printable characters.
  - An ack counts only for the pending asset id from the same client.
- **Unchanged:** the economy is untouched. Sending is free, and nothing a client says changes the profile, the stage or any file except the outbox copy it acknowledges.

## 7. Decision: ownership and lifetime

- **`FStageBridgeServer`** (non-UObject; one `FRunnable` worker thread) owns the listening socket and every client socket.
  - The worker accepts connections, reads, checks the handshake, parses messages and writes queued lines.
  - It waits on the listener in 50 ms slices when idle. That is message latency, not a game-thread cost.
  - Events go into a locked queue. The first event after a drain asks the game thread for one drain, the same pattern as ADR 0019 §3.2.
  - `Stop` joins the worker before the listener closes, and is idempotent.
- **`UStageBridgeSubsystem`** (GameInstance, so it survives level travel) owns the server, the token, the session file, the outbox, the connected-app list and the single in-flight send.
  - A send ends on the app's ack, its disconnect, `StopBridge`, or a 120 s timer (handle kept and cleared).
  - `Deinitialize` stops everything.
  - Delegates: `OnStateChanged`, `OnClientsChanged`, `OnSendCompleted`.
- **`AModularPlayerController`** has `RequestSetBridgeEnabled`, `RequestExportScene` and `RequestSendScene`. Refusals are reported on `OnRequestRejected`.
- **`UStageSendToWidget`** (File › Send to 3D App…, a floating window) reads the subsystem, writes through the controller, and is push-bound. Its contents:
  - the switch;
  - the saved-scene picker;
  - one button per connected app (Maya is shown but disabled until D2);
  - "Save as .glb file…", which uses the new `StageFileDialog::SaveFile`: DesktopPlatform, or COM `IFileSaveDialog` in Shipping.

## 8. Add-ons and roadmap

**Add-ons** live in `Tools/DCCBridge/`. They use only Python's standard library, plus each app's own API.

| App | File | Import | Verified |
|---|---|---|---|
| Blender 3.6+ | `blender/stagecraft_bridge/` (add-on, sidebar panel, `bpy.app.timers`) | `bpy.ops.import_scene.gltf`, into a "StageCraft - <name>" collection | Yes, headless Blender 4.3 (R53) |
| Cinema 4D 2023+ | `cinema4d/stagecraft_bridge/stagecraft_bridge.pyp` (command + MessageData timer) | `c4d.documents.MergeDocument` | No (not installed here) |
| 3ds Max | `3dsmax/stagecraft_bridge.py` (Qt window + QTimer) | `importFile` (versions with a glTF importer) | No (not installed here) |
| Maya | `maya/stagecraft_bridge.py` (connects and shows its state) | Waits for USD (D2) | No (not installed here) |

The add-ons poll their socket from a main-thread timer every 0.25–0.5 s. None of these apps offers socket events to Python, and scene changes must happen on their main thread.

**Roadmap** (each phase gets its own ADR addendum):

- **D2:** our own USD (`.usda`) writer for meshes, transforms and display colours; the Maya import; export off the game thread for large scenes.
- **D3:** the buy-and-send screen: live credits (`OnBalanceChanged`), the existing economy purchase path and confirmation (ADR 0021 §5, §13.3), and a target picker. A bought stage still cannot be exported, by §4. "Send" for marketplace content means opening it in StageCraft, unless Gevor changes that decision per listing (an "allow export" flag set by the publisher).
- **D4:** bilingual search. An offline provider first (an Armenian–English synonym and tag dictionary, query expansion with fallback to keyword search), behind a provider interface that a multilingual embedding service can implement later.
- **D5:** AI generation. A provider interface for text/image-to-3D services, plus post-processing: simplification and LODs via the engine's runtime mesh tools, then format conversion. It needs a chosen provider and an API key.

## 9. Alternatives considered

- **WebSocket server.** Rejected: browsers can open WebSockets to localhost, which widens the attack surface. Raw TCP is simpler and enough.
- **Streaming the file over the socket.** Rejected: the outbox path plus SHA-1 is simpler, needs no framing for binary data, and the file stays inspectable.
- **Each app running its own server, with StageCraft connecting to it.** Rejected: StageCraft would need a port per app and a discovery mechanism. The hub-and-spoke shape matches "StageCraft is the hub", and one token file serves all add-ons.
- **Our own glTF writer.** Rejected for D1: the engine exporter handles meshes, materials, instancing, GPU readback and GLB packing. Only procedural meshes needed bridging.
- **Exporting the live stage.** Rejected: it would include editor helpers and unsaved edits, and could not apply the own-stages rule.

## 10. Consequences

- New module dependencies: `GLTFExporter` (plugin enabled in the `.uproject`), `MeshDescription`, `StaticMeshDescription`.
- Scene format version 4. Older builds refuse version 4 files as VersionMismatch, by the existing rule.
- New dev console commands (compiled out of Shipping): `StageCraft.Bridge.Start`, `Stop`, `Status`, `Export <scene>`, `Send <id|first> <scene>`, `Screenshot`.
- Tests: `StageCraft.Bridge.Protocol`, `.SceneOrigin`, `.ExportRefusals`, `.ProceduralConversion`, `.Server` (real loopback sockets).
- End to end: `Tools/DCCBridge/tests/blender_receive.py` with headless Blender.

## 11. Phase D3 addendum: acquire and send (2026-11-02, STATE.md #54)

Gevor's brief (2026-11-02) asked for an acquire screen with:
- live balance and subscription checks;
- top-up;
- a target picker covering DCC apps, AI generators and disk;
- API key management.

His choices when asked:

| Question | Choice |
|---|---|
| Bought stages leaving the app (§4) | **A publisher flag.** The publisher decides per listing. |
| Scope | **D3 plus an AI tool seam.** A provider interface, secure key storage and an offline mock. No live provider (ADR 0023). |
| Top-up and subscriptions | **A seam with a clear refusal.** Offline, the answer is always ServerRequired. |

Not built, by decision or by constraint:
- **FBX:** there is no runtime writer (§2).
- **USD:** D2. Maya stays "needs USD".
- **WebSocket:** rejected in §9.
- **Checking Maya, 3ds Max or Cinema 4D licences:** StageCraft has no way to verify them. The honest signal is that the app's add-on is connected.

### 11.1 The publisher's export flag (amends §4)

**Package format (manifest version 2).**
- `FStagePackageManifest::bAllowExport` is written as `"allowExport"`. It is off by default, set on the publish page ("where buyers may use it").
- **Deny by default:** a version 1 package reads as false. A version 2 manifest without the field, or with anything other than a JSON boolean, is Corrupt. `TryGetBoolField` would accept the string "yes", so the parser checks the value type.
- The flag reaches listings as `FStageMarketplaceListing::bAllowsExport`, for display and target choice.

**Source of truth: the library package.**
- `UStageMarketplaceSubsystem::CheckExportRights(OriginListingId)` reads `Library/<id>.stage`. That is the publisher's bytes exactly as they were acquired.
- It then applies `StageMarketplaceRules::ResolveExportRights`. The result is allowed only when all of these hold:
  - the manifest is present and readable;
  - it is that listing's manifest;
  - the flag is set;
  - the listing is free, or the profile owns it.
- `FStageSceneExporter::ExportSavedScene` asks for these rights instead of refusing every stage with an origin. File export and send both pass through it.
- The scene file still carries only `OriginListingId`, so the scene format is unchanged.

**Republishing a bought stage is still refused, whatever the flag.** Export rights are not resale rights.

**Limits:**
- Losing the library copy now loses the export rights (NoLibraryCopy). Getting the listing again restores them.
- A hand-edited library package can flip the flag. Like every client-side check, this deters casual copying and is not a security boundary (Rules.md §66).

### 11.2 Orders: `UStageAcquireDispatchSubsystem`

An order is "get this listing, then send it there".

**Types:** `FStageAcquireOrder` holds the listing, `ExpectedCredits` and an `FStageDispatchTarget`. Target kinds: OpenInStageCraft, SaveFile, BridgeApp, AITool.

**Lifetime:** a GameInstance subsystem owns the order, so it outlives the dialog and the panel. There is one order at a time.

**Flow:**
1. `RequestOrder` checks, in order:
   - Busy;
   - the listing is in the current results (NotFound);
   - its price equals the confirmed one (PriceChanged);
   - a SaveFile target has an absolute .glb path;
   - the target is available.
2. It calls `UStageMarketplaceSubsystem::RequestAcquire`. The economy's validate → settle → re-validate → commit path is unchanged.
3. On `OnAcquireCompleted` it checks the target again, then delivers:
   - Open: done; the panel opens the scene with its unsaved-changes confirm.
   - SaveFile: `ExportSceneToFile`.
   - BridgeApp: `SendSceneToApp`, finished by the app's answer.
   - AITool: export into `Saved/StageCraft/AITools/Outbox`, then `UStageAIToolSubsystem::SubmitModel`. The outbox copy is deleted when the tool answers.
4. It broadcasts `OnOrderProgress` and `OnOrderCompleted`.

**One rule for display and enforcement:** `StageDispatchRules::EvaluateTarget` gives Available, NeedsExportRights, NotConnected, NeedsUsd, NeedsKey, KeyRejected, Busy or Unavailable. The exporter's rights check (§11.1) is the last gate.

**Failed delivery:** a purchase is never rolled back because a delivery failed. The stage is in the user's scenes, and the result says both parts ("…is in your scenes, but it could not go to Blender: …").

**Delegates:** the delegates of the other subsystems are bound only while an order waits for them.

**Controller:** `AModularPlayerController::RequestAcquireAndDispatch`. Refusals go to `OnRequestRejected`.

### 11.3 The acquire dialog

`UStageAcquireDialogWidget` replaces one-click buying. Before this change, the panel's "BUY · N CREDITS" spent credits at once; only the assistant asked first.

**Hosting.** A dimmed overlay layer inside the Marketplace panel holds it. `FStageWorkspaceShell::ShowDialog` does not cover floating panel windows. Removal is deferred to the next tick, because it is asked from the dialog's own callbacks.

**Listing section:** the title, the byline and the publisher's export rule.

**Wallet section:**
- Balance, price and balance afterwards, live through `OnBalanceChanged`.
- The reason buying is blocked, from the read-only `CanPurchaseListing`: shortfall, frozen profile, real money, pending.
- The store line: backend name, whether credits can be bought, and the entitlement count. Subscriptions arrive as entitlements.
- When credits are short: GET CREDITS (`RequestCreditTopUp`). Development builds also show DEV: ADD THE MISSING CREDITS (`RequestDevGrantCredits`, false in Shipping, collapsed there).

**Targets:**
- Open in StageCraft;
- Save as .glb (the path is chosen when the row is picked, before buying);
- each connected bridge app;
- each AI tool that takes models.

Rows that cannot be used stay visible with the reason, and cannot be picked. Links open Send to 3D App and Services & Keys.

**Confirm button.** It names the action ("BUY FOR 120 CREDITS & OPEN", "GET & SEND TO BLENDER") and sends the price on screen as `ExpectedCredits`.

**Lifetime.** Push-bound, rebuilt on the next tick, no tick. Closing never cancels an order.

### 11.4 Top-up seam

- **Commerce backend.** `UStageCommerceBackend::CanSellCredits` (false by default) and `ProcessCreditTopUp`, which declines by default and answers the amount granted.
- **Economy.** `UStageEconomySubsystem::RequestCreditTopUp` checks, through the pure rule `StageTopUpRules::Validate`:
  - profile and integrity;
  - a concrete currency;
  - an amount of 1..1,000,000 that cannot overflow the balance;
  - one top-up in flight at a time;
  - a store that sells credits, otherwise ServerRequired.
- **On the backend's answer** it re-checks the transaction, and refuses a grant larger than the request. It applies the credits with `AdjustBalance`; the profile keeps a single writer. The outcome goes to `OnCreditTopUpCompleted`.
- **Price.** The request carries no price. A server looks it up and treats `TransactionId` as an idempotency key.
- **Subscriptions** are entitlement tags granted by such a server. Expiry needs a server too, so it is not modelled offline.

### 11.5 Consequences

- Manifest version 2. Older builds refuse version 2 packages as VersionMismatch, by the existing rule.
- **New dev commands** (compiled out of Shipping):
  - `StageCraft.Marketplace.PublishExportable`, `.Order`, `.Dialog`;
  - `StageCraft.Shop.TopUp`;
  - `StageCraft.AITools.Mock`, `.MockKey`, `.Status` (ADR 0023).
- **New tests:**
  - `StageCraft.Marketplace.ManifestExportFlag`, `.ExportRights`;
  - `StageCraft.Dispatch.Rules`;
  - `StageCraft.Economy.TopUp`;
  - `StageCraft.Services.CredentialVault`, `.AIToolMock`.
- **Follow-ups:**
  - Getting an owned listing again adds another copy of the scene. Reusing the existing copy needs an origin index of the saved scenes.
  - The order path is verified end to end, not by an automated test with a live marketplace.
  - D2 (USD, Maya), D4 and D5 as in §8.
