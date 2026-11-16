# ADR 0035: MIDI and OSC remote control (Step 3)

- **Status:** accepted for MR1 (2026-11-10, STATE.md #89); MR2 (MIDI MAP editor and MIDI output feedback) added 2026-11-16, STATE.md #96, §11–§12; closing on a lapsed licence added 2026-11-16, STATE.md #98, §13. The protocol scope and security (D7) were locked by Gevor without their wording being recorded (ADR 0034 §2); this ADR follows the safest reading and lists what Gevor must confirm in §9.
- **Related:**
  - ADR 0034 §2 (tiers: MIDI / OSC is **Pro**; D8 puts it at Step 3, after MVR).
  - ADR 0026 §6 (playbacks and masters), ADR 0019 (DMX IN, the precedent for a network input and its settings), ADR 0013 §2.5 (Preferences panel).
  - Rules.md: Architecture (state by lifetime, the request bridge, push not poll), Engineering Standard §17 (timers), §21 (no tick polling), §29 (threads), §55 (validate at the trust boundary), §65–66 (Shipping and security boundaries).

## 1. Problem

Lighting designers run shows from hardware: a MIDI fader box or pad controller on the desk, or a tablet running an OSC surface (TouchOSC, Open Stage Control, QLab cues). StageCraft's live controls (cue GO / BACK, ten playback faders with flash and GO, Grand Master, Blackout, Audio master) only answer the mouse, the keyboard and the console commands.

## 2. Decision overview

- **Engine plugins, no third-party code.** `MIDIDevice` (PortMidi; Windows, macOS, Linux) and `OSC` (UDP). Both deliver on the game thread: the MIDI module polls its devices on its own tickable (`FMIDIDeviceModule::Tick` → `ProcessIncomingMIDIEvents`), and `UOSCServer` reads on its socket thread and pumps its queue on the game thread. StageCraft adds no thread and no lock.
- **One world subsystem owns the devices:** `UStageRemoteControlSubsystem` (`UWorldSubsystem`, Game and PIE worlds), like `UStageDMXNetworkSubsystem`: the stage's show is per world, it reopens from the saved settings in `OnWorldBeginPlay`, and `Deinitialize` closes the port and the device. No tick.
- **Inputs become commands, commands go through the request bridge.** A pure layer (`StageRemote`, any thread) turns an OSC message or a MIDI event into `FStageRemoteCommand`s. The subsystem queues them and, once per frame, hands each to `AModularPlayerController::RequestRemoteCommand`, which asks the GameMode (`EvaluateRemoteControl`, then the usual `EvaluateCueRequest`) and calls the existing `RequestCueGo`, `RequestSetPlaybackLevel`, `RequestSetGrandMaster`… So a remote surface can do exactly what the desk can, under the same rules, and nothing else.
- **Live controls only, no undo.** The actions are the console's live controls, which are not undoable today (cue GO, faders, masters). Edits (store, record, effects, FOLLOW MUSIC) are not reachable from a remote in MR1.

## 3. The actions

| Action | OSC address (fixed) | Argument | MIDI default (§5) |
|---|---|---|---|
| Cue GO | `/stagecraft/cue/go` | none, or a press value | CC 41 (▶) |
| Cue BACK | `/stagecraft/cue/back` | none, or a press value | CC 43 (◀◀) |
| Go to cue | `/stagecraft/cue/goto` | cue number | – |
| Playback level | `/stagecraft/playback/N/level` | 0…1 | CC 0–7 → playbacks 1–8 |
| Playback flash | `/stagecraft/playback/N/flash` | 1 down, 0 up | CC 32–39 (S) → playbacks 1–8 |
| Playback GO | `/stagecraft/playback/N/go` | none, or a press value | CC 48–55 (M) → playbacks 1–8 |
| Playback release | `/stagecraft/playback/N/release` | none, or a press value | CC 64–71 (R) → playbacks 1–8 |
| Grand Master | `/stagecraft/master/grand` | 0…1 | CC 16 (knob 1) |
| Blackout | `/stagecraft/master/blackout` | 1 on, 0 off | CC 45 (●) |
| Audio master | `/stagecraft/master/audio` | 0…1 | CC 17 (knob 2) |

- `N` is the playback as the desk shows it, 1…10.
- **Press values.** A button sends 1 on press and 0 on release (TouchOSC, MIDI). A trigger (GO, BACK, release) fires on a value ≥ 0.5 and ignores the release; with no argument it fires at once. Flash and Blackout follow the value: down while ≥ 0.5.
- **Arguments.** The first argument is read as a number from float, int, double or bool. Other types, NaN and infinities are refused. Levels are clamped to 0…1 by the show.
- Addresses are matched exactly, case-insensitive, at most 128 characters. Unknown addresses are counted as ignored, not errors.

## 4. Settings and security (D7)

`FStageRemoteSettings` in `UStageCraftUserSettings` (per PC, the subsystem is the only writer, sanitized on load and on every set):
- **OSC:** `Off` (default) / `This computer` / `Network`. Port 8000 (1024–65535).
  - **Engine finding (UE 5.8):** `UOSCServer` cannot bind loopback. `FServerReceiver::InitSocket` replaces 127.0.0.1 with this PC's LAN address (`SetAsLocalHostIfLoopback`), so a "127.0.0.1" server receives nothing sent to 127.0.0.1 and is reachable from the network. Found by `StageCraft.Remote.OscLoopback`.
  - So the socket always listens on every card (0.0.0.0), and `This computer` turns on the server's allow-list with only 127.0.0.1. The engine filters on the game thread before dispatch (`CanProcessPacket` in `PumpPacketQueue`), so a packet from another address, this PC's own LAN address included, is never carried out; the test sends one to prove it. The engine still parses it first, as it does in `Network` mode.
  - Windows may show its firewall prompt the first time either mode opens, because the socket listens on every card.
- **OSC feedback:** to the address that last sent us a message, port 9000 (0 = off); the show's state is sent when it changes (§6).
- **MIDI input:** a device name (empty = off). The device is found by name each time it opens, so a controller plugged into another USB port still matches.
- **MIDI map:** the bindings of §5.

Security, deny by default:
- Both inputs are off until the user turns them on. `Network` must be chosen explicitly; `This computer` is enough for an OSC app on the same PC.
- A remote can only reach the actions of §3, each through the GameMode. `SessionRules.bAllowRemoteControl` turns them all off for a curated session; Step 6 adds the Pro entitlement check (`StageCraft.Entitlement.Pro.MidiOsc`) to `EvaluateRemoteControl`, so until then the gate is written and open (ADR 0034 §2.1).
- OSC has no authentication. On `Network`, anyone on the LAN who knows the port can run the show's live controls — the same exposure as DMX IN (ADR 0019). Gevor must confirm this is acceptable for D7, or ask for an allow-list (the engine server has one: `AddAllowlistedClient`).
- **Flood protection.** A fader on a tablet sends 60+ messages a second. The queue keeps one entry per continuous target (the latest value wins) and at most 256 commands per frame; the rest are counted as dropped. Refusals do not raise a status-bar message per message: one is shown when the reason changes, and the count is in the status.

## 5. MIDI mapping

- A binding: trigger (Note or Control Change), channel (0 = any, 1–16), number (0–127), the action, the playback (1–10) or the cue number.
- Note On with velocity 0 is a Note Off (the MIDI convention). A note's value is its velocity / 127 while held and 0 on release; a CC's value is its data / 127.
- **Default map:** the Korg nanoKONTROL2 factory CC layout (eight faders, knobs, S / M / R buttons, transport), chosen because it is the most common small fader box. It is an assumption, not tested on the hardware.
- **MIDI learn:** `StartMidiLearn(Action, Playback, CueNumber)`; the next Note On or CC with a non-zero value becomes the binding and replaces any binding with the same trigger. MR1 exposed learn through the dev console only; MR2 adds the MIDI MAP editor in Preferences (§11), where LEARN also removes the target's earlier control.

## 6. OSC feedback

When feedback is on and a sender is known, the subsystem binds to the show (`OnCueStarted`, `OnDeskStateChanged`) and, once per frame after a change, sends only the values that changed since the last send: `/stagecraft/cue/active` (cue number), `/stagecraft/playback/N/level`, `/stagecraft/master/grand`, `/stagecraft/master/blackout`, `/stagecraft/master/audio`. A tablet's faders then follow the desk. MIDI output (motor faders, button lights) is MR2 (§12).

## 7. Phases and verification

| Phase | What | Verification |
|---|---|---|
| **MR1** (this) | Plugins, pure parser and resolver, queue, subsystem, controller and GameMode gate, settings, Preferences section (OSC mode, MIDI device, status), OSC feedback, dev console commands, help EN/HY | Unit tests for the pure layer, subsystem tests with a test sink, a scripted `-game` run that sends real OSC UDP packets from PowerShell; Editor, Game and Shipping at 0 warnings |
| **MR2** (STATE.md #96) | MIDI MAP editor with LEARN / CLEAR / RESET / CLEAR ALL in Preferences (§11), MIDI output feedback (§12), map edits without reopening or a GameMode verdict, dev console `MidiOut=`, `Clear=`, `#n` device picks | Pure tests (map operations, overlap rule, verdict rule, feedback values, holds, echoes, unbound controls going dark), a real-device test with the settle timer and an echo on Windows' GS synth, a scripted `-game` run (output opened, map edited with Pro withdrawn, screenshot); a real controller is Gevor's check |
| MR3 | Pro gate (Step 6) | Licensing tests |

## 8. Alternatives considered

- **Our own UDP OSC server** (like DMX IN's receiver): more code to own; the engine's server already parses, filters by allow-list and pumps on the game thread.
- **A GameInstance subsystem**: survives level travel, but would have to follow each world's show and controller; every stage world already reopens its devices from the settings, as DMX does.
- **OSC user mapping** instead of a fixed address space: a fixed, documented namespace is what QLab and Eos use; it needs no setup on our side, and an OSC surface is configured on the tablet anyway.
- **Driving `UShowControlSubsystem` directly**: faster, but bypasses the GameMode and the single write path (Rules.md Architecture).

## 9. Open for Gevor

- D7 wording: is an unauthenticated OSC listener on the LAN (opt-in) acceptable, or should `Network` require an allow-list of tablet addresses?
- The default MIDI map (nanoKONTROL2) — or another controller you use.
- MR2: LEARN gives a row exactly one control (the earlier one is removed). Is one control per row right, or should a row keep several (a pedal and a button for GO)?
- MR2: check MIDI MAP and MIDI output with the real controller (motor faders or button lights; on the nanoKONTROL2 the lights need LED Mode External).

## 10. Rollback

Remove the subsystem, the settings fields (MidiOutputName included) and the plugin entries; nothing is saved in scenes or the profile.
For MR2 alone: remove the MIDI MAP rows and the MIDI output row; an ini with `MidiOutputName` loads without it (unknown keys are ignored).

## 11. MIDI MAP editor (MR2)

Preferences › REMOTE CONTROL › MIDI map. The view edits the map through the controller's request bridge only (`RequestSetRemoteControl`, `RequestMidiLearn`); the map's operations are pure (`StageRemote`), so the panel, the dev console and the subsystem share them and the tests prove them.
- **Rows (targets):** Cue GO, Cue BACK, Grand Master, Blackout, Audio master; then a Playback 1–10 picker and Fader, Flash, GO, Release for that playback. A row lists its controls (`DescribeControl`: "CC 41", "Note 60 ch 10"). Go to cue needs a cue number per binding, so it has no row; RESET and CLEAR ALL still remove such bindings.
- **LEARN** arms `StartMidiLearn` for the row (pressed again: cancel). The next press becomes the binding through `StageRemote::AssignLearned`: the map in force becomes the user's, the control's other bindings (an "any channel" binding of it included: `OverlapsTrigger`) and the row's earlier controls are removed. `Sanitize` applies the same overlap rule, so one control never drives two rows by accident. One control per row is the decision of this phase (open for Gevor, §9). Learn needs an open MIDI input (LEARN is disabled otherwise). Closing Preferences cancels a waiting learn, so a stray press later cannot rebind silently.
- **CLEAR** (`ClearTarget`), **RESET TO DEFAULT** (`bCustomMidiMap` off), **CLEAR ALL** (an empty user map).
- **Rows are built once** and only relabelled and enabled: LEARN and CLEAR change the map from inside their own click (Rules.md Safe data binding).
- **A map edit is not a new input.** `StageRemote::HasSameInputs` (OSC scope and ports, both MIDI devices): when only the map differs, `ApplySettings` saves it and reopens nothing (the controller stays connected, a learn keeps waiting), and `NeedsVerdict` lets it through without `EvaluateRemoteControl`, because every command a control sends is judged when it runs. Applying identical settings (a retry after a missing device) and every change that opens or moves an input are still judged; switching off never is.

## 12. MIDI output feedback (MR2)

`FStageRemoteSettings::MidiOutputName` (empty = off; Preferences › MIDI output, Off plus the first three outputs). It counts as switching something on, so it needs the GameMode's verdict and Pro.
- **What is sent:** for each binding of the map in force, `FeedbackValue`: a fader its playback level, Grand Master and Audio master their level (0–127), Flash lit while the playback is flashing, playback GO lit while it is live, Blackout lit while on. Cue GO / BACK, go to cue and release have no state and send nothing. A CC binding answers with a CC, a note binding with Note On (velocity 0 = off).
- **When:** the subsystem binds the show (`OnCueStarted`, `OnDeskStateChanged`) while OSC feedback or a MIDI output is open; at most once per frame, `CollectMidiFeedback` sends only the controls whose value differs from what `FStageMidiFeedbackMemory` says the controller shows. The memory is reset (everything sent once) when the output opens and when the map changes.
- **No fight with the hand:** the desk applies a move one frame after it arrives, so during a move it is always a step behind the hand. `NoteMidiInput` therefore records a fader's or knob's own move as shown and holds the control for `MidiHoldSeconds` (0.5 s) after its last move; `CollectMidiFeedback` skips held controls and reports when the last hold ends. A settle timer (`ArmMidiSettle`) then runs feedback once more, so a released fader settles on the desk's value and a refused or clamped move snaps back, even though the show itself did not change (read-back, as in the UI). The hold survives a map change, so a learned fader is not yanked either. Buttons are not recorded or held: their light is the show's to set.
- **Echo:** an input equal to a message sent within `MidiEchoSeconds` (0.1 s) is dropped as the output's echo (a virtual port or a controller's MIDI thru chosen as both input and output), counted as ignored and shown as "(echo of feedback)"; otherwise a lit GO light would fire GO again.
- **Map changes:** before the memory resets, `CollectUnboundOff` sends 0 to every lit or raised control that no binding answers any more (a cleared Blackout button goes dark).
- **Unplugged controller:** a device listing marks a controller the engine cannot restart as garbage. `CheckMidiDevices` (before learn, before feedback, and when Preferences opens, since building it lists the devices) closes such a device and says "The MIDI controller was disconnected".
- **Channels:** a binding on "any channel" answers on the channel the control was last heard on (channel 1 until then; a controller on another global channel gets its lights once each control has been touched; an output-channel setting is possible later). A control bound twice answers for its first binding only, so it never flickers between two values.
- **Engine findings (UE 5.8):**
  - `UMIDIDeviceOutputController::SendMIDIEvent` builds the status byte as `(type << 4) | channel`: it takes channels 0–15, unlike the input, which reports 1–16. StageCraft sends `Channel - 1`; channel 16 sent as 16 would have changed the message type.
  - `UMIDIDeviceManager::FindAllMIDIDeviceInfo` re-initializes PortMidi and restarts every live `UMIDIDeviceControllerBase` object, a shut-down one too, until garbage collection removes it. It would take the device back and the next open would fail as "in use" (this also affected reopening the MIDI input in MR1). `CloseMidi` marks both controllers as garbage after `ShutdownDevice`, as the engine does with a controller it could not restart; the MidiOutput test reopens the same output to prove it.
  - `FTimerManager` runs once per engine frame (`HasBeenTickedThisFrame`), so a test that ticks its own world several times in one frame runs the timers once: the MidiOutput test checks the settle timer in a latent command, one world tick per engine frame.
  - Device listings are expensive for that reason: the panel and `OpenMidi` list inputs and outputs in one call (`FindMidiDevices`).

## 13. Closing on a lapsed licence (2026-11-16, STATE.md #98)

**Problem.** Until #98 the OSC server and the MIDI devices opened from the saved settings at every stage start, whatever the licence said, and on Free every command was refused (#93). That left an unauthenticated UDP listener on the LAN, and a controller held open, for a capability the user no longer had: an attack surface with no benefit, and a status that said "listening" when nothing would ever be carried out.

**Decision.** The GameMode's verdict decides whether the inputs are open at all, as it already decides the network DMX's limits (ADR 0034 §17):
- `UStageRemoteControlSubsystem::SetCapabilityAllowed(bAllowed, Reason)`. The local `AModularPlayerController` pushes `EvaluateRemoteControl` (the session's rules, then Pro) in `ApplyLicenceLimits`, at BeginPlay and on every `UStageLicenceSubsystem::OnLicenceChanged` (expiry timer, refresh, cancel, release, the development override).
- **Refused:** OSC and MIDI close, a learn is cancelled, queued commands are dropped, and the status shows the GameMode's message as a warning (only when the user had something switched on). `ApplySettings` stores but does not open, and returns that message.
- **The user's choice is kept.** The saved settings and the MIDI map are not touched, so a renewed licence reopens exactly what was open, with no click. This mirrors the DMX input's "the user's choice stays on" (`SetCapabilityLimits`).
- **No open before the verdict.** `OnWorldBeginPlay` no longer opens; it marks the restore pending, and the controller's first push opens or refuses. `UWorld::BeginPlay` runs the subsystems' `OnWorldBeginPlay` before the actors' BeginPlay in the same call, so a Free licence never binds the port or opens a device, not even for a frame.
- **Unbound use.** The verdict defaults to allowed, so tests and tools that call `ApplySettings` directly work without a controller. In play, `ApplySettings` is reached only through `RequestSetRemoteControl`, which asks the GameMode first. Deny by default still holds for the saved settings: without a push they never open, and without a GameMode the controller pushes a refusal. A world with no `AModularPlayerController` (none in this project plays without one) never opens the saved settings and shows no reason; this matches the DMX network subsystem.
- **Same verdict, new reason** (a session rule after the licence): the warning follows the latest reason. **Web Remote:** switching "Remember devices" off while refused deletes the file and is not reported as a refusal.

**Alternatives.**
- Keep refusing per command (#93): the listener stays open; rejected for the reason above.
- Switch the settings off on a lapse: the user would have to set OSC, MIDI and the map up again after renewing, and a lapse during a show's preparation would silently lose them. Rejected.
- Close only after a grace period: the licence already has 14 days offline (ADR 0034 §17); a second grace adds nothing.

**Verification.** `StageCraft.Remote.Capability` (no open before the verdict; Free at start keeps the choice; `ApplySettings` while refused; renew opens; lapse closes and drops a queued command; no warning when everything is off), and a scripted `-game` run that lets the development override lapse and come back (STATE.md #98).

**Rollback.** Call `OpenInputs` in `OnWorldBeginPlay` again and remove the push; behaviour returns to #93.
