# The O11 Session plug-in

O11 Session is a VST3 plug-in for playing together over the internet: your track's input goes to the room, and the other players come back
on the plug-in's outputs, in time with the room's intervals. This folder is
the plug-in built on [JUCE 8.0.9](https://juce.com) over `o11proto`, the
clean-room NINJAM client, server and engine in this repository. It replaces
`ninjam/vst3` in the older Jamsa repository (`LeonidStern/Jamsa`), the plug-in
on Steinberg's SDK and VSTGUI over Cockos' NJClient, whose UI, engine wrapper,
tests and CI it ports.

Nothing in here derives from the GPL code of NINJAM in that repository (`ninjam/njclient*`,
`ninjam/mpb*`, `ninjam/netmsg*`, `ninjam/audiostream*`, `ninjam/server`,
`WDL`, `jmde`): the protocol, the codecs and the mixing are `o11proto`'s.

## Building

Needs CMake 3.25 or later, Ninja (or another generator) and a C++17 compiler.
JUCE, Opus, FLAC, miniupnpc, libnatpmp and Ableton Link (tag `Link-4.1`,
header-only, with its `modules/asio-standalone`) are fetched by CMake (all
built in and linked statically, as in `ninjam/vst3`; there is no Ogg or
Vorbis). On Windows Link needs `ws2_32` and `iphlpapi`, which the build adds. On Linux
the JUCE GUI needs the X11, freetype and fontconfig development packages
(`libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
libfreetype-dev libfontconfig-dev`).

    cmake -S plugin -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

The plug-in is `build/O11Session_artefacts/Release/VST3/O11Session.vst3`. On Windows,
configure with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` for a plug-in that needs no Visual C++
runtime; on macOS add `"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"` for a
universal build. `-DO11_TESTS=OFF` leaves the tests out.
`.github/workflows/o11session.yml` does all of this on the three platforms and
checks the plug-in with pluginval.

### Targets

- `o11_core`: a static library with the engine wrapper (`engine.cpp`), the
  built-in server (`hostserver.cpp`), following the host (`hostsync.cpp`),
  Ableton Link (`linksync.cpp`), the metronome's click (`click.cpp`) and the
  texts of the controls (`uitext.cpp`). It needs only `juce_core`,
  `juce_audio_basics`, Link and o11proto (the engine and the port mapper), so
  the tests link it without a plug-in host. It is built with hidden symbols
  and `ASIO_DISABLE_VISIBILITY`, so Link's and asio's code stays inside the
  plug-in, apart from the host's own Link.
- `O11 Session`: the VST3, from `o11_core` plus `processor.cpp` (the
  `juce::AudioProcessor`), `editor.cpp` and `views.cpp` (the editor).
- `o11_unit_test` and `o11_e2e_test` (see Tests). o11proto's own tests are
  built and run too, as it is added as a subdirectory.

## What is ported

Everything the old plug-in did on its own, in the same way:

- The engine wrapper: the network thread, the command queue, connecting with
  the server's licence shown in the editor (the answer has a minute before
  the connection is given up), the chat with `/msg`, `/topic`, `/bpm` and
  `/bpi` handled by the server, votes for the room's tempo and interval,
  transmit and monitor, the metronome (the same click, ported sample for
  sample), the Opus bitrate list (64 to 510 kbps) and `!bitrate` and `!codec`
  as chat commands, the other players' volume, pan, mute and solo, saved with
  the state by name, and each player on their own stereo output pair
  ("Player 1" to "Player 8").
- The built-in server (`host on`), on o11proto's server: on the first free port
  from 2049, anonymous users with chat and vote, 120 BPM and 4 BPI to start,
  Opus and FLAC allowed, the host's Opus bitrate as the room's. A single vote
  changes the room's tempo, interval or codec, as before.
- The room's addresses: the router is asked to forward the port (UPnP-IGD,
  then NAT-PMP, with o11proto's `PortMapper`, started and stopped with the
  server; its `[portmap]` lines go to the chat), and a "what's my IP" service
  (api.ipify.org, then checkip.amazonaws.com, over plain HTTP on a thread of
  its own; `O11_WHATSMYIP=host:port/path` points it elsewhere, `off` asks
  nobody) is asked for the public address, for a port forwarded by hand. The
  host bar's address box shows the address people connect to: the router's
  when it forwarded the port (WAN), else the looked-up public address (WAN),
  else the LAN address; the port text says "Port open" (also when a player
  joined from the internet), "Asking the router…" or "Forward port N", with
  the router's complaint in the tooltip. `O11_PORTMAP=off` leaves the router
  alone and `O11_HOST_PORT=0` takes any free port (the tests).
- Following the host ("Sync"): the intervals start on the host's bars while
  it plays at the room's tempo, with the interval playing when it starts
  shortened or lengthened once, and the room is voted to the host's tempo when
  that changes. The host's position comes from
  `juce::AudioPlayHead::PositionInfo`.
- Ableton Link, while Sync is on in a room (`linksync.cpp`): the room leads
  Live through Link. One instance in the program leads Link (the first in a
  room with Sync on; another takes over when it leaves); the others leave it
  alone. The leader puts the room's tempo on Link when the intervals change
  to it, and the intervals' beat (Link's quantum is BPI), moving Link's
  timeline at once when it starts, when the BPM or BPI changes or the
  timeline jumped, and by steps of at most 2 ms as the audio clock drifts.
  While Link has peers, the intervals aren't moved to the host's bars
  (BarSync), so the two don't chase each other. A block is taken to be heard
  one block after `process()` is called (by the earliest calls of the last
  two seconds), plus what the calibration measures from the host's beat while
  it plays at the room's tempo. A tempo change in Live still votes the room;
  the room's tempo goes back on Link when the room doesn't take the vote, or
  another app set its own (the tempo keeper), and an app that keeps moving
  Link's beat or tempo makes the plug-in stop leading (contested) until Sync
  goes off and on. Out of a room, or with Sync off, the plug-in isn't in
  Link. The chat says when Live follows the room, and once that Link sees no
  other app ("Turn on Link in Live ..."); the Sync chip shows "Link", and the
  Sync button's tooltip, the footer and the tempo note say what Link does.
  `status` reports `link`, `peers`, `linktempo`, `leads`, `linkaligned`,
  `linkerr`, `latency`, `jumps`, `steps` and `restores`, as before. The
  host's position for it comes from `juce::AudioPlayHead::PositionInfo`
  (playing, ppq and tempo) instead of VST3's `ProcessContext`.
- The state: the same text format (`O11-SESSION-STATE 1`), so a state saved
  by the old plug-in loads here.
- The editor, in JUCE: the same 720 x 330 layout, resizable with the same
  limits and zoom, the same colours, fonts, texts and tooltips; the BPM box
  that drags and takes typed values, the BPI, codec and bitrate menus, the
  faders and pan knobs, the meters, the beat cells, the chat, the host bar
  with the server's address to copy and the port's state, and the licence over
  everything.
- The text commands of `Engine::command()` (`connect`, `disconnect`, `name`,
  `host`, `chat`, `license`, `transmit`, `monitor`, `metronome`, `bitrate`,
  `codec`, `follow`, `vote`, `status`, `users`, `chatlog`, `nextinterval`,
  `syncnote`, `synctip`), for the tests and automation.

### Changed on purpose

- A player's channel you hear for the first time starts at 0 dB, as in the old
  plug-in (o11proto's engine starts them at -12 dB on its own).
- `status` has the port mapping and lookup fields (`public`, `lookedup`,
  `joinedfrominternet`) besides the Link fields.
- Link: the old plug-in held off `process()` with its audio mutex to stop
  leading Link; this engine has no such mutex, so `LinkSync::release()` waits
  for a `process()` running at that moment to return (a flag each side sets,
  sequentially consistent) and the audio thread still takes no lock. The
  texts that named the old plug-in ("Another Jamsa ...") name O11 Session.
- The host bar has one address box (the old plug-in showed the WAN address
  only): it shows the LAN address until a public address is known, and
  switches to WAN then. The plug-in joins its own room at `127.0.0.1`, not
  `localhost` (which may resolve to `::1` while the server listens on IPv4).
- The old plug-in loaded the plug-in to write its VST3 manifest at build time;
  here `VST3_AUTO_MANIFEST` is off, so the build never loads the plug-in. The
  host reads the plug-in's classes from the binary, as it does for plug-ins
  without a manifest.
- The server's licence used to be answered in a blocking callback on the
  network thread; o11proto's engine defers it, and the wrapper answers when the
  editor does, or declines when a minute passes or the plug-in quits.

## Tests

- `o11_unit_test`: the saved state (also in a locale that writes 95,5 when
  one is installed), the players' names, the text commands, the bar math and
  the bar sync against a simulated host (loops, moves, jitter, long runs),
  the tempo follower, the texts about the host, the click (shape, spectrum,
  the same at 44.1, 48 and 96 kHz), the controls' texts, and the room's
  addresses: the port mapper's and the server's lines as read, the public
  address lookup against a "what's my IP" service on localhost, and what the
  host bar shows for each case (the router's address, a private one, pending,
  the lookup's result, nothing). The port mapper itself is tested in o11proto,
  against a fake router. And Ableton Link, as in the old plug-in: the beat and
  phase math (with ten minutes of a drifting audio clock), when blocks start
  on Link's clock, the calibration, the tempo keeper, the texts about Link,
  and one instance leading Link in real time while another can't.
  The tests that use Link run one at a time (`RESOURCE_LOCK ableton_link`):
  Link apps on one machine see each other.
- `o11_e2e_test`: two engine wrappers in one process, one hosting a room
  (on a free port, without the router or the lookup), the other connecting to
  it; a 997 Hz tone through `process()` with a
  simulated clock, checked for an SNR of at least 35 dB on Opus and 60 dB
  after `codec flac`; the bitrate command reaching the room, transmit off
  giving silence, disconnecting and closing the room. With Sync on in both,
  the host leads Link and the guest doesn't; a Link peer in the process (as
  Live with its Link on) takes the room's 120 BPM, and 100 after a vote; with
  Sync off in the host, the guest leads Link. The peer's part is skipped when
  Link doesn't see it (no multicast). The old plug-in's Python end-to-end
  checks of a host that follows Link (the measured latency, the restores, the
  hint once its Link is off) are not ported; the unit tests cover that logic.

## Deferred

Not in this version; `ninjam/vst3` has them on its own stack:

- Live mode and CLAP.

## Licence

The plug-in's own sources (`plugin/`) are by the O11 Session contributors. JUCE is
available under the AGPLv3 or a commercial licence from JUCE; the plug-in is
built without `JUCE_DISPLAY_SPLASH_SCREEN=0`, as the free licence requires.
Opus, FLAC, miniupnpc and libnatpmp are BSD licensed; o11proto is this
repository's. Ableton Link is under the GPL, version 2 or later, and is in
every build, so the plug-in as built is GPL (with JUCE under its AGPLv3, the
combination is under the GPLv3 and AGPLv3 terms) unless Link and JUCE are
licensed commercially. Under which licence the plug-in's own sources are
released is decided at release, by Leo.
