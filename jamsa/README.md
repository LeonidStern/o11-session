# Jamsa on JUCE

Jamsa is a VST3 plug-in for playing together over the internet with NINJAM
servers: your track's input goes to the room, and the other players come back
on the plug-in's outputs, in time with the room's intervals. This folder is
the plug-in built on [JUCE 8.0.9](https://juce.com) over `njproto`, the
clean-room NINJAM client, server and engine in this repository. It replaces
`ninjam/vst3`, the plug-in on Steinberg's SDK and VSTGUI over Cockos' NJClient,
whose UI, engine wrapper, tests and CI it ports.

Nothing in here derives from the GPL code of NINJAM (`ninjam/njclient*`,
`ninjam/mpb*`, `ninjam/netmsg*`, `ninjam/audiostream*`, `ninjam/server`,
`WDL`, `jmde`): the protocol, the codecs and the mixing are `njproto`'s.

## Building

Needs CMake 3.25 or later, Ninja (or another generator) and a C++17 compiler.
JUCE, Opus, FLAC, miniupnpc and libnatpmp are fetched by CMake (all built in
and linked statically, as in `ninjam/vst3`; there is no Ogg or Vorbis). On Linux
the JUCE GUI needs the X11, freetype and fontconfig development packages
(`libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev
libfreetype-dev libfontconfig-dev`).

    cmake -S jamsa -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

The plug-in is `build/Jamsa_artefacts/Release/VST3/Jamsa.vst3`. On Windows,
configure with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` for a plug-in that needs no Visual C++
runtime; on macOS add `"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"` for a
universal build. `-DJAMSA_TESTS=OFF` leaves the tests out.
`.github/workflows/jamsa.yml` does all of this on the three platforms and
checks the plug-in with pluginval.

### Targets

- `jamsa_core`: a static library with the engine wrapper (`engine.cpp`), the
  built-in server (`hostserver.cpp`), following the host (`hostsync.cpp`),
  the metronome's click (`click.cpp`) and the texts of the controls
  (`uitext.cpp`). It needs only `juce_core`, `juce_audio_basics` and njproto
  (the engine and the port mapper), so the tests link it without a plug-in
  host.
- `Jamsa`: the VST3, from `jamsa_core` plus `processor.cpp` (the
  `juce::AudioProcessor`), `editor.cpp` and `views.cpp` (the editor).
- `jamsa_unit_test` and `jamsa_e2e_test` (see Tests). njproto's own tests are
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
- The built-in server (`host on`), on njproto's server: on the first free port
  from 2049, anonymous users with chat and vote, 120 BPM and 4 BPI to start,
  Opus and FLAC allowed, the host's Opus bitrate as the room's. A single vote
  changes the room's tempo, interval or codec, as before.
- The room's addresses: the router is asked to forward the port (UPnP-IGD,
  then NAT-PMP, with njproto's `PortMapper`, started and stopped with the
  server; its `[portmap]` lines go to the chat), and a "what's my IP" service
  (api.ipify.org, then checkip.amazonaws.com, over plain HTTP on a thread of
  its own; `JAMSA_WHATSMYIP=host:port/path` points it elsewhere, `off` asks
  nobody) is asked for the public address, for a port forwarded by hand. The
  host bar's address box shows the address people connect to: the router's
  when it forwarded the port (WAN), else the looked-up public address (WAN),
  else the LAN address; the port text says "Port open" (also when a player
  joined from the internet), "Asking the router…" or "Forward port N", with
  the router's complaint in the tooltip. `JAMSA_PORTMAP=off` leaves the router
  alone and `JAMSA_HOST_PORT=0` takes any free port (the tests).
- Following the host ("Sync"): the intervals start on the host's bars while
  it plays at the room's tempo, with the interval playing when it starts
  shortened or lengthened once, and the room is voted to the host's tempo when
  that changes. The host's position comes from
  `juce::AudioPlayHead::PositionInfo`.
- The state: the same text format (`NINJAM-VST3-STATE 1`), so a state saved
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
  plug-in (njproto's engine starts them at -12 dB on its own).
- `status` no longer reports the Link fields (`link*`); see below. The port
  mapping and lookup fields (`public`, `lookedup`, `joinedfrominternet`) are
  there.
- The host bar has one address box (the old plug-in showed the WAN address
  only): it shows the LAN address until a public address is known, and
  switches to WAN then. The plug-in joins its own room at `127.0.0.1`, not
  `localhost` (which may resolve to `::1` while the server listens on IPv4).
- The old plug-in loaded the plug-in to write its VST3 manifest at build time;
  here `VST3_AUTO_MANIFEST` is off, so the build never loads the plug-in. The
  host reads the plug-in's classes from the binary, as it does for plug-ins
  without a manifest.
- The server's licence used to be answered in a blocking callback on the
  network thread; njproto's engine defers it, and the wrapper answers when the
  editor does, or declines when a minute passes or the plug-in quits.

## Tests

- `jamsa_unit_test`: the saved state (also in a locale that writes 95,5 when
  one is installed), the players' names, the text commands, the bar math and
  the bar sync against a simulated host (loops, moves, jitter, long runs),
  the tempo follower, the texts about the host, the click (shape, spectrum,
  the same at 44.1, 48 and 96 kHz), the controls' texts, and the room's
  addresses: the port mapper's and the server's lines as read, the public
  address lookup against a "what's my IP" service on localhost, and what the
  host bar shows for each case (the router's address, a private one, pending,
  the lookup's result, nothing). The port mapper itself is tested in njproto,
  against a fake router.
- `jamsa_e2e_test`: two engine wrappers in one process, one hosting a room
  (on a free port, without the router or the lookup), the other connecting to
  it; a 997 Hz tone through `process()` with a
  simulated clock, checked for an SNR of at least 35 dB on Opus and 60 dB
  after `codec flac`; the bitrate command reaching the room, transmit off
  giving silence, disconnecting and closing the room.

## Deferred

Not in this version; `ninjam/vst3` has them on its own stack:

- Ableton Link (leading or following Link from the room's tempo and beats).
  The `follow` setting and the bar sync with the host are here; Link needs
  the Link SDK and the calibration of when the blocks are heard.
- Live mode and CLAP.

## Licence

The plug-in's own sources (`jamsa/`) are by the Jamsa contributors. JUCE is
available under the AGPLv3 or a commercial licence from JUCE; the plug-in is
built without `JUCE_DISPLAY_SPLASH_SCREEN=0`, as the free licence requires.
Opus, FLAC, miniupnpc and libnatpmp are BSD licensed; njproto is this
repository's. Under which
licence Jamsa itself is released is decided at release, by Leo.
