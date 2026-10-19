# o11proto

A clean-room C++17 implementation of the NINJAM network protocol, written from
`PROTOCOL.md` alone. It contains:

- a small library (`include/o11proto/`, `src/`): message framing and codecs,
  SHA-1 and the password hash, a non-blocking client state machine and a
  single-threaded server;
- `o11server`, a command-line server;
- `o11cli`, a command-line client that logs every message it receives as JSON,
  for interoperability tests against other NINJAM software;
- unit tests and loopback client-against-server tests.

Audio encoding and decoding are out of scope: the client takes opaque encoded
bytes per interval and hands received bytes to the caller. The server never
looks inside audio data. Dependencies: the C++17 standard library and the
platform's sockets only (POSIX, or Winsock2 on Windows).

## Build

    cmake -S o11proto -B build-o11proto
    cmake --build build-o11proto
    cd build-o11proto && ctest

Builds with g++ on Linux at `-Wall -Wextra` without warnings, and on Windows
with MSVC or MinGW (the socket layer lives in `src/socket_compat.h`; CMake links
`ws2_32` there). The loopback test takes about ten seconds because it waits for
real keepalive and transfer timeouts.

The client engine (`o11proto_engine`, `o11jam`, the `test_engine_*` tests) needs
libopus and libFLAC, found with pkg-config. Pass
`-DO11PROTO_BUILD_ENGINE=OFF` to build only the protocol library and its tools.

## o11server

    o11server <port> [--anon] [--anon-multi] [--user name:pass:privs]...
             [--bpm N] [--bpi N] [--allow-codecs opus,flac|none]
             [--require-codec opus|flac] [--opus-kbps N|none]
             [--keepalive N] [--max-users N] [--max-pending TOTAL PER_IP]
             [--max-channels N] [--topic TEXT] [--motd LINE]... [--licence FILE]
             [--mask-ip] [--vote-threshold PCT] [--login-timeout N] [--port-map]
             [--live no|yes|on|require] [--live-kbps N] [--live-frame N]
             [--live-max-peers N] [--live-port N] [--live-strict-address yes|no] [--quiet]

Defaults: Opus relayed besides Vorbis, room Opus bitrate 128 kbps (a value
outside 32-510 is ignored with a warning; `none` means clients send at their
own bitrate), 16 users, 64 pending connections / 8 per address, 3 s keepalive.
`--require-codec` allows that codec by itself. `--anon-multi` lets anonymous
users log in several times with the same name (`.2`, `.3`, ...); without it a
second login with the same name replaces the first.

Privilege letters for `--user`:

| Letter | Privilege |
|---|---|
| `t` | set the topic (`/topic`) |
| `c` | chat (`MSG`, `PRIVMSG`) |
| `b` | set the tempo (`/bpm`, `/bpi`) |
| `k` | kick (`/kick`) |
| `r` | reserved slot: may log in when the server is full |
| `m` | multiple logins with the same name (`.2`, `.3`, ... appended) |
| `h` | hidden: not shown to others and not counted |
| `v` | vote (`!vote`, `!codec`, `!bitrate`) |
| `p` | see private rooms (lobby mode, not implemented) |
| `s` | status account: polled for the user list, never logged in |
| `*` | all of `t c b k r m v p` |

Anonymous users (`--anon`) get `c` and `v`, plus `m` with `--anon-multi`. Example:

    o11server 2049 --anon --user admin:secret:tcbkv --opus-kbps 128 --topic "jam"

`--port-map` asks the home router to forward the listening port to this
machine (see below); the mapper's lines appear in the server log and the
mapping is removed when the server exits. With Live mode open it forwards the
UDP port of the same number too.

`--live` turns on Live mode (PROTOCOL.md section 15): players who are on Live
hear each other in real time over UDP, directly between each pair of players
where they can (all to all, 15.10) and through the server's relay on the TCP
port's number for a player without a direct path. `yes` opens the UDP port
with the room's Live switch off, `on` with it on, and `require` is `yes` that
refuses to start when the UDP port can't be opened. Players switch it with
`!live on` / `!live off` in chat; while it is on the room is Live for
everyone, with no interval audio, and the server tells the players who has
trouble (15.11). A pair of players with no direct path can't hear each other,
and the advice says so. The room-wide relay fallback (everyone through the
relay once a pair has no direct path, until `!live direct`) is a backend
option, off by default and without a flag: `O11_LIVE_RELAY_FALLBACK=1`.
`--live-kbps`,
`--live-frame` and `--live-max-peers` set what the server tells the clients;
`--live-port` announces another UDP port (a router that forwards a different
external port); `--live-strict-address no` accepts UDP from any address of a
player who knows the key (for players whose UDP leaves through another
address than their TCP). The server's thread relays every Live datagram as it
comes, so it is a network thread (`o11proto/realtime.h`, real-time on macOS);
the server logs `[o11server] the server's thread: real-time (time-constraint)`
(or `latency-critical`, `its scheduling unchanged`) at start.

