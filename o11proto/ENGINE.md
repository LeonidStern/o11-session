# NINJAM client engine specification

Revision 2c, 2026-10-18. Companion to `PROTOCOL.md` (revision 2d), which this
document builds on and never repeats: wire formats, message types, login,
codec negotiation and chat are defined there, by section number.

This document specifies the **client engine**: the part of a NINJAM client
that sits between the protocol and the audio device. It turns the user's input
audio into uploaded intervals, turns downloaded intervals into audio in time
with the room, and keeps the state a user interface shows (users, channels,
meters, tempo, position). It is written so that it can be implemented without
reading any existing NINJAM client.

The engine is the audio engine of the Jamsa plug-in and of a networked audio
device, so it covers what those need and nothing more. Out of scope on
purpose: Vorbis (a client built from this document decodes and sends Opus and
FLAC only), recording to disk, voice-chat and session channel modes (protocol
7.6 and 7.7: such channels are treated as silent and never created locally),
per-channel effect callbacks, custom mixers, lobby mode and DAW seek playback.
A later revision may add lossless local capture. Section 16 adds the Live
(UDP) mode.

## 1. Conventions

- "Sample" means one sample of one channel; "frame" means one sample of every
  channel at one instant. Audio in and out of the engine is 32-bit float,
  non-interleaved (one array per channel), nominal range -1..1.
- "Block" means one call of the audio callback with *n* frames.
- The engine has two contexts, defined in section 2: the **audio context** and
  the **control context**.
- Times in samples are at the engine's current sample rate, which is whatever
  the last audio block was called with.
- Numbers given as "the reference uses" are the values of the existing clients;
  a new implementation must use the same values where the document says
  **must**, and may improve on them where it says **may**.

## 2. Threads and contexts

The engine has no threads of its own, except Live mode's network thread
(16.1).

- The **audio context** is the host's audio callback (a DAW's process call, a
  device's I/O thread). The application calls `process()` (section 4) from
  there, once per block. `process()` must never wait for the network, the disk
  or the control context: it reads what has been prepared for it and queues
  what the control context should take. It must not allocate per block in
  steady state (allocations when a channel or user appears are acceptable).
- The **control context** is a loop the application runs on a thread of its
  choosing, calling `run()` (section 3.4) at least every 50 ms, ideally
  whenever `run()` says there is more to do. Everything else in the API (connect,
  settings, chat, queries) is called from the control context, or from any
  thread the application serialises with it. The engine's callbacks (section
  3.6) are invoked from inside `run()`.

Data crossing from one context to the other goes through queues or atomics
that the audio context never blocks on. A mutex that the control context holds
only for a few microseconds (swapping a pointer) is acceptable in the audio
context; a mutex held across network, codec or disk work is not. The reference
uses short mutexes around the user list and the local channel list; an
implementation may use lock-free queues instead.

## 3. Connection

### 3.1 Connecting

`connect(host, port, user, password)` resets all per-connection state (section
3.5), remembers the credentials and starts a non-blocking TCP connection. The
host string may carry a port as `host:port`; without one, port 2049. Calling it
while connected disconnects first.

The login follows PROTOCOL.md section 4 with these choices:

- `client_caps` always has the Opus bit (`0x100`) and the FLAC bit (`0x200`);
  bit 0 (licence) as decided below.
- `client_version` is `0x00020000`. A challenge whose `protocol_version` is
  below `0x00020000` or at or above `0x00030000` is a failure (reason
  `server is incorrect protocol version`) and the connection is closed.
- The keepalive interval is the one from the challenge (section 2 of the
  protocol); keepalives are sent whenever nothing else was sent for that long.

### 3.2 Licence

When the challenge carries a licence text the engine calls the application's
licence callback with the text. The callback answers **true** (agree) or
**false** at once, or **defer**, in which case the engine waits for
`answerLicence(bool)` from the control context. While waiting, the engine keeps
sending keepalives so the server doesn't drop it; the reference client instead
blocks its whole control loop, which is why servers give 120 s for this step.
Agreeing sets bit 0 of `client_caps`; not agreeing sends the login without it,
and the server's refusal then ends the connection like any refused login.

### 3.3 Status

`status()` is one of:

| Status | Meaning |
|---|---|
| `Preconnect` | not connected, connecting, or logging in |
| `Ok` | logged in |
| `CantConnect` | the TCP connection failed or was closed before the challenge |
| `InvalidAuth` | the server refused the login, or the engine closed the connection during login (bad version, licence refused) |
| `Disconnected` | the connection closed after a successful login |

`errorText()` holds the server's reason for a refused login (the text of the
AUTH_REPLY, protocol 4.3) or the engine's own reason, else "". The status
stays until the next `connect()` or `disconnect()`.

