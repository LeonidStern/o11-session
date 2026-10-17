# njproto

A clean-room C++17 implementation of the NINJAM network protocol, written from
`PROTOCOL.md` alone. It contains:

- a small library (`include/njproto/`, `src/`): message framing and codecs,
  SHA-1 and the password hash, a non-blocking client state machine and a
  single-threaded server;
- `njserver`, a command-line server;
- `njcli`, a command-line client that logs every message it receives as JSON,
  for interoperability tests against other NINJAM software;
- unit tests and loopback client-against-server tests.

Audio encoding and decoding are out of scope: the client takes opaque encoded
bytes per interval and hands received bytes to the caller. The server never
looks inside audio data. Dependencies: the C++17 standard library and POSIX
sockets only.

## Build

    cmake -S njproto -B build-njproto
    cmake --build build-njproto
    cd build-njproto && ctest

Builds with g++ on Linux at `-Wall -Wextra` without warnings. The loopback test
takes about ten seconds because it waits for real keepalive and transfer timeouts.

The client engine (`njproto_engine`, `njjam`, the `test_engine_*` tests) needs
libopus and libFLAC, found with pkg-config. Pass
`-DNJPROTO_BUILD_ENGINE=OFF` to build only the protocol library and its tools.

## njserver

    njserver <port> [--anon] [--anon-multi] [--user name:pass:privs]...
             [--bpm N] [--bpi N] [--allow-codecs opus,flac|none]
             [--require-codec opus|flac] [--opus-kbps N|none]
             [--keepalive N] [--max-users N] [--max-pending TOTAL PER_IP]
             [--max-channels N] [--topic TEXT] [--motd LINE]... [--licence FILE]
             [--mask-ip] [--vote-threshold PCT] [--login-timeout N] [--quiet]

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

    njserver 2049 --anon --user admin:secret:tcbkv --opus-kbps 128 --topic "jam"

## njcli

    njcli <host> <port> <user> [pass] [--codecs opus,flac] [--channel NAME]...
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

    njserver 2049 --anon --quiet &
    njcli 127.0.0.1 2049 anonymous:listener --subscribe-all --dump /tmp/rx --seconds 6 &
    njcli 127.0.0.1 2049 anonymous:player --upload take.ogg --say "hi" --seconds 2

## Library overview

- `njproto/message.h`: constants, `Reader`/`Writer`, `parseFrame`/`appendFrame`,
  one struct per message and `encode()`/`decode()` for each. `decode` returns
  false on malformed payloads instead of crashing.
- `njproto/sha1.h`: `Sha1`, `sha1()`, `passwordHash(user, pass, challenge)`.
- `njproto/connection.h`: one non-blocking framed TCP connection with a bounded
  send queue and the keepalive rule; used by both sides.
- `njproto/client.h`: `Client` with `connect()`, `poll()`, `waitFor()`,
  `setChannels()`, `subscribe()`, `beginInterval()`/`writeInterval()`/
  `endInterval()`/`sendSilence()`, `say()`/`privmsg()`/`admin()` and callbacks
  for every incoming message. Set `Options::auto_subscribe` to hear everyone.
- `njproto/server.h`: `ServerConfig` and `Server` with `start()`, `poll()`,
  `stop()`. Drive `poll()` from your own loop or call `runFor()`.

## Client engine

`include/njproto/engine.h` (`njproto::engine::Engine`) implements `ENGINE.md`:
the interval clock, local channels captured in `process()` and encoded to Opus
or FLAC in `run()`, remote channels decoded and mixed with the interval queue,
crossfade, pan law, meters, metronome and chat. `include/njproto/engine/codec.h`
has the encoders, decoders, the windowed-sinc resampler and the DSP helpers on
their own. Call `process()` from the audio callback and `run()` from a control
loop at least every 50 ms.

## njjam

    njjam host:port --user NAME [--password P] [--tone HZ | --wav FILE] [--amp A]
          [--record out.wav] [--seconds N] [--srate N] [--block N] [--kbps N]
          [--no-flac] [--listen-only] [--toggle-transmit SECONDS] [--two]
          [--say TEXT]... [--quiet]

A command-line client on the engine with a simulated audio device running in
real time. It plays the tone (default 997 Hz, amplitude 0.5) or the WAV file
into local channel 0, subscribes to everyone, records what it hears and prints
`bpi=... interval_samples=... peak=... snr=...` measured over the last two
intervals. `--two` runs a sender and a listener (`NAME_l`) in one process and
reports the listener's figures; `--say "!codec flac"` switches the room to
FLAC. On an anonymous server use `--user anonymous:NAME`.

See `NOTES.md` for interpretation choices and what is not implemented, and
`engine/NOTES.md` for the engine.
