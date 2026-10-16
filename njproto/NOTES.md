# NOTES

Clean-room implementation notes. The only input was `PROTOCOL.md` (revision 2a,
2026-10-16) plus general knowledge of C++, TCP sockets and SHA-1 (RFC 3174). No
other NINJAM, Jamsa or WDL source was read. Wherever the spec leaves room, the
choice is listed here. Everything that revision 2 pinned down (exact `[server]`
and `[voting system]` texts, the 128 kbps default, anonymous multi-login,
replacement before the capacity check, `!codec` under RequireCodec) is
implemented as written and no longer listed as a choice.

## Not implemented (deliberately)

- **Lobby mode and private rooms** (section 10): `/join` and `!join` answer
  with a "this server has no private rooms" PRIVMSG; `/stat` prints a one-line
  room summary instead of private-room statistics.
- **Session channels** (7.7) and **voice channels** (7.6): the server does not
  care about channel flags, it relays intervals of any flags the same way, and
  it relays `SESSION` chat tags (9.3). The client has no playback, so the
  receiver-side behaviour of these channel types does not exist here.
- **Audio, timing and playback** (5, 7.1, 7.5): no encoder, decoder, interval
  clock or queue. The client sends what the caller gives it when the caller
  says so, and delivers received bytes as they arrive. Acceptance test 12
  (playback exactly one interval late across a tempo change) therefore cannot
  be run; the data path it depends on (ordered, unchanged forwarding and
  CONFIG_CHANGE to all) is covered by the other loopback tests.
- **Access lists** (deny or reserve by address, 4.3) and the **session archive**
  (saving WRITE bytes to files, 7.3).
- **Legacy "play from cache" BEGIN** (nonzero GUID, fourcc 0; 7.4): the server
  relays it to subscribers and does not remember a transfer; the client passes
  it to `on_download_begin` and does not track it.
- The server listens on IPv4 only. The client resolves with `getaddrinfo` and
  accepts either family.

## Framing and codecs (sections 2, 3, 9)

- A header with payload length > 16384 or type `0xFF` is a protocol error:
  both sides close the connection. A payload of exactly 16384 is accepted.
- The send queue holds at most 512 framed messages; on overflow the connection
  is closed (client: `send queue overflow` failure; server: drops the user).
- Extra bytes after a fixed-layout message are ignored. For the three
  **list messages** (USERINFO, SET_CHANNEL_INFO, SET_USERMASK) a trailing
  partial record makes the whole message malformed and it is rejected; the
  spec's "extra bytes are harmless" is read as applying to fixed layouts.
- SET_CHANNEL_INFO: a record size below 4 is malformed; above 4 the padding
  is skipped. The encoder always writes 4.
- AUTH_REPLY decoding tolerates a missing string (flag only) and a missing
  `maxchan` after the string, since the spec marks the string optional and
  says `maxchan` is meaningless on failure.
- CODEC_MASK: `opus_kbps` is read when at least 8 bytes are present, longer
  payloads are accepted and ignored; the encoder writes the second field only
  when it is nonzero, as the spec says.
- CHAT: the encoder always writes all five strings, unused ones empty
  (`JOIN\0user\0\0\0\0`, section 9 revision 2a); the decoder accepts fewer,
  requires the final byte to be `0x00` (the Jamsa rule) and otherwise reports
  the message malformed. The server then ignores
  it silently (acceptance test 7). The same decoder is used client-side, which
  is safe because the server always terminates its strings. More than five
  strings: the extras are ignored.
- `Writer::str` stops at an embedded NUL so a frame can never be broken by a
  string containing one.

## Client (sections 4, 7, 8)

- Accepts `protocol_version` in `[0x00020000, 0x0002ffff)`; sends
  `client_version 0x00020000` unless overridden (`njcli --version`).
