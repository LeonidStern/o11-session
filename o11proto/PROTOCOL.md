# NINJAM protocol specification

Revision 2c, 2026-10-17 (revision 1 plus the exact server texts and defaults found by running a clean-room implementation against the reference; see section 14). Written from the behaviour of the NINJAM server and client in the Jamsa repository (`LeonidStern/Jamsa`, commit `e040ae1`), including the Jamsa extensions (codec negotiation and the room bitrate). It describes what goes over the wire and how each side must behave; it contains no code. It is meant to be enough to write a new client or server from scratch that works with existing NINJAM clients (ReaNINJAM, JamTaba, JamWide, the original NINJAM clients) and with the Jamsa server and plug-in.

Where this spec says "Jamsa extension", original NINJAM software doesn't know the feature and ignores it. Everything else is the protocol as Cockos defined it in 2005.

## 1. Conventions

- All integers are **little-endian**. `u8`, `u16`, `u32` are unsigned; `s8`, `s16` are two's complement.
- A **string** is a run of bytes ending in a single `0x00`. Strings are UTF-8 by convention (the server never validates the encoding). An empty string is just the `0x00`.
- A **GUID** is 16 opaque bytes. Clients create them at random; there is no structure. When shown as text (in chat or log files) it is 32 lowercase hex digits.
- A **fourcc** is a `u32` whose four bytes, in wire order, are ASCII characters. The codec `OGGv` is the bytes `'O' 'G' 'G' 'v'`.
- "Interval" means one block of music of BPI beats at the room's BPM. "Channel" means one audio stream of one user (mono or stereo). "Subscriber" means a user who asked to receive a channel.

## 2. Transport and framing

- **TCP**, one connection per client, default port **2049**. Both sides set `TCP_NODELAY`.
- Every message is a 5-byte header followed by a payload:

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | message type |
| 1 | u32 | payload length (bytes) |
| 5 | ... | payload |

- A payload longer than **16384** bytes is a protocol error; the receiving side closes the connection. Type `0xFF` is invalid.
- Messages are processed strictly in order. There are no sequence numbers and no acknowledgements beyond what the messages below say.
- An implementation queues outgoing messages. The reference keeps at most 512 queued messages per connection; if the queue overflows it closes the connection.

### Keepalive and timeouts

| Type | Direction | Payload |
|---|---|---|
| `0xFD` KEEPALIVE | both | empty |

The server states a keepalive interval *k* (seconds) in its first message (section 4.1); 0 means the default of **3 s**. Each side sends KEEPALIVE whenever it has sent nothing for *k* seconds, and closes the connection when it has received nothing (any message, keepalives included) for **3 × k** seconds. While a licence text is being shown (section 4.1) both sides use *k* = 45 s.

Type `0xFE` is reserved ("extended") and never sent.

## 3. Message types

| Type | Name | Direction | Section |
|---|---|---|---|
| `0x00` | SERVER_AUTH_CHALLENGE | server → client | 4.1 |
| `0x01` | SERVER_AUTH_REPLY | server → client | 4.3 |
| `0x02` | SERVER_CONFIG_CHANGE_NOTIFY | server → client | 5 |
| `0x03` | SERVER_USERINFO_CHANGE_NOTIFY | server → client | 6.2 |
| `0x04` | SERVER_DOWNLOAD_INTERVAL_BEGIN | server → client | 7.4 |
| `0x05` | SERVER_DOWNLOAD_INTERVAL_WRITE | server → client | 7.4 |
| `0x06` | SERVER_CODEC_MASK (Jamsa extension) | server → client | 8 |
| `0x07` | SERVER_LIVE (Live mode, O11 Session extension) | server → client | 15 |
| `0x08` | SERVER_LIVE_ROOM (Live mode) | server → client | 15.11 |
| `0x09` | SERVER_LIVE_PATHS (Live mode) | server → client | 15.10 |
| `0x80` | CLIENT_AUTH_USER | client → server | 4.2 |
| `0x81` | CLIENT_SET_USERMASK | client → server | 6.3 |
| `0x82` | CLIENT_SET_CHANNEL_INFO | client → server | 6.1 |
| `0x83` | CLIENT_UPLOAD_INTERVAL_BEGIN | client → server | 7.2 |
| `0x84` | CLIENT_UPLOAD_INTERVAL_WRITE | client → server | 7.2 |
| `0x85` | CLIENT_LIVE_REPORT (Live mode) | client → server | 15.11 |
| `0x86` | CLIENT_LIVE_LAN (Live mode) | client → server | 15.10 |
| `0x87` | CLIENT_LIVE_PATHS (Live mode) | client → server | 15.10 |
| `0xC0` | CHAT_MESSAGE | both | 9 |
| `0xFD` | KEEPALIVE | both | 2 |

A receiver ignores types it doesn't know. The server ignores every message except `0x80` until the client is logged in.

## 4. Connecting and logging in

Sequence: the server speaks first.

```
client                                   server
  |  <-- 0x00 AUTH_CHALLENGE               |
  |  --> 0x80 AUTH_USER                    |
  |  <-- 0x01 AUTH_REPLY (success)         |
  |  <-- 0x02 CONFIG_CHANGE (BPM, BPI)     |
  |  <-- 0x03 USERINFO (everyone's channels)
  |  <-- 0xC0 PRIVMSG * <motd line>  (0 or more)
  |  <-- 0xC0 TOPIC "" <topic>             |
  |  --> 0x82 SET_CHANNEL_INFO             |   (server tells the others: JOIN, USERINFO)
  |  --> 0x81 SET_USERMASK  (per user, as users appear)
  |  <--> intervals, chat, keepalives ...  |
```

### 4.1 SERVER_AUTH_CHALLENGE (`0x00`)

| Size | Field |
|---|---|
| 8 | challenge: random bytes, new for every connection |
| u32 | server_caps |
| u32 | protocol_version: `0x00020000` |
| string | licence text, present only when server_caps bit 0 is set |

`server_caps`: bit 0 = a licence text follows and the client must agree to it; bits 8-15 = keepalive interval in seconds (0 = default 3). **Never set bits 16-31**: JamTaba reads the keepalive as `server_caps >> 8` without masking, so higher bits would break it. (JamWide uses bit 1 for a purpose of its own; leave it clear.)

A client accepts `protocol_version` from `0x00020000` up to but excluding `0x0002ffff`, else it disconnects.

### 4.2 CLIENT_AUTH_USER (`0x80`)

| Size | Field |
|---|---|
| 20 | passhash |
| string | username |
| u32 | client_caps |
| u32 | client_version: `0x00020000` |

`passhash = SHA1( SHA1( username + ":" + password ) + challenge )`, where `+` is byte concatenation, `username` and `password` are the raw bytes the user typed (no terminator), and `challenge` is the 8 bytes from the server. For anonymous logins the password is the empty string (the hash is still sent; the server ignores it).

`client_caps`: bit 0 = the user agreed to the licence (required when the server sent one; the server refuses otherwise). Bits 8-15 are codec capabilities (Jamsa extension, section 8): `0x100` = can decode Opus, `0x200` = can decode FLAC. Bit 16 (`0x10000`) = can play Live mode (section 15). All other bits must be 0 (JamWide uses bit 2 for a purpose of its own).

The server accepts `client_version` from `0x00020000` to `0x0002ffff` inclusive.

**Usernames.** A username is `anonymous` or `anonymous:<name>` for an anonymous login, or a name from the server's user list. The server rewrites anonymous names: `<name>` is cut to 16 characters, `@` and `.` become `_`, then `@<client IP>` is appended (the IP's last octet replaced by `x` when the server masks addresses); a bare `anonymous` becomes `anon@<IP>`. Any character in the final name that is not a letter, digit, `-`, `_`, `@` or `.` becomes `_`, and the name is cut to 128 characters. The client learns its effective name from the AUTH_REPLY.

If a user with the same name (case-insensitive) is already logged in, the server disconnects the old connection (sending PART and channel removals to the others), unless the account has the "multiple logins" privilege, in which case the new login gets `.2`, `.3`, ... appended (at most 16 of them). This replacement happens **before** the capacity check below. Anonymous users have the multiple-logins privilege only when the server is configured for it (`AnonymousUsers 2` in the reference); by default a second anonymous login with the same name replaces the first.

**Status accounts.** A server may have one status user. A login as that user gets the user list, a CONFIG_CHANGE, the TOPIC and a `USERCOUNT` chat line, and is then disconnected without an AUTH_REPLY. It's a way to poll a server.

### 4.3 SERVER_AUTH_REPLY (`0x01`)

| Size | Field |
|---|---|
| u8 | flag: bit 0 = success |
| string | on success: the client's effective username; on failure: the reason (optional) |
| u8 | maxchan: the number of channels this client may use (0-32; 0 in a lobby). Present whenever the string is; meaningless on failure |

On failure the server sends the reply and closes the connection. Reasons in use: `invalid login/password`, `server full`, `invalid authorization reply`, `incorrect client version`, `license not agreed to`, `authorization timeout`, and (Jamsa) `this server needs a NINJAM client that plays Opus` (or FLAC).