## Router port mapping

`include/o11proto/portmap.h` (`o11proto::PortMapper`, library `o11proto_portmap`)
implements `PORTMAP.md`: it maps one TCP port through UPnP-IGD (miniupnpc)
first and NAT-PMP (libnatpmp) second, on its own thread, renews the mapping at
half the granted lease, retries every five minutes when no router answers and
removes the mapping on `stop()`. Every event is one log line starting with
`[portmap] `; a line with `people can connect to <addr>:<port>` means success.
`O11_UPNP_ROOTDESC` (a root description URL) skips the multicast discovery
and `O11_NATPMP_GATEWAY` overrides the default route; the test
`test_portmap` uses both against the fake router in
`portmap/tests/fake_router.py` (Python 3, standard library only). The CMake
option `O11PROTO_BUILD_PORTMAP` defaults to ON when both libraries are found
(miniupnpc through pkg-config, libnatpmp by header and library name). On
Windows, link a static miniupnpc with `MINIUPNP_STATICLIB` defined and a
static libnatpmp with `NATPMP_STATICLIB`. See `portmap/NOTES.md`.

## o11cli

    o11cli <host> <port> <user> [pass] [--codecs opus,flac] [--channel NAME]...
          [--upload FILE [--bpm-interval] [--fourcc XXXX]] [--subscribe-all]
          [--say TEXT]... [--privmsg USER TEXT] [--admin CMD] [--dump DIR]
          [--seconds N] [--json] [--no-keepalive] [--version HEX] [--refuse-licence]

Log in as `anonymous` or `anonymous:name` for an anonymous login. Every message
received is printed as one JSON object per line on stdout (`--json` is the
default and accepted for clarity). Types: `auth_challenge`, `auth_reply`,
`config`, `userinfo`, `download_begin` (GUID, user, channel, fourcc, silence
flag), `download_end` (GUID, user, channel, byte count, `ended` = the end flag
was seen rather than a timeout), `chat` (command and fields), `codec_mask`,
`keepalive`, `unknown`, `upload`, `disconnect`, `exit`. The process exits after
`--seconds` (default 5) with status 0 when the login succeeded, 1 otherwise.

`--upload FILE` sends the file's bytes as one interval on channel 0 right after
login; with `--bpm-interval` it repeats the upload at every interval boundary
(60 x BPI / BPM seconds) as a fake "always playing" client. The fourcc comes from
the extension (`.opus`, `.opur`, `.flac`, else `OGGv`) or `--fourcc`. `--dump DIR` saves
every received interval as `DIR/<guid>.<ext>`, creating `DIR` if needed.

Example session:

    o11server 2049 --anon --quiet &
    o11cli 127.0.0.1 2049 anonymous:listener --subscribe-all --dump /tmp/rx --seconds 6 &
    o11cli 127.0.0.1 2049 anonymous:player --upload take.ogg --say "hi" --seconds 2

## Library overview

- `o11proto/message.h`: constants, `Reader`/`Writer`, `parseFrame`/`appendFrame`,
  one struct per message and `encode()`/`decode()` for each. `decode` returns
  false on malformed payloads instead of crashing.
- `o11proto/sha1.h`: `Sha1`, `sha1()`, `passwordHash(user, pass, challenge)`.
- `o11proto/connection.h`: one non-blocking framed TCP connection with a bounded
  send queue and the keepalive rule; used by both sides.
- `o11proto/client.h`: `Client` with `connect()`, `poll()`, `waitFor()`,
  `setChannels()`, `subscribe()`, `beginInterval()`/`writeInterval()`/
  `endInterval()`/`sendSilence()`, `say()`/`privmsg()`/`admin()` and callbacks
  for every incoming message. Set `Options::auto_subscribe` to hear everyone.
