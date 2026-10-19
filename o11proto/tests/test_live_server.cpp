// The server's Live mode (PROTOCOL.md section 15): the Live message, !live
// and the UDP relay, following the conformance tests of the Live spec's
// section 9 that need no audio. Clients are o11proto Clients with Live on;
// the UDP side is driven by raw test sockets. Everything but the latency
// test runs in one thread that pumps the server and the clients.
#include <algorithm>
#include <cmath>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "o11proto/client.h"
#include "o11proto/live.h"
#include "o11proto/realtime.h"
#include "o11proto/server.h"
#include "socket_compat.h"
#include "udp_compat.h"

using namespace o11proto;
using Bytes = std::vector<std::uint8_t>;

static void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ------------------------------------------------------------- datagrams

static Bytes header(std::uint8_t type, std::uint32_t id, std::uint64_t key, std::uint32_t dest) {
    Bytes b(live::kClientHeader);
    b[0] = 'N';
    b[1] = 'L';
    b[2] = 1;
    b[3] = type;
    live::putLE32(&b[4], id);
    live::putLE64(&b[8], key);
    live::putLE32(&b[16], dest);
    return b;
}
static Bytes hello(std::uint32_t id, std::uint64_t key, std::uint64_t ts, std::uint64_t cookie) {
    Bytes b = header(live::kTypeHello, id, key, 0);
    b.resize(live::kHelloSize, 0);
    live::putLE64(&b[20], ts);
    live::putLE64(&b[28], cookie);
    return b;
}
static Bytes data(std::uint32_t id, std::uint64_t key, std::uint32_t dest, const Bytes& payload) {
    Bytes b = header(live::kTypeData, id, key, dest);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
static Bytes payloadOf(std::size_t n, std::uint8_t seed) {
    Bytes p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(seed + i * 7);
    return p;
}

// A raw UDP test socket.
struct Udp {
    socket_t fd = kInvalidSocket;
    std::uint16_t serverPort = 0;
    std::string serverIp = "127.0.0.1";
    ~Udp() {
        if (fd != kInvalidSocket) compat::closeSocket(fd);
    }
    // Binds to `ip` and `localPort` (0: any); with `connectToServer`,
    // accepts only the server.
    bool open(std::uint16_t port, const char* ip = "127.0.0.1", bool connectToServer = false,
              const char* server = "127.0.0.1", std::uint16_t localPort = 0) {
        serverPort = port;
        serverIp = server;
        fd = udp::open(1 << 20, false);
        if (fd == kInvalidSocket) return false;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(localPort);
        inet_pton(AF_INET, ip, &sa.sin_addr);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            compat::closeSocket(fd);
            fd = kInvalidSocket;
            return false;
        }
        if (connectToServer) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(port);
            inet_pton(AF_INET, server, &to.sin_addr);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) return false;
        }
        return true;
    }
    void send(const Bytes& b) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIp.c_str(), &to.sin_addr);
#ifdef _WIN32
        ::sendto(fd, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to));
#else
        ::sendto(fd, b.data(), b.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
#endif
    }
    bool tryRecv(Bytes& out) {
        std::uint8_t buf[2048];
        std::size_t n = 0;
        bool trunc = false, has_local = false;
        udp::Addr from, local;
        for (;;) {
            udp::Recv r = udp::receive(fd, buf, sizeof(buf), n, trunc, from, local, has_local);
            if (r == udp::Recv::Empty) return false;
            if (r == udp::Recv::Datagram) break;
        }
        out.assign(buf, buf + n);
        return true;
    }
};

// ------------------------------------------------------------- harness

struct LiveClient {
    Client c;
    std::vector<live::ServerLive> msgs;
    std::vector<ChatMessage> chats;
    const live::ServerLive& last() const { return msgs.back(); }
    std::uint32_t id() const { return msgs.empty() ? 0 : msgs.back().peer_id; }
    std::uint64_t key() const { return msgs.empty() ? 0 : msgs.back().key; }
    bool lists(std::uint32_t peer) const {
        if (msgs.empty()) return false;
        for (const auto& p : msgs.back().peers)
            if (p.id == peer) return true;
        return false;
    }
    bool listsBound(std::uint32_t peer) const {
        if (msgs.empty()) return false;
        for (const auto& p : msgs.back().peers)
            if (p.id == peer) return (p.flags & live::kPeerBound) != 0;
        return false;
    }
    bool chatHas(const std::string& needle) const {
        for (const auto& m : chats)
            if (m.fields[2].find(needle) != std::string::npos) return true;
        return false;
    }
    std::string lastServerLine() const {
        for (auto it = chats.rbegin(); it != chats.rend(); ++it)
            if (it->fields[0] == "MSG" && it->fields[1].empty()) return it->fields[2];
        return "";
    }
};

struct Harness {
    Server server;
    std::vector<Client*> clients;
    std::vector<std::string> log;

