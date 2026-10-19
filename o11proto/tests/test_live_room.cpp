// Live mode for the whole room (o11proto/live_room.h): the health report
// (0x85) and the room message (0x08) on the wire, the server's advice with
// a simulated clock, and the advice inside a real server, with Live players
// whose transports bind and whose reports are written by hand. The room's
// mode follows its switch alone.
#include <atomic>
#include <functional>
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
using Rule = live::RoomAdvice;
using live::Advice;

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

// Runs the advice from `from` to `to` seconds in 0.1 s steps; the first
// time it is `want` (by reason and user), or -1.
double adviceAt(Rule& r, double from, double to, const std::vector<Rule::Player>& ps, Advice want,
                std::uint32_t user = 0) {
    for (int i = 0; from + i * 0.1 <= to + 1e-9; ++i) {
        const Rule::State& st = r.update(from + i * 0.1, true, ps);
        if (st.advice == want && st.user == user) return from + i * 0.1;
    }
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
    m.live = true;
    m.advice = Advice::CantLive;
    m.user = "carol";
    m.count = 2;
    const Bytes b = live::encode(m);
    CHECK(b == Bytes({3, 1, 2, 0, 'c', 'a', 'r', 'o', 'l', 0, 2, 0, 0, 0, 0, 0, 0, 0}));
    live::LiveRoom d;
    CHECK(live::decode(b, d) && d.live && d.advice == Advice::CantLive && d.user == "carol" && d.count == 2 &&
          d.route == live::RoomRoute::None);
    live::LiveRoom off;
    CHECK(live::encode(off) == Bytes({3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(live::decode(Bytes({2, 1, 0, 0}), d) && d.live && d.user.empty() && d.count == 0);   // no name at all
    CHECK(live::decode(Bytes({2, 1, 1, 0, 'x', 0}), d) && d.advice == Advice::Dropouts && d.count == 1);   // no count
    // Version 3's route: the room on the relay since a and b had no path,
    // and everyone can connect directly again.
    live::LiveRoom relay;
    relay.live = true;
    relay.route = live::RoomRoute::Relay;
    relay.relay_fallback = true;
    relay.direct_possible = true;
    relay.why = live::RelayWhy::NoPath;
    relay.why_a = "a";
    relay.why_b = "bc";
    const Bytes rb = live::encode(relay);
    CHECK(rb == Bytes({3, 1, 0, 0, 0, 0, 0, 2, 3, 1, 0, 'a', 0, 'b', 'c', 0}));
    CHECK(live::decode(rb, d) && d.route == live::RoomRoute::Relay && d.direct_possible && d.relay_fallback &&
          d.why == live::RelayWhy::NoPath && d.why_a == "a" && d.why_b == "bc");
    CHECK(live::decode(Bytes({3, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0}), d) && d.route == live::RoomRoute::Direct &&
          !d.relay_fallback && !d.direct_possible && d.why_a.empty() && d.why_b.empty());   // no names at all
    CHECK(live::decode(Bytes({3, 1, 0, 0, 0, 0, 0, 7, 0, 9, 0}), d) && d.route == live::RoomRoute::None &&
          d.why == live::RelayWhy::None);        // unknown route and reason
    CHECK(live::decode(Bytes({3, 1, 1, 0, 'x', 0, 5, 0, 2, 1}), d) && d.count == 5 &&
          d.route == live::RoomRoute::None);     // the route cut short: none
    CHECK(live::decode(Bytes({4, 1, 1, 0, 'x', 0, 5, 0, 2, 0, 3, 0, 'y', 0, 0, 9, 9}), d) && d.count == 5 &&
          d.user == "x" && d.route == live::RoomRoute::Relay && d.why == live::RelayWhy::RelayOnly &&
          d.why_a == "y" && d.why_b.empty());    // later: read as 3
    // Version 1: the reason said why the room was on intervals; 3 (joining) is none now.
    CHECK(live::decode(Bytes({1, 0, 2, 0, 'c', 'a', 'r', 'o', 'l', 0}), d) && !d.live &&
          d.advice == Advice::CantLive && d.user == "carol" && d.count == 1);
    CHECK(live::decode(Bytes({1, 7, 3, 0, 'x'}), d) && !d.live && d.advice == Advice::None && d.count == 0);
    CHECK(!live::decode(Bytes({2, 1, 0}), d));
    CHECK(!live::decode(Bytes({0, 1, 0, 0, 0}), d));
}

// ----------------------------------------------------------- the advice

TEST(advice_none_while_clean_or_off) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2), player(3)};
    for (int i = 0; i <= 100; ++i) CHECK(r.update(i * 0.1, true, ps).advice == Advice::None);
    ps[1] = player(2, false, 3);
    CHECK(r.update(10.1, false, ps) == Rule::State());   // the room isn't Live: no advice
    CHECK(r.update(10.2, true, ps).user == 3u);
    CHECK(r.update(10.3, false, ps).count == 0);          // and it is forgotten at once
}

