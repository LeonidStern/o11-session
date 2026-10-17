# O11 Session

The software side of O11 Session, a networked jam box, and of Jamsa, its
plug-in for playing together over the internet. Everything here is written
from specifications, without the GPL code of NINJAM, so Leonid Stern owns it
and decides its licence (`LICENSE` is a placeholder until then).

- `njproto/`: the clean-room NINJAM stack in C++17. `PROTOCOL.md` describes the
  wire protocol, `ENGINE.md` the client audio engine (Opus and FLAC, raw Opus
  interval framing), `PORTMAP.md` the router port mapping. The library, the
  `njserver` and `njcli` tools, the engine and the tests are written from those
  documents alone. See `njproto/README.md`.
- `jamsa/`: the Jamsa VST3 plug-in on JUCE 8 over `njproto`: the room, the
  mixer, the click, the hosted server with router port mapping and the public
  address lookup. See `jamsa/README.md`.
- `.github/workflows/jamsa.yml`: builds and tests both on Linux, Windows and
  macOS, runs pluginval, and signs and notarizes the macOS build when the
  repository has the Developer ID secrets (`MACOS_CERT_P12`,
  `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD`).

## Rule for changes

`njproto/` is changed only from its specifications, by someone who has not
read NINJAM's GPL sources (`ninjam/njclient*`, `ninjam/mpb*`, `ninjam/netmsg*`,
`ninjam/audiostream*`, `ninjam/server`, `WDL`, `jmde` in the older
`LeonidStern/Jamsa` repository). When the behaviour must change, change the
specification first, then the code. `jamsa/` follows the same rule; it may
reuse the project's own plug-in code from that repository (`ninjam/vst3`).

## Building

    cmake -S jamsa -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJAMSA_TESTS=ON
    cmake --build build
    ctest --test-dir build --output-on-failure

That builds `njproto` too. For `njproto` alone, `cmake -S njproto -B
build-njproto`; it needs libopus and libFLAC (pkg-config), and miniupnpc and
libnatpmp for the port mapper.

The history of this repository is the history of these folders in
`LeonidStern/Jamsa` (pull request 10), carried over on 2026-10-17.