    explicit Harness(LiveMode mode, std::function<void(ServerConfig&)> tweak = nullptr) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote | kPrivMulti;
        cfg.keepalive_seconds = 3;
        cfg.live.mode = mode;
        cfg.users.push_back({"novote", "pw", kPrivChat});
        cfg.users.push_back({"ghost", "pw", kPrivChat | kPrivVote | kPrivHidden});
        cfg.log = [this](const std::string& s) { log.push_back(s); };
        if (tweak) tweak(cfg);
        bool ok = server.start(cfg);
        // Live's UDP port has the TCP port's number, which another program on
        // the machine may hold: then another port.
        for (int i = 0; ok && cfg.live.mode != LiveMode::No && !server.liveUdpOpen() && i < 5; ++i) {
            std::printf("    UDP port %u is taken: the server moves\n", server.port());
            server.stop();
            log.clear();
            ok = server.start(cfg);
        }
        CHECK(ok);
    }
    std::uint16_t port() const { return server.port(); }
    void pumpOnce() {
        server.poll(1);
        for (Client* c : clients) c->poll(0);
    }
    void pump(int ms) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(ms);
        do pumpOnce();
        while (Clock::now() < deadline);
    }
    bool until(const std::function<bool()>& pred, int timeout_ms = 2000) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (Clock::now() >= deadline) return false;
            pumpOnce();
        }
        return true;
    }
    // Logs in; with `live`, also waits for the first Live message.
    bool login(LiveClient& lc, const std::string& user = "anonymous:p", bool live = true,
               const std::string& pass = "", const char* host = "127.0.0.1", bool waitLive = true) {
        lc.c.on_live = [&lc](const live::ServerLive& m) { lc.msgs.push_back(m); };
        lc.c.on_chat = [&lc](const ChatMessage& m) { lc.chats.push_back(m); };
        clients.push_back(&lc.c);
        Client::Options o;
        o.username = user;
        o.password = pass;
        o.codec_caps = kCapOpus;
        o.live = live;
        ChannelInfoRecord ch;
        ch.name = "ch0";
        lc.c.setChannels({ch});
        if (!lc.c.connect(host, port(), o)) return false;
        if (!until([&] { return lc.c.loggedIn() || lc.c.state() == Client::State::Failed; }) || !lc.c.loggedIn())
            return false;
        return !live || !waitLive || until([&] { return !lc.msgs.empty(); });
    }
    void drop(LiveClient& lc) {
        lc.c.disconnect();
        clients.erase(std::remove(clients.begin(), clients.end(), &lc.c), clients.end());
        pump(30);
    }
    // Waits for a datagram on `u`, pumping everything meanwhile.
    bool recv(Udp& u, Bytes& out, int timeout_ms = 500) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (u.tryRecv(out)) return true;
            if (Clock::now() >= deadline) return false;
            pumpOnce();
        }
    }
    // True when nothing arrives within `ms`.
    bool silent(Udp& u, int ms = 200) {
        Bytes b;
        return !recv(u, b, ms);
    }
    // Sends a hello and returns the reply's cookie (0: no reply).
    std::uint64_t helloCookie(Udp& u, LiveClient& lc, std::uint64_t cookie = 0, std::uint64_t ts = 0x8000000000000001ULL) {
        u.send(hello(lc.id(), lc.key(), ts, cookie));
        Bytes r;
        if (!recv(u, r)) return 0;
        if (r.size() != live::kHelloAckSize || r[3] != live::kTypeHello) return 0;
        return live::getLE64(&r[16]);
    }
    // A hello, then a hello echoing the reply's cookie.
    bool bind(Udp& u, LiveClient& lc) {
        std::uint64_t c = helloCookie(u, lc);
        if (!c) return false;
        return helloCookie(u, lc, c) == c;
    }
    int logCount(const std::string& needle) const {
        int n = 0;
        for (const auto& l : log)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

// Drains everything waiting on `u`; returns how many datagrams.
static int drain(Harness& h, Udp& u, int quiet_ms = 100) {
    int n = 0;
    Bytes b;
    while (h.recv(u, b, quiet_ms)) ++n;
    return n;
}

// Is a payload `p` from `from` what arrived in `got`?
static bool isData(const Bytes& got, std::uint32_t from, const Bytes& p) {
    return got.size() == live::kServerHeader + p.size() && got[0] == 'N' && got[1] == 'L' && got[2] == 1 &&
           got[3] == live::kTypeData && live::getLE32(&got[4]) == from &&
           std::equal(p.begin(), p.end(), got.begin() + live::kServerHeader);
}

// ------------------------------------------------------------- the cookie (28)

TEST(cookie_vectors) {
    std::uint8_t secret[16];
    for (int i = 0; i < 16; ++i) secret[i] = static_cast<std::uint8_t>(i);
    std::uint8_t msg[15];
    for (int i = 0; i < 15; ++i) msg[i] = static_cast<std::uint8_t>(i);
    CHECK_EQ(live::siphash24(secret, msg, 15), 0xa129ca6149be45e5ULL);
    const std::uint8_t ip1[4] = {127, 0, 0, 1}, port1[2] = {0xc3, 0x50};   // 50000
    CHECK_EQ(live::helloCookie(secret, 1, 0x0123456789abcdefULL, ip1, port1, 1000), 0xc180445be3d73103ULL);
    const std::uint8_t ip2[4] = {192, 0, 2, 10}, port2[2] = {0x08, 0x01};   // 2049
    CHECK_EQ(live::helloCookie(secret, 7, 0xfedcba9876543210ULL, ip2, port2, 57000000), 0xf1620faed24ff4a6ULL);
    std::uint8_t le[8];
    live::putLE64(le, 0xc180445be3d73103ULL);
    const std::uint8_t want[8] = {0x03, 0x31, 0xd7, 0xe3, 0x5b, 0x44, 0x80, 0xc1};
    CHECK(std::memcmp(le, want, 8) == 0);
    CHECK(live::constantTimeEqual(5, 5));
    CHECK(!live::constantTimeEqual(5, 5 | (1ULL << 63)));
}

// ------------------------------------------------------------- TCP side

TEST(lone_client_and_old_client) {                 // 1
    Harness h(LiveMode::Yes);
    LiveClient a, old;
    REQUIRE(h.login(a));
    h.pump(100);
    REQUIRE_EQ(a.msgs.size(), 1u);
    const auto& m = a.last();
    CHECK_EQ(m.version, 1);
    CHECK_EQ(m.flags, live::kFlagUdpOpen | live::kFlagMaySend);
    CHECK_EQ(m.udp_port, 0);
    CHECK_EQ(m.live_kbps, 128);
    CHECK_EQ(m.frame_samples, live::kDefaultFrame);
    CHECK_EQ(m.frame_samples, 480);
    CHECK_EQ(m.max_peers, 8);
    CHECK(m.peer_id != 0);
    CHECK(m.key != 0);
    CHECK(m.peers.empty());
    REQUIRE(h.login(old, "anonymous:old", false));
    h.pump(500);
    CHECK(old.msgs.empty());
    CHECK(!old.c.hasLiveMessage());
    CHECK(h.server.liveUdpOpen());
    CHECK_EQ(h.server.liveUdpPort(), h.port());
}

TEST(configured_values) {                          // 2
    Harness h(LiveMode::On, [](ServerConfig& c) {
        c.live.kbps = 96;
        c.live.frame = 120;
        c.live.max_peers = 2;
        c.live.announce_port = 4000;
    });
    LiveClient a;
    REQUIRE(h.login(a));
    const auto& m = a.last();
    CHECK_EQ(m.flags, live::kFlagRoomOn | live::kFlagUdpOpen | live::kFlagMaySend);
    CHECK_EQ(m.live_kbps, 96);
    CHECK_EQ(m.frame_samples, 120);
    CHECK_EQ(m.max_peers, 2);
    CHECK_EQ(m.udp_port, 4000);
}

TEST(lists_and_bound) {                             // 3, 15
    Harness h(LiveMode::On);
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    REQUIRE(h.until([&] { return a.lists(b.id()) && b.lists(a.id()); }));
    CHECK(!a.lists(a.id()));
    CHECK(!b.lists(b.id()));
    CHECK(!a.listsBound(b.id()));
    CHECK_EQ(a.last().peers[0].name, b.c.username());
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));
    // b never bound: no audio for it (15)
    ua.send(data(a.id(), a.key(), 0, payloadOf(50, 1)));
    CHECK(h.silent(ub));
    REQUIRE(h.bind(ub, b));
    CHECK(h.until([&] { return a.listsBound(b.id()) && b.listsBound(a.id()); }, 1000));
    Bytes p = payloadOf(50, 2), got;
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
    h.drop(b);
    CHECK(h.until([&] { return !a.lists(b.id()) && a.last().peers.empty(); }));
}