TEST(advice_dropouts) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2), player(3), player(4)};
    r.update(0, true, ps);
    // 2 and 3 blame 4, 4 blames itself: at once, naming 4.
    ps[1] = player(2, false, 4);
    ps[2] = player(3, false, 4);
    ps[3] = player(4, false, 4);
    Rule::State st = r.update(0.1, true, ps);
    CHECK(st.advice == Advice::Dropouts && st.user == 4u && st.count == 1);
    // Clean again: it clears after 10 s clean, not before.
    ps = {player(1), player(2), player(3), player(4)};
    CHECK(r.update(0.2, true, ps).user == 4u);
    const double clear = adviceAt(r, 0.3, 20, ps, Advice::None);
    std::printf("    the advice clears %.1f s after the reports are clean\n", clear - 0.2);
    CHECK(clear >= 0.2 + 9.95 && clear <= 0.2 + 10.05);
    // Trouble again within the 10 s starts them again.
    ps[0] = player(1, false, 2);
    r.update(30, true, ps);
    ps[0] = player(1);
    r.update(30.1, true, ps);
    ps[0] = player(1, false, 2);
    r.update(35, true, ps);
    ps[0] = player(1);
    CHECK(adviceAt(r, 35.1, 50, ps, Advice::None) >= 45.05);
    // Blaming a player who doesn't send is ignored; a reporter's own fault isn't.
    ps = {player(1), player(2, false, 3), player(3, true, 0, false)};
    CHECK(r.update(60, true, ps).advice == Advice::None);
    ps = {player(1), player(2, false, 2, false)};
    st = r.update(60.1, true, ps);
    CHECK(st.advice == Advice::Dropouts && st.user == 2u);
    // A binding lost is trouble at once; and someone who leaves takes theirs along.
    ps = {player(1), player(3)};
    ps[1].bound = false;
    st = r.update(60.2, true, ps);
    CHECK(st.user == 3u && st.count == 1);               // 2 left
    ps = {player(1)};
    CHECK(r.update(60.3, true, ps) == Rule::State());
}

TEST(advice_cant_do_live) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2)};
    Rule::Player old;                          // an older client: no Live, can't from the start
    old.id = 3;
    old.joined = 0;
    ps.push_back(old);
    Rule::State st = r.update(0.1, true, ps);
    CHECK(st.advice == Advice::CantLive && st.user == 3u && st.count == 1);
    ps[2].listener = true;                     // unless it can't send at all
    CHECK(r.update(0.2, true, ps).user == 3u);   // (the trouble it had still clears as usual)
    CHECK(adviceAt(r, 0.3, 15, ps, Advice::None) >= 10.15);
    // Over the sender limit; a report that says its Live can't.
    ps = {player(1), player(2)};
    ps[1].sender = false;
    CHECK(r.update(20, true, ps).advice == Advice::CantLive);
    ps[1] = player(2, false, 2);
    ps[1].report.flags |= live::kReportCantLive;
    CHECK(r.update(20.1, true, ps).advice == Advice::CantLive);
    ps = {player(1)};
    CHECK(r.update(20.2, true, ps) == Rule::State());
    // Someone joining: no advice while it binds and reports; can't after 10 s.
    Rule::Player j;
    j.id = 5;
    j.live_client = j.sender = true;
    j.joined = 30;
    ps = {player(1), player(2, false, 5), j};  // and 2's blame of it doesn't count yet
    CHECK(r.update(31, true, ps).advice == Advice::None);
    ps[2].bound = ps[2].was_bound = true;
    CHECK(r.update(35, true, ps).advice == Advice::None);
    st = r.update(40.1, true, ps);             // no report in 10 s
    CHECK(st.advice == Advice::CantLive && st.user == 5u);
    ps[2].has_report = ps[2].was_clean = true;
    ps[2].report.flags = live::kReportClean | live::kReportSending;
    ps[1] = player(2);
    CHECK(adviceAt(r, 40.2, 60, ps, Advice::None) >= 50.15);
    // A joining reporter's own blame doesn't count either.
    Rule::Player k = j;
    k.id = 6;
    k.joined = 70;
    k.bound = k.was_bound = k.has_report = true;
    k.report.flags = live::kReportSending;     // not clean: its slots are starting
    k.report.fault = 1;
    ps = {player(1), player(2), k};
    CHECK(r.update(71, true, ps).advice == Advice::None);
}

