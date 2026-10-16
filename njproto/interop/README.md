# Comparing njproto with the reference NINJAM code

njproto was written from `PROTOCOL.md` alone. These are the checks that were
run against the reference server (`ninjam/server`) and client (`ninjam/njclient.cpp`)
on 2026-10-16, so that anyone can repeat them. They read the reference code's
*behaviour* only; the person running them is not bound by the clean-room rule,
the person writing njproto is.

## 1. The reference server's own test suite against njserver

`njserver_cfg.py` starts `njserver` from a ninjamsrv-style config file, which is
what the suite expects.

    cmake -S njproto -B build-njproto && cmake --build build-njproto
    cmake -S ninjam/server -B build-server -DNINJAM_PORTMAP=OFF && cmake --build build-server
    cd ninjam/server/tests
    NJSERVER=../../../build-njproto/njserver NINJAMSRV=../../../njproto/interop/njserver_cfg.py \
      python3 -m unittest -v test_server

Result: all 28 protocol tests pass. The 10 that fail test ninjamsrv features
outside the protocol: router port mapping (6), the session archive on disk (1),
dropping root after binding a low port (1), the exit code when the port is taken (1),
and one skipped-on-purpose feature set. Against ninjamsrv itself the suite gives
32 passed, 6 skipped.

## 2. Reference clients with real audio through njserver

`ninjam/tests/njclient_e2e` runs two reference clients in one process: one plays
a 997 Hz tone, the other listens, and it prints the tone's SNR after the trip
through the encoder, the server and the decoder.

    cd ninjam/tests && make njclient_e2e njclient_e2e_vorbis
    build-njproto/njserver 20491 --anon --allow-codecs opus,flac --bpm 120 --bpi 4 --quiet &
    ninjam/tests/njclient_e2e 127.0.0.1:20491 12 48000          # Opus
    ninjam/tests/njclient_e2e_vorbis 127.0.0.1:20491 12 48000   # Vorbis

Result: Opus 42.0 dB SNR, Vorbis 29.1 dB, the same figures as through ninjamsrv.

## 3. njcli against ninjamsrv

    build-server/ninjamsrv some.cfg &       # AnonymousUsers yes, AllowCodecs opus flac
    njcli 127.0.0.1 2049 anonymous:listen --codecs opus,flac --subscribe-all --dump /tmp/rx --seconds 8 &
    njcli 127.0.0.1 2049 anonymous:talker --codecs opus,flac --channel guitar \
          --upload take.opus --bpm-interval --say "!bitrate 96" --say "!codec" --seconds 5

Result: login, channel list, JOIN/PART, chat, codec mask (128 then 96 kbps), the
`[server]` replies and every uploaded interval arrive at the listener byte for byte.

## What the comparison changed in the spec

Revision 1 of the spec was enough for audio, login, channels, subscriptions and
forwarding to work at once. Everything found afterwards was text or a default the
spec had left open; see section 14 of `PROTOCOL.md`.