TEST(sender_limit) {                                // 4
    Harness h(LiveMode::On, [](ServerConfig& c) { c.live.max_peers = 2; });
    LiveClient a, b, c;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    REQUIRE(h.login(c, "anonymous:c"));
    h.pump(50);
    CHECK_EQ(c.last().flags, live::kFlagRoomOn | live::kFlagUdpOpen);
    CHECK(c.lists(a.id()) && c.lists(b.id()));
    CHECK(!a.lists(c.id()) && !b.lists(c.id()));
    CHECK(a.last().maySend() && b.last().maySend());
    // over the limit: its data goes nowhere
    Udp ua, uc;
    REQUIRE(ua.open(h.port()));
    REQUIRE(uc.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(uc, c));
    uc.send(data(c.id(), c.key(), 0, payloadOf(40, 3)));
    CHECK(h.silent(ua));
    h.drop(b);
    CHECK(h.until([&] { return c.last().maySend() && a.lists(c.id()); }));
}

TEST(listener_only) {                               // 5 (with a server that gives no channels)
    Harness h(LiveMode::Yes, [](ServerConfig& c) { c.max_channels = 0; });
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    h.pump(50);
    CHECK_EQ(b.last().flags, live::kFlagUdpOpen);
    CHECK(b.last().peers.empty());
    CHECK(a.last().peers.empty());
    Udp ub;
    REQUIRE(ub.open(h.port()));
    CHECK(h.helloCookie(ub, b) != 0);            // its hellos are answered
    a.c.say("!live");
    CHECK(h.until([&] { return a.chatHas("Nobody here can play Live yet"); }));
}

TEST(live_texts) {                                  // 6
    Harness h(LiveMode::Yes, [](ServerConfig& c) { c.live.max_peers = 2; });
    LiveClient a;
    REQUIRE(h.login(a, "anonymous:a"));
    a.c.say("!live");
    CHECK(h.until([&] { return !a.lastServerLine().empty(); }));
    const std::string an = a.c.username();
    CHECK_EQ(a.lastServerLine(), "[server] Live is off. Players who can play Live: " + an + ". Turn it on with !live on");
    LiveClient b, c, old;
    REQUIRE(h.login(b, "anonymous:b"));
    REQUIRE(h.login(c, "anonymous:c"));
    REQUIRE(h.login(old, "anonymous:old", false));
    a.chats.clear();
    a.c.say("  !live");
    CHECK(h.until([&] { return !a.lastServerLine().empty(); }));
    const std::string bn = b.c.username(), cn = c.c.username(), on = old.c.username();
    CHECK_EQ(a.lastServerLine(), "[server] Live is off. Players who can play Live: " + an + " and " + bn +
                                     ". Live has room for 2, so " + cn + " isn't heard while it is on. Turn it on with !live on");
    // refusal 1
    LiveClient nv;
    REQUIRE(h.login(nv, "novote", true, "pw"));
    nv.c.say("!live on");
    CHECK(h.until([&] { return nv.chatHas("[server] you don't have permission to change the room's audio"); }));
    // the on broadcast reaches everyone, the old client too
    a.chats.clear();
    old.chats.clear();
    a.c.say("!LIVE ON");
    const std::string onText = "[server] " + an + " turned Live on: O11 Session players now hear each other in "
                               "real time. " + on + "'s client can't play Live, so " + on + " isn't heard until "
                               "Live is off. Live has room for 2, so " + cn + " and " + nv.c.username() +
                               " aren't heard while it is on.";
    CHECK(h.until([&] { return a.lastServerLine() == onText && old.lastServerLine() == onText; }));
    CHECK(h.until([&] { return a.last().roomOn() && b.last().roomOn() && c.last().roomOn(); }));
    CHECK(h.server.liveRoomOn());
    // a hidden player's command shows "someone"
    LiveClient ghost;
    REQUIRE(h.login(ghost, "ghost", true, "pw", "127.0.0.1", false));
    h.pump(100);                                // hidden users never get 0x07
    CHECK(ghost.msgs.empty());
    ghost.c.say("!live off");
    const std::string offText = "[server] someone turned Live off: everyone hears each other through intervals again.";
    CHECK(h.until([&] { return a.lastServerLine() == offText && old.lastServerLine() == offText &&
                               ghost.lastServerLine() == offText; }));
    CHECK(h.until([&] { return !a.last().roomOn(); }));
    // !livestream is not !live
    a.c.say("!livestream");
    CHECK(h.until([&] {
        for (const auto& m : a.chats)
            if (m.fields[0] == "PRIVMSG" &&
                m.fields[2] == "Unknown !command. Commands available: !vote, !topic, !codec, !bitrate, !live")
                return true;
        return false;
    }));
    // with several cant
    LiveClient old2;
    REQUIRE(h.login(old2, "anonymous:old2", false));
    a.chats.clear();
    a.c.say("!live on");
    CHECK(h.until([&] { return a.chatHas("The clients of " + on + " and " + old2.c.username() +
                                         " can't play Live, so they aren't heard until Live is off."); }));
}

