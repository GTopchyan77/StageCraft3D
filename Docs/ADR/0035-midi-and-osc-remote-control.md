# ADR 0035: MIDI and OSC remote control (Step 3)

- **Status:** accepted for MR1 (2026-11-10, STATE.md #89). The protocol scope and security (D7) were locked by Gevor without their wording being recorded (ADR 0034 §2); this ADR follows the safest reading and lists what Gevor must confirm in §9.
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
- **MIDI learn:** `StartMidiLearn(Action, Playback, CueNumber)`; the next Note On or CC with a non-zero value becomes the binding and replaces any binding with the same trigger. MR1 exposes learn through the dev console only; a mapping editor is a follow-up (§8).

## 6. OSC feedback

When feedback is on and a sender is known, the subsystem binds to the show (`OnCueStarted`, `OnDeskStateChanged`) and, once per frame after a change, sends only the values that changed since the last send: `/stagecraft/cue/active` (cue number), `/stagecraft/playback/N/level`, `/stagecraft/master/grand`, `/stagecraft/master/blackout`, `/stagecraft/master/audio`. A tablet's faders then follow the desk. MIDI output (motor faders, LED rings) is a follow-up.

## 7. Phases and verification

| Phase | What | Verification |
|---|---|---|
| **MR1** (this) | Plugins, pure parser and resolver, queue, subsystem, controller and GameMode gate, settings, Preferences section (OSC mode, MIDI device, status), OSC feedback, dev console commands, help EN/HY | Unit tests for the pure layer, subsystem tests with a test sink, a scripted `-game` run that sends real OSC UDP packets from PowerShell; Editor, Game and Shipping at 0 warnings |
| MR2 | MIDI mapping editor with LEARN in Preferences, MIDI output feedback | Manual check with a real controller |
| MR3 | Pro gate (Step 6) | Licensing tests |

## 8. Alternatives considered

- **Our own UDP OSC server** (like DMX IN's receiver): more code to own; the engine's server already parses, filters by allow-list and pumps on the game thread.
- **A GameInstance subsystem**: survives level travel, but would have to follow each world's show and controller; every stage world already reopens its devices from the settings, as DMX does.
- **OSC user mapping** instead of a fixed address space: a fixed, documented namespace is what QLab and Eos use; it needs no setup on our side, and an OSC surface is configured on the tablet anyway.
- **Driving `UShowControlSubsystem` directly**: faster, but bypasses the GameMode and the single write path (Rules.md Architecture).

## 9. Open for Gevor

- D7 wording: is an unauthenticated OSC listener on the LAN (opt-in) acceptable, or should `Network` require an allow-list of tablet addresses?
- The default MIDI map (nanoKONTROL2) — or another controller you use.

## 10. Rollback

Remove the subsystem, the settings field and the plugin entries; nothing is saved in scenes or the profile.
