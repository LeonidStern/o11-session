// o11proto engine - Live mode's AOO subset: OSC and binary messages
// (live-mode spec 5.4-5.6).
#include "live_aoo.h"

#include <cmath>
#include <cstring>

namespace o11proto {
namespace engine {
namespace aoo {

// ------------------------------------------------------------ time tags

namespace {

std::uint64_t toNtp(std::chrono::nanoseconds since, std::uint64_t offset_seconds) {
    const auto ns = since.count();
    const std::uint64_t secs = static_cast<std::uint64_t>(ns / 1000000000) + offset_seconds;
    const std::uint64_t frac = (static_cast<std::uint64_t>(ns % 1000000000) << 32) / 1000000000ULL;
    return (secs << 32) | frac;
}

}  // namespace

std::uint64_t ntpWallNow() {
    // 70 years (17 leap days) from 1900 to the Unix epoch.
    return toNtp(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch()),
                 2208988800ULL);
}

std::uint64_t ntpFromSteady(std::chrono::steady_clock::time_point t) {
    return toNtp(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()), 0);
}

double ntpDiffSeconds(std::uint64_t later, std::uint64_t earlier) {
    const std::int64_t d = static_cast<std::int64_t>(later - earlier);
    return static_cast<double>(d) / 4294967296.0;
}

// --------------------------------------------------------------- writer

namespace {

class Writer {
public:
    explicit Writer(std::uint8_t* buf) : p_(buf) {}
    void str(const char* s) {
        std::size_t n = std::strlen(s);
        std::memcpy(p_ + at_, s, n);
        at_ += n;
        do p_[at_++] = 0;
        while (at_ % 4);
    }
    void i32(std::int32_t v) {
        putBE32(p_ + at_, static_cast<std::uint32_t>(v));
        at_ += 4;
    }
    void f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        putBE32(p_ + at_, u);
        at_ += 4;
    }
    void time(std::uint64_t t) {
        putBE64(p_ + at_, t);
        at_ += 8;
    }
    void blob(const std::uint8_t* b, std::size_t n) {
        i32(static_cast<std::int32_t>(n));
        std::memcpy(p_ + at_, b, n);
        at_ += n;
        while (at_ % 4) p_[at_++] = 0;
    }
    std::size_t size() const { return at_; }

private:
    std::uint8_t* p_;
    std::size_t at_ = 0;
};

}  // namespace

std::size_t writeStart(std::uint8_t* buf, const StartInfo& s) {
    Writer w(buf);
    w.str("/aoo/sink/1/start");
    w.str(",isiiiiiisbtiiNNi");
    w.i32(kId);
    w.str(kVersion);
    w.i32(s.stream);
    w.i32(s.start_seq);
    w.i32(s.format);
    w.i32(s.channels);
    w.i32(s.rate);
    w.i32(s.block);
    w.str("opus");
    std::uint8_t ext[4];
    putBE32(ext, static_cast<std::uint32_t>(kOpusRestrictedLowDelay));
    w.blob(ext, sizeof(ext));
    w.time(s.time);
    w.i32(s.latency);
    w.i32(s.codec_delay);
    w.i32(s.offset);
    return w.size();
}

std::size_t writeStop(std::uint8_t* buf, std::int32_t stream, std::int32_t next_seq) {
    Writer w(buf);
    w.str("/aoo/sink/1/stop");
    w.str(",iiii");
    w.i32(kId);
    w.i32(stream);
    w.i32(next_seq);
    w.i32(0);
    return w.size();
}

std::size_t writeSinkPing(std::uint8_t* buf, std::uint64_t t1) {
    Writer w(buf);
    w.str("/aoo/sink/1/ping");
    w.str(",it");
    w.i32(kId);
    w.time(t1);
    return w.size();
}

std::size_t writeSinkPong(std::uint8_t* buf, std::uint64_t t1, std::uint64_t t2, std::uint64_t t3) {
    Writer w(buf);
    w.str("/aoo/sink/1/pong");
    w.str(",ittt");
    w.i32(kId);
    w.time(t1);
    w.time(t2);
    w.time(t3);
    return w.size();
}