TEST(no_live_server) {                              // 6: answer 2
    Harness h(LiveMode::No);
    LiveClient a;
    REQUIRE(h.login(a, "anonymous:a", true, "", "127.0.0.1", false));
    h.pump(300);
    CHECK(a.msgs.empty());
    a.c.say("!live on");
    CHECK(h.until([&] { return a.chatHas("[server] this server doesn't have Live"); }));
    CHECK(!h.server.liveUdpOpen());
}

TEST(reload_keeps_choice) {                         // 7, 26
    Harness h(LiveMode::On);
    LiveClient a;
    REQUIRE(h.login(a, "anonymous:a"));
    CHECK(a.last().roomOn());
    LiveConfig yes;
    yes.mode = LiveMode::Yes;
    h.server.setLiveConfig(yes);                // no choice: follows the default
    CHECK(h.until([&] { return !a.last().roomOn(); }));
    LiveConfig on;
    on.mode = LiveMode::On;
    h.server.setLiveConfig(on);
    CHECK(h.until([&] { return a.last().roomOn(); }));
    a.c.say("!live on");                       // matches the state, but is the room's choice
    CHECK(h.until([&] { return a.chatHas("[server] Live is on."); }));
    h.server.setLiveConfig(yes);
    h.pump(100);
    CHECK(a.last().roomOn());
    // 26: Live no takes it away, the port closes; back on, the choice holds
    std::uint16_t port = h.server.liveUdpPort();
    LiveConfig no;
    h.server.setLiveConfig(no);
    CHECK(h.until([&] { return a.last().flags == 0 && a.last().peers.empty(); }));
    CHECK(h.logCount("Live is off, closed UDP port " + std::to_string(port)) == 1);
    CHECK(!h.server.liveUdpOpen());
    Udp probe;
    CHECK(probe.open(0, "0.0.0.0", false, "127.0.0.1", port));   // the port is free again
    Udp u;
    REQUIRE(u.open(h.port()));
    u.send(hello(a.id(), a.key(), 1, 0));
    CHECK(h.silent(u));
    if (probe.fd != kInvalidSocket) {
        compat::closeSocket(probe.fd);
        probe.fd = kInvalidSocket;
    }
    h.server.setLiveConfig(yes);
    CHECK(h.until([&] { return a.last().flags == (live::kFlagRoomOn | live::kFlagUdpOpen | live::kFlagMaySend); }));
    LiveClient b;
    REQUIRE(h.login(b, "anonymous:b"));
    CHECK_EQ(b.last().flags, live::kFlagRoomOn | live::kFlagUdpOpen | live::kFlagMaySend);
    Udp ub;
    REQUIRE(ub.open(h.port()));
    CHECK(h.bind(ub, b));
}

// ------------------------------------------------------------- UDP

TEST(hello_reply) {                                 // 8
    Harness h(LiveMode::On);
    LiveClient a;
    REQUIRE(h.login(a));
    Udp u, u2;
    REQUIRE(u.open(h.port()));
    REQUIRE(u2.open(h.port()));
    const std::uint64_t ts = 0x8000001234567890ULL;
    u.send(hello(a.id(), a.key(), ts, 0));
    Bytes r;
    REQUIRE(h.recv(u, r));
    REQUIRE_EQ(r.size(), live::kHelloAckSize);
    const std::uint8_t head[8] = {0x4e, 0x4c, 0x01, 0x01, 0, 0, 0, 0};
    CHECK(std::memcmp(r.data(), head, 8) == 0);
    CHECK_EQ(live::getLE64(&r[8]), ts);
    std::uint64_t c = live::getLE64(&r[16]);
    CHECK(c != 0);
    CHECK_EQ(h.helloCookie(u, a), c);
    std::uint64_t c2 = h.helloCookie(u2, a);
    CHECK(c2 != 0 && c2 != c);
}

