// o11proto - the server's Live mode: the Live message (0x07), the !live
// command and the UDP relay. See PROTOCOL.md section 15.
//
// The relay never decodes audio. It checks ids, keys, addresses and rates,
// answers hellos with a cookie that binds a player's UDP address, and copies
// data datagrams, untouched, to the room's other senders.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "o11proto/live.h"
#include "o11proto/live_room.h"
#include "o11proto/server.h"
#include "server_net.h"
#include "udp_compat.h"

namespace o11proto {

namespace {

// Why a datagram was dropped, in the order of the relay's traffic line.
enum Reason {
    kMalformed,
    kTooBig,
    kUnknownSender,
    kWrongKey,
    kWrongAddress,
    kOverRate,
    kUnverified,
    kRoomOff,
    kMayNotSend,
    kNoSuchPlayer,
    kCouldNotSend,
    kReasons
};
const char* const kReasonText[kReasons] = {
    "malformed", "too big", "unknown sender", "wrong key", "wrong address", "over the rate limit",
    "unverified address (answered)", "room's Live is off", "sender may not send", "no such player",
    "could not send",
};

std::string lowerCopy(const std::string& s) {
    std::string r = s;
    for (auto& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}

std::string trimBlanks(const std::string& s) {            // spaces and tabs only
    std::size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// "a", "a and b", "a, b and c".
std::string joinNames(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += i + 1 == v.size() ? " and " : ", ";
        s += v[i];
    }
    return s;
}

// Why the room's Live goes through the relay, of the users named `a` and
// `b`: as it is now, or since it was.
std::string whyText(live::RelayWhy w, const std::string& a, const std::string& b, bool now) {
    switch (w) {
    case live::RelayWhy::NoPath: return a + " and " + b + (now ? " can't reach each other directly" : " had no direct path");
    case live::RelayWhy::NoDirect: return a + "'s client doesn't send directly";
    case live::RelayWhy::RelayOnly: return a + "'s client sends through the relay only";
    case live::RelayWhy::None: break;
    }
    return "nothing";
}

// A number rounded as printf rounds it (ties to even).
std::string rounded(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

LiveConfig sanitize(LiveConfig c, const std::function<void(const std::string&)>& log) {
    auto warn = [&](const std::string& s) {
        if (log) log("warning: " + s);
    };
    if (c.kbps < live::kMinKbps || c.kbps > live::kMaxKbps) {
        warn("LiveKbps " + std::to_string(c.kbps) + " is outside 32-510, using 128");
        c.kbps = live::kDefaultKbps;
    }
    if (!live::validFrame(c.frame)) {
        warn("LiveFrame " + std::to_string(c.frame) + " is not 120, 240 or 480, using 240");
        c.frame = live::kDefaultFrame;
    }
    if (c.max_peers < 1 || c.max_peers > live::kMaxMaxPeers) {
        warn("LiveMaxPeers " + std::to_string(c.max_peers) + " is outside 1-64, using 8");
        c.max_peers = live::kDefaultMaxPeers;
    }
    if (c.announce_port < 0 || c.announce_port > 65535) {
        warn("LivePort " + std::to_string(c.announce_port) + " is outside 0-65535, using 0");
        c.announce_port = 0;
    }
    if (!(c.fresh_seconds > 0)) c.fresh_seconds = live::kFreshSeconds;
    if (!(c.stats_seconds > 0)) c.stats_seconds = live::kStatsSeconds;
    return c;
}

// A backend option: "1" on, "0" off, else `fallback`.
bool envFlag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || (std::strcmp(v, "1") != 0 && std::strcmp(v, "0") != 0)) return fallback;
    return v[0] == '1';
}

// The test hooks: a decimal number of seconds above 0, else `fallback`.
double envSeconds(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    double d = std::strtod(v, &end);
    return (end != v && std::isfinite(d) && d > 0) ? d : fallback;
}

}  // namespace

// ------------------------------------------------------------- the state

struct Server::Live {
    struct Player {                            // a Live player: logged in, bit 16, not hidden
        std::uint64_t key = 0;                 // 0: none (yet): not known to the relay
        std::uint32_t tcp_ip = 0;              // network order
        bool channels = false;                 // maxchan > 0; else a listener only
        bool direct = false;                   // client_caps bit 17: sends to the others directly where it can
        udp::Addr lan;                         // its LAN address (0x86), ip 0 for none
        std::vector<std::uint8_t> last_paths;  // the last 0x09 sent
        bool relay_only = false;               // its latest CLIENT_LIVE_PATHS (0x87): through the relay only
        std::map<int, live::PathState> path_states;   // and its paths to the others, by peer id
        bool told = false;                     // has had a Live message
        std::vector<std::uint8_t> last;        // the last Live message sent
        // relay
        bool bound = false;                    // a verified hello within fresh_seconds
        udp::Addr addr;                        // bound address
        std::uint32_t local = 0;               // where its datagrams arrive
        bool has_local = false;
        TimePoint last_hello;
        bool buckets = false;                  // filled at the first datagram that reaches the check
        double bytes = 0, dgrams = 0;
        TimePoint refill;
        // the advice (live_room.h)
        TimePoint joined;
        bool was_bound = false;                // bound at some time since joining
        bool has_report = false;
        live::LiveReport report;
        bool was_clean = false;                // has reported clean at some time
        std::vector<std::uint8_t> last_room;   // the last room message sent
    };

