// njproto - message framing and codecs. See PROTOCOL.md sections 2-9.
#include "njproto/message.h"

#include <cstring>

namespace njproto {

// ------------------------------------------------------------------ fourcc

std::uint32_t fourccFromString(const std::string& s) {
    if (s.size() != 4) return 0;
    return fourcc(s[0], s[1], s[2], s[3]);
}

std::string fourccToString(std::uint32_t f) {
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i) {
        unsigned char c = static_cast<unsigned char>((f >> (8 * i)) & 0xFF);
        s[static_cast<std::size_t>(i)] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return s;
}

std::uint32_t codecCapForFourcc(std::uint32_t f) {
    if (f == kFourccOpus) return kCapOpus;
    if (f == kFourccFlac) return kCapFlac;
    return 0;
}

bool guidIsZero(const Guid& g) {
    for (auto b : g)
        if (b) return false;
    return true;
}

std::string guidToHex(const Guid& g) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : g) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 15]);
    }
    return s;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool guidFromHex(const std::string& hex, Guid& out) {
    if (hex.size() != 32) return false;
    for (std::size_t i = 0; i < 16; ++i) {
        int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}

// ----------------------------------------------------------------- framing

FrameStatus parseFrame(const std::uint8_t* data, std::size_t len, RawMessage& out,
                       std::size_t& consumed) {
    consumed = 0;
    if (len < kHeaderSize) return FrameStatus::NeedMore;
    std::uint8_t type = data[0];
    std::uint32_t plen = static_cast<std::uint32_t>(data[1]) |
                         (static_cast<std::uint32_t>(data[2]) << 8) |
                         (static_cast<std::uint32_t>(data[3]) << 16) |
                         (static_cast<std::uint32_t>(data[4]) << 24);
    if (type == kMsgInvalid || plen > kMaxPayload) return FrameStatus::Error;
    if (len < kHeaderSize + plen) return FrameStatus::NeedMore;
    out.type = type;
    out.payload.assign(data + kHeaderSize, data + kHeaderSize + plen);
    consumed = kHeaderSize + plen;
    return FrameStatus::Ok;
}

bool appendFrame(std::vector<std::uint8_t>& out, std::uint8_t type,
                 const std::uint8_t* payload, std::size_t len) {
    if (len > kMaxPayload || type == kMsgInvalid) return false;
    out.push_back(type);
    auto n = static_cast<std::uint32_t>(len);
    out.push_back(static_cast<std::uint8_t>(n & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
    if (len) out.insert(out.end(), payload, payload + len);
    return true;
}

bool appendFrame(std::vector<std::uint8_t>& out, std::uint8_t type,
                 const std::vector<std::uint8_t>& payload) {
    return appendFrame(out, type, payload.data(), payload.size());
}

// --------------------------------------------------------------- byte I/O

bool Reader::u8(std::uint8_t& v) {
    if (remaining() < 1) return false;
    v = *p_++;
    return true;
}
bool Reader::s8(std::int8_t& v) {
    std::uint8_t u;
    if (!u8(u)) return false;
    v = static_cast<std::int8_t>(u);
    return true;
}
bool Reader::u16(std::uint16_t& v) {
    if (remaining() < 2) return false;
    v = static_cast<std::uint16_t>(p_[0] | (p_[1] << 8));
    p_ += 2;
    return true;
}
bool Reader::s16(std::int16_t& v) {
    std::uint16_t u;
    if (!u16(u)) return false;
    v = static_cast<std::int16_t>(u);
    return true;
}
bool Reader::u32(std::uint32_t& v) {
    if (remaining() < 4) return false;
    v = static_cast<std::uint32_t>(p_[0]) | (static_cast<std::uint32_t>(p_[1]) << 8) |
        (static_cast<std::uint32_t>(p_[2]) << 16) | (static_cast<std::uint32_t>(p_[3]) << 24);
    p_ += 4;
    return true;
}
bool Reader::bytes(std::uint8_t* dst, std::size_t n) {
    if (remaining() < n) return false;
    if (n) std::memcpy(dst, p_, n);
    p_ += n;
    return true;
}
bool Reader::str(std::string& v) {
    const auto* nul = static_cast<const std::uint8_t*>(std::memchr(p_, 0, remaining()));
    if (!nul) return false;
    v.assign(reinterpret_cast<const char*>(p_), static_cast<std::size_t>(nul - p_));
    p_ = nul + 1;
    return true;
}
bool Reader::skip(std::size_t n) {
    if (remaining() < n) return false;
    p_ += n;
    return true;
}

void Writer::u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
}
void Writer::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void Writer::bytes(const std::uint8_t* src, std::size_t n) {
    if (n) buf_.insert(buf_.end(), src, src + n);
}
void Writer::str(const std::string& s) {
    // A NUL inside the string would truncate it on the wire; stop there so the
    // frame stays well-formed.
    std::size_t n = s.find('\0');
    if (n == std::string::npos) n = s.size();
    bytes(reinterpret_cast<const std::uint8_t*>(s.data()), n);
    buf_.push_back(0);
}

// ---------------------------------------------------------- 0x00 challenge

int AuthChallenge::keepaliveSeconds() const {
    int k = static_cast<int>((server_caps >> 8) & 0xFF);
    return k ? k : kDefaultKeepaliveSeconds;
}

std::vector<std::uint8_t> encode(const AuthChallenge& m) {
    Writer w;
    w.bytes(m.challenge.data(), m.challenge.size());
    std::uint32_t caps = m.server_caps;
    if (m.has_licence) caps |= kServerCapLicence;
    w.u32(caps);
    w.u32(m.protocol_version);
    if (m.has_licence) w.str(m.licence);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, AuthChallenge& out) {
    Reader r(p);
    if (!r.bytes(out.challenge.data(), out.challenge.size())) return false;
    if (!r.u32(out.server_caps) || !r.u32(out.protocol_version)) return false;
    out.has_licence = (out.server_caps & kServerCapLicence) != 0;
    out.licence.clear();
    if (out.has_licence && !r.str(out.licence)) return false;
    return true;
}

// ---------------------------------------------------------- 0x80 auth user

std::vector<std::uint8_t> encode(const AuthUser& m) {
    Writer w;
    w.bytes(m.passhash.data(), m.passhash.size());
    w.str(m.username);
    w.u32(m.client_caps);
    w.u32(m.client_version);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, AuthUser& out) {
    Reader r(p);
    return r.bytes(out.passhash.data(), out.passhash.size()) && r.str(out.username) &&
           r.u32(out.client_caps) && r.u32(out.client_version);
}

// --------------------------------------------------------- 0x01 auth reply

std::vector<std::uint8_t> encode(const AuthReply& m) {
    Writer w;
    w.u8(m.flag);
    w.str(m.text);
    w.u8(m.maxchan);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, AuthReply& out) {
    Reader r(p);
    if (!r.u8(out.flag)) return false;
    out.has_text = false;
    out.text.clear();
    out.maxchan = 0;
    if (r.atEnd()) return true;              // the string is optional
    if (!r.str(out.text)) return false;
    out.has_text = true;
    if (r.atEnd()) return true;              // tolerate a missing maxchan
    return r.u8(out.maxchan);
}

// -------------------------------------------------------------- 0x02 config

std::vector<std::uint8_t> encode(const ConfigChange& m) {
    Writer w;
    w.u16(m.bpm);
    w.u16(m.bpi);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, ConfigChange& out) {
    Reader r(p);
    return r.u16(out.bpm) && r.u16(out.bpi);
}

// ------------------------------------------------------------ 0x03 userinfo

std::vector<std::uint8_t> encode(const UserInfoChange& m) {
    Writer w;
    for (const auto& rec : m.records) {
        w.u8(rec.active ? 1 : 0);
        w.u8(rec.index);
        w.s16(rec.volume);
        w.s8(rec.pan);
        w.u8(rec.flags);
        w.str(rec.username);
        w.str(rec.channel);
    }
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, UserInfoChange& out) {
    out.records.clear();
    Reader r(p);
    while (!r.atEnd()) {
        UserInfoRecord rec;
        std::uint8_t active;
        if (!r.u8(active) || !r.u8(rec.index) || !r.s16(rec.volume) || !r.s8(rec.pan) ||
            !r.u8(rec.flags) || !r.str(rec.username) || !r.str(rec.channel))
            return false;                    // a truncated record is malformed
        rec.active = active != 0;
        out.records.push_back(std::move(rec));
    }
    return true;
}

// -------------------------------------------------------- 0x82 channel info

std::vector<std::uint8_t> encode(const SetChannelInfo& m) {
    Writer w;
    w.u16(4);                                // record size after the name
    for (const auto& ch : m.channels) {
        w.str(ch.name);
        w.s16(ch.volume);
        w.s8(ch.pan);
        w.u8(ch.flags);
    }
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, SetChannelInfo& out) {
    out.channels.clear();
    Reader r(p);
    std::uint16_t n;
    if (!r.u16(n)) return false;
    if (n < 4) return false;                 // must hold volume, pan and flags
    while (!r.atEnd()) {
        ChannelInfoRecord ch;
        if (!r.str(ch.name) || !r.s16(ch.volume) || !r.s8(ch.pan) || !r.u8(ch.flags) ||
            !r.skip(static_cast<std::size_t>(n) - 4))
            return false;
        out.channels.push_back(std::move(ch));
    }
    return true;
}

// ------------------------------------------------------------ 0x81 usermask

std::vector<std::uint8_t> encode(const SetUserMask& m) {
    Writer w;
    for (const auto& e : m.entries) {
        w.str(e.username);
        w.u32(e.mask);
    }
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, SetUserMask& out) {
    out.entries.clear();
    Reader r(p);
    while (!r.atEnd()) {
        UserMaskEntry e;
        if (!r.str(e.username) || !r.u32(e.mask)) return false;
        out.entries.push_back(std::move(e));
    }
    return true;
}

// -------------------------------------------------------- 0x83 upload begin

std::vector<std::uint8_t> encode(const UploadBegin& m) {
    Writer w;
    w.bytes(m.guid.data(), m.guid.size());
    w.u32(m.estsize);
    w.u32(m.fourcc);
    w.u8(m.channel);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, UploadBegin& out) {
    Reader r(p);
    return r.bytes(out.guid.data(), out.guid.size()) && r.u32(out.estsize) &&
           r.u32(out.fourcc) && r.u8(out.channel);
}

// -------------------------------------------------- 0x84 / 0x05 interval write

std::vector<std::uint8_t> encode(const IntervalWrite& m) {
    Writer w;
    w.bytes(m.guid.data(), m.guid.size());
    w.u8(m.flags);
    w.bytes(m.data.data(), m.data.size());
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, IntervalWrite& out) {
    Reader r(p);
    if (!r.bytes(out.guid.data(), out.guid.size()) || !r.u8(out.flags)) return false;
    out.data.assign(r.ptr(), r.ptr() + r.remaining());
    return true;
}

// ------------------------------------------------------ 0x04 download begin

std::vector<std::uint8_t> encode(const DownloadBegin& m) {
    Writer w;
    w.bytes(m.guid.data(), m.guid.size());
    w.u32(m.estsize);
    w.u32(m.fourcc);
    w.u8(m.channel);
    w.str(m.username);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, DownloadBegin& out) {
    Reader r(p);
    return r.bytes(out.guid.data(), out.guid.size()) && r.u32(out.estsize) &&
           r.u32(out.fourcc) && r.u8(out.channel) && r.str(out.username);
}

// ---------------------------------------------------------- 0x06 codec mask

std::vector<std::uint8_t> encode(const CodecMask& m) {
    Writer w;
    w.u32(m.codecs);
    if (m.opus_kbps) w.u32(m.opus_kbps);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, CodecMask& out) {
    Reader r(p);
    if (!r.u32(out.codecs)) return false;
    out.opus_kbps = 0;
    if (r.remaining() >= 4) r.u32(out.opus_kbps);
    return true;                             // longer payloads are allowed
}

// ---------------------------------------------------------------- 0xC0 chat

std::vector<std::uint8_t> encode(const ChatMessage& m) {
    // Always all five strings, unused ones empty: some receivers index the
    // fields without checking (section 9).
    Writer w;
    for (const auto& f : m.fields) w.str(f);
    return std::move(w.data());
}

bool decode(const std::vector<std::uint8_t>& p, ChatMessage& out) {
    for (auto& f : out.fields) f.clear();
    if (p.empty() || p.back() != 0) return false;   // Jamsa: must end in NUL
    Reader r(p);
    for (std::size_t i = 0; i < out.fields.size() && !r.atEnd(); ++i)
        if (!r.str(out.fields[i])) return false;
    return true;                                     // extra strings ignored
}

}  // namespace njproto
