# Audit: Acquire & Send (D1 bridge, D3 dispatcher, economy top-up, AI tools and vault)

**Date:** 2026-11-02 · **Record:** STATE.md #55 · **Scope:** the code added in #53 and #54, and the paths that touch it. **Method:** read every binding, timer, worker hand-off and public API of the classes below against Rules.md (Architecture, Economy security, §9, §16, §17, §29, §61), then fix, test and measure. Severity: **bug** (wrong behaviour today), **risk** (latent, or appears with a real server, store or tool), **style** (a Rules.md deviation without a behaviour change).

Classes in scope:
- `UStageAcquireDispatchSubsystem`, `StageDispatchRules`
- `UStageAIToolSubsystem`, `UStageAIToolProvider`, `FStageCredentialVault`, `UStageMockAIToolProvider`
- `UStageEconomySubsystem` (top-up and purchase answers), `UStageCommerceBackend`
- `UStageBridgeSubsystem`, `FStageBridgeServer`, `FStageSceneExporter`
- `UStageAcquireDialogWidget`, `UStageServicesWidget`, `UStageMarketplaceWidget`, `UStageSendToWidget`
- `AModularPlayerController` (request functions)

## Findings and status

| # | Severity | Where | Finding | Status |
|---|---|---|---|---|
| 1 | risk | economy, marketplace fetch, AI tools, dispatcher | No deadline on any answer: a backend, fetch or tool that never answers keeps the subsystem pending for the run, and every later order is `Busy`. | **Fixed:** timeouts everywhere; late economy approvals honoured, late refusals dropped (ADR 0021 §14). |
| 2 | bug | `StageServicesWidget.cpp` CHECK | "Checking…" was set after the request, overwriting a synchronous answer. | **Fixed:** set before the request. The same for the dialog's top-up text. |
| 3 | bug | `StageAcquireDispatchSubsystem.cpp`, `StageDispatchRules.cpp` | Two different texts under the LOCTEXT key `StageCraftDispatch/Busy`. | **Fixed:** `OrderBusy`, `TargetBusy`. |
| 4 | risk | Services panel, vault | Typed keys copied into an unwiped `FString` (`Reset` frees without zeroing) and into a `Typed` `FText` cache across rebuilds; `WipeString` zeroed only the used length with a plain memset the optimiser may drop. | **Fixed:** wiped with `WipeString`; the cache removed (cards update in place, so a typed key survives without copies); wiping uses `SecureZeroMemory` over the whole allocation including slack. FText and Slate's own buffer cannot be wiped (documented). |
| 5 | risk | `StageCredentialVault.cpp` Load | Size checked from `FileSize`, then the read buffer indexed: a file truncated in between reads out of bounds. | **Fixed:** checked on the bytes read. |
| 6 | bug | dialog progress | "Sending … to X" named the selected row, which could change during the order. | **Fixed:** names the order's own target; rows are locked while an order runs. |
| 7 | bug | dispatcher AI-tool delivery | Its outbox export was broadcast on `OnSendCompleted`, so the Send-To panel showed "Saved to …/Outbox/…" for a temporary file. | **Fixed:** file exports report to their requester only (ADR 0022 §12). |
| 8 | risk | dispatcher | Delivery (a world plus a glTF write) and the "Open" load ran inside the marketplace's `OnAcquireCompleted` broadcast. | **Fixed:** delivery on the next tick. |
| 9 | risk | dialog refresh | O(rows × tools) `FileExists` calls per refresh on the game thread (key state read from disk each time). | **Fixed:** key presence kept in memory (filled at Initialize, Store, Remove); one dispatch-context snapshot per refresh. |
| 10 | risk | dialog confirm | Confirm sent the listing's price at click time, which can differ from the one drawn during the one-frame refresh delay. | **Fixed:** sends the price as drawn. |
| 11 | risk | dispatcher price check | Compared credits only; a changed real-money part would pass once a server exists. | **Fixed:** the whole `FStageMarketplacePrice` (`ExpectedPrice`). |
| 12 | style | `UStageMarketplaceWidget::CloseAcquireDialog` | Timer handle not kept (Rules §17); a close and a reopen in the same frame would remove the new dialog. | **Fixed:** handle kept, cleared on open and destruct. |
| 13 | risk | `UStageBridgeSubsystem` | `Deinitialize` broadcast to listeners during teardown; a listener stopping the bridge mid-drain left the rest of the batch adding clients to a stopped bridge. | **Fixed:** delegates cleared first; the drain stops when the bridge stops. |
| 14 | risk | `UStageAIToolSubsystem::HandleAnswer` | The contract says game-thread answers; nothing checked it. | **Fixed:** `ensureMsgf(IsInGameThread())`, and an answer from elsewhere is refused. |
| 15 | style | `UStageServicesWidget` | CHECK stayed disabled after a dispatcher hand-off (no rebuild on `OnJobCompleted`); a dead `bSelected` branch. | **Fixed.** |
| 16 | style | `StageSceneExporter.h` | Comments said marketplace scenes are refused and meshes are destroyed before returning. | **Fixed.** |
| 17 | perf | export | One game-thread frame of 86–113 ms per export; SHA-1 of the whole file on the game thread. | **Fixed:** `FStageSceneExportJob` (ADR 0022 §12): longest slice 17–23 ms steady; worker hashing. |
| 18 | perf | preview | A full scene capture per mouse-move event while orbiting. | **Fixed:** at most one capture per frame (input coalesced). Not measured: no mouse automation for the preview. |
| 19 | style | controller | `RequestAcquireAndDispatch`, `RequestCreditTopUp`, key requests do not ask the GameMode first. | **Deferred:** follows the existing `RequestMarketplaceAcquire`; changing the authority path for all marketplace requests is its own ADR. |
| 20 | style | `FStageDispatchTarget`, `FStageAcquireOrder` | `BlueprintReadWrite` on every field. | **Kept:** Blueprint is meant to build orders; every field is validated by the dispatcher. |
| 21 | note | Economy ↔ Marketplace | Include each other in `.cpp` files. | **Kept:** no header cycle; the economy reads offers through the marketplace by design (ADR 0021 §5). |
| 22 | note | bridge worker | The drain callback captures `this`; safe because `Stop` joins the thread before the subsystem goes. | **Kept:** documented at the capture. |
| 23 | note | test classes | `UStageTestCommerceBackend`, `UStageTestAIToolProvider`, `UStageTestEventRecorder` and the mock tool ship as unreachable classes (UHT cannot compile a UCLASS out). | **Accepted:** never configured or registered outside tests and development commands. |