TEST(forwarding) {                                  // 9
    Harness h(LiveMode::On);
    LiveClient a, b, c;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    REQUIRE(h.login(c, "anonymous:c"));
    Udp ua, ub, uc;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(uc.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    REQUIRE(h.bind(uc, c));
    Bytes p = payloadOf(100, 9), got;
    ua.send(data(a.id(), a.key(), b.id(), p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
    CHECK(h.silent(uc, 100));
    CHECK(h.silent(ua, 50));
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
    CHECK(h.recv(uc, got) && isData(got, a.id(), p));
    CHECK(h.silent(ua, 100));
    ua.send(data(a.id(), a.key(), a.id(), p));  // to itself
    ua.send(data(a.id(), a.key(), 999999, p));  // unknown
    CHECK(h.silent(ub, 100) && h.silent(uc, 50) && h.silent(ua, 50));
    Bytes big = payloadOf(live::kMaxPayload, 4);
    ua.send(data(a.id(), a.key(), b.id(), big));
    CHECK(h.recv(ub, got) && isData(got, a.id(), big));
    ua.send(data(a.id(), a.key(), b.id(), payloadOf(live::kMaxPayload + 1, 4)));
    CHECK(h.silent(ub));
}

TEST(refused_datagrams) {                           // 10, 17
    Harness h(LiveMode::On);
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    drain(h, ua);
    drain(h, ub);
    Bytes p = payloadOf(30, 5);
    std::vector<Bytes> bad;
    bad.push_back(data(a.id(), a.key() ^ 1, 0, p));                  // wrong key
    bad.push_back(data(4000000, a.key(), 0, p));                     // unknown sender
    bad.push_back(data(b.id(), a.key(), 0, p));                      // another's id, own key
    Bytes m = data(a.id(), a.key(), 0, p);
    m[0] = 'X';
    bad.push_back(m);                                                // magic XL
    m = data(a.id(), a.key(), 0, p);
    m[2] = 2;
    bad.push_back(m);                                                // version 2
    m = data(a.id(), a.key(), 0, p);
    m[3] = 3;
    bad.push_back(m);                                                // type 3
    bad.push_back(header(live::kTypeData, a.id(), a.key(), 0));      // the header alone
    bad.push_back(Bytes(m.begin(), m.begin() + 19));                 // 19 bytes
    for (std::size_t n : {39u, 32u, 41u}) {                          // hellos of the wrong size
        Bytes hb = hello(a.id(), a.key(), 1, 0);
        hb.resize(n, 0);
        bad.push_back(hb);
    }
    bad.push_back(Bytes());                                          // 0 bytes
    bad.push_back(Bytes(1, 'N'));                                    // 1 byte
    bad.push_back(data(a.id(), a.key(), 0, payloadOf(3000, 1)));    // 3020 bytes
    for (const auto& d : bad) ua.send(d);
    {   // 60020 bytes
        Bytes huge = data(a.id(), a.key(), 0, payloadOf(60000, 1));
        ua.send(huge);
    }
    for (std::uint32_t id = 1; id < 64; ++id) ua.send(hello(id, 0, 1, 0));   // 17: an old client guessing
    CHECK(h.silent(ua, 300));
    CHECK(h.silent(ub, 50));
    Bytes got;
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
}

TEST(strict_address) {                              // 11
    for (bool strict : {true, false}) {
        Harness h(LiveMode::On, [strict](ServerConfig& c) { c.live.strict_address = strict; });
        LiveClient a, b;
        REQUIRE(h.login(a, "anonymous:a"));
        REQUIRE(h.login(b, "anonymous:b"));
        Udp ub, other;
        REQUIRE(ub.open(h.port()));
        REQUIRE(h.bind(ub, b));
        if (!other.open(h.port(), "127.0.0.2")) {
            std::printf("    127.0.0.2 not available here: skipped\n");
            return;
        }
        Bytes p = payloadOf(20, 6), got;
        other.send(data(a.id(), a.key(), 0, p));
        if (strict) {
            CHECK(h.silent(ub));
            other.send(hello(a.id(), a.key(), 1, 0));
            CHECK(h.silent(other));
        } else {
            CHECK(h.recv(ub, got) && isData(got, a.id(), p));
            other.send(data(a.id(), a.key() + 1, 0, p));
            CHECK(h.silent(ub));
        }
    }
}

TEST(nat_rebinding) {                               // 12, 13
    Harness h(LiveMode::On);
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub_old, ub_new, stranger;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub_old.open(h.port()));
    REQUIRE(ub_new.open(h.port()));
    REQUIRE(stranger.open(h.port()));
    REQUIRE(h.bind(ua, a));
    std::uint64_t old_cookie = h.helloCookie(ub_old, b);
    REQUIRE(h.helloCookie(ub_old, b, old_cookie) == old_cookie);
    // 13: a hello without a cookie from another socket on the same IP
    CHECK(h.helloCookie(stranger, b) != 0);
    // 12: the old port's cookie from the new port: answered, doesn't move b
    std::uint64_t new_cookie = h.helloCookie(ub_new, b, old_cookie);
    CHECK(new_cookie != 0 && new_cookie != old_cookie);
    Bytes p = payloadOf(20, 7), got;
    ua.send(data(a.id(), a.key(), b.id(), p));
    CHECK(h.recv(ub_old, got) && isData(got, a.id(), p));
    CHECK(h.silent(ub_new, 100) && h.silent(stranger, 50));
    CHECK_EQ(h.helloCookie(ub_new, b, new_cookie), new_cookie);
    ua.send(data(a.id(), a.key(), b.id(), p));
    CHECK(h.recv(ub_new, got) && isData(got, a.id(), p));
    CHECK(h.silent(ub_old, 100));
}

TEST(forged_hellos) {                               // 14
    Harness h(LiveMode::On, [](ServerConfig& c) { c.live.strict_address = false; });
    LiveClient a, b, victimClient;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    REQUIRE(h.login(victimClient, "anonymous:v"));
    Udp ua, ub, victim;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(victim.open(h.port()));
    REQUIRE(h.bind(ub, b));
    // the attacker knows a's id and key; it can only make the victim's
    // socket receive replies (the test sends from the victim's socket)
    std::uint64_t c = h.helloCookie(ua, a);
    REQUIRE(h.helloCookie(ua, a, c) == c);
    victim.send(hello(a.id(), a.key(), 5, c));   // a's current cookie, from the victim's address
    Bytes got;
    CHECK(h.recv(victim, got) && got.size() == live::kHelloAckSize);
    Bytes p = payloadOf(20, 8);
    ub.send(data(b.id(), b.key(), a.id(), p));
    CHECK(h.recv(ua, got) && isData(got, b.id(), p));
    CHECK(h.silent(victim, 100));
    // a player that never sent a hello is never bound by forgeries
    for (int i = 0; i < 3; ++i) victim.send(hello(victimClient.id(), victimClient.key(), 6, 0x1234 + i));
    drain(h, victim);
    h.pump(150);
    CHECK(!a.listsBound(victimClient.id()));
}

TEST(departed_player) {                             // 16
    Harness h(LiveMode::On);
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    std::uint32_t bid = b.id();
    std::uint64_t bkey = b.key();
    h.drop(b);
    Bytes p = payloadOf(20, 9);
    ua.send(data(a.id(), a.key(), 0, p));
    ua.send(data(a.id(), a.key(), bid, p));
    CHECK(h.silent(ub));
    ub.send(data(bid, bkey, 0, p));
    ub.send(hello(bid, bkey, 1, 0));
    CHECK(h.silent(ua, 100) && h.silent(ub, 50));
}

TEST(freshness) {                                   // 18
    Harness h(LiveMode::On, [](ServerConfig& c) { c.live.fresh_seconds = 1; });
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    CHECK(h.until([&] { return a.listsBound(b.id()); }));
    // b keeps sending data but no hello; a keeps its hellos up
    TimePoint end = Clock::now() + std::chrono::milliseconds(1600);
    TimePoint next_hello = Clock::now();
    while (Clock::now() < end) {
        ub.send(data(b.id(), b.key(), 0, payloadOf(20, 1)));
        if (Clock::now() >= next_hello) {
            h.bind(ua, a);
            next_hello = Clock::now() + std::chrono::milliseconds(400);
        }
        h.pump(20);
    }
    CHECK(!a.listsBound(b.id()));
    drain(h, ub, 50);
    ua.send(data(a.id(), a.key(), 0, payloadOf(20, 2)));
    CHECK(h.silent(ub));
    REQUIRE(h.bind(ub, b));
    CHECK(h.until([&] { return a.listsBound(b.id()); }));
    Bytes p = payloadOf(20, 3), got;
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
}

TEST(rate_limit) {                                  // 19
    setEnv("O11_LIVE_STATS_SECONDS", "1");
    Harness h(LiveMode::On);
    setEnv("O11_LIVE_STATS_SECONDS", "");
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ub, b));
    h.pump(300);                                // the buckets fill completely at a's first datagram
    auto burst = [&](int count, std::size_t size, double& t) {
        Bytes d = data(a.id(), a.key(), b.id(), payloadOf(size - live::kClientHeader, 1));
        TimePoint t0 = Clock::now();
        for (int i = 0; i < count; ++i) ua.send(d);
        int got = 0;
        Bytes r;
        TimePoint last = t0;
        while (h.recv(ub, r, 150)) {
            ++got;
            last = Clock::now();
        }
        t = std::chrono::duration<double>(last - t0).count();
        return got;
    };
    // The buckets at the default frame and bitrate (128 kbps): datagrams
    // 2 x fps + 50 a second, bytes 2 x (16000 + 64 x fps) + 8000 a second,
    // a quarter second of each. At 480 samples (fps 100): 250 datagrams and
    // 52800 bytes a second, buckets of 62.5 and 13200; at 240 they were
    // 450 and 65600, 112.5 and 16400.
    const double fps = 48000.0 / live::kDefaultFrame;
    const double dgram_rate = 2.0 * fps + 50.0, byte_rate = 2.0 * (128000.0 / 8.0 + fps * 64.0) + 8000.0;
    const double cap1 = std::min(dgram_rate, byte_rate / 120.0) * 0.25, rate1 = std::min(dgram_rate, byte_rate / 120.0);
    const double cap2 = std::min(dgram_rate, byte_rate / 1472.0) * 0.25, rate2 = std::min(dgram_rate, byte_rate / 1472.0);
    double t = 0;
    int n = burst(1000, 120, t);
    std::printf("    1000 x 120 bytes: %d arrived in %.3f s (a bucket of %.1f, %.1f a second)\n", n, t, cap1, rate1);
    CHECK(n >= cap1 - 2.5);
    CHECK(n <= cap1 + rate1 * t + 2);
    h.pump(1200);
    n = burst(200, 1472, t);
    std::printf("    200 x 1472 bytes: %d arrived in %.3f s (a bucket of %.1f, %.1f a second)\n", n, t, cap2, rate2);
    CHECK(n >= std::floor(cap2) - 1);
    CHECK(n <= cap2 + rate2 * t + 2);
    h.pump(300);
    Bytes p = payloadOf(10, 1), got;
    ua.send(data(a.id(), a.key(), b.id(), p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
    h.pump(1100);
    CHECK(h.logCount("over the rate limit") >= 1);
    CHECK(h.logCount("[live]") < 10);
    CHECK(h.logCount("[live] in the last 1 s: ") >= 1);
}

TEST(steady_small_stream) {                         // 20 (2 s instead of 5)
    Harness h(LiveMode::On, [](ServerConfig& c) {
        c.live.frame = 120;
        c.live.kbps = 32;
        c.live.stats_seconds = 1;
    });
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ub, b));
    Bytes d = data(a.id(), a.key(), b.id(), payloadOf(53 - live::kClientHeader, 1));
    int sent = 0, got = 0;
    TimePoint t0 = Clock::now();
    Bytes r;
    while (Clock::now() - t0 < std::chrono::seconds(2)) {
        double el = std::chrono::duration<double>(Clock::now() - t0).count();
        while (sent < static_cast<int>(el * 400)) {
            ua.send(d);
            ++sent;
        }
        h.pumpOnce();
        while (ub.tryRecv(r)) ++got;
    }
    while (h.recv(ub, r, 100)) ++got;
    std::printf("    %d of %d arrived\n", got, sent);
    CHECK_EQ(got, sent);
    CHECK_EQ(h.logCount("over the rate limit"), 0);
}

