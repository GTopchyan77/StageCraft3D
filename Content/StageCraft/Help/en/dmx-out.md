# DMX OUT: Art-Net and sACN to real fixtures
keywords: dmx out, output, send, art-net, artnet, sacn, e1.31, broadcast, multicast, unicast, node, priority, rate, real fixtures, visualiser
panel: StageCraft.Panel.Patch
control: DMXOut

DMX OUT in the Patch panel sends the show as Art-Net and/or sACN to real fixtures, a DMX node or a visualiser.
- Art-Net: off, broadcast to every node, or unicast to the IP address you type.
- sACN: off, multicast to each universe's group (every sACN receiver listens to it), or unicast.
- PRI is the sACN priority (0-200, default 100). HZ is the frame rate while the look changes (1-44); unchanged universes repeat once a second.

Only universes with patched fixtures are sent, and only while the cue list drives the show. Click a universe chip to mute it on this PC. Settings are remembered on this PC.