std::size_t writeSourcePing(std::uint8_t* buf, std::uint64_t t1) {
    Writer w(buf);
    w.str("/aoo/src/1/ping");
    w.str(",it");
    w.i32(kId);
    w.time(t1);
    return w.size();
}

std::size_t writeSourcePong(std::uint8_t* buf, std::uint64_t t1, std::uint64_t t2, std::uint64_t t3, float loss) {
    Writer w(buf);
    w.str("/aoo/src/1/pong");
    w.str(",itttf");
    w.i32(kId);
    w.time(t1);
    w.time(t2);
    w.time(t3);
    w.f32(loss);
    return w.size();
}

std::size_t writeStartRequest(std::uint8_t* buf) {
    Writer w(buf);
    w.str("/aoo/src/1/start");
    w.str(",is");
    w.i32(kId);
    w.str(kVersion);
    return w.size();
}

std::size_t writeStopRequest(std::uint8_t* buf, std::int32_t stream) {
    Writer w(buf);
    w.str("/aoo/src/1/stop");
    w.str(",ii");
    w.i32(kId);
    w.i32(stream);
    return w.size();
}

// --------------------------------------------------------------- reader

namespace {

// A NUL-terminated string at `at`, padded to 4. Returns its length or -1.
long paddedString(const std::uint8_t* p, std::size_t n, std::size_t at, std::size_t& next) {
    std::size_t end = at;
    while (end < n && p[end] != 0) ++end;
    if (end >= n) return -1;
    std::size_t padded = (end + 1 + 3) & ~static_cast<std::size_t>(3);
    if (padded > n) return -1;
    next = padded;
    return static_cast<long>(end - at);
}

bool commandOf(const char* s, std::size_t n, Cmd& out) {
    struct Name {
        const char* text;
        Cmd cmd;
    };
    static const Name names[] = {
        {"start", Cmd::Start}, {"stop", Cmd::Stop}, {"data", Cmd::Data}, {"ping", Cmd::Ping}, {"pong", Cmd::Pong}};
    for (const auto& nm : names)
        if (std::strlen(nm.text) == n && std::memcmp(nm.text, s, n) == 0) {
            out = nm.cmd;
            return true;
        }
    return false;
}

}  // namespace

bool parseOsc(const std::uint8_t* p, std::size_t n, OscMessage& out) {
    out = OscMessage();
    if (n < 4 || p[0] != '/') return false;      // "#bundle" and anything else
    std::size_t at = 0;
    long alen = paddedString(p, n, 0, at);
    if (alen < 0) return false;
    const char* a = reinterpret_cast<const char*>(p);
    static const char kSink[] = "/aoo/sink/1/", kSrc[] = "/aoo/src/1/";
    std::size_t prefix;
    if (alen > 12 && std::memcmp(a, kSink, 12) == 0) {
        out.dest = Dest::Sink;
        prefix = 12;
    } else if (alen > 11 && std::memcmp(a, kSrc, 11) == 0) {
        out.dest = Dest::Source;
        prefix = 11;
    } else {
        return false;
    }
    if (!commandOf(a + prefix, static_cast<std::size_t>(alen) - prefix, out.cmd)) return false;
    if (at >= n) return false;                   // no type tags: no arguments, so no id
    std::size_t args_at = 0;
    long tlen = paddedString(p, n, at, args_at);
    if (tlen < 1 || p[at] != ',') return false;
    const char* tags = reinterpret_cast<const char*>(p + at + 1);
    std::size_t ntags = static_cast<std::size_t>(tlen) - 1;
    if (ntags > sizeof(out.args) / sizeof(out.args[0])) return false;
    at = args_at;
    for (std::size_t k = 0; k < ntags; ++k) {
        OscArg& g = out.args[k];
        g.tag = tags[k];
        switch (g.tag) {
        case 'i':
        case 'f':
            if (n - at < 4) return false;
            if (g.tag == 'i') g.i = static_cast<std::int32_t>(getBE32(p + at));
            else {
                std::uint32_t u = getBE32(p + at);
                std::memcpy(&g.f, &u, 4);
            }
            at += 4;
            break;
        case 'd':
        case 't':
            if (n - at < 8) return false;
            if (g.tag == 't') g.t = getBE64(p + at);
            else {
                std::uint64_t u = getBE64(p + at);
                std::memcpy(&g.d, &u, 8);
            }
            at += 8;
            break;
        case 's': {
            std::size_t next = 0;
            long l = paddedString(p, n, at, next);
            if (l < 0) return false;
            g.s = reinterpret_cast<const char*>(p + at);
            g.len = static_cast<std::size_t>(l);
            at = next;
            break;
        }
        case 'b': {
            if (n - at < 4) return false;
            std::int32_t l = static_cast<std::int32_t>(getBE32(p + at));
            at += 4;
            if (l < 0 || static_cast<std::size_t>(l) > n - at) return false;
            g.s = reinterpret_cast<const char*>(p + at);
            g.len = static_cast<std::size_t>(l);
            at += (static_cast<std::size_t>(l) + 3) & ~static_cast<std::size_t>(3);
            if (at > n) return false;
            break;
        }
        case 'N':
            break;
        default:
            return false;                        // a tag this subset doesn't have
        }
    }
    out.count = static_cast<int>(ntags);
    return out.count >= 1 && out.args[0].tag == 'i' && out.args[0].i == kId;
}