TEST(advice_names_the_worst_and_counts) {
    Rule r;
    std::vector<Rule::Player> ps = {player(1), player(2, false, 4), player(3, false, 4), player(4), player(5, false, 1)};
    Rule::State st = r.update(0, true, ps);
    CHECK(st.advice == Advice::Dropouts && st.user == 4u && st.count == 2);   // 4 twice, 1 once
    ps[0].bound = false;                       // 1's binding is lost too: the worst now
    st = r.update(0.1, true, ps);
    CHECK(st.user == 1u && st.count == 2);
    Rule::Player old;                          // an older client joins: as bad, but later in the room
    old.id = 6;
    ps.push_back(old);
    st = r.update(0.2, true, ps);
    CHECK(st.user == 1u && st.count == 3);
    ps[0].bound = true;                        // 1 is clean, 4 too: the one with trouble now
    ps[1] = player(2);
    ps[2] = player(3);
    ps[4] = player(5);
    st = r.update(0.3, true, ps);
    CHECK(st.advice == Advice::CantLive && st.user == 6u && st.count == 3);
    ps.pop_back();                             // it leaves: the first in the room still to clear
    st = r.update(0.4, true, ps);
    CHECK(st.advice == Advice::Dropouts && st.user == 1u && st.count == 2);
    CHECK(adviceAt(r, 0.5, 15, ps, Advice::None) >= 10.25);
}

// ----------------------------------------------- the rule in the server