TEST(room_off) {                                    // 21
    Harness h(LiveMode::Yes);
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));                    // hellos are answered while off
    REQUIRE(h.bind(ub, b));
    Bytes p = payloadOf(20, 1), got;
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.silent(ub));
    a.c.say("!live on");
    CHECK(h.until([&] { return a.last().roomOn() && b.last().roomOn(); }));
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
    a.c.say("!live off");
    CHECK(h.until([&] { return !a.last().roomOn() && !b.last().roomOn(); }));
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.silent(ub));
    CHECK(h.helloCookie(ua, a) != 0);
}

TEST(forwarding_latency) {                          // 22
    // A datagram every 2 ms, within the rate limit of a frame of 240
    // samples (450 datagrams a second and a bucket of 112.5): 500 a
    // second for 2 s stays inside it. The default frame's (480: 250 a
    // second) would drop half.
    Harness h(LiveMode::On, [](ServerConfig& c) { c.live.frame = 240; });
    LiveClient a, b;
    REQUIRE(h.login(a, "anonymous:a"));
    REQUIRE(h.login(b, "anonymous:b"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port()));
    REQUIRE(ub.open(h.port()));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    // The server runs on its own thread now, as it would in production; the
    // clients' TCP is left alone meanwhile. One datagram every 2 ms for 1000
    // of them, from a thread whose timed waits the system doesn't stretch
    // (o11proto/realtime.h: macOS's timer coalescing can make a 2 ms wait 20
    // on a loaded machine), and for 4 s at the most, well inside the
    // binding's 20 s without a hello.
    std::atomic<bool> stop{false};
    std::thread server([&] {
        thread::makeLatencyCritical();
        while (!stop.load()) h.server.poll(5);
    });
    std::vector<double> lat;
    int sent = 0;
    double took = 0;
    bool audio_thread = false;
    std::thread client([&] {
        audio_thread = thread::makeAudioThread(0.002);
        Bytes d = data(a.id(), a.key(), b.id(), payloadOf(100, 1)), r;
        const TimePoint start = Clock::now();
        for (int i = 0; i < 1000 && Clock::now() - start < std::chrono::seconds(4); ++i) {
            TimePoint t0 = Clock::now();
            ua.send(d);
            ++sent;
            pollfd pfd{};
            pfd.fd = ub.fd;
            pfd.events = POLLIN;
            while (Clock::now() - t0 < std::chrono::milliseconds(50)) {
                compat::poll(&pfd, 1, 5);
                if (ub.tryRecv(r)) {
                    lat.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
                    break;
                }
            }
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(2));
        }
        took = std::chrono::duration<double>(Clock::now() - start).count();
    });
    client.join();
    stop.store(true);
    server.join();
    std::printf("    %zu of %d arrived within 50 ms, in %.2f s (the sending thread %s an audio thread)\n", lat.size(),
                sent, took, audio_thread ? "was" : "wasn't");
    if (sent < 1000 || lat.size() * 100 < static_cast<std::size_t>(sent) * 99)
        std::printf("    timing: %s\n", thread::describe().c_str());
    CHECK(sent >= 250);                          // enough for a 99th percentile
    REQUIRE(lat.size() * 100 >= static_cast<std::size_t>(sent) * 99);
    std::sort(lat.begin(), lat.end());
    double p99 = lat[lat.size() * 99 / 100];
