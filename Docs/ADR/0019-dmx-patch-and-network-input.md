# ADR 0019: DMX patch subsystem and Art-Net / sACN input

- **Status:** accepted. See STATE.md #46. Amended by §9 (DMX output, STATE.md #65) and §10 (patch options and fixture schedule, STATE.md #66).
- **Related:**
  - ADR 0007 (lighting, patch paths, Patch panel): §2 and §5 move the patch logic out of the show control and extend the panel.
  - ADR 0011 (cue list, patch sheet): the patch sheet now reads conflicts from the patch subsystem.
  - ADR 0013 (preferences): the input settings are per-PC preferences in `UStageCraftUserSettings`.
  - STATE.md #9: planned a `StageCraftDMX` module on Epic's DMX Engine plugin. This ADR replaces that plan.
  - Rules.md: Architecture (state by lifetime, the request bridge, push not poll), Engineering Standard §21 (no tick polling), §29 (async and threads), §55 (validate at the trust boundary), §56 (undo), §66 (security boundary).

## 1. Problem

Gevor's phase (2026-10-26):

1. **Patch and universes.** A `UStageDMXPatchSubsystem` manages universes and channel allocation (start address, footprint per fixture type) and patches without conflicts. A Patch panel inspects, re-addresses and assigns fixtures.
2. **Network input.** A light background UDP listener for Art-Net and sACN (E1.31). External consoles or visualisers drive dimmer, colour (RGBW), pan/tilt and gobo channels in real time.
3. `TObjectPtr`, encapsulation, no per-frame Tick (event-driven packets, efficient buffers), all tests, zero warnings, STATE.md.

What existed already:

- **The fixture's patch.** `ALightingFixtureActor::Patch` (SaveGame, since #40), with a footprint taken from `ULightingFixtureData::DMXChannels`.
- **The codec.** `WriteDMX` / `ReadDMX` handle 8/16-bit channels and CMY.
- **The Patch panel.** It has a table, a channel map, PATCH UNPATCHED and an export.
- **External control mode.** `UShowControlSubsystem` has an External mode and a `UStageDMXBridge` boundary, but nothing implemented it.
- **Patch functions in the show control.** Free range, conflicts and auto-patch lived in `UShowControlSubsystem`. Conflict detection was O(n²), with no tests. An address that ran past channel 512 was accepted. Patch edits were not undoable. PATCH UNPATCHED sent the universe and the address as two separate requests.

Gevor's choices, asked before design:

| Question | Choice |
|---|---|
| Receiver | **Our own small receiver** on the engine's sockets (no DMX Engine plugin) |
| Numbering | **StageCraft universe 1 = Art-Net port-address 0 (0:0:0)** and sACN universe 1; offsets in the settings |
| Start | **Off until switched on**, remembered on this PC |
| ArtPoll | **Answer ArtPoll**, so consoles that send only to discovered nodes find StageCraft |

## 2. Patch

### 2.1 Pure allocation: `StageDMXPatching`

The allocation is pure code over `FStageDMXPatchSlot {Key, Patch, Footprint}`. It uses no world and no actors, and it is tested directly.

- **`Fits` / `GetLastStartAddress`.** A footprint fits when it ends on or before channel 512.
- **`FindFree`.** It finds the first gap at or after a start universe and address, and moves to later universes when the start universe is full. The ranges are sorted once and the start universe is found by binary search.
- **`FindConflicts`.** A sweep over the ranges sorted by address, O(n log n). A range overlaps an earlier one when it starts at or before the furthest end seen so far. A range past channel 512 is a conflict on its own.
- **`PlanConsecutive`.** Patches the given fixtures one after another from a start, around the fixtures that keep their patch. It is all or nothing.
- **`PlanConflictRepair`.** In each universe, in address order, a range stays when it fits and starts after everything already kept. On a tie, the lower key wins, which means the lower fixture ID. Every other range moves to the first free fit, starting from address 1 of its own universe.
- **`SummarizeUsage`.** Per universe: the channels used (an overlap counts once), the fixture count and the conflict count.

Overlaps are still allowed on input, as on a console. They are reported, and FIX CONFLICTS repairs them.

### 2.2 `UStageDMXPatchSubsystem` (world)