- Licence: the `on_licence` callback is synchronous; declining fails the
  connection locally with `licence not agreed` (nothing is sent). While the
  challenge carried a licence and the reply has not arrived, the keepalive
  interval is 45 s; afterwards it is the server's value (`server_caps >> 8 &
  0xFF`, 0 = 3 s).
- AUTH_USER is sent as soon as the challenge is processed. The password hash
  is `SHA1(SHA1(user ":" pass) + challenge)` with the username exactly as the
  caller gave it (`anonymous:name` included).
- SET_CHANNEL_INFO is sent immediately after a successful AUTH_REPLY, with
  whatever list the caller set, even an empty one (the server then lists the
  user with an empty channel 0). `setChannels()` after login resends the whole
  list, as 6.1 asks.
- `Options::auto_subscribe` sends `SET_USERMASK name 0xFFFFFFFF` the first time
  a user appears in USERINFO, which is what 6.3 describes for the reference
  client. Otherwise the caller subscribes explicitly.
- BEGIN is sent at once by `beginInterval()` rather than together with the
  first WRITE (7.1 describes that as what the reference does for latency; the
  wire format is the same). `writeInterval()` splits the data into WRITEs of
  at most 9216 bytes and sets the end flag on the last chunk. The 2048/8192
  minimum block sizes are not enforced because the client does not buffer
  encoder output; the caller decides chunking.
- Downloads: a BEGIN with nonzero GUID and nonzero fourcc opens a transfer;
  WRITEs for unknown GUIDs are ignored; a transfer with nothing received for
  8 s is reported through `on_download_end(guid, false)`; the end flag reports
  `true`. A zero-GUID BEGIN (silence) only triggers `on_download_begin`.
- Keepalive: a keepalive is sent when nothing was sent for *k* seconds; the
  client fails with `keepalive timeout` when nothing (any bytes) arrived for
  3 *k* seconds. `send_keepalives = false` exists only for the drop test.
- `preferredFourcc()` implements 8.3: no mask ever received -> Vorbis;
  FLAC when the mask has it and the caller wants FLAC, else Opus, else FLAC,
  else Vorbis.
- A refused login keeps the server's reason in `failReason()` even though the
  socket is closed by the server afterwards.

## Server (sections 4, 6, 7, 8, 9)

### Login

- Checks in this order: malformed AUTH_USER -> `invalid authorization reply`;
  version outside `0x00020000..0x0002ffff` -> `incorrect client version`;
  licence configured and bit 0 clear -> `license not agreed to`; account
  lookup (`invalid login/password`, also used when anonymous logins are off);
  status account; `RequireCodec` -> `this server needs a NINJAM client that
  plays Opus` (or FLAC); same-name replacement; capacity -> `server full`.
- Account names match case-insensitively; the hash is checked with the
  username as the client sent it; the effective name is the account's
  spelling.
- Anonymous users get chat and vote; the multiple-logins privilege only with
  `--anon-multi` (`ServerConfig::anonymous_privs`), as 4.2 revision 2 says.
  Without it a second anonymous login with the same name (same IP) replaces
  the first.
- "Multiple logins": suffixes `.2` .. `.16` are tried; when all are taken the
  login is refused with `server full` (the spec only says "at most 16").
- Anonymous rewriting follows 4.2 literally: name part cut to 16, `@` and `.`
  to `_`, `@<ip>` appended (last octet `x` with `--mask-ip`), bare `anonymous`
  becomes `anon@<ip>`, then every character outside letters, digits, `-`,
  `_`, `@`, `.` becomes `_`, and the result is cut to 128.
- Status account: an account with privilege `s`. Its password is checked like
  any other; it receives CONFIG_CHANGE, USERINFO, TOPIC and
  `USERCOUNT <n> <max>` and is closed without an AUTH_REPLY.
- Login sequence: AUTH_REPLY, CONFIG_CHANGE, USERINFO (sent even when there
  are no other users, as an empty `0x03`), one `PRIVMSG *` per MOTD line,
  `TOPIC "" <topic>`, then CODEC_MASK for clients with codec bits. The others
  get a USERINFO record (`active, index 0, empty name`) and `JOIN`.
- The CODEC_MASK sent at login already includes the new client in the room's
  codec computation. If that changed the effective codec, the 8.5 announcement
  goes to everyone right after the JOIN and the new mask to the other codec
  clients; nothing is announced when the new user is alone in the room.
- Pending-connection limits (default 64 total, 8 per address; `--max-pending
  T P`) are checked at accept time and a refused connection is closed without
  any message. Login timeout is 30 s (120 s with a licence) and answers
  `authorization timeout`. Before login, with a licence configured, the
  keepalive interval is 45 s.
- After a refused login the socket is closed once the reply has been flushed
  (or after 2 s at most).

### Channels and user lists

- Each user has a "visible" channel array: what the others have been told.
  SET_CHANNEL_INFO keeps the first `maxchan` records, treats `0x80` and
  missing indices as inactive, and sends the others only the records that
  differ (activity, name, volume, pan or flags). A removal record repeats the
  old name/volume/pan/flags.
- A list leaving no active channel (and `allow_hidden_users` off, the
  default) shows the user as channel 0 with an empty name, which is also the
  state right after login.
- On disconnect: inactive records for every visible active channel plus
  channel 0 (not duplicated when 0 was active), then `PART`, then any codec
  announcement.
- Hidden users (`h`) never appear in USERINFO/JOIN/PART, are not counted for
  capacity or votes, and are called `someone` in codec announcements; they
  still receive everything and can chat. The spec does not say whether hidden
  users should see JOIN/PART of others; here they do.
- Subscriptions are stored per connection by lower-cased username, bounded to
  1024 names; mask 0 removes one.

### Intervals

- BEGIN with channel >= `maxchan` or >= 32 is ignored. The matching
  DOWNLOAD_BEGIN goes to every subscriber whose mask has the channel bit.
- Jamsa substitution: a subscriber without the capability bit for the
  interval's codec gets a BEGIN with zero GUID and fourcc 0 (same channel and
  username). If the server does not relay the codec at all (`AllowCodecs`),
  every subscriber gets the silence BEGIN. Vorbis and unknown fourccs need no
  capability and are relayed to everyone.
- A zero-GUID BEGIN is relayed as-is and nothing is remembered. A transfer is
  remembered when GUID and fourcc are nonzero, even if it ended up with no
  receivers (it then just times out).
- Per-user limit of 64 open uploads: the oldest (by BEGIN time) is dropped.
  Transfers idle for 8 s are dropped. A WRITE is forwarded only when the
  sender is the connection that began the GUID, with the payload bytes passed
  through untouched; the end flag forgets the transfer.

### Chat

- `MSG` and `PRIVMSG` need the chat privilege (`No MSG permission`,
  `No PRIVMSG permission`). `MSG` is echoed to everyone including the sender.
- PRIVMSG target: an exact (case-insensitive) match wins; otherwise a prefix
  matches when the name continues with `@` right after it or the prefix
  contains `@`; two prefix matches -> `Username is ambiguous (specify partial
  IP): <name>`; none -> `No such user: <name>`.
- Unknown chat commands from a client are ignored silently.
- `!topic` -> `TOPIC "" <topic>` to the sender. Unknown `!word` ->
  `PRIVMSG * Unknown !command. Commands available: !vote, !topic, !codec, !bitrate`.
- `!vote` uses the 9.4 texts verbatim, all as `MSG ""`: `Voting not enabled`
  (threshold over 100), `No vote permission`, `!vote requires <bpm|bpi>
  <value> parameters` to the sender only; `setting BPM to N` and the
  `leading candidate` line to everyone. The threshold is
  `lround(percent * visible_users / 100)` with a minimum of 1; on a tie the
  lowest value leads. Votes expire after `VotingTimeout` seconds and are
  dropped on disconnect. The "Voting not enabled" check runs before the
  privilege check (order not given by the spec).
- `/topic` with text needs the topic privilege and is broadcast as
  `TOPIC <user> <text>`; without text the sender gets `TOPIC "" <topic>`.
  `/kick name` matches exactly, `/kick name*` kicks every prefix match; each
  victim gives `MSG "" User X kicked by Y` to everyone (the victim included,
  before it is closed); no match -> `User "<name>" not found!` to the sender.
  `/bpm` and `/bpi` answer `BPM parameter must be between 20 and 400` /
  `BPI parameter must be between 2 and 1024` to the sender on bad values.
  Unknown `/command` -> `Valid commands: /topic, /kick, /bpm, /bpi, /stat, /join`.
- `SESSION` tags are relayed to everyone else with the sender's name inserted
  as the second field.

### Codec negotiation

- Effective codec exactly as 8.2: the chosen codec if every logged-in client
  has its bit (a chosen Vorbis is 0 for everyone); else Opus if everyone can
  and it is relayed; else FLAC likewise; else Vorbis. With no clients it is the
  chosen codec. Hidden users count here (they receive audio).
- `AllowCodecs` defaults to Opus; `--allow-codecs none` relays Vorbis only
  (mask 0 for everyone, `!codec opus` answers `this server doesn't allow
  Opus`). `RequireCodec` adds the required codec to the allowed set by itself,
  makes it the chosen codec, and refuses only `!codec vorbis`; switching
  between allowed codecs still works. A configured chosen codec that is not
  allowed falls back to Opus, then FLAC, then Vorbis.
- The room Opus bitrate defaults to 128 kbps; a configured value outside
  32-510 is replaced by 128 with a warning in the log. `ServerConfig::opus_kbps
  = 0` (`--opus-kbps none`) means "no bitrate, everyone sends their own", which
  the spec's `!bitrate` status text describes but gives no configuration for.
- All `!codec` and `!bitrate` texts are the 8.5 ones verbatim, sent as
  `MSG ""`. The `<who>` in "can't play it" lines is the first logged-in user
  (in connection order) whose caps lack the codec in question, `someone` when
  hidden. After a successful `!codec X` or `!bitrate N` the chat line goes to
  everyone first, then the CODEC_MASK to every client with codec bits whose
  last mask differs (8.1 revision 2a): the server remembers the last mask sent
  per connection and skips identical ones, so an unchanged `!codec` or a
  repeated `!bitrate N` produces the chat line only.
- The "any other change" announcement (`room audio is now <E>` without a
  reason) only happens when the effective codec is neither the chosen one nor
  blocked by a user lacking the chosen codec; it is kept as a fallback.

## Tools

- `njcli` always prints JSON lines (`--json` is accepted and changes nothing)
  because that is what a test harness wants; there is no human-readable mode.
  `--dump DIR` creates DIR when it does not exist.
- `--upload FILE --bpm-interval` is read as "upload the file as one interval
  at every interval boundary of the room tempo (60 x BPI / BPM seconds)";
  without `--bpm-interval` the file is uploaded once after login. `njcli`
  does not implement the 7.1 rule of sending the previous interval's end at
  the boundary; it ends each upload right away.
- `njserver --user name:pass:privs`: `privs` is the letter string documented
  in README.md; a missing privilege part means `cv`. `--anon-multi` gives
  anonymous users the multiple-logins privilege; `--max-pending T P` sets the
  pending-connection limits.

## Open questions about the spec

1. What happens after sixteen multi-logins of one name. Chosen: `server full`.
2. Whether a transfer whose codec is not relayed should still be remembered
   (7.3 says "if the GUID is nonzero and the fourcc is nonzero, remember").
   Chosen: remembered.
3. Whether hidden users receive JOIN/PART/USERINFO about others. Chosen: yes.
4. Whether the login USERINFO is sent when there is nobody else. Chosen: yes,
   empty.
5. Where CODEC_MASK sits in the login sequence ("right after login"). Chosen:
   after TOPIC.
6. Whether `maxchan` can really be absent from a success AUTH_REPLY (4.3 says
   "present whenever the string is"). The decoder tolerates its absence.
7. PRIVMSG prefix rule when an exact name is also a prefix of another
   (`bob` and `bob@1.2.3.4`). Chosen: exact match wins.
8. Spec 4.2 says the server "ignores" the hash for anonymous logins; this
   server does, but still requires a well-formed 20-byte field.
9. Which user is named in "<who>'s client can't play it" when several lack
   the codec. Chosen: the earliest connection.
10. How a server configuration expresses "no Opus bitrate" now that 128 is the
    default and out-of-range values are ignored. Chosen: `--opus-kbps none`.
