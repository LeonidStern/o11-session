# Engine notes

**Ambiguous sentences in ENGINE.md (revision 1):** 13.3 "digital silence of 2 s encodes to under 200 bytes" (not reachable with libFLAC's mandatory VORBIS_COMMENT block and 4096-sample subset blocks: about 400 bytes); 6.4 "any other value is scaled by 32768, gets TPDF dither of 1 LSB" together with 13.3 "a signal already quantised to 16 bits decodes bit-exactly" (dither would move exact values by up to 1 LSB); 13.10 "the tone's peak within 0.45..0.55" together with 7.1 "volume (default **0.25**)" (the listener must raise the channel volume to 1.0 to see that peak); 13.13 "plays the newest intervals, not the stale ones" together with 7.3 "a new entry goes into slot 0 if empty, else into slot 1, replacing whatever was there" (after a stall slot 0 holds the first interval that arrived during it, so one stale interval plays before the newest); 13.2 "the decoded 48 kHz stream has 96000 ± 2 frames" together with 6.4 "the stream of one interval begins exactly where the previous ended" (a frame count that varies by ±1 between intervals moves the join by a sample; this engine makes the count exact); 8.1 "`read(n)` / `skip(n)`" (the decoder here exposes `data()` + `skip(n)` instead of a copying `read`); 4.4 "a boundary falls exactly when `intervalPos == intervalLength`" (taken as: the boundary is processed when the next sample is about to be produced, so after a block that ends exactly at the boundary `position()` still reports `pos == len` and `loopCount()` is unchanged, as 13.6 reads); 8.5 "`phase` is the resampler's fractional position carried from the last piece" (not said whether it survives an interval boundary; reset here).

Clean-room implementation of `ENGINE.md` revision 1 on top of the njproto
client. The only inputs were `PROTOCOL.md`, `ENGINE.md`, the njproto sources in
this folder and the public headers of libopus, libogg and libFLAC. Wherever the
specification leaves room, the choice is listed here.

## Layout

- `include/njproto/engine.h`, `engine/src/engine.cpp`: `Engine` (sections 3-11).
- `include/njproto/engine/codec.h`, `engine/src/dsp.cpp`: float conversion,
  LPC extrapolation, the windowed-sinc resampler, the shared `Encoder`,
  `Decoder` and `ByteStream` base classes.
- `engine/src/opus_codec.cpp`, `engine/src/flac_codec.cpp`: the two encoders
  and decoders (6.4, 8.1) and `makeEncoder`/`makeDecoder`.
- `engine/tests/test_engine_codecs.cpp` (13.1-13.4 plus a resampler/LPC
  check), `engine/tests/test_engine_clock.cpp` (13.5-13.9),
  `engine/tests/test_engine_e2e.cpp` (13.10-13.13, two engines against the
  in-process `Server` with a simulated audio clock).
- `tools/njjam.cpp`: the command-line client of section 14.
- CMake option `NJPROTO_BUILD_ENGINE` (default ON) finds opus, ogg and flac
  with pkg-config; OFF builds only the protocol library.

## Threads and contexts (section 2)

- `process()` holds `remote_mutex_` and `local_mutex_` for the whole block.
  The control context takes them only to swap pointers or copy settings: user
  list edits, queueing a decoded interval, reading settings. Decoders are
  created and primed, and encoders run, *outside* these mutexes. Decoding
  itself (pulling bytes from the download buffer and running libopus/libFLAC)
  happens in the audio context under `remote_mutex_`; the download buffer has
  its own short mutex (`ByteStream`).
- Each local channel's upload queue has its own mutex; the audio context
  pushes blocks and reuses buffers returned by the control context (`spare`),
  so steady-state capture does not allocate. The decoders' output buffers and
  the resamplers' histories are `std::vector`s that grow to a working size
  and stay there, so steady-state playback does not allocate either. A
  strictly allocation-free audio path (fixed ring buffers) was not attempted.
- Decoders that `process()` drops go to a garbage list freed in `run()`
  (`collectGarbage`), so the audio context never runs codec destructors.
- Queries (`position`, `bpm`, peaks, status...) are atomics; strings
  (`username`, `errorText`, `hostName`) use a small mutex.

## Connection (section 3)

- `connect(hostport)` splits at the last `:`; no port means 2049. The client
  options ask for Opus and FLAC, accept server versions below 0x00030000 and
  send the login without the licence bit when the licence callback refuses
  (the server's refusal then ends the connection: status `InvalidAuth` with
  the AUTH_REPLY text). `Defer` keeps the connection alive with keepalives
  until `answerLicence()`.
- Status mapping on a closed socket: `CantConnect` before the challenge,
  `InvalidAuth` after the challenge and before a successful AUTH_REPLY,
  `Disconnected` after login. `errorText()` is the AUTH_REPLY text or the
  client's own reason ("server is incorrect protocol version", "connection
  closed", ...).
- The channel list is sent from inside the AUTH_REPLY callback (the njproto
  client sends `setChannels()` right after it), before the status turns `Ok`.
- `run()` returns true when the socket produced no message and nothing was
  uploaded in this step.
- `disconnect()` suppresses the disconnect callback so the status is
  `Preconnect`, not `Disconnected`.
- `SESSION` chat messages (protocol 9.3) are not passed to `onChat`.

## Clock (section 4)

- `intervalLengthFor(bpm, bpi, sr) = (int)(bpi / (bpm / 60.0) * sr)`.
- The first boundary is at the start of the first `process()` after the
  first CONFIG_CHANGE. Before that `bpm()`/`bpi()` report the defaults
  120/32 and `usualIntervalLength()` 0.
- A block that ends exactly on a boundary leaves `position()` at `pos ==
  len`; the boundary is processed at the start of the next piece (test 13.6
  expects `loopCount()` to advance only when a sample of the next interval is
  produced).
- `beat_len = len / bpi` with `len` the usual length at the boundary; the
  metronome restarts its beat counter at every boundary and ignores
  `setIntervalEnd`.

## Local channels and upload (sections 5, 6)

- `source >= numIn` and missing second channels are silence; `maxchan` from
  AUTH_REPLY limits what is announced, captured and encoded; channels above it
  keep their settings and metering.
- Codec per interval: FLAC if the mask has it and `preferFlac`, else Opus,
  else none (the interval is dropped silently, `uploadCodec()` is 0). With no
  mask received the engine sends nothing.
- Encoder bitrate: the room's `opus_kbps` when nonzero, else the channel's
  `bitrate`; FLAC ignores it. The encoder is replaced when codec, rate,
  channels or bitrate change at the start of an interval.
- BEGIN is sent together with the first WRITE once 8192 bytes are encoded (or
  at the end, whatever is there); later WRITEs at 2048 bytes; WRITEs never
  exceed 9216 bytes; when the final flush leaves nothing an empty WRITE with
  the end flag closes the interval. The silence marker is one zero-GUID BEGIN
  queued at the boundary where transmit went off (after the End of the
  interval it closes).
- The upload queue holds 512 blocks per channel; a full queue drops blocks.

## Opus (6.4, 8.1)

- `pre_skip` is `OPUS_GET_LOOKAHEAD` after the encoder is created. Each
  interval: `OPUS_RESET_STATE`, a new Ogg stream with a random serial,
  OpusHead (input rate, gain 0, family 0) and OpusTags ("njproto engine", no
  comments) each flushed as their own page.
- Resampler: windowed sinc, 32 taps each side, Blackman window, 512 phases
  with linear interpolation between phases, cutoff scaled when downsampling.
  Its state persists across intervals of one encoder. It is primed with 64
  zero frames and starts 32 in, so the output lags the input by a constant 32
  input frames and every interval of N input frames yields exactly
  `floor(N * 48000 / rate)` frames (positions are kept as integers, no drift);
  the stream of one interval begins exactly where the previous ended.
- Padding: `real + pre_skip` rounded up to a multiple of 960, minus `real`,
  from a Levinson-Durbin fit of order 24 on the last 1024 resampled samples
  per channel (Gaussian lag window of about 20 Hz, bandwidth expansion
  0.998^k, white-noise correction 1e-5), 128 samples at full level then a
  raised-cosine fade to 0. The last packet's granule is `real + pre_skip`
  with e_o_s.
- Decoder: output gain applied as 10^(gain/(20*256)); samples at index >=
  the e_o_s page's granule (minus pre_skip) are the lapping frames; family
  != 0 or more than two channels decode to nothing.

## FLAC (6.4, 8.1)

- 16-bit, level 5, verify off, `streamable_subset` when
  `FLAC__format_sample_rate_is_subset(rate)`, total-samples estimate 0.
  libFLAC always writes a VORBIS_COMMENT block; 2 s of silence at 44.1 kHz is
  about 400 bytes, not "under 200" (test 13.3 checks < 512).
- Float to 16-bit: 0 and NaN give 0; a value that already sits exactly on a
  16-bit step is kept as it is (no dither), so quantised audio round-trips
  bit-exactly (13.3); any other value gets 1 LSB TPDF dither, rounding to
  nearest, clipping. The dither generator is a per-encoder `std::mt19937`.
- Decoder: the read callback returns ABORT when starved; the decoder is then
  `flush()`ed and the input rewound to the last `get_decode_position` (a tell
  callback is supplied), so a truncated frame is retried from its start once
  more data arrives. Headers need 42 bytes before the first attempt. Frames
  whose sample number is ahead of the expected one are preceded by silence
  (at most 5 s). More than two channels: the first two are used.

## Remote users and playback (sections 7, 8)

- The channel's default volume is 0.25 as specified; `njjam` and the e2e
  tests set remote channels to 1.0, otherwise the 13.10 peak of 0.45..0.55
  cannot be observed.
- The interval queue follows 7.3 literally: slot 0 if free, else slot 1
  replaced. After a stall, slot 0 holds the first interval that arrived
  during the stall and slot 1 the newest; test 13.13 therefore checks that
  the newest plays within two intervals after resuming and that the next one
  after that is a fresh interval (nothing in between is drained).
- Download timeout 8 s, checked on every `run()`; a playable download that
  times out has its stream marked ended and plays what arrived.
- Playback resampling is linear interpolation with the fractional position
  kept as a rational number (`phase_num / dstRate`), so an interval whose
  length maps to a whole number of source frames consumes exactly that many.
  The phase is reset at each boundary (the new stream starts at its frame 0).
- A late or short stream: skip what is available, add the shortfall to the
  drop count, output nothing for the piece; the drop count is reset at the
  boundary.
- The fade-out is the previous decoder's lapping frames followed by what it
  still has available, up to 128 frames; for FLAC that is empty when the
  stream was exactly the interval long. The fade-in is applied in place to
  the new decoder's first frames after priming it.
- Output pairs follow 8.4: `idx = (outChannel + offset) & 1023`, mono when
  bit 1024 is set or `numOut < 2`, a pair that does not fit is moved down.
  Automatic assignment (mode 1 per channel, mode 2 per user) picks among the
  even pairs from 2 up the first unused, else the least used (present
  channel 16, remembered one 1); nothing is assigned with `numOut < 4`.
- Peak meters decay by `0.015625^(len/sr)` per piece; remote meters measure
  after volume and pan.

## njjam

- `--user` is passed verbatim; on an anonymous server use `anonymous:NAME`.
  `--two` logs the listener in as `NAME_l`.
- Extra options: `--password`, `--amp` (default 0.5), `--say TEXT` (sent once
  after login; `--say "!codec flac"` switches a room to FLAC), `--quiet`.
- The sender's own channel is muted for monitoring so the recording of a
  `--two` run holds only what the listener hears. SNR is a least-squares fit
  of a sinusoid at the tone's frequency over the last two intervals.
- `--wav` reads 16-bit PCM or 32-bit float WAV (mono, or the first channel)
  and resamples it linearly to `--srate`.

## Not implemented

- Recording received intervals to a work directory by GUID (protocol 7.5
  mentions it; ENGINE.md does not ask for it).
- Vorbis (as in ENGINE.md: a room without a mask gets nothing from this
  engine).
- A strictly allocation-free audio path; see "Threads and contexts".
