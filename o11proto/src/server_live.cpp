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
#include "o11proto/server.h"
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
    };

    LiveConfig cfg;
    bool room_on = false;                      // the room's switch (the whole server is one room)
    bool room_chosen = false;                  // switched with !live: survives reloads
    socket_t fd = kInvalidSocket;
    std::uint16_t port = 0;
    std::uint8_t secret[16] = {};
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

    bool allowed() const { return cfg.mode != LiveMode::No; }
    bool open() const { return fd != kInvalidSocket; }
    bool roomOn() const { return allowed() && open() && room_on; }
    bool isSender(int id) const { return std::find(senders.begin(), senders.end(), id) != senders.end(); }
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
        for (auto& kv : players) kv.second.bound = kv.second.has_local = kv.second.buckets = false;
        cfg.fresh_seconds = envSeconds("NINJAM_LIVE_FRESH_SECONDS", cfg.fresh_seconds);
        cfg.stats_seconds = envSeconds("NINJAM_LIVE_STATS_SECONDS", cfg.stats_seconds);
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

// Opens the relay's socket on `port` (on `bind_address` when the TCP
// listener has one) and draws a new cookie secret.
OpenResult openRelaySocket(std::uint16_t port, const std::string& bind_address, std::uint8_t secret[16],
                           socket_t& out) {
    out = kInvalidSocket;
    socket_t fd = udp::open(live::kRelaySocketBuffer, true);   // no address reuse
    if (fd == kInvalidSocket) return OpenResult::Busy;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    std::uint32_t ip = 0;
    if (!bind_address.empty() && udp::parseIPv4(bind_address, ip)) sa.sin_addr.s_addr = ip;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        compat::closeSocket(fd);
        return OpenResult::Busy;
    }
    if (!live::systemRandom(secret, 16)) {     // no secret, no cookies: the port stays closed
        compat::closeSocket(fd);
        return OpenResult::NoRandom;
    }
    udp::reportLocalAddress(fd);
    out = fd;
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
    socket_t fd;
    OpenResult r = openRelaySocket(port_, cfg_.bind_address, L.secret, fd);
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
    L.opened(fd, port_);
    log("Live: listening on UDP port " + p);
    return true;
}

void Server::liveShutdown() {
    if (!live_ || live_->fd == kInvalidSocket) return;
    compat::closeSocket(live_->fd);
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
        socket_t fd;
        OpenResult r = openRelaySocket(port_, cfg_.bind_address, L.secret, fd);
        if (r == OpenResult::Ok) {
            L.opened(fd, port_);
            log("Live: listening on UDP port " + p);
        } else {                               // require doesn't stop a running server
            log(r == OpenResult::Busy ? busyWarning(p) : randomWarning(p));
        }
    } else {
        L.cfg.fresh_seconds = envSeconds("NINJAM_LIVE_FRESH_SECONDS", L.cfg.fresh_seconds);
        L.cfg.stats_seconds = envSeconds("NINJAM_LIVE_STATS_SECONDS", L.cfg.stats_seconds);
    }
    if (!L.room_chosen) L.room_on = L.cfg.mode == LiveMode::On;
    L.dirty = true;
}

bool Server::liveUdpOpen() const { return live_ && live_->open(); }
std::uint16_t Server::liveUdpPort() const { return live_ ? live_->port : 0; }
bool Server::liveRoomOn() const { return live_ && live_->roomOn(); }
socket_t Server::liveSocket() const { return live_ ? live_->fd : kInvalidSocket; }

int Server::livePollTimeout(int timeout_ms) const {
    // Freshness is checked every 100 ms while the port is open.
    if (live_ && live_->open() && (timeout_ms < 0 || timeout_ms > 100)) return 100;
    return timeout_ms;
}

// ------------------------------------------------------- players come and go

void Server::liveJoined(Conn& c) {
    if (!live_ || !(c.caps & live::kCapLive) || c.hidden()) return;
    Live::Player p;
    udp::parseIPv4(c.addr, p.tcp_ip);
    p.channels = c.maxchan > 0;
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
    if (!L.dirty) return;
    L.dirty = false;

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
                r.flags = L.open() && L.players[sid].bound ? live::kPeerBound : 0;
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
}

// ---------------------------------------------------------------- the relay

void Server::liveReadable() {
    if (!live_ || !live_->open()) return;
    Live& L = *live_;
    for (int budget = 0; budget < 4096; ++budget) {   // bounded, so TCP is served too
        std::size_t n = 0;
        bool truncated = false, has_local = false;
        udp::Addr from, local;
        udp::Recv r = udp::receive(L.fd, L.buf, sizeof(L.buf), n, truncated, from, local, has_local);
        if (r == udp::Recv::Empty) return;
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
        // 6: the rate limit, charged once per datagram that gets this far
        const TimePoint now = Clock::now();
        const double fps = 48000.0 / L.cfg.frame;
        const double byte_rate = 2.0 * (L.cfg.kbps * 1000.0 / 8.0 + fps * 64.0) + 8000.0;
        const double dgram_rate = 2.0 * fps + 50.0;
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
        if (p->bytes < static_cast<double>(n) || p->dgrams < 1.0) {
            drop(kOverRate);
            continue;
        }
        p->bytes -= static_cast<double>(n);
        p->dgrams -= 1.0;

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
            if (!p->bound) L.dirty = true;
            p->bound = true;
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
            sendFrom(fwd, flen, to.addr, to.local, to.has_local, gone);
            if (gone) to.has_local = false;     // until its next verified hello
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
}

// ------------------------------------------------------------- !live

bool Server::liveCommand(Conn& c, const std::string& text) {
    std::size_t a = text.find_first_not_of(" \t");
    if (a == std::string::npos || text.size() - a < 5 || lowerCopy(text.substr(a, 5)) != "!live") return false;
    if (a + 5 < text.size() && text[a + 5] != ' ' && text[a + 5] != '\t') return false;   // !livestream
    if (!live_) return false;
    Live& L = *live_;
    std::string arg = lowerCopy(trimBlanks(text.substr(a + 5)));
    bool on = arg == "on", off = arg == "off";
    if ((on || off) && !(c.privs & kPrivVote)) {
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
    auto overText = [&](const char* tail) {
        return "Live has room for " + std::to_string(L.cfg.max_peers) + ", so " + joinNames(over) +
               (over.size() == 1 ? " stays on intervals" : " stay on intervals") + tail;
    };

    if (!(on || off) || on == L.room_on) {
        if (on || off) L.room_chosen = true;    // the room's choice, even when nothing changes
        std::string s = L.room_on ? "[server] Live is on. " : "[server] Live is off. ";
        s += senders.empty() ? "Nobody here can play Live yet. "
                             : "Players who can play Live: " + joinNames(senders) + ". ";
        if (!over.empty()) s += overText(". ");
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
            s += " " + cant[0] + "'s client can't play Live and stays on intervals.";
        else if (cant.size() > 1)
            s += " The clients of " + joinNames(cant) + " can't play Live, so they stay on intervals.";
        if (!over.empty()) s += " " + overText(".");
    } else {
        s = "[server] " + who + " turned Live off: everyone hears each other through intervals again.";
    }
    broadcastChat(ChatMessage("MSG", "", s));
    L.dirty = true;
    return true;
}

}  // namespace o11proto
