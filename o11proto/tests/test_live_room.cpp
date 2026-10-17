// Live mode for the whole room (o11proto/live_room.h): the health report
// (0x85) and the room message (0x08) on the wire, the server's rule with a
// simulated clock, and the rule inside a real server, with Live players
// whose transports bind and whose reports are written by hand.
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "o11proto/client.h"
#include "o11proto/live.h"
#include "o11proto/live_room.h"
#include "o11proto/live_transport.h"
#include "o11proto/server.h"

using namespace o11proto;
using Bytes = std::vector<std::uint8_t>;
using Rule = live::RoomRule;
using live::RoomReason;

namespace {

Rule::Player player(std::uint32_t id, bool clean = true, std::uint32_t fault = 0, bool sending = true) {
    Rule::Player p;
    p.id = id;
    p.live_client = true;
    p.sender = true;
    p.bound = p.was_bound = true;
    p.has_report = true;
    p.report.flags = static_cast<std::uint8_t>((clean ? live::kReportClean : 0) | (sending ? live::kReportSending : 0));
    p.report.fault = clean ? 0 : fault;
    p.was_clean = true;
    p.joined = -100;                           // long ago
    return p;
}

// Runs the rule from `from` to `to` seconds in 0.1 s steps; the first time
// it plays Live, or -1.
double liveAt(Rule& r, double from, double to, const std::vector<Rule::Player>& ps) {
    for (int i = 0; from + i * 0.1 <= to + 1e-9; ++i)
        if (r.update(from + i * 0.1, true, ps).live) return from + i * 0.1;
    return -1;
}

}  // namespace

// ------------------------------------------------------------- the wire

TEST(report_message) {
    live::LiveReport r;
    r.flags = live::kReportSending;
    r.fault = 0x01020304;
    const Bytes b = live::encode(r);
    CHECK(b == Bytes({1, 0x02, 0, 0, 4, 3, 2, 1}));
    live::LiveReport d;
    CHECK(live::decode(b, d) && d == r && !d.clean() && d.sending() && !d.cantLive());
    Bytes later = b;
    later[0] = 9;                              // a later version, with more bytes: read as 1
    later.push_back(0xff);
    CHECK(live::decode(later, d) && d.fault == 0x01020304u);
    CHECK(!live::decode(Bytes(b.begin(), b.begin() + 7), d));   // short
    Bytes zero = b;
    zero[0] = 0;
    CHECK(!live::decode(zero, d));
}

TEST(room_message) {
    live::LiveRoom m;
    m.reason = RoomReason::CantLive;
    m.user = "carol";
    const Bytes b = live::encode(m);
    CHECK(b == Bytes({1, 0, 2, 0, 'c', 'a', 'r', 'o', 'l', 0}));
    live::LiveRoom d;
    CHECK(live::decode(b, d) && !d.live && d.reason == RoomReason::CantLive && d.user == "carol");
    live::LiveRoom on;
    on.live = true;
    CHECK(live::encode(on) == Bytes({1, 1, 0, 0, 0}));
    CHECK(live::decode(Bytes({2, 1, 0, 0}), d) && d.live && d.user.empty());   // no name at all
    CHECK(live::decode(Bytes({1, 7, 9, 0, 'x'}), d) && !d.live && d.reason == RoomReason::None && d.user == "x");
    CHECK(!live::decode(Bytes({1, 1, 0}), d));
    CHECK(!live::decode(Bytes({0, 1, 0, 0, 0}), d));
}

// ------------------------------------------------------------- the rule

TEST(rule_settles_five_seconds) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2), player(3)};
    CHECK(!r.update(0, false, ps).live);       // the room's Live is off
    CHECK(r.state().reason == RoomReason::None);
    const double at = liveAt(r, 0, 10, ps);
    std::printf("    Live after %.1f s of clean reports\n", at);
    CHECK(at >= 4.95 && at <= 5.05);
    CHECK(!r.update(10.1, false, ps).live);    // off again: intervals at once
}

