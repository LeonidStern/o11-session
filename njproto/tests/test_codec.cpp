// Round trips and malformed-input checks for every message codec.
#include <string>

#include "check.h"
#include "njproto/message.h"

using namespace njproto;

static Guid testGuid(std::uint8_t seed) {
    Guid g;
    for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<std::uint8_t>(seed + i);
    return g;
}

TEST(framing_round_trip) {
    std::vector<std::uint8_t> buf;
    std::vector<std::uint8_t> p = {1, 2, 3};
    CHECK(appendFrame(buf, kMsgChat, p));
    CHECK(appendFrame(buf, kMsgKeepalive, std::vector<std::uint8_t>()));
    CHECK_EQ(buf.size(), 5u + 3u + 5u);
    CHECK_EQ(buf[0], 0xC0);
    CHECK_EQ(buf[1], 3);
    CHECK_EQ(buf[2], 0);
    RawMessage m;
    std::size_t used = 0;
    CHECK(parseFrame(buf.data(), buf.size(), m, used) == FrameStatus::Ok);
    CHECK_EQ(used, 8u);
    CHECK_EQ(m.type, kMsgChat);
    CHECK(m.payload == p);
    CHECK(parseFrame(buf.data() + used, buf.size() - used, m, used) == FrameStatus::Ok);
    CHECK_EQ(m.type, kMsgKeepalive);
    CHECK(m.payload.empty());
    // Partial header / partial payload.
    CHECK(parseFrame(buf.data(), 3, m, used) == FrameStatus::NeedMore);
    CHECK(parseFrame(buf.data(), 6, m, used) == FrameStatus::NeedMore);
    // Oversized payload and type 0xFF are protocol errors.
    std::uint8_t big[5] = {0xC0, 0x01, 0x40, 0, 0};   // 16385
    CHECK(parseFrame(big, 5, m, used) == FrameStatus::Error);
    std::uint8_t bad[5] = {0xFF, 0, 0, 0, 0};
    CHECK(parseFrame(bad, 5, m, used) == FrameStatus::Error);
    std::uint8_t max[5] = {0x05, 0x00, 0x40, 0, 0};   // 16384 is allowed
    CHECK(parseFrame(max, 5, m, used) == FrameStatus::NeedMore);
    std::vector<std::uint8_t> toolong(kMaxPayload + 1);
    CHECK(!appendFrame(buf, kMsgChat, toolong));
}

TEST(auth_challenge) {
    AuthChallenge c;
    for (std::size_t i = 0; i < 8; ++i) c.challenge[i] = static_cast<std::uint8_t>(i * 7);
    c.server_caps = 5u << 8;
    c.has_licence = true;
    c.licence = "be nice";
    auto p = encode(c);
    CHECK_EQ(p.size(), 8u + 4u + 4u + 8u);
    CHECK_EQ(p[8], 1);                                 // licence flag set by encode
    AuthChallenge d;
    CHECK(decode(p, d));
    CHECK(d.challenge == c.challenge);
    CHECK(d.has_licence);
    CHECK_EQ(d.licence, "be nice");
    CHECK_EQ(d.keepaliveSeconds(), 5);
    CHECK_EQ(d.protocol_version, kProtocolVersion);
    // No licence: 16 bytes, keepalive default.
    AuthChallenge n;
    auto pn = encode(n);
    CHECK_EQ(pn.size(), 16u);
    CHECK(decode(pn, d));
    CHECK(!d.has_licence);
    CHECK_EQ(d.keepaliveSeconds(), 3);
    // Malformed: too short, licence flag without a terminated string.
    CHECK(!decode(std::vector<std::uint8_t>(15), d));
    std::vector<std::uint8_t> nolic(16, 0);
    nolic[8] = 1;
    CHECK(!decode(nolic, d));
    nolic.push_back('x');
    CHECK(!decode(nolic, d));
    nolic.push_back(0);
    CHECK(decode(nolic, d));
    CHECK_EQ(d.licence, "x");
    // Extra trailing bytes are harmless.
    pn.push_back(0xAA);
    CHECK(decode(pn, d));
}