The server waits **30 s** for AUTH_USER (120 s when it sent a licence), then sends `authorization timeout` and closes. A server also limits connections that haven't logged in yet (the reference: 64 in total, 8 per address) and may deny or reserve addresses by an access list; a denied address is simply closed.

**Capacity.** A server has a maximum user count. A login counts the other logged-in, non-hidden users; if that already reaches the maximum, the reply is `server full`, unless the connection comes from a reserved address or the account has the reserve privilege.

### 4.4 What a client does after success

1. Send CLIENT_SET_CHANNEL_INFO with its channels (section 6.1). Until it does, the others see it with one empty channel.
2. For every user it learns about through USERINFO messages, send CLIENT_SET_USERMASK to subscribe to the channels it wants to hear (section 6.3). Nothing is sent to a client that doesn't subscribe.
3. Start the interval clock from the first CONFIG_CHANGE (section 5) and begin uploading (section 7).

## 5. Tempo: SERVER_CONFIG_CHANGE_NOTIFY (`0x02`)

| Size | Field |
|---|---|
| u16 | BPM (beats per minute) |
| u16 | BPI (beats per interval) |

Sent once after login and again to everyone whenever either value changes (an admin `/bpm` or `/bpi`, or a vote, section 9.4). Valid ranges: BPM 20-400 for admins and 40-400 for votes; BPI 2-1024 for admins and 2-64 for votes. Defaults are server configuration (the reference ships 120 BPM, 8 BPI).

A client applies a new BPM/BPI **at its next interval boundary**, not immediately. The interval length in samples is `sample_rate × 60 × BPI / BPM`, truncated to a whole number.

**There is no clock synchronisation.** Each client runs its own interval clock from the moment it enabled audio; the server does not tell clients when an interval starts. Intervals stay musically aligned because every client plays each received interval at the start of one of its own intervals (section 7.5), so what you hear is always a whole number of intervals behind what was played.

## 6. Users and channels

Every user has up to 32 channels, numbered 0-31, each with a name and flags. Channel records look the same in both directions.

**Channel flags** (u8):

| Bit | Meaning |
|---|---|
| `0x01` | "no default subscribe": a hint that clients should not auto-subscribe to this channel (the reference client ignores it) |
| `0x02` | voice / live channel: continuous 2-second chunks instead of intervals (section 7.6) |
| `0x04` | session channel: ReaNINJAM session mode, chunks tied to a timeline (section 7.7) |
| `0x80` | inactive: in SET_CHANNEL_INFO only, a placeholder that keeps the indices of the following records |

Channels with neither `0x02` nor `0x04` are ordinary interval channels, which is all that most clients use.

### 6.1 CLIENT_SET_CHANNEL_INFO (`0x82`)

| Size | Field |
|---|---|
| u16 | record size *n* (the bytes after each name; always 4 today) |
| records | |

Each record:

| Size | Field |
|---|---|
| string | channel name |
| s16 | volume, dB × 10 (clients send 0) |
| s8 | pan, -128..127 (clients send 0) |
| u8 | flags |
| *n* − 4 | zero padding, if *n* > 4 |

Record *i* describes channel *i*. The client sends records for channels 0 up to its highest used index, with flag `0x80` and an empty name for unused ones in between. It sends the whole list again whenever anything changes.

The server keeps the first `maxchan` records (and never more than 32), marks any channel not listed as inactive, and tells the **other** users only about channels that actually changed, by a USERINFO message. The server stores the volume and pan it was given and repeats them to others; it doesn't use them. If the result leaves the user with no active channel and the server doesn't allow hidden users, the server shows that user to others as having channel 0 with an empty name, so they appear in user lists.

### 6.2 SERVER_USERINFO_CHANGE_NOTIFY (`0x03`)