TEST(rule_bad_reports) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2), player(3), player(4)};
    REQUIRE(liveAt(r, 0, 6, ps) > 0);
    // 2 and 3 blame 4, 4 blames itself: the one named most.
    ps[1] = player(2, false, 4);
    ps[2] = player(3, false, 4);
    ps[3] = player(4, false, 4);
    Rule::State st = r.update(6.1, true, ps);
    CHECK(!st.live && st.reason == RoomReason::Dropouts && st.user == 4u);
    // A tie: the earliest in the room.
    ps = {player(1), player(2, false, 3), player(3, false, 2), player(4)};
    st = r.update(6.2, true, ps);
    CHECK(st.user == 2u);
    // Blaming a player who doesn't send is ignored; a reporter's own fault isn't.
    ps = {player(1), player(2, false, 3), player(3, true, 0, false)};
    st = r.update(6.3, true, ps);
    CHECK(st.reason == RoomReason::Dropouts);  // still the old reason: the conditions hold, it settles
    const double back = liveAt(r, 6.3, 20, ps);
    std::printf("    back to Live %.1f s after the reports were clean\n", back - 6.3);
    CHECK(back >= 6.3 + 4.95 && back <= 6.3 + 5.05);
    ps = {player(1), player(2, false, 2, false)};
    st = r.update(back + 0.1, true, ps);
    CHECK(!st.live && st.user == 2u);
}

TEST(rule_repeats_settle_longer) {
    Rule r;
    std::vector<Rule::Player> ok = {player(1), player(2)};
    std::vector<Rule::Player> bad = {player(1, false, 2), player(2)};
    REQUIRE(liveAt(r, 0, 6, ok) > 0);
    r.update(10, true, bad);                   // the first fall back: 5 s
    double back = liveAt(r, 10.1, 30, ok);
    CHECK(back > 0 && back - 10.1 < 5.1);
    r.update(20, true, bad);                   // a second within 60 s: 15 s
    back = liveAt(r, 20.1, 50, ok);
    std::printf("    a second fall back within 60 s: back after %.1f s\n", back - 20.1);
    CHECK(back - 20.1 >= 14.95 && back - 20.1 <= 15.05);
    r.update(100, true, bad);                  // 80 s later: 5 s again
    back = liveAt(r, 100.1, 120, ok);
    CHECK(back - 100.1 >= 4.95 && back - 100.1 <= 5.05);
}

TEST(rule_cant_and_joining) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2)};
    REQUIRE(liveAt(r, 0, 6, ps) > 0);
    Rule::Player old;                          // an older client: no Live
    old.id = 3;
    ps.push_back(old);
    Rule::State st = r.update(7, true, ps);
    CHECK(!st.live && st.reason == RoomReason::CantLive && st.user == 3u);
    // It leaves: while the room waits, it names no one.
    std::vector<Rule::Player> without = {player(1), player(2)};
    st = r.update(7.5, true, without);
    CHECK(!st.live && st.reason == RoomReason::None && st.user == 0u);
    ps[2].listener = true;                     // unless it can't send at all
    CHECK(liveAt(r, 7.6, 13, ps) > 0);
    // Over the sender limit; a report that says its Live can't.
    ps = {player(1), player(2)};
    ps[1].sender = false;
    CHECK(r.update(20, true, ps).reason == RoomReason::CantLive);
    ps[1] = player(2, false, 2);
    ps[1].report.flags |= live::kReportCantLive;
    CHECK(r.update(20.1, true, ps).reason == RoomReason::CantLive);
    // Someone joining: not bound, then bound without a report, then can't after 10 s.
    Rule::Player j;
    j.id = 5;
    j.live_client = j.sender = true;
    j.joined = 30;
    ps = {player(1), player(2), j};
    st = r.update(31, true, ps);
    CHECK(st.reason == RoomReason::Joining && st.user == 5u);
    ps[2].bound = ps[2].was_bound = true;
    CHECK(r.update(35, true, ps).reason == RoomReason::Joining);
    CHECK(r.update(40.1, true, ps).reason == RoomReason::CantLive);   // no report in 10 s
    ps[2].has_report = ps[2].was_clean = true;
    ps[2].report.flags = live::kReportClean | live::kReportSending;
    CHECK(liveAt(r, 41, 47, ps) > 0);
    // A binding lost later is a connection problem.
    ps[2].bound = false;
    st = r.update(50, true, ps);
    CHECK(st.reason == RoomReason::Dropouts && st.user == 5u);
}

// ----------------------------------------------- the rule in the server