- `o11proto/server.h`: `ServerConfig` and `Server` with `start()`, `poll()`,
  `stop()`. Drive `poll()` from your own loop or call `runFor()`.
  `ServerConfig::live` and `setLiveConfig()` run Live mode's UDP relay
  (PROTOCOL.md section 15) inside the same loop.
- `o11proto/live.h`: Live mode's messages `0x07` (`live::ServerLive`),
  `0x09`, `0x86` and `0x87` (the direct paths: `LivePaths`, `LiveLan`,
  `PathsReport`, `pairKey()`), each with `encode()`/`decode()`, the datagram
  and path constants and the hello cookie. With `Client::Options::live` the
  client sets caps bit 16 (and with `live_direct` bit 17) and hands each
  `0x07` to `on_live`.
- `o11proto/live_room.h`: the room's one mode: `0x85` (`LiveReport`), `0x08`
  (`LiveRoom`) and the server's `RoomAdvice`.
- `o11proto/live_transport.h`: `live::Transport`, the client's Live UDP: one
  socket for the relay and the direct paths (not connected), hellos and the
  cookie, the probes and the paths' states, where each payload goes, the
  client's drop rules, and opaque payloads in (`on_data`) and out (`send()`),
  all on its own network thread, which `wake()` wakes. The engine's Live
  layer (ENGINE.md section 16) runs on it.
- `o11proto/realtime.h`: a thread's scheduling, where the system lets a
  program choose it: `thread::makeAudioThread()`, `makeLatencyCritical()`,
  `makeNetworkThread()` and `describe()` (ENGINE.md 16.1).

## Client engine

`include/o11proto/engine.h` (`o11proto::engine::Engine`) implements `ENGINE.md`:
the interval clock, local channels captured in `process()` and encoded to Opus
or FLAC in `run()`, remote channels decoded and mixed with the interval queue,
crossfade, pan law, meters, metronome and chat. `include/o11proto/engine/codec.h`
has the encoders, decoders, the windowed-sinc resampler and the DSP helpers on
their own. Call `process()` from the audio callback and `run()` from a control
loop at least every 50 ms. On a server with Live mode the engine also plays the
room's Live players in real time while the room is Live (ENGINE.md section
16): `onLiveNotice` gets its chat lines, `liveCommand()` answers `/live` and
`/live buffer auto|N`, `setLiveHostState()` gives it the host's transport, and
`liveRoomLive()`, the `liveAdvice…()` and `liveUser…()` calls give a UI the
room's mode, the server's advice and each player's delay, buffer and
counters. The jitter buffer is automatic (10 ms, 20 ms for a player whose
audio comes late) unless `setLiveBufferMs()` fixes it. `/live` shows the
room's mode and the advice, the jitter buffer and the round trip to the
server, then a line per player: live or not heard, about how late, the loss
and the dropouts.

## o11jam

    o11jam host:port --user NAME [--password P] [--tone HZ | --wav FILE] [--amp A]
          [--record out.wav] [--seconds N] [--srate N] [--block N] [--kbps N]
          [--no-flac] [--listen-only] [--toggle-transmit SECONDS] [--two]
          [--say TEXT]... [--no-live] [--quiet]

A command-line client on the engine with a simulated audio device running in
real time. It plays the tone (default 997 Hz, amplitude 0.5) or the WAV file
into local channel 0, subscribes to everyone, records what it hears and prints
`bpi=... interval_samples=... peak=... snr=...` measured over the last two
intervals. `--two` runs a sender and a listener (`NAME_l`) in one process and
reports the listener's figures; `--say "!codec flac"` switches the room to
FLAC. On an anonymous server use `--user anonymous:NAME`. On a Live server the
listener hears the sender live while the room is Live (ENGINE.md section 16)
and the figures measure that; the engine's Live lines are printed as `[NAME]
live: ...`, `--say "/live"` or `--say "/live buffer 40"` goes to the engine
instead of the chat, and `--no-live` keeps both engines off Live (while the
room is Live they hear no one). Its host thread is an audio thread
(`thread::makeAudioThread()`). A machine that stalls the audio thread for tens
of milliseconds makes the listener's stream run dry and be concealed, which
shows in the SNR.

See `NOTES.md` for interpretation choices and what is not implemented,
`engine/NOTES.md` for the engine and `portmap/NOTES.md` for the port mapper.
