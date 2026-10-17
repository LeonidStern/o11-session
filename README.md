# O11 Session

**Jam with anyone, anywhere, right from your DAW.**

O11 Session is a plug-in that turns your DAW into a shared rehearsal room.
Drop it on a track, send a link to your bandmates, and play together over the
internet, in time, each of you in your own studio with your own sounds.

Inspired by [NINJAM](https://www.cockos.com/ninjam/) and
[JamTaba](https://jamtaba-music-web-site.appspot.com/), rebuilt from the
ground up to be lighter, cleaner and easier to use.

## Download

| | |
|---|---|
| **macOS** (Apple silicon and Intel, macOS 12 or later) | [O11Session-macos.dmg](https://github.com/LeonidStern/o11-session/releases/download/o11session-latest/O11Session-macos.dmg) |
| **Windows** (64-bit) | [O11Session-windows-x64.zip](https://github.com/LeonidStern/o11-session/releases/download/o11session-latest/O11Session-windows-x64.zip) |
| **Linux** (x86_64) | [O11Session-linux-x86_64.zip](https://github.com/LeonidStern/o11-session/releases/download/o11session-latest/O11Session-linux-x86_64.zip) |

These links always point to the newest build.

## Install

- **macOS:** open the disk image and drag O11 Session onto the VST3 folder.
  It is signed and notarized by Apple, so it opens without warnings.
- **Windows:** unzip and copy `O11Session.vst3` into
  `C:\Program Files\Common Files\VST3`.
- **Linux:** unzip and copy `O11Session.vst3` into `.vst3` in your home folder.

Then rescan plug-ins in your DAW. In Ableton Live that is Settings, Plug-Ins,
Rescan.

## How it works

Everyone hears everyone else one bar (or a few bars) late, perfectly in time.
While you play the next phrase, the last one arrives from the others, so the
music lines up on the beat no matter how far apart you are. It suits jams,
loops, grooves and songs built on a steady pulse.

1. **Start a room.** Put O11 Session on a track and press Host. It opens the
   room on your computer, asks your router to let people in, and shows the
   address to share.
2. **Invite your band.** They paste the address into their own copy and join.
3. **Play.** Each player arrives on their own output in your DAW, so you can
   mix, mute, solo, record and add effects to them like any other track.

## Features

- **Lives in your DAW.** A VST3 plug-in for Ableton Live, Bitwig, Reaper,
  Studio One, Cubase and other VST3 hosts on macOS, Windows and Linux.
- **Every player on their own track.** Up to eight other players each get a separate
  stereo output, ready to mix, record or process.
- **Studio sound.** Opus at up to 510 kbps for near-transparent audio, or
  lossless FLAC when the connection allows. The room switches with one vote.
- **Locked to your DAW's tempo.** The room follows your bars and tempo, and a
  tempo change in your DAW becomes a vote for the room.
- **Ableton Link.** Link-enabled DAWs such as Live and Bitwig follow the
  room's tempo automatically.
- **Host with one click.** No server to rent and nothing to configure. The
  room runs inside the plug-in, and your router is set up for you where it
  allows it.
- **Room chat and votes.** Talk, set the tempo and bar length, and choose the
  sound quality together.
- **Built-in metronome**, saved with your project, along with every other
  setting.
- **Small and fast.** One compact window, a light footprint, and a server that
  uses less than half the CPU of the classic one (see below).

## Benchmarks

Measured on 2026-10-16 and 2026-10-17, back to back on the same Linux
machine with the same inputs. The comparison is against NINJAM's reference
server and client, the classic setup most online jam tools are built on.

**Server load** (every player sending two channels at 128 kbps):

| Room | Classic server CPU | O11 Session CPU |
|---|---|---|
| 8 players | 1.6 to 1.7 % | **0.6 to 0.7 %** |
| 16 players | 4.1 to 4.8 % | **2.5 to 2.8 %** |

**Forwarding delay** inside the server:

| | Classic server | O11 Session |
|---|---|---|
| Typical | 0.40 ms | **0.19 ms** |
| Worst seen | 5 to 6 ms | **0.4 to 1.7 ms** |

**Audio quality** (a test tone sent through a real server and compared with
what the listener hears; higher is better):

| | Opus 510 kbps | FLAC |
|---|---|---|
| Classic client | 42.6 dB | 87.3 dB |
| O11 Session | **47.8 dB** | 87.3 dB |

**Client CPU** for a sender and a listener together: about 1.7 % of one core
at Opus 510 kbps, a little above the classic client's 1.4 %. Profiling shows
why: about 78 % of it is the Opus library at its highest quality setting,
which is also what gives the better sound above. O11 Session's own code is 5
to 8 %. Two parts of it cost more than they should have, and were tuned on
2026-10-17:

| | Before | After |
|---|---|---|
| Opus at 44.1 kHz, CPU of one core | 2.5 % | **2.2 %** |
| FLAC's 16-bit conversion, instructions per 20 s | 172 M | **59 M** |
| 44.1 kHz resampler, instructions per 20 s | 1041 M | **318 M** |

The sound is the same to 0.1 dB.

**Compatibility:** the classic server's own protocol test suite passes 28 of
28 cases against the O11 Session server.

Every change is built and tested automatically on macOS, Windows and Linux,
including Tracktion's pluginval at its strictest level and a full two-player
session through a hosted room.

## Coming next

- **Live mode:** real-time playing with no bar of delay, for players close
  to each other.
- **CLAP** support.
- **The O11 Session box:** a networked audio interface for jamming without a
  computer.

## For developers

Building from source needs CMake 3.25 or later, Ninja and a C++17 compiler:

    cmake -S plugin -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

The plug-in is in `plugin/` and the network and audio engine with its
specifications is in `o11proto/`.

## Licence

To be decided by Leonid Stern. O11 Session includes Ableton Link (GPLv2 or
later), so builds of the plug-in are GPL. It also uses JUCE (AGPLv3 or
commercial), Opus, FLAC, miniupnpc and libnatpmp.