    LiveConfig cfg;
    bool room_on = false;                      // the room's switch (the whole server is one room)
    bool room_chosen = false;                  // switched with !live: survives reloads
    socket_t fd = kInvalidSocket;
    std::uint16_t port = 0;
    std::uint8_t secret[16] = {};
    std::uint8_t path_secret[16] = {};         // the pairs' keys (0x09)
    bool paths_ok = false;                     // path_secret was drawn
    // The room's route (live_room.h): Direct until a pair has no direct
    // path, then Relay until `!live direct`; Direct again once the room has
    // no Live player.
    live::RoomRoute route = live::RoomRoute::Direct;
    live::RelayWhy why = live::RelayWhy::None;
    std::string why_a, why_b;                  // the user names it gives
    bool possible_logged = false;              // "everyone can connect directly again" was logged
    bool random_warned = false;
    bool dirty = true;                         // the Live messages must be worked out again
    // By peer id, which is the connection id: ids count up as connections
    // are accepted, so this is also the room's order.
    std::map<int, Player> players;
    std::vector<int> senders;                  // the first max_peers players with channels and a key
    TimePoint next_fresh_check;
    TimePoint stats_since;
    std::uint64_t in = 0, out = 0;
    std::array<std::uint64_t, kReasons> dropped{};
    std::uint8_t buf[2048];
    // A datagram since the last pass that may change what a pass does (a
    // hello: a binding, its 0x07 and 0x09, the advice). Relayed data
    // changes nothing there: no pass of its own (the 20 ms tick has them).
    bool pass_wanted = false;
    // The advice: worked out at every tick from the players' reports.
    live::RoomAdvice advice;
    TimePoint epoch = Clock::now();            // the advice's clock starts here
    std::string advice_name;                   // the advice's user name, "" for none
    bool logged_on = false;                    // the room's Live was on at the last log line
    // No interval audio while the room is Live: since when it is.
    bool noted_on = false;
    TimePoint live_since;

