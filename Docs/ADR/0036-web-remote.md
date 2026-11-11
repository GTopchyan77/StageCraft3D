# ADR 0036: Web Remote (Step 5)

- **Status:** accepted for WR1 (2026-11-11, STATE.md #92).
- **Related:**
  - ADR 0034 §2 (tiers: Web Remote is **Pro**; D8 puts it at Step 5, after the download manager).
  - ADR 0035 (MIDI and OSC): the address space, the pure `StageRemote` layer and the controller's `RequestRemoteCommand` are reused as they are.
  - ADR 0022 §5–7 (the DCC bridge's TCP server: one worker thread, a locked event queue, game-thread drains).
  - Rules.md: Architecture (state by lifetime, the request bridge, push not poll), Engineering Standard §29 (threads), §55 (validate at the trust boundary), §59 (handles), §66 (security boundaries).

## 1. Problem

A lighting designer walks the room with a phone or tablet: GO the next cue from the back of the hall, ride a playback, black out. OSC (ADR 0035) needs an OSC app and a layout on the tablet. A browser needs nothing installed: StageCraft serves the remote page itself.

## 2. Decision overview

- **Our own small HTTP/1.1 and WebSocket server** on the engine's TCP sockets (`FStageWebRemoteServer`), built like the DCC bridge server: one worker thread owns every socket, the game thread only drains events and posts state. The engine's `HTTPServer` module has no WebSocket server, and `WebSocketNetworking` is an experimental plugin around libwebsockets; the part we need (one page, three JSON routes, text frames) is a few hundred lines of pure, testable code.
- **The pure layer `StageWebRemote`** (`Private/Remote/StageWebRemoteProtocol`): HTTP request parsing (incremental, capped), routing by method and request path, the WebSocket handshake (RFC 6455 accept key with `FSHA1` and `FBase64`), frame decoding and encoding, UTF-8 checks, the JSON messages, PIN pairing and settings sanitizing. No sockets, no UObjects; any thread.
- **One world subsystem owns the server:** `UStageWebRemoteSubsystem` (Game and PIE worlds), like `UStageRemoteControlSubsystem`. `OnWorldBeginPlay` reopens from the saved settings, `Deinitialize` stops the server. No tick.
- **Commands are the OSC address space.** A phone sends `{"type":"command","path":"/stagecraft/playback/3/level","value":0.5}`; `StageRemote::ParseOsc` resolves it exactly like an OSC message, so the web and OSC can do the same things and nothing else. The subsystem queues the frame's commands (`StageRemote::Enqueue`: continuous values coalesced, at most 256) and hands each to the command handler, which the local `AModularPlayerController` binds to `RequestRemoteCommand`. The command carries `Source = Web`, so the controller asks the GameMode's `EvaluateWebRemote` instead of `EvaluateRemoteControl`, then `EvaluateCueRequest` as for the desk.
- **The show's state is pushed.** The subsystem binds to the show (`OnDeskStateChanged`, `OnCueStarted`, `OnCueListChanged`, `OnPlaybacksChanged`) and posts a state snapshot at most 30 times a second; the worker sends it to every paired page only when it changed.

## 3. Routes

Every request must carry a `Host` that is an IP address or `localhost` (with an optional port); anything else is `421`. This defeats DNS rebinding: a page on another site that resolves its own name to this PC cannot talk to the server. A request with an `Origin` must come from the same host and port (`403`).

| Method and path | Auth | Answer |
|---|---|---|
| `GET /` | – | The remote page (one HTML file, inline script and style; `Content-Security-Policy`, `X-Frame-Options: DENY`, `no-store`). |
| `POST /api/pair` | – | Body `{"pin":"123456","name":"Gevor's phone"}` → `200 {"token":"…"}`, `403` wrong PIN, `429` locked. |
| `POST /api/command` | `Authorization: Bearer <token>` | Body `{"path":"…","value":0.5}` → `202 {"ok":true}` when accepted for judgement (an unknown path is ignored and counted, like an OSC message), `400` for a malformed body, `401` without a paired token. For Stream Deck–style HTTP buttons and scripts. |
| `GET /api/state` | Bearer | The last state snapshot (§5). |
| `GET /ws` | hello message | WebSocket upgrade (§4). |

Anything else is `404`, a known path with another method `405`. Every answer closes the connection (`Connection: close`); the page makes one request per action and then lives on its WebSocket. Responses are JSON (`{"error":"…"}` on failure) except the page.

**Limits:** request head 8 KB, body 4 KB, the whole request within 10 s, at most 16 connections, at most 8 WebSockets, 256 KB unsent per connection (a phone that stops reading is closed).

## 4. WebSocket

- Handshake: `Upgrade: websocket`, `Connection` containing `upgrade`, `Sec-WebSocket-Version: 13`, a key that decodes to 16 bytes; anything else is `400` (`426` with `Sec-WebSocket-Version: 13` for another version).
- Browsers cannot set headers on a WebSocket, and a token in the URL ends up in logs, so the first message must be `{"type":"hello","token":"…"}` within 5 s; the server answers `{"type":"welcome"}`, then the cue list and the state. A wrong token is `{"type":"bye","reason":"…"}` and close code 1008.
- Client → server: `hello`, `command` (`path`, optional number `value`).
- Server → client: `welcome`, `state`, `cues`, `refused` (`message`: the GameMode's reason, sent to every page, once per reason like the desk), `bye` (`reason`).
- Frames: text only, unfragmented, masked (RFC 6455 §5.1), at most 4 KB. Binary or fragmented messages close with 1003, bad framing with 1002, invalid UTF-8 or JSON with 1007, too large with 1009. Ping is answered with pong. The server pings every 20 s and closes a socket that sent nothing for 60 s (browsers answer pings on their own).

## 5. Messages

```json
{"type":"state","cue":{"active":3,"label":"Chorus"},
 "playbacks":[{"name":"Strobe","level":0.5,"flash":false,"active":true,"empty":false}, …10],
 "grand":1,"blackout":false,"audio":1}
{"type":"cues","cues":[{"number":1,"label":"Preset"}, …]}
```
- `cue.active` is negative when no cue has run. The cue list is cut to 200 cues and labels to 64 characters; it is sent only when it changes.
- Numbers are the show's own; the page shows them as the desk does (playbacks 1–10).

## 6. Pairing and security (D7)

Settings, `FStageWebRemoteSettings` in `UStageCraftUserSettings` (per PC; the subsystem is the only writer; sanitized on load and set):
- **Scope:** `Off` (default) / `This computer` (listens on 127.0.0.1 only: our own socket can bind loopback, unlike the engine's OSC server) / `Network` (every card).
- **Port:** 8080 (1024–65535).

Pairing:
- The server makes a **6-digit PIN** when it starts (from `FGuid::NewGuid`, Windows' cryptographic generator). Preferences shows it with the addresses to open (`http://192.168.1.20:8080`).
- A page that sends the PIN gets a **token** (two GUIDs, 64 hex characters), compared in constant time. Tokens live in memory only: a restart or a new stage world means pairing again. At most 8 paired devices; a ninth replaces the oldest.
- **Five wrong PINs in a row from one address** lock that address out for 30 s; other devices can still pair, so one host on the LAN cannot keep the real phone out. **Twenty wrong PINs from all addresses together** replace the PIN, which bounds guessing from many addresses. From one address, guessing takes on average about two months of tries. (Changed after the code review of WR1: the first version locked everyone and replaced the PIN on every lock, which let any LAN host keep pairing locked.)
- **Connections:** at most 4 unauthenticated connections (requests and WebSockets before their hello) per address; only paired pages count toward the 8 WebSockets. A connection that is closing is shut down for writing and closed after 5 s at most. At most 4096 events wait for the game thread; more are dropped and counted. A device evicted by a ninth pairing has its open page closed.
- **NEW PIN** (Preferences) replaces the PIN; **UNPAIR ALL** forgets every device, closes their sockets and replaces the PIN.

Deny by default:
- Off until the user turns it on; `Network` must be chosen explicitly.
- Every command needs a token and goes through the GameMode. `SessionRules.bAllowWebRemote` refuses switching on and every command for a curated session; Step 6 adds the Pro entitlement `StageCraft.Entitlement.Pro.WebRemote` to `EvaluateWebRemote`.
- **No TLS.** Plain HTTP on the LAN: the PIN and tokens can be read by anyone who can sniff the network. A self-signed certificate makes every phone warn and is no better against a capable attacker on the LAN. Gevor must confirm this for D7 (§9).
- Logs never contain the PIN or a token. The one exception is the development-only console command `StageCraft.WebRemote.Status Pin`, compiled out of Shipping, which the scripted run uses to pair.

## 7. Phases and verification

| Phase | What | Verification |
|---|---|---|
| **WR1** (this) | Pure protocol, server, pairing, subsystem, controller and GameMode gate, settings, Preferences section (scope, port shown, PIN, addresses, NEW PIN, UNPAIR ALL, status), the page, dev console commands, help EN/HY | Unit tests for the pure layer; a loopback test with real sockets (page, pairing, lockout, WebSocket hello, command, state, refusal); the GameMode rule; a scripted `-game` run that pairs and sends commands from PowerShell; Editor, Game and Shipping at 0 warnings |
| WR2 | QR code with the address and PIN; remembered devices across runs; layout options on the page | Manual check on a phone |
| WR3 | Pro gate (Step 6) | Licensing tests |

## 8. Alternatives considered

- **Engine `HTTPServer` + `WebSocketNetworking`:** two engine dependencies, one experimental; neither gives us the Host/Origin checks or the limits; the WebSocket server would add libwebsockets threads we do not own.
- **Remote Control API plugin:** exposes properties and functions generically, which bypasses the GameMode (Rules.md Architecture) and is far larger than the desk's live controls.
- **Commands as HTTP only (no WebSocket):** simpler, but faders would make a request per move and the page could not follow the desk.
- **Token in the WebSocket URL:** shows up in logs and history; a hello message costs nothing.

## 9. Open for Gevor

- D7: plain HTTP on the LAN with PIN pairing (no TLS) — acceptable?
- Should pairings survive a restart (a token saved per PC), at the cost of a stolen token staying valid?

## 10. Rollback

Remove the subsystem, the settings field, the controller request and the Preferences section; nothing is saved in scenes or the profile.