- **Registry.** Fixtures register in `BeginPlay`, after the show control has assigned the fixture ID, and unregister in `EndPlay`. Nothing iterates the world.
- **Source of truth.** The fixture's `Patch` stays the source of truth. The subsystem keeps a derived per-universe index, `UniverseIndex`. The index is marked stale whenever:
  - a fixture is registered or unregistered;
  - a fixture reports a change through `NotifyPatchChanged`. The fixture reports from `SetPatch` and from `ApplyItemData`; the latter covers a swapped model with another footprint and a restored state.

  The index is rebuilt on the first read after it goes stale. `GetPatchRevision` changes on every patch change, so a consumer can tell its derived state is stale without binding a delegate.
- **Auto-patch.** New fixtures are auto-patched when `bAutoPatchNewFixtures` is on. A fixture with no room left stays unpatched and a warning is logged.
- **Plans.** `PlanUnpatched`, `PlanConsecutive` and `PlanConflictRepair` return `FStageDMXPatchAssignment`s and change nothing.
- **Moved out of the show control.** `FindNextFreePatch`, `FindPatchConflicts`, `GetPatchedUniverses` and `bAutoPatchNewFixtures` moved here. The show control initializes the patch subsystem as a dependency. It renders and decodes only through `GetFixturesInUniverse`.

### 2.3 Writes, rules and undo

- **The address rule.** `AStageCraftGameModeBase::EvaluateItemEdit` refuses an address the fixture's footprint does not fit, for example "uses 21 channels, so its address can be 1 to 492".
- **Undo for single edits.** `StageCraftTags::IsPatchParameter` covers the fixture ID, the universe and the address. `RequestParameterChange` now records these edits as undoable steps. This closes the patch half of the todo item "undo for attribute and patch edits".
- **`RequestPatchFixtures(Assignments, EmptyReason)`.** The batch request used by every plan:
  - It checks every new universe and address with the GameMode first.
  - It applies them through the session inside one edit transaction, so the whole plan is one undo step.
  - It rolls back if any write fails.
  - It refuses an empty plan with the planner's reason.

## 3. Network input

### 3.1 Why our own receiver, not the DMX Engine plugin

- **What it takes.** The receiver is two UDP sockets and a parser of two packet formats. All of it is pure code that can be fully unit-tested, built on the engine's `Sockets` / `Networking` modules: `FUdpSocketBuilder`, `FUdpSocketReceiver`, `FIPv4Address`.
- **What the plugin would bring.** Enabling DMXEngine / DMXProtocol / DMXRuntime brings their fixture library, port settings and editor tooling. It adds a plugin to every build and a second set of DMX settings.
- **What we give up.** We do not get their output ports, RDM or MVR import. Output is not part of this phase.

### 3.2 Threads and the hand-off (no tick)

- **Socket threads.** Each socket is read by an engine `FUdpSocketReceiver` thread, which waits on the socket for up to 100 ms per loop. Each thread calls `FStageDMXInbox`, which is shared with the game thread.
- **What the inbox does on the socket thread.** For every datagram it:
  1. parses it with `StageDMXNet::Parse`, which validates every field;
  2. maps the universe through the settings;
  3. arbitrates the source (§3.3);
  4. keeps the newest 512 slots of each universe in `Pending`, under one lock.
- **Hand-off.** Only the first frame after a drain asks the owner to drain (`ScheduleDrain`). The bridge then queues one `AsyncTask(GameThread)`, which holds only a weak pointer. However many packets arrive in between, the game thread does one hand-off per batch, and nothing at all while the wire is quiet.
- **On the game thread.** `UStageNetworkDMXBridge::DrainInbox` takes the pending frames out under the lock, then hands each universe to `UStageDMXBridge::ReceiveUniverse` and from there to `UShowControlSubsystem::ReceiveDMXUniverse` outside it.
- **Teardown.** `Stop` (and the destructor) stops and joins the threads before the sockets are destroyed. A callback therefore never sees a dead socket. `Stop` is idempotent.

### 3.3 Protocols, validation and sources

