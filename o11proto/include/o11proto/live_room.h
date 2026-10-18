// o11proto - Live mode for the whole room: the clients' health reports
// (CLIENT_LIVE_REPORT, 0x85), the room's mode and the advice
// (SERVER_LIVE_ROOM, 0x08), and the server's view of the reports.
//
// The room has one mode for everyone, and its !live switch alone sets it:
// while the switch is on (and the server's Live UDP is open) the room is
// Live, every Live player is heard live, and there is no interval audio at
// all (none is uploaded or relayed); else everyone is heard through
// intervals. Nothing switches it on its own. A player who can't play Live
// neither hears nor is heard in a Live room. Each Live client tells the
// server whether everything it should hear live sounds clean, and the
// server tells every Live client who has trouble (dropouts, or can't play
// Live): advice, so that a UI can suggest switching the room to intervals.
// This replaces the live-mode spec's per-player switching of 6.5 (a
// deliberate difference).
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
constexpr std::uint8_t kLiveRoomVersion = 3;
constexpr std::size_t kLiveReportSize = 8;
constexpr std::size_t kLiveRoomFixed = 4;             // then the advice's user name, then (version 2) the count

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

// Who has trouble in a Live room: the advice's reason.
enum class Advice : std::uint8_t {
    None = 0,          // nobody, or the room isn't Live
    Dropouts = 1,      // the user's audio has dropouts, or their UDP binding was lost
    CantLive = 2,      // the user can't play Live: no Live client, no UDP, or no place among the senders
};

// How the room's Live audio goes between its players (all-to-all,
// PROTOCOL.md 15.10). Direct: each player sends to each other player
// directly where the pair's path is up, and only there; a pair whose path
// is unreachable (one of the two reports it so: no probe answered for
// kPathDeadMs) can't hear each other, and both are "can't play Live"
// advice. A player that can't send directly (no client_caps bit 17, or
// relay-only in its CLIENT_LIVE_PATHS) has no paths, and the others send to
// it, and it to the room, through the relay.
//
// With the server's backend option relay_fallback (kRouteRelayFallback),
// a pair whose path is still opening goes through the relay meanwhile, and
// once a pair of bound senders has no direct path, or a bound sender can't
// send directly, the room goes Relay: every player sends once to the room
// through the relay. It stays Relay until a player sends `!live direct`
// (shown nowhere); while it is, the paths are still probed, and
// `direct_possible` says when every pair has a direct path again. A room
// with no Live player starts Direct again. None: the server has no direct
// paths.
enum class RoomRoute : std::uint8_t { None = 0, Direct = 1, Relay = 2 };
// Why the room is Relay: the first of these the server found.
enum class RelayWhy : std::uint8_t {
    None = 0,
    NoPath = 1,          // the pair `why_a`, `why_b` had no direct path
    NoDirect = 2,        // `why_a`'s client doesn't send directly (no client_caps bit 17)
    RelayOnly = 3,       // `why_a`'s client sends through the relay only (CLIENT_LIVE_PATHS)
};
constexpr std::uint8_t kRouteDirectPossible = 0x01;   // the route's flags
constexpr std::uint8_t kRouteRelayFallback = 0x02;


// SERVER_LIVE_ROOM: u8 version, u8 mode (0 intervals, 1 live), u8 the
// advice's reason, u8 reserved (0), string: the advice's user name ("" with
// no advice), then in version 2 u16 how many players have trouble (the one
// named among them; 0 with no advice), then in version 3 the route: u8
// route (RoomRoute), u8 flags (kRouteDirectPossible, kRouteRelayFallback), u8 why (RelayWhy), u8
// reserved (0), and two strings, the names `why` gives ("" for none). The
// mode is the room's switch; the advice comes only while the room is Live.
struct LiveRoom {
    std::uint8_t version = kLiveRoomVersion;
    bool live = false;
    Advice advice = Advice::None;
    std::string user;
    std::uint16_t count = 0;
    RoomRoute route = RoomRoute::None;
    bool relay_fallback = false;               // the server's backend option
    bool direct_possible = false;              // Relay only
    RelayWhy why = RelayWhy::None;             // Relay only
    std::string why_a, why_b;
};
std::vector<std::uint8_t> encode(const LiveRoom& m);
// False (ignore it) when shorter than 4 bytes or the version is 0. A mode
// other than 1 is intervals, a reason above 2 is none; the name runs to the
// first NUL or the end. Version 1 (where the reason said why the room was
// on intervals) has no count: 1 with a reason. A later version is read as
// 3, its count as 1 with a reason when the two bytes after the name's NUL
// are missing, its route as None when the four after the count are, and
// bytes after the route's names are ignored. A route above 2 is None, a
// reason above 3 none; a name runs to its NUL or the end.
bool decode(const std::vector<std::uint8_t>& p, LiveRoom& out);

// ----------------------------------------------- the server's advice

// While the room is Live, the server works out from the players' state and
// reports who has trouble, at every tick:
// - can't play Live: no Live client, no place among the senders, a report
//   that its own Live can't, or a Live client still not bound or silent
//   kJoinGraceSeconds after it joined;
// - dropouts: a binding that was lost, or reports that it doesn't sound
//   clean (a player who doesn't send is no one's fault but the reporter's
//   own). The reports of a player still joining (within kJoinGraceSeconds,
//   and not yet bound, reported and clean once), and those that blame one,
//   don't count: its stream is only starting.
// A player has trouble from then until it has had none for kCleanSeconds,
// or has left. The advice names the one with the worst trouble now (can't
// play Live and a lost binding count as everyone's complaint; else the most
// reports that blame it), the earliest in the room on a tie, and says how
// many have trouble. Nothing here changes the room's mode.
class RoomAdvice {
public:
    static constexpr double kJoinGraceSeconds = 10;
    static constexpr double kCleanSeconds = 10;

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
        // All-to-all, direct only: how many of its pairs have no direct
        // path. It can't play Live with them: "can't play Live", the one
        // with the most named first.
        int no_path = 0;
    };
    struct State {
        Advice advice = Advice::None;
        std::uint32_t user = 0;          // the advice's user id
        int count = 0;                   // players with trouble
        bool operator==(const State& o) const { return advice == o.advice && user == o.user && count == o.count; }
        bool operator!=(const State& o) const { return !(*this == o); }
    };

    // Works the advice out again at `now` (seconds of a steady clock); none
    // while the room isn't Live.
    const State& update(double now, bool live, const std::vector<Player>& players);
    const State& state() const { return state_; }

private:
    struct Trouble {
        std::uint32_t id = 0;
        Advice advice = Advice::None;    // the latest
        double clean_since = -1;         // -1 while it has trouble now
    };
    State state_;
    std::vector<Trouble> trouble_;       // in the order it began
};

}  // namespace live
}  // namespace o11proto

#endif  // O11PROTO_LIVE_ROOM_H
