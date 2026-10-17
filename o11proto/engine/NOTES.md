# Engine notes

**Ambiguous sentences in ENGINE.md (revisions 1 and 2):** 13.3 "digital silence of 2 s encodes to under 200 bytes" (not reachable with libFLAC's mandatory VORBIS_COMMENT block and 4096-sample subset blocks: about 400 bytes); 6.4 "any other value is scaled by 32768, gets TPDF dither of 1 LSB" together with 13.3 "a signal already quantised to 16 bits decodes bit-exactly" (dither would move exact values by up to 1 LSB); 13.10 "the tone's peak within 0.45..0.55" together with 7.1 "volume (default **0.25**)" (the listener must raise the channel volume to 1.0 to see that peak); 13.13 "plays the newest intervals, not the stale ones" together with 7.3 "a new entry goes into slot 0 if empty, else into slot 1, replacing whatever was there" (after a stall slot 0 holds the first interval that arrived during it, so one stale interval plays before the newest); 13.2 "the decoded 48 kHz stream has 96000 ± 2 frames" together with 6.4 "the stream of one interval begins exactly where the previous ended" (a frame count that varies by ±1 between intervals moves the join by a sample; this engine makes the count exact); 8.1 "`read(n)` / `skip(n)`" (the decoder here exposes `data()` + `skip(n)` instead of a copying `read`); 4.4 "a boundary falls exactly when `intervalPos == intervalLength`" (taken as: the boundary is processed when the next sample is about to be produced, so after a block that ends exactly at the boundary `position()` still reports `pos == len` and `loopCount()` is unchanged, as 13.6 reads); 8.5 "`phase` is the resampler's fractional position carried from the last piece" (not said whether it survives an interval boundary; reset here); 8.1 (revision 2) "packets are decoded at 48000 Hz as each one's length and bytes have fully arrived" together with "on the trailer, samples past `real` are not output but kept as the lapping frames" (the padding is only known to be padding when the trailer arrives, after the last packet; the decoder here decodes each packet as it arrives but holds back the *output* of the most recent one until the next length prefix, the trailer or the end of the input is seen, which works because the padding minus the pre-skip is always shorter than one packet); 8.1 "a stream that stops without a trailer is played to its last whole packet with no trimming" (taken as: the pre-skip from the header is still dropped, only the trim at `real` is skipped, and there are no lapping frames); 8.1 "a packet ... that libopus refuses is bad and decodes to nothing from that point" (the packet held back before it was good and is still output, without lapping frames).

Clean-room implementation of `ENGINE.md` revision 2 (revision 1 plus the
`OPUr` raw Opus framing) on top of the o11proto client. The only inputs were
`PROTOCOL.md`, `ENGINE.md`, the o11proto sources in this folder and the public
headers of libopus and libFLAC (libogg is no longer used). Wherever the
specification leaves room, the choice is listed here.

## Layout

- `include/o11proto/engine.h`, `engine/src/engine.cpp`: `Engine` (sections 3-11).
- `include/o11proto/engine/codec.h`, `engine/src/dsp.cpp`: float conversion,
  LPC extrapolation, the windowed-sinc resampler, the shared `Encoder`,
  `Decoder` and `ByteStream` base classes.
- `engine/src/opus_codec.cpp`, `engine/src/flac_codec.cpp`: the two encoders
  and decoders (6.4, 8.1) and `makeEncoder`/`makeDecoder`.
- `engine/tests/test_engine_codecs.cpp` (13.1-13.4 plus a resampler/LPC
  check and the `OPUr` byte-at-a-time / truncated-stream checks of 8.1), `engine/tests/test_engine_clock.cpp` (13.5-13.9),
  `engine/tests/test_engine_e2e.cpp` (13.10-13.13, two engines against the
  in-process `Server` with a simulated audio clock, plus duplex rooms where
  both engines send and listen with offset interval clocks, FLAC and Opus).
- `tools/o11jam.cpp`: the command-line client of section 14.
- CMake option `O11PROTO_BUILD_ENGINE` (default ON) finds opus and flac with
  pkg-config; OFF builds only the protocol library.

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
- The channel list is sent from inside the AUTH_REPLY callback (the o11proto
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

## Opus (6.4, 8.1) - fourcc `OPUr`

- Fourcc `OPUr` (`kFourccOpusRaw`, bytes `O P U r` in wire order), the raw
  framing of revision 2: `makeEncoder`/`makeDecoder` accept only this fourcc;
  an incoming `OPUS` (Ogg Opus from the original plug-in) has no decoder and
  is ignored like any unknown codec. The protocol library maps both fourccs to
  the `0x100` capability (`codecCapForFourcc`), as PROTOCOL.md 7.8 says, and
  `o11cli` takes `.opur` files as `OPUr` (`.opus` still passes through as
  `OPUS`).
- `pre_skip` is `OPUS_GET_LOOKAHEAD` after the encoder is created. Each
  interval: `OPUS_RESET_STATE`, then the 12-byte header (version 1, channels,
  pre-skip, the input rate, reserved 0) written at once, each packet as `u16`
  length + bytes as soon as it is encoded, and the trailer `u16` 0 + `u32`
  real at the end. An encoder error or a packet above 65535 bytes (impossible:
  libopus never produces more than 1275 bytes for 20 ms) stops the packets of
  that interval; the trailer is still written.
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
  raised-cosine fade to 0. The trailer's `real` is the resampled frame count.
- Decoder: the header is parsed once 12 bytes are in (version != 1 or
  channels not 1 or 2: bad, nothing decoded); each packet is decoded once its
  length and all its bytes are in, so partial deliveries (down to one byte at
  a time) decode identically to the whole stream. The output of the most
  recent packet is held back until the next length prefix (then it is whole
  output), the trailer (split at `pre_skip + real` into output and lapping
  frames) or the end of the input without a trailer (whole output, no lapping
  frames, trailing partial bytes dropped). This is exact because the padding
  minus the pre-skip is 0..959 frames, always inside the last packet, and it
  costs 20 ms of latency between a packet's arrival and its availability,
  well inside the 8192-byte prebuffer. Frames are counted by what libopus
  returns for each packet (960 for a conforming stream, as 6.4 counts them).
  The header's input rate and reserved word are ignored; `rate()` is 48000.
  A packet libopus refuses marks the stream bad; what was decoded before it
  (including the held packet) is output and nothing after it.
- A stream from an `OPUS` (Ogg) sender is not decoded: test 13.14 is FLAC
  only, as revision 2 says.

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

- The channel's default volume is 0.25 as specified; `o11jam` and the e2e
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

## o11jam

- `--user` is passed verbatim; on an anonymous server use `anonymous:NAME`.
  `--two` logs the listener in as `NAME_l`.
- Extra options: `--password`, `--amp` (default 0.5), `--say TEXT` (sent once
  after login; `--say "!codec flac"` switches a room to FLAC), `--quiet`.
- The sender's own channel is muted for monitoring and the metronome is
  muted, so the recording of a `--two` run holds only what the listener
  hears. SNR is a least-squares fit of a sinusoid at the tone's frequency
  over the last two intervals, after trailing digital silence is trimmed:
  when the other player leaves before this run ends (7.1: a user's intervals
  are discarded the moment it goes), the recording ends in silence and a
  window that includes it would report a few dB although every decoded
  sample was right. The tone generator and the fit use a rotating phasor
  instead of a `sin()` per sample.
- The window is the last two intervals of the recording, leading silence
  included. In a `--two` run the listener logs in about 20 ms after the
  sender, so its interval clock (and the start of the first interval it
  plays, at 4 s with the server's default 120 BPM / 8 BPI) is offset by that
  much; a 12 s run's window of 8 s then begins some 10 ms before the first
  tone sample and reports about 29 dB although every played sample is right
  (10 log10(8 / 0.011) = 28.7). `--seconds 13` or more puts the window inside
  the played audio and shows the real figure (about 47 dB with Opus at 128
  kbps).
- The control loop sleeps 10 ms when `run()` reports idle (ENGINE.md asks
  for 50 ms at most; the upload queue holds about 5 s).
- `--wav` reads 16-bit PCM or 32-bit float WAV (mono, or the first channel)
  and resamples it linearly to `--srate`.

## Live mode (section 16)

Clean-room implementation of the Live mode specification, revision 2
(2026-10-17), sections 5 and 6. The only inputs were that document,
`PROTOCOL.md`, `ENGINE.md`, the o11proto sources and the public header of
libopus; no AOO, NINJAM or Jamsa source was read, and no AOO code is in it.
Live is o11proto's own ecosystem: nothing here exists only to talk to the
reference build, but the wire formats are the specification's.

**Files.** `engine/src/live_aoo.*` (the messages of Live 5.4-5.6),
`engine/src/live_sink.*` (the queues between the contexts and one sink's
buffer, decoder and resampler), `engine/src/live_layer.*` (the shared state,
the slots and health, the network thread's source and sinks, the audio
entry point), `engine/src/engine_live.cpp` (the `Engine` side: lifecycle,
switching, mixing, `/live`). Hooks in `engine.cpp`: `run()` calls
`liveControl()` after the client's poll; `process()` calls
`liveAudioBlock()` before the pieces and `mixLive()` after `mixRemote()` in
each piece; the boundary calls `liveBoundary()` before the new-interval step;
`mixRemote()` mutes a channel 0 that is heard live.

**Where o11proto differs from the Live spec, and why:**

- **Clock drift by the buffer's fill, not a time DLL** (Live 6.9). Each sink
  resamples 48 kHz to the host rate with a cubic. The fill is smoothed over
  0.5 s, its level after 1 s of playing is kept, and while the smoothed fill
  is more than 2 ms from that level the step is corrected by 0.1 per second
  of the distance beyond the 2 ms, within ±0.5 %. It keeps a sink from slowly
  filling up or running dry, which is what the DLL is for, without time
  stamps on every 64-sample block. The kept level is the one buffering gave,
  not *B*: regulating to *B* made the pitch wander by up to 0.1 % for tens of
  seconds after each start (a 997 Hz tone came out at 998 Hz), and the dead
  band keeps the fill's own jitter (whole blocks arriving at once) from
  moving the pitch at all, so a steady tone keeps its phase (`o11jam --two`
  on a Live server: SNR about 43 dB over two intervals at 48 kHz, 45 at
  44.1). In return the source writes the nominal 48000.0 into every `/data`
  instead of a measured rate, and a sink ignores the rate it receives (after
  checking it is finite and positive). Nobody between o11proto clients reads
  it.
- **The source's resampler** (host rate to 48 kHz) is a plain cubic, with no
  low-pass. That is right for 44.1 kHz (upsampling); at 88.2 or 96 kHz and up
  content above 24 kHz can alias. Hosts at 48 kHz skip it.
- **Buffering drops a burst.** When a sink starts playing it drops the oldest
  blocks above the latency's number, so a burst that arrived while it waited
  (a host that delivered several blocks at once) doesn't make it play later
  than *B*. The buffer figure is then at most *B* + a frame + a host block,
  as revision 2 asks.
- **Giving up after 4 × *B*** (Live 5.9) counts one underrun and starts
  waiting again; it doesn't start playing with fewer blocks than the latency.
- **Limits.** A sink's ring holds 256 blocks and its capacity is capped at
  250 (a 200 ms buffer with frame 120 and long host blocks could ask for
  more). Host blocks over 16384 frames get no Live audio and play intervals:
  the slots' output buffers are allocated for that size.
- **Accepted payloads.** The OSC forms of `/data` and of the data request are
  dropped (no Live client sends them); blocks split into frames are treated
  as lost (Live 5.4 allows it); channel onsets other than 0 are dropped
  (senders must send 0); `/start` block sizes other than 120, 240 and 480 are
  refused (quirk 25).
- **Threads.** One network thread (`live::Transport`'s) both receives and
  sends, waking at least every 1 ms, in place of the reference's receiving
  and sending threads (informative in Live 6.1).
- **Texts** say "O11 Session" where the reference names its own product, in
  `/live`, the offer and the slot-limit line.
- **The quirks** (Live 10), where a new client may or should choose: stop
  requests are answered at most once per 100 ms per peer (20); the source's
  `/stop`s go out before the socket closes (21); the stream time of a later
  `/start` counts 48 kHz samples at 48 kHz (22); a sink sends at most one
  request message per 10 ms (24); answer limits run on the steady clock, which
  keeps running while the stream is stopped (28); `/live buffer30` is read as
  `buffer 30` (12); slots go to the first listed peers (27); `/live` shows the
  latest server round trip (14) and keeps the reference's dropout count,
  underruns + overruns + lost blocks, while health counts underruns, overruns
  and xrun gaps (15).

**Ambiguities in the Live spec and how they were read:**

- *"After every underrun or overrun, the sink buffers … It gives up after 4 ×
  its latency and counts an underrun"* (5.9): what it does after giving up
  isn't said. Read as above: it counts the underrun and keeps waiting.
- *"If that needs more room than is free, it is an overrun"* (5.9): the room
  is counted from the next block to play to the new block, against the
  capacity in blocks.
- *"Its stream is active"* (6.4) is taken as the sink playing: not idle, not
  buffering and not dry. *"Healthy since"* restarts whenever health is lost.
- *"The report comes at the first block that doesn't arrive early"* (6.9):
  the late block itself is only noted; the gap is reported at the next block
  that isn't early, if the host is still behind by more than the threshold.
  So a host that misses time and doesn't catch up reports it one block late;
  one that catches up reports nothing.
- *"The host's announced largest block"* (6.8, the capacity): an engine has
  no announcement, so *H* (the longest block of the last 30 s) is used.
- *"The queue … holds max(25 ms, 2 × the host's largest block / rate + 10
  ms)"* (6.7): the capture queue is a fixed 1024 chunks of 64 frames (1.4 s
  at 48 kHz), which covers every host block up to the 16384-frame limit;
  chunks that don't fit are dropped.
- *"Stop requests … The first underrun starts the 1 s"* (5.9): measured from
  the last `/data` the sink took, which is the same moment.
- *"Live off … at the next boundary"*: when the room's Live goes off the
  mixer stops at once (6.5, "At once"), not at a boundary; a stall or packet
  loss makes a listener fall back at the next boundary. The tests check both.
- *"Health … about every 30 ms"* (6.4): worked out at every `run()` pass, which
  ENGINE.md section 2 asks for at least every 50 ms; the reference's network
  loop runs every 10 ms or so, and so does `o11jam`'s.

**Tests.** `test_engine_live_units` and `test_engine_live` (ENGINE.md 16.11).
The real-time test runs about 50 s. Machines that stall a thread for tens of
milliseconds (virtual machines do) can make a listener fall back for real;
the test notes every wake more than 10 ms late and makes the checks that
need a steady stream only when there was none.

## Efficiency notes

- The metronome keeps only its beat clock when muted or below 0.0001 (no
  per-sample loop); the master stage leaves the samples alone at unity gain
  and only meters them; decoders write straight into their output buffer
  (`reserveOutput`), the Opus decoder in runs rather than frame by frame.
- Of what remains, libopus (complexity 10, as 6.4 says) is about 85 % of a
  `o11jam --two` Opus run; the engine's own mixing, meters and buffer clearing
  are a few percent. A FLAC run shows more system time than an Opus run with
  the same syscall counts (it sends about four times as many WRITEs at the
  2048-byte threshold of 6.1 and three times the bytes); the threshold is
  kept as specified.

## Not implemented

- Recording received intervals to a work directory by GUID (protocol 7.5
  mentions it; ENGINE.md does not ask for it).
- Vorbis (as in ENGINE.md: a room without a mask gets nothing from this
  engine).
- A strictly allocation-free audio path; see "Threads and contexts".
