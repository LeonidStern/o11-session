# O11 Session

Play music together over the internet, in time, with people who are not in
the room. O11 Session is a jam platform in two parts: **Jamsa**, a VST3
plug-in that puts a NINJAM-style room inside your DAW, and **njproto**, the
protocol stack and server underneath it, written from scratch and owned by
this project. A hardware jam box on the same stack is the next step.

It is inspired by [NINJAM](https://www.cockos.com/ninjam/) and
[JamTaba](https://jamtaba-music-web-site.appspot.com/): the idea of playing in
sync by sharing whole bars of audio with a bar of delay comes from NINJAM, and
the idea that a plug-in in the DAW is the nicest place to jam from comes from
JamTaba. O11 Session takes those ideas and rebuilds them with a modern codec
path, a leaner server and an engine that is tested end to end on every change.

## Features

**Jamsa, the plug-in** (VST3; Linux, Windows and macOS, Intel and Apple
silicon, signed and notarized on macOS):

- Your track's input goes to the room; each other player comes back on their
  own stereo output pair ("Player 1" to "Player 8"), so you mix them in the
  DAW like any other track, with volume, pan, mute and solo per player.
- Opus (64 to 510 kbps) or lossless FLAC per room, switchable with a vote;
  Opus intervals use a raw packet framing with no Ogg container.
- Follow the DAW: the room's intervals start on the host's bars while it
  plays at the room's tempo, and a tempo change in the DAW becomes a vote.
- Host a room from the plug-in: a built-in server on the first free port from
  2049, with the router asked to forward it (UPnP-IGD, then NAT-PMP), a
  public-address lookup for ports forwarded by hand, and the address to send
  people shown in the host bar.
- Chat with `/msg`, `/topic`, `/bpm`, `/bpi`, `!bitrate` and `!codec`; votes
  for the room's tempo, interval and codec; the server's licence shown before
  you join; a metronome with the same click at 44.1, 48 and 96 kHz.
- One compact 720 x 330 window, resizable, with meters, beat cells and the
  host bar; the state is saved with the DAW project.

**njproto, the stack:**

- A C++17 library for the NINJAM wire protocol (`PROTOCOL.md`): framing,
  login, channels, subscriptions, interval upload and forwarding, chat and
  votes, codec negotiation; non-blocking client, single-threaded `poll()`
  server.
- `njserver`, a command-line server for hosted rooms (accounts, anonymous
  users, keepalive, limits, Opus and FLAC, voting), and `njcli`, a client
  that logs every message as JSON for interoperability tests.
- The client audio engine (`ENGINE.md`): Opus and FLAC encode and decode,
  resampling, the two-interval queue, mixing and the clock.
- Router port mapping (`PORTMAP.md`): UPnP-IGD then NAT-PMP, renewed before
  the lease ends, removed on stop, tested against a fake router.
- Everything builds on Linux, Windows (MSVC or MinGW) and macOS, with no
  dependencies beyond Opus, FLAC, miniupnpc and libnatpmp.

## Benchmarks

All numbers below were measured by this project, with the tools in this
repository, on 2026-10-16 and 2026-10-17. The machine was a Linux x86_64
container (the development sandbox), so absolute numbers say little; the
comparisons were run back to back on the same machine, with the same
inputs. The reference server and client are the ones in the older
[Jamsa repository](https://github.com/LeonidStern/Jamsa): NINJAM's `ninjamsrv`
and `NJClient`, with that repository's Opus and FLAC extensions.

### Server: njserver against ninjamsrv

`loadtest.py` (in the older repository, `ninjam/server/tests/`) simulates a
room: every player uploads two channels continuously at 128 kbps in 1 KB
writes with a new interval every 4 s, and subscribes to everyone else. CPU is
the server process's user plus system time over 15 s; "relayed" is what the
players received. Forwarding delay was measured by a second tool that
timestamps a write on one connection and its arrival on another.

| Room | ninjamsrv CPU | njserver CPU | relayed |
|---|---|---|---|
| 8 players, 2 channels, 128 kbps | 1.6 to 1.7 % | 0.6 to 0.7 % | 1.82 MB/s both |
| 16 players, 2 channels, 128 kbps | 4.1 to 4.8 % | 2.5 to 2.8 % | 7.8 MB/s both |

| Forwarding delay (1 KB and 8 KB writes) | ninjamsrv | njserver |
|---|---|---|
| median | 0.40 ms | 0.19 ms |
| worst seen | 5 to 6 ms | 0.4 to 1.7 ms |

Compatibility: the reference server's own protocol test suite passes 28 of 28
cases against `njserver`, and `njcli` logged in, chatted, voted and received
every interval byte for byte from `ninjamsrv`.

### Engine: audio quality and CPU

A 997 Hz tone at -6 dBFS is sent through a real server and compared with what
the listener plays, after one interval of delay. SNR is signal to the
difference, so it measures the codec and the clock together.

| Path | Opus (510 kbps) | FLAC |
|---|---|---|
| njproto engine to njproto engine, through `ninjamsrv` | 47.8 dB | 87.3 dB |
| njproto engine to njproto engine, in the plug-in's end-to-end test (CI, three platforms) | 46.8 to 47.6 dB | 87.3 dB |
| Reference client to reference client (for scale) | 42.6 dB | 87.3 dB |

CPU for 20 s of real-time audio, one sender and one listener in one process,
48 kHz:

| | Opus | FLAC |
|---|---|---|
| njproto engine (both sides) | 0.34 s | 0.18 to 0.20 s |
| reference NJClient pair | 0.28 to 0.29 s | 0.15 to 0.16 s |

The engine spends a little more than the reference: about 1.7 % of one core
for a sender and a listener together at Opus 510 kbps. It has not been tuned
for speed yet.

### Plug-in checks

- pluginval at strictness level 10 passes on Linux, Windows and macOS on
  every push.
- Unit test: about 4,070 checks (state, commands, bar sync against a
  simulated host over 20 minutes with at most 1.1 ms of error, the click, the
  host bar, the port mapper's messages, the address lookup). End-to-end test:
  40 checks, two plug-in engines in one process through a hosted room.
- The port mapper: 7 scenarios against a fake UPnP and NAT-PMP router,
  including a 718 refusal falling back to NAT-PMP and a 10 s lease renewed
  after 5 s.

## Getting it

Builds of the plug-in for Linux, Windows and macOS are attached to every
run of the `jamsa` workflow under Actions (`Jamsa-juce-*`). Releases will
follow once the plug-in has been tested in Ableton Live, Reaper and others.

## Building

Needs CMake 3.25 or later, Ninja and a C++17 compiler. JUCE, Opus, FLAC,
miniupnpc and libnatpmp are fetched by CMake.

    cmake -S jamsa -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAMSA_TESTS=ON
    cmake --build build
    ctest --test-dir build --output-on-failure

That builds `njproto` too; for the stack alone, `cmake -S njproto -B
build-njproto` (it needs libopus, libFLAC, miniupnpc and libnatpmp through
pkg-config). See `jamsa/README.md` and `njproto/README.md` for the details,
the platform notes and the text commands.

## How it is written

Everything here is written from specifications, not from NINJAM's code, so
this project owns it and chooses its licence (`LICENSE` is a placeholder
until then). `njproto/PROTOCOL.md`, `ENGINE.md` and `PORTMAP.md` are the
specifications; the code is written from them by people who have not read
NINJAM's GPL sources. When the behaviour must change, the specification
changes first. `jamsa/` follows the same rule and reuses only this project's
own plug-in code from the older repository.

The repository's history is the history of these folders in
`LeonidStern/Jamsa` (pull request 10), carried over on 2026-10-17.

## Roadmap

- Ableton Link in the plug-in.
- Live mode: a low-latency path for players close to each other.
- CLAP, and a signed Windows build.
- The O11 Session box: a two-input networked audio interface on this stack.

## Licence

To be decided by Leonid Stern. JUCE is used under its licence terms (AGPLv3
or commercial); Opus, FLAC, miniupnpc and libnatpmp are BSD licensed.