- **Art-Net 4.**
  - ArtDmx needs protocol version 14 or later, a length of 1 to 512 that the datagram actually holds, and a 15-bit port-address. The reserved top bit of Net is ignored.
  - ArtPoll is answered with ArtPollReply (239 bytes). There is one reply per Net + Sub-Net group with at most four ports, whose bind indexes count from 1. The ports are the patched universes.
  - Replies go back to the poller, and there are at most ten reply rounds a second, so a poll flood cannot turn StageCraft into a sender.
  - Other opcodes are ignored, including our own echoed reply and ArtSync.
- **E1.31 (sACN).**
  - These fields are checked: the root, framing and DMP vectors, the PDU flags, priority ≤ 200, universe 1..63999, the DMP address type, first address and increment, a property count of 1..513 that the datagram holds, and START code 0. Packets with an alternate START code are ignored, for example 0xDD (per-address priority), as are discovery and sync packets.
  - Stream_Terminated releases the source at once.
  - The socket is bound to every network card and joins the multicast group of each patched universe (239.255.hi.lo) on the chosen card. On Windows a socket bound to one card's address does not receive multicast. Unicast sACN always arrives.
- **Sources (`FStageDMXSourceArbiter`).**
  - There is one source per universe, with no HTP/LTP merge, as most visualisers do.
  - The highest priority wins. Art-Net counts as 100. An equal or lower priority is ignored while the current source is alive.
  - A source is gone after 2.5 s of silence (the E1.31 network data loss timeout) or after Stream_Terminated.
  - sACN frames up to 20 behind the last sequence number are dropped.
  - Art-Net sources are told apart by address and port.
- **Counters.** Frames, invalid datagrams and ignored frames are counted for the status.

### 3.4 Settings, state and switching

- **Settings.** `FStageDMXInputSettings` is stored in `UStageCraftUserSettings` (GameUserSettings.ini, per PC) and is always sanitized. It holds:
  - on / off (off by default);
  - Art-Net and sACN on / off;
  - whether to answer ArtPoll;
  - the bind address (empty means every card; an invalid address also means every card);
  - the Art-Net port-address of universe 1 (0);
  - the sACN universe of universe 1 (1).
- **`UStageDMXNetworkSubsystem` (world).**
  - States: Off, Listening or Failed (the ports could not be opened; the error is kept for the panel).
  - Switching on creates the bridge, opens it and installs it in the show control, then saves the setting on.
  - On `OnWorldBeginPlay` a remembered "on" opens the input again and gives the show to External.
  - Patch changes are folded into one update of the multicast groups and ArtPollReply on the next tick.
  - `OnInputChanged` fires at once on every state change, and at most every 0.5 s while frames arrive. That is one non-looping timer armed by the traffic; none runs while the wire is quiet.
- **`AModularPlayerController::RequestSetDMXInput`.** On opens the ports and switches the show to External through `RequestSetControlSource` (the cue rules). Off closes them and switches back to Internal. A port that cannot be opened is reported on `OnRequestRejected` and nothing changes.
- **Input only.** The bridge has `bOutputEnabled = false`. With output on, the show would render universes 44 times a second for nobody.

## 4. Applying frames

`UShowControlSubsystem::ReceiveDMXUniverse` decodes only:

- in External mode;
- the fixtures patched in that universe (`GetFixturesInUniverse`);
- among those, the fixtures whose own channels differ from the last frame of the universe (a `memcmp` over their footprint).

The last frames are forgotten when the patch revision or the control source changes, so the next frame is applied in full. Consoles repeat unchanged frames about 44 times a second, and those now cost nothing. The function returns the number of fixtures updated, which the tests check. A fixture set by hand while External is not overwritten until the console changes one of its channels. This is the price of skipping repeats, and it is documented here on purpose.

## 5. Patch panel

- **A second header bar.** It holds:
  - **DMX IN: ON / OFF**, which calls `RequestSetDMXInput`;
  - the input status: what it listens on, live universes and sources, frame counts, invalid datagrams, or the error in the warning colour;
  - **FIX CONFLICTS**;
  - **PATCH SELECTION AT** with universe and address fields;
  - the usage of each universe (`U1 412/512 !`).
- **Renamed button.** PATCH UNPATCHED is now **AUTO PATCH** and uses `PlanUnpatched`.
- **Writes.** Every tool goes through `RequestPatchFixtures` as one undo step.
- **Updates.** The panel rebuilds on `OnPatchChanged` on the next tick. The input line updates on `OnInputChanged` without a rebuild. There is still no tick.

