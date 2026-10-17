// njproto - clean-room NINJAM protocol library.
// Message framing and the encode/decode functions for every message type.
//
// Everything here is written from PROTOCOL.md only. All integers on the wire
// are little-endian; strings are NUL-terminated byte runs.
#ifndef NJPROTO_MESSAGE_H
#define NJPROTO_MESSAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace njproto {

// ---------------------------------------------------------------- constants

constexpr std::size_t kHeaderSize = 5;
constexpr std::size_t kMaxPayload = 16384;
constexpr std::size_t kMaxSendQueue = 512;     // queued messages per connection
constexpr std::uint32_t kProtocolVersion = 0x00020000;
constexpr int kDefaultKeepaliveSeconds = 3;
constexpr int kLicenceKeepaliveSeconds = 45;
constexpr int kTransferTimeoutSeconds = 8;
constexpr std::size_t kMaxWriteChunk = 9216;   // bytes of audio per WRITE
constexpr int kMaxChannels = 32;

// client_caps bits
constexpr std::uint32_t kCapLicenceAgreed = 0x1;
constexpr std::uint32_t kCapOpus = 0x100;
constexpr std::uint32_t kCapFlac = 0x200;
constexpr std::uint32_t kCapCodecMask = 0xFF00;

// server_caps bits
constexpr std::uint32_t kServerCapLicence = 0x1;

// channel flags
constexpr std::uint8_t kChanNoDefaultSubscribe = 0x01;
constexpr std::uint8_t kChanVoice = 0x02;
constexpr std::uint8_t kChanSession = 0x04;
constexpr std::uint8_t kChanInactive = 0x80;

enum MsgType : std::uint8_t {
    kMsgAuthChallenge = 0x00,
    kMsgAuthReply = 0x01,
    kMsgConfigChange = 0x02,
    kMsgUserInfoChange = 0x03,
    kMsgDownloadBegin = 0x04,
    kMsgDownloadWrite = 0x05,
    kMsgCodecMask = 0x06,
    kMsgClientAuthUser = 0x80,
    kMsgClientSetUserMask = 0x81,
    kMsgClientSetChannelInfo = 0x82,
    kMsgClientUploadBegin = 0x83,
    kMsgClientUploadWrite = 0x84,
    kMsgChat = 0xC0,
    kMsgKeepalive = 0xFD,
    kMsgExtended = 0xFE,
    kMsgInvalid = 0xFF,
};

// ------------------------------------------------------------------ fourcc

using Guid = std::array<std::uint8_t, 16>;

// Builds a fourcc from four ASCII characters in wire order ("OGGv").
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}
constexpr std::uint32_t kFourccVorbis = fourcc('O', 'G', 'G', 'v');
constexpr std::uint32_t kFourccOpus = fourcc('O', 'P', 'U', 'S');      // Ogg Opus (reference)
constexpr std::uint32_t kFourccOpusRaw = fourcc('O', 'P', 'U', 'r');   // raw Opus packets (7.8)
constexpr std::uint32_t kFourccFlac = fourcc('F', 'L', 'A', 'C');

std::uint32_t fourccFromString(const std::string& s);   // 0 if not 4 chars
std::string fourccToString(std::uint32_t f);            // 4 chars (non-printables as '?')

// The CLIENT_CAP codec bit a subscriber needs to decode a fourcc, or 0 when
// no capability is needed (Vorbis, and unknown fourccs which are passed on).
std::uint32_t codecCapForFourcc(std::uint32_t f);

bool guidIsZero(const Guid& g);
std::string guidToHex(const Guid& g);
bool guidFromHex(const std::string& hex, Guid& out);

// ----------------------------------------------------------------- framing

struct RawMessage {
    std::uint8_t type = kMsgInvalid;
    std::vector<std::uint8_t> payload;
};

enum class FrameStatus { Ok, NeedMore, Error };

// Parses one framed message from the front of [data, data+len).
// Ok: `out` is filled and `consumed` says how many bytes were used.
// NeedMore: not a whole message yet. Error: bad header (too long / type 0xFF);
// the caller must close the connection.
FrameStatus parseFrame(const std::uint8_t* data, std::size_t len, RawMessage& out,
                       std::size_t& consumed);

// Appends header + payload to `out`. Returns false (appends nothing) when the
// payload is too long for the protocol.
bool appendFrame(std::vector<std::uint8_t>& out, std::uint8_t type,
                 const std::uint8_t* payload, std::size_t len);
bool appendFrame(std::vector<std::uint8_t>& out, std::uint8_t type,
                 const std::vector<std::uint8_t>& payload);

// --------------------------------------------------------------- byte I/O

// Bounds-checked little-endian reader over a byte range. Every accessor
// returns false and leaves the output untouched when the data runs out.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t len) : p_(data), end_(data + len) {}
    explicit Reader(const std::vector<std::uint8_t>& v) : Reader(v.data(), v.size()) {}

    std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    bool atEnd() const { return p_ == end_; }
    bool u8(std::uint8_t& v);
    bool s8(std::int8_t& v);
    bool u16(std::uint16_t& v);
    bool s16(std::int16_t& v);
    bool u32(std::uint32_t& v);
    bool bytes(std::uint8_t* dst, std::size_t n);
    bool str(std::string& v);               // up to and including the NUL
    bool skip(std::size_t n);
    const std::uint8_t* ptr() const { return p_; }

private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