    bool allowed() const { return cfg.mode != LiveMode::No; }
    bool open() const { return fd != kInvalidSocket; }
    bool roomOn() const { return allowed() && open() && room_on; }
    bool isSender(int id) const { return std::find(senders.begin(), senders.end(), id) != senders.end(); }
    bool hasPaths() const { return allowed() && open() && paths_ok; }
    // What the bound senders' paths say now: the first trouble that takes
    // the room to the relay (a sender that can't send directly, or a pair
    // either of whose two reports its path unreachable), and whether every
    // pair's path is up as both of its players report it.
    struct RouteCheck {
        live::RelayWhy why = live::RelayWhy::None;
        int a = 0, b = 0;
        bool all_up = true;
        std::map<int, int> no_path;            // each sender's pairs without a path
    };
    RouteCheck checkRoute() const {
        RouteCheck r;
        std::vector<int> direct;               // the bound senders that send directly
        for (int id : senders) {
            auto it = players.find(id);
            if (it == players.end() || !it->second.bound) continue;
            const Player& p = it->second;
            const live::RelayWhy w = !p.direct ? live::RelayWhy::NoDirect
                                     : p.relay_only ? live::RelayWhy::RelayOnly
                                                    : live::RelayWhy::None;
            if (w == live::RelayWhy::None) {
                direct.push_back(id);
                continue;
            }
            r.all_up = false;
            if (r.why == live::RelayWhy::None) {
                r.why = w;
                r.a = id;
            }
        }
        auto state = [&](int of, int to) {
            const auto& m = players.at(of).path_states;
            auto it = m.find(to);
            return it == m.end() ? live::PathState::Opening : it->second;
        };
        for (std::size_t i = 0; i < direct.size(); ++i)
            for (std::size_t j = i + 1; j < direct.size(); ++j) {
                const live::PathState x = state(direct[i], direct[j]), y = state(direct[j], direct[i]);
                if (x == live::PathState::Up && y == live::PathState::Up) continue;
                r.all_up = false;
                if (x != live::PathState::Down && y != live::PathState::Down) continue;
                ++r.no_path[direct[i]];
                ++r.no_path[direct[j]];
                if (r.why == live::RelayWhy::None) {
                    r.why = live::RelayWhy::NoPath;
                    r.a = direct[i];
                    r.b = direct[j];
                }
            }
        return r;
    }
    Player* known(std::uint32_t id) {          // a player the relay takes datagrams from
        if (!allowed() || !open() || id == 0 || id > 0x7fffffffu) return nullptr;
        auto it = players.find(static_cast<int>(id));
        return it == players.end() || it->second.key == 0 ? nullptr : &it->second;
    }
    void recomputeSenders() {
        senders.clear();
        for (const auto& kv : players) {
            if (static_cast<int>(senders.size()) >= cfg.max_peers) break;
            if (kv.second.channels && kv.second.key != 0) senders.push_back(kv.first);
        }
    }
    // Opening forgets every binding and starts the traffic line again.
    void opened(socket_t s, std::uint16_t p) {
        fd = s;
        port = p;
        for (auto& kv : players) {
            kv.second.bound = kv.second.has_local = kv.second.buckets = false;
            kv.second.path_states.clear();     // new keys: every path opens again
        }
        paths_ok = live::systemRandom(path_secret, sizeof(path_secret));   // new keys for every pair
        cfg.fresh_seconds = envSeconds("O11_LIVE_FRESH_SECONDS", cfg.fresh_seconds);
        cfg.relay_fallback = envFlag("O11_LIVE_RELAY_FALLBACK", cfg.relay_fallback);
        cfg.stats_seconds = envSeconds("O11_LIVE_STATS_SECONDS", cfg.stats_seconds);
        stats_since = next_fresh_check = Clock::now();
        in = out = 0;
        dropped.fill(0);
        dirty = true;
    }
};

void Server::LiveDeleter::operator()(Live* p) const { delete p; }

// ------------------------------------------------------------ the UDP port

namespace {

enum class OpenResult { Ok, Busy, NoRandom };

// Opens the relay's socket `s` on `port` (on `bind_address` when the TCP
// listener has one) and draws a new cookie secret.
OpenResult openRelaySocket(asio::ip::udp::socket& s, std::uint16_t port, const std::string& bind_address,
                           std::uint8_t secret[16]) {
    asio::error_code ec;
    if (!udp::openSocket(s, live::kRelaySocketBuffer, true, ec)) return OpenResult::Busy;   // no address reuse
    asio::ip::address_v4 addr = asio::ip::address_v4::any();
    if (!bind_address.empty()) {
        asio::error_code bad;
        const asio::ip::address_v4 a = asio::ip::make_address_v4(bind_address, bad);
        if (!bad) addr = a;
    }
    s.bind(asio::ip::udp::endpoint(addr, port), ec);
    if (ec || !live::systemRandom(secret, 16)) {   // no secret, no cookies: the port stays closed
        asio::error_code ignored;
        s.close(ignored);
        return ec ? OpenResult::Busy : OpenResult::NoRandom;
    }
    udp::reportLocalAddress(s.native_handle());
    return OpenResult::Ok;
}

std::string busyWarning(const std::string& p) {
    return "warning: Live is off, could not open UDP port " + p + ". Another program may already be using it "
           "(2049 is also the NFS port). Set a different Port in the config, or Live no.";
}

std::string randomWarning(const std::string& p) {
    return "warning: Live is off, could not read the system's random source for the cookies of UDP port " + p;
}

}  // namespace

bool Server::liveStart() {
    live_.reset(new Live);
    Live& L = *live_;
    L.cfg = sanitize(cfg_.live, cfg_.log);
    L.room_on = L.cfg.mode == LiveMode::On;
    if (!L.allowed()) return true;
    const std::string p = std::to_string(port_);
    OpenResult r = openRelaySocket(net_->relay, port_, cfg_.bind_address, L.secret);
    if (r != OpenResult::Ok) {
        if (L.cfg.mode == LiveMode::Require) {
            if (r == OpenResult::Busy)
                log("Error listening on UDP port " + p + " for Live! Another program may already be using it "
                    "(2049 is also the NFS port), or you may need permission to use it. Set a different Port in "
                    "the config or use -port.");
            else
                log("Error opening UDP port " + p + " for Live! Could not read the system's random source.");
            return false;
        }
        log(r == OpenResult::Busy ? busyWarning(p) : randomWarning(p));
        return true;
    }
    L.opened(static_cast<socket_t>(net_->relay.native_handle()), port_);
    log("Live: listening on UDP port " + p);
    armRelay();
    return true;
}

void Server::liveShutdown() {
    if (!live_ || live_->fd == kInvalidSocket) return;
    asio::error_code ec;
    net_->relay.close(ec);                     // its wait ends with operation_aborted
    net_->relay_reading = false;
    live_->fd = kInvalidSocket;
    live_->port = 0;
    live_->dirty = true;
}

void Server::setLiveConfig(const LiveConfig& config) {
    if (!live_) return;
    Live& L = *live_;
    cfg_.live = config;
    L.cfg = sanitize(config, cfg_.log);
    if (!L.allowed()) {
        if (L.open()) {
            const std::string p = std::to_string(L.port);
            liveShutdown();
            log("Live is off, closed UDP port " + p);
        }
    } else if (!L.open()) {
        const std::string p = std::to_string(port_);
        OpenResult r = openRelaySocket(net_->relay, port_, cfg_.bind_address, L.secret);
        if (r == OpenResult::Ok) {
            L.opened(static_cast<socket_t>(net_->relay.native_handle()), port_);
            log("Live: listening on UDP port " + p);
            armRelay();
        } else {                               // require doesn't stop a running server
            log(r == OpenResult::Busy ? busyWarning(p) : randomWarning(p));
        }
    } else {
        L.cfg.fresh_seconds = envSeconds("O11_LIVE_FRESH_SECONDS", L.cfg.fresh_seconds);
        L.cfg.relay_fallback = envFlag("O11_LIVE_RELAY_FALLBACK", L.cfg.relay_fallback);
        L.cfg.stats_seconds = envSeconds("O11_LIVE_STATS_SECONDS", L.cfg.stats_seconds);
    }
    if (!L.room_chosen) L.room_on = L.cfg.mode == LiveMode::On;
    L.dirty = true;
    schedulePass();                            // the Live messages go out at the next poll()
}

bool Server::liveUdpOpen() const { return live_ && live_->open(); }
std::uint16_t Server::liveUdpPort() const { return live_ ? live_->port : 0; }
bool Server::liveRoomOn() const { return live_ && live_->roomOn(); }
bool Server::liveRoomLive() const { return liveRoomOn(); }

Server::IntervalTraffic Server::intervalTraffic() const {
    IntervalTraffic t;
    t.from_live = iv_from_live_.load();
    t.from_others = iv_from_others_.load();
    t.relayed = iv_relayed_.load();
    t.refused = iv_refused_.load();
    return t;
}

// While the room is Live there is no interval audio. Its Live players stop
// uploading at the boundary where they go Live, the first after the room
// message by half a second or more; an upload that begins more than an
// interval after the room went Live is refused (from an older client, which
// doesn't know). The ones before it are relayed to their end: what the
// players play up to their switch, and over its crossfade.
bool Server::liveRefusesUpload(TimePoint now) {
    if (!live_) return false;
    Live& L = *live_;
    if (L.roomOn() != L.noted_on) {
        L.noted_on = L.roomOn();
        L.live_since = now;
    }
    if (!L.roomOn()) return false;
    const double interval = 60.0 * std::max<int>(1, bpi_) / std::max<int>(1, bpm_);
    return now - L.live_since > std::chrono::duration<double>(interval);
}
std::string Server::liveAdviceUser() const { return live_ ? live_->advice_name : std::string(); }
int Server::liveAdviceCount() const { return live_ ? live_->advice.state().count : 0; }

// ------------------------------------------------------- players come and go

void Server::liveJoined(Conn& c) {
    if (!live_ || !(c.caps & live::kCapLive) || c.hidden()) return;
    Live::Player p;
    udp::parseIPv4(c.addr, p.tcp_ip);
    p.channels = c.maxchan > 0;
    p.direct = (c.caps & live::kCapLiveDirect) != 0;
    p.joined = Clock::now();
    live_->players[c.id] = std::move(p);
    live_->dirty = true;
}

void Server::liveLeft(Conn& c) {
    if (!live_ || !live_->players.erase(c.id)) return;   // forgotten at once
    live_->dirty = true;
}

// --------------------------------------------------------- the Live message

void Server::liveTick(TimePoint now) {
    if (!live_) return;
    Live& L = *live_;
    if (L.open() && now >= L.next_fresh_check) {
        L.next_fresh_check = now + std::chrono::milliseconds(100);
        const auto fresh_for = std::chrono::duration<double>(L.cfg.fresh_seconds);
        for (auto& kv : L.players) {
            Live::Player& p = kv.second;
            if (p.bound && now - p.last_hello > fresh_for) {
                p.bound = false;
                L.dirty = true;
            }
        }
    }
    if (L.open() && now - L.stats_since >= std::chrono::duration<double>(L.cfg.stats_seconds)) {
        if (L.in > 0) {
            const double secs = std::chrono::duration<double>(now - L.stats_since).count();
            int bound = 0;
            for (const auto& kv : L.players) bound += kv.second.bound ? 1 : 0;
            std::string s = "[live] in the last " + rounded(secs) + " s: " + std::to_string(bound) +
                            (bound == 1 ? " player, " : " players, ") +
                            rounded(static_cast<double>(L.in) / secs) + " datagrams/s in, " +
                            rounded(static_cast<double>(L.out) / secs) + " out, ";
            std::uint64_t total = 0;
            for (auto n : L.dropped) total += n;
            if (total == 0) {
                s += "nothing dropped";
            } else {
                s += "dropped " + std::to_string(total) + ":";
                bool first = true;
                for (int r = 0; r < kReasons; ++r) {
                    if (!L.dropped[static_cast<std::size_t>(r)]) continue;
                    s += first ? " " : ", ";
                    s += std::to_string(L.dropped[static_cast<std::size_t>(r)]) + " " + kReasonText[r];
                    first = false;
                }
            }
            log(s);
        }
        L.in = L.out = 0;
        L.dropped.fill(0);
        L.stats_since = now;
    }
    if (L.dirty) {
        L.dirty = false;
        liveSendMessages();
    }
    liveRoomTick(now);
}

void Server::liveSendMessages() {
    Live& L = *live_;

    // Keys first: a player without one is neither known nor a sender.
    if (L.allowed()) {
        for (auto& kv : L.players) {
            Live::Player& p = kv.second;
            for (int i = 0; i < 4 && p.key == 0; ++i)
                if (!live::systemRandom(&p.key, sizeof(p.key))) p.key = 0;
            if (p.key == 0 && !L.random_warned) {
                L.random_warned = true;
                log("warning: Live: could not read the system's random source, so nobody can play Live");
            }
        }
    }
    L.recomputeSenders();

    for (auto& kv : L.players) {
        const int id = kv.first;
        Live::Player& p = kv.second;
        Conn* c = connById(id);
        if (!c || !c->logged_in || p.key == 0) continue;   // no message without a key
        live::ServerLive m;
        m.udp_port = static_cast<std::uint16_t>(L.cfg.announce_port ? L.cfg.announce_port
                                                : (L.open() && L.port != port_) ? L.port : 0);
        m.live_kbps = static_cast<std::uint16_t>(L.cfg.kbps);
        m.peer_id = static_cast<std::uint32_t>(id);
        m.key = p.key;
        m.frame_samples = static_cast<std::uint16_t>(L.cfg.frame);
        m.max_peers = static_cast<std::uint16_t>(L.cfg.max_peers);
        if (!L.allowed()) {
            if (!p.told) continue;              // Live taken away: flags 0, no peers
        } else {
            if (L.roomOn()) m.flags |= live::kFlagRoomOn;
            if (L.open()) m.flags |= live::kFlagUdpOpen;
            if (L.isSender(id)) m.flags |= live::kFlagMaySend;
            for (int sid : L.senders) {
                if (sid == id) continue;
                Conn* s = connById(sid);
                if (!s) continue;
                live::PeerRecord r;
                r.id = static_cast<std::uint32_t>(sid);
                const Live::Player& q = L.players[sid];
                r.flags = L.open() && q.bound ? live::kPeerBound : 0;
                if (q.has_report && q.report.sending()) r.flags |= live::kPeerSending;
                r.name = s->name;
                m.peers.push_back(std::move(r));
            }
        }
        std::vector<std::uint8_t> bytes = live::encode(m);
        if (bytes == p.last) continue;          // an identical message is not sent again
        p.last = bytes;
        p.told = true;
        send(*c, live::kMsgServerLive, bytes);
    }

    // 0x09: to each sender with bit 17, the other senders with bit 17,
    // where their hellos come from and their LAN, and each pair's key.
    // After 0x07, so that a client knows the ids first. Without Live (or
    // when it stops sending) a client that had records gets none.
    for (auto& kv : L.players) {
        const int id = kv.first;
        Live::Player& p = kv.second;
        Conn* c = connById(id);
        if (!c || !c->logged_in || p.key == 0 || !p.direct) continue;
        live::LivePaths m;
        const bool on = L.allowed() && L.open() && L.paths_ok;
        if (on && L.isSender(id) && !p.relay_only) {
            for (int sid : L.senders) {
                if (sid == id) continue;
                const Live::Player& q = L.players[sid];
                if (!q.direct || q.relay_only || !connById(sid)) continue;   // through the relay
                live::PathRecord r;
                r.id = static_cast<std::uint32_t>(sid);
                r.key = live::pairKey(L.path_secret, static_cast<std::uint32_t>(id), r.id);
                if (q.bound) {
                    r.public_ip = q.addr.ip;
                    r.public_port = ntohs(q.addr.port);
                }
                r.lan_ip = q.lan.ip;
                r.lan_port = ntohs(q.lan.port);
                m.peers.push_back(r);
            }
        }
        if (!on && p.last_paths.empty()) continue;   // never told: nothing to take back
        std::vector<std::uint8_t> bytes = live::encode(m);
        if (bytes == p.last_paths) continue;
        p.last_paths = bytes;
        send(*c, live::kMsgServerLivePaths, bytes);
    }
}

// ------------------------------------------------- the room and the advice

// A Live client's report (0x85); other types the server doesn't know are ignored.
void Server::liveClientMessage(Conn& c, const RawMessage& m) {
    if (!live_) return;
    if (m.type == live::kMsgClientLiveLan) {   // its LAN address, for the others' 0x09
        auto it = live_->players.find(c.id);
        live::LiveLan lan;
        if (it == live_->players.end() || !it->second.direct || !live::decode(m.payload, lan)) return;
        udp::Addr a;
        a.ip = lan.ip;
        a.port = htons(lan.port);
        if (a.ip == 0 || a.port == 0 || udp::isBroadcastOrMulticast(a.ip)) a = udp::Addr();
        if (a.ip != it->second.lan.ip || a.port != it->second.lan.port) live_->dirty = true;
        it->second.lan = a;
        return;
    }
    if (m.type == live::kMsgClientLivePaths) {   // how its direct paths stand, for the room's route
        auto it = live_->players.find(c.id);
        live::PathsReport r;
        if (it == live_->players.end() || !it->second.direct || !live::decode(m.payload, r)) return;
        if (it->second.relay_only != r.relayOnly()) live_->dirty = true;   // the 0x09s: listed or not
        it->second.relay_only = r.relayOnly();
        it->second.path_states.clear();
        for (const auto& e : r.peers) it->second.path_states[static_cast<int>(e.id)] = e.state;
        return;
    }
    if (m.type != live::kMsgClientLiveReport) return;
    auto it = live_->players.find(c.id);
    live::LiveReport r;
    if (it == live_->players.end() || !live::decode(m.payload, r)) return;
    Live::Player& p = it->second;
    if (!p.has_report || p.report.sending() != r.sending()) live_->dirty = true;   // the others' peer flags
    p.has_report = true;
    p.report = r;
    p.was_clean |= r.clean();
}

// The room's mode is its switch. The advice of live_room.h over the room's
// users, then the room message (0x08) to every Live player whose last one
// differs.
void Server::liveRoomTick(TimePoint now) {
    Live& L = *live_;
    auto secs = [&](TimePoint t) { return std::chrono::duration<double>(t - L.epoch).count(); };
    const Live::RouteCheck rc = L.hasPaths() ? L.checkRoute() : Live::RouteCheck();
    std::vector<live::RoomAdvice::Player> in;
    for (Conn* o : loggedIn()) {
        if (o->hidden()) continue;
        live::RoomAdvice::Player r;
        r.id = static_cast<std::uint32_t>(o->id);
        auto it = L.players.find(o->id);
        if (it == L.players.end()) {           // no Live client
            r.listener = o->maxchan <= 0;
        } else {
            const Live::Player& p = it->second;
            r.live_client = p.key != 0;
            r.listener = !p.channels;
            r.sender = L.isSender(o->id);
            r.bound = L.open() && p.bound;
            r.was_bound = p.was_bound;
            r.has_report = p.has_report;
            r.report = p.report;
            r.was_clean = p.was_clean;
            r.joined = secs(p.joined);
            auto np = rc.no_path.find(o->id);   // direct only: the pairs it can't play Live with
            if (!L.cfg.relay_fallback && np != rc.no_path.end()) r.no_path = np->second;
        }
        in.push_back(r);
    }
    const live::RoomAdvice::State before = L.advice.state();
    const live::RoomAdvice::State& st = L.advice.update(secs(now), L.roomOn(), in);
    Conn* who = st.user ? connById(static_cast<int>(st.user)) : nullptr;
    const std::string before_name = L.advice_name;
    L.advice_name = who ? who->name : std::string();
    liveRefusesUpload(now);                    // notes when the room went Live
    if (L.roomOn() != L.logged_on) {           // not "[live]": that is the traffic line
        log(L.roomOn() ? "[live room] the room is Live" : "[live room] the room is on intervals: Live is off");
        L.logged_on = L.roomOn();
    }
    if (st != before && L.roomOn() && (st.advice != before.advice || L.advice_name != before_name)) {
        const std::string u = who ? displayName(who) : std::string("someone");
        const std::string more = st.count > 1 ? " (" + std::to_string(st.count) + " players have trouble)" : "";
        if (st.advice == live::Advice::Dropouts)
            log("[live room] advice: " + u + "'s connection has dropouts" + more);
        else if (st.advice == live::Advice::CantLive)
            log("[live room] advice: " + u + " can't play Live" + more);
        else
            log("[live room] advice: none, everyone's Live sounds clean");
    }
    live::LiveRoom m;
    m.live = L.roomOn();
    m.advice = st.advice;
    m.user = L.advice_name;
    m.count = static_cast<std::uint16_t>(std::min(st.count, 0xffff));

    // The route: to the relay for everyone once a pair has no direct path
    // while the room is Live, and back only with `!live direct`.
    if (L.players.empty() || !L.cfg.relay_fallback) {   // a new room starts direct; without the option it stays so
        L.route = live::RoomRoute::Direct;
        L.why = live::RelayWhy::None;
        L.why_a.clear();
        L.why_b.clear();
    }
    if (L.hasPaths()) {
        m.relay_fallback = L.cfg.relay_fallback;
        auto nameOf = [&](int id) {
            Conn* o = id ? connById(id) : nullptr;
            return o ? o->name : std::string();
        };
        if (L.cfg.relay_fallback && L.route == live::RoomRoute::Direct && L.roomOn() && rc.why != live::RelayWhy::None) {
            L.route = live::RoomRoute::Relay;
            L.why = rc.why;
            L.why_a = nameOf(rc.a);
            L.why_b = nameOf(rc.b);
            L.possible_logged = false;
            log("[live room] Live goes through the relay for everyone now: " +
                whyText(rc.why, L.why_a, L.why_b, false));
        }
        m.route = L.route;
        if (L.route == live::RoomRoute::Relay) {
            m.why = L.why;
            m.why_a = L.why_a;
            m.why_b = L.why_b;
            m.direct_possible = rc.why == live::RelayWhy::None && rc.all_up;
            if (m.direct_possible != L.possible_logged) {
                L.possible_logged = m.direct_possible;
                if (m.direct_possible) log("[live room] advice: everyone can connect directly again");
            }
        }
    }
    const std::vector<std::uint8_t> bytes = live::encode(m);
    for (auto& kv : L.players) {
        Live::Player& p = kv.second;
        Conn* c = connById(kv.first);
        if (!c || !c->logged_in || p.last_room == bytes || (!L.allowed() && p.last_room.empty())) continue;
        p.last_room = bytes;
        send(*c, live::kMsgServerLiveRoom, bytes);
    }
}

// ---------------------------------------------------------------- the relay

// The relay's socket is read until it would block, then waited on (Asio's
// waits are not speculative: see server_net.h). A read that stops at its
// budget goes on in a handler of its own, after what else is ready.
void Server::armRelay() {
    Net& n = *net_;
    if (n.relay_reading || !live_ || !live_->open()) return;
    n.relay_reading = true;
    const unsigned e = n.epoch;
    asio::post(n.io, [this, e] {               // the first read: anything that came before the wait
        if (net_->epoch != e || !live_ || !live_->open()) return;
        net_->relay_reading = false;
        relayReadable();
    });
}

void Server::relayReadable() {
    Net& n = *net_;
    const unsigned e = n.epoch;
    const bool more = liveReadable();
    if (!live_ || !live_->open() || n.epoch != e) return;
    n.relay_reading = true;
    if (more) {
        asio::post(n.io, [this, e] {
            if (net_->epoch != e || !live_ || !live_->open()) return;
            net_->relay_reading = false;
            relayReadable();
        });
    } else {
        n.relay.async_wait(asio::socket_base::wait_read, [this, e](const asio::error_code& ec) {
            if (net_->epoch != e) return;
            net_->relay_reading = false;
            if (ec == asio::error::operation_aborted || !live_ || !live_->open()) return;
            relayReadable();
        });
    }
    if (live_->pass_wanted) {
        live_->pass_wanted = false;
        schedulePass();
    }
}

bool Server::liveReadable() {
    if (!live_ || !live_->open()) return false;
    Live& L = *live_;
    for (int budget = 0; budget < 4096; ++budget) {   // bounded, so TCP is served too
        std::size_t n = 0;
        bool truncated = false, has_local = false;
        udp::Addr from, local;
        udp::Recv r = udp::receive(L.fd, L.buf, sizeof(L.buf), n, truncated, from, local, has_local);
        if (r == udp::Recv::Empty) return false;
        if (r == udp::Recv::Error) continue;
        if (has_local && udp::isBroadcastOrMulticast(local.ip)) has_local = false;
        ++L.in;
        std::uint8_t* d = L.buf;
        auto drop = [&](Reason why) { ++L.dropped[static_cast<std::size_t>(why)]; };
        // 1-2: size and shape
        if (truncated || n > live::kMaxServerDatagram) {
            drop(kTooBig);
            continue;
        }
        if (n < live::kClientHeader || d[0] != live::kMagic0 || d[1] != live::kMagic1 ||
            d[2] != live::kUdpVersion ||
            !((d[3] == live::kTypeHello && n == live::kHelloSize) ||
              (d[3] == live::kTypeData && n > live::kClientHeader))) {
            drop(kMalformed);
            continue;
        }
        const std::uint32_t sender = live::getLE32(d + 4);
        const std::uint64_t key = live::getLE64(d + 8);
        const std::uint32_t dest = live::getLE32(d + 16);
        // 3-5: who
        Live::Player* p = L.known(sender);
        if (!p) {
            drop(kUnknownSender);
            continue;
        }
        if (!live::constantTimeEqual(key, p->key)) {
            drop(kWrongKey);
            continue;
        }
        if (L.cfg.strict_address && from.ip != p->tcp_ip) {
            drop(kWrongAddress);
            continue;
        }
        // 6: the rate limit, charged once per datagram that gets this far.
        // A player that sends to each of the others on its own (bit 17, to
        // those it can't reach directly) may send as many times the stream:
        // the buckets are that many times larger, and a datagram to
        // destination 0 costs that many, which leaves the copies the relay
        // makes where they were.
        const TimePoint now = Clock::now();
        const double fps = 48000.0 / L.cfg.frame;
        int others = static_cast<int>(L.senders.size()) - (L.isSender(static_cast<int>(sender)) ? 1 : 0);
        const double scale = std::max(1, others);
        const double cost = d[3] == live::kTypeData && dest == 0 ? scale : 1.0;
        const double byte_rate = scale * (2.0 * (L.cfg.kbps * 1000.0 / 8.0 + fps * 64.0) + 8000.0);
        const double dgram_rate = scale * (2.0 * fps + 50.0);
        if (!p->buckets) {
            p->buckets = true;
            p->bytes = byte_rate * 0.25;
            p->dgrams = dgram_rate * 0.25;
        } else {                               // a level above a new, smaller burst is cut to it
            double dt = std::chrono::duration<double>(now - p->refill).count();
            p->bytes = std::min(byte_rate * 0.25, p->bytes + dt * byte_rate);
            p->dgrams = std::min(dgram_rate * 0.25, p->dgrams + dt * dgram_rate);
        }
        p->refill = now;
        if (p->bytes < cost * static_cast<double>(n) || p->dgrams < cost) {
            drop(kOverRate);
            continue;
        }
        p->bytes -= cost * static_cast<double>(n);
        p->dgrams -= cost;

        // Sends from `src` when known; once more from where the routing
        // table chooses when that address is gone.
        auto sendFrom = [&](const std::uint8_t* b, std::size_t len, const udp::Addr& to, std::uint32_t src,
                            bool use_src, bool& src_gone) {
            src_gone = false;
            int err = udp::sendTo(L.fd, b, len, to, src, use_src);
            if (err != 0 && use_src && udp::isAddressGone(err)) {
                src_gone = true;
                err = udp::sendTo(L.fd, b, len, to, 0, false);
            }
            if (err != 0)
                drop(kCouldNotSend);
            else
                ++L.out;
        };

        if (d[3] == live::kTypeHello) {
            L.pass_wanted = true;
            // 7: always answered; bound only with the cookie sent to this very address
            std::uint8_t ip[4], port[2];
            std::memcpy(ip, &from.ip, 4);
            std::memcpy(port, &from.port, 2);
            const auto t = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            const std::uint32_t epoch = static_cast<std::uint32_t>(t / live::kCookieEpochSeconds);
            const std::uint64_t cur = live::helloCookie(L.secret, sender, key, ip, port, epoch);
            const std::uint64_t echoed = live::getLE64(d + 28);
            bool verified = live::constantTimeEqual(echoed, cur);
            if (epoch > 0)                      // the previous epoch too, not wrapping at 0
                verified |= live::constantTimeEqual(echoed,
                                                    live::helloCookie(L.secret, sender, key, ip, port, epoch - 1));
            std::uint8_t reply[live::kHelloAckSize];
            reply[0] = live::kMagic0;
            reply[1] = live::kMagic1;
            reply[2] = live::kUdpVersion;
            reply[3] = live::kTypeHello;
            live::putLE32(reply + 4, 0);
            std::memcpy(reply + 8, d + 20, 8);   // the timestamp, as received
            live::putLE64(reply + 16, cur);
            bool gone;
            sendFrom(reply, sizeof(reply), from, local.ip, has_local, gone);
            if (!verified) {
                drop(kUnverified);
                continue;
            }
            if (!p->bound || p->addr.ip != from.ip || p->addr.port != from.port) L.dirty = true;   // and 0x09
            p->bound = true;
            p->was_bound = true;
            p->addr = from;
            p->local = local.ip;
            p->has_local = has_local && !gone;
            p->last_hello = now;
            continue;
        }

        // data, 7-9
        if (!L.roomOn()) {
            drop(kRoomOff);
            continue;
        }
        if (!L.isSender(static_cast<int>(sender))) {
            drop(kMayNotSend);
            continue;
        }
        // The forwarded copy: the 8-byte server header over the last 8 bytes
        // of the received one, then the payload as received.
        std::uint8_t* fwd = d + (live::kClientHeader - live::kServerHeader);
        fwd[0] = live::kMagic0;
        fwd[1] = live::kMagic1;
        fwd[2] = live::kUdpVersion;
        fwd[3] = live::kTypeData;
        live::putLE32(fwd + 4, sender);
        const std::size_t flen = n - (live::kClientHeader - live::kServerHeader);
        auto copyTo = [&](Live::Player& to) {
            bool gone;
            const std::uint64_t out0 = L.out;
            sendFrom(fwd, flen, to.addr, to.local, to.has_local, gone);
            if (gone) to.has_local = false;     // until its next verified hello
            if (L.out != out0) live_relayed_.fetch_add(1, std::memory_order_relaxed);
        };
        if (dest != 0) {
            Live::Player* to = dest != sender && L.isSender(static_cast<int>(dest)) ? L.known(dest) : nullptr;
            if (!to || !to->bound) {
                drop(kNoSuchPlayer);
                continue;
            }
            copyTo(*to);
            continue;
        }
        int copies = 0;
        for (int sid : L.senders) {
            if (sid == static_cast<int>(sender) || copies >= live::kMaxCopies) continue;
            Live::Player* to = L.known(static_cast<std::uint32_t>(sid));
            if (!to || !to->bound) continue;
            ++copies;
            copyTo(*to);
        }
    }
    return true;
}

// ------------------------------------------------------------- !live

bool Server::liveCommand(Conn& c, const std::string& text) {
    std::size_t a = text.find_first_not_of(" \t");
    if (a == std::string::npos || text.size() - a < 5 || lowerCopy(text.substr(a, 5)) != "!live") return false;
    if (a + 5 < text.size() && text[a + 5] != ' ' && text[a + 5] != '\t') return false;   // !livestream
    if (!live_) return false;
    Live& L = *live_;
    std::string arg = lowerCopy(trimBlanks(text.substr(a + 5)));
    // `!live direct` only with the backend option relay_fallback, and it is
    // shown nowhere.
    bool on = arg == "on", off = arg == "off", direct = arg == "direct" && L.cfg.relay_fallback;
    if ((on || off || direct) && !(c.privs & kPrivVote)) {
        sendServerMsg(c, "[server] you don't have permission to change the room's audio");
        return true;
    }
    if (!L.allowed()) {
        sendServerMsg(c, "[server] this server doesn't have Live");
        return true;
    }
    if (!L.open()) {
        sendServerMsg(c, "[server] Live is off: the server could not open its UDP port");
        return true;
    }

    // The lists, from the room's users at this moment.
    std::vector<std::string> senders, cant, over;
    for (int id : L.senders)
        if (Conn* s = connById(id)) senders.push_back(s->name);
    for (Conn* o : loggedIn())
        if (!o->hidden() && !(o->caps & live::kCapLive)) cant.push_back(o->name);
    for (const auto& kv : L.players) {
        Conn* o = connById(kv.first);
        if (o && kv.second.channels && !L.isSender(kv.first)) over.push_back(o->name);
    }
    // `!live direct`: the room's route back to all-to-all, while no pair is
    // known to have no direct path.
    if (direct) {
        if (!L.hasPaths() || L.route != live::RoomRoute::Relay) {
            sendServerMsg(c, L.hasPaths() ? "[server] Live already goes directly between the players."
                                          : "[server] Live goes through the server here.");
            return true;
        }
        const Live::RouteCheck rc = L.checkRoute();
        if (rc.why != live::RelayWhy::None) {
            auto nameOf = [&](int id) {
                Conn* o = id ? connById(id) : nullptr;
                return o ? displayName(o) : std::string("someone");
            };
            sendServerMsg(c, "[server] Live stays on the server's relay: " + whyText(rc.why, nameOf(rc.a), nameOf(rc.b), true) +
                                 ".");
            return true;
        }
        L.route = live::RoomRoute::Direct;
        L.why = live::RelayWhy::None;
        L.why_a.clear();
        L.why_b.clear();
        L.possible_logged = false;
        sendServerMsg(c, "[server] Live goes directly between the players again.");
        log("[live room] " + c.name + " switched Live to direct");
        return true;
    }
    auto overText = [&](const char* tail) {
        return "Live has room for " + std::to_string(L.cfg.max_peers) + ", so " + joinNames(over) +
               (over.size() == 1 ? " isn't heard while it is on" : " aren't heard while it is on") + tail;
    };

    if (!(on || off) || on == L.room_on) {
        if (on || off) L.room_chosen = true;    // the room's choice, even when nothing changes
        std::string s = L.room_on ? "[server] Live is on. " : "[server] Live is off. ";
        s += senders.empty() ? "Nobody here can play Live yet. "
                             : "Players who can play Live: " + joinNames(senders) + ". ";
        if (!over.empty()) s += overText(". ");
        if (L.hasPaths() && L.route == live::RoomRoute::Relay) {
            const Live::RouteCheck rc = L.checkRoute();
            s += "It goes through the server's relay";
            if (L.why != live::RelayWhy::None) s += " since " + whyText(L.why, L.why_a, L.why_b, false);
            s += rc.why == live::RelayWhy::None && rc.all_up ? ". Everyone can connect directly again. " : ". ";
        }
        s += L.room_on ? "Turn it off with !live off" : "Turn it on with !live on";
        sendServerMsg(c, s);
        return true;
    }

    L.room_on = on;
    L.room_chosen = true;
    std::string who = displayName(&c);
    std::string s;
    if (on) {
        s = "[server] " + who + " turned Live on: O11 Session players now hear each other in real time.";
        if (cant.size() == 1)
            s += " " + cant[0] + "'s client can't play Live, so " + cant[0] + " isn't heard until Live is off.";
        else if (cant.size() > 1)
            s += " The clients of " + joinNames(cant) + " can't play Live, so they aren't heard until Live is off.";
        if (!over.empty()) s += " " + overText(".");
    } else {
        s = "[server] " + who + " turned Live off: everyone hears each other through intervals again.";
    }
    broadcastChat(ChatMessage("MSG", "", s));
    L.dirty = true;
    return true;
}

}  // namespace o11proto