## 6. Console (development builds only)

| Command | What it does |
|---|---|
| `StageCraft.DMX.Input On\|Off\|Status` | Switches the input on or off, or reports its status |
| `StageCraft.DMX.Settings [Bind=…] [ArtNetFirst=n] [SACNFirst=n] [ArtNet\|SACN\|ArtPoll=On\|Off]` | Logs or changes the input settings: saved on this PC, and a running input restarts with them |
| `StageCraft.DMX.AutoPatch` | Patches every unpatched fixture |
| `StageCraft.DMX.FixConflicts` | Repairs every conflict |
| `StageCraft.DMX.PatchSelection <u> <a>` | Patches the selection one after another from u.a |
| `StageCraft.DMX.SendTest ArtNet\|sACN <wire universe> <channel> <value> [count]` | Sends one real frame to this PC, for scripted runs |

## 7. Alternatives considered

- **DMX Engine plugin.** Rejected for the reasons in §3.1. A bridge on top of it can still be added later behind `UStageDMXBridge`.
- **Polling sockets from a tickable subsystem.** This would mean work every frame, even when idle. Rejected under §21.
- **A game-thread task per packet.** Up to 44 × universes tasks a second. The inbox coalesces them instead.
- **HTP merge of several consoles.** Real, but rare for a visualiser. One source per universe with sACN priority is predictable. A merge can be added inside the arbiter later.
- **Keeping the patch in `UShowControlSubsystem`.** It mixed playback with addressing and had no index. The split follows the "one responsibility" rule; the show control depends on the patch, not the other way round.

## 8. Consequences and follow-ups

- **New dependencies.** `Sockets` and `Networking` are new private dependencies. A Shipping build includes the receiver; the console commands are compiled out.
- **Firewall.** Windows may ask to allow StageCraft through the firewall the first time the input is switched on. The automated tests use other ports on loopback.
- **Not done:**
  - DMX output (Art-Net / sACN send): done in §9;
  - HTP / LTP merging;
  - RDM;
  - several DMX modes per fixture (the todo item stays open);
  - choosing the network card or the universe offsets in the panel (for now `StageCraft.DMX.Settings`, development builds only);
  - sACN universe discovery;
  - ArtPollReply on every card when listening on all of them (it announces the address the OS prefers).
- **Not verified on this machine:** a real console on the LAN, the firewall prompt, discovery by a real controller, and multicast from another PC. Loopback unicast and ArtPoll are tested.

## 9. Amendment (2026-11-04, Phase F5a, STATE.md #65): Art-Net / sACN output

Gevor's brief for F5 asked for DMX output (Art-Net and sACN) to drive real fixtures, nodes and visualisers, with patch options and a fixture schedule import to follow in F5b. Everything below follows the input's architecture (§3) wherever it can.

### 9.1 One transport owner

- **Decision.** `UStageDMXNetworkSubsystem` is the Art-Net / sACN transport of the world in both directions. `UStageNetworkDMXBridge` owns the existing `FStageDMXNetworkReceiver` and a new `FStageDMXNetworkSender`, each opened and closed on its own, and stays the show control's single bridge.
  - The bridge is installed while input **or** output is on and removed when both are off (`EnsureBridge`, `ReleaseBridgeIfIdle`).
  - Input and output are independent states: `EStageDMXInputState` (Off, Listening, Failed) and `EStageDMXOutputState` (Off, Sending, Failed), each with its own error, delegate and remembered on/off.
  - `UStageDMXBridge::bOutputEnabled` follows whether the sender is open, so a bridge that only listens never makes the show render.
- **Alternatives rejected.**
  - A second bridge slot in the show control, or a separate output subsystem: two owners of the same network card, numbering and patch-change handling, and a show control that would need a list of bridges.
  - The DMX Engine plugin's output ports: rejected for the input in §3.1, for the same reasons.

### 9.2 No idle tick, no idle render