After a successful AUTH_REPLY the engine: records the effective username (the
reply's text; `username()` returns it from then on), records `maxchan`, sends
the local channel list (section 5.2) and sets status `Ok`. The engine is
**not** yet "running" (section 4.2) until the first CONFIG_CHANGE arrives.

### 3.4 `run()`

`run()` performs one step of control work and returns **true when the caller
may sleep** (nothing more to do right now) or **false when it should call
`run()` again at once**. One step is: read and handle every message available
on the socket, send keepalives, time out downloads (section 7.4), and encode
and upload everything the audio context has queued (section 6). When the
socket reports closed or an error, the status becomes `CantConnect` (no login
yet), `InvalidAuth` (during login) or `Disconnected` (after login) and the
audio stops running.

### 3.5 Disconnecting

`disconnect()` closes the socket (nothing is sent first), forgets every remote
user and download, discards queued and partially encoded local audio, ends the
interval clock and sets status `Preconnect`. Local channel definitions
(section 5.1) and their settings survive, with their indices unchanged, so the
same channels are announced at the next login. The licence, chat and user
settings callbacks are not called during disconnect.

### 3.6 Callbacks

The application may set these; all are invoked from `run()`:

- `licence(text) -> Agree | Refuse | Defer` (section 3.2);
- `chat(fields)`: every CHAT_MESSAGE except `SESSION` (protocol 9.2, 9.3), with
  its fields as received (up to five strings, missing ones empty);
- `usersChanged()`: the set of remote users or channels changed (section 7.1);
  the application then re-reads the lists. The reference exposes a flag the
  application polls (`hasUserInfoChanged()` clears it and returns whether it
  was set); an implementation provides at least the flag.

## 4. The audio callback and the interval clock

### 4.1 `process()`

    process(in, numIn, out, numOut, frames, sampleRate)

`in[c]` for `c < numIn` are the input channels, `out[c]` for `c < numOut` the
output channels, each `frames` long. The engine **clears every output channel
first**, then adds to them: the mix of remote channels (section 8), local
monitoring (section 5.4), the metronome (section 9), and finally applies the
master volume (section 8.5). Inputs are read before outputs are written only
in the sense that the engine must work when `in` and `out` are different
buffers; the application must not pass the same buffer as input and output.

`sampleRate` is the rate of this block. The engine uses it for all time
computations in this block; a change of rate takes effect at the next interval
boundary for the interval length and at once for everything else.

### 4.2 Running

The engine is **running** from the first `process()` call after the first
CONFIG_CHANGE of a connection until `disconnect()` or a lost connection. While
not running, `process()` does only local monitoring and the metronome is
silent; no interval clock advances, nothing is queued for upload, and
`isIntervalRunning()` is false.

### 4.3 Tempo and interval length

CONFIG_CHANGE (protocol section 5) sets the **server tempo** `serverBpm()`,
`serverBpi()` at once. The **active tempo** `bpm()`, `bpi()` and the interval
length change only at the next interval boundary (or when running starts):

    intervalLength = trunc( bpi * 60 / bpm * sampleRate )   samples

where `trunc` rounds toward zero. The reference computes this in double
precision exactly in that order (`bpi / (bpm / 60.0) * sampleRate`), then casts
to int; an implementation must produce the same integer for every
`bpm` in 40..400, `bpi` in 2..64 and `sampleRate` in 8000..192000. The beat
length used by the metronome is `trunc(intervalLength / bpi)`.

Before the first CONFIG_CHANGE the active tempo reads 120 BPM, 32 BPI and
`bpi()` of a disconnected engine is reported as those defaults; an application
that wants "has the server set a tempo" asks `serverBpi() > 0`.

### 4.4 Position

The engine keeps `intervalPos`, the number of samples of the current interval
already processed, and `intervalLength`. `position()` returns both (`pos` is 0
while not running). `loopCount()` counts interval boundaries since the
connection (the first interval is 1). `isIntervalRunning()` is true when
running and at least one interval has started.

`process()` splits its block at interval boundaries: it advances through the
block in pieces that never cross a boundary, so every piece belongs to one
interval and one tempo. At a boundary it applies a pending tempo change, calls
the **new-interval step** (sections 6.3 and 8.2), resets `intervalPos` to 0 and
increments `loopCount`. A boundary is processed when the first sample **after** the interval's
last one is about to be produced: after a block that ends exactly on the
boundary, `position()` still reports `pos == len` and `loopCount()` is
unchanged until the next block. The first boundary is at the start of the
first running block.

### 4.5 Ending an interval early or late

`setIntervalEnd(samples)` (audio context, between `process()` calls, or from
inside the application's own callback around it) makes the interval playing
now end `samples` (≥ 1) samples from the current position, i.e. sets
`intervalLength = intervalPos + samples` for this interval only. The next
interval starts at that point with the **usual** length (the length computed
from the tempo). `usualIntervalLength()` returns that usual length at any
time, while `position()` returns the possibly changed length of the interval
playing now. Calling it while no interval runs, or with `samples < 1`, does
nothing. Each client's interval phase is its own, so this lets a client line
its intervals up with something else (a DAW's bars) without a jump inside an
interval; the uploads and downloads are unaffected because they are cut at
whatever boundary the clock produces.

## 5. Local channels

### 5.1 Definition

A local channel has:

| Field | Default | Meaning |
|---|---|---|
| index | – | 0..31, the channel index on the wire; chosen by the application, stable |
| name | "" | sent to the server (protocol 6.1) |
| source | 0 | input channel index (0-based); with the stereo flag, that index and the next form a stereo pair |
| stereo | false | send two channels |
| bitrate | 64 | kbps, the Opus target when the room sets none (section 6.2) |
| broadcast | false | send audio (takes effect at the next interval boundary) |
| volume, pan | 1.0, 0.0 | monitoring level and pan |
| mute, solo | false | monitoring |
| outChannel | 0 | output channel index for monitoring, with the mono flag 1024 (section 8.4) |

`setLocalChannel(index, …)` creates the channel if it doesn't exist and sets
the given fields; `removeLocalChannel(index)` deletes it;
`localChannels()` lists the indices. Channel flags on the wire are always 0
(interval mode). A channel whose index is ≥ the server's `maxchan` is kept but
neither announced nor uploaded (its monitoring still works).

A missing input (source ≥ `numIn`) is silence. A stereo channel whose second
input is missing uses the first input for both sides.

### 5.2 Announcing

After login, and after any change of a channel's name or the set of channels
while logged in, the engine sends CLIENT_SET_CHANNEL_INFO (protocol 6.1) with
one record per index from 0 up to the highest defined index: defined channels
with their name, volume 0, pan 0, flags 0; undefined indices in between with
name "", flags `0x80` (deleted). The reference sends it whenever the
application asks (`notifyServerOfChannelChange()`); an implementation sends it
automatically on those changes and provides the explicit call too.

### 5.3 Capturing for upload (audio context)

For each local channel with index < `maxchan`, per block piece (section 4.4),
`process()` takes the source audio (one or two input channels, as is: no
volume applied) and, when the channel is **transmitting**, appends it to the
channel's upload queue as one block tagged with its channel count.

"Transmitting" is a per-channel state decided only at interval boundaries
(section 6.3), so that an interval is either sent whole or not at all.

The upload queue is bounded: the reference drops new blocks when the queue
holds 512 blocks (roughly 5 s at 512-frame blocks), so a stalled control
context can't eat memory. Blocks are copies; the queue must be safe to add to
from the audio context and take from the control context.

### 5.4 Monitoring

Each local channel is also added to the output, so the user hears their own
input through the engine when they want (Jamsa keeps this muted and monitors
on its own, but the behaviour is part of the engine):

- active = `(!anySolo && !mute) || solo`, where `anySolo` is true when any
  local or any remote channel is soloed;
- output pair as in section 8.4 with `outChannel`; stereo unless the mono
  flag or `numOut < 2`;
- stereo output: left += src1 × vol × (pan > 0 ? 1-pan : 1), right += src2 ×
  vol × (pan < 0 ? 1+pan : 1), where src2 = src1 for a mono channel;
- mono output: out += (src1 + src2)/2 × vol;
- the channel's peak meter (section 8.6) is updated from the source audio
  even when the channel is inactive.

Monitoring works whether or not the engine is running.

## 6. Uploading intervals (control context)

### 6.1 The upload loop

In each `run()`, for each local channel, the engine takes everything from the
channel's upload queue in order. The queue holds three kinds of items:

- an **audio block** (section 5.3);
- an **end marker**: the interval ended;
- a **silence marker**: the channel stopped transmitting at this boundary.

Processing, per channel, with a per-channel **current interval**:

1. **Audio block, no current interval:** start an interval. Pick the codec
   (section 6.2). If there is none, mark this interval as skipped: this and
   every later block up to the end marker is discarded and nothing is sent.
   Otherwise create or reuse the encoder (section 6.2), make a random GUID,
   and remember that the BEGIN (`CLIENT_UPLOAD_INTERVAL_BEGIN`: this GUID,
   estsize 0, the codec's fourcc, the channel index) is **pending**; it is
   sent together with the first WRITE, not before.
2. **Audio block, current interval:** feed the block to the encoder. Then,
   while the encoder has output ready and its size is at least the
   **threshold** (8192 bytes while the BEGIN is pending, 2048 after it), send
   the pending BEGIN if any, then one WRITE of at most **9216** bytes with the
   end flag clear, and repeat.
3. **End marker, current interval:** finish the encoder's stream (section
   6.4), then send the pending BEGIN if any and the remaining output in WRITEs
   of at most 9216 bytes; the last one has the end flag set. When the encoder
   has no output left at all, one WRITE with an empty payload and the end flag
   is still sent, so the server closes the transfer. Then forget the current
   interval (the encoder object is kept, section 6.2).
4. **End marker, no current interval** (a skipped interval): nothing.
5. **Silence marker:** send one BEGIN with an all-zero GUID, estsize 0,
   fourcc 0 and the channel index (protocol 7.1).

Blocks of a channel whose index is ≥ `maxchan` are discarded.

### 6.2 Codec, bitrate and encoder lifetime

The codec of an interval is decided when its first block is processed, from
the latest codec mask (protocol 8.3): FLAC if the mask has FLAC, else Opus if
it has Opus, else **none** (no Vorbis in this engine; with no mask received
the room is Vorbis and the engine sends nothing). An application may turn FLAC
off (`preferFlac(false)`, default on) so that a FLAC mask also means Opus;
this mirrors the reference's `config_send_flac`.

The Opus bitrate of an interval is the room's `opus_kbps` from the mask when
nonzero, else the channel's `bitrate` setting. (The reference leaves applying
the room's bitrate to the application; this engine does it itself, and
`uploadKbps(index)` reports the value in use.)

The encoder of a channel is created with (codec, sample rate, channel count,
bitrate). It is kept across intervals and **re-started** (a new stream with new
headers and a new random serial number) for each interval, because the Opus
encoder's resampler must run on across the boundary (section 6.4). It is
destroyed and created anew when any of the four parameters differs at the
start of an interval. `uploadCodec()` returns the fourcc the next interval
will use (0 for none), for the user interface.

### 6.3 Interval boundaries (audio context)

At each boundary of the interval clock, before anything else, for each local
channel (index < `maxchan`):

1. if the channel was transmitting, queue an **end marker**;
2. transmitting := the channel's `broadcast` setting;
3. if it was transmitting and no longer is, queue a **silence marker**.

So a channel that is switched off sends its last interval whole, then one
silence BEGIN, then nothing; one switched on starts sending with the next
whole interval. Remote players never get a partial interval.

### 6.4 The two encoders

Both produce one complete stream per interval, cut exactly at the interval's
sample count, so the receiver can play intervals back to back.

**Opus** (fourcc `OPUr`, raw Opus packets, protocol 7.8; no Ogg and no
container library):

- Encoder at 48000 Hz, 1 or 2 channels, application "audio", complexity 10,
  signal type music, constrained VBR at the interval's bitrate.
- The stream begins with a **12-byte header**: `u8` version = 1, `u8`
  channels (1 or 2), `u16` pre-skip (the encoder's lookahead plus the
  240-sample pre-roll below, in 48 kHz samples), `u32` input sample rate
  (informational, like `OpusHead`), `u32` reserved = 0. Little-endian, as
  everything on the wire.
- Then **packets**, each as `u16` length (1..65535) followed by that many
  bytes of one Opus packet of 960 samples (20 ms). Frames are counted by
  packet: packet *k* starts at 48 kHz sample `960 k` of the decoded stream.
- The stream ends with a **trailer**: `u16` 0, then `u32` `real`, the number
  of real 48 kHz samples after the pre-skip. A stream that stops without a
  trailer (the sender left) is played to its last whole packet with no
  trimming.
- Input at another rate is resampled to 48000 Hz with a windowed-sinc (or
  equally good) resampler whose state **persists across intervals** of the
  same encoder, so the stream of one interval begins exactly where the
  previous ended. The resampler keeps its position as an exact rational, so
  an interval of N input frames yields exactly `N × 48000 / rate` output
  frames (rounded the same way every time) and consecutive intervals never
  slip by a sample against each other. The Opus encoder state itself is reset at each new stream.
- **Pre-roll:** each stream begins with the last 240 samples (5 ms) of the
  resampled audio before it (zeros before the encoder's first), which the
  pre-skip drops again. A freshly reset encoder's first samples come out
  poorly; with the pre-roll they fall before the stream's real start, so the
  crossfade into the stream (8.3) is clean.
- **End of interval:** the real audio ends at `real` samples (after
  resampling). The stream is padded up to the next multiple of 960 of `real +
  pre_skip`, so the encoder's lookahead is flushed and the last frame is
  whole. The padding is not silence: it is a linear-prediction extrapolation
  of the audio (order 24, Burg's fit to the last 1024 samples of each
  channel, with bandwidth expansion 0.998^k for stability and each sample
  clamped to ±4), kept at full level for the first 128 samples and then
  faded out with a raised cosine over the rest. The trailer's `real` lets the
  decoder trim the padding; a NINJAM decoder uses the trimmed part for the
  crossfade (section 8.3). Burg's fit uses the samples themselves, with no
  window and no zeros assumed beyond them, so the extrapolation continues the
  audio's slope even where it changes within the 1024 samples (a note's
  attack just before the boundary); an autocorrelation fit (as libvorbis'
  `lpc.c`) starts with a kink there, which the crossfade turns into a
  tick.
- Output bytes are written as produced: the header at once, each packet as
  soon as it is encoded, the trailer at the end. Overhead is 2 bytes per
  packet.

**FLAC** (fourcc `FLAC`, native FLAC):

- 16-bit samples at the engine's sample rate (no resampling), 1 or 2
  channels, compression level 5, verify off, the "streamable subset" when the
  sample rate allows it, else not. The stream is written as it is encoded, so
  STREAMINFO's total sample count stays 0.
- Float to 16-bit: exact zero (and NaN) stays exactly 0, so digital silence
  compresses to almost nothing; a value that is already exactly on a 16-bit
  step is converted exactly; any other value is scaled by 32768, gets TPDF
  dither of 1 LSB (two uniform random values in 0..1, added, minus 1), is
  rounded to nearest and clipped to -32768..32767.
- End of interval: finish the stream, which writes the last, shorter frame.
  The next interval starts a fresh stream (`fLaC`, STREAMINFO, frames).

## 7. Remote users and downloads (control context)

### 7.1 Users and channels

The engine keeps the remote users in a list in order of first appearance; the
application addresses them by index in that list and channels by their wire
index 0..31. From each USERINFO record (protocol 6.2):

- **active:** create the user if unknown; mark the channel present; set its
  name and flags. If the channel's flags changed in bits `0x02` or `0x04`
  (voice/session), discard its playing and queued intervals. If auto-subscribe
  is on (default on), add the channel to the user's subscription mask and send
  CLIENT_SET_USERMASK with the whole mask. If the channel is newly present and
  has no output pair yet, assign one (section 8.4).
- **inactive:** if the user is known: clear the channel's name, present bit,
  subscription bit and solo bit; discard its intervals; if the user has no
  present channel left, delete the user (and its settings). Recompute `anySolo`.

Every record sets the users-changed flag. Users are matched by exact name
(the protocol's names are case-insensitive for login, but the server sends one
spelling).

Per user the application can read and set: volume (default 1.0), pan (0.0),
mute (false). Per channel: subscribed, volume (default **0.25**), pan (0.0),
mute, solo, output pair, flags, name, the peak and level meters (section 8.6)
and the dropout count (section 8.7). Setting
`subscribed` sends CLIENT_SET_USERMASK with the updated mask; unsubscribing
also discards the channel's playing and queued intervals.

### 7.2 DOWNLOAD_BEGIN

For a BEGIN naming a known user and a channel index < 32 (others are ignored):

- **zero GUID** (silence): queue a **silent interval** for that channel
  (section 7.3's queue rules). The reference instead removes the newest queued
  interval when two are queued, which in the normal case of at most one queued
  interval per boundary plays the same silence.
- **nonzero GUID, nonzero fourcc:** start a **download**: remember GUID, user,
  channel, fourcc, a growing byte buffer, and the time of last data. The
  reference keeps no limit on the number of downloads beyond the 8 s timeout.
- **nonzero GUID, fourcc 0:** ignore (protocol 7.4).

Channels with flags `0x02` or `0x04` never get intervals queued; their
downloads are accepted and discarded.

### 7.3 DOWNLOAD_WRITE and the interval queue

For a WRITE whose GUID matches a download: append the bytes, note the time.
When the download has grown past the **prebuffer** of 8192 bytes, or the WRITE
has the end flag, the download becomes **playable** (once): a decoder for its
fourcc is created on its buffer, primed (decode until it has produced at
least one frame of output or the data so far is exhausted), and the result is
put in the channel's interval queue. When there is no decoder for the fourcc
(a codec this engine doesn't play, such as Vorbis), an **undecodable** entry
goes in the queue in its place: it plays as silence and counts as a dropout
(section 8.7). The buffer keeps receiving later WRITEs
while the decoder reads from it, so decoding streams; the end flag just marks
that no more will come. A download with the end flag is forgotten after this
(its buffer lives on with the decoder).

The **interval queue** of a channel holds at most **two** entries: slot 0 and
slot 1, each a decoded-interval-in-progress, a silent interval or an
undecodable one. A new entry
goes into slot 0 if empty, else into slot 1, **replacing** whatever was there.
Section 8.2 says how the queue advances.

### 7.4 Download timeout

A download that received nothing for **8 s** is dropped; if it was already
playable, its decoder plays what arrived and then silence. The check runs in
`run()` (the reference checks only while handling another WRITE, which means
a last stalled download can linger; checking on every `run()` is required
here).

## 8. Playing intervals (audio context)

### 8.1 Decoders

A decoder takes the bytes of one stream as they arrive and yields float
frames, interleaved, at the stream's sample rate and channel count (`rate()`,
`channels()`, known once the headers are decoded; before that, 48000 and 1).
It has `available()` frames, `read(n)` / `skip(n)`, and `lappingFrames()`:
the trailing audio the stream carried past its real end, used for the
crossfade (section 8.3). Feeding it the partial data of a stream must never
fail: it decodes what it can and resumes when more arrives. Decoding runs in
the audio context, on demand: `decodeStep()` decodes one Opus packet or one
FLAC frame, and the mixer calls `decodeUntil(n)` (steps until `n` frames are
available or no step can make progress) for the frames a block needs, so a
block never decodes much more than it plays.

**Opus:** the 12-byte header gives channels and pre-skip (`rate()` reports
48000); packets are decoded at 48000 Hz as each one's length and bytes have
fully arrived; the first `pre_skip` samples are dropped. On the trailer,
samples past `real` are not output but kept as the lapping frames. A stream
with version ≠ 1, channels not 1 or 2, or a packet length of more than
65535 bytes (impossible by construction) or that libopus refuses is bad and
decodes to nothing from that point.

**FLAC:** STREAMINFO gives channels (more than two: only the first two are
used), bit depth and sample rate. The library in use may not resume from a
truncated frame, so the decoder must keep the input itself and retry a frame
from its start once more data has arrived. Samples are scaled by 1/2^(bits-1).
A frame whose sample number is beyond the expected one (a damaged frame was
dropped) is preceded by silence of the missing length (at most 5 s), so the
rest of the interval stays in time. No lapping frames (lossless streams join
exactly).

### 8.2 The new-interval step

At each interval boundary (section 4.4), for every remote channel of every
user whose flags have neither `0x02` nor `0x04`:

1. remember the **fade-out**: the current decoder's lapping frames followed by
   whatever it has available, up to 128 frames (two channels at most);
2. drop the current interval;
3. if the channel is subscribed and present, slot 0 of the queue becomes the
   current interval, else slot 0 is discarded;
4. slot 1 moves to slot 0; slot 1 is cleared;
5. reset the channel's drop count (section 8.5), and count a dropout when
   the channel is late or the new interval is undecodable (section 8.7);
6. if there is a new current interval with a decoder, apply the **fade-in**
   (section 8.3).

A silent interval as the current interval plays nothing.

### 8.3 Crossfade

The fade-out (≤ 128 frames) of the previous interval is mixed into the first
frames of the new one: the new decoder is primed until it has at least that
many frames (or the data is exhausted), then for i = 0..n-1 (n = fade length),
with s = 0.5 − 0.5 cos(π (i + 0.5) / n), a raised cosine:
`new[i] = new[i] × s + old[i] × (1-s)`, per channel, only when both have the
same channel count. The crossfade hides the small
discontinuity at an Opus boundary; for FLAC the fade-out is empty.

### 8.4 Output pairs

Every remote channel and every local channel has an `outChannel` value:
bits 0..9 are the index of the first output channel, bit 10 (1024) means mono
(one output channel). An application may add a constant offset to all remote
or all local `outChannel`s (`setRemoteChannelOffset`, `setLocalChannelOffset`,
default 0). The pair used is `idx = (outChannel + offset) & 1023` and
`idx + 1`; it is mono when the mono bit is set or `numOut < 2`; if `idx + width
> numOut` the pair is moved down so it fits, and never below 0.

**Automatic assignment** (`autoOutputPairs(mode, numOut)`, default off):
mode 1 assigns per channel, mode 2 per user (a user's channels share one
pair; a new channel of a user who already has a channel with a pair takes
that pair). A pair is chosen among the even indices 2, 4, … up to `numOut - 2`:
the first pair nobody uses, else the least used one, where a present
(active) channel on a pair counts 16 and a channel that is gone but still
remembered counts 1. Pair 0 (the main output) is never assigned. With `numOut
< 4` nothing is assigned (value 0). Jamsa uses mode 2 with 2 + 2 × 8 outputs,
so every player lands on an output pair of their own after the main pair.

### 8.5 Mixing a block piece

For each remote channel that is present, with its current interval:

- volume = user.volume × channel.volume, clipped to 0..4; pan = user.pan +
  channel.pan clipped to -1..1;
- muted = `anySolo ? !channel.solo : (channel.mute || user.mute)`, where
  `anySolo` counts local and remote solos;
- decay the channel's peak meter (section 8.6);
- if there is no decoder (no interval, or a silent one): nothing more;
- **needed** = the number of source frames this piece needs: `len` when the
  stream's rate equals the engine's, else `trunc(srcRate × len / dstRate +
  phase)` where `phase` is the resampler's fractional position carried from
  the last piece (reset to 0 at each interval boundary);
- decode until `available() > needed`, or the stream is exhausted;
- if the channel has a **drop count** (section 8.2, below): skip that many
  frames first (as many as are available), reducing the count;
- if `available() ≥ needed`: unless volume < 10⁻⁷, clip the source samples
  to -1..1 in place and work out the output samples: the output pair with
  the pan law of section 5.4, resampling by linear interpolation when the
  rates differ (an implementation may use a better resampler; the reference
  uses linear). A mono source goes to both outputs; a stereo source into a
  mono output is summed at half volume. Each output sample is clipped to
  -1..1. Update the level meter (section 8.6) from them; unless muted, also
  update the peak meter and add them to the output pair. Then skip `needed`
  frames.
- else (the stream is late or short): skip everything available and add the
  shortfall `needed - available()` to the channel's drop count, so that when
  the data arrives the stream catches up by dropping those frames instead of
  playing late. Output nothing for this piece. This may be a dropout (section
  8.7). The drop count is the frames the stream owes, not a count of
  dropouts.

Then the **master** volume, pan and mute (defaults 1.0, 0.0, false; the same
pan law) are applied to output channels 0 and 1 only, and the output peak
meters are updated from them. Other output channels carry their pairs as mixed.

### 8.6 Meters

Peak meters (per local channel, per remote channel, two values each for the
two output sides, and the master) hold the largest absolute sample seen,
decaying by a factor of `0.015625^(len / sampleRate)` per piece of `len`
frames (-36 dB per second). Remote channel meters measure after volume (the
reference stores the peak × volume). They are readable from any thread as
plain floats.

Each remote channel has a second meter, its **level** (`channelLevel(user,
ch, side)`): the same measurement as its peak meter (after volume and pan,
with the same decay), but taken whether or not the channel is heard, so it
goes on showing what the channel plays while the channel is muted, its user
is muted or another channel is soloed. While the channel is heard the two
are equal. (The reference has only the peak meter.)

### 8.7 Dropouts

Each remote channel counts its **dropouts**: intervals, or parts of
intervals, that a network or decoding problem kept from playing in full on
time. The count is for a user interface that tells the user a player's
audio isn't getting through, so nothing else may count.
`channelDropouts(user, ch)` returns it, readable from any thread. It is 0
when the channel appears (a USERINFO record makes it present when it was
not) and grows by at most one per interval of the engine's clock, in three
cases:

- **Late data:** a piece of the current interval finds fewer frames than it
  needs (section 8.5) while the interval's download is still arriving (or
  the stream went bad and decodes no further, section 8.1), and it lacks
  more than **64** source frames of what the rest of the interval needs. A
  stream that runs dry and catches up several times in one interval is one
  dropout.
- **Missing:** at an interval boundary the channel is subscribed and
  present, slot 0 of its queue is empty, and the channel is **expecting**
  audio: its last current interval was audio (a decoder or an undecodable
  one). The next interval had not arrived, or not become playable (section
  7.3), when it should have started; it plays one interval later if it
  comes at all. The channel goes on expecting across such boundaries, so
  each interval missed counts. It stops expecting when a silent interval
  (zero GUID) becomes current and when its intervals are discarded (section
  7.1); a channel that has sent nothing yet expects nothing.
- **Undecodable:** an undecodable interval (section 7.3) becomes current.
  This always counts, also in the cases below.

Not dropouts:

- A stream that has all arrived, however much shorter than the interval it
  plays in: its sender's interval was shorter (the sender started
  mid-interval, the interval was recorded at another tempo around a tempo
  change, the sender's host stopped).
- Late data in the **first audio interval** of a channel that was not
  expecting audio (it just appeared, was subscribed again, or comes back
  from a silent interval): that interval is not held to time.
- A missing interval at the boundary that ends an interval whose stream
  **ran long**, with more than 64 frames left over (the engine decodes that
  far to see): the sender's interval was longer (its interval end moved, it
  switched to a slower tempo first), so its next one is due that much later.
- Late data and missing intervals while this engine's **own clock changes**:
  from a CONFIG_CHANGE with a new BPM or BPI until the boundary that applies
  it, and from any boundary where the BPM, BPI or interval length change or
  where `setIntervalEnd` moved the end (section 4.5) until one interval of
  the old usual length and one of the new have played. Each side switches at
  its own boundary, so for that long intervals can come early or late by the
  change alone: when the listener switches to a faster tempo before the
  sender, one of its new intervals gets nothing and the next plays silence.
- Silent intervals, unsubscribed channels, voice and session channels, an
  interval replaced in slot 1 of a full queue (section 7.3: the listener's
  own audio context stalled), and audio that a boundary cuts off.

## 9. Metronome

Settings: volume (default 0.5), pan (0.0), mute (false), output pair in the
`outChannel` form (default 0). While running, at every beat (every beat
length from section 4.3, restarting at each interval boundary) a click of
`sampleRate / 100` samples starts: sample k of the click is `sin(k × 6000 /
sampleRate)` on the first beat of the interval and `0.25 × sin(2k × 6000 /
sampleRate)` on the others, times the volume (0 when muted), with the pan law
of section 5.4, added to the pair. Not played while not running, or while the
volume is below 0.0001. (Jamsa plays its own click and mutes this one, but a
device without a user interface needs it.)

## 10. Chat

`say(text)` sends `MSG`; `privateMessage(user, text)` sends `PRIVMSG`;
`admin(command)` sends `ADMIN`; `sendChat(fields)` sends any five fields. The
room commands (`!vote`, `!codec`, `!bitrate`, `/topic`, …) are plain text the
application puts in a `MSG` or `ADMIN` as the protocol describes; the engine
treats none of them specially. Received messages go to the chat callback
(section 3.6) except `SESSION`, which is dropped. A `!vote` that counts comes
back to everyone as the voter's own `MSG`, before the `[voting system]` line
(protocol 9.4), and reaches the callback like any other chat.

## 11. Queries

For the user interface, all readable from the control context (and the plain
numbers from any thread):

- connection: `status()`, `errorText()`, `username()`, `hostName()`,
  `maxLocalChannels()` (the server's `maxchan`);
- tempo and clock: `serverBpm()`, `serverBpi()`, `bpm()`, `bpi()`,
  `position(&pos, &len)`, `usualIntervalLength()`, `isIntervalRunning()`,
  `loopCount()`, `sampleRate()` (of the last block);
- codecs: `codecMask()`, `roomOpusKbps()`, `uploadCodec()`, `uploadKbps(i)`;
- remote: `numUsers()`, user name/volume/pan/mute by index, channel
  enumeration (`channelAt(user, i)` returns the i-th present channel's index
  or -1), channel name/subscribed/volume/pan/mute/solo/outChannel/flags, peaks,
  levels (`channelLevel`, section 8.6), dropouts (`channelDropouts`, section
  8.7);
- local: channel list, each field of section 5.1, peaks;
- `anySolo()`, master peaks.

## 12. Constants

| Name | Value |
|---|---|
| default port | 2049 |
| protocol version sent | 0x00020000 |
| accepted server versions | 0x00020000 ≤ v < 0x00030000 |
| first WRITE threshold | 8192 bytes |
| later WRITE threshold | 2048 bytes |
| largest WRITE | 9216 bytes |
| upload queue limit | 512 blocks |
| playback prebuffer | 8192 bytes |
| interval queue depth | 2 |
| download timeout | 8 s |
| crossfade length | ≤ 128 frames |
| Opus rate / frame | 48000 Hz / 960 samples |
| Opus stream | `OPUr`: 12-byte header, `u16` length + packet, trailer `u16` 0 + `u32` real |
| Opus pre-roll | 240 samples, inside the pre-skip |
| Opus padding | LPC order 24, Burg's fit to 1024 samples, 128 full-level |
| FLAC | 16 bit, level 5, TPDF 1 LSB |
| meter decay | -36 dB/s |
| default local bitrate | 64 kbps |
| default remote channel volume | 0.25 |
| default metronome volume | 0.5 |
| drop limit for a FLAC gap | 5 s |
| dropout slack for late data at the end of an interval | 64 source frames |

## 13. Acceptance tests

A conforming implementation passes:

**Codecs (unit tests, no network)**
1. Opus round trip at 48000 Hz, stereo, 128 kbps: a 997 Hz tone of 2.0 s
   encoded as one interval and decoded yields exactly `2.0 × 48000` frames
   after trimming, with SNR ≥ 35 dB over frames 4800..91200, plus lapping
   frames that continue the tone (first 128 of them within 6 dB of the signal
   level).
2. The same at 44100 Hz input: the decoded 48 kHz stream has 96000 ± 2
   frames; two consecutive intervals
   of a continuous tone, decoded and joined with the crossfade of section 8.3,
   show no discontinuity (no sample-to-sample step larger than the tone's own
   maximum step × 1.5 around the join).
3. FLAC round trip, stereo, 44100 Hz: a signal already quantised to 16 bits
   decodes bit-exactly; digital silence of 2 s encodes to under 512 bytes
   (libFLAC writes a VORBIS_COMMENT block and 4096-sample frames, about 400
   bytes in all);
   decoding from data delivered in chunks of 700 bytes gives the same output
   as from the whole stream.
4. Float conversion: 0.0 → 0, 1.0 → 32767, -1.0 → -32768; a constant 0.5
   dithers to 16384 ± 1.
18. Joins after an onset: three consecutive Opus intervals (48 kHz, mono,
    128 kbps) of a note sequence whose boundaries fall 270 frames after a
    note starts: the second difference into the lapping frames and across
    the crossfaded join is at most twice the source's own there, and each
    new stream's first frame is within 0.01 of the source. The padding's
    fit alone, 270 frames after an onset: no kink where the extrapolation
    starts (its second difference at most 1.5 × the note's own); a sine
    continues within 3 dB over 128 samples; silence extrapolates to
    silence.

**Clock (unit tests)**
5. `intervalLength` for (120 BPM, 32 BPI, 44100) = 705600; (100, 16, 48000) =
   460800; (97, 7, 48000) = 207835; (40, 64, 192000) = 18432000.
6. `setIntervalEnd(100)` at position 1000 of a 705600 interval ends it at
   1100 and the next one is 705600 long; `usualIntervalLength()` is 705600
   throughout.
7. A tempo change arriving mid-interval changes `serverBpm()` at once and
   `bpm()` only at the next boundary.

**Mixing (unit tests)**
8. Pan law: a 1.0 source at pan -1 gives left 1.0 × vol, right 0; at pan 0.5
   left 0.5, right 1.0.
9. A stream at 48000 played at 44100 keeps time: over 10 s of output the
   consumed source frame count is 480000 ± 100.

**Meters and dropouts (end to end, simulated clock)**
15. One engine fed by a protocol client that uploads FLAC intervals of a 997
    Hz tone at 0.5 when the test says (1 s intervals), the remote channel at
    volume 1.0: while it is heard, the level equals the peak (both
    0.45..0.55); with the channel muted, its user muted, or another channel
    soloed, the output is 0, the peak falls below 0.01 within 1 s and the
    level stays within 0.45..0.55; at volume 0.5 and pan -1 the level is
    0.22..0.28 left and below 0.01 right, muted or not.
16. The same rig. Count 0: a joiner's first interval holding half an
    interval, then one 3 frames short and a later one holding half an
    interval, all fully arrived; a zero-GUID BEGIN and the silence after it;
    intervals while unsubscribed; data that stops after 9000 bytes in the
    first audio after a silence. Count 1 each: an interval arriving after
    the boundary it should have started at; each of two missing intervals;
    data that stops after 9000 bytes and comes later in the interval (the
    audio is back before the interval ends); an undecodable (Vorbis)
    interval. The count is per channel and starts again at 0 when a channel
    goes and comes back. A vote from 120 to 100 BPM arriving during an
    interval: stalled data in the first interval at the new tempo counts 0,
    in an interval after the grace of 8.7 it counts 1.
17. Two engines, Opus, the listener's clock 1 s behind the sender's: votes
    from 120 to 100 BPM and from 8 to 16 BPI count 0, and the last two
    intervals have SNR ≥ 35 dB. At 100 BPM with the listener's clock 0.9 s
    behind, a vote to 120 BPM just after a sender boundary leaves one whole
    listener interval silent (the listener switches 3.9 s before the
    sender) and counts 0. In tests 10-13 every listener counts 0.

**Against servers (end to end, needs `o11server` from this repository and may
also be run against the reference server)**
10. Two engines in one process, 48000 Hz, 512-frame blocks, a 997 Hz tone of
    0.5 amplitude on one, the other subscribed with the channel volume set to
    1.0: after 12 s the listener's last two intervals have SNR ≥ 35 dB with
    Opus at the room's 128 kbps and the tone's peak within 0.45..0.55; the
    same room switched with `!codec flac` gives SNR ≥ 60 dB, measured once two
    whole FLAC intervals have played (the interval in progress at the switch
    stays Opus).
11. The same at 44100 Hz on both sides, Opus: SNR ≥ 30 dB.
12. Transmit off then on: the listener gets a zero-GUID BEGIN once, then
    whole intervals again; while off, the listener's output for that channel
    is exactly 0 after the current interval ends.
13. A listener whose `process()` is not called for 20 s and then resumes
    plays, within two intervals, the newest interval and nothing older after
    it (the queue depth of 2 holds: slot 0 keeps the first interval that
    arrived during the stall, slot 1 the newest).
14. Against the reference client (`ninjam/tests/njclient_e2e` sender or
    listener, run by a tester who may read both): engine listener with
    reference sender and the reverse both reach SNR ≥ 60 dB with FLAC. (Opus
    no longer interoperates with the reference: `OPUr` is not Ogg. The
    reference ignores it, and the engine ignores the reference's `OPUS`.)

## 14. Deliverables and dependencies

- A library on top of the o11proto client (`include/o11proto/engine.h` and
  friends, in the same CMake project, target `o11proto_engine`), C++17, no
  dependency besides the standard library, POSIX sockets, **libopus and
  libFLAC** (both BSD-licensed; no libogg, the Opus framing is the engine's own; found with `find_package`/`pkg-config`,
  with a CMake option to build the engine at all so the protocol library still
  builds without them).
- Unit tests for sections 13.1-13.9 under `ctest`.
- A command-line tool `o11jam` for the end-to-end tests and for interoperability
  testing by someone else: `o11jam host:port --user NAME [--tone HZ | --wav
  FILE] [--record out.wav] [--seconds N] [--srate N] [--block N] [--kbps N]
  [--no-flac] [--listen-only] [--toggle-transmit SECONDS] [--two]`. It plays
  the tone (or the WAV file) into local channel 0, subscribes to everyone,
  records what it hears to a 32-bit float WAV, and at the end prints
  `bpi=… interval_samples=… peak=… snr=…` measured over the last two
  intervals of the recording against the tone, like the reference's
  `njclient_e2e`. `--two` runs a sender and a listener in one process and
  reports the listener's figures (test 13.10). `--toggle-transmit S` switches
  transmit off after S seconds and on again after 2S (test 13.12).
- `NOTES.md` in the engine's folder listing anything the specification left
  open and how it was decided.

## 15. Revision history

- **1** (2026-10-16): first version, from the behaviour of the reference client
  as the Jamsa plug-in uses it.
- **2** (2026-10-17): Opus intervals are raw packets with a 2-byte length
  prefix, fourcc `OPUr` (6.4, 8.1); libogg is no longer a dependency; test
  13.14 is FLAC only. Decided because the new plug-in does not need to
  interoperate with Ogg-based NINJAM clients.
- **2a** (2026-10-17): a level meter per remote channel that keeps measuring
  while the channel is muted or soloed away (8.5, 8.6), dropout counts per
  remote channel (8.7) and the undecodable queue entry they need (7.3),
  tests 13.15 and 13.16; a counted `!vote` is echoed to the room (protocol
  revision 2c, section 10 here).
- **2b** (2026-10-17): dropouts count network and decoding problems only
  (8.7): a stream that has all arrived never counts however short, nor
  late data in a channel's first audio interval, nor a missing interval
  after one that ran long, nor anything while the listener's own clock
  changes; tests 13.16 and 13.17.
- **2c** (2026-10-18): each Opus stream begins with a 240-sample pre-roll
  inside its pre-skip, and the padding's fit is Burg's (6.4); the crossfade
  is a raised cosine (8.3); decoders decode on demand, one packet or frame
  a step (8.1); test 18. Live mode (16) as the engine plays it now: the
  room's one mode with no interval audio while Live and the health report
  and advice in place of per-player switching; all to all; the automatic
  jitter buffer; concealment by prediction with a primed decoder; the sink
  back on its delay after a gap, the host's schedule and path changes; the
  two-path mixer; the per-user counters and events; the network thread's
  scheduling and wake; `/live`'s texts; the tests.
- **1a** (2026-10-17): clarifications after the first implementation: exact
  boundary semantics (4.4), exact resampler positions and undithered exact
  values (6.4), playback phase reset (8.5), tests 13.3, 13.10 and 13.13
  restated.

## 16. Live mode

Live mode (PROTOCOL.md section 15) lets the Live players of a room hear each
other's channel 0 in real time over UDP. The room has one mode for everyone,
set by its `!live` switch alone (PROTOCOL.md 15.11): while it is Live, every
Live player's channel 0 is heard live, nothing is uploaded and no interval
audio is heard; otherwise everyone plays intervals. The players send to each
other directly where they can, all to all (PROTOCOL.md 15.10). This section
is the engine's side, written from the Live mode specification, revision 2
(2026-10-17), called "the Live spec" below and cited by its section numbers
("Live 6.4"). It doesn't repeat that document's texts and wire details; it
says how the engine maps them onto sections 2-11, what it chose where the
Live spec leaves room, and what it does in place of the Live spec where
o11proto differs (the room's one mode and its advice in place of Live 6.5's
per-player switching, the automatic jitter buffer, concealment by
prediction, the sink's delay after a gap; why in `engine/NOTES.md`). The
audio payload is the subset of AOO in Live 5 (PROTOCOL.md 15.9), written from
that description alone, with libopus as the only codec library.

### 16.1 Contexts

- A third context, the **network thread**, is the thread of
  `live::Transport` (PROTOCOL.md 15.6): one per connection, started and
  stopped from `run()`. It runs the client's AOO source (encoding, `/start`,
  `/stop`, pings, resends), the network side of its eight sinks (the
  receive filter, pongs, start and stop requests, the source timeout) and
  the probes of the direct paths. It is a network thread
  (`thread::makeNetworkThread()`, below): real-time on macOS. It wakes for
  each datagram, for `Transport::wake()`, which `process()` calls whenever
  it queues capture or resend requests and the control context calls when it
  applies a `0x07` or a room message, and at least every 10 ms
  (`kLiveNetworkWaitMs`) for a pass of its own; on Windows, which has no
  wake, every millisecond.
- The control context and the network thread share one mutex; neither is
  real time.
- The audio context never takes that mutex and allocates nothing for Live.
  It talks to the network thread through single-producer single-consumer
  queues allocated when the engine is made (capture: 1024 items of up to 64
  frames; per slot: 256 received items; resend requests: 1024) and to the
  control context through atomics. The Opus decoders, and each sink's
  primer encoder (16.6), are set up in memory allocated with the engine.
- Each slot has a **generation**. It changes when the slot gets a new peer or
  that peer's stream is dropped (the peer left, the room went off, the Live
  client restarted). Items of an older generation are discarded, the sink
  plays out and conceals what it had for the mixer to fade out (16.6), and
  its statistics restart.
- **Thread scheduling** (`o11proto/realtime.h`, for the engine and the
  hosts): `thread::makeAudioThread(period)` makes the calling thread a
  host's audio thread (on macOS the time-constraint policy, its computation
  half the period, at most 5 ms, and its constraint the period; on Linux the
  least timer slack); `makeLatencyCritical()` gives the user-interactive QoS
  class on macOS and the least timer slack on Linux; `makeNetworkThread()`
  gives on macOS the time-constraint policy of a 2 ms period with 1 ms of
  computation, falling back to `makeLatencyCritical()`, which it is
  elsewhere; `describe()` is one line on the calling thread's scheduling and
  how long timed waits of 1, 2 and 5 ms take on it. A DAW's audio thread
  already is an audio thread; o11proto's own hosts (`o11jam` and the tests)
  call `makeAudioThread()`, and `o11server`'s thread is a network thread.

### 16.2 API

| Call | Context | Meaning |
|---|---|---|
| `onLiveNotice(text, error)` | from `run()` and `liveCommand()` | the Live chat lines (16.3, 16.7, 16.9); `error` marks the error style |
| `setLiveEnabled(bool)`, `liveEnabled()` | control | announce Live (caps bits 16 and 17) at the next `connect()`; default on |
| `setLiveBufferMs(ms)`, `setLiveBufferAutomatic()`, `liveBufferMs()`, `liveBufferAutomatic()` | control | the jitter buffer (16.6): automatic (`kLiveBufferAutomatic`, 0, the default), or fixed at `ms`, clamped to 5..200 |
| `liveBufferInUseMs()` | any | *B*: the fixed setting, or what an automatic stream starts at, raised for long host blocks |
| `parseLiveBufferSetting(value, &ms)` | static | reads a saved `live_buffer_ms=` value: `auto` or 0 is automatic, a whole decimal number is clamped to 5..200, anything else is false |
| `liveCommand(text)` | control | handles `/live …` (16.9): true when the text was a `/live` command; it is never sent |
| `setLiveHostState(playing, offline)` | any | the host's transport state and offline rendering (Live 6.1, 6.7); default playing, not offline |
| `liveRunning()` | any | the Live client (its UDP socket) runs |
| `userLive(user)` | control | the mixer plays this user's channel 0 live now |
| `liveRoomLive()` | control | this engine plays the room Live now (16.7) |
| `liveAdviceReason()`, `liveAdviceUser()`, `liveAdviceCount()` | control | the server's advice while the room is Live (16.7): `LiveAdvice::None`, `Dropouts` or `CantLive`; the user it names ("" with none); how many players have trouble, that one among them (0 with none) |
| `liveServerHas()`, `liveRoomSwitchOn()` | control | the latest `0x07` says the server has Live with its UDP port open, and the room's switch is on |
| `liveHeardMs()` | control | about how late this listener hears the Live players: the median of the 6.10 estimates of the slots playing live, rounded; -1 while the room isn't Live or there is none yet |
| `liveUserBufferMs(user)`, `liveUserHeardMs(user)` | any | that user's stream: its buffer's fill smoothed over 0.5 s, and how late this listener hears it (the 6.10 estimate with that fill in place of what the buffer held when playback began), ms; -1 while the stream doesn't play here (the second also without a round trip) |
| `liveUserBufferInUseMs(user)`, `liveUserBufferFellBack(user)` | any | the jitter buffer that user's stream plays with now (-1 without a stream), and whether it fell back (16.6) |
| `liveUserCounters(user, &c)`, `liveUserEvents(user, max)` | any | the stream's `LiveCounters` (false without a stream) and its latest `LiveEvent`s, the oldest first, at most 64 (16.10); lock-free with the audio |
| `liveRoomRoute()`, `liveRoomRouteText()` | control | the room's route from the latest room message (PROTOCOL.md 15.10): `LiveRoomRoute` {`route` (`LiveRoute::None`, `Direct`, `Relay`), while Relay the `reason` (`LiveRelayReason::NoPath`, `NoDirectClient`, `RelayOnly`) and the users it names, and `direct_possible`}; and one line for a log: `none`, `direct`, or `relay: alice and bob had no direct path` (`relay: alice's client doesn't send directly`, `… sends through the relay only`), with `; everyone can connect directly again` when that is so |
| `setLiveRoomDirect()` | control | sends `!live direct` while connected |
| `liveUserHasDirectPath(user)`, `liveUserProbeRttMs(user)` | any | the direct path to that user holds now (a probe answered within 10 s), and the latest answered probe's round trip on it (-1 for none) |
| `liveRouteName(route)`, `liveEventName(kind)` | free functions | `none`, `direct`, `relay`; the event names of 16.10 |
| `setLiveTestLoss(fraction)` | any | test hook: drop that share of the datagrams received |
| `setLiveTestSendGap(ms)`, `setLiveTestSendHold(ms)` | any | test hooks: the Live data sent in the next `ms` is lost, or held back and then sent all at once with the first datagram after that time |
| `setLiveTestForceRelay(on)` | any | test hook (for benchmarks): no direct path at all; this engine sends and takes Live through the relay only and says so in its `0x87`, so the others reach it through the relay too (and with the relay fallback the room goes to the relay) |
| `setLiveTestBlockPeer(name, blocked)` | any | test hook: nothing goes directly between this engine and that user, either way, as on a network that stops letting it through; the probe rule finds it |
| `liveTestConcealedFrom(user)` | any | test hook: the frame (counted from the first `process()` block) where that user's live audio last went into concealment, -1 for never |
| `liveTestTiming()` | any | one line on the network thread: `the Live network thread: real-time (time-constraint); woken <n> times, the longest wake-up delay <x> ms, <k> over 5 ms; <t> timed waits of 10 ms ran out, the longest <y> ms, <j> over 5 ms past their time` (`user-interactive QoS` or `the least timer slack` where only that took, `its scheduling unchanged` where nothing did) |

An application gives `/live` lines to `liveCommand()` before treating them as
chat. The plug-in doesn't save the buffer setting (its state ignores an old
`live_buffer_ms=` line); an application that does reads it back with
`parseLiveBufferSetting()`. `o11jam` prints the Live lines as `[NAME] live:
…`, passes `--say "/live …"` to `liveCommand()`, and `--no-live` turns Live
off for its engines.

### 16.3 Starting and stopping (Live 6.1)

- `connect()` sets caps bits 16 and 17 when Live is enabled, and each `0x07`
  the client accepts replaces the engine's copy. A `0x09` hands the direct
  paths to the transport (while Live is enabled), and a `0x08` gives the
  room's mode, advice and route (16.7).
- `run()` runs the Live client while the client is logged in and the latest
  `0x07` has UDP open: to the IPv4 address of the TCP connection's server
  end, at the message's UDP port (the TCP port when that is 0). It restarts
  when the peer id, the key, the address or the port changes, and stops when
  UDP open clears, at disconnection, and on a new connection until its first
  `0x07`. Stopping sends the source's `/stop`s before the socket closes.
  Each start is direct only until a room message says otherwise, opens the
  latest `0x09`'s paths afresh, and sends one `0x86` once a `0x09` has come
  on the connection; a `0x87` goes whenever the paths' states change.
- A failed start is said once per connection, `Live mode can't start here
  (<reason>): while the room is Live you hear no one, and no one hears
  you.` (error style), and tried again every 10 s.
- While the host renders offline nothing is sent and Live doesn't play here
  (16.7).

### 16.4 Peers, slots and the receive filter (Live 6.2, 6.3)

- Listed peers, slots and active sinks are as Live 6.2: up to 64 listed, 8
  slots, a peer keeps its slot, freed slots go to unslotted peers in list
  order when a message is applied, and every listed peer is an active sink
  of the source.
- The receive filter is Live 6.3, items 1-6, for what comes through the
  relay and directly alike. A `/start` must be Opus, 1 or 2 channels, 48000
  Hz, with a block of 120, 240 or 480. Also dropped: the OSC forms of
  `/data` and of the data request, blocks split into frames (taken as lost),
  and blocks for a channel onset other than 0.
- Each slot notes the path its stream's blocks come by: the first block's,
  then the other once 4 blocks in a row came by it (a suspect path sends
  both ways). The sink is told (16.6) and `LiveCounters::route` shows it.
- "The host isn't calling the audio callback": no `process()` has started
  within 0.25 s after the end of the last block, or none has run yet.

### 16.5 The source (Live 5.7-5.10, 6.7)

- **The signal** is local channel 0's source input (two inputs when stereo,
  L = R when mono), as captured for upload (5.3): no volume applied.