A list of records, no header. Sent after login (the complete list of every other user's active channels) and afterwards with just the changes.

| Size | Field |
|---|---|
| u8 | active: 1 = this channel exists, 0 = it was removed |
| u8 | channel index |
| s16 | volume (dB × 10) |
| s8 | pan |
| u8 | flags |
| string | username |
| string | channel name |

A client creates a user the first time it sees an active record for them and removes the user when none of their channels is active any more. When a user leaves, the server sends inactive records for each of their active channels plus channel 0 (so a user with no channels is removed too), and a `PART` chat line.

Changing flags `0x02` or `0x04` on a channel resets its playback state on the receivers.

### 6.3 CLIENT_SET_USERMASK (`0x81`)

A list of records:

| Size | Field |
|---|---|
| string | username (matched case-insensitively) |
| u32 | channel mask: bit *i* set = send me channel *i* |

A mask of 0 removes the subscription. The reference client subscribes to every channel of every user it sees (mask `0xFFFFFFFF` or the channels known so far). The server forwards an interval only to subscribers of that user and channel; everyone else never sees the audio.

## 7. Intervals and audio

### 7.1 Timing on the uploading side

For each broadcasting interval channel, at each interval boundary of its own clock, the client:

1. finishes the interval that just ended: flushes the encoder and sends the remaining data with the end flag set;
2. starts a new interval: a new random GUID, a fresh encoder stream (new container headers), and a new BEGIN. The BEGIN is sent together with the first WRITE, not at the boundary itself.

When a channel **stops** broadcasting (the user muted the transmit), the client ends the current interval as above and at the next boundary sends one BEGIN with an all-zero GUID, which means "silence this interval" (section 7.3). Nothing more is sent until it broadcasts again.

Data is sent in WRITE messages of at least **2048** bytes as the encoder produces them (the first WRITE of an interval waits for 8192 bytes, so the container headers arrive together with real audio); one WRITE never carries more than **9216** bytes. The `estsize` field is 0 in practice.

### 7.2 CLIENT_UPLOAD_INTERVAL_BEGIN (`0x83`) and _WRITE (`0x84`)

BEGIN:

| Size | Field |
|---|---|
| 16 | GUID of this interval; all zero = silence |
| u32 | estsize: estimated size in bytes, 0 = unknown |
| u32 | fourcc of the codec (section 7.8); 0 with a zero GUID |
| u8 | channel index |

WRITE:

| Size | Field |
|---|---|
| 16 | GUID |
| u8 | flags: bit 0 = this is the last WRITE of the interval |
| rest | encoded audio bytes, in order |

The concatenation of all WRITE payloads for one GUID is one complete encoded stream (an Ogg file, an Opus file, a FLAC file).

### 7.3 What the server does with an upload

For a BEGIN with channel index *c* from user *U*:

- If *c* ≥ the user's `maxchan` or *c* ≥ 32, ignore it.
- Build the matching DOWNLOAD_BEGIN (same GUID, estsize, fourcc, channel) with *U*'s username appended.
- Send it to every user whose subscription mask for *U* has bit *c* set. Jamsa extension: a subscriber whose `client_caps` lack the codec of this interval, or whose codec the server doesn't relay, instead gets a DOWNLOAD_BEGIN with a zero GUID and fourcc 0 (silence), so their playback stays in time.
- If the GUID is nonzero and the fourcc is nonzero, remember the transfer: who uploads it and who receives it. The reference keeps at most 64 open uploads per user and drops the oldest beyond that, and drops any transfer idle for more than **8 s**.
- A BEGIN with a zero GUID (silence) is relayed to subscribers and nothing is remembered.

For a WRITE: find the transfer by GUID. Only the connection that sent the BEGIN may write to it (a WRITE for a GUID that this connection didn't begin is dropped). Forward the payload unchanged, as DOWNLOAD_WRITE, to every receiver of that transfer. When bit 0 of flags is set, forget the transfer. Optionally the server saves the bytes to a file for the session archive.

The server never decodes, mixes or re-encodes audio, never touches the bytes, and never buffers beyond its send queue. It doesn't know or care where intervals begin in time.

### 7.4 SERVER_DOWNLOAD_INTERVAL_BEGIN (`0x04`) and _WRITE (`0x05`)

BEGIN is the upload BEGIN with one more field:

| Size | Field |
|---|---|
| 16 | GUID; all zero = silence this interval |
| u32 | estsize |
| u32 | fourcc; 0 = none |
| u8 | channel index |
| string | username of the uploader |

WRITE has the same layout as the upload WRITE. A client gives up on a download that receives nothing for **8 s**.

Three cases of BEGIN:

| GUID | fourcc | Meaning |
|---|---|---|
| zero | any | silence: drop whatever interval was queued for this channel, so the next slot plays nothing |
| nonzero | nonzero | a new interval follows in WRITE messages with this GUID |
| nonzero | 0 | legacy: play the interval with this GUID from the local cache (an interval received earlier). The reference server only produces this if a client uploads such a BEGIN; a new implementation may ignore it |

### 7.5 Timing on the receiving side

A receiving client keeps, per remote channel, the interval that is playing now and a queue of the next ones (the reference holds two). A download is decoded as it arrives, and once enough has arrived (the reference waits for 8192 bytes, or the end of the interval, before it is playable) it is placed in the queue. At the client's own next interval boundary, the oldest queued interval becomes the playing one, with a short crossfade at the join. If the queue is empty the channel is silent for that interval. A new download arriving while two are already queued replaces the newest.

Decoded audio is resampled to the client's sample rate if it differs from the stream's (Opus always streams at 48 kHz). A stream that is longer or shorter than the local interval is cut or padded with silence at the boundary; the crossfade hides small differences. Clients record intervals they receive to a work directory by GUID, so a session can be reconstructed later from the client's log.

### 7.6 Voice / live channels (flag `0x02`)

The channel ignores the room's intervals. The uploader cuts the audio into fixed **2-second** chunks, each with its own GUID, BEGIN and WRITEs, with small WRITE blocks (2048 bytes for the first, then every 64 bytes). The receiver plays each chunk as soon as it has 128 samples decoded and crossfades chunks back to back, so the delay is the network delay plus a few hundred milliseconds instead of a whole interval. It's a chat-quality feature, not a performance one.

### 7.7 Session channels (flag `0x04`)

Used by ReaNINJAM to sync a DAW timeline. Audio is uploaded in **2-second** chunks while the DAW plays, each chunk tagged with where it belongs on the timeline through a `SESSION` chat message (section 9.3). Receivers don't play session channels live; they file the chunks by timeline position. A client that doesn't implement session mode can treat such channels as silent.

### 7.8 Codecs and containers

| fourcc | Bytes | Stream |
|---|---|---|
| `OGGv` | `O G G v` | Ogg Vorbis. One complete Ogg logical stream per interval: identification, comment and setup headers, then audio pages; new random serial number for each interval. Every NINJAM client plays this; it is the only codec without the Jamsa extension. |
| `OPUr` | `O P U r` | Raw Opus (Jamsa, revision 2b): a 12-byte header (`u8` version 1, `u8` channels, `u16` pre-skip in 48 kHz samples, `u32` input sample rate, `u32` reserved 0), then 20 ms Opus packets at 48 kHz each prefixed by its `u16` length, then a trailer `u16` 0 and `u32` the count of real samples after the pre-skip; the decoder trims what follows (an extrapolation of the audio, usable for a crossfade). All little-endian. Needs the same `0x100` capability as `OPUS`: a server maps both fourccs to it. The new engine sends and decodes only this form. |
| `OPUS` | `O P U S` | Ogg Opus as in RFC 7845 (the original Jamsa plug-in's form): `OpusHead` and `OpusTags` headers, then 20 ms Opus packets, 48 kHz. The encoder's pre-skip is in `OpusHead`; the last packet's granule position marks the end of the real audio and the decoder trims the padding after it (the padding is an extrapolation of the audio, which a client may use for its crossfade). Jamsa extension. |
| `FLAC` | `F L A C` | Native FLAC: the `fLaC` marker, a STREAMINFO block, then frames. Not in Ogg. Lossless. Jamsa extension. |

Mono or stereo, any sample rate the container allows; the stream's own headers say which. The Jamsa Opus encoder uses constrained VBR at the room's bitrate (section 8.3).

Four-character codes other than these are passed through by the server unchanged (it only checks that the characters are letters, digits, space, `-`, `.` or `_` when naming archive files). A client ignores intervals in a codec it can't decode.

## 8. Codec negotiation (Jamsa extension)

Without this extension everyone sends Vorbis. The extension lets a room agree on Opus or FLAC while older clients keep working.

### 8.1 SERVER_CODEC_MASK (`0x06`)

| Size | Field |
|---|---|
| u32 | codecs: `CLIENT_CAP` codec bits of the codec the room uses now; 0 = Vorbis |
| u32 | opus_kbps: the room's Opus bitrate; present only when nonzero |

Longer payloads are allowed and the extra bytes ignored (room for future fields). The server sends this only to clients that set at least one codec bit in `client_caps`, right after login and again whenever the room's codec or bitrate changes. It is sent to a client **only when the codec or bitrate differs from the last mask that client received**: a `!codec` that doesn't change the effective codec produces the chat line but no new mask. Clients that set no codec bits never see it and never see non-Vorbis audio either.

### 8.2 Choosing the room's codec

The server has a configured set of codecs it relays (`AllowCodecs`, default Opus only; Vorbis is always relayed) and a *chosen* codec, Opus at start, changed by the `!codec` chat command. The room's **effective** codec is:

- the chosen codec, if every logged-in client has its capability bit;
- otherwise Opus if everyone can play Opus, else FLAC if everyone can play FLAC (and the server relays it);
- otherwise Vorbis (codec mask 0).

So one old client in the room moves everyone to Vorbis, and the room returns to the chosen codec when they leave. The server announces each such change in chat (`[server] room audio is now Opus ...`).

A server may be configured to **require** a codec (`RequireCodec opus`): logins without that capability bit are refused with the reason in section 4.3, and the room never falls back to Vorbis.

### 8.3 Client rules

- A client decides the codec of each interval **at the start of the interval**, from the latest mask: FLAC if the mask has it (and the client wants to send FLAC), else Opus, else Vorbis. Without any mask ever received (an older server) it sends Vorbis. A client built without Vorbis sends nothing while the mask is 0.
- When `opus_kbps` is nonzero and the client sends Opus, it encodes at that bitrate; when it is 0, it uses its own setting. Older clients read only the first 4 bytes and send at their own bitrate.

### 8.4 Chat commands

- `!codec` shows the room's codec; `!codec opus`, `!codec flac`, `!codec vorbis` choose one (needs the vote privilege; refused when the server doesn't relay that codec or requires another). The choice applies at everyone's next interval.
- `!bitrate` shows the room's Opus bitrate; `!bitrate N` sets it, 32-510 kbps (needs the vote privilege). The reference server starts at **128 kbps** unless its configuration (`OpusBitrate`) says otherwise; a configured value outside 32-510 is ignored with a warning and 128 stays.

With `RequireCodec`, `!codec` may still switch between the allowed codecs (for example Opus to FLAC); only `!codec vorbis` is refused.

### 8.5 Exact server texts

All of these are chat lines `MSG` with an **empty username** (the same channel as voting results), not `PRIVMSG *`. Codec names are written `Opus`, `FLAC`, `Vorbis`. A hidden user is named `someone`.

Answers to the sender only:

| Situation | Text |
|---|---|
| `!codec` with no or an unknown argument | `[server] room audio is <E>. <choices>` where `<choices>` is `Change it with !codec opus, !codec flac or !codec vorbis`, or `Change it with !codec opus or !codec flac` with RequireCodec |
| same, while the effective codec differs from the chosen one because of someone | `[server] room audio is <E> (<C> was chosen, but <user>'s client can't play it). <choices>` |
| `!codec X` or `!bitrate N` without the vote privilege | `[server] you don't have permission to change the room's audio` |
| `!codec X` for a codec the server doesn't relay, or `!codec vorbis` with RequireCodec | `[server] this server doesn't allow <X>` |
| `!bitrate` with no or a bad argument, bitrate set | `[server] the room's Opus bitrate is <N> kbps. Change it with !bitrate N, from 32 to 510` |
| same, no bitrate set | `[server] the room has no Opus bitrate: everyone sends their own. Set one with !bitrate N, from 32 to 510` |

Broadcast to everyone (after a successful command, before the CODEC_MASK):

| Situation | Text |
|---|---|
| `!codec X` and the room now uses X | `[server] <user> switched the room's audio to <X>` |
| `!codec X` but someone can't play X | `[server] <user> chose <X>, but <who>'s client can't play it, so the room uses <E> until they leave` |
| `!codec X` but the room uses another codec for another reason | `[server] <user> chose <X>, but the room uses <E>` |
| `!bitrate N` while the room plays Opus | `[server] <user> set the room's Opus bitrate to <N> kbps` |
| `!bitrate N` while it plays something else | `[server] <user> set the room's Opus bitrate to <N> kbps, for when the room plays Opus` |

Announcements when the effective codec changes on its own (someone joined or left), sent to everyone right after the JOIN or PART; nothing is announced for the first user of an empty room:

| Situation | Text |
|---|---|
| back to the chosen codec | `[server] room audio is back to <C>` |
| fell back because of a user who can't play the chosen one | `[server] room audio is now <E>, because <who>'s client can't play <C>` |
| any other change | `[server] room audio is now <E>` |

## 9. Chat: CHAT_MESSAGE (`0xC0`)

The payload is **up to five strings**, each ending in `0x00`, including the last one (the Jamsa server rejects a message whose last byte isn't `0x00`; the original server accepted it). The first string is the command. **The reference always sends all five strings**, the unused ones empty (a lone `0x00`), so a `JOIN` is `JOIN\0user\0\0\0\0`; a new implementation should do the same, because some receivers index the fields without checking. A receiver must still accept fewer than five, treating the missing ones as empty.

### 9.1 Client → server

| Fields | Meaning |
|---|---|
| `MSG`, text | say *text* to the room. Needs the chat privilege. Text starting with `!` is a command (9.4) and isn't broadcast, except a `!vote` that counts, which the room sees (9.4) |
| `PRIVMSG`, user, text | private message to *user*. Needs the chat privilege. *user* may also be a prefix of a name, when the name continues with `@` right after the prefix or the prefix itself contains `@` (anonymous names), so `bob` reaches `bob@1.2.3.x`; an ambiguous prefix is refused |
| `ADMIN`, command | a `/command` (9.5), sent without the slash |
| `SESSION`, guid, index, "offset length" | ReaNINJAM session chunk tag (9.3) |

### 9.2 Server → client

| Fields | Meaning |
|---|---|
| `MSG`, user, text | *user* said *text*. An empty user means the server itself (vote results, errors, kicks). The sender receives their own MSG back. A `!` command is not passed on, except a `!vote` that counts: everyone, the voter included, gets it as the voter's MSG (9.4) |
| `PRIVMSG`, user, text | a private message from *user*; user `*` is the server (message of the day, stats) |
| `TOPIC`, user, text | the topic; user is empty when the server sends it on login or in answer to `!topic`, and the changer's name when someone sets it |
| `JOIN`, user | *user* logged in (sent to everyone else) |
| `PART`, user | *user* left (sent to everyone else) |
| `USERCOUNT`, n, max | current and maximum users; only sent to status logins |
| `SESSION`, user, guid, index, "offset length" | relayed session chunk tag |

Server error texts in use (as `MSG` from the empty user): `No MSG permission`, `No PRIVMSG permission`, `No TOPIC permission`, `No KICK permission`, `No BPM/BPI permission`, `No such user: <name>`, `Username is ambiguous (specify partial IP): <name>`, `Valid commands: /topic, /kick, /bpm, /bpi, /stat, /join`.

### 9.3 SESSION

From the client: `SESSION`, GUID as 32 hex digits, channel index as decimal, then one string with the chunk's start time and length in seconds as two decimals separated by a space (`"12.5000000000 2.0000000000"`). The server rebroadcasts it to everyone else with the sender's username inserted as the second field.

### 9.4 `!` commands (in a `MSG`)

| Command | Effect |
|---|---|
| `!topic` | the server sends the current TOPIC to the sender |
| `!vote bpm N`, `!vote bpi N` | cast a vote (needs the vote privilege; N within 40-400 or 2-64). A vote that counts is first shown to everyone, the voter included, as the voter's own chat line: `MSG`, the voter's name, the command as typed with leading and trailing white space removed (`MSG\0Anna\0!vote bpm 100\0\0\0`); the `[voting system]` line it produces follows it. Each user's latest vote per setting counts for `VotingTimeout` seconds (default 120). When the leading value has at least `threshold % × visible users` votes (rounded), the server applies it, broadcasts `[voting system] setting BPM to N` (or `BPI`) and a CONFIG_CHANGE, and clears the votes; otherwise it broadcasts `[voting system] leading candidate: a/b votes for N BPM [each vote expires in Ts]` to everyone. Voting is off when the threshold is over 100: `[voting system] Voting not enabled`; without the privilege: `[voting system] No vote permission`; bad arguments: `[voting system] !vote requires <bpm|bpi> <value> parameters`. These three go to the sender only, and a refused vote is not shown to the room. All `[voting system]` lines are `MSG` with an empty user. (Showing the vote is new in revision 2c: the reference server only sends the `[voting system]` lines; to any client the voter's line is ordinary chat.) |
| `!codec [opus|flac|vorbis]`, `!bitrate [N]` | section 8.4 |
| `!live [on|off]` | section 15.3 |
| other `!word` | `PRIVMSG * Unknown !command. Commands available: !vote, !topic, !codec, !bitrate, !live` to the sender (also on a server without Live) |

### 9.5 `/` commands (in an `ADMIN`)

| Command | Privilege | Effect |
|---|---|---|
| `topic <text>` | topic | set the topic; everyone gets `TOPIC <user> <text>`. Without text: the sender gets the topic |
| `kick <name>` | kick | disconnect the user (every match, when `name*` gives a prefix). Everyone gets `MSG "" User X kicked by Y`; no match: `User "<name>" not found!` to the sender |
| `bpm N`, `bpi N` | bpm | set the tempo at once (20-400, 2-1024); everyone gets CONFIG_CHANGE and `MSG "" <user> sets BPM to N`. Out of range: `BPM parameter must be between 20 and 400` / `BPI parameter must be between 2 and 1024` to the sender |
| `stat [full]` | any | private-room statistics (lobby servers) |
| `join <room>` | any | move to a private room (lobby servers only) |

### 9.6 Privileges

Per account, set in the server's user list: topic, chat, bpm/bpi, kick, reserved slot, multiple logins, hidden (not shown or counted), vote, see private rooms. Anonymous users get chat (if the server allows), vote, and multiple logins (if configured). They are never visible on the wire except through the refusals above.

## 10. Lobby and private rooms (optional)

A server can run in lobby mode: logins land in a lobby where no audio flows and only `!join <room>`, `!stat` and `!topic` work (`PRIVMSG * [lobby] usage: !join private_room_name`). `!join <room>` (or `/join`) moves the connection into a named private room, creating it if needed. The client gets `MSG "" [lobby] joining room!`, then a **second AUTH_REPLY** (success, with the room's maxchan) followed by the usual login sequence for the room (config, user list, topic); it must answer with SET_CHANNEL_INFO again, as after a normal login. Lobby users see PART, room users see JOIN. Clients need no special support beyond that; Jamsa does not use lobby mode.

## 11. Constants

| Name | Value |
|---|---|
| Default port | 2049 |
| Max payload | 16384 bytes |
| Protocol version | `0x00020000`; server accepts `0x00020000`-`0x0002ffff`, client accepts `0x00020000`-`0x0002fffe` |
| Keepalive | server-chosen, default 3 s; drop after 3× with nothing received; 45 s during a licence prompt |
| Login timeout | 30 s (120 s with a licence) |
| Channels per user | up to 32; server tells each client its own limit |
| Transfer idle timeout | 8 s (server and client) |
| Open uploads per user | 64 (reference server) |
| Send queue | 512 messages (reference) |
| Upload block | ≥ 2048 bytes (first block 8192), ≤ 9216 bytes per WRITE |
| Live/session chunk | 2.0 s |
| BPM | admin 20-400, votes 40-400 |
| BPI | admin 2-1024, votes 2-64 |
| Opus bitrate | 32-510 kbps |
| Codec capability bits | `0x100` Opus (fourcc `OPUS` or `OPUr`), `0x200` FLAC; bits 8-15 reserved for codecs |

## 12. Compatibility notes for a new implementation

- **Interoperability is decided by sections 2-7 and 9.** A client that implements them with Vorbis works with every NINJAM server. Add section 8 to get Opus and FLAC with Jamsa servers.
- Send the whole channel list every time something changes; the server diffs it.
- Don't rely on `estsize`; everyone sends 0.
- Treat unknown message types, unknown chat commands and extra bytes at the end of a payload as harmless.
- The server must forward WRITE bytes untouched and in order: receivers feed them straight into a decoder.
- A server must bound every per-connection list (subscriptions, transfers, queued messages) because clients come and go without warning.
- Keepalives both ways: a client that only listens is dropped after 9 s by a default server.

## 13. Acceptance tests

A new client or server should pass these against the existing ones (the Jamsa repository has a Python test client, `ninjam/server/tests/njclient.py`, and tests that exercise most rows):

1. Login as `anonymous:test` with no password → success, name `test@<ip>`; wrong password on a listed account → `invalid login/password` and the socket closes.
2. Old protocol version → `incorrect client version`; licence not agreed → `license not agreed to`; no AUTH_USER for 30 s → `authorization timeout`.
3. Two clients: the second sees the first's channels in USERINFO after SET_CHANNEL_INFO, and a JOIN; after the first disconnects, PART and inactive records.
4. Subscribed client receives DOWNLOAD_BEGIN with the uploader's name and the WRITE bytes unchanged, ending with the end flag; an unsubscribed client receives nothing.
5. A WRITE for a GUID that another connection began is dropped.
6. Channel index 32 or beyond `maxchan` is ignored.
7. Chat MSG is broadcast with the sender's name; a chat whose last byte isn't `0x00` is ignored (Jamsa server).
8. `!vote bpm 100` from enough users changes the tempo and sends CONFIG_CHANGE to all. Every vote that counts reaches everyone as the voter's `MSG` before the `[voting system]` line it causes; a refused vote (voting off, no privilege, bad arguments) answers the sender and nobody else sees it.
9. Codec mask: a client with `0x100` gets `0x06` with Opus; when a client without codec bits joins, everyone gets mask 0 and `[server] room audio is now Vorbis ...`; it comes back when they leave. An Opus interval reaches only clients with `0x100`; others get a zero-GUID BEGIN.
10. `!bitrate 128` → the mask carries 128 for everyone with codec bits; out-of-range values only answer the sender.
11. A client that stops sending keepalives is dropped after about 9 s; one that sends them stays.
12. Upload, in sequence, three intervals from one client while another client listens: the listener's playback is exactly one interval late and stays in time across a `!vote bpm` change.

## 14. Revision history

- **Revision 2d (2026-10-18):** Live mode (section 15) as o11proto plays it now: the room has one mode for everyone, its `!live` switch alone, with no interval audio while it is Live, and the server's advice says who has trouble (15.11: `0x85`, `0x08`, the upload refusal, the `[live room]` lines, the new `!live` texts in 15.3); the players send to each other directly, all to all (15.10: caps bit 17, `0x09`, `0x86`, `0x87`, the probes, the pair key; the relay only for players without paths, make before break while a new path opens, and the off-by-default relay fallback with `!live direct`); the peer flag `0x02` sending (15.1); the client's socket isn't `connect()`ed, and its thread's wake and wait (15.6); the rate limit scaled for direct senders and the traffic line's order (15.7); the port mapper warning's new ending (15.8).
- **Revision 2c (2026-10-17):** a `!vote` that counts is shown to the whole room as the voter's own `MSG`, before the `[voting system]` line (9.1, 9.2, 9.4, test 8); refused votes stay private to the sender. Other `!` commands are unchanged.
- **Revision 2b (2026-10-17):** fourcc `OPUr`, raw Opus packets with length prefixes (7.8); servers map it to the `0x100` capability like `OPUS`.
- **Revision 2a (2026-10-16):** two more findings from the same comparison: chat messages always carry five strings (9), and CODEC_MASK is only resent when it changes (8.1).
- **Revision 2 (2026-10-16):** a clean-room implementation written from revision 1 was run against the reference server and client. Audio, login, channels, subscriptions and forwarding matched at once; the differences were all in things revision 1 left open, now fixed in the text: the exact `[server]` and `[voting system]` chat texts and the fact that they are `MSG` lines with an empty user (8.5, 9.4, 9.5); the default Opus bitrate of 128 kbps; anonymous users don't get multiple logins unless configured; the same-name replacement runs before the capacity check; `!codec` may switch between allowed codecs under RequireCodec.
- **Revision 1 (2026-10-16):** first version, written from the Jamsa repository at `e040ae1`.

## 15. Live mode (O11 Session extension)

Live mode lets the players of a room hear each other in real time: each Live player sends small Opus packets over UDP, directly to each of the room's other Live players where the pair has a direct path (15.10), and through the server it is connected to, which copies them on, for a player without paths. The room has one mode for everyone, set by its `!live` switch alone: while the room is Live every Live player is heard live and there is no interval audio at all (none is uploaded or relayed); otherwise everyone plays intervals (15.11). This section is the wire reference for o11proto; it follows the Live mode specification revision 2 (2026-10-17, written from Jamsa's Live at `44a4904`), where every rule here is explained, except the room's one mode with its advice (15.11, in place of that specification's per-player switching) and the direct paths (15.10), which are O11 Session's own. The payload is in 15.9, the client's behaviour (audio, jitter buffer, the room's mode, `/live`) in `ENGINE.md` section 16, and what o11proto chose where that specification leaves room is in `NOTES.md` and `engine/NOTES.md`.

### 15.1 The caps bit and SERVER_LIVE (`0x07`)

A client that can play Live sets bit 16 (`0x10000`) of `client_caps`; one that also sends directly to the other players where it can (15.10) sets bit 17 (`0x20000`) too, which is read only together with bit 16. The server reads them at login only and never refuses a login because of them. There is no `server_caps` bit: a client learns that the server has Live only by receiving `0x07`, which is sent only to clients that set the bit, never to hidden users, and only to players with a key (below).

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | u8 | version | 1 |
| 1 | u8 | fixed length | 32: where the peer list starts |
| 2 | u8 | flags | `0x01` room on, `0x02` UDP open, `0x04` may send; other bits 0 |
| 3 | u8 | reserved | 0 |
| 4 | u16 | UDP port | 0 = the TCP port number the client connected to |
| 6 | u16 | bitrate | the room's Live Opus bitrate, kbps (32-510) |
| 8 | u32 | peer id | this connection's id, never 0 |
| 12 | u64 | key | this connection's secret |
| 20 | u16 | frame size | Opus frame at 48 kHz: 120, 240 or 480 |
| 22 | u16 | sender limit | how many players of a room play Live |
| 24 | 8 | reserved | 0 |
| 32 | … | peers | records to the end: u32 peer id, u8 flags (`0x01` bound, `0x02` sending: its own latest report says its source sends, 15.11), string name |

The peers are the room's senders other than the receiver, in the room's order. **Parsing**: a payload under 32 bytes, version 0, or a fixed length below 32 or beyond the payload is ignored; a later version is read as version 1; a record needs 6 bytes left and a NUL inside the payload, otherwise the list ends; at most 256 records are read; records with id 0 or the receiver's own id are skipped. Each message is a full state that replaces the previous one; a client forgets it when it connects again.

### 15.2 What the server says, and when

- A **Live player** is logged in, set bit 16, and is not hidden. The **room's order** is the order in which the TCP connections were accepted (peer ids count up in that order).
- Each Live player gets a 64-bit **key** from the system's cryptographic random source, never 0, the first time the server works out the Live state with Live allowed; it lasts for the connection. Without randomness the player keeps key 0, gets no `0x07`, and the server logs once `warning: Live: could not read the system's random source, so nobody can play Live` and tries again at later computations.
- The **senders** are the first sender-limit Live players in the room's order that have a key and a channel limit (maxchan) above 0. Others with channels are **over the limit**; Live players with maxchan 0 are **listeners only**. Both get `0x07` without may send, are known to the relay and can bind, but receive no audio and are listed by nobody.
- **Live allowed** means the configuration's Live mode isn't `no`. The room is **on** when Live is allowed, the UDP port is open and the room's switch is on.
- Contents for player *P*: the UDP port is the announced port when set, else 0 (the relay always listens on the TCP port's number); flags as above (with the UDP port closed a sender gets may send alone); peers with bound set when the UDP port is open and that peer is bound (15.6).
- The server works the messages out again whenever an input changes (a player joins or leaves, the switch, the options, the UDP port opening or closing, any player's bound state, the sending bit of a player's report, and for `0x09` a player's LAN address or relay-only flag) and sends each player its message when it differs, byte for byte, from the last one sent to it, within 100 ms. The first one follows the login sequence. Each `0x09` (15.10) goes after the `0x07` of the same pass, so that its ids are known.
- **Live taken away**: when the options change to `no`, each player who was told about Live gets one message with flags 0 and no peers.

### 15.3 The `!live` command

A MSG whose text, after leading spaces and tabs, starts with `!live` in any letter case followed by a space, a tab or the end (`!livestream` isn't it). The argument is the rest, trimmed of spaces and tabs; `on` and `off` (any case) change the switch, `direct` takes the room's route back to direct (15.10) on a server with the relay fallback and is an unknown argument elsewhere, anything else asks for the state. It is never broadcast as chat and works without the chat privilege. All answers are MSG lines with an empty user, in this order of checks:

1. `on`/`off`/`direct` without the vote privilege: `[server] you don't have permission to change the room's audio`
2. Live not allowed: `[server] this server doesn't have Live`
3. UDP port not open: `[server] Live is off: the server could not open its UDP port`
4. `direct`: the answers of 15.10, to the sender.
5. no or an unknown argument, or `on`/`off` equal to the state (which still counts as the room's choice): the status text, to the sender: `[server] Live is on. ` or `[server] Live is off. `, then `Players who can play Live: <senders>. ` or `Nobody here can play Live yet. `, then (only with an *over* list) `Live has room for <limit>, so <over> isn't heard while it is on. ` (`aren't` with several), then (only while the room's route is Relay, 15.10) `It goes through the server's relay since <why>. ` (without ` since <why>` when there is none), with `Everyone can connect directly again. ` after it while every pair has a direct path again, then `Turn it off with !live off` or `Turn it on with !live on`.
6. otherwise the switch changes and every logged-in user gets `[server] <who> turned Live on: O11 Session players now hear each other in real time.` followed by ` <name>'s client can't play Live, so <name> isn't heard until Live is off.` for one user without bit 16 (or ` The clients of <names> can't play Live, so they aren't heard until Live is off.` for several) and the *over* sentence ` Live has room for <limit>, so <names> isn't heard while it is on.` (`aren't` with several); or `[server] <who> turned Live off: everyone hears each other through intervals again.` `<who>` is `someone` for a hidden user.

Names are joined as `a`, `a and b`, `a, b and c`. Once switched with `!live`, the room's choice survives option changes (also to `no` and back); otherwise the room takes the default again (on for `on`, off otherwise).

### 15.4 Options

| `LiveConfig` field | `o11server` flag | Values | Default |
|---|---|---|---|
| `mode` | `--live` | `no`, `yes` (starts off), `on` (starts on), `require` (like `yes`, but `start()` fails without the UDP port) | `no` |
| `kbps` | `--live-kbps` | 32-510 | 128 |
| `frame` | `--live-frame` | 120, 240, 480 | 240 |
| `max_peers` | `--live-max-peers` | 1-64 | 8 |
| `announce_port` | `--live-port` | 0-65535; the UDP port to announce instead of the real one | 0 |
| `strict_address` | `--live-strict-address` | `yes`, `no`: UDP must come from the player's TCP address | `yes` |
| `relay_fallback` | none; the environment variable `O11_LIVE_RELAY_FALLBACK` (`1` or `0`, anything else leaves the field) overrides it when the UDP port opens and at each `setLiveConfig()` while it is open | off: Live goes only directly between the players (15.10); on: the room goes through the relay for everyone once a pair has no direct path | off |

Out-of-range values are logged (`warning: LiveKbps 1000 is outside 32-510, using 128`) and replaced by the default. `Server::setLiveConfig()` applies new options while running, as a configuration reload would.

**The UDP port.** With Live allowed, the server binds UDP on the TCP port's number (on the TCP listener's address when it has one), IPv4 only, without address reuse, with 1 MiB buffers, non-blocking, reporting each datagram's local address. Success logs `Live: listening on UDP port <p>`. A busy port logs `warning: Live is off, could not open UDP port <p>. Another program may already be using it (2049 is also the NFS port). Set a different Port in the config, or Live no.` and the server runs without UDP; with `require`, `start()` logs `Error listening on UDP port <p> for Live! Another program may already be using it (2049 is also the NFS port), or you may need permission to use it. Set a different Port in the config or use -port.` and fails. Opening draws a new cookie secret (and fails, with its own line, when no randomness can be had), forgets every binding, and draws a new path secret, so that every pair gets a new key and every path opens again (15.10); options changed to `no` close the port (`Live is off, closed UDP port <p>`). At shutdown the UDP port closes before the users are dropped.

### 15.5 Datagrams

All header fields are little-endian. A client sends to the IPv4 address of the server end of its TCP connection, at the UDP port of `0x07`, or at the TCP port number it connected to when that is 0.

| Client → server, 20 bytes | | Server → client, 8 bytes | |
|---|---|---|---|
| 0, 2 | magic `N` `L` | 0, 2 | magic `N` `L` |
| 2, u8 | version 1 | 2, u8 | version 1 |
| 3, u8 | type: 1 hello, 2 data (3 probe, 4 probe reply: between players, 15.10) | 3, u8 | type: 1 hello reply, 2 data |
| 4, u32 | sender: the peer id | 4, u32 | origin: the sender's peer id (0 in a hello reply) |
| 8, u64 | key | | |
| 16, u32 | destination: 0 = every other sender, else one peer id (0 in a hello) | | |

- **Hello**, exactly 40 bytes: the header, u64 timestamp at 20 (echoed; o11proto sends monotonic microseconds with bit 63 set), u64 cookie at 28 (the latest accepted, 0 if none), 4 zero bytes.
- **Hello reply**, exactly 24 bytes: the server header with origin 0, the hello's 8 timestamp bytes, the u64 cookie for the hello's source address and port.
- **Data**: the header, then 1-1452 bytes of payload (15.9), forwarded untouched; 21-1472 bytes from a client, 9-1460 from the server.
- The server drops anything over 1472 bytes. A client takes a datagram from the relay's address and port as the relay's; any other is a player's (15.10). It drops a datagram over 1500 bytes, and from the relay one with the wrong magic or version, a hello reply that isn't 24 bytes or has a nonzero origin, data with origin 0 or a payload of 0 or over 1452 bytes, and any other type.

### 15.6 Hellos, the cookie and binding

- The client sends a hello when its Live client starts, then every 2 s, and at once whenever an accepted reply brings a cookie other than the one it holds. It remembers its last 8 timestamps and accepts a reply only with a nonzero timestamp among them and a round trip of 0 to 10 s; that sets the server round trip and the cookie. Hellos go on while the room is off. In o11proto this is `live::Transport` (`live_transport.h`): one IPv4 socket per connection for the relay and the direct paths, bound to a port of its own and not `connect()`ed (what comes from the relay's address and port is the relay's, anything else must be a player's, 15.10), with 256 KiB buffers, no SIGPIPE and (on Windows) no "port unreachable" reset. Its start fails with `no peer ID or server address` or `can't open a UDP socket (error <n>)`. Its own thread is a network thread (`o11proto/realtime.h`, `thread::makeNetworkThread()`: on macOS the time-constraint policy, as an audio thread, else latency-critical). It waits on the socket and on `wake()` (one write to a non-blocking pipe, safe from the audio thread; none on Windows), and at least every `Options::wait_ms` (1-100 ms, default 1; always 1 on Windows) runs a pass of its own: an owner that sets more than 1 must call `wake()` for its own work, as the engine does with 10. `timing()` reports for diagnostics the scheduling it got (real-time or latency-critical), the timed waits that ran out and the longest of them and how many ended over 5 ms past their time, and the wakes, the longest wake-up delay and how many were over 5 ms.
- **The cookie** = SipHash-2-4, keyed with the 16-byte secret, over 22 bytes: peer id (u32 LE), key (u64 LE), the hello's source IPv4 address and UDP port as on the wire, and epoch = floor(monotonic seconds / 30) (u32 LE). Test vectors (secret 00 01 … 0f): SipHash of 00 01 … 0e is `a129ca6149be45e5`; id 1, key `0x0123456789abcdef`, 127.0.0.1:50000, epoch 1000 gives `c180445be3d73103`; id 7, key `0xfedcba9876543210`, 192.0.2.10:2049, epoch 57000000 gives `f1620faed24ff4a6`.
- A hello is **verified** when its cookie equals the cookie of its sender, key, source address and port at the current epoch or the previous one, compared in constant time. The reply always carries the current epoch's cookie.
- A verified hello binds the player: bound address := the hello's source, local address := where it arrived, last hello := now. Nothing else binds; data never does. The player stays **bound** for 20 s after its last verified hello (checked every 100 ms), until the UDP port opens again, or until it leaves.

### 15.7 The relay

A **known player** is a Live player with a key, with Live allowed and the UDP port open (over the limit and listeners only included). Each datagram goes through these checks; one that fails is dropped silently and counted under the reason given:

| Step | Check | Reason |
|---|---|---|
| 1 | length ≤ 1472 | too big |
| 2 | length ≥ 20, magic, version 1, and type 1 with length 40 or type 2 with length above 20 | malformed |
| 3 | the sender is a known player | unknown sender |
| 4 | the key matches (constant time) | wrong key |
| 5 | strict address: the source IPv4 address is the player's TCP peer address | wrong address |
| 6 | the rate limit | over the rate limit |
| 7 | hello: always answered; unverified ones are counted here | unverified address (answered) |
| 7 | data: the room is on | room's Live is off |
| 8 | data: the sender is a sender | sender may not send |
| 9 | data: destination ≠ 0 must be a known player, not the sender, a sender, and bound | no such player |

- Destination 0 goes to every other sender that is bound, at most 64 copies. Addresses only ever come from the relay's own table. The copy is the 8-byte server header followed by the payload, byte for byte, sent at once. A copy that can't be sent counts as `could not send`.
- **Rate limit**, per known player, with *fps* = 48000 / frame and *scale* = max(1, the number of senders other than this player): bytes per second = *scale* × (2 × (kbps × 125 + *fps* × 64) + 8000); datagrams per second = *scale* × (2 × *fps* + 50); each bucket holds 0.25 s, fills completely at the player's first datagram that reaches the check, refills continuously and is charged once per datagram (header included), except that a data datagram to destination 0 costs *scale* times as much. So a player that sends to each of the others on its own (15.10) may send that many streams, and the copies the relay makes of one datagram to everyone are limited as before. The levels survive option changes (a level above a new burst is cut to it) and start full again when the player is known again.
- **The right address.** Every reply and every copy is sent from the local address at which the hello (or the destination's last verified hello) arrived, except a broadcast or multicast one. When that address is gone, the server sends once more from where the routing table chooses and forgets it until the next verified hello.
- **The traffic line.** Every 60 s, when datagrams arrived in that period: `[live] in the last <T> s: <n> player<s>, <in> datagrams/s in, <out> out` followed by `, nothing dropped` or `, dropped <total>:` and ` <count> <reason>` for each reason that occurred, separated by commas, in this order: malformed, too big, unknown sender, wrong key, wrong address, over the rate limit, unverified address (answered), room's Live is off, sender may not send, no such player, could not send (the first two are the other way round from the table). *T* is the real time of the period, *n* the bound players, *in* every datagram read and *out* every datagram sent, divided by *T*; numbers are rounded as `printf` rounds. A normal bind shows `dropped 1: 1 unverified address (answered)`.
- **Test hooks**: `NINJAM_LIVE_FRESH_SECONDS` (the bound time) and `NINJAM_LIVE_STATS_SECONDS` (the period), decimal seconds above 0, read when the UDP port opens and at each `setLiveConfig()` while it is open; also `LiveConfig::fresh_seconds` and `stats_seconds`.
- For tests, any thread: `Server::intervalTraffic()` gives the interval audio since the start, in bytes of `UPLOAD_INTERVAL_WRITE` received (from clients with bit 16, and from the others) and of `DOWNLOAD_INTERVAL_WRITE` sent, and the uploads refused while the room was Live (15.11); `Server::liveDataRelayed()` the Live data datagrams the relay has copied to players.

### 15.8 Port mapping

With the UDP port open, the port mapper (`PORTMAP.md`, `PortMapper::start(port, true)`) maps UDP on the same number too, right after TCP and at every renewal: UPnP with the same ports, address, description and the TCP lease (0 with a permanent-only router); NAT-PMP asking for the public port TCP was granted and lifetime 3600, counted as working only when granted exactly that port. The lines say `TCP and UDP` instead of `TCP` while UDP works (`asking the router to forward TCP and UDP port <p> to this machine`, `removed the forward for TCP and UDP port <p>`, the section 7 lines); without a router, `Forward TCP port <p> on your router to this machine by hand, and UDP port <p> for Live, or turn on UPnP …`. A UDP failure never fails TCP and is logged once, until UDP works again, as `warning: Live: the router did not forward UDP port <p> (<why>), so players outside your network can't play Live with you: while the room is Live they hear no one, and no one hears them (!live off puts everyone on intervals)` (not a `[portmap]` line), with `<why>` one of `UPnP: the router forwards it to another machine`, `UPnP error <n>`, `NAT-PMP: the router did not answer`, `NAT-PMP error <n>`, `NAT-PMP: it gave public port <p>, not <q>`. UDP is removed first, quietly.

### 15.9 The payload

The relay never reads the payload. Between Live clients it is the subset of AOO 2.0 in Live spec section 5, written for o11proto from that description (no AOO code): every number big-endian, every source and sink AOO id 1, the UDP header's peer ids saying who is who. Messages for a sink come from a source and messages for a source from a sink. Each client has one source (its channel 0) and up to eight sinks (`ENGINE.md` 16.4).

**Binary messages** (first byte has bit 7 set):

| Message | Bytes | Layout |
|---|---|---|
| `/data`, an audio block (source → sinks) | 24 + packet | `81 00 01 01`, i32 stream id, i32 sequence, u8 channel 0, u8 flags `0x01`, u16 packet size, f64 rate 48000.0, the Opus packet |
| `/data`, an xrun block | 16 | the same with flags `0x08`, size 0, no rate and no data |
| data request (sink → source) | 12 + 8 × *n* | `80 00 01 01`, i32 stream id, i32 *n* (1-32), then *n* × (i32 sequence, i16 frame offset −1, u16 0) |

- The audio blocks are one stream for every sink (broadcast mode: one stream id). Each block goes to each listed player directly where the pair's path is up and through the relay, addressed to that player, to a bound player without a path (15.10); when every player would get it through the relay it goes once to destination 0 instead. Everything else is unicast to one peer, by the same route.
- Reading `/data` follows Live 5.4: 16 fixed bytes, then the optional fields its flags announce (`0x02` frames: u32 total, u16 count, u16 index; `0x04` stream messages: u32 bytes; `0x01` f64 rate, finite and above 0; `0x10` u64 time), then the size's bytes; stream messages are skipped. o11proto never splits a block and treats a split one as lost; it drops a negative stream id or sequence, command bit `0x80`, and channel onsets other than 0.
- Sequences run from 0 to `0x7ffffffe` and start again at 0; a block 2^30 or more below the last one played is a wrap. A data request's frame offset −1 asks for the whole block; o11proto also reads requests for frame 0, ignores other frames, drops a message whose count is negative or larger than it holds, and reads at most 32.

**OSC messages** (single OSC 1.0 messages; no bundles), each with the sender's id 1 as first argument:

| Address | Tags | Bytes | Arguments after the id |
|---|---|---|---|
| `/aoo/sink/1/start` | `,isiiiiiisbtiiNNi` | 116 | version `2.0-test4`, stream id, start sequence, format id, channels 2, rate 48000, block (frame size), codec `opus`, blob `00 00 08 03` (restricted low delay), stream time (the sender's monotonic clock), latency, codec delay 120, two nils, sample offset 0 |
| `/aoo/sink/1/stop` | `,iiii` | 44 | stream id, next sequence, 0 |
| `/aoo/sink/1/ping` | `,it` | 36 | t1 (wall clock) |
| `/aoo/src/1/pong` | `,itttf` | 56 | t1, t2, t3, f32 loss since the previous pong |
| `/aoo/src/1/ping` | `,it` | 32 | t1 |
| `/aoo/sink/1/pong` | `,ittt` | 56 | t1, t2, t3 |
| `/aoo/src/1/start` (start request) | `,is` | 40 | version |
| `/aoo/src/1/stop` (stop request) | `,ii` | 28 | stream id |

- A version is accepted when it reads as a decimal number 2, a period and a digit (`2.0-test4`, `2.1`; not `2`). A `/start` is accepted for codec `opus`, 1 or 2 channels, 48000 Hz and a block of 120, 240 or 480; its latency is clamped to 0..10 s and its codec delay and sample offset to 0..rate/10. The latency the source sends is trunc(max(0, *F* − 64*r*) + *L r*), *r* = 48000 / host rate, *L* = 0 at 48 kHz multiples and 2 otherwise (176 at 48 kHz and frame 240).
- Round trips are (t4 − t1) − (t3 − t2) on the measuring side's clock. Time tags are NTP 32.32; the `/start`'s stream time is never compared with another clock.
- o11proto doesn't send or accept the OSC forms of `/data` and of the data request (`/aoo/sink/1/data`, `/aoo/src/1/data`); it sends and accepts the binary forms only.

**The codec**: Opus, 48 kHz, stereo (mono input as L = R), one packet of one frame per block, the frame size and bitrate of `0x07` (frame 120, 240 or 480, otherwise 240; 32-510 kbps), application restricted low delay. Decoded samples are clamped to −2..+2. Resends, budgets, stream life and the client's rules are `ENGINE.md` 16.5-16.6.

### 15.10 All to all: the direct paths (`0x09`, `0x86`, `0x87`)

O11 Session's own. A client with caps bit 17 sends its Live datagrams straight to each of the room's other players over a direct path, and the relay (15.7) carries Live only for the players who have none. Each pair finds its path itself, by probes, from addresses and a key the server gives it.

**SERVER_LIVE_PATHS (`0x09`)**, server → client: to each sender with bits 16 and 17 that isn't relay-only (below), after its `0x07` and whenever it changes (15.2). It lists the room's other senders with bit 17 that aren't relay-only, a full state each time. Without Live allowed, the UDP port open and a path secret, or to a client that isn't such a sender, it lists no one, and that empty message goes only to a client that had records.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | u8 | version | 1 |
| 1 | u8 | fixed length | 8: where the records start |
| 2 | u16 | record size | 24 |
| 4 | 4 | reserved | 0 |
| 8 | … | records | to the end: u32 peer id, u64 pair key, u32 public IPv4, u16 public port, u32 LAN IPv4, u16 LAN port |

- The public address is where the player's verified hellos come from (its bound address, 15.6; 0 while it isn't bound), the LAN address the one its `0x86` reported (0 for none). IPv4 addresses are in network order, ports little-endian like every other field.
- **Parsing**: a payload under 8 bytes, version 0, a fixed length below 8 or beyond the payload, or a record size below 24 is ignored. Records follow at the record size to the end, at most 256; those with id 0 or key 0 are skipped. A client also skips its own id and a second record for the same id.
- **The pair key** = SipHash-2-4 keyed with the server's 16-byte path secret, over the smaller peer id and then the larger (u32 LE each); 1 where that gives 0. Only the two players and the server know it. The server draws a new path secret whenever the UDP port opens (15.4), so every pair gets a new key and every path opens again; without randomness it has no paths (no `0x09`, and the room's route is none).

**CLIENT_LIVE_LAN (`0x86`)**, client → server, 8 bytes: u8 version 1, u8 0, u16 port, u32 IPv4 (network order; 0 for none). The address of the client's Live socket on its own network, for the players on that network: the address the system sends to the relay from (loopback counts as none) and the socket's port. A client sends it once per start of its Live client, once a `0x09` has come on the connection. The server takes it only from a player with bit 17; under 8 bytes or version 0 is ignored, and a zero address or port, or a broadcast or multicast address, counts as none.

**CLIENT_LIVE_PATHS (`0x87`)**, client → server: from a client with bit 17 whose Live client runs and that has had a `0x09`, whenever its content changes. u8 version 1, u8 flags (`0x01` relay only: it sends and takes Live through the relay only, a test setting), u16 count, then count records of 8 bytes: u32 peer id, u8 state (0 opening, 1 up, 2 unreachable), 3 zero bytes; one for each player of its latest `0x09`, in that order, at most 256. **Parsing**: under 4 bytes or version 0 is ignored; records follow to the count or the end, at most 256; a state above 2 reads as opening; id 0 is skipped. The server takes it only from a player with bit 17 and keeps its relay-only flag and its states; a relay-only player is listed in no one's `0x09`, and its own lists no one.

**Datagrams between players** use the client header of 15.5 with the pair's key in place of the player's key and the receiver's peer id as destination:

- data (type 2): the header, then 1-1452 bytes of payload, as to the relay;
- probe (type 3) and probe reply (type 4), exactly 32 bytes: the header, u64 timestamp at 20 (the prober's monotonic microseconds with bit 63 set; echoed), u8 candidate at 28 (which of the receiver's addresses the probe was sent to; echoed), 3 zero bytes. The reply is the probe with type 4, the answering player as sender and the prober as destination.

A client takes such a datagram only from a player of its latest `0x09`, with that pair's key (compared in constant time), its own id as destination and one of these lengths; everything else is dropped. It answers each probe to the address it came from. An address it doesn't have for that player becomes the player's third candidate, *seen*.

**The paths.** A client has up to three candidate addresses for each player: the LAN address, the public address (left out when it is the LAN one) and the seen one. A player's loopback address is read as the relay's address, unless the relay is on loopback itself (a player who hosts the room). The network thread works each path out at every pass:

- **opening** from the time the pair first has an address (again with a new key): probes to every candidate every 250 ms for the first 2 s, then every 2 s;
- **up** at a probe reply with a round trip of 0 to 10 s: it sends to the *active* address, the first of LAN, public and seen answered in the last 10 s, and probes only that one, every 2 s, which also keeps the routers' mappings open;
- **suspect** 3 s after the last reply: probes to every candidate every 250 ms;
- **unreachable** 10 s after the last reply, or 10 s after it opened without one: probes to every candidate every 2 s, and a reply brings it up again.

A probe from a player whose path isn't up makes the client probe back at its next pass. When a `0x09` changes the addresses of a path that is up, it turns suspect and is probed at once. The `0x87` reports suspect as up.

**Where Live goes.** A payload for one player goes directly while the pair's path is up or suspect. Otherwise it follows the room's route (`0x08` version 3, 15.11), which a client applies when the message comes:

- **Direct only**, the server's default: nothing goes to a player whose path isn't up, not while it opens (but for make before break, below) and not a second copy while it is suspect. A pair that can't reach each other can't hear each other, and the advice says so (15.11). A client is direct only from each start of its Live client until a room message says otherwise.
- **Make before break** (direct only): a bound player that the client's audio blocks have reached through the relay for 1 s or more, up to the block before (a run ends after 250 ms with none through the relay; blocks to destination 0 count for every bound player), goes on getting them through the relay, addressed to it, while its new path is opening (listed, never answered and not yet unreachable). The first block after the path is up goes directly; if the path becomes unreachable instead, nothing more goes to that player. The blocks a player gets through the relay between its `0x07` listing and its `0x09` are no such run, so a pair whose path never opens still can't hear each other.
- With the server's **relay fallback**: a payload for a player whose path isn't up goes through the relay, addressed to that player, and one for a suspect path goes both ways; while the room's route is relay, everything goes through the relay.
- A player without a path (no bit 17, or relay-only) is reached through the relay, addressed to it, while it is bound, and sends to the room through the relay.
- The audio blocks go to each player by its own route. When none would go directly, and none is held back by direct only, they go once to destination 0 instead, as without paths.

A client without bit 17, or on a server without paths, sends everything through the relay as in 15.7. Data received directly is handled as the relay's, from the header's sender.

**The room's route.** At every pass the server checks the bound senders: the first that doesn't send directly gives the reason *no direct* (no bit 17) or *relay only* (its `0x87`); then each pair of the others that either side reports unreachable has *no path*, and the first such pair is the reason if there is none yet. All are up when every one of them sends directly and both sides of every pair report up.

- Without the relay fallback the route stays direct, and each sender's pairs with no path go into the advice: it can't play Live (15.11).
- With the relay fallback, the route goes from direct to relay once there is a reason while the room is Live, with the log line `[live room] Live goes through the relay for everyone now: <a> and <b> had no direct path` (or `<a>'s client doesn't send directly`, `<a>'s client sends through the relay only`). The paths are still probed and reported. While it is relay, *direct possible* is set when there is no reason and all are up, logged once as `[live room] advice: everyone can connect directly again`. It stays relay until `!live direct`; a room with no Live player is direct again.
- **`!live direct`** (15.3; only with the relay fallback, and shown nowhere): `[server] Live goes through the server here.` on a server without paths; `[server] Live already goes directly between the players.` when the route isn't relay; `[server] Live stays on the server's relay: <a> and <b> can't reach each other directly.` (or `<a>'s client doesn't send directly.`, `<a>'s client sends through the relay only.`) while there is a reason now; otherwise the route is direct again, the sender gets `[server] Live goes directly between the players again.` and the server logs `[live room] <name> switched Live to direct`.

### 15.11 The room's mode and the advice (`0x85`, `0x08`)

O11 Session's own, in place of the Live spec's per-player switching (its 6.5): the room has one mode for everyone, its switch (15.3) alone. While the room is on (15.2) it is **Live**: every Live player is heard live, no one is moved to intervals for its health, and there is no interval audio at all; otherwise everyone plays intervals. Each Live client reports whether everything it should hear live sounds clean, and the server tells every Live client who has trouble: advice, so that a UI can suggest switching the room to intervals. Nothing changes the mode but `!live`.

**CLIENT_LIVE_REPORT (`0x85`)**, client → server, 8 bytes: u8 version 1, u8 flags (`0x01` clean: everything it should hear live sounds clean; `0x02` sending: its own source sends; `0x04` can't Live: its Live client doesn't run, or a sender is one it can't hear), u16 0, u32 fault: the peer id of the one player whose audio doesn't sound clean, or the reporter's own id when several don't or its own Live can't; 0 when clean. A client sends it once it has a `0x07`, whenever it changes, at most every 250 ms (`ENGINE.md` 16.7 says when it is clean). **Parsing**: under 8 bytes or version 0 is ignored; a later version is read as 1, and bytes after the eighth are ignored. The server keeps each Live player's latest; its sending bit becomes that player's peer flag `0x02` in the others' `0x07`.

**SERVER_LIVE_ROOM (`0x08`)**, server → client, version 3, fields in this order:

| Size | Field | Meaning |
|---|---|---|
| u8 | version | 3 |
| u8 | mode | 0 intervals, 1 Live: the room is on |
| u8 | reason | the advice: 0 none, 1 dropouts, 2 can't play Live |
| u8 | reserved | 0 |
| string | user | the player the advice names ("" with none) |
| u16 | count | how many players have trouble, the one named among them (0 with no advice); from version 2 |
| u8 | route | 0 none (a server without paths), 1 direct, 2 relay (15.10); from version 3 |
| u8 | flags | `0x01` direct possible (while relay), `0x02` the server has the relay fallback |
| u8 | why | while relay: 0 none, 1 the pair *a*, *b* had no direct path, 2 *a*'s client doesn't send directly, 3 *a*'s client sends through the relay only |
| u8 | reserved | 0 |
| string, string | a, b | the names *why* gives ("" for none) |

- The server works it out at every pass (at least every 100 ms while the UDP port is open) and sends it to each Live player (bit 16, not hidden) whose last one differs byte for byte; while Live isn't allowed, not to one that never had one. The advice is none while the room isn't Live.
- **Parsing**: under 4 bytes or version 0 is ignored. A mode other than 1 is intervals, a reason above 2 none; each name runs to its NUL or the end. Version 1 (where the reason said why the room was on intervals) has no count: 1 with a reason. A later version is read as 3: its count is 1 with a reason when the two bytes after the name's NUL are missing, its route none when the four after the count are; a route above 2 is none, a why above 3 none; bytes after the route's names are ignored.

**The advice.** At every pass while the room is Live, over the logged-in users who aren't hidden, in the room's order (users without channels count for nothing):

- **can't play Live**: no Live client (no bit 16, or no key), not among the senders, a report saying it can't, or still never bound or without a report 10 s after it joined; and, direct only (15.10), a sender with pairs that have no direct path;
- **dropouts**: bound once but not now, or the reports of others that blame it (their fault, or the reporter itself when the fault is 0). The reports of a player still joining (in its first 10 s, and not yet bound, reported and clean once), those that blame a player still joining, and those that blame another player who doesn't send by its own report, don't count.

A player has trouble from then until it has had none for 10 s, or has left. The advice names the player whose trouble now is the worst (can't play Live and a lost binding count as everyone's complaint, pairs without a path one each, else one for each report that blames it), the earliest in the room on a tie, else the first in the room of those whose trouble is still clearing, with that player's latest reason; the count is how many players have trouble.

**No interval audio while Live.** The Live players stop uploading at the boundary where they go Live (`ENGINE.md` 16.7). The server refuses an upload that begins more than one interval (60 × BPI / BPM s) after the room went Live (from an older client, which doesn't know): its BEGIN goes to no one and its WRITEs are dropped. The uploads begun before that are relayed to their end, and silent intervals (zero GUID or fourcc 0) are still relayed: they tell the others none is due. So a player who can't play Live (an older client, no UDP, no place among the senders) neither hears nor is heard while the room is Live; after `!live off` the players' audio comes back from their first interval after Live.

**Log lines** (`[live room]`, apart from the traffic line's `[live]`): `[live room] the room is Live` and `[live room] the room is on intervals: Live is off` when the room's state changes; while Live, when the advice's reason or name changes, `[live room] advice: <name>'s connection has dropouts` or `[live room] advice: <name> can't play Live`, each followed by ` (<count> players have trouble)` when more than one has, or `[live room] advice: none, everyone's Live sounds clean`; and the route lines of 15.10.
