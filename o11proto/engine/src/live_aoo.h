// o11proto engine - Live mode's audio payload: the subset of AOO 2.0 that
// Live clients exchange (live-mode spec section 5, PROTOCOL.md 15.9),
// written from that description alone. Internal to the engine.
//
// Every message is big-endian. The binary forms are the audio block
// (`/data`, sink type 0x81) and the resend request (source type 0x80); the
// others are single OSC 1.0 messages addressed to `/aoo/sink/1/...` or
// `/aoo/src/1/...`. Readers never read past the datagram.
#ifndef O11PROTO_ENGINE_LIVE_AOO_H
#define O11PROTO_ENGINE_LIVE_AOO_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace o11proto {
namespace engine {
namespace aoo {

constexpr int kId = 1;                          // every source and sink is AOO id 1
constexpr char kVersion[] = "2.0-test4";
constexpr std::size_t kMaxMessage = 1452;       // the Live payload limit
constexpr std::size_t kMaxBlockBytes = 1276;    // one Opus packet of one frame (1275) and a spare byte
constexpr std::uint8_t kTypeSource = 0x80;      // binary, to a source: the resend request
constexpr std::uint8_t kTypeSink = 0x81;        // binary, to a sink: /data
constexpr std::uint8_t kFlagRate = 0x01;
constexpr std::uint8_t kFlagFrames = 0x02;
constexpr std::uint8_t kFlagMessages = 0x04;
constexpr std::uint8_t kFlagXrun = 0x08;
constexpr std::uint8_t kFlagTime = 0x10;
constexpr std::int32_t kOpusRestrictedLowDelay = 2051;
constexpr std::int32_t kCodecDelay = 120;       // the encoder's latency, 48 kHz samples
constexpr int kMaxRequests = 32;                // per resend request message
constexpr std::int32_t kMaxSequence = 0x7ffffffe;   // after it the source starts again at 0

// ------------------------------------------------------------ time tags

// NTP 32.32: seconds since 1900 in the high half.
std::uint64_t ntpWallNow();                     // wall clock, for pings
std::uint64_t ntpFromSteady(std::chrono::steady_clock::time_point t);   // the /start stream time
double ntpDiffSeconds(std::uint64_t later, std::uint64_t earlier);      // signed

// --------------------------------------------------------- OSC messages

enum class Dest { Sink, Source };
enum class Cmd { Start, Stop, Data, Ping, Pong, Other };

struct OscArg {
    char tag = 0;                               // i f d s b t N
    std::int32_t i = 0;
    float f = 0;
    double d = 0;
    std::uint64_t t = 0;
    const char* s = nullptr;                    // strings and blobs point into the datagram
    std::size_t len = 0;                        // string length (without the NUL) or blob size
};

struct OscMessage {
    Dest dest = Dest::Sink;
    Cmd cmd = Cmd::Other;
    int count = 0;                              // arguments, the first (the sender's id 1) included
    OscArg args[20];
};

// A single message to `/aoo/sink/1/<cmd>` or `/aoo/src/1/<cmd>` whose first
// argument is the i32 1. False for anything else, a bundle included.
bool parseOsc(const std::uint8_t* p, std::size_t n, OscMessage& out);

// The version string rule: a decimal number, a period and at least one
// digit, whose number is 2.
bool versionOk(const char* s, std::size_t n);

struct StartInfo {
    std::int32_t stream = 0;
    std::int32_t start_seq = 0;                 // the sequence number of the next block
    std::int32_t format = 0;
    std::int32_t channels = 2;
    std::int32_t rate = 48000;
    std::int32_t block = 240;                   // the frame size
    std::uint64_t time = 0;                     // the sender's monotonic clock, NTP form
    std::int32_t latency = 0;                   // the source's latency, 48 kHz samples
    std::int32_t codec_delay = kCodecDelay;
    std::int32_t offset = 0;
};

// Each writer fills `buf` (at least kMaxMessage bytes) and returns the size.
std::size_t writeStart(std::uint8_t* buf, const StartInfo& s);                       // /aoo/sink/1/start, 116
std::size_t writeStop(std::uint8_t* buf, std::int32_t stream, std::int32_t next_seq); // /aoo/sink/1/stop, 44
std::size_t writeSinkPing(std::uint8_t* buf, std::uint64_t t1);                      // /aoo/sink/1/ping, 36
std::size_t writeSinkPong(std::uint8_t* buf, std::uint64_t t1, std::uint64_t t2, std::uint64_t t3);   // 56
std::size_t writeSourcePing(std::uint8_t* buf, std::uint64_t t1);                    // /aoo/src/1/ping, 32
std::size_t writeSourcePong(std::uint8_t* buf, std::uint64_t t1, std::uint64_t t2, std::uint64_t t3,
                            float loss);                                             // /aoo/src/1/pong, 56
std::size_t writeStartRequest(std::uint8_t* buf);                                    // /aoo/src/1/start
std::size_t writeStopRequest(std::uint8_t* buf, std::int32_t stream);                // /aoo/src/1/stop

// Reads a /start's fields (after the receive filter's format checks:
// false when it isn't an acceptable Opus stream). The latency is clamped to
// 0..10 s, the codec delay and offset to 0..rate/10.
bool readStart(const OscMessage& m, StartInfo& out);

// ------------------------------------------------------ binary messages

struct Block {
    std::int32_t stream = 0;
    std::int32_t seq = 0;
    std::uint8_t channel = 0;
    std::uint8_t flags = 0;
    bool xrun = false;                          // an empty block standing for a gap
    bool split = false;                         // more than one frame: not taken here
    const std::uint8_t* data = nullptr;         // the Opus packet (stream messages skipped)
    std::size_t len = 0;
};

// The reading rules of 5.4. False when the message is too short, a stream
// id or sequence is negative, the rate is not finite and positive, or the
// stream messages don't fit.
bool parseData(const std::uint8_t* p, std::size_t n, Block& out);
std::size_t writeData(std::uint8_t* buf, std::int32_t stream, std::int32_t seq, double rate,
                      const std::uint8_t* packet, std::size_t len);                  // flags 0x01, 24 + len
std::size_t writeXrun(std::uint8_t* buf, std::int32_t stream, std::int32_t seq);    // flags 0x08, 16

// The resend request: whole blocks only (frame offset -1).
std::size_t writeRequest(std::uint8_t* buf, std::int32_t stream, const std::int32_t* seqs, int count);
// Reads at most kMaxRequests sequences of whole-block requests (or of
// requests for frame 0, the only frame a block has here). False: drop the
// message (a negative count, or more requests than it holds).
bool parseRequest(const std::uint8_t* p, std::size_t n, std::int32_t& stream, std::int32_t* seqs, int& count);

// ------------------------------------------------------- big-endian bits

inline void putBE16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
inline void putBE32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
inline void putBE64(std::uint8_t* p, std::uint64_t v) {
    putBE32(p, static_cast<std::uint32_t>(v >> 32));
    putBE32(p + 4, static_cast<std::uint32_t>(v));
}
inline std::uint16_t getBE16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
inline std::uint32_t getBE32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline std::uint64_t getBE64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(getBE32(p)) << 32) | getBE32(p + 4);
}

}  // namespace aoo
}  // namespace engine
}  // namespace o11proto

#endif  // O11PROTO_ENGINE_LIVE_AOO_H