- **The gate** is open while all of these hold: room on, the Live client
  runs, may send, channel 0 exists with transmit (`broadcast`) on,
  `maxchan` > 0, the host is playing and not rendering offline, and channel
  0's input exists in this block. Closing it sends `/stop` to each sink; opening it
  starts a new stream.
- `process()` copies the signal in chunks of at most 64 frames into the
  capture queue and wakes the network thread. That thread resamples it to 48
  kHz (cubic interpolation, when the host rate isn't 48 kHz) and encodes each
  frame with libopus (stereo, 48 kHz, restricted low delay, the room's
  bitrate clamped to 32-510 kbps, the room's frame size or 240; libopus's
  defaults otherwise). Each packet goes as binary `/data` (flags `0x01`,
  rate 48000.0) to each listed peer directly where the pair's path is up,
  through the relay to a bound peer without a path, and to no one whose path
  isn't up while the room is direct only; when no peer would get it
  directly it goes once to destination 0 (PROTOCOL.md 15.10).
- `/start` goes to each listed peer with the first packet of a stream, to a
  peer listed mid-stream at once, and on a start request; a peer no longer
  listed gets a `/stop`. The source keeps 512 sent blocks (at least 1 s) for
  resends, within the budgets of Live 5.10. It pings each sink every second
  while it streams, and answers pings, start requests and stop requests at
  most once per 100 ms per peer.
- A frame-size change in `0x07` starts a new stream with a new format id; a
  bitrate change applies from the next packet.
- Stream ids and format ids come from a full-period generator over 0..2^31-1,
  seeded from the system's randomness, so they don't repeat within 2^31.

### 16.6 The sinks (Live 5.9, 6.8, 6.9)

- **The jitter buffer**, per stream. **Automatic** (the default): each
  stream starts at 10 ms (`kLiveAutoBufferMs`) and falls back to 20 ms
  (`kLiveFallbackBufferMs`) at its second *late dropout* within 10 s, and
  stays there until the stream starts again (a new `/start`). A late dropout
  is a block that comes after its turn, unplayed, in a concealment that was
  heard; one per concealment. A block lost for good isn't one, nor a block
  that was asked for again (it may be the resend's answer, a round trip
  late), nor one before the stream first played. The fallback takes the
  larger sizes at once; the delay grows by the difference while the audio is
  concealing (a block more of concealment at a time, or the turns moved back
  while the stream is dry or buffering on its delay), at this dropout if it
  still conceals, else at the next; the fill the speed keeps is raised by
  the difference, and what the speed adds meanwhile by playing slower is
  owed no more. **Fixed** (`setLiveBufferMs(ms)`): every stream at `ms`, with
  no fallback; a change to a fixed buffer clears a stream's fallback and the
  late dropouts counted towards one.
- **The buffer in use** *B* = min(200, max(*size*, 5, ceil(2000 × *H* /
  *sr* + 5))) ms, where *size* is the fixed setting, 10, or 20 for a stream that
  fell back, and *H* is the longest host block of the last 30 s. The
  fallback is raised the same way, and an automatic buffer whose raised
  fallback isn't above its raised start has none. Each sink's latency is
  *B* (in blocks, at least ceil(64 × 48000 / (host rate × frame))); its
  capacity is max(2*B*, *B* + 2*H* / *sr* + 5 ms), in blocks as Live 5.9, at
  most 250. A change of the sizes makes only the sinks whose buffer changes
  buffer again.
- **New blocks** follow Live 5.9: old blocks (a block that comes after its
  turn was concealed counts as late, not lost), the wrap (which also counts
  the stream's turns afresh), the far block (a jump only after a second far
  block that follows it), missing blocks. A block beyond the sink's *room*
  from the next block to play is an overrun: the sink keeps only that block
  and buffers afresh. The room is the capacity, plus, while the host catches
  up (below), what came meanwhile, plus 100 ms after a path change (below).
- **Buffering** plays concealment until the sink has waited *B* and holds
  *B*'s blocks (plus what came while the host was behind). It then drops the
  oldest blocks above that number, so that a burst that arrived while it
  waited doesn't add delay (a skip), and plays. The buffer figure (Live 6.10)
  is what it then holds, at most what it waited. After 4 × *B* without
  enough blocks it counts an underrun and waits again. A `/stop` makes it
  play out what it has.
- **Back on its delay.** The sink remembers the delay its stream plays at
  (block *s* plays at a fixed point of its own count). A stream that runs dry
  (nothing at a block's turn) is concealed; when its audio comes again the
  sink counts an underrun and goes back on that delay, so that it stays in
  time with the other players: blocks too late for their turn are dropped
  (and counted late), early ones wait for their turn. A stream more than its
  room ahead of its turn buffers afresh instead. When nothing comes in time
  for its turn for *B* (late for good: its sender's host stopped, say), it
  buffers afresh on a delay of its own and counts an overrun; not while this
  host is behind its schedule. A wrap or a new stream finds its delay
  afresh.
- **Playing.** Each block is decoded when its turn comes (libopus, stereo
  float) and every decoded sample is clamped to −2..+2, NaN to −2. A block
  missing at its turn (lost for good, unless it comes later), an xrun block,
  a dry stream and buffering play concealment.
- **Concealment**, in place of Opus's own (which at 5 ms frames fades a held
  note within 40 ms): linear prediction, each sample from the 24 before it
  and 8 further back (32, 48, 64, 96, 128, 192, 256 and 384 samples), fitted
  by least squares (the diagonal raised by 1e-4 of its mean) to the last 60
  ms of the stream's best guess, one fit for both channels, to their mid (or
  their side, where that is the louder). A steady sound goes on at its level
  and in its phase, a decaying one decays, noise dies away, and nothing comes
  out louder than the last 5 ms played (0.5 dB aside). After (4 × *B* +
  20 + 10) ms of it (what the mixer holds and fades, 16.8) it ends in silence
  over 5 ms, though the prediction goes on unheard for 1 s more: the best
  guess, which is fitted to and primes the decoder, is the prediction and the
  audio decoded before it is crossfaded, not what was heard. Before the
  first block after concealment is decoded, the sink **primes its
  decoder**: an encoder of its own (restricted low delay, 256 kbps,
  complexity 5) codes whole blocks covering 1024 + 2 × its lookahead samples
  (at most 1920): the last of the best guess, and a lookahead's worth of the
  prediction that would follow. The reset decoder decodes them, and then the
  block, so that the stream comes back at its level rather than with a burst
  or a dip. Not on a stream's first block while the decoder is as the stream
  began (sequence 0). The decoded audio crossfades from the concealment over
  5 ms (a raised cosine).
- **Clock drift.** The 48 kHz stream is resampled to the host rate by cubic
  (Catmull-Rom) interpolation whose step follows the buffer. The fill
  (blocks held plus decoded frames not yet played) is smoothed over 0.5 s;
  after 1 s of playing its level (with what a fallback still owes) is kept
  as the reference, for the stream's life: after the stream buffers again
  (it overflowed, or was late for good) the speed brings the delay back to
  it, and only a new stream or a new buffer setting takes a new one. While
  the smoothed fill is more than 2 ms from that reference, the step is the
  nominal one × (1 + 0.1/s × the distance beyond the 2 ms), limited to ±0.5
  %; within them it is the nominal step. So the stream plays at its own
  pitch, and only a real difference between the two machines' clocks moves
  the step. This takes the place of AOO's time DLL (`engine/NOTES.md`).
- **The host's schedule.** From a late block through the early ones after
  it (a host whose blocks stall and then come back to back to catch up),
  `process()` knows how far behind its schedule it is, at most 1 s (a
  tolerance of max(1 ms, half a block)); a block on time, or a second late
  one that isn't early, takes the schedule up afresh. While it is behind,
  each sink holds the stream that arrived meanwhile without its being any
  later: room for it, not counted as fill (the fill's estimate holds), not
  a reason to buffer afresh or to be late for good.
- **Host gaps.** A block that starts more than max(20 ms, 2*H* / *sr*) after
  the previous one ended is noted; at the next block that isn't early (that
  starts at least 90 % of the previous block's length after it), if the host
  is still that far behind, the gap (at most 0.5 s) is reported: the source
  sends the xrun blocks of Live 5.4, and every sink, before that block's
  arrivals, skips the rest of the block it decoded last and whole blocks to
  the nearest of the time missed, so its delay and fill stay. A host that
  catches up in time reports nothing.
- **Path changes** (all to all). When a stream's blocks come by the other
  path (16.4), the sink has 100 ms more room for at least 1 s, until it holds
  less than its capacity again: a shorter path brings the blocks early, and
  the speed plays them back to the fill it keeps. A longer path brings them
  late, as any path that grows longer does, and the stream keeps its delay
  as after a gap, until it is late for good.
- **Requests.** At every audio block a sink asks for its missing blocks but
  the newest, at most 16 per check, each one again after 10 ms. The network
  thread sends at most one request message per slot per 10 ms (up to 32
  requests). A sink pings its source every second from the first block of a
  stream, sends a stop request after 1 s without audio and every 1 s after
  that, and drops a source silent for 5 s.
- **After a stream.** When a stream ends (`/stop`, the timeout) or is dropped
  (its slot's generation changed), what it holds plays out, then concealment
  for 4 × *B* + 20 + 10 ms, for the mixer to fade out (16.8). Of each
  rendered block the sink says the first frame of its stream's own audio
  (`firstReal()`) and the frame where the concealment it ends with began
  (`quietFrom()`).
- Sinks play only while the room is on and the Live client runs, and only
  for host blocks of at most 16384 frames; longer blocks get no Live audio.

### 16.7 The room's mode, the report and the advice (PROTOCOL.md 15.11)

- **Live plays here** while the room's switch is on (the latest `0x07`), the
  Live client runs and the server has answered its hellos, it may send (a
  place among the senders), and the host isn't rendering offline.
- **The mode** is the latest room message's (`0x08`). The mixer takes it at
  the first interval boundary at least 0.5 s after the message came, for
  every user at once (`liveBoundary()`, before the uploads of 6.3 and the
  steps of 8.2): from there the room plays Live while Live plays here. When
  Live stops playing here (the switch goes off, the Live client stops, ...),
  the mixer plays the room on intervals at once, from that block.
  `liveRoomLive()` says the room plays Live now.
- **No interval audio while Live.** While the mode the mixer took is Live
  and the switch is on (`liveHoldsIntervals()`), even when Live can't play
  here, nothing is uploaded: at the boundary where it begins, each channel
  that was transmitting ends its interval and sends one silent interval (a
  zero-GUID BEGIN); at the first boundary after, the channel starts again
  with a fresh encoder (nothing carried over) and its first interval fades in
  over 20 ms (`kLiveResumeFadeSeconds`). The mixer plays no interval audio
  while Live (16.8), so a user without a Live stream here isn't heard. After
  `!live off` the intervals come from the players' first interval after
  Live: one to two intervals of silence.
- **The report** (`0x85`), worked out at every `run()` pass once a `0x07` has
  come, is sent when it changes, at most every 250 ms. Sending: its source's
  gate is open (16.5). Can't Live (fault: its own id): its Live client
  doesn't run, or a listed peer that sends has no slot here. Otherwise clean
  when every listed peer that sends (by its `0x07` flag) has a healthy slot:
  its sink has played for at least 1 s without buffering again, there were
  at most 2 dropouts (underruns, overruns and xrun gaps) in the last 2 s, and
  at most floor(2880 / frame size) blocks were lost for good in the last 2 s
  (counted in 100 ms buckets). Once clean it stays so until a slot has more
  than 2 dropouts in 2 s, passes the loss limit, or has played no audio for
  more than 1 s while its peer still sends and no `/stop` came. The fault is
  the one peer that isn't clean, or its own id when several aren't. A peer
  that doesn't send is no one's fault.
- **Notices.** When the mixer takes Live: `The room is Live now, about <N> ms
  late.`, with *N* from `liveHeardMs()`, waiting up to 1.5 s
  (`kLiveNoticeWaitMs`) for a stream that plays, else `The room is Live
  now.`; none when the room leaves Live (the server's chat line says so).
  The advice, when it names someone, and again when more players have
  trouble: `<U>'s connection has dropouts`, `<U> can't play Live and isn't
  heard while the room is Live`, `Your connection has dropouts` or `Your
  client can't play Live, so while the room is Live you hear no one, and no
  one hears you`, then `, and <n> more player has trouble` (`players have`)
  when the count is above 1, then `: Intervals may sound better (switch with
  /live off).`. When the named player's trouble is over while they are still
  here and the switch is on: `<U>'s connection is clean again.` or `<U> can
  play Live now.` (`Your connection is clean again.`, `Your client can play
  Live now.`).

### 16.8 Mixing (Live 6.6)

- `mixLive()` plays channel 0 of every user once the server has Live (each
  user then has a mixer, and the remote channels of 8.5 leave that channel
  out): the user's interval audio and its slot's live audio, each through a
  short delay line, with channel 0's volume, pan, mute and solo, the pan law
  of 5.4, the mono rule (both sides at half volume) and the double clip of
  Live 6.6. Channel 0's peak meter follows what is heard.
- The path the room's mode picks is heard: the live one while the room is
  Live (16.7), the interval one otherwise; while Live the interval path is
  silent. Each change crossfades over 20 ms (`kLiveFadeMs`) on an
  equal-power curve, gain = sin(π/2 × *s*) with *s* = 0.5 − 0.5 cos(π *q*) of
  the fade position *q*, so that the slope has no step at either end.
- **Lining up.** Before a path fades in against the other, the last 8 ms of
  the outgoing one are compared with the incoming one at lags up to 4 ms. At
  a correlation of 0.8 or more the incoming path is read at the shortest lag
  within 0.02 of the best, so that one held note heard an interval apart and
  live lines up; that correlation *ρ* (or the one at lag 0, below 0.8) also
  scales both gains by sqrt(*e* / (*e* + 2*ρ g₀ g₁*)), *e* = *g₀*² + *g₁*², so
  that two alike sounds keep their level too. While the room is Live the live
  path keeps its delay, also through a slot's fade-out and fade-in, so a
  slot that comes back after a gap is lined up as it was.
- **Gaps.** While the room is Live, a slot without its stream's audio (lost,
  dry, buffering, stopped, timed out) is heard as its concealment (16.6) for
  4 × the buffer that slot's stream plays with (`kLiveConcealBuffers`; its
  fallback if it fell back) plus the live path's delay up to 10 ms
  (`kLiveConcealSpareMs`), counted from the frame where the concealment is
  heard to begin (the sink's `quietFrom()`, through the delay line), so that
  every gap is held equally long; then it fades out over 20 ms, and fades in
  again when its audio comes back. A block lost for good counts as
  concealment.
- `userLive()` is the mixer's state: it plays the live path for that user,
  from a slot.

### 16.9 `/live` and the other lines (Live 6.10-6.12)

- `liveCommand()` answers, with "O11 Session" where the reference names its
  own product:
  - `/live on|off`: sent as `!live on|off`; not connected: `Not connected:
    Live mode is the room's, so join a room first.` (error style).
  - `/live buffer`: `Live's jitter buffer is automatic: 10 ms, or 20 ms for a
    player whose audio comes late. Type /live buffer 30 to fix it (5 to
    200).`, or with a fixed buffer `Live's jitter buffer is <N> ms. Type
    /live buffer 30 to change it (5 to 200), or /live buffer auto.`
  - `/live buffer auto` and `/live buffer <N>` (5-200): `Live's jitter buffer
    is automatic now (it was <old>): 10 ms, or 20 ms for a player whose
    audio comes late.` or `Live's jitter buffer is <N> ms now (it was
    <old>). Less is less late, more has fewer dropouts: type /live to see
    them.`, with a sentence on the DAW's buffer when long host blocks raise
    it; anything else, `/live buffer takes a number of ms from 5 to 200, such
    as /live buffer 30, or auto.` (error style). `/live buffer` works without
    a connection.
  - any other argument: `Type /live to see how the Live players sound, or
    /live buffer 30 to set Live's jitter buffer (ms).` (error style).
- `/live` alone, not connected, says what it would show and the buffer
  setting; on a server without Live, with its Live off, in a room switched
  off or where the Live client couldn't start, one line says so (`This
  server doesn't have Live mode, so everyone plays through intervals.`, …).
  Otherwise, in this order:
  1. the room: `The room is Live.`, `The room goes Live at the next
     interval.`, `The room is Live, but Live can't play here, so you hear no
     one, and no one hears you.` or `The room is on intervals.`, then the
     advice's text (16.7) while there is advice;
  2. `Live is on. Jitter buffer automatic: <B> ms, or <F> ms for a player
     whose audio comes late (/live buffer N to fix it). ` (`Live is on.
     Jitter buffer <B> ms (/live buffer N to change it). ` for a fixed
     buffer, or one whose fallback isn't larger), the sentences on the DAW's
     buffer when long host blocks raise it, `Round trip to the server <r>
     ms.` or `The server doesn't answer over UDP yet.`, and a sentence when
     it may not send, Transmit is off or the host is stopped;
  3. one line per listed peer: `live`, or `not heard` while the room is Live
     (`intervals` while it isn't), with the Live spec's reasons and health
     words, the 6.10 estimate and loss, `; their audio came late, so their
     buffer is <F> ms` for a stream that fell back, and the dropouts;
  4. everyone else: `<U>: not heard while the room is Live (not a Live
     player: an older client, or the room is full)` (`intervals (…)` while
     it isn't).
- The statistics behind `/live` are Live 6.10's: the sink's smoothed round
  trip from its pings, the buffer figure, the codec latency (the `/start`'s
  latency and codec delay, after its clamps, plus the sink's own), the
  one-way estimate, the loss since the stream last went active, and the
  dropout counts since the slot was assigned. The dropout count is the
  reference's (underruns + overruns + blocks lost for good); after a buffer
  change while Live runs, `/live` also counts those since the change.
- The offer and the no-answer line of Live 6.12 are said once per connection
  (the no-answer line again after a restart for a new id, key, address or
  port, 5 s after the start): `This server has Live mode: type !live on, and
  the O11 Session players in the room hear each other in real time (it needs
  UDP to the server's port <p>).` and `Live: the server doesn't answer on UDP
  port <p>, so while the room is Live you hear no one, and no one hears you.
  A firewall or router may block UDP; whoever hosts the server needs UDP port
  <p> forwarded too.` (error style).

### 16.10 Counters, events and constants

**`LiveCounters`**, per user, for its stream since the user's slot got it
(blocks are the stream's frames, 5 ms at the default; frames are at 48 kHz):
`late` (blocks that came after their turn, unplayed: a block concealed as
lost that comes later counts here, not as lost), `lost` (missing at their
turn and not come so far), `concealed_frames` (concealment played once the
stream had played), `underruns` (times it ran dry), `overflows` (times it had
no room, or was late for good, and buffered afresh), `xrun_gaps` (the
sender's host missed time), `resent` (blocks that came when asked for again),
`skips` and `skipped_frames` (jumps when it buffered afresh), `host_gaps` and
`host_gap_frames` (this host's own gaps, the stream skipped by as much),
`speed_changes`, `speed_moved_ms` and `speed_ppm` (the speed leaving the
clock's own to bring the fill back: how often, how far it moved the stream in
all, + ahead, and the speed now, + faster), `fill_ms` (as
`liveUserBufferMs()`), `buffer_ms` and `fell_back` (the buffer it plays with
now), `route` (the path its blocks come by now; `None` before one).

**`LiveEvent`s**, a ring of the stream's latest 64, each with its
`std::chrono::steady_clock` time (ns) and a value: `stream` (a new stream:
its first sequence), `play` (playback began after buffering: µs buffered),
`late` (µs after its turn), `lost` and `dry` (concealed: the sequence),
`rejoin` (back on its delay after running dry: the block played),
`overflow` (no room, buffers afresh: the sequence), `rebuffer` (late for good
after running dry, buffers afresh), `skip` (frames), `host gap` (frames),
`xrun` (the sequence), `speed on` (the fill's error, µs) and `speed off` (the
speed has been the clock's own for 1 s: the furthest it went, ppm), so one
speed correction is one pair; `end`, `fallback` (the buffer now, ms) and
`route` (a `LiveRoute`; the first with the stream).

| Name | Value |
|---|---|
| slots / listed peers | 8 / 64 |
| jitter buffer | automatic: 10 ms, 20 ms after 2 late dropouts in 10 s (`kLiveAutoBufferMs`, `kLiveFallbackBufferMs`); fixed: 5-200 ms |
| sink ring / largest capacity | 256 / 250 blocks |
| longest host block played live | 16384 frames |
| source history | 512 blocks |
| resend budgets | 0.5 × blocks/s for all (burst 32), 0.25 × blocks/s per sink (burst 16) |
| resend check | each audio block, ≤ 16 blocks, again after 10 ms; ≤ 1 message per slot per 10 ms |
| pings / answer limit | 1 s / 100 ms per peer |
| stop requests / source timeout | after 1 s silent, every 1 s / 5 s |
| host not running | 0.25 s after the end of the last block |
| network thread's wait | 10 ms (`kLiveNetworkWaitMs`; 1 ms on Windows) |
| health | active ≥ 1 s; ≤ 2 dropouts and ≤ floor(2880 / frame) lost in 2 s; once clean, also ≤ 1 s without audio from a sender |
| report | when it changes, ≤ every 250 ms |
| room message to the mixer | the first boundary ≥ 0.5 s after it came |
| crossfades and fades | 20 ms (`kLiveFadeMs`), equal power; resumed uploads fade in over 20 ms (`kLiveResumeFadeSeconds`) |
| gap held | 4 × the stream's buffer (`kLiveConcealBuffers`) + the line-up delay up to 10 ms (`kLiveConcealSpareMs`) |
| line-up | 8 ms compared, lags ≤ 4 ms, correlation ≥ 0.8 |
| concealment | 24 + 8 lags, least squares over 60 ms, load 1e-4; (4 × *B* + 20 + 10) ms heard, 5 ms to silence, 1 s unheard; 5 ms crossfade back |
| decoder primer | restricted low delay, 256 kbps, complexity 5; 1024 + 2 × lookahead samples, ≤ 1920 |
| drift control | fill smoothed over 0.5 s, reference after 1 s, dead band ±2 ms, 0.1 per second beyond it, at most ±0.5 % |
| host schedule | tolerance max(1 ms, half a block), at most 1 s behind |
| host-gap threshold | max(20 ms, 2*H* / *sr*), early below 90 %, reported ≤ 0.5 s |
| path change | 100 ms more room for ≥ 1 s; a change after 4 blocks in a row |
| decoded sample clamp | −2.0..+2.0 |
| failed start retry / no-answer line | 10 s / 5 s |
| "The room is Live now" waits | ≤ 1.5 s (`kLiveNoticeWaitMs`) |

### 16.11 Tests

- `test_engine_live_units`: message sizes and bytes (`/start` 116, `/stop`
  44, pings 36 and 32, pongs 56, the requests), the receive filter's
  parsing, the version rule, the `/start` clamps and format checks, binary
  `/data` and xrun blocks, the resend request (the Live spec's example), the
  sink's sizes (Live 5.9's examples and test 39), the far block (test 41),
  overruns and the wrap, missing blocks and their requests, an underrun and
  the buffer figure after a burst (test 43), the counters and the event log,
  the automatic buffer's fallback on late audio (and not on loss), the delay
  kept after a gap, a path change with a late block, the frame where
  concealment begins, `/stop` and reset, the concealment after a stream, a
  sender clock 200 ppm fast or slow for 150 s (no underrun or overrun, the
  fill within a block of where it settled), the decoder clamp (test 40),
  display names, the buffer setting's parsing, `/live` without a connection
  and the texts for long host blocks.
- `test_engine_live`: two engines through `o11server` with Live on, in real
  time (an audio thread calls `process()` every 270 frames at 48 kHz, tone
  bursts at uneven gaps on channel 0): both hear each other live at low
  latency with the room's line, and no burst is heard twice; the room's
  line waits for its estimate; `!live off` takes the mixer to intervals at
  once, with their audio from the players' first interval after Live, and
  `!live on` brings Live back at a boundary; a sender's stall of 1.5 s and
  packet loss leave the room Live and give both players advice naming the
  sender, which clears once it has been clean for 10 s; `/live buffer 60`
  raises the delay; host stalls that catch up keep every delay to the frame;
  and a long run on clocks 200 ppm apart with stalls and 300 ms outages keeps
  each delay within 1 ms of where it was.
- `test_engine_live_room` (the room through `o11server` in real time, a
  steady sine per sender, each listener measured around every switch: the
  largest step against the steady tone's, the longest near-silence, the 10
  ms RMS against the steady RMS): seamless switches at 48 and 44.1 kHz with
  Opus and FLAC intervals; a room of four switching together; a stopped DAW
  gives no advice; a client without Live in a Live room neither hears nor is
  heard and is "can't play Live"; gaps give advice and the room stays Live;
  a slot lost mid-interval; the accessors for a UI; and the listening file,
  which writes `live-transitions.wav` and a timeline (`.txt`) of what one
  listener hears of two players' synthesized 909-style drums (each bar an
  interval) across the switches, gaps, a client without Live and `!live off`,
  with a click check against the drums played straight and the equal hold of
  every gap (`O11PROTO_LISTENING=tones` plays steady tones instead,
  `live-transitions-tones.wav`).
- `test_engine_live_tone`: two engines on clocks of their own, one `ppm`
  faster, each playing a steady sine (1 kHz and 750 Hz) and recording the
  other: no dip in the tone's level (5 %) or bend in its phase (0.05 rad)
  beyond steady playback's, with the dips shown against the listener's event
  log; the automatic buffer stays at 10 ms on a clean stream, falls back
  once on a jittery one (the link held back every 0.5-1.5 s with
  `setLiveTestSendHold()`), ignores loss, and a fixed buffer never falls
  back.
- `test_engine_live_paths`: three players, all to all through `o11server`,
  each playing a steady sine: all to all on loopback; a pair without a path
  can't hear each other and gets the advice; the relay fallback takes the
  room to the relay and `!live direct` back; a forced relay with and without
  the fallback. Over whole spans, across the route changes, each listener's
  output stays within 1 dB of the span's RMS every 10 ms, with no
  near-silence over 1 ms and no step over 1.5 times a steady mix's largest.
- The harness (`live_harness.h`) and its hiccup rules are in
  `engine/NOTES.md`.