class Writer {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void s8(std::int8_t v) { buf_.push_back(static_cast<std::uint8_t>(v)); }
    void u16(std::uint16_t v);
    void s16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void u32(std::uint32_t v);
    void bytes(const std::uint8_t* src, std::size_t n);
    void str(const std::string& s);          // appends the NUL
    std::vector<std::uint8_t>& data() { return buf_; }
    const std::vector<std::uint8_t>& data() const { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

// --------------------------------------------------------- message structs

struct AuthChallenge {                     // 0x00
    std::array<std::uint8_t, 8> challenge{};
    std::uint32_t server_caps = 0;
    std::uint32_t protocol_version = kProtocolVersion;
    bool has_licence = false;              // server_caps bit 0
    std::string licence;
    int keepaliveSeconds() const;          // bits 8-15, 0 -> default 3
};

struct AuthUser {                          // 0x80
    std::array<std::uint8_t, 20> passhash{};
    std::string username;
    std::uint32_t client_caps = 0;
    std::uint32_t client_version = kProtocolVersion;
};

struct AuthReply {                         // 0x01
    std::uint8_t flag = 0;                 // bit 0 = success
    bool has_text = false;
    std::string text;                      // username on success, reason on failure
    std::uint8_t maxchan = 0;
    bool success() const { return (flag & 1) != 0; }
};

struct ConfigChange {                      // 0x02
    std::uint16_t bpm = 0;
    std::uint16_t bpi = 0;
};

struct UserInfoRecord {                    // one record of 0x03
    bool active = false;
    std::uint8_t index = 0;
    std::int16_t volume = 0;
    std::int8_t pan = 0;
    std::uint8_t flags = 0;
    std::string username;
    std::string channel;
};
struct UserInfoChange {
    std::vector<UserInfoRecord> records;
};

struct ChannelInfoRecord {                 // one record of 0x82
    std::string name;
    std::int16_t volume = 0;
    std::int8_t pan = 0;
    std::uint8_t flags = 0;
    bool active() const { return (flags & kChanInactive) == 0; }
};
struct SetChannelInfo {
    std::vector<ChannelInfoRecord> channels;   // record i is channel i
};

struct UserMaskEntry {                     // one record of 0x81
    std::string username;
    std::uint32_t mask = 0;
};
struct SetUserMask {
    std::vector<UserMaskEntry> entries;
};

struct UploadBegin {                       // 0x83
    Guid guid{};
    std::uint32_t estsize = 0;
    std::uint32_t fourcc = 0;
    std::uint8_t channel = 0;
};

struct IntervalWrite {                     // 0x84 and 0x05 share one layout
    Guid guid{};
    std::uint8_t flags = 0;                // bit 0 = last write
    std::vector<std::uint8_t> data;
    bool last() const { return (flags & 1) != 0; }
};

struct DownloadBegin {                     // 0x04
    Guid guid{};
    std::uint32_t estsize = 0;
    std::uint32_t fourcc = 0;
    std::uint8_t channel = 0;
    std::string username;
};

struct CodecMask {                         // 0x06 (Jamsa extension)
    std::uint32_t codecs = 0;
    std::uint32_t opus_kbps = 0;           // only on the wire when nonzero
};

struct ChatMessage {                       // 0xC0
    std::array<std::string, 5> fields;     // fields[0] is the command
    ChatMessage() = default;
    ChatMessage(const std::string& a, const std::string& b = "", const std::string& c = "",
                const std::string& d = "", const std::string& e = "")
        : fields{{a, b, c, d, e}} {}
    const std::string& command() const { return fields[0]; }
};

// ------------------------------------------------------- encode / decode

// decode* return false on a malformed payload (and may leave `out` partly
// filled). Extra bytes after a fixed layout are ignored, as the spec asks.
std::vector<std::uint8_t> encode(const AuthChallenge& m);
bool decode(const std::vector<std::uint8_t>& p, AuthChallenge& out);
std::vector<std::uint8_t> encode(const AuthUser& m);
bool decode(const std::vector<std::uint8_t>& p, AuthUser& out);
std::vector<std::uint8_t> encode(const AuthReply& m);
bool decode(const std::vector<std::uint8_t>& p, AuthReply& out);
std::vector<std::uint8_t> encode(const ConfigChange& m);
bool decode(const std::vector<std::uint8_t>& p, ConfigChange& out);
std::vector<std::uint8_t> encode(const UserInfoChange& m);
bool decode(const std::vector<std::uint8_t>& p, UserInfoChange& out);
std::vector<std::uint8_t> encode(const SetChannelInfo& m);
bool decode(const std::vector<std::uint8_t>& p, SetChannelInfo& out);
std::vector<std::uint8_t> encode(const SetUserMask& m);
bool decode(const std::vector<std::uint8_t>& p, SetUserMask& out);
std::vector<std::uint8_t> encode(const UploadBegin& m);
bool decode(const std::vector<std::uint8_t>& p, UploadBegin& out);
std::vector<std::uint8_t> encode(const IntervalWrite& m);
bool decode(const std::vector<std::uint8_t>& p, IntervalWrite& out);
std::vector<std::uint8_t> encode(const DownloadBegin& m);
bool decode(const std::vector<std::uint8_t>& p, DownloadBegin& out);
std::vector<std::uint8_t> encode(const CodecMask& m);
bool decode(const std::vector<std::uint8_t>& p, CodecMask& out);
// Chat: the payload must end with 0x00 (Jamsa rule) or decoding fails.
std::vector<std::uint8_t> encode(const ChatMessage& m);
bool decode(const std::vector<std::uint8_t>& p, ChatMessage& out);

}  // namespace njproto

#endif  // NJPROTO_MESSAGE_H