bool versionOk(const char* s, std::size_t n) {
    std::size_t i = 0;
    long major = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        major = major * 10 + (s[i] - '0');
        if (major > 1000) return false;
        ++i;
    }
    if (i == 0 || i >= n || s[i] != '.') return false;
    ++i;
    if (i >= n || s[i] < '0' || s[i] > '9') return false;
    return major == 2;
}

bool readStart(const OscMessage& m, StartInfo& out) {
    static const char kTags[] = "isiiiiiisbtii";   // the fields read; the metadata and offset follow
    if (m.count < 16) return false;
    for (int k = 0; k < 13; ++k)
        if (m.args[k].tag != kTags[k]) return false;
    if (m.args[15].tag != 'i') return false;
    if (!versionOk(m.args[1].s, m.args[1].len)) return false;
    out.stream = m.args[2].i;
    out.start_seq = m.args[3].i;
    out.format = m.args[4].i;
    out.channels = m.args[5].i;
    out.rate = m.args[6].i;
    out.block = m.args[7].i;
    if (out.stream < 0 || out.start_seq < 0 || out.format < 0) return false;
    if (m.args[8].len != 4 || std::memcmp(m.args[8].s, "opus", 4) != 0) return false;
    if (out.channels < 1 || out.channels > 2 || out.rate != 48000) return false;
    if (out.block != 120 && out.block != 240 && out.block != 480) return false;   // only these decode
    if (m.args[9].len < 4) return false;         // the Opus application, not checked
    out.time = m.args[10].t;
    auto clamp = [](std::int32_t v, std::int32_t hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    out.latency = clamp(m.args[11].i, out.rate * 10);
    out.codec_delay = clamp(m.args[12].i, out.rate / 10);
    out.offset = clamp(m.args[15].i, out.rate / 10);
    return true;
}

// ------------------------------------------------------ binary messages

bool parseData(const std::uint8_t* p, std::size_t n, Block& out) {
    out = Block();
    if (n < 16) return false;
    out.stream = static_cast<std::int32_t>(getBE32(p + 4));
    out.seq = static_cast<std::int32_t>(getBE32(p + 8));
    out.channel = p[12];
    out.flags = p[13];
    const std::size_t size = getBE16(p + 14);
    if (out.stream < 0 || out.seq < 0) return false;
    std::size_t at = 16;
    std::size_t total = size, frames = size ? 1 : 0, index = 0, msg_bytes = 0;
    if (out.flags & kFlagFrames) {
        if (n - at < 8) return false;
        total = getBE32(p + at);
        frames = getBE16(p + at + 4);
        index = getBE16(p + at + 6);
        at += 8;
    }
    if (out.flags & kFlagMessages) {
        if (n - at < 4) return false;
        msg_bytes = getBE32(p + at);
        at += 4;
    }
    if (out.flags & kFlagRate) {
        if (n - at < 8) return false;
        std::uint64_t u = getBE64(p + at);
        double rate;
        std::memcpy(&rate, &u, 8);
        if (!std::isfinite(rate) || rate <= 0) return false;
        at += 8;
    }
    if (out.flags & kFlagTime) {
        if (n - at < 8) return false;
        at += 8;
    }
    if (n - at < size) return false;
    out.xrun = (out.flags & kFlagXrun) != 0;
    out.split = frames > 1 || index != 0 || total != size;
    if (out.split) return true;                  // the caller treats it as lost
    if (msg_bytes % 4 != 0 || msg_bytes > size) return false;
    out.data = p + at + msg_bytes;               // stream messages are skipped, never read
    out.len = size - msg_bytes;
    return true;
}

std::size_t writeData(std::uint8_t* buf, std::int32_t stream, std::int32_t seq, double rate,
                      const std::uint8_t* packet, std::size_t len) {
    buf[0] = kTypeSink;
    buf[1] = 0;
    buf[2] = kId;
    buf[3] = kId;
    putBE32(buf + 4, static_cast<std::uint32_t>(stream));
    putBE32(buf + 8, static_cast<std::uint32_t>(seq));
    buf[12] = 0;
    buf[13] = kFlagRate;
    putBE16(buf + 14, static_cast<std::uint16_t>(len));
    std::uint64_t u;
    std::memcpy(&u, &rate, 8);
    putBE64(buf + 16, u);
    std::memcpy(buf + 24, packet, len);
    return 24 + len;
}

std::size_t writeXrun(std::uint8_t* buf, std::int32_t stream, std::int32_t seq) {
    buf[0] = kTypeSink;
    buf[1] = 0;
    buf[2] = kId;
    buf[3] = kId;
    putBE32(buf + 4, static_cast<std::uint32_t>(stream));
    putBE32(buf + 8, static_cast<std::uint32_t>(seq));
    buf[12] = 0;
    buf[13] = kFlagXrun;
    putBE16(buf + 14, 0);
    return 16;
}

std::size_t writeRequest(std::uint8_t* buf, std::int32_t stream, const std::int32_t* seqs, int count) {
    buf[0] = kTypeSource;
    buf[1] = 0;
    buf[2] = kId;
    buf[3] = kId;
    putBE32(buf + 4, static_cast<std::uint32_t>(stream));
    putBE32(buf + 8, static_cast<std::uint32_t>(count));
    std::size_t at = 12;
    for (int i = 0; i < count; ++i) {
        putBE32(buf + at, static_cast<std::uint32_t>(seqs[i]));
        putBE16(buf + at + 4, 0xFFFF);           // frame offset -1: the whole block
        putBE16(buf + at + 6, 0);
        at += 8;
    }
    return at;
}

bool parseRequest(const std::uint8_t* p, std::size_t n, std::int32_t& stream, std::int32_t* seqs, int& count) {
    count = 0;
    if (n < 12) return false;
    stream = static_cast<std::int32_t>(getBE32(p + 4));
    std::int32_t c = static_cast<std::int32_t>(getBE32(p + 8));
    if (c < 0 || static_cast<std::size_t>(c) > (n - 12) / 8) return false;
    if (c > kMaxRequests) c = kMaxRequests;
    for (std::int32_t i = 0; i < c; ++i) {
        const std::uint8_t* r = p + 12 + 8 * static_cast<std::size_t>(i);
        std::int32_t seq = static_cast<std::int32_t>(getBE32(r));
        std::int16_t offset = static_cast<std::int16_t>(getBE16(r + 4));
        std::uint16_t set = getBE16(r + 6);
        if (seq < 0) continue;
        if (offset < 0 || (offset == 0 && (set & 1))) seqs[count++] = seq;   // a block is one frame
    }
    return true;
}

}  // namespace aoo
}  // namespace engine
}  // namespace o11proto