#ifdef __linux__
    const double bound = 2.0;                   // spec test: p99 under 2 ms on loopback
#else
    const double bound = 20.0;                  // shared CI machines on other systems schedule coarsely
#endif
    std::printf("    p99 %.3f ms over %zu (bound %.0f ms)\n", p99, lat.size(), bound);
    CHECK(p99 < bound);
}

TEST(several_addresses) {                           // 24
#ifdef _WIN32
    std::printf("    Windows answers from the routing table's address: skipped\n");
#else
    Harness h(LiveMode::On);
    LiveClient a, b;
    if (!h.login(a, "anonymous:a", true, "", "127.0.0.2") || a.c.serverAddress() != "127.0.0.2") {
        std::printf("    127.0.0.2 not available here: skipped\n");
        return;
    }
    REQUIRE(h.login(b, "anonymous:b", true, "", "127.0.0.2"));
    Udp ua, ub;
    REQUIRE(ua.open(h.port(), "0.0.0.0", true, "127.0.0.2"));
    REQUIRE(ub.open(h.port(), "0.0.0.0", true, "127.0.0.2"));
    REQUIRE(h.bind(ua, a));
    REQUIRE(h.bind(ub, b));
    Bytes p = payloadOf(30, 1), got;
    ua.send(data(a.id(), a.key(), 0, p));
    CHECK(h.recv(ub, got) && isData(got, a.id(), p));
#endif
}