## Where each step of an acquire-and-send runs (after #55)

| Step | Thread | Notes |
|---|---|---|
| Dialog refresh (wallet, targets) | game | next-tick coalesced; no disk access |
| Price, rights and target checks | game | pure rules, in memory |
| Commerce backend | backend's choice | deadline 30 s (config) |
| Package fetch, verification, import, library copy | `UE::Tasks` pipes | deadline 60 s on fetch |
| Delivery start | game, next tick | never inside a broadcast |
| Scene read, library manifest read | worker | export job |
| Rights decision, world, spawn, bake | game, over frames | 4 ms budget per frame for spawn and bake |
| glTF conversion and write | game, one frame | engine API needs UObjects |
| Move into place, size, SHA-1 | worker | export job |
| Socket accept, read, write | bridge worker thread | unchanged (`FStageBridgeServer`) |
| AI tool hand-off | provider's choice | deadline 120 s; key read from the vault for the one call, then wiped |

## Verification

- 149/149 automation tests with and without a renderer (`Scripts/Tests/RunAutomation.ps1`), user data byte-identical.
- 8 new integration tests on a harness game instance (`FStageTestGameInstance`): `StageCraft.Economy.ListingPurchaseFlow`, `.BackendTimeout`, `.EntitlementGates`; `StageCraft.Dispatch.OrderFlow`, `.OrderTimeout`; `StageCraft.Services.VaultBeforeDispatch`; `StageCraft.Bridge.LoopbackApps`, `.ExportJob`.
- `StageCraft.Bench.Export` numbers: ADR 0022 §12.