namespace {

struct Room {
    Server server;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::vector<Client*> clients;
    explicit Room(bool relay_fallback = false) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.live.relay_fallback = relay_fallback;
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
    bool login(Room& r, const std::string& name, bool live = true, bool direct = false) {
        r.clients.push_back(&c);
        c.on_message = [this](const RawMessage& m) {
            live::LiveRoom lr;
            if (m.type == live::kMsgServerLiveRoom && live::decode(m.payload, lr)) rooms.push_back(lr);
        };
        Client::Options o;
        o.username = "anonymous:" + name;
        o.codec_caps = kCapOpus;
        o.live = live;
        o.live_direct = direct;
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
    // CLIENT_LIVE_PATHS: how its paths to these peers stand.
    void paths(const std::vector<std::pair<std::uint32_t, live::PathState>>& states, bool relay_only = false) {
        live::PathsReport m;
        m.flags = relay_only ? live::kPathsRelayOnly : 0;
        for (const auto& st : states) {
            live::PathsReport::Entry e;
            e.id = st.first;
            e.state = st.second;
            m.peers.push_back(e);
        }
        c.sendRaw(live::kMsgClientLivePaths, live::encode(m));
    }
    bool route(live::RoomRoute rt, bool possible = false, live::RelayWhy why = live::RelayWhy::None) const {
        return !rooms.empty() && rooms.back().route == rt && rooms.back().direct_possible == possible &&
               rooms.back().why == why;
    }
    bool last(bool live, Advice advice = Advice::None, const std::string& user = "", int count = 0) const {
        return !rooms.empty() && rooms.back().live == live && rooms.back().advice == advice &&
               rooms.back().user == user && rooms.back().count == count;
    }
    bool allSince(std::size_t from, bool live) const {   // every room message since `from`: this mode, no advice
        for (std::size_t i = from; i < rooms.size(); ++i)
            if (rooms[i].live != live || rooms[i].advice != Advice::None) return false;
        return true;
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
    auto secs = [](TimePoint t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    // The room is Live from the start (its switch is on); nobody has bound
    // or reported yet, and that is no one's trouble while they join.
    REQUIRE(r.until([&] { return !a.rooms.empty() && !b.rooms.empty(); }, 2000));
    CHECK(a.last(true) && b.last(true));
    REQUIRE(a.bind() && b.bind());
    a.report(true);
    b.report(true);
    // Their reports' sending flags reach the others' 0x07.
    CHECK(r.until([&] { return a.sees(b.id(), live::kPeerSending) && b.sees(a.id(), live::kPeerSending); }, 2000));
    r.until([] { return false; }, 300);
    CHECK(a.allSince(0, true) && b.allSince(0, true));
    // a can't hear b cleanly: advice at once, naming b; the room stays Live.
    const std::string bn = b.c.username();
    TimePoint t = Clock::now();
    a.report(false, b.id());
    REQUIRE(r.until([&] { return a.last(true, Advice::Dropouts, bn, 1) && b.last(true, Advice::Dropouts, bn, 1); },
                    2000));
    std::printf("    advice %.0f ms after the report\n", secs(t) * 1000);
    // Clean again: the advice clears 10 s later, the room Live all along.
    std::size_t from = a.rooms.size();
    t = Clock::now();
    a.report(true);
    REQUIRE(r.until([&] { return a.last(true) && b.last(true); }, 12000));
    const double cleared = secs(t);
    std::printf("    advice cleared %.2f s after the clean report\n", cleared);
    CHECK(cleared >= 9.9 && cleared <= 10.6);
    for (std::size_t i = from; i < a.rooms.size(); ++i) CHECK(a.rooms[i].live);
    // b stops sending: a's blame is nobody's fault, so there is no advice.
    b.report(true, 0, false);
    CHECK(r.until([&] { return !a.sees(b.id(), live::kPeerSending); }, 2000));
    from = a.rooms.size();
    a.report(false, b.id());
    r.until([] { return false; }, 500);
    CHECK(a.allSince(from, true));
    a.report(true);
    // An older client joins: it can't play Live, at once; it gets no room message.
    REQUIRE(old.login(r, "old", false));
    const std::string on = old.c.username();
    CHECK(r.until([&] { return a.last(true, Advice::CantLive, on, 1) && b.last(true, Advice::CantLive, on, 1); }, 2000));
    CHECK(old.rooms.empty());
    // It leaves: the advice clears at once.
    old.c.disconnect();
    t = Clock::now();
    REQUIRE(r.until([&] { return a.last(true) && b.last(true); }, 2000));
    std::printf("    advice cleared %.0f ms after the older client left\n", secs(t) * 1000);
    // !live off: the room is on intervals, and a's blame (b sends again)
    // gives no advice; !live on: Live, and the advice again.
    b.report(true);
    CHECK(r.until([&] { return a.sees(b.id(), live::kPeerSending); }, 2000));
    a.c.say("!live off");
    REQUIRE(r.until([&] { return a.last(false) && b.last(false); }, 2000));
    from = a.rooms.size();
    a.report(false, b.id());
    r.until([] { return false; }, 500);
    CHECK(a.allSince(from, false));
    b.c.say("!live on");
    REQUIRE(r.until([&] { return a.last(true, Advice::Dropouts, bn, 1) && b.last(true, Advice::Dropouts, bn, 1); },
                    2000));
    a.t.stop();
    b.t.stop();
}

// All-to-all, direct only (the default): a pair of bound senders without
// a direct path is "can't play Live" for both, the earlier named, and the
// room's route stays Direct.
TEST(server_room_direct_only) {
    using live::PathState;
    using live::RoomRoute;
    Room r;
    Player a, b, c;
    REQUIRE(a.login(r, "a", true, true));
    REQUIRE(b.login(r, "b", true, true));
    REQUIRE(c.login(r, "c", true, true));
    REQUIRE(a.bind() && b.bind() && c.bind());
    REQUIRE(r.until([&] { return a.route(RoomRoute::Direct) && c.route(RoomRoute::Direct); }, 2000));
    CHECK(!c.rooms.back().relay_fallback);
    a.paths({{b.id(), PathState::Opening}, {c.id(), PathState::Up}});
    r.until([] { return false; }, 200);
    CHECK(c.last(true));                           // opening is no one's trouble
    a.paths({{b.id(), PathState::Down}, {c.id(), PathState::Up}});
    const std::string an = a.c.username();
    REQUIRE(r.until([&] { return c.last(true, Advice::CantLive, an, 2) && b.last(true, Advice::CantLive, an, 2); },
                    2000));
    CHECK(c.route(RoomRoute::Direct));
    c.c.say("!live direct");                       // no such command here
    r.until([] { return false; }, 300);
    CHECK(c.route(RoomRoute::Direct) && c.last(true, Advice::CantLive, an, 2));
    a.t.stop();
    b.t.stop();
    c.t.stop();
}

// The backend option relay_fallback: Direct until a pair of bound senders
// has no direct path, then Relay for everyone, with the advice once every
// pair has one again, until `!live direct`.
TEST(server_room_relay_fallback) {
    using live::PathState;
    using live::RelayWhy;
    using live::RoomRoute;
    Room r(true);
    Player a, b, c, old;
    REQUIRE(a.login(r, "a", true, true));
    REQUIRE(b.login(r, "b", true, true));
    REQUIRE(c.login(r, "c", true, true));
    REQUIRE(a.bind() && b.bind() && c.bind());
    auto all = [&](RoomRoute rt, bool possible = false, RelayWhy why = RelayWhy::None) {
        return r.until([&] { return a.route(rt, possible, why) && b.route(rt, possible, why) && c.route(rt, possible, why); },
                       2000);
    };
    CHECK(all(RoomRoute::Direct));
    CHECK(c.rooms.back().relay_fallback);
    // Opening paths are no one's trouble.
    a.paths({{b.id(), PathState::Opening}, {c.id(), PathState::Up}});
    r.until([] { return false; }, 200);
    CHECK(all(RoomRoute::Direct));
    // a's path to b is unreachable: everyone through the relay, naming them.
    a.paths({{b.id(), PathState::Down}, {c.id(), PathState::Up}});
    REQUIRE(all(RoomRoute::Relay, false, RelayWhy::NoPath));
    const std::string an = a.c.username(), bn = b.c.username();
    CHECK(c.rooms.back().why_a == an && c.rooms.back().why_b == bn);
    // !live direct is refused while that is so.
    c.c.say("!live direct");
    r.until([] { return false; }, 300);
    CHECK(all(RoomRoute::Relay, false, RelayWhy::NoPath));
    // Up again, as both of each pair report it: the advice, and the room
    // stays on the relay.
    a.paths({{b.id(), PathState::Up}, {c.id(), PathState::Up}});
    b.paths({{a.id(), PathState::Up}, {c.id(), PathState::Up}});
    r.until([] { return false; }, 300);
    CHECK(all(RoomRoute::Relay, false, RelayWhy::NoPath));   // c hasn't said yet
    c.paths({{a.id(), PathState::Up}, {b.id(), PathState::Up}});
    REQUIRE(all(RoomRoute::Relay, true, RelayWhy::NoPath));
    r.until([] { return false; }, 300);
    CHECK(all(RoomRoute::Relay, true, RelayWhy::NoPath));
    b.c.say("!live direct");
    REQUIRE(all(RoomRoute::Direct));
    // A client that sends through the relay only.
    b.paths({{a.id(), PathState::Down}, {c.id(), PathState::Down}}, true);
    REQUIRE(all(RoomRoute::Relay, false, RelayWhy::RelayOnly));
    CHECK(a.rooms.back().why_a == bn && a.rooms.back().why_b.empty());
    b.paths({{a.id(), PathState::Up}, {c.id(), PathState::Up}});
    REQUIRE(all(RoomRoute::Relay, true, RelayWhy::RelayOnly));
    a.c.say("!live direct");
    REQUIRE(all(RoomRoute::Direct));
    // A sender without bit 17, once bound; the advice once it leaves.
    REQUIRE(old.login(r, "old"));
    CHECK(all(RoomRoute::Direct));                 // not bound yet: no one's trouble
    REQUIRE(old.bind());
    REQUIRE(all(RoomRoute::Relay, false, RelayWhy::NoDirect));
    CHECK(a.rooms.back().why_a == old.c.username());
    old.t.stop();
    old.c.disconnect();
    REQUIRE(all(RoomRoute::Relay, true, RelayWhy::NoDirect));
    a.t.stop();
    b.t.stop();
    c.t.stop();
}

TEST_MAIN()
