# NINJAM client engine specification

Revision 1, 2026-10-16. Companion to `PROTOCOL.md` (revision 2a), which this
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
A later revision may add lossless local capture and the Live (UDP) mode.

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

The engine has no threads of its own.

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
increments `loopCount`. A boundary falls exactly when `intervalPos ==
intervalLength`; the first boundary is at the start of the first running block.

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

**Opus** (fourcc `OPUS`, Ogg Opus as in RFC 7845):

- Encoder at 48000 Hz, 1 or 2 channels, application "audio", complexity 10,
  signal type music, constrained VBR at the interval's bitrate. The encoder's
  lookahead becomes the `pre_skip` of `OpusHead`; `OpusHead` carries the
  *input* sample rate, output gain 0, channel mapping family 0. `OpusTags`
  has a vendor string and no comments. Each is its own Ogg page; the first has
  the beginning-of-stream flag.
- Frames of 960 samples (20 ms). Each packet's granule position is the number
  of 48 kHz samples encoded so far including the pre-skip, except the last
  (below).
- Input at another rate is resampled to 48000 Hz with a windowed-sinc (or
  equally good) resampler whose state **persists across intervals** of the
  same encoder, so the stream of one interval begins exactly where the
  previous ended. The Opus encoder state itself is reset at each new stream.
- **End of interval:** the real audio ends at `real` samples (after
  resampling). The stream is padded up to the next multiple of 960 of `real +
  pre_skip`, so the encoder's lookahead is flushed and the last frame is
  whole. The padding is not silence: it is a linear-prediction extrapolation
  of the audio (order 24, fitted to the last 1024 samples of each channel,
  with a mild lag window and bandwidth expansion for stability), kept at full
  level for the first 128 samples and then faded out with a raised cosine over
  the rest. The last packet's granule position is `real + pre_skip` and
  carries the end-of-stream flag, so a decoder trims the padding; a NINJAM
  decoder uses the trimmed part for the crossfade (section 8.3). The reference
  takes its predictor fit from libvorbis' `lpc.c` (BSD); any Levinson-Durbin
  fit of the same order on the same data serves.
- Output bytes are the Ogg pages, as the Ogg library produces them (pages are
  flushed after the headers and at the end; otherwise written when the
  library decides a page is full).

**FLAC** (fourcc `FLAC`, native FLAC, no Ogg):

- 16-bit samples at the engine's sample rate (no resampling), 1 or 2
  channels, compression level 5, verify off, the "streamable subset" when the
  sample rate allows it, else not. The stream is written as it is encoded, so
  STREAMINFO's total sample count stays 0.
- Float to 16-bit: exact zero (and NaN) stays exactly 0, so digital silence
  compresses to almost nothing; any other value is scaled by 32768, gets TPDF
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
mute, solo, output pair, flags, name, and the peak meter. Setting
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
put in the channel's interval queue. The buffer keeps receiving later WRITEs
while the decoder reads from it, so decoding streams; the end flag just marks
that no more will come. A download with the end flag is forgotten after this
(its buffer lives on with the decoder).

The **interval queue** of a channel holds at most **two** entries: slot 0 and
slot 1, each a decoded-interval-in-progress or a silent interval. A new entry
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
fail: it decodes what it can and resumes when more arrives.

**Opus:** Ogg pages are demultiplexed; `OpusHead` gives channels, pre-skip
and output gain (applied as 10^(gain/(20×256))); packets after the two header
packets are decoded at 48000 Hz; the first `pre_skip` samples are dropped. On
the page with the end-of-stream flag, samples past the page's granule position
are not output but kept as the lapping frames. A stream with channel mapping
family ≠ 0 or more than two channels is bad and decodes to nothing.

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
5. reset the channel's drop count (section 8.3);
6. if there is a new current interval with a decoder, apply the **fade-in**
   (section 8.3).

A silent interval as the current interval plays nothing.

### 8.3 Crossfade

The fade-out (≤ 128 frames) of the previous interval is mixed into the first
frames of the new one: the new decoder is primed until it has at least that
many frames (or the data is exhausted), then for i = 0..n-1 (n = fade length),
with s = (i+1)/n: `new[i] = new[i] × s + old[i] × (1-s)`, per channel, only
when both have the same channel count. The crossfade hides the small
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
  the last piece;
- decode until `available() > needed`, or the stream is exhausted;
- if the channel has a **drop count** (section 8.2, below): skip that many
  frames first (as many as are available), reducing the count;
- if `available() ≥ needed`: unless muted or volume < 10⁻⁷, clip the source
  samples to -1..1 in place, update the peak meter from them, and add them to
  the output pair with the pan law of section 5.4, resampling by linear
  interpolation when the rates differ (an implementation may use a better
  resampler; the reference uses linear). A mono source goes to both outputs;
  a stereo source into a mono output is summed at half volume. Each output
  sample is clipped to -1..1 before being added. Then skip `needed` frames.
- else (the stream is late or short): skip everything available and add the
  shortfall `needed - available()` to the channel's drop count, so that when
  the data arrives the stream catches up by dropping those frames instead of
  playing late. Output nothing for this piece.

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
(section 3.6) except `SESSION`, which is dropped.

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
  or -1), channel name/subscribed/volume/pan/mute/solo/outChannel/flags, peaks;
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
| Opus padding | LPC order 24 from 1024 samples, 128 full-level |
| FLAC | 16 bit, level 5, TPDF 1 LSB |
| meter decay | -36 dB/s |
| default local bitrate | 64 kbps |
| default remote channel volume | 0.25 |
| default metronome volume | 0.5 |
| drop limit for a FLAC gap | 5 s |

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
   decodes bit-exactly; digital silence of 2 s encodes to under 200 bytes;
   decoding from data delivered in chunks of 700 bytes gives the same output
   as from the whole stream.
4. Float conversion: 0.0 → 0, 1.0 → 32767, -1.0 → -32768; a constant 0.5
   dithers to 16384 ± 1.

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

**Against servers (end to end, needs `njserver` from this repository and may
also be run against the reference server)**
10. Two engines in one process, 48000 Hz, 512-frame blocks, a 997 Hz tone of
    0.5 amplitude on one, the other subscribed: after 12 s the listener's last
    two intervals have SNR ≥ 35 dB with Opus at the room's 128 kbps and the
    tone's peak within 0.45..0.55; the same room switched with `!codec flac`
    gives SNR ≥ 60 dB.
11. The same at 44100 Hz on both sides, Opus: SNR ≥ 30 dB.
12. Transmit off then on: the listener gets a zero-GUID BEGIN once, then
    whole intervals again; while off, the listener's output for that channel
    is exactly 0 after the current interval ends.
13. A listener whose `process()` is not called for 20 s and then resumes plays
    the newest intervals, not the stale ones (the queue depth of 2 holds).
14. Against the reference client (`ninjam/tests/njclient_e2e` sender or
    listener, run by a tester who may read both): engine listener with
    reference sender and the reverse both reach SNR ≥ 35 dB with Opus.

## 14. Deliverables and dependencies

- A library on top of the njproto client (`include/njproto/engine.h` and
  friends, in the same CMake project, target `njproto_engine`), C++17, no
  dependency besides the standard library, POSIX sockets, **libopus, libogg
  and libFLAC** (all BSD-licensed; found with `find_package`/`pkg-config`,
  with a CMake option to build the engine at all so the protocol library still
  builds without them).
- Unit tests for sections 13.1-13.9 under `ctest`.
- A command-line tool `njjam` for the end-to-end tests and for interoperability
  testing by someone else: `njjam host:port --user NAME [--tone HZ | --wav
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
