// o11proto - Live mode: the TCP message SERVER_LIVE (0x07), the UDP datagram
// layout, the HELLO cookie and the constants shared by the relay and the
// client (PROTOCOL.md section 15, "Live mode").
//
// The TCP message and the UDP headers are little-endian, like the rest of
// the protocol. What a DATA datagram carries (the audio payload) is opaque
// here: the relay never looks inside it.
#ifndef O11PROTO_LIVE_H
#define O11PROTO_LIVE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o11proto {
namespace live {

// ---------------------------------------------------------------- TCP side

constexpr std::uint32_t kCapLive = 0x10000;        // client_caps bit 16
constexpr std::uint8_t kMsgServerLive = 0x07;      // SERVER_LIVE, server -> client
constexpr std::uint8_t kServerLiveVersion = 1;
constexpr std::size_t kServerLiveFixed = 32;       // fixed_len: where the peer list starts
constexpr std::size_t kMaxPeerRecords = 256;       // read at most this many

// SERVER_LIVE flags
constexpr std::uint8_t kFlagRoomOn = 0x01;
constexpr std::uint8_t kFlagUdpOpen = 0x02;
constexpr std::uint8_t kFlagMaySend = 0x04;
// peer record flags
constexpr std::uint8_t kPeerBound = 0x01;
constexpr std::uint8_t kPeerSending = 0x02;        // its own report says it sends (live_room.h)

// ---------------------------------------------------------------- UDP side

constexpr std::uint8_t kMagic0 = 'N';
constexpr std::uint8_t kMagic1 = 'L';
constexpr std::uint8_t kUdpVersion = 1;
constexpr std::uint8_t kTypeHello = 1;             // HELLO, HELLO_ACK
constexpr std::uint8_t kTypeData = 2;
constexpr std::size_t kClientHeader = 20;          // magic, version, type, sender_id, key, dest_id
constexpr std::size_t kServerHeader = 8;           // magic, version, type, from_id
constexpr std::size_t kHelloSize = 40;
constexpr std::size_t kHelloAckSize = 24;
constexpr std::size_t kMaxPayload = 1452;          // DATA payload 1..1452
constexpr std::size_t kMaxServerDatagram = 1472;   // the relay drops anything longer
constexpr std::size_t kMaxClientDatagram = 1500;   // a client drops anything longer

// ------------------------------------------------------- defaults and limits

constexpr int kDefaultKbps = 128;
constexpr int kMinKbps = 32;
constexpr int kMaxKbps = 510;
constexpr int kDefaultFrame = 240;                 // 120, 240 or 480 samples at 48 kHz
constexpr int kDefaultMaxPeers = 8;
constexpr int kMaxMaxPeers = 64;
constexpr int kHelloIntervalMs = 2000;
constexpr int kHelloTimestamps = 8;                // remembered by the client
constexpr int kAckMaxRoundTripUs = 10000000;       // 10 s
constexpr int kFreshSeconds = 20;
constexpr int kStatsSeconds = 60;
constexpr int kCookieEpochSeconds = 30;
constexpr int kMaxCopies = 64;                     // copies of one datagram
constexpr int kRelaySocketBuffer = 1 << 20;        // 1 MiB
constexpr int kClientSocketBuffer = 256 << 10;     // 256 KiB

inline bool validFrame(int frame) { return frame == 120 || frame == 240 || frame == 480; }

// ---------------------------------------------------------------- 0x07

struct PeerRecord {
    std::uint32_t id = 0;
    std::uint8_t flags = 0;                        // kPeerBound, kPeerSending
    std::string name;
};

struct ServerLive {
    std::uint8_t version = kServerLiveVersion;     // a later version is read as 1
    std::uint8_t flags = 0;
    std::uint16_t udp_port = 0;                    // 0 = the TCP port the client connected to
    std::uint16_t live_kbps = kDefaultKbps;
    std::uint32_t peer_id = 0;
    std::uint64_t key = 0;
    std::uint16_t frame_samples = kDefaultFrame;
    std::uint16_t max_peers = kDefaultMaxPeers;
    std::vector<PeerRecord> peers;                 // the room's other senders, in join order

    bool roomOn() const { return (flags & kFlagRoomOn) != 0; }
    bool udpOpen() const { return (flags & kFlagUdpOpen) != 0; }
    bool maySend() const { return (flags & kFlagMaySend) != 0; }
};

// Writes version 1, fixed_len 32 and zero reserved bytes.
std::vector<std::uint8_t> encode(const ServerLive& m);
// The parsing rules of the Live section: false (ignore the message) when the
// payload is shorter than 32 bytes, the version is 0, or fixed_len is below
// 32 or beyond the payload. A later version is read as version 1. The peer
// list ends at the first record without 6 bytes left or without a NUL, and
// after 256 records; records with id 0 or the receiver's own id are skipped.
bool decode(const std::vector<std::uint8_t>& p, ServerLive& out);

// ---------------------------------------------------------------- cookie

// SipHash-2-4 of Aumasson and Bernstein with a 16-byte key.
std::uint64_t siphash24(const std::uint8_t key[16], const std::uint8_t* msg, std::size_t len);

// The HELLO cookie: SipHash-2-4 keyed with the relay's secret over
// peer_id (u32 LE), key (u64 LE), the source IPv4 address and UDP port as on
// the wire (network order) and the epoch (u32 LE), 22 bytes in all.
std::uint64_t helloCookie(const std::uint8_t secret[16], std::uint32_t peer_id, std::uint64_t key,
                          const std::uint8_t ip[4], const std::uint8_t port[2], std::uint32_t epoch);

// Compares two 64-bit values without an early exit.
bool constantTimeEqual(std::uint64_t a, std::uint64_t b);

// Fills `buf` with `n` bytes from the operating system's cryptographic random
// source. Returns false when none can be read.
bool systemRandom(void* buf, std::size_t n);

// ------------------------------------------------- little-endian helpers

inline void putLE16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void putLE32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline void putLE64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t getLE16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t getLE32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::uint64_t getLE64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(getLE32(p)) | (static_cast<std::uint64_t>(getLE32(p + 4)) << 32);
}

}  // namespace live
}  // namespace o11proto

#endif  // O11PROTO_LIVE_H