TEST(auth_user) {
    AuthUser a;
    for (std::size_t i = 0; i < 20; ++i) a.passhash[i] = static_cast<std::uint8_t>(255 - i);
    a.username = "anonymous:bob";
    a.client_caps = kCapOpus | 1;
    a.client_version = 0x00020001;
    auto p = encode(a);
    CHECK_EQ(p.size(), 20u + 14u + 8u);
    AuthUser d;
    CHECK(decode(p, d));
    CHECK(d.passhash == a.passhash);
    CHECK_EQ(d.username, "anonymous:bob");
    CHECK_EQ(d.client_caps, kCapOpus | 1u);
    CHECK_EQ(d.client_version, 0x00020001u);
    p.pop_back();
    CHECK(!decode(p, d));                               // version truncated
    std::vector<std::uint8_t> noterm(20, 0);
    noterm.push_back('a');
    CHECK(!decode(noterm, d));                          // unterminated name
}

TEST(auth_reply) {
    AuthReply r;
    r.flag = 1;
    r.text = "bob@1.2.3.4";
    r.maxchan = 32;
    auto p = encode(r);
    AuthReply d;
    CHECK(decode(p, d));
    CHECK(d.success());
    CHECK(d.has_text);
    CHECK_EQ(d.text, "bob@1.2.3.4");
    CHECK_EQ(d.maxchan, 32);
    // Flag only, string only.
    CHECK(decode(std::vector<std::uint8_t>{0}, d));
    CHECK(!d.success());
    CHECK(!d.has_text);
    std::vector<std::uint8_t> so = {0, 'n', 'o', 0};
    CHECK(decode(so, d));
    CHECK_EQ(d.text, "no");
    CHECK_EQ(d.maxchan, 0);
    CHECK(!decode(std::vector<std::uint8_t>{}, d));
    CHECK(!decode(std::vector<std::uint8_t>{1, 'a'}, d));
}

TEST(config_change) {
    ConfigChange c;
    c.bpm = 120;
    c.bpi = 8;
    auto p = encode(c);
    CHECK_EQ(p.size(), 4u);
    CHECK_EQ(p[0], 120);
    ConfigChange d;
    CHECK(decode(p, d));
    CHECK_EQ(d.bpm, 120);
    CHECK_EQ(d.bpi, 8);
    CHECK(!decode(std::vector<std::uint8_t>{1, 2, 3}, d));
}

TEST(userinfo) {
    UserInfoChange u;
    UserInfoRecord r;
    r.active = true;
    r.index = 3;
    r.volume = -100;
    r.pan = -5;
    r.flags = 2;
    r.username = "al";
    r.channel = "gtr";
    u.records.push_back(r);
    r.active = false;
    r.index = 0;
    r.channel = "";
    u.records.push_back(r);
    auto p = encode(u);
    UserInfoChange d;
    CHECK(decode(p, d));
    CHECK_EQ(d.records.size(), 2u);
    CHECK(d.records[0].active);
    CHECK_EQ(d.records[0].index, 3);
    CHECK_EQ(d.records[0].volume, -100);
    CHECK_EQ(d.records[0].pan, -5);
    CHECK_EQ(d.records[0].flags, 2);
    CHECK_EQ(d.records[0].username, "al");
    CHECK_EQ(d.records[0].channel, "gtr");
    CHECK(!d.records[1].active);
    CHECK_EQ(d.records[1].channel, "");
    CHECK(decode(std::vector<std::uint8_t>{}, d));
    CHECK(d.records.empty());
    p.pop_back();                                       // truncated last record
    CHECK(!decode(p, d));
}