TEST(udp_port_taken) {                              // 25
    Udp squatter;
    REQUIRE(squatter.open(0, "0.0.0.0"));
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    getsockname(squatter.fd, reinterpret_cast<sockaddr*>(&sa), &len);
    std::uint16_t port = ntohs(sa.sin_port);
    {
        std::vector<std::string> log;
        Server s;
        ServerConfig cfg;
        cfg.port = port;
        cfg.allow_anonymous = true;
        cfg.live.mode = LiveMode::Yes;
        cfg.log = [&log](const std::string& l) { log.push_back(l); };
        if (!s.start(cfg)) {
            std::printf("    TCP port %u is taken too: skipped\n", port);
            return;
        }
        bool warned = false;
        for (const auto& l : log)
            if (l == "warning: Live is off, could not open UDP port " + std::to_string(port) +
                         ". Another program may already be using it (2049 is also the NFS port). Set a different "
                         "Port in the config, or Live no.")
                warned = true;
        CHECK(warned);
        CHECK(!s.liveUdpOpen());
        LiveClient a;
        a.c.on_live = [&a](const live::ServerLive& m) { a.msgs.push_back(m); };
        a.c.on_chat = [&a](const ChatMessage& m) { a.chats.push_back(m); };
        Client::Options o;
        o.username = "anonymous:a";
        o.live = true;
        ChannelInfoRecord ch;
        a.c.setChannels({ch});
        REQUIRE(a.c.connect("127.0.0.1", port, o));
        auto until = [&](const std::function<bool()>& f) {
            TimePoint d = Clock::now() + std::chrono::seconds(2);
            while (!f() && Clock::now() < d) {
                s.poll(1);
                a.c.poll(0);
            }
            return f();
        };
        CHECK(until([&] { return !a.msgs.empty(); }));
        if (!a.msgs.empty()) CHECK_EQ(a.last().flags, live::kFlagMaySend);
        a.c.say("!live on");
        CHECK(until([&] { return a.chatHas("[server] Live is off: the server could not open its UDP port"); }));
        a.c.disconnect();
        s.stop();
    }
    {
        std::vector<std::string> log;
        Server s;
        ServerConfig cfg;
        cfg.port = port;
        cfg.live.mode = LiveMode::Require;
        cfg.log = [&log](const std::string& l) { log.push_back(l); };
        CHECK(!s.start(cfg));
        CHECK(!s.running());
        bool err = false;
        for (const auto& l : log)
            if (l.rfind("Error listening on UDP port " + std::to_string(port) + " for Live", 0) == 0) err = true;
        CHECK(err);
    }
}

TEST(traffic_line) {                                // 46
    Harness h(LiveMode::On, [](ServerConfig& c) { c.live.stats_seconds = 1; });
    LiveClient a;
    REQUIRE(h.login(a));
    Udp u;
    REQUIRE(u.open(h.port()));
    h.pump(1100);                                  // a period without datagrams: no line
    CHECK_EQ(h.logCount("[live]"), 0);
    REQUIRE(h.bind(u, a));
    for (int i = 0; i < 20; ++i) u.send(data(a.id(), a.key(), 0, payloadOf(10, 1)));
    CHECK(h.until([&] { return h.logCount("[live]") > 0; }, 2500));
    h.pump(1200);                                  // a bind that straddled two periods gives two lines
    std::vector<std::string> lines;
    for (const auto& l : h.log)
        if (l.rfind("[live]", 0) == 0) lines.push_back(l);
    REQUIRE(!lines.empty());
    for (const auto& l : lines) std::printf("    %s\n", l.c_str());
    const std::string tail = ", dropped 1: 1 unverified address (answered)";
    auto endsWithTail = [&](const std::string& l) {
        return l.size() > tail.size() && l.compare(l.size() - tail.size(), tail.size(), tail) == 0;
    };
    CHECK(lines.back().rfind("[live] in the last 1 s: 1 player, ", 0) == 0);
    CHECK(lines.back().find(" datagrams/s in, ") != std::string::npos);
    if (lines.size() == 1) CHECK(endsWithTail(lines[0]));
    else CHECK(endsWithTail(lines[0]) || endsWithTail(lines[1]));
}

TEST(load_eight_players) {                          // 8 senders each to everyone, for 1 s
    Harness h(LiveMode::On);
    const int kPlayers = 8;
    std::vector<std::unique_ptr<LiveClient>> lc;
    std::vector<std::unique_ptr<Udp>> us;
    for (int i = 0; i < kPlayers; ++i) {
        lc.emplace_back(new LiveClient);
        us.emplace_back(new Udp);
        REQUIRE(h.login(*lc.back(), "anonymous:p" + std::to_string(i)));
        REQUIRE(us.back()->open(h.port()));
    }
    for (int i = 0; i < kPlayers; ++i) REQUIRE(h.bind(*us[i], *lc[i]));
    // 200 datagrams/s each (frame 240) of 125 bytes, to destination 0
    std::vector<int> got(kPlayers, 0);
    int sent = 0;
    TimePoint t0 = Clock::now();
    Bytes r;
    while (Clock::now() - t0 < std::chrono::seconds(1)) {
        double el = std::chrono::duration<double>(Clock::now() - t0).count();
        while (sent < static_cast<int>(el * 200)) {
            for (int i = 0; i < kPlayers; ++i) us[i]->send(data(lc[i]->id(), lc[i]->key(), 0, payloadOf(105, 1)));
            ++sent;
        }
        h.pumpOnce();
        for (int i = 0; i < kPlayers; ++i)
            while (us[i]->tryRecv(r)) ++got[i];
    }
    h.pump(100);
    for (int i = 0; i < kPlayers; ++i)
        while (us[i]->tryRecv(r)) ++got[i];
    for (int i = 0; i < kPlayers; ++i) CHECK_EQ(got[i], sent * (kPlayers - 1));
    std::printf("    %d players, %d datagrams each way, %d received by player 0\n", kPlayers, sent, got[0]);
}

int main() {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
