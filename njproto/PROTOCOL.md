# NINJAM protocol specification

Revision 2, 2026-10-16 (revision 1 plus the exact server texts and defaults found by running a clean-room implementation against the reference; see section 14). Written from the behaviour of the NINJAM server and client in the Jamsa repository (`LeonidStern/Jamsa`, commit `e040ae1`), including the Jamsa extensions (codec negotiation and the room bitrate). It describes what goes over the wire and how each side must behave; it contains no code. It is meant to be enough to write a new client or server from scratch that works with existing NINJAM clients (ReaNINJAM, JamTaba, JamWide, the original NINJAM clients) and with the Jamsa server and plug-in.

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
| `0x80` | CLIENT_AUTH_USER | client → server | 4.2 |
| `0x81` | CLIENT_SET_USERMASK | client → server | 6.3 |
| `0x82` | CLIENT_SET_CHANNEL_INFO | client → server | 6.1 |
| `0x83` | CLIENT_UPLOAD_INTERVAL_BEGIN | client → server | 7.2 |
| `0x84` | CLIENT_UPLOAD_INTERVAL_WRITE | client → server | 7.2 |
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

`client_caps`: bit 0 = the user agreed to the licence (required when the server sent one; the server refuses otherwise). Bits 8-15 are codec capabilities (Jamsa extension, section 8): `0x100` = can decode Opus, `0x200` = can decode FLAC. All other bits must be 0 (JamWide uses bit 2 for a purpose of its own).

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
| `OPUS` | `O P U S` | Ogg Opus as in RFC 7845: `OpusHead` and `OpusTags` headers, then 20 ms Opus packets, 48 kHz. The encoder's pre-skip is in `OpusHead`; the last packet's granule position marks the end of the real audio and the decoder trims the padding after it (the padding is an extrapolation of the audio, which a client may use for its crossfade). Jamsa extension. |
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

Longer payloads are allowed and the extra bytes ignored (room for future fields). The server sends this only to clients that set at least one codec bit in `client_caps`, right after login and again whenever the room's codec or bitrate changes. Clients that set no codec bits never see it and never see non-Vorbis audio either.

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

The payload is **up to five strings**, each ending in `0x00`, including the last one (the Jamsa server rejects a message whose last byte isn't `0x00`; the original server accepted it). Fewer than five strings is normal; missing ones are empty. The first string is the command.

### 9.1 Client → server

| Fields | Meaning |
|---|---|
| `MSG`, text | say *text* to the room. Needs the chat privilege. Text starting with `!` is a command (9.4) and isn't broadcast |
| `PRIVMSG`, user, text | private message to *user*. Needs the chat privilege. *user* may also be a prefix of a name, when the name continues with `@` right after the prefix or the prefix itself contains `@` (anonymous names), so `bob` reaches `bob@1.2.3.x`; an ambiguous prefix is refused |
| `ADMIN`, command | a `/command` (9.5), sent without the slash |
| `SESSION`, guid, index, "offset length" | ReaNINJAM session chunk tag (9.3) |

### 9.2 Server → client

| Fields | Meaning |
|---|---|
| `MSG`, user, text | *user* said *text*. An empty user means the server itself (vote results, errors, kicks). The sender receives their own MSG back |
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
| `!vote bpm N`, `!vote bpi N` | cast a vote (needs the vote privilege; N within 40-400 or 2-64). Each user's latest vote per setting counts for `VotingTimeout` seconds (default 120). When the leading value has at least `threshold % × visible users` votes (rounded), the server applies it, broadcasts `[voting system] setting BPM to N` (or `BPI`) and a CONFIG_CHANGE, and clears the votes; otherwise it broadcasts `[voting system] leading candidate: a/b votes for N BPM [each vote expires in Ts]` to everyone. Voting is off when the threshold is over 100: `[voting system] Voting not enabled`; without the privilege: `[voting system] No vote permission`; bad arguments: `[voting system] !vote requires <bpm|bpi> <value> parameters`. These three go to the sender only. All vote lines are `MSG` with an empty user |
| `!codec [opus|flac|vorbis]`, `!bitrate [N]` | section 8.4 |
| other `!word` | `PRIVMSG * Unknown !command. Commands available: !vote, !topic, !codec, !bitrate` to the sender |

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
| Codec capability bits | `0x100` Opus, `0x200` FLAC; bits 8-15 reserved for codecs |

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
8. `!vote bpm 100` from enough users changes the tempo and sends CONFIG_CHANGE to all.
9. Codec mask: a client with `0x100` gets `0x06` with Opus; when a client without codec bits joins, everyone gets mask 0 and `[server] room audio is now Vorbis ...`; it comes back when they leave. An Opus interval reaches only clients with `0x100`; others get a zero-GUID BEGIN.
10. `!bitrate 128` → the mask carries 128 for everyone with codec bits; out-of-range values only answer the sender.
11. A client that stops sending keepalives is dropped after about 9 s; one that sends them stays.
12. Upload, in sequence, three intervals from one client while another client listens: the listener's playback is exactly one interval late and stays in time across a `!vote bpm` change.

## 14. Revision history

- **Revision 2 (2026-10-16):** a clean-room implementation written from revision 1 was run against the reference server and client. Audio, login, channels, subscriptions and forwarding matched at once; the differences were all in things revision 1 left open, now fixed in the text: the exact `[server]` and `[voting system]` chat texts and the fact that they are `MSG` lines with an empty user (8.5, 9.4, 9.5); the default Opus bitrate of 128 kbps; anonymous users don't get multiple logins unless configured; the same-name replacement runs before the capacity check; `!codec` may switch between allowed codecs under RequireCodec.
- **Revision 1 (2026-10-16):** first version, written from the Jamsa repository at `e040ae1`.
