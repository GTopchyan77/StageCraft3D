# ADR 0019: DMX patch subsystem and Art-Net / sACN input

- **Status:** accepted. See STATE.md #46.
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
  - DMX output (Art-Net / sACN send);
  - HTP / LTP merging;
  - RDM;
  - several DMX modes per fixture (the todo item stays open);
  - choosing the network card or the universe offsets in the panel (for now `StageCraft.DMX.Settings`, development builds only);
  - sACN universe discovery;
  - ArtPollReply on every card when listening on all of them (it announces the address the OS prefers).
- **Not verified on this machine:** a real console on the LAN, the firewall prompt, discovery by a real controller, and multicast from another PC. Loopback unicast and ArtPoll are tested.
