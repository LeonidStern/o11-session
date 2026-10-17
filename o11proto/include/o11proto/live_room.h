// o11proto - Live mode for the whole room: the clients' health reports
// (CLIENT_LIVE_REPORT, 0x85), the room's mode (SERVER_LIVE_ROOM, 0x08) and
// the server's rule that picks the mode from the reports.
//
// The room plays one way for everyone: either every Live player is heard
// live, or everyone is heard through intervals. Each Live client tells the
// server whether everything it should hear live sounds clean; the server
// switches the room and tells every Live client, which switches at its next
// interval boundary. This replaces the live-mode spec's per-player switching
// of 6.5 (a deliberate difference).
#ifndef O11PROTO_LIVE_ROOM_H
#define O11PROTO_LIVE_ROOM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace o11proto {
namespace live {

constexpr std::uint8_t kMsgClientLiveReport = 0x85;   // client -> server
constexpr std::uint8_t kMsgServerLiveRoom = 0x08;     // server -> client
constexpr std::uint8_t kLiveReportVersion = 1;
constexpr std::uint8_t kLiveRoomVersion = 1;
constexpr std::size_t kLiveReportSize = 8;
constexpr std::size_t kLiveRoomFixed = 4;             // then the reason user's name

// CLIENT_LIVE_REPORT flags
constexpr std::uint8_t kReportClean = 0x01;           // everything it should hear live sounds clean
constexpr std::uint8_t kReportSending = 0x02;         // its own source sends (the send gate is open)
constexpr std::uint8_t kReportCantLive = 0x04;        // its Live client doesn't run, or can't hear a sender

// CLIENT_LIVE_REPORT: u8 version, u8 flags, u16 reserved (0), u32 fault.
// `fault` is the peer id of the one player whose audio doesn't sound
// clean, or the reporter's own id when several don't or its own Live
// can't; 0 when clean.
struct LiveReport {
    std::uint8_t version = kLiveReportVersion;
    std::uint8_t flags = 0;
    std::uint32_t fault = 0;
    bool clean() const { return (flags & kReportClean) != 0; }
    bool sending() const { return (flags & kReportSending) != 0; }
    bool cantLive() const { return (flags & kReportCantLive) != 0; }
    bool operator==(const LiveReport& o) const { return flags == o.flags && fault == o.fault; }
    bool operator!=(const LiveReport& o) const { return !(*this == o); }
};
std::vector<std::uint8_t> encode(const LiveReport& r);
// False (ignore it) when shorter than 8 bytes or the version is 0. A later
// version is read as 1; bytes after the eighth are ignored.
bool decode(const std::vector<std::uint8_t>& p, LiveReport& out);

// Why the room is on intervals.
enum class RoomReason : std::uint8_t {
    None = 0,          // Live, or the room's Live is off
    Dropouts = 1,      // the user's audio had dropouts, or their UDP binding was lost
    CantLive = 2,      // the user can't do Live: no Live client, no UDP, or no place among the senders
    Joining = 3,       // the user is joining: not bound or no report yet
};

// SERVER_LIVE_ROOM: u8 version, u8 mode (0 intervals, 1 live), u8 reason,
// u8 reserved (0), string: the reason user's name ("" with reason 0).
struct LiveRoom {
    std::uint8_t version = kLiveRoomVersion;
    bool live = false;
    RoomReason reason = RoomReason::None;
    std::string user;
};
std::vector<std::uint8_t> encode(const LiveRoom& m);
// False (ignore it) when shorter than 4 bytes or the version is 0. A later
// version is read as 1; a mode other than 1 is intervals, a reason above 3
// is 0; the name runs to the first NUL or the end.
bool decode(const std::vector<std::uint8_t>& p, LiveRoom& out);

// ------------------------------------------------------ the server's rule

// The room is Live only while the room's Live is on and every participant
// can do Live, is bound and reports clean. Anything else puts it on
// intervals at once, naming a user; it returns to Live after all of that
// held for kSettleSeconds, or kRepeatSettleSeconds when it fell back for a
// connection problem twice within kRepeatWindowSeconds. While it waits, the
// reason stays, unless its user has left: then there is none (None, 0).
class RoomRule {
public:
    static constexpr double kSettleSeconds = 5;
    static constexpr double kRepeatSettleSeconds = 15;
    static constexpr double kRepeatWindowSeconds = 60;
    static constexpr double kJoinGraceSeconds = 10;   // to bind and report, else "can't do Live"

    struct Player {                      // a logged-in, listed user, in the room's order
        std::uint32_t id = 0;
        bool live_client = false;        // caps bit 16 and a key
        bool listener = false;           // no channels: never sends live, never counted
        bool sender = false;             // among the relay's senders
        bool bound = false;
        bool was_bound = false;          // bound at some time since joining
        bool has_report = false;
        LiveReport report;
        bool was_clean = false;          // has reported clean at some time since joining
        double joined = 0;               // seconds, the same clock as update()'s
    };
    struct State {
        bool live = false;
        RoomReason reason = RoomReason::None;
        std::uint32_t user = 0;          // the reason user's id
        bool operator==(const State& o) const { return live == o.live && reason == o.reason && user == o.user; }
        bool operator!=(const State& o) const { return !(*this == o); }
    };

    // Works the state out again at `now` (seconds of a steady clock).
    const State& update(double now, bool room_on, const std::vector<Player>& players);
    const State& state() const { return state_; }
    // Seconds the conditions have held, or -1 while they don't.
    double heldFor(double now) const { return ok_since_ < 0 ? -1 : now - ok_since_; }

private:
    State state_;
    double ok_since_ = -1;
    std::vector<double> fallbacks_;      // when it fell back for a connection problem
};

}  // namespace live
}  // namespace o11proto

#endif  // O11PROTO_LIVE_ROOM_H
