// Live all-to-all (PROTOCOL.md 15.10, ENGINE.md 16), end to end in real time
// through o11server: the room's Live goes directly between every pair of
// players, or, once a pair has no direct path, through the relay for
// everyone until a player chooses `!live direct`, without the players
// doing anything else. Each of three players plays a steady sine of its
// own; each listener's output is checked over whole spans, across the
// route changes too: every 10 ms within 1 dB of the span's RMS, no
// near-silence over 1 ms, and no step over 1.5 times the largest a steady
// mix of the two tones makes (the seamless rule of the room's switches,
// which crossfade over kLiveFadeMs).
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "live_harness.h"
#include "o11proto/live.h"

using namespace harness;

namespace {

// A stream's blocks a second at the room's default frame (100 at 480
// samples, 200 at 240), and within 10 % of that.
constexpr long kBlocks = 48000 / o11proto::live::kDefaultFrame;
constexpr long kBlocksLo = kBlocks * 9 / 10, kBlocksHi = kBlocks * 11 / 10;

const double kHz[3] = {500, 700, 1100};
float tone(long n, int rate, double hz) { return static_cast<float>(0.25 * std::sin(2 * M_PI * hz * n / rate)); }

void tones(Room& r) {
    for (int i = 0; i < r.opt.sides; ++i) {
        const double f = kHz[i % 3];
        r[i].signal = [f](long n, int rt) { return tone(n, rt, f); };
    }
}

// The user index of `prefix` (alice@...) on side `s`, -1 for none.
int userIndex(Side& s, const std::string& prefix) {
    for (int u = 0; u < s.e.numUsers(); ++u)
        if (s.e.userName(u).compare(0, prefix.size() + 1, prefix + "@") == 0) return u;
    return -1;
}

// How side `s` hears side `of`: the path its stream's blocks come by.
LiveRoute streamRoute(Room& r, int s, int of) {
    const int u = userIndex(r[s], r[of].name);
    LiveCounters c;
    return u >= 0 && r[s].e.liveUserCounters(u, &c) ? c.route : LiveRoute::None;
}

// Every stream comes by the path `want(listener, sender)` gives.
bool streamsAre(Room& r, const std::function<LiveRoute(int, int)>& want, double seconds) {
    const int n = r.opt.sides;
    return r.until(
        [&] {
            for (int a = 0; a < n; ++a)
                for (int b = 0; b < n; ++b)
                    if (a != b && streamRoute(r, a, b) != want(a, b)) return false;
            return true;
        },
        seconds);
}

// Every side sees the room's route as `route`.
bool roomRouteIs(Room& r, LiveRoute route, double seconds) {
    return r.until(
        [&] {
            for (int a = 0; a < r.opt.sides; ++a)
                if (r[a].e.liveRoomRoute().route != route) return false;
            return true;
        },
        seconds);
}

void printRoutes(Room& r) {
    for (int a = 0; a < r.opt.sides; ++a) {
        std::string line = "    " + r[a].name + ": the room " + r[a].e.liveRoomRouteText() + "; hears";
        for (int b = 0; b < r.opt.sides; ++b) {
            if (a == b) continue;
            const int u = userIndex(r[a], r[b].name);
            char rtt[48] = "";
            if (u >= 0 && r[a].e.liveUserProbeRttMs(u) >= 0)
                std::snprintf(rtt, sizeof(rtt), " (probes %.2f ms)", r[a].e.liveUserProbeRttMs(u));
            line += std::string(" ") + r[b].name + " by " + liveRouteName(streamRoute(r, a, b)) + rtt + ",";
        }
        line.back() = ' ';
        std::printf("%s\n", line.c_str());
    }
}

// The route events of side `s`'s stream of side `of`.
std::string routeEvents(Room& r, int s, int of) {
    const int u = userIndex(r[s], r[of].name);
    std::string out;
    if (u < 0) return out;
    for (const LiveEvent& ev : r[s].e.liveUserEvents(u))
        if (ev.kind == LiveEventKind::Route)
            out += std::string(out.empty() ? "" : ", ") + liveRouteName(static_cast<LiveRoute>(ev.value));
    return out;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// What a listener's output did over [from, to): see the top.
struct Evenness {
    int windows = 0, skipped = 0;
    double rms = 0;                              // the median 10 ms RMS
    double low_db = 0, high_db = 0;              // each 10 ms RMS against it, the extremes
    double silence_ms = 0;                       // the longest run below -60 dBFS
    double step_ratio = 0;                       // the largest step against a steady mix's largest
    long worst = -1;                             // the window furthest off
    bool ok() const { return step_ratio <= 1.5 && silence_ms <= 1.0 && low_db >= -1.0 && high_db <= 1.0; }
};

// `unheard`: a side the listener doesn't hear, -1 for none.
Evenness evenness(Room& r, int listener, long from, long to, int unheard = -1) {
    Evenness ev;
    const float* x = r[listener].heard.get();
    const long ms = r.opt.rate / 1000;
    std::vector<double> rms;
    std::vector<long> at;
    for (long w = from; w + 10 * ms <= to; w += 5 * ms) {
        if (!r.calm(w - 500 * ms, w + 100 * ms)) {
            ++ev.skipped;
            continue;
        }
        rms.push_back(steady(x, w, w + 10 * ms).rms);
        at.push_back(w);
    }
    ev.windows = static_cast<int>(rms.size());
    if (rms.empty()) return ev;
    std::vector<double> sorted = rms;
    std::sort(sorted.begin(), sorted.end());
    ev.rms = sorted[sorted.size() / 2];
    // The two others' tones, each at an amplitude of rms: a steady mix
    // steps at most this much from one sample to the next.
    double sines = 0;
    for (int i = 0; i < 3; ++i)
        if (i != listener % 3 && i != unheard % 3) sines += std::sin(M_PI * kHz[i] / r.opt.rate);
    const double steady_step = 2 * ev.rms * sines;
    double worst_off = 0;
    for (std::size_t k = 0; k < rms.size(); ++k) {
        const double d = dB(rms[k] / std::max(1e-12, ev.rms));
        ev.low_db = std::min(ev.low_db, d);
        ev.high_db = std::max(ev.high_db, d);
        if (std::fabs(d) > worst_off) {
            worst_off = std::fabs(d);
            ev.worst = at[k];
        }
        const long w = at[k];
        long run = 0;
        for (long i = w; i < w + 10 * ms; ++i) {
            ev.step_ratio = std::max(ev.step_ratio, std::fabs(x[i] - x[i - 1]) / std::max(1e-9, steady_step));
            run = std::fabs(x[i]) < 0.001f ? run + 1 : 0;
            ev.silence_ms = std::max(ev.silence_ms, run * 1000.0 / r.opt.rate);
        }
    }
    return ev;
}

void print(Room& r, int listener, const char* what, const Evenness& ev) {
    std::printf("    %s hears %s: %d windows of 10 ms (%d not, for the machine's hiccups), RMS %.3f, each %+.2f..%+.2f "
                "dB of it, longest near-silence %.2f ms, largest step x%.2f of a steady mix's%s\n",
                r[listener].name.c_str(), what, ev.windows, ev.skipped, ev.rms, ev.low_db, ev.high_db, ev.silence_ms,
                ev.step_ratio, ev.ok() ? "" : " (NOT EVEN)");
    if (!ev.ok() && ev.worst >= 0) std::printf("      the worst window at %.3f s\n", ev.worst / double(r.opt.rate));
    if (ev.ok()) return;
    // Each stream's events, against now, for the window's neighbourhood.
    const std::int64_t now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    for (int s = 0; s < r.opt.sides; ++s) {
        const int u = s == listener ? -1 : userIndex(r[listener], r[s].name);
        if (u < 0) continue;
        std::string line;
        for (const LiveEvent& e : r[listener].e.liveUserEvents(u)) {
            char b[96];
            std::snprintf(b, sizeof(b), " %s %lld at %.3f s,", liveEventName(e.kind), static_cast<long long>(e.value),
                          (e.steady_ns - now_ns) / 1e9);
            line += b;
        }
        std::printf("      %s's stream of %s:%s (now %.3f s of output)\n", r[listener].name.c_str(), r[s].name.c_str(),
                    line.c_str(), r.frames.load() / double(r.opt.rate));
    }
}

// Each listener hears the other two, each at about the level sent (0.25),
// and evenly over [from, to).
void hearsEveryoneEvenly(Room& r, long from, long to, const char* what) {
    for (int l = 0; l < r.opt.sides; ++l) {
        std::string levels;
        for (int s = 0; s < r.opt.sides; ++s) {
            if (s == l) continue;
            const double a = toneAmplitude(r[l].heard.get(), to - r.opt.rate, to, kHz[s % 3], r.opt.rate);
            char b[64];
            std::snprintf(b, sizeof(b), "%s%s at %.3f", levels.empty() ? "" : ", ", r[s].name.c_str(), a);
            levels += b;
            CHECK(a > 0.2 && a < 0.3);
        }
        const Evenness ev = evenness(r, l, from, to);
        std::printf("    %s: %s hears %s\n", what, r[l].name.c_str(), levels.c_str());
        print(r, l, "both", ev);
        CHECK(ev.windows > 0);
        CHECK(ev.ok());
    }
}

// The data the relay copied over `seconds`.
std::uint64_t relayedOver(Room& r, double seconds) {
    const std::uint64_t a = r.server.liveDataRelayed();
    r.wait(seconds);
    return r.server.liveDataRelayed() - a;
}

}  // namespace

// Three players on one machine: the room goes all-to-all, every pair's
// path opens directly, and the relay copies no audio at all.
TEST(all_to_all_on_loopback) {
    Options o;
    o.sides = 3;
    Room r(o, {"alice", "bob", "carl"});
    tones(r);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    const TimePoint t0 = Clock::now();
    CHECK(roomRouteIs(r, LiveRoute::Direct, 2));
    const bool direct = streamsAre(r, [](int, int) { return LiveRoute::Direct; }, 5);
    std::printf("    every stream direct %.2f s after the room went Live\n",
                std::chrono::duration<double>(Clock::now() - t0).count());
    printRoutes(r);
    REQUIRE(direct);
    r.wait(1.0);
    const long from = r.frames.load();
    const std::uint64_t relayed = relayedOver(r, 3.0);
    std::printf("    the relay copied %llu data datagrams in 3 s\n", static_cast<unsigned long long>(relayed));
    CHECK_EQ(relayed, 0u);
    hearsEveryoneEvenly(r, from, r.frames.load(), "all direct");
    for (int a = 0; a < 3; ++a) {
        const LiveRoomRoute rr = r[a].e.liveRoomRoute();
        CHECK(rr.route == LiveRoute::Direct && rr.reason == LiveRelayReason::None && !rr.direct_possible);
        for (int b = 0; b < 3; ++b) {
            if (a == b) continue;
            const double rtt = r[a].e.liveUserProbeRttMs(userIndex(r[a], r[b].name));
            CHECK(rtt >= 0 && rtt < 50);
            std::printf("    %s's stream of %s came by: %s\n", r[a].name.c_str(), r[b].name.c_str(),
                        routeEvents(r, a, b).c_str());
            CHECK(routeEvents(r, a, b) == "direct");
        }
    }
}

// Direct only (the default): alice and bob can't reach each other
// directly (a test hook drops what goes between them, as a network that
// lets only the relay through would). They don't hear each other at all,
// carl hears both and they hear him, all directly; once their path has
// gone unanswered for 10 s, the advice names one of them as can't play
// Live. Let through again, they hear each other directly.
TEST(a_pair_without_a_path_cant_hear_each_other) {
    Options o;
    o.sides = 3;
    Room r(o, {"alice", "bob", "carl"});
    tones(r);
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", true);   // anonymous names: name@address
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    const long live_at = r.frames.load();
    const TimePoint t0 = Clock::now();
    auto apart = [](int a, int b) {
        return (a == 0 && b == 1) || (a == 1 && b == 0) ? LiveRoute::None : LiveRoute::Direct;
    };
    REQUIRE(streamsAre(r, apart, 5));
    CHECK(roomRouteIs(r, LiveRoute::Direct, 0.1));
    const bool advice = r.until(
        [&] {
            for (int a = 0; a < 3; ++a)
                if (r[a].e.liveAdviceReason() != Engine::LiveAdvice::CantLive || r[a].e.liveAdviceCount() != 2)
                    return false;
            return true;
        },
        20);
    std::printf("    the advice came %.2f s after the room went Live: %s can't play Live (%d with trouble)\n",
                std::chrono::duration<double>(Clock::now() - t0).count(), r[2].e.liveAdviceUser().c_str(),
                r[2].e.liveAdviceCount());
    REQUIRE(advice);
    const std::string named = r[2].e.liveAdviceUser();
    CHECK(named == "alice@127.0.0.1" || named == "bob@127.0.0.1");
    printRoutes(r);
    CHECK(streamsAre(r, apart, 0.1));
    CHECK(roomRouteIs(r, LiveRoute::Direct, 0.1));
    CHECK(!r[0].e.liveUserHasDirectPath(userIndex(r[0], "bob")));
    CHECK(!r[1].e.liveUserHasDirectPath(userIndex(r[1], "alice")));
    for (int a = 0; a < 2; ++a) {
        CHECK(r[a].e.liveUserHasDirectPath(userIndex(r[a], "carl")));
        CHECK(r[2].e.liveUserHasDirectPath(userIndex(r[2], r[a].name)));
        CHECK(r[2].e.liveUserProbeRttMs(userIndex(r[2], r[a].name)) >= 0);
    }
    const std::uint64_t relayed = relayedOver(r, 2.0);
    std::printf("    the relay copied %llu data datagrams in 2 s\n", static_cast<unsigned long long>(relayed));
    CHECK_EQ(relayed, 0u);
    const long from = live_at + r.opt.rate, to = r.frames.load();
    for (int l = 0; l < 3; ++l) {
        const int unheard = l == 0 ? 1 : l == 1 ? 0 : -1;
        std::string levels;
        for (int s = 0; s < 3; ++s) {
            if (s == l) continue;
            const double a = toneAmplitude(r[l].heard.get(), to - r.opt.rate, to, kHz[s], r.opt.rate);
            char b[64];
            std::snprintf(b, sizeof(b), "%s%s at %.3f", levels.empty() ? "" : ", ", r[s].name.c_str(), a);
            levels += b;
            CHECK(s == unheard ? a < 0.005 : a > 0.2 && a < 0.3);
        }
        std::printf("    apart: %s hears %s\n", r[l].name.c_str(), levels.c_str());
        const Evenness ev = evenness(r, l, from, to, unheard);
        print(r, l, unheard < 0 ? "both" : "carl", ev);
        CHECK(ev.windows > 0);
        CHECK(ev.ok());
    }

    // Let through: their path opens with the next probe, and they hear each other.
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", false);
    const TimePoint t1 = Clock::now();
    const bool direct = streamsAre(r, [](int, int) { return LiveRoute::Direct; }, 8);
    std::printf("    let through: every stream direct %.2f s after\n",
                std::chrono::duration<double>(Clock::now() - t1).count());
    REQUIRE(direct);
    r.wait(3.0);
    printRoutes(r);
    hearsEveryoneEvenly(r, r.frames.load() - 2 * r.opt.rate, r.frames.load(), "together");
    CHECK_EQ(relayedOver(r, 1.0), 0u);
}

// The backend option relay_fallback: alice and bob can't reach each other
// directly. While their path opens, their audio goes through the relay and
// the others' directly; once it has gone unanswered for 10 s, the whole
// room goes through the relay, and nobody hears a difference. Let through
// again, the room stays on the relay, with the advice that everyone can
// connect directly, until a player chooses `!live direct`.
TEST(relay_fallback_takes_the_room_to_the_relay) {
    Options o;
    o.sides = 3;
    o.relay_fallback = true;
    Room r(o, {"alice", "bob", "carl"});
    tones(r);
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", true);   // anonymous names: name@address
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    const long live_at = r.frames.load();
    const TimePoint t0 = Clock::now();
    auto opening = [](int a, int b) {
        return (a == 0 && b == 1) || (a == 1 && b == 0) ? LiveRoute::Relay : LiveRoute::Direct;
    };
    REQUIRE(streamsAre(r, opening, 5));
    CHECK(roomRouteIs(r, LiveRoute::Direct, 0.1));
    printRoutes(r);

    // The probe rule: 10 s without an answer.
    const bool relay = roomRouteIs(r, LiveRoute::Relay, 20);
    std::printf("    the room went to the relay %.2f s after it went Live\n",
                std::chrono::duration<double>(Clock::now() - t0).count());
    REQUIRE(relay);
    REQUIRE(streamsAre(r, [](int, int) { return LiveRoute::Relay; }, 2));
    r.wait(2.0);
    printRoutes(r);
    for (int a = 0; a < 3; ++a) {
        const LiveRoomRoute rr = r[a].e.liveRoomRoute();
        const std::string pair = rr.user_a + " " + rr.user_b;
        CHECK(rr.reason == LiveRelayReason::NoPath);
        CHECK(pair == "alice@127.0.0.1 bob@127.0.0.1" || pair == "bob@127.0.0.1 alice@127.0.0.1");
        CHECK(!rr.direct_possible);
    }
    const std::uint64_t relayed = relayedOver(r, 2.0);
    // Each of three sends kBlocks a second, copied to the two others.
    std::printf("    the relay copied %llu data datagrams in 2 s\n", static_cast<unsigned long long>(relayed));
    CHECK(relayed >= 2 * 3 * 2 * kBlocksLo && relayed <= 2 * 3 * 2 * kBlocksHi + 80);
    hearsEveryoneEvenly(r, live_at + r.opt.rate, r.frames.load(), "across the change to the relay");

    // Let through: the advice comes, and the room stays on the relay.
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", false);
    const TimePoint t1 = Clock::now();
    const bool possible = r.until(
        [&] {
            for (int a = 0; a < 3; ++a)
                if (!r[a].e.liveRoomRoute().direct_possible) return false;
            return true;
        },
        8);
    std::printf("    let through: everyone can connect directly again %.2f s after\n",
                std::chrono::duration<double>(Clock::now() - t1).count());
    REQUIRE(possible);
    r.wait(3.0);
    CHECK(roomRouteIs(r, LiveRoute::Relay, 0.1));
    CHECK(streamsAre(r, [](int, int) { return LiveRoute::Relay; }, 0.1));

    // `!live direct`: everyone goes all-to-all, and the change is as even.
    const long direct_at = r.frames.load();
    r[2].e.setLiveRoomDirect();
    REQUIRE(roomRouteIs(r, LiveRoute::Direct, 3));
    REQUIRE(streamsAre(r, [](int, int) { return LiveRoute::Direct; }, 2));
    r.wait(3.0);
    printRoutes(r);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (a != b)
                std::printf("    %s's stream of %s came by: %s\n", r[a].name.c_str(), r[b].name.c_str(),
                            routeEvents(r, a, b).c_str());
    // A stream that began while its path opened came by the relay first.
    CHECK(routeEvents(r, 0, 1) == "relay, direct");
    CHECK(routeEvents(r, 1, 0) == "relay, direct");
    CHECK(endsWith(routeEvents(r, 2, 0), "direct, relay, direct"));
    CHECK(endsWith(routeEvents(r, 0, 2), "direct, relay, direct"));
    hearsEveryoneEvenly(r, direct_at - r.opt.rate, r.frames.load(), "across the change to direct");
    CHECK_EQ(relayedOver(r, 1.0), 0u);
}

// A player whose client sends through the relay only (the test setting,
// for benchmarks): the others reach it through the relay, and each other
// directly, and everyone hears everyone evenly. Without the setting, it
// goes direct with the others, make before break: its streams come by the
// relay until their paths are up, a path that opens late too.
TEST(forced_relay) {
    Options o;
    o.sides = 3;
    Room r(o, {"alice", "bob", "carl"});
    tones(r);
    r[0].e.setLiveTestForceRelay(true);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    const long live_at = r.frames.load();
    auto want = [](int a, int b) { return a == 0 || b == 0 ? LiveRoute::Relay : LiveRoute::Direct; };
    REQUIRE(streamsAre(r, want, 5));
    r.wait(2.0);
    printRoutes(r);
    CHECK(streamsAre(r, want, 0.1));
    CHECK(roomRouteIs(r, LiveRoute::Direct, 0.1));
    for (int b = 1; b < 3; ++b) {
        CHECK(!r[0].e.liveUserHasDirectPath(userIndex(r[0], r[b].name)));
        CHECK(!r[b].e.liveUserHasDirectPath(userIndex(r[b], "alice")));
        CHECK(r[b].e.liveUserProbeRttMs(userIndex(r[b], "alice")) < 0);
    }
    hearsEveryoneEvenly(r, live_at + r.opt.rate, r.frames.load(), "alice through the relay");
    const std::uint64_t relayed = relayedOver(r, 2.0);
    // alice's blocks to the two others, and theirs to her: 4 streams of kBlocks a second.
    std::printf("    the relay copied %llu data datagrams in 2 s\n", static_cast<unsigned long long>(relayed));
    CHECK(relayed >= 2 * 4 * kBlocksLo && relayed <= 2 * 4 * kBlocksHi + 80);
    for (int a = 0; a < 3; ++a) CHECK(r[a].e.liveAdviceReason() == Engine::LiveAdvice::None);

    // alice can send directly again. Make before break: each stream the
    // relay carried goes on through it until its path is up. Her path
    // with bob opens late (the test's block, for 1.5 s): meanwhile their
    // two streams still come by the relay, and nobody hears a gap.
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", true);
    r[0].e.setLiveTestForceRelay(false);
    const long open_at = r.frames.load();
    auto late = [](int a, int b) { return (a == 0 && b == 1) || (a == 1 && b == 0) ? LiveRoute::Relay : LiveRoute::Direct; };
    REQUIRE(streamsAre(r, late, 8));
    r.wait(1.5);
    printRoutes(r);
    CHECK(streamsAre(r, late, 0.1));
    r[0].e.setLiveTestBlockPeer("bob@127.0.0.1", false);
    REQUIRE(streamsAre(r, [](int, int) { return LiveRoute::Direct; }, 8));
    r.wait(2.0);
    printRoutes(r);
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (a != b)
                std::printf("    %s's stream of %s came by: %s\n", r[a].name.c_str(), r[b].name.c_str(),
                            routeEvents(r, a, b).c_str());
    hearsEveryoneEvenly(r, open_at - r.opt.rate, r.frames.load(), "across alice's change to direct");
    CHECK_EQ(relayedOver(r, 1.0), 0u);
}

// With relay_fallback, a player whose client sends through the relay only
// takes the room to the relay at once, and `!live direct` doesn't take it
// back while that player is there. Without the setting, the advice comes,
// and the room stays on the relay.
TEST(relay_fallback_forced_relay) {
    Options o;
    o.sides = 3;
    o.relay_fallback = true;
    Room r(o, {"alice", "bob", "carl"});
    tones(r);
    r[0].e.setLiveTestForceRelay(true);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    const long live_at = r.frames.load();
    REQUIRE(roomRouteIs(r, LiveRoute::Relay, 5));
    REQUIRE(streamsAre(r, [](int, int) { return LiveRoute::Relay; }, 2));
    r.wait(2.0);
    printRoutes(r);
    for (int a = 0; a < 3; ++a) {
        const LiveRoomRoute rr = r[a].e.liveRoomRoute();
        CHECK(rr.reason == LiveRelayReason::RelayOnly && rr.user_a.compare(0, 6, "alice@") == 0 && !rr.direct_possible);
    }
    for (int b = 1; b < 3; ++b) {
        CHECK(r[0].e.liveUserProbeRttMs(userIndex(r[0], r[b].name)) < 0);   // no probe answered
        CHECK(r[b].e.liveUserProbeRttMs(userIndex(r[b], "alice")) < 0);
    }
    r[2].e.setLiveRoomDirect();                  // refused: alice is there
    r.wait(1.0);
    CHECK(roomRouteIs(r, LiveRoute::Relay, 0.1));
    hearsEveryoneEvenly(r, live_at + r.opt.rate, r.frames.load(), "everyone through the relay");
    const std::uint64_t relayed = relayedOver(r, 2.0);
    std::printf("    the relay copied %llu data datagrams in 2 s\n", static_cast<unsigned long long>(relayed));
    CHECK(relayed >= 2 * 3 * 2 * kBlocksLo && relayed <= 2 * 3 * 2 * kBlocksHi + 80);

    r[0].e.setLiveTestForceRelay(false);
    const bool possible = r.until([&] { return r[2].e.liveRoomRoute().direct_possible; }, 8);
    printRoutes(r);
    REQUIRE(possible);
    r.wait(2.0);
    CHECK(roomRouteIs(r, LiveRoute::Relay, 0.1));
}

TEST_MAIN()