- **Dirty universes.** Every write of a fixture's output by the show goes through one helper, `UShowControlSubsystem::WriteFixtureOutput`. It applies the values (`SetAttributes`, only what differs) and, when something changed and output is active, marks the fixture's universe dirty. The three mix paths (fade frame, fade end, `ResolveFixture`) use it; the masters re-resolve every fixture, so they mark too.
- **Full frames** are asked for with `RefreshDMXOutput` when the output opens or the universes it sends change. A patch change is seen through `GetPatchRevision` and also sends every universe again.
- **Render.** `TickDMXOutput` renders only the dirty universes, at most `DMXOutputRateHz` times a second (set from the settings' rate), into one reused buffer (`RenderDMXUniverseInto`). An idle show sends its next change at once instead of waiting out an interval.
- **Tick.** `IsTickable` includes output only while a universe is waiting (`HasPendingDMXOutput`). An idle show with output on does not tick at all.
- **Keep-alive** is the sender's job (§9.3), so receivers still see a steady stream.

### 9.3 `FStageDMXNetworkSender`: network I/O off the game thread

- **Threading.** One UDP socket and one `FRunnable` thread of its own (Rules §29).
  - The game thread posts a universe's frame (`Post`) into a mailbox that keeps only the newest frame per universe, under a lock, and wakes the thread with an `FEvent`. Posting never waits on the network.
  - The thread owns the per-universe streams (last slots, last send time, sequence numbers); nothing else touches them.
- **Packets.** `StageDMXNet::BuildArtDmx` and `BuildSACNData` (already tested).
  - Art-Net sequence runs 1..255 (0 means unsequenced); sACN has its own sequence per universe.
  - The sACN CID, priority and source name come from the settings.
- **Keep-alive.** An unchanged universe is sent again every second (E1.31 §6.6.1 and common Art-Net practice), so the game thread renders only on change.
- **Which universes.** `SetUniverses` takes the universes to send: the patched ones, minus the muted ones, and none while the show is External (§9.5).
  - A universe that leaves the set gets three sACN `Stream_Terminated` packets (E1.31 §6.7.1) and is no longer kept alive.
  - A frame for a universe outside the set is dropped.
- **Close** terminates every sACN stream, joins the thread, then closes the socket. It is idempotent and runs from the destructor and from the bridge's `Stop` / `BeginDestroy`.
- **Socket.**
  - Bound to the chosen network card (any free port), so broadcast and multicast leave through it.
  - Broadcast is on; multicast TTL is 16, with the card as the multicast interface.
  - Multicast loopback is on, so a visualiser on the same PC sees the output.
  - Blocking, because only the sender's thread waits on it.
- **Status.** Atomic counters (Art-Net and sACN packets, send errors) and the universes being sent, for the Patch panel and the console.

### 9.4 Settings (per PC)

`FStageDMXOutputSettings` is stored in `UStageCraftUserSettings` (GameUserSettings.ini) and always read through `StageDMXOutput::Sanitize`:

| Field | Values | Default |
|---|---|---|
| `bEnabled` | remembered on/off | off |
| `ArtNet` | Off, Broadcast (255.255.255.255 from the chosen card), Unicast | Broadcast |
| `SACN` | Off, Multicast (239.255.hi.lo), Unicast | Multicast |
| `UnicastAddress` | IPv4 for the protocols set to Unicast; anything else is emptied | empty |
| `SACNPriority` | 0..200 | 100 |
| `RateHz` | 1..44 | 44 |
| `MutedUniverses` | sorted, unique, 1..63999 | none |
| `Cid` | this PC's sACN component ID, made once and kept by a reset to defaults | new GUID |

- **Shared with the input.** The network card (`BindAddress`) and the universe numbering (`ArtNetFirstPortAddress`, `SACNFirstUniverse`) stay in `FStageDMXInputSettings`, so each is set in one place. A change to them restarts a running output too.
- **Unicast without an address** is refused when the output opens, with the reason, rather than quietly sending somewhere else.
- **Own-frame guard.** The receiver ignores sACN frames carrying this PC's CID (`FStageDMXReceiverConfig::IgnoredSource`). With multicast loopback on, our own output would otherwise arrive as input.
- **First save.** The first time the settings are saved after this change, GameUserSettings.ini gains a `DMXOutput=` line with the defaults and this PC's CID. Later saves keep the same CID.

### 9.5 Output and the control source

Output sends only while the show is Internal.
- **Switching to External** (DMX IN drives the fixtures): the subsystem sets the sent universes to none, which terminates the sACN streams. StageCraft must not become a second source on the wire next to the console it follows.
- **Back to Internal:** every universe is sent again.

Merging the input with the output, or passing it through, is a follow-up.

### 9.6 Requests and UI

- **Requests.** `AModularPlayerController::RequestSetDMXOutput(bool)` and `RequestApplyDMXOutputSettings(Settings)`.
  - Refusals (no protocol, unicast without an address, no socket) go to `OnRequestRejected`.
  - Output is a live device action, like DMX IN, so it is not undoable.
- **Patch panel DMX OUT line:**
  - ON/OFF;
  - ART-NET (OFF, BCAST, UNICAST) and sACN (OFF, MCAST, UNICAST), each cycled by a click;
  - the unicast IPv4 field, and the PRI and HZ fields;
  - one chip per patched universe, lit while it is sent; a click mutes or unmutes it.
- **Status line** below the controls shows one of:
  - "Sending 1 universe · Art-Net to 127.0.0.1, sACN to 127.0.0.1 · 189 Art-Net, 189 sACN packets";
  - "Paused while DMX IN drives the show";
  - the error.
- **The fields read the saved settings back,** so a refused or sanitized edit snaps back.
- **Push, never poll.** The line updates on `OnOutputChanged`: at once on a state change, and twice a second while sending (one looping timer that exists only while sending).

### 9.7 Console and bench (development builds only)

| Command | What it does |
|---|---|
| `StageCraft.DMX.Output On\|Off\|Status` | Switches the output on or off, or reports its status |
| `StageCraft.DMX.OutputSettings [ArtNet=Off\|Broadcast\|Unicast] [SACN=Off\|Multicast\|Unicast] [To=<IPv4>] [Priority=n] [Rate=n] [Mute\|Unmute=<universe>]` | Logs or changes the output settings through the controller |
| `StageCraft.Bench.Fixtures … DMXOut=1` | Also sends every bench universe as Art-Net and sACN unicast to 127.0.0.1 on unused ports (26454 / 25568), through a bench-owned bridge; refused while the user has DMX IN or OUT on |

### 9.8 Consequences and follow-ups

- **Firewall.** Outgoing UDP needs no prompt on Windows. A receiving visualiser on another PC may need its own rule.
- **Not done (todo.md):**
  - ArtPoll discovery of output targets (use broadcast or a typed unicast address);
  - ArtSync;
  - passthrough or merge of DMX IN to OUT;
  - RDM;
  - per-universe destinations.
- **Not verified on this machine:** a real node, console or visualiser on the LAN receiving our broadcast, multicast or unicast. Loopback unicast is tested, and multicast loopback is on.

## 10. Amendment (2026-11-04, Phase F5b, STATE.md #66): pan / tilt patch options and the fixture schedule

### 10.1 Invert Pan, Invert Tilt, Swap Pan/Tilt

- **Problem.** A moving head hung upside down, the other way round or sideways turns the wrong way for the console or visualiser downstream. Consoles fix this in the patch, not in the cues.
- **Decision: wire-only.** `FStageDMXPatchOptions {bInvertPan, bInvertTilt, bSwapPanTilt}` is a SaveGame member of `ALightingFixtureActor` (its fields SaveGame too, as `FStageDMXPatch`'s must be). Only `WriteDMX` and `ReadDMX` map through it.
  - The attributes, the beam on the stage, the programmer and the cues stay as they are.
  - A cue therefore plays the same on a fixture hung either way. What changes is what the real fixture receives.
- **One pure mapping for both directions.** `StageDMXPatchOptions::GetChannelSource(ChannelAttribute, Options)` returns the attribute a channel carries and whether it is inverted.
  - **Swap:** the pan channel carries the tilt (normalised in the tilt's own range) and the other way round.
  - **Invert:** belongs to the console axis. Invert Pan inverts the pan on whichever channel it travels.
  - **16-bit channels:** follow automatically.
  - **The CMY inversion** (`ResolveDMXAttribute`) still applies first; the two inverts combine by XOR.
  - **Round trip.** The mapping is a per-channel permutation plus inversion, so `ReadDMX(WriteDMX(x)) == x` for all eight combinations (tested).
- **Unusable options.** `StageDMXPatchOptions::GetEffective` turns off what the DMX mode cannot use: a swap needs both a pan and a tilt channel, and an invert needs its axis. The stored options are kept as given, so a mode change does not lose them. When no option is set, the codec skips the channel scan.
- **Undo for free.** `StageCraft.Param.Patch.InvertPan`, `InvertTilt` and `SwapPanTilt` are Bool patch parameters in `IsPatchParameter`.
  - `RequestParameterChange` records them as undoable steps like the universe and address.
  - The inspector's Patch section offers them for the axes the mode has.
  - `SetPatchOptions` reports through `NotifyPatchSubsystem`, so the patch revision moves: DMX output sends the universe again, and input reads its next frame in full.
- **No scene format change.** Item state is tagged SaveGame serialization (`AModularBaseActor::CaptureSnapshot`), so states and scenes saved before #66 load with no options.
- **UI.** Each Patch panel row has INV P, INV T and SWAP toggles: lit when on, greyed out without the axis. Each click is one `RequestParameterChange`.

### 10.2 Fixture schedule import

- **Problem.** A rig is usually planned in a spreadsheet: fixture number, universe, address. Typing hundreds of addresses again is slow and error-prone.
- **Decision: read the patch sheet back.** The import reads the CSV the patch sheet already writes (`StagePatchSheet::ToCsv`). That CSV gains `Invert Pan`, `Invert Tilt` and `Swap Pan/Tilt` columns at the end (Yes or empty), so an export imports back exactly.
- **`StagePatchSchedule::Parse` (pure)** reads the file as user input:
  - **Format:** a UTF-8 BOM; CRLF or LF; comma or semicolon (taken from the header row); double-quoted fields with `""` escapes; the sheet's leading-apostrophe formula guard stripped.
  - **Header:** the first row with a "Fixture ID" field. Lines before it (the title, the summary) are skipped.
  - **Columns** are found by name in any order. Fixture ID, Universe and Address are required; the three option columns are optional.
  - **Values:** numbers are digits only, at most six (fixture ID 1–99999, universe 1–63999, address 1–512). Universe and Address both empty means unpatch; one of them alone is an error. Flags are Yes/No, True/False, 1/0 or On/Off, and empty means No.
  - **Limits:** 1 M characters per file (refused whole), 10 000 rows (reading stops with one reason), 512 characters per field.
  - **Errors:** a NUL or another bad field reports that row with its line number, and the other rows still read.
- **`StagePatchSchedule::Plan` (pure)** checks the rows against the stage's fixtures.
  - **Rejected, each with its line:** an unknown fixture ID; an ID on more than one row (every such row, since the file is ambiguous); a patch for a fixture that takes no DMX; an address its footprint does not fit at (`StageDMXPatching::Fits`).
  - **Unchanged:** rows equal to the stage are counted, not changed.
  - **Conflicts:** overlaps are allowed (§2), computed over the whole stage as it would be after the plan, and reported.
- **`UStageDMXPatchSubsystem::PlanSchedule`** runs both over the registered fixtures and returns `FStagePatchScheduleImport`:
  - the parse and the plan;
  - the changes as `FStageDMXPatchAssignment`s, each carrying its options only when the file set them;
  - a one-line summary and the first four reasons.
  
  `LoadScheduleFile` refuses a missing file or one larger than the character limit could need, before reading it.
- **Requests.**
  - `AModularPlayerController::RequestPlanPatchSchedule(Path, OutImport)` reads and plans. It changes nothing, and refuses a file that is unreadable, too large or not a schedule.
  - `RequestPatchFixtures` now takes the optional options. A fixture's change is a list of patch parameters (universe and address when the patch moves; each option that differs), all evaluated by the GameMode first, then applied and recorded as **one undo step**. On a failure, everything applied so far is put back.
- **UI.** Patch panel IMPORT CSV opens the file picker, starting in the Reports folder where EXPORT PDF writes the CSV. The plan shows in a bar above the table: summary, reasons, APPLY and CANCEL. Nothing changes before APPLY.
- **Console (development builds):** `StageCraft.DMX.ImportSchedule <path> [Apply]`.

### 10.3 Consequences

- **Not done:** importing fixtures that are not on the stage yet (a schedule only re-patches existing fixtures), mode or type columns, a GDTF / MVR import.
- **The console command splits arguments on spaces,** so a path with spaces needs the panel.