TEST(set_channel_info) {
    SetChannelInfo s;
    ChannelInfoRecord a;
    a.name = "vox";
    s.channels.push_back(a);
    ChannelInfoRecord gap;
    gap.flags = kChanInactive;
    s.channels.push_back(gap);
    ChannelInfoRecord b;
    b.name = "bass";
    b.volume = 10;
    b.pan = 3;
    s.channels.push_back(b);
    auto p = encode(s);
    CHECK_EQ(p[0], 4);
    CHECK_EQ(p[1], 0);
    SetChannelInfo d;
    CHECK(decode(p, d));
    CHECK_EQ(d.channels.size(), 3u);
    CHECK_EQ(d.channels[0].name, "vox");
    CHECK(!d.channels[1].active());
    CHECK_EQ(d.channels[2].name, "bass");
    CHECK_EQ(d.channels[2].volume, 10);
    CHECK_EQ(d.channels[2].pan, 3);
    // A record size of 6 with zero padding is accepted.
    std::vector<std::uint8_t> big = {6, 0, 'x', 0, 0, 0, 0, 0, 0, 0};
    CHECK(decode(big, d));
    CHECK_EQ(d.channels.size(), 1u);
    CHECK_EQ(d.channels[0].name, "x");
    // Malformed: record size < 4, missing header, truncated record.
    CHECK(!decode(std::vector<std::uint8_t>{3, 0, 'x', 0, 0, 0, 0}, d));
    CHECK(!decode(std::vector<std::uint8_t>{4}, d));
    CHECK(!decode(std::vector<std::uint8_t>{4, 0, 'x', 0, 0}, d));
    CHECK(decode(std::vector<std::uint8_t>{4, 0}, d));
    CHECK(d.channels.empty());
}

TEST(set_usermask) {
    SetUserMask m;
    m.entries.push_back({"bob", 0xFFFFFFFFu});
    m.entries.push_back({"al", 0});
    auto p = encode(m);
    SetUserMask d;
    CHECK(decode(p, d));
    CHECK_EQ(d.entries.size(), 2u);
    CHECK_EQ(d.entries[0].username, "bob");
    CHECK_EQ(d.entries[0].mask, 0xFFFFFFFFu);
    CHECK_EQ(d.entries[1].mask, 0u);
    p.pop_back();
    CHECK(!decode(p, d));
    CHECK(!decode(std::vector<std::uint8_t>{'b', 'o', 'b'}, d));
}

TEST(upload_begin_and_write) {
    UploadBegin b;
    b.guid = testGuid(1);
    b.estsize = 123;
    b.fourcc = kFourccOpus;
    b.channel = 2;
    auto p = encode(b);
    CHECK_EQ(p.size(), 25u);
    CHECK_EQ(p[20], 'O');
    CHECK_EQ(p[23], 'S');
    UploadBegin d;
    CHECK(decode(p, d));
    CHECK(d.guid == b.guid);
    CHECK_EQ(d.estsize, 123u);
    CHECK_EQ(d.fourcc, kFourccOpus);
    CHECK_EQ(d.channel, 2);
    p.pop_back();
    CHECK(!decode(p, d));

    IntervalWrite w;
    w.guid = testGuid(9);
    w.flags = 1;
    w.data = {9, 8, 7};
    auto pw = encode(w);
    CHECK_EQ(pw.size(), 20u);
    IntervalWrite dw;
    CHECK(decode(pw, dw));
    CHECK(dw.guid == w.guid);
    CHECK(dw.last());
    CHECK(dw.data == w.data);
    std::vector<std::uint8_t> empty_write(17, 0);
    CHECK(decode(empty_write, dw));
    CHECK(dw.data.empty());
    CHECK(!decode(std::vector<std::uint8_t>(16), dw));
}

TEST(download_begin) {
    DownloadBegin b;
    b.guid = testGuid(5);
    b.fourcc = kFourccVorbis;
    b.channel = 1;
    b.username = "al@1.2.3.4";
    auto p = encode(b);
    DownloadBegin d;
    CHECK(decode(p, d));
    CHECK(d.guid == b.guid);
    CHECK_EQ(d.fourcc, kFourccVorbis);
    CHECK_EQ(d.channel, 1);
    CHECK_EQ(d.username, "al@1.2.3.4");
    p.pop_back();                                       // no NUL
    CHECK(!decode(p, d));
    CHECK(!decode(std::vector<std::uint8_t>(25), d));   // upload layout without the name
    DownloadBegin silence;
    CHECK(guidIsZero(silence.guid));
}