namespace {

struct Room {
    Server server;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::vector<Client*> clients;
    Room() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote;
        cfg.live.mode = LiveMode::On;
        CHECK(server.start(cfg));
        thread = std::thread([this] {
            while (!stop.load()) server.poll(1);
        });
    }
    ~Room() {
        stop.store(true);
        thread.join();
    }
    void pump() {
        for (Client* c : clients) c->poll(0);
    }
    bool until(const std::function<bool()>& pred, int timeout_ms) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (Clock::now() >= deadline) return false;
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

struct Player {
    Client c;
    live::Transport t;
    std::vector<live::LiveRoom> rooms;         // every room message
    bool login(Room& r, const std::string& name, bool live = true) {
        r.clients.push_back(&c);
        c.on_message = [this](const RawMessage& m) {
            live::LiveRoom lr;
            if (m.type == live::kMsgServerLiveRoom && live::decode(m.payload, lr)) rooms.push_back(lr);
        };
        Client::Options o;
        o.username = "anonymous:" + name;
        o.codec_caps = kCapOpus;
        o.live = live;
        ChannelInfoRecord ch;
        ch.name = "ch0";
        c.setChannels({ch});
        if (!c.connect("127.0.0.1", r.server.port(), o)) return false;
        return r.until([&] { return c.loggedIn() && (!live || c.hasLiveMessage()); }, 2000);
    }
    bool bind() {
        const live::ServerLive& m = c.liveMessage();
        live::Transport::Options o;
        o.server_address = c.serverAddress();
        o.port = m.udp_port ? m.udp_port : c.serverPort();
        o.peer_id = m.peer_id;
        o.key = m.key;
        std::string err;
        return t.start(o, err);
    }
    void report(bool clean, std::uint32_t fault = 0, bool sending = true) {
        live::LiveReport r;
        r.flags = static_cast<std::uint8_t>((clean ? live::kReportClean : 0) | (sending ? live::kReportSending : 0));
        r.fault = fault;
        c.sendRaw(live::kMsgClientLiveReport, live::encode(r));
    }
    std::uint32_t id() const { return c.liveMessage().peer_id; }
    bool last(bool live, RoomReason reason, const std::string& user = "") const {
        return !rooms.empty() && rooms.back().live == live && rooms.back().reason == reason && rooms.back().user == user;
    }
    bool sees(std::uint32_t peer, std::uint8_t flag) const {
        for (const auto& p : c.liveMessage().peers)
            if (p.id == peer) return (p.flags & flag) != 0;
        return false;
    }
};

}  // namespace

TEST(server_room_messages) {
    Room r;
    Player a, b, old;
    REQUIRE(a.login(r, "a"));
    REQUIRE(b.login(r, "b"));
    // Joining: nobody has bound or reported yet.
    REQUIRE(r.until([&] { return !a.rooms.empty() && !b.rooms.empty(); }, 2000));
    CHECK(!a.rooms.back().live && a.rooms.back().reason == RoomReason::Joining);
    REQUIRE(a.bind() && b.bind());
    a.report(true);
    b.report(true);
    // Their reports' sending flags reach the others' 0x07.
    CHECK(r.until([&] { return a.sees(b.id(), live::kPeerSending) && b.sees(a.id(), live::kPeerSending); }, 2000));
    const TimePoint t0 = Clock::now();
    REQUIRE(r.until([&] { return a.last(true, RoomReason::None) && b.last(true, RoomReason::None); }, 8000));
    const double took = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("    Live %.2f s after both reported clean\n", took);
    CHECK(took >= 4.9 && took <= 5.6);
    // a can't hear b cleanly: the room is on intervals at once, naming b.
    const TimePoint t1 = Clock::now();
    a.report(false, b.id());
    REQUIRE(r.until([&] { return a.last(false, RoomReason::Dropouts, b.c.username()) && b.last(false, RoomReason::Dropouts, b.c.username()); },
                    2000));
    std::printf("    on intervals %.0f ms after the report\n",
                std::chrono::duration<double>(Clock::now() - t1).count() * 1000);
    // b stops sending: a's blame is nobody's fault, so the room settles again.
    b.report(true, 0, false);
    CHECK(r.until([&] { return a.c.liveMessage().peers.size() == 1 && !a.sees(b.id(), live::kPeerSending); }, 2000));
    const std::size_t before = a.rooms.size();
    // An older client joins: it can't do Live.
    REQUIRE(old.login(r, "old", false));
    CHECK(r.until([&] { return a.last(false, RoomReason::CantLive, old.c.username()); }, 2000));
    CHECK(a.rooms.size() > before);
    CHECK(old.rooms.empty());                  // it gets none
    // It leaves: after 5 s the room is Live, though a still blames b, who doesn't send.
    old.c.disconnect();
    const TimePoint t2 = Clock::now();
    REQUIRE(r.until([&] { return a.last(true, RoomReason::None); }, 8000));
    std::printf("    Live again %.2f s after the older client left\n",
                std::chrono::duration<double>(Clock::now() - t2).count());
    a.t.stop();
    b.t.stop();
}

TEST_MAIN()
