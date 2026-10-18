// o11proto - Live mode wire helpers: SERVER_LIVE, SipHash-2-4, the HELLO
// cookie and the system random source. See PROTOCOL.md section 15.
#include "o11proto/live.h"

#include <algorithm>
#include <cstring>

#include "o11proto/message.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace o11proto {
namespace live {

// ----------------------------------------------------------------- 0x07

std::vector<std::uint8_t> encode(const ServerLive& m) {
    Writer w;
    w.u8(kServerLiveVersion);
    w.u8(static_cast<std::uint8_t>(kServerLiveFixed));
    w.u8(m.flags);
    w.u8(0);
    w.u16(m.udp_port);
    w.u16(m.live_kbps);
    w.u32(m.peer_id);
    w.u32(static_cast<std::uint32_t>(m.key));
    w.u32(static_cast<std::uint32_t>(m.key >> 32));
    w.u16(m.frame_samples);
    w.u16(m.max_peers);
    w.u32(0);
    w.u32(0);
    for (const auto& r : m.peers) {
        w.u32(r.id);
        w.u8(r.flags);
        w.str(r.name);
    }
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, ServerLive& out) {
    if (p.size() < kServerLiveFixed) return false;
    const std::uint8_t* d = p.data();
    std::size_t fixed = d[1];
    if (d[0] == 0 || fixed < kServerLiveFixed || fixed > p.size()) return false;
    out = ServerLive();
    out.version = d[0];                            // above 1: read as version 1
    out.flags = d[2];
    out.udp_port = getLE16(d + 4);
    out.live_kbps = getLE16(d + 6);
    out.peer_id = getLE32(d + 8);
    out.key = getLE64(d + 12);
    out.frame_samples = getLE16(d + 20);
    out.max_peers = getLE16(d + 22);
    std::size_t at = fixed;
    for (std::size_t read = 0; read < kMaxPeerRecords && p.size() - at >= 6; ++read) {
        std::size_t nul = at + 5;
        while (nul < p.size() && d[nul] != 0) ++nul;
        if (nul >= p.size()) break;                // no terminator: the list ends here
        PeerRecord r;
        r.id = getLE32(d + at);
        r.flags = d[at + 4];
        r.name.assign(reinterpret_cast<const char*>(d + at + 5), nul - (at + 5));
        at = nul + 1;
        if (r.id == 0 || r.id == out.peer_id) continue;   // skipped
        out.peers.push_back(std::move(r));
    }
    return true;
}

// ----------------------------------------------------------- 0x09, 0x86

std::vector<std::uint8_t> encode(const LivePaths& m) {
    Writer w;
    w.u8(kLivePathsVersion);
    w.u8(static_cast<std::uint8_t>(kLivePathsFixed));
    w.u16(static_cast<std::uint16_t>(kLivePathRecord));
    w.u32(0);
    for (const auto& r : m.peers) {
        w.u32(r.id);
        w.u32(static_cast<std::uint32_t>(r.key));
        w.u32(static_cast<std::uint32_t>(r.key >> 32));
        std::uint8_t a[4];
        std::memcpy(a, &r.public_ip, 4);
        for (std::uint8_t b : a) w.u8(b);
        w.u16(r.public_port);
        std::memcpy(a, &r.lan_ip, 4);
        for (std::uint8_t b : a) w.u8(b);
        w.u16(r.lan_port);
    }
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, LivePaths& out) {
    if (p.size() < kLivePathsFixed) return false;
    const std::uint8_t* d = p.data();
    const std::size_t fixed = d[1], rec = getLE16(d + 2);
    if (d[0] == 0 || fixed < kLivePathsFixed || fixed > p.size() || rec < kLivePathRecord) return false;
    out = LivePaths();
    out.version = d[0];
    for (std::size_t at = fixed, n = 0; n < kMaxPeerRecords && p.size() - at >= rec; at += rec, ++n) {
        PathRecord r;
        r.id = getLE32(d + at);
        r.key = getLE64(d + at + 4);
        std::memcpy(&r.public_ip, d + at + 12, 4);
        r.public_port = getLE16(d + at + 16);
        std::memcpy(&r.lan_ip, d + at + 18, 4);
        r.lan_port = getLE16(d + at + 22);
        if (r.id == 0 || r.key == 0) continue;
        out.peers.push_back(r);
    }
    return true;
}

std::vector<std::uint8_t> encode(const LiveLan& m) {
    Writer w;
    w.u8(1);
    w.u8(0);
    w.u16(m.port);
    std::uint8_t a[4];
    std::memcpy(a, &m.ip, 4);
    for (std::uint8_t b : a) w.u8(b);
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, LiveLan& out) {
    if (p.size() < kLiveLanSize || p[0] == 0) return false;
    out = LiveLan();
    out.port = getLE16(p.data() + 2);
    std::memcpy(&out.ip, p.data() + 4, 4);
    return true;
}

std::vector<std::uint8_t> encode(const PathsReport& m) {
    Writer w;
    w.u8(kLivePathsReportVersion);
    w.u8(m.flags);
    const std::size_t n = std::min<std::size_t>(m.peers.size(), kMaxPeerRecords);
    w.u16(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        w.u32(m.peers[i].id);
        w.u8(static_cast<std::uint8_t>(m.peers[i].state));
        w.u8(0);
        w.u16(0);
    }
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, PathsReport& out) {
    if (p.size() < kLivePathsReportFixed || p[0] == 0) return false;
    out = PathsReport();
    out.flags = p[1];
    const std::size_t count = getLE16(p.data() + 2);
    for (std::size_t i = 0, at = kLivePathsReportFixed;
         i < count && i < kMaxPeerRecords && at + kLivePathsReportRecord <= p.size();
         ++i, at += kLivePathsReportRecord) {
        PathsReport::Entry e;
        e.id = getLE32(p.data() + at);
        e.state = p[at + 4] <= 2 ? static_cast<PathState>(p[at + 4]) : PathState::Opening;
        if (e.id != 0) out.peers.push_back(e);
    }
    return true;
}

// -------------------------------------------------------------- SipHash

namespace {

inline std::uint64_t rotl(std::uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

inline void sipRound(std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2, std::uint64_t& v3) {
    v0 += v1;
    v1 = rotl(v1, 13);
    v1 ^= v0;
    v0 = rotl(v0, 32);
    v2 += v3;
    v3 = rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotl(v1, 17);
    v1 ^= v2;
    v2 = rotl(v2, 32);
}

}  // namespace

std::uint64_t siphash24(const std::uint8_t key[16], const std::uint8_t* msg, std::size_t len) {
    const std::uint64_t k0 = getLE64(key), k1 = getLE64(key + 8);
    std::uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    std::uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    std::uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    std::uint64_t v3 = k1 ^ 0x7465646279746573ULL;
    std::size_t whole = len - len % 8;
    for (std::size_t i = 0; i < whole; i += 8) {
        std::uint64_t m = getLE64(msg + i);
        v3 ^= m;
        sipRound(v0, v1, v2, v3);
        sipRound(v0, v1, v2, v3);
        v0 ^= m;
    }
    std::uint64_t b = static_cast<std::uint64_t>(len & 0xFF) << 56;
    for (std::size_t i = 0; i < len % 8; ++i) b |= static_cast<std::uint64_t>(msg[whole + i]) << (8 * i);
    v3 ^= b;
    sipRound(v0, v1, v2, v3);
    sipRound(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    for (int i = 0; i < 4; ++i) sipRound(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

std::uint64_t helloCookie(const std::uint8_t secret[16], std::uint32_t peer_id, std::uint64_t key,
                          const std::uint8_t ip[4], const std::uint8_t port[2], std::uint32_t epoch) {
    std::uint8_t m[22];
    putLE32(m, peer_id);
    putLE32(m + 4, static_cast<std::uint32_t>(key));
    putLE32(m + 8, static_cast<std::uint32_t>(key >> 32));
    for (int i = 0; i < 4; ++i) m[12 + i] = ip[i];
    m[16] = port[0];
    m[17] = port[1];
    putLE32(m + 18, epoch);
    return siphash24(secret, m, sizeof(m));
}

std::uint64_t pairKey(const std::uint8_t secret[16], std::uint32_t a, std::uint32_t b) {
    std::uint8_t m[8];
    putLE32(m, a < b ? a : b);
    putLE32(m + 4, a < b ? b : a);
    const std::uint64_t k = siphash24(secret, m, sizeof(m));
    return k != 0 ? k : 1;
}

bool constantTimeEqual(std::uint64_t a, std::uint64_t b) {
    volatile std::uint64_t diff = a ^ b;
    std::uint64_t d = diff;
    d |= d >> 32;
    d |= d >> 16;
    d |= d >> 8;
    return (d & 0xFF) == 0;
}

// --------------------------------------------------------- randomness

bool systemRandom(void* buf, std::size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    return got == n;
#endif
}

}  // namespace live
}  // namespace o11proto
