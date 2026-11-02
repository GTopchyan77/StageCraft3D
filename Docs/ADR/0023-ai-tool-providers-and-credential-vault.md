# ADR 0023: AI tool providers and the credential vault

- **Status:** accepted. Implemented with STATE.md #54. There is no live provider yet, by decision.
- **Related:**
  - ADR 0022 §8 (D5: AI generation) and §11 (acquire and send, which hands models to AI tools).
  - ADR 0016 (AI assistant: the Claude and OpenAI keys come from environment variables, unchanged here).
  - Rules.md: Architecture (state by lifetime, request bridge), Engineering Standard §20/§55/§66 (trust boundaries), §59 (handles), §65 (Shipping safety).

## 1. Problem

Gevor's brief (2026-11-02) asked for two things:
- external AI 3D tools (Meshy, Tripo, Luma, or a custom cloud or local pipeline) as destinations for acquired stages;
- a secure place in the UI to manage their subscriptions and API keys.

Gevor's choice when asked: **the interface only.** That means a provider seam, safe key storage and an offline mock, with a real service chosen later.

What existed:
- No secret storage of any kind. The assistant reads `ANTHROPIC_API_KEY` and `OPENAI_API_KEY` from the environment on every request.
- No settings UI for keys.
- Crypt32 was not linked.

## 2. Decision: the credential vault

`FStageCredentialVault` (Private/Services) is a plain value type.

**Windows:**
- Each secret is encrypted with DPAPI (`CryptProtectData`, current user, `CRYPTPROTECT_UI_FORBIDDEN`). The entropy is `"StageCraft.Credential.<id>"`.
- A file copied to another id therefore does not decrypt, and neither does one read by another Windows user.
- Files are `%LOCALAPPDATA%\StageCraft\Credentials\<id>.key`: a 5-byte header (`SCKV` and a version) followed by the blob, written atomically.
- `crypt32.lib` is linked on Win64.

**Other platforms:** Unavailable. The vault never falls back to plain text.

**Rules:**
- Ids are letters, digits, `_` and `-`, at most 32 characters, so an id can never form a path.
- Secrets are 1–4096 characters.
- Damaged files are refused (Failed) and never read as a different key.
- Keys never enter logs, saves or result messages.
- Buffers that held a key are zeroed (UTF-8 conversions, the DPAPI output, the strings).

**Honest limit:** DPAPI protects against other users and against casual copying of files. It does not protect against code running as the same user. A key that must be protected from the user's own machine has to live on a server.

## 3. Decision: providers

**`UStageAIToolProvider`** (abstract UObject, Public/Services):
- `GetProviderId`, `GetDisplayName`;
- `GetCapabilities`: a bitmask of AcceptModel, TextTo3D and ImageTo3D. Only AcceptModel is used in D3; generation arrives with D5;
- `RequiresApiKey`;
- `VerifyKey(ApiKey, OnAnswer)`, `SubmitModel(Job, ApiKey, OnAnswer)`, `Cancel`.

**Contract for implementations:**
- Answer exactly once, on the game thread.
- Use the key only for the call that received it. Never store, log or echo it.
- Never delete the caller's files.

**`UStageAIToolSubsystem`** (GameInstance, `Config=Game`):
- Creates providers from `ProviderClasses`, following the `CommerceBackendClass` pattern. The list is empty by default.
- Owns the vault and runs at most one operation (a key check or a model hand-off).
- Reads a key only for the call that needs it, and wipes it afterwards.
- Answers are matched to the operation by a serial number. A late or second answer is ignored.
- It keeps a key state per tool for this run: NotNeeded, Missing, Saved, Verified, Rejected. A key the service rejected is not sent again until a new one is saved.
- Delegates: `OnToolsChanged`, `OnKeyVerified`, `OnJobCompleted`.
- `InitializeForTests(VaultDirectory)` lets tests use a temporary folder. `RegisterProviderForDevelopment` has no effect in Shipping.

**`UStageMockAIToolProvider`** ("Local test tool"):
- Offline. It accepts keys starting with `mock-`, and copies a submitted model into `Saved/StageCraft/AITools/Mock`.
- It is registered only by tests and by `StageCraft.AITools.Mock`.
- `StageCraft.AITools.MockKey` stores the mock's own test key. No console command ever takes a real key, because console history would keep it.

## 4. Decision: requests and permissions

- **Controller requests:** `RequestStoreProviderKey`, `RequestRemoveProviderKey` and `RequestVerifyProviderKey` on `AModularPlayerController`. They are deliberately not Blueprint-callable, so keys stay out of Blueprint graphs. Refusals go to `OnRequestRejected`.
- **Dispatcher check:** the acquire dispatcher (ADR 0022 §11.2) cross-checks a tool target with `StageDispatchRules`:
  - the tool must take models (AcceptModel);
  - its key must be present and not rejected;
  - it must not be busy;
  - the stage's publisher must allow export.

## 5. Decision: the Services & Keys panel

`UStageServicesWidget` is opened from Window › Services & Keys… or from the acquire dialog's link. Its tag is `StageCraft.Panel.Services` and its asset `DA_Panel_Services`, made by `Scripts/Content/create_services_content.py`.

**AI tools section:** one row per tool.
- The name and the key state.
- A password field: the text is cleared as soon as SAVE is pressed, and whatever is left is cleared when the panel closes.
- SAVE, CHECK and REMOVE.

**3D apps section:**
- The bridge state and the connected apps, with what each receives.
- A note that app licences are managed by the apps themselves.

The panel is push-bound and rebuilds on the next tick.

## 6. Alternatives considered

- **Environment variables, like the assistant.** Rejected for user-facing tools: they cannot be set from the app and they leak into child processes.
- **Plain-text config with obfuscation.** Rejected: obfuscation is not protection (§66).
- **Windows Credential Manager (`CredWrite`).** Also per user and also suitable. DPAPI files were chosen because they stay inside StageCraft's folder, are easy to remove with the app, and need no extra UI permission. Credential Manager remains an option if keys should roam with the Windows profile.
- **A live provider now.** Gevor chose the interface first. Choosing a service also means accepting its terms and handling its job polling, which is D5 work.

## 7. Consequences

- **New module and files:** `Public/Services/*`, `Private/Services/*`, a new panel and its content asset.
- **Tests:** `StageCraft.Services.CredentialVault` (round trip, not plain text on disk, a file swapped to another id is refused, damage refused, remove) and `StageCraft.Services.AIToolMock` (no key, rejected key, verified key, hand-off, cancel).
- **Follow-ups:**
  - A real provider (D5): an HTTP implementation of `SubmitModel` with job polling, plus the generation calls.
  - Moving the assistant's keys into the vault.
  - An expiry or plan field once a server grants subscriptions.