TEST(codec_mask) {
    CodecMask m;
    m.codecs = kCapOpus;
    auto p = encode(m);
    CHECK_EQ(p.size(), 4u);                             // kbps omitted when zero
    m.opus_kbps = 128;
    p = encode(m);
    CHECK_EQ(p.size(), 8u);
    CodecMask d;
    CHECK(decode(p, d));
    CHECK_EQ(d.codecs, kCapOpus);
    CHECK_EQ(d.opus_kbps, 128u);
    CHECK(decode(std::vector<std::uint8_t>{0, 1, 0, 0}, d));
    CHECK_EQ(d.codecs, kCapOpus);
    CHECK_EQ(d.opus_kbps, 0u);
    p.push_back(1);                                     // longer payloads allowed
    p.push_back(2);
    CHECK(decode(p, d));
    CHECK_EQ(d.opus_kbps, 128u);
    CHECK(!decode(std::vector<std::uint8_t>{0, 1}, d));
}

TEST(chat) {
    ChatMessage m("MSG", "bob", "hello there");
    auto p = encode(m);
    CHECK_EQ(p.size(), 4u + 4u + 12u + 2u);        // always five strings
    CHECK_EQ(p.back(), 0);
    CHECK_EQ(p[p.size() - 2], 0);
    ChatMessage d;
    CHECK(decode(p, d));
    CHECK_EQ(d.command(), "MSG");
    CHECK_EQ(d.fields[1], "bob");
    CHECK_EQ(d.fields[2], "hello there");
    CHECK_EQ(d.fields[3], "");
    // JOIN is JOIN\0user\0\0\0\0.
    std::vector<std::uint8_t> join = {'J', 'O', 'I', 'N', 0, 'u', 0, 0, 0, 0};
    CHECK(encode(ChatMessage("JOIN", "u")) == join);
    ChatMessage t("TOPIC", "", "the topic");
    auto pt = encode(t);
    CHECK_EQ(pt.size(), 6u + 1u + 10u + 2u);
    CHECK(decode(pt, d));
    CHECK_EQ(d.fields[1], "");
    CHECK_EQ(d.fields[2], "the topic");
    CHECK_EQ(encode(ChatMessage("MSG")).size(), 8u);
    // Fewer than five strings is still accepted on receive.
    std::vector<std::uint8_t> two = {'P', 'A', 'R', 'T', 0, 'u', 0};
    CHECK(decode(two, d));
    CHECK_EQ(d.fields[0], "PART");
    CHECK_EQ(d.fields[1], "u");
    CHECK_EQ(d.fields[2], "");
    // Missing final NUL is rejected (Jamsa rule); empty payload too.
    std::vector<std::uint8_t> unterminated = {'M', 'S', 'G', 0, 'h', 'i'};
    CHECK(!decode(unterminated, d));
    CHECK(!decode(std::vector<std::uint8_t>{}, d));
    // More than five strings: the extras are ignored.
    std::vector<std::uint8_t> six;
    for (int i = 0; i < 6; ++i) {
        six.push_back(static_cast<std::uint8_t>('a' + i));
        six.push_back(0);
    }
    CHECK(decode(six, d));
    CHECK_EQ(d.fields[4], "e");
}

TEST(fourcc_and_guid_helpers) {
    CHECK_EQ(fourccToString(kFourccOpus), "OPUS");
    CHECK_EQ(fourccToString(kFourccVorbis), "OGGv");
    CHECK_EQ(fourccFromString("FLAC"), kFourccFlac);
    CHECK_EQ(fourccFromString("FLA"), 0u);
    CHECK_EQ(codecCapForFourcc(kFourccOpus), kCapOpus);
    CHECK_EQ(codecCapForFourcc(kFourccOpusRaw), kCapOpus);
    CHECK_EQ(fourccToString(kFourccOpusRaw), "OPUr");
    CHECK_EQ(codecCapForFourcc(kFourccFlac), kCapFlac);
    CHECK_EQ(codecCapForFourcc(kFourccVorbis), 0u);
    Guid g = testGuid(0x10);
    std::string hex = guidToHex(g);
    CHECK_EQ(hex.size(), 32u);
    CHECK_EQ(hex.substr(0, 4), "1011");
    Guid back;
    CHECK(guidFromHex(hex, back));
    CHECK(back == g);
    CHECK(!guidFromHex("zz", back));
    CHECK(!guidFromHex(std::string(32, 'g'), back));
}

TEST_MAIN()
