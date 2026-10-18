// Live mode for the whole room (o11proto/live_room.h, ENGINE.md 16.7 and
// 16.8), end to end in real time through o11server: the room's mode follows
// its !live switch and nothing else, and while it is Live there is no
// interval audio at all. From intervals to Live every switch is seamless;
// from Live to intervals (at once) the live audio fades out, and the
// intervals fade in where they come back, at a boundary, with nothing heard
// live replayed. A player who can't play Live neither hears nor is heard in
// a Live room, and the advice says who has trouble. Each sender plays a
// steady sine of its own, and each listener's output is measured around
// every switch: the largest step against the steady tone's, the longest
// near-silence, and the 10 ms RMS against the steady RMS. The last case
// writes live-transitions.wav for listening.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "live_harness.h"
#include "live_sink.h"
#include "o11proto/client.h"

using namespace harness;

namespace {

float tone(long n, int rate, double hz) { return static_cast<float>(0.25 * std::sin(2 * M_PI * hz * n / rate)); }

// A client without Live (an older one): it can't play Live.
struct Plain {
    Client c;
    bool join(Room& r, const std::string& name) {
        Client::Options o;
        o.username = "anonymous:" + name;
        o.codec_caps = kCapOpus | kCapFlac;
        ChannelInfoRecord ch;
        ch.name = "ch0";
        c.setChannels({ch});
        if (!c.connect("127.0.0.1", r.server.port(), o)) return false;
        r.extra = [this] { c.poll(0); };
        return r.until([&] { return c.loggedIn(); }, 5);
    }
    void leave(Room& r) {
        c.disconnect();
        r.extra = nullptr;
    }
};

struct Tally {
    int measured = 0, skipped = 0, hushes = 0;
    double step = 0, silence = 0, low = 0, high = 0, level = 0;
    double hush_least = 0, hush_most = 0, hush_step = 0;   // the switches at once: their silence (ms), the fades' steps
    void add(const Transition& t) {
        ++measured;
        step = std::max(step, t.step_ratio);
        silence = std::max(silence, t.silence_ms);
        low = std::min(low, t.rms_low_db);
        high = std::max(high, t.rms_high_db);
        level = std::max(level, std::fabs(t.level_db));
    }
    void print(const char* what) const {
        std::printf("    %s: %d switches measured (%d not, for the machine's hiccups): largest step x%.2f of the steady "
                    "tone's, longest near-silence %.2f ms, 10 ms RMS %+.2f..%+.2f dB, level change at most %.2f dB\n",
                    what, measured, skipped, step, silence, low, high, level);
        if (hushes > 0)
            std::printf("    %s: %d switches at once from Live: silent for %.0f to %.0f ms, the fades' largest step "
                        "x%.2f\n",
                        what, hushes, hush_least, hush_most, hush_step);
    }
};

// `listener`'s switch away from `was`, from block `from` on: measured and
// checked when the machine was calm around it. Its frame, or -1.
long measureSwitch(Room& r, int listener, long from, bool was, const std::string& what, Tally& t,
                   Transition* out = nullptr) {
    long frame = 0;
    if (r.roomSwitch(listener, from, was, &frame) < 0) {
        std::printf("    %s: no switch\n", what.c_str());
        return -1;
    }
    const long ms = r.opt.rate / 1000;
    r.until([&] { return r.frames.load() > frame + 1400 * ms; }, 3);
    const Transition tr = transition(r[listener].heard.get(), frame, r.opt.rate);
    if (out) *out = tr;
    if (!r.calm(frame - 700 * ms, frame + 700 * ms)) {
        ++t.skipped;
        std::printf("    %s: the machine had a hiccup around it, not measured\n", what.c_str());
        return frame;
    }
    print(what.c_str(), tr);
    t.add(tr);
    CHECK(seamless(tr));
    return frame;
}

// A switch from Live to intervals at once (`!live off`, 16.8): the live
// audio fades out, and nothing is heard until the intervals come in at a
// boundary, with each player's first interval after Live, which fades in.
// No one uploads interval audio while the room is Live, so nothing heard
// live is replayed.
struct Hush {
    long frame = -1;                             // the switch
    long in = -1;                                // where the intervals come in
    long boundary = -1;                          // the listener's boundary they come in at
    double out_step = 0;                         // the fade-out's largest step, against the steady sound's
    double in_step = 0;                          // the fade-in's largest step, against the steady sound's after
    double in_rms_db = 0;                        // the 10 ms RMS from 25 ms on, against the steady RMS after
    double level_db = 0;                         // `hz` through the intervals after against live before
};

// `listener`'s switch from Live to intervals at once, from block `from` on,
// measured and checked when the machine was calm around it; `hz` is a tone
// heard live before it. With `steady_sound` false (the drums) only where
// the intervals come in is checked.
Hush measureOff(Room& r, int listener, long from, const std::string& what, Tally& t, double hz,
                bool steady_sound = true) {
    Hush h;
    if (r.roomSwitch(listener, from, true, &h.frame) < 0) {
        std::printf("    %s: no switch\n", what.c_str());
        h.frame = -1;
        return h;
    }
    const long ms = r.opt.rate / 1000, s = h.frame, L = intervalLengthFor(r.opt.bpm, r.opt.bpi, r.opt.rate);
    r.until([&] { return r.frames.load() > s + 2 * L + 1500 * ms; }, 8);
    const float* x = r[listener].heard.get();
    for (long i = s + 25 * ms; i < s + 2 * L + 100 * ms && h.in < 0; ++i)
        if (std::fabs(x[i]) >= 0.001f) h.in = i;
    if (h.in < 0) {
        std::printf("    %s: the intervals don't come in\n", what.c_str());
        CHECK(h.in > 0);
        return h;
    }
    const long n = h.in;
    for (long b = n / kBlock; b >= n / kBlock - 1 && h.boundary < 0; --b)
        if (r.boundaryIn(listener, b)) h.boundary = (b + 1) * kBlock - r[listener].pos[b];
    const Steady before = steady(x, s - 600 * ms, s - 100 * ms), after = steady(x, n + 100 * ms, n + 600 * ms);
    double out = 0, in = 0;
    for (long i = s - 50 * ms; i < s + 50 * ms; ++i) out = std::max(out, double(std::fabs(x[i] - x[i - 1])));
    for (long i = n - 5 * ms; i < n + 100 * ms; ++i) in = std::max(in, double(std::fabs(x[i] - x[i - 1])));
    h.out_step = out / std::max(1e-9, std::max(before.step, after.step));
    h.in_step = in / std::max(1e-9, after.step);
    h.in_rms_db = dB(steady(x, n + 25 * ms, n + 35 * ms).rms / std::max(1e-12, after.rms));
    h.level_db = dB(toneAmplitude(x, n + 300 * ms, n + 1300 * ms, hz, r.opt.rate) /
                    std::max(1e-9, toneAmplitude(x, s - 1300 * ms, s - 300 * ms, hz, r.opt.rate)));
    if (!r.calm(s - 700 * ms, n + 1400 * ms)) {
        ++t.skipped;
        std::printf("    %s: the machine had a hiccup around it, not measured\n", what.c_str());
        return h;
    }
    ++t.hushes;
    t.hush_least = t.hushes == 1 ? r.ms(n - s) : std::min(t.hush_least, r.ms(n - s));
    t.hush_most = std::max(t.hush_most, r.ms(n - s));
    const double late = h.boundary >= 0 ? r.ms(n - h.boundary) : -1;
    if (!steady_sound) {
        std::printf("    %s: silent for %.0f ms (an interval is %.0f), then the intervals, %.1f ms after a boundary\n",
                    what.c_str(), r.ms(n - s), r.ms(L), late);
    } else {
        t.hush_step = std::max(t.hush_step, std::max(h.out_step, h.in_step));
        std::printf("    %s: the fade-out's largest step x%.2f, silent for %.0f ms (an interval is %.0f), then the "
                    "intervals, %.1f ms after a boundary, fade in with a largest step x%.2f of the steady sound's, "
                    "10 ms RMS from 25 ms on %+.2f dB, level %+.2f dB against live\n",
                    what.c_str(), h.out_step, r.ms(n - s), r.ms(L), late, h.in_step, h.in_rms_db, h.level_db);
        CHECK(h.out_step <= 1.5);
        CHECK(h.in_step <= 1.5);
        CHECK(std::fabs(h.in_rms_db) <= 1.0);
        CHECK(std::fabs(h.level_db) <= 0.5);
    }
    CHECK(n - s >= L);                           // at least an interval: what would replay the live audio
    CHECK(h.boundary >= 0 && late >= 0 && late <= 3);
    return h;
}

// The level of `hz` heard live after a switch at `s` against through
// intervals before it (or the other way round), in dB.
double levelMatch(Room& r, int listener, long s, double hz) {
    const long ms = r.opt.rate / 1000;
    const float* x = r[listener].heard.get();
    const double before = toneAmplitude(x, s - 1300 * ms, s - 300 * ms, hz, r.opt.rate);
    const double after = toneAmplitude(x, s + 300 * ms, s + 1300 * ms, hz, r.opt.rate);
    return dB(after / std::max(1e-9, before));
}

std::string sideName(Room& r, int i) { return r[i].name; }

// The advice `s` has now (16.7): `who` is the start of the user's name.
bool advised(Side& s, Engine::LiveAdvice reason, const std::string& who, int count = 1) {
    return s.e.liveAdviceReason() == reason && s.e.liveAdviceUser().find(who) == 0 && s.e.liveAdviceCount() == count;
}
bool noAdvice(Side& s) {
    return s.e.liveAdviceReason() == Engine::LiveAdvice::None && s.e.liveAdviceUser().empty() &&
           s.e.liveAdviceCount() == 0;
}
// The advice names `who` as a player who can't play Live, whoever else
// has trouble too (after a hiccup of the machine's, every player may).
bool cantLiveNamed(Side& s, const std::string& who) {
    return s.e.liveAdviceReason() == Engine::LiveAdvice::CantLive && s.e.liveAdviceUser().find(who) == 0 &&
           s.e.liveAdviceCount() >= 1;
}
// Whether any side in `which` has advice within `seconds`.
bool anyAdvice(Room& r, const std::vector<int>& which, double seconds) {
    return r.until(
        [&] {
            for (int i : which)
                if (!noAdvice(r[i])) return true;
            return false;
        },
        seconds);
}

// The notice of advice that a player can't play Live, or has dropouts.
std::string cantText(const std::string& who) {
    return who + " can't play Live and isn't heard while the room is Live: Intervals may sound better (switch with "
                 "/live off).";
}
std::string dropoutsText(const std::string& who) {
    return (who.empty() ? std::string("Your connection") : who + "'s connection") +
           " has dropouts: Intervals may sound better (switch with /live off).";
}
// The first notice from `from` on that gives the advice `text` (one of the
// above), also when it says that more players have trouble (", and 1 more
// player has trouble" before the colon), as after one of the machine's
// hiccups, when every player may have had dropouts; or "".
std::string findAdvice(Side& s, const std::string& text, std::size_t from = 0) {
    const std::size_t colon = text.find(": Intervals");
    const std::string head = text.substr(0, colon), tail = text.substr(colon);
    std::lock_guard<std::mutex> lock(s.m);
    for (std::size_t i = from; i < s.notices.size(); ++i) {
        const std::string& n = s.notices[i];
        if (n.size() < head.size() + tail.size() || n.compare(0, head.size(), head) != 0 ||
            n.compare(n.size() - tail.size(), tail.size(), tail) != 0)
            continue;
        const std::string mid = n.substr(head.size(), n.size() - head.size() - tail.size());
        if (mid.empty() || mid.compare(0, 6, ", and ") == 0) return n;
    }
    return std::string();
}

std::string num3(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", v);
    return b;
}

// The index of the user whose name starts with `name` at `s`, or -1.
int userIndex(Side& s, const std::string& name) {
    for (int u = 0; u < s.e.numUsers(); ++u)
        if (s.e.userName(u).find(name) == 0) return u;
    return -1;
}

// Both directions, at once and at a boundary, for one host rate and
// interval codec; and an older client joining and leaving, which doesn't
// switch the room.
void transitions(int rate, std::uint32_t codec, const std::string& label) {
    Options o;
    o.rate = rate;
    o.codec = codec;
    Room r(o, {"alice", "bob"});
    const double hz[2] = {500, 700};
    for (int i = 0; i < 2; ++i) {
        const double f = hz[i];
        r[i].signal = [f](long n, int rt) { return tone(n, rt, f); };
    }
    r.startHost();
    REQUIRE(r.connect());
    Tally t;
    std::vector<double> levels;
    const long ms = rate / 1000;

    // 1. The room is Live from the start (its switch is on), at a
    // boundary. Its first intervals had no audio yet: not measured.
    const long b0 = r.block();
    REQUIRE(r.roomIs(true, 30));
    std::printf("    the room is Live %.1f s after connecting\n", r.secondsOf(r.block() - b0));
    for (int i = 0; i < 2; ++i) {
        long s = 0;
        CHECK(r.roomSwitch(i, b0, false, &s) > 0 && r.boundaryIn(i, s / kBlock));
    }
    CHECK(r.until([&] { return !r[1].findNotice("The room is Live now, about ").empty(); }, 2));

    // 2. Live to intervals at once: `!live off`, in the middle of an
    // interval. Nothing is heard until the intervals come in.
    r.wait(1.5);
    REQUIRE(r.until(
        [&] {
            int p = 0, len = 0;
            r[1].e.position(&p, &len);
            return len > 0 && p > len / 4 && p < len / 2;
        },
        3));
    const long b1 = r.block();
    CHECK(r[0].e.liveCommand("/live off"));
    REQUIRE(r.roomIs(false, 2));
    for (int i = 0; i < 2; ++i) {
        const Hush h = measureOff(r, i, b1, label + " " + sideName(r, i) + ", Live to intervals at once", t, hz[1 - i]);
        REQUIRE(h.frame > 0);
        CHECK(!r.boundaryIn(i, h.frame / kBlock));
        std::printf("    %s on intervals %.0f ms after !live off\n", sideName(r, i).c_str(),
                    double(h.frame - b1 * kBlock) * 1000 / rate);
    }

    // 3. Intervals to Live, at a boundary: `!live on`.
    const long b2 = r.block();
    CHECK(r[1].e.liveCommand("/live on"));
    REQUIRE(r.roomIs(true, 6));
    for (int i = 0; i < 2; ++i) {
        const long s = measureSwitch(r, i, b2, false, label + " " + sideName(r, i) + ", intervals to Live", t);
        REQUIRE(s > 0);
        CHECK(r.boundaryIn(i, s / kBlock));
        if (r.calm(s - 1400 * ms, s + 1400 * ms)) levels.push_back(levelMatch(r, i, s, hz[1 - i]));
    }

    // 4. An older client joins: it can't play Live. The room stays Live, and
    // everyone is advised who. A player's dropouts count for 10 s (16.7):
    // after a hiccup of the machine's within them, alice and bob may have
    // trouble too, and the advice counts them, so carol is the only one only
    // on a calm machine.
    Plain carol;
    const long b3 = r.block();
    const std::size_t seen[2] = {r[0].noticeCount(), r[1].noticeCount()};
    REQUIRE(carol.join(r, "carol"));
    for (int i = 0; i < 2; ++i) {
        CHECK(r.until([&] { return !findAdvice(r[i], cantText("carol"), seen[i]).empty(); }, 3));
        CHECK(cantLiveNamed(r[i], "carol"));
        const bool calm = r.calm(b3 * kBlock - 11L * rate, r.frames.load());
        if (calm) CHECK(advised(r[i], Engine::LiveAdvice::CantLive, "carol"));
        if (!advised(r[i], Engine::LiveAdvice::CantLive, "carol"))
            std::printf("    %s's advice names carol and counts %d players with trouble%s\n", sideName(r, i).c_str(),
                        r[i].e.liveAdviceCount(), calm ? "" : " (the machine had a hiccup)");
    }
    r.wait(3);
    for (int i = 0; i < 2; ++i) CHECK(r.roomSwitch(i, b3, true) < 0 && r[i].e.liveRoomLive());

    // 5. It leaves: the advice clears, without a notice (it isn't here).
    // carol's at once; after a hiccup of the machine's, the others'
    // dropouts may hold advice of their own for up to 10 s more.
    carol.leave(r);
    for (int i = 0; i < 2; ++i) {
        CHECK(r.until([&] { return !cantLiveNamed(r[i], "carol") && r[i].e.liveAdviceUser().find("carol") != 0; }, 3));
        const bool none = r.until([&] { return noAdvice(r[i]); }, 3);
        const bool calm = r.calm(b3 * kBlock - 11L * rate, r.frames.load());
        if (calm) CHECK(none);
        if (!none)
            std::printf("    %s's advice 3 s after carol left: %s, %d players%s\n", sideName(r, i).c_str(),
                        r[i].e.liveAdviceUser().c_str(), r[i].e.liveAdviceCount(),
                        calm ? "" : " (the machine had a hiccup)");
        CHECK(r[i].findNotice("carol can play Live now.").empty());
        CHECK(r.roomSwitch(i, b3, true) < 0);
    }
    std::printf("    the room stayed Live while carol was here; the advice: \"%s\"\n",
                findAdvice(r[0], cantText("carol")).c_str());
    t.print(label.c_str());
    for (double l : levels) {
        std::printf("    %s: the same player live against through intervals: %+.2f dB\n", label.c_str(), l);
        CHECK(std::fabs(l) <= 0.5);
    }
}

}  // namespace

TEST(transitions_opus_48k) { transitions(48000, kCapOpus, "Opus 48 kHz"); }
TEST(transitions_flac_48k) { transitions(48000, kCapFlac, "FLAC 48 kHz"); }
TEST(transitions_opus_44k) { transitions(44100, kCapOpus, "Opus 44.1 kHz"); }
TEST(transitions_flac_44k) { transitions(44100, kCapFlac, "FLAC 44.1 kHz"); }

// Four players: no interval audio flows while the room is Live; `!live off`
// switches everyone at once, and `!live on` everyone at its own next
// boundary, so all within one interval of each other, each one seamless. An
// older client joining switches no one.
TEST(room_of_four_switches_together) {
    Options o;
    o.sides = 4;
    Room r(o, {"alice", "bob", "carl", "dina"});
    const double hz[4] = {500, 700, 1100, 1300};
    for (int i = 0; i < 4; ++i) {
        const double f = hz[i];
        r[i].signal = [f](long n, int rt) { return tone(n, rt, f); };
    }
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.allConnectedSee(3));
    Tally t;
    const long interval = 2L * o.rate, ms = o.rate / 1000;
    REQUIRE(r.roomIs(true, 30));
    r.wait(2);
    // Live: no interval audio flows.
    const Server::IntervalTraffic t0 = r.server.intervalTraffic();
    r.wait(2);
    const Server::IntervalTraffic t1 = r.server.intervalTraffic();
    std::printf("    2 s of a Live room of four: %llu bytes of interval audio uploaded, %llu relayed\n",
                static_cast<unsigned long long>(t1.from_live + t1.from_others - t0.from_live - t0.from_others),
                static_cast<unsigned long long>(t1.relayed - t0.relayed));
    CHECK(t1.from_live == t0.from_live && t1.from_others == t0.from_others && t1.relayed == t0.relayed);

    const long b1 = r.block();
    CHECK(r[2].e.liveCommand("/live off"));
    REQUIRE(r.roomIs(false, 3));
    long lo = -1, hi = -1;
    for (int i = 0; i < 4; ++i) {
        // Measured against the tone of the player after this one.
        const Hush h = measureOff(r, i, b1, "Live to intervals at once, " + sideName(r, i), t, hz[(i + 1) % 4]);
        if (h.frame < 0) continue;
        lo = lo < 0 ? h.frame : std::min(lo, h.frame);
        hi = std::max(hi, h.frame);
    }
    std::printf("    !live off: the four switched within %.0f ms of each other\n", r.ms(hi - lo));
    CHECK(lo >= 0 && hi - lo <= 250 * ms);

    const long b2 = r.block();
    CHECK(r[3].e.liveCommand("/live on"));
    REQUIRE(r.roomIs(true, 6));
    lo = hi = -1;
    for (int i = 0; i < 4; ++i) {
        const long s = measureSwitch(r, i, b2, false, "intervals to Live, " + sideName(r, i), t);
        if (s < 0) continue;
        CHECK(r.boundaryIn(i, s / kBlock));
        lo = lo < 0 ? s : std::min(lo, s);
        hi = std::max(hi, s);
    }
    std::printf("    !live on: the four switched within %.0f ms of each other (an interval is %.0f ms)\n",
                r.ms(hi - lo), r.ms(interval));
    CHECK(lo >= 0 && hi - lo <= interval);

    Plain eve;
    const long b3 = r.block();
    REQUIRE(eve.join(r, "eve"));
    CHECK(r.until(
        [&] {
            for (int i = 0; i < 4; ++i)
                if (!advised(r[i], Engine::LiveAdvice::CantLive, "eve")) return false;
            return true;
        },
        3));
    r.wait(2);
    eve.leave(r);
    CHECK(r.until(
        [&] {
            for (int i = 0; i < 4; ++i)
                if (!noAdvice(r[i])) return false;
            return true;
        },
        3));
    bool stayed = true;
    for (int i = 0; i < 4; ++i) stayed = stayed && r.roomSwitch(i, b3, true) < 0;
    std::printf("    eve, an older client, joins and leaves: the room %s Live\n", stayed ? "stays" : "doesn't stay");
    CHECK(stayed);
    t.print("room of four");
}

// Requirement: a player whose host is stopped sends nothing; that is no
// one's fault, so nobody is advised, and starting and stopping again gives
// no advice either.
TEST(stopped_daw_gives_no_advice) {
    Options o;
    o.sides = 3;
    Room r(o, {"alice", "bob", "dave"});
    const double hz[3] = {500, 700, 1100};
    for (int i = 0; i < 3; ++i) {
        const double f = hz[i];
        r[i].signal = [f](long n, int rt) { return tone(n, rt, f); };
    }
    r[2].e.setLiveHostState(false, false);       // dave's host is stopped
    r.startHost();
    REQUIRE(r.connect());
    const long b0 = r.block();
    REQUIRE(r.roomIs(true, 30));
    std::printf("    the room is Live %.1f s after connecting, with dave's host stopped\n", r.secondsOf(r.block() - b0));
    const bool early = anyAdvice(r, {0, 1, 2}, 3);
    std::printf("    advice while dave's host is stopped: %s\n", early ? "some" : "none");
    if (r.calm(b0 * kBlock, r.frames.load())) CHECK(!early);
    std::size_t seen = r[0].noticeCount();
    r[0].e.liveCommand("/live");
    const std::string line = r[0].findNotice("dave: ", seen);
    std::printf("    %s\n", line.c_str());
    CHECK(line.find("dave: not sending") == 0);
    long checked_from = b0 * kBlock;             // where the window of the check before began
    for (int step = 0; step < 2; ++step) {
        // Advice lasts until 10 s after the reports are clean again, 2 s
        // after the last dropout (16.7). Advice still there from before
        // this step came with a hiccup of the machine's (else the check
        // before failed), so it clears first.
        auto none = [&] { return noAdvice(r[0]) && noAdvice(r[1]) && noAdvice(r[2]); };
        if (!none() && !r.calm(checked_from, r.frames.load()))
            std::printf("    the advice from the machine's hiccup %s\n",
                        r.until(none, 25) ? "clears first" : "doesn't clear within 25 s");
        const long from = r.block();
        checked_from = from * kBlock;
        r[2].e.setLiveHostState(step == 0, false);   // it starts, then stops again
        const bool some = anyAdvice(r, {0, 1, 2}, 4);
        r.until([&] { return r.frames.load() > (from * kBlock) + 4L * o.rate; }, 4);
        const bool calm = r.calm(from * kBlock, r.frames.load());
        bool stayed = true;
        for (int i = 0; i < 3; ++i) stayed = stayed && r.roomSwitch(i, from, true) < 0;
        std::printf("    dave's host %s: the room %s Live for 4 s, %s%s\n", step == 0 ? "starts" : "stops",
                    stayed ? "stays" : "doesn't stay", some ? "with advice" : "no advice",
                    calm ? "" : " (the machine had a hiccup)");
        CHECK(stayed);
        if (calm) CHECK(!some);
    }
}

// Requirement: in a Live room nobody uploads interval audio, and the server
// relays none. A client with Live off still uploads (it doesn't know), but
// isn't relayed: it neither hears nor is heard, and the others are advised
// who. After !live off the interval traffic is back by the next interval,
// and everyone hears everyone through intervals.
TEST(client_without_live_in_a_live_room) {
    Options o;
    o.sides = 3;
    Room r(o, {"alice", "bob", "carol"});
    const double hz[3] = {500, 700, 1100};
    for (int i = 0; i < 3; ++i) {
        const double f = hz[i];
        r[i].signal = [f](long n, int rt) { return tone(n, rt, f); };
    }
    r[2].e.setLiveEnabled(false);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.allConnectedSee(2));
    REQUIRE(r.roomIs(true, 30, {0, 1}));
    CHECK(!r[2].e.liveRoomLive());
    for (int i = 0; i < 2; ++i) {
        CHECK(r.until([&] { return !findAdvice(r[i], cantText("carol")).empty(); }, 3));
        CHECK(advised(r[i], Engine::LiveAdvice::CantLive, "carol"));
    }
    std::printf("    %s: %s\n", r[0].name.c_str(), r[0].findNotice("carol").c_str());
    const long ms = o.rate / 1000, L = intervalLengthFor(o.bpm, o.bpi, o.rate);
    // Past the interval the room went Live in, 4 s of a Live room.
    r.wait(r.ms(L) / 1000 + 0.5);
    const Server::IntervalTraffic t0 = r.server.intervalTraffic();
    const long f0 = r.frames.load();
    r.wait(4);
    const Server::IntervalTraffic t1 = r.server.intervalTraffic();
    const long f1 = r.frames.load();
    std::printf("    4 s of a Live room: %llu bytes of interval audio from the Live players, %llu from carol (%llu "
                "uploads refused), %llu relayed\n",
                static_cast<unsigned long long>(t1.from_live - t0.from_live),
                static_cast<unsigned long long>(t1.from_others - t0.from_others),
                static_cast<unsigned long long>(t1.refused - t0.refused),
                static_cast<unsigned long long>(t1.relayed - t0.relayed));
    CHECK(t1.from_live == t0.from_live);
    CHECK(t1.relayed == t0.relayed);
    CHECK(t1.from_others > t0.from_others && t1.refused > t0.refused);
    // Meanwhile alice and bob hear each other live and not carol, and carol
    // hears nothing.
    const bool calm = r.calm(f0 - 100 * ms, f1);
    for (int i = 0; i < 3; ++i) {
        const float* x = r[i].heard.get();
        double a[3], loud = 0;
        for (int k = 0; k < 3; ++k) a[k] = toneAmplitude(x, f0, f1, hz[k], o.rate);
        for (long n = f0; n < f1; ++n) loud = std::max(loud, double(std::fabs(x[n])));
        std::string heard;
        for (int k = 0; k < 3; ++k) {
            if (k == i) continue;
            const int u = userIndex(r[i], r[k].name);
            CHECK(u >= 0);
            const bool live = u >= 0 && r[i].e.userLive(u);
            heard += " " + r[k].name + (live ? " live" : " not live") + " at " + num3(a[k]);
            CHECK(live == (i != 2 && k != 2));
            if (!calm) continue;
            if (i != 2 && k != 2) CHECK(a[k] > 0.2);
            else CHECK(a[k] < 0.001);
        }
        std::printf("    %s hears%s, the loudest sample %.5f%s\n", r[i].name.c_str(), heard.c_str(), loud,
                    calm ? "" : " (the machine had a hiccup)");
        if (i == 2 && calm) CHECK(loud < 0.001);
    }
    std::size_t seen = r[0].noticeCount();
    r[0].e.liveCommand("/live");
    const std::string room = r[0].findNotice("The room", seen);
    std::printf("    %s\n    %s\n", room.c_str(), r[0].findNotice("carol: ", seen).c_str());
    CHECK(room == "The room is Live. " + cantText("carol"));
    CHECK(!r[0].findNotice("carol: not heard while the room is Live (not a Live player", seen).empty());

    // !live off: the traffic is back by the next interval, and some
    // intervals later everyone hears everyone, through intervals.
    const Server::IntervalTraffic t2 = r.server.intervalTraffic();
    const long f2 = r.frames.load();
    CHECK(r[0].e.liveCommand("/live off"));
    REQUIRE(r.roomIs(false, 2, {0, 1}));
    CHECK(noAdvice(r[0]) && noAdvice(r[1]));
    const bool back = r.until([&] { return r.server.intervalTraffic().relayed > t2.relayed; }, r.ms(L) / 1000 + 1.5);
    std::printf("    !live off: interval audio relayed again %.0f ms after\n", r.ms(r.frames.load() - f2));
    CHECK(back);
    const long w0 = f2 + 3 * L, w1 = w0 + 1000 * ms;
    r.until([&] { return r.frames.load() > w1; }, 10);
    if (r.calm(w0 - 100 * ms, w1)) {
        for (int i = 0; i < 3; ++i) {
            const int p = i == 0 ? 1 : 0, q = i == 2 ? 1 : 2;   // the two others
            const float* x = r[i].heard.get();
            const double ap = toneAmplitude(x, w0, w1, hz[p], o.rate), aq = toneAmplitude(x, w0, w1, hz[q], o.rate);
            std::printf("    then %s hears %s at %.3f and %s at %.3f, through intervals\n", r[i].name.c_str(),
                        r[p].name.c_str(), ap, r[q].name.c_str(), aq);
            CHECK(ap > 0.2 && aq > 0.2);
            CHECK(std::fabs(dB(aq / std::max(1e-9, ap))) <= 0.5);
        }
    } else {
        std::printf("    the levels after !live off aren't checked: the machine had a hiccup\n");
    }
}

// The health rule (16.7) gives advice, and the room stays Live whatever
// happens: a single gap of up to a second fades out and comes back with no
// advice; three gaps within 2 s, or one longer than a second, give advice
// naming the player, which clears once they have been clean for 10 s.
TEST(gaps_give_advice_and_the_room_stays_live) {
    Options o;
    Room r(o, {"alice", "bob"});
    r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
    r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    r.wait(1);
    const long ms = o.rate / 1000;
    const long b0 = r.block();

    // One gap of 250 ms in alice's packets.
    const long g = r.frames.load();
    r[0].e.setLiveTestSendGap(250);
    const bool advice1 = anyAdvice(r, {0, 1}, 4);
    r.until([&] { return r.frames.load() > g + 4L * o.rate; }, 4);
    const bool calm = r.calm(g - 500 * ms, r.frames.load());
    // What bob heard: concealment, the fade, silence, the fade back.
    const float* x = r[1].heard.get();
    const Steady st = steady(x, g - 500 * ms, g);
    long quiet_from = -1, quiet_to = -1;
    for (long i = g; i < g + 1000 * ms && quiet_to < 0; ++i) {
        if (quiet_from < 0 && std::fabs(x[i]) < 0.001f) {
            bool run = true;
            for (long k = i; k < i + 5 * ms && run; ++k) run = std::fabs(x[k]) < 0.001f;
            if (run) quiet_from = i;
        } else if (quiet_from >= 0 && std::fabs(x[i]) >= 0.001f) {
            quiet_to = i;
        }
    }
    double back_step = 0;
    for (long i = std::max(quiet_to, 1L); quiet_to > 0 && i < quiet_to + 40 * ms; ++i)
        back_step = std::max(back_step, double(std::fabs(x[i] - x[i - 1])));
    std::printf("    a 250 ms gap: bob silent from %.0f to %.0f ms after alice's gap began, the fade back's largest "
                "step x%.2f of the steady tone's; %s%s\n",
                r.ms(quiet_from - g), r.ms(quiet_to - g), back_step / std::max(1e-9, st.step),
                advice1 ? "advice" : "no advice", calm ? "" : " (the machine had a hiccup)");
    if (calm) {
        CHECK(!advice1);
        CHECK(quiet_from > g && quiet_to > quiet_from);
        CHECK(back_step <= 1.5 * st.step);
    }

    // Three gaps of 150 ms within 2 s: advice, naming alice.
    const std::size_t seen[2] = {r[0].noticeCount(), r[1].noticeCount()};
    const long g3 = r.frames.load();
    for (int k = 0; k < 3; ++k) {
        r[0].e.setLiveTestSendGap(150);
        r.wait(0.6);
    }
    REQUIRE(r.until([&] { return !findAdvice(r[1], dropoutsText("alice"), seen[1]).empty(); }, 6));
    std::printf("    three gaps within 2 s: advice %.2f s after the first; bob: \"%s\"\n", r.ms(r.frames.load() - g3) / 1000,
                findAdvice(r[1], dropoutsText("alice"), seen[1]).c_str());
    CHECK(r.until([&] { return !findAdvice(r[0], dropoutsText(""), seen[0]).empty(); }, 2));
    CHECK(advised(r[1], Engine::LiveAdvice::Dropouts, "alice"));
    CHECK(advised(r[0], Engine::LiveAdvice::Dropouts, "alice"));

    // Clean again: the advice clears 10 s after the reports are clean (a
    // player is clean again 2 s after its last dropout, 6.4). A dropout
    // meanwhile that follows a hiccup of the machine's (three within 2 s
    // make a clean report fail) starts those 10 s again, rightly: then the
    // time isn't checked.
    const long last = r.frames.load();
    const int alice_at_bob = userIndex(r[1], "alice");
    auto dropouts = [&] {
        LiveCounters c;
        return alice_at_bob >= 0 && r[1].e.liveUserCounters(alice_at_bob, &c)
                   ? static_cast<long>(c.underruns + c.overflows + c.xrun_gaps)
                   : -1L;
    };
    const long drops0 = dropouts();
    REQUIRE(r.until([&] { return !r[1].findNotice("alice's connection is clean again.", seen[1]).empty(); }, 20));
    const double cleared = r.ms(r.frames.load() - last) / 1000;
    const long drops = dropouts() - drops0;
    const bool clean_calm = r.calm(last, r.frames.load());
    std::printf("    the advice clears %.1f s after the last gap; bob's stream of alice had %ld dropouts meanwhile%s\n",
                cleared, drops, clean_calm ? "" : " (the machine had a hiccup)");
    if (clean_calm || drops == 0) CHECK(cleared >= 10.0 && cleared <= 14.5);
    CHECK(r.until([&] { return !r[0].findNotice("Your connection is clean again.", seen[0]).empty(); }, 2));
    CHECK(noAdvice(r[0]) && noAdvice(r[1]));

    // One gap of 1.5 s: advice again; the audio comes back, live.
    r.wait(1);
    const std::size_t seen5 = r[1].noticeCount();
    const long g5 = r.frames.load();
    r[0].e.setLiveTestSendGap(1500);
    REQUIRE(r.until([&] { return !findAdvice(r[1], dropoutsText("alice"), seen5).empty(); }, 6));
    std::printf("    a 1.5 s gap: advice %.2f s after it began\n", r.ms(r.frames.load() - g5) / 1000);
    r.until([&] { return r.frames.load() > g5 + 4L * o.rate; }, 5);
    const double before = toneAmplitude(x, g5 - 1100 * ms, g5 - 100 * ms, 500, o.rate);
    const double after = toneAmplitude(x, g5 + 2500 * ms, g5 + 3500 * ms, 500, o.rate);
    std::printf("    alice's tone after the gap at %+.2f dB of before it\n", dB(after / std::max(1e-9, before)));
    if (r.calm(g5 - 1200 * ms, g5 + 3600 * ms)) CHECK(std::fabs(dB(after / std::max(1e-9, before))) <= 0.5);
    for (int i = 0; i < 2; ++i) CHECK(r.roomSwitch(i, b0, true) < 0);   // Live all along
}

// A slot lost in the middle of an interval while the room is Live: the
// room stays Live, and the listener hears concealment, a 20 ms fade to
// silence, and silence (no interval audio: the player is still heard live);
// when the audio comes back it fades in again, live.
TEST(slot_lost_mid_interval) {
    Options o;
    Room r(o, {"alice", "bob"});
    r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
    r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    r.wait(1);
    r.until(
        [&] {
            int p = 0, len = 0;
            r[1].e.position(&p, &len);
            return len > 0 && p > len / 4 && p < len / 2;
        },
        3);
    const long ms = o.rate / 1000;
    const std::size_t seen = r[1].noticeCount();
    const long g = r.frames.load(), gap = 4000 * ms;
    r[0].e.setLiveTestSendGap(4000);
    r.until([&] { return r.frames.load() > g + gap + 4000 * ms; }, 10);
    const float* x = r[1].heard.get();
    const Steady live = steady(x, g - 500 * ms, g);
    // The fade: the 2 ms RMS from -1 dB of the steady RMS to silence.
    long down = -1, quiet = -1;
    for (long i = g; i < g + gap && quiet < 0; i += ms / 2) {
        const double rms = steady(x, i, i + 2 * ms).rms;
        if (down < 0 && rms < live.rms * 0.891) down = i;
        if (rms < 0.001) quiet = i;
    }
    // Anything heard between the end of the fade and the gap's end: from
    // the buffer, the concealment and the fade on (the concealment itself
    // may have gone quiet before).
    const double B = r[1].e.liveBufferInUseMs();
    const long faded = g + static_cast<long>((B + kLiveConcealBuffers * B + kLiveFadeMs + 20) * ms);
    double loud = 0;
    long loud_at = faded;
    for (long i = std::max(quiet + 2 * ms, faded); quiet > 0 && i < g + gap; ++i)
        if (std::fabs(x[i]) > loud) {
            loud = std::fabs(x[i]);
            loud_at = i;
        }
    // The audio back, live.
    long back = -1;
    for (long i = g + gap - 100 * ms; i < g + gap + 2500 * ms && back < 0; ++i)
        if (std::fabs(x[i]) >= 0.001f) back = i;
    double back_step = 0;
    for (long i = std::max(back, 1L); back > 0 && i < back + 40 * ms; ++i)
        back_step = std::max(back_step, double(std::fabs(x[i] - x[i - 1])));
    const double level = dB(toneAmplitude(x, back + 300 * ms, back + 1300 * ms, 500, o.rate) /
                            std::max(1e-9, toneAmplitude(x, g - 1100 * ms, g - 100 * ms, 500, o.rate)));
    const bool calm = r.calm(g - 1200 * ms, back + 1400 * ms);
    std::printf("    alice's packets stop for 4 s: bob's level falls 1 dB %.0f ms later and is silent %.0f ms later; "
                "the loudest sample from %.0f ms on %.5f (at %.0f ms); her audio is back %.0f ms after the gap's end, "
                "with a largest step x%.2f of the steady tone's, at %+.2f dB of before%s\n",
                r.ms(down - g), r.ms(quiet - g), r.ms(faded - g), loud, r.ms(loud_at - g), r.ms(back - g - gap),
                back_step / std::max(1e-9, live.step), level, calm ? "" : " (the machine had a hiccup)");
    for (int i = 0; i < 2; ++i) CHECK(r.roomSwitch(i, g / kBlock, true) < 0);   // the room stays Live
    // The advice names alice; after a hiccup of the machine's (whose
    // dropouts count for 10 s, 16.7) it may name another player.
    const std::string named = findAdvice(r[1], dropoutsText("alice"), seen);
    const bool calm_advice = r.calm(g - 11L * o.rate, r.frames.load());
    if (named.empty()) {
        std::printf("    no advice named alice%s; bob's notices since the gap:\n",
                    calm_advice ? "" : " (the machine had a hiccup)");
        std::lock_guard<std::mutex> lock(r[1].m);
        for (std::size_t i = seen; i < r[1].notices.size(); ++i) std::printf("      %s\n", r[1].notices[i].c_str());
    }
    if (calm_advice) CHECK(!named.empty());
    if (calm) {
        CHECK(down > g && quiet > down);
        CHECK(r.ms(quiet - g) <= 250);          // the buffer, 80 ms of concealment, the 20 ms fade
        CHECK(loud < 0.001);                     // no interval audio while she is heard live
        CHECK(back > g + gap);
        CHECK(back_step <= 1.5 * live.step);
        CHECK(std::fabs(level) <= 0.5);
    }
}

// 16.2: what a UI reads from the control context. liveServerHas() and
// liveRoomSwitchOn() follow the latest SERVER_LIVE, and liveHeardMs() is the
// N of "The room is Live now, about N ms late." while the room is Live; the
// advice is none in a room where everyone's Live sounds clean, and while it
// is on intervals.
TEST(accessors_for_a_ui) {
    {
        Options o;
        Room r(o, {"alice", "bob"});
        r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
        r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
        CHECK(!r[1].e.liveServerHas() && !r[1].e.liveRoomSwitchOn() && r[1].e.liveHeardMs() == -1);
        CHECK(!r[1].e.liveRoomLive() && noAdvice(r[1]));
        r.startHost();
        REQUIRE(r.connect());
        CHECK(r.until([&] { return r[1].e.liveServerHas() && r[1].e.liveRoomSwitchOn(); }, 5));
        REQUIRE(r.roomIs(true, 30));
        CHECK(r.until([&] { return !r[1].findNotice("The room is Live now, about ").empty(); }, 5));
        const std::string notice = r[1].findNotice("The room is Live now, about ");
        const int n = notice.empty() ? -2 : std::atoi(notice.c_str() + 28);
        const int heard = r[1].e.liveHeardMs();
        std::printf("    Live: has %d, switch %d, heard %d ms; the notice: %s\n", r[1].e.liveServerHas(),
                    r[1].e.liveRoomSwitchOn(), heard, notice.c_str());
        CHECK(heard >= 5 && heard <= 200);
        CHECK(std::abs(heard - n) <= 1);         // the estimate may have moved since
        CHECK(r[1].e.liveRoomLive());
        const long from = r.frames.load();
        const bool some = anyAdvice(r, {0, 1}, 2);
        if (some)
            for (int i = 0; i < 2; ++i)
                std::printf("    %s is advised: %d, %s, %d\n", r[i].name.c_str(), int(r[i].e.liveAdviceReason()),
                            r[i].e.liveAdviceUser().c_str(), r[i].e.liveAdviceCount());
        if (r.calm(from, r.frames.load())) CHECK(!some);
        CHECK(r[0].e.liveCommand("/live off"));
        CHECK(r.until([&] { return !r[1].e.liveRoomSwitchOn(); }, 5));
        CHECK(r.roomIs(false, 5));
        std::printf("    after !live off: has %d, switch %d, heard %d\n", r[1].e.liveServerHas(),
                    r[1].e.liveRoomSwitchOn(), r[1].e.liveHeardMs());
        CHECK(r[1].e.liveServerHas());
        CHECK(r[1].e.liveHeardMs() == -1);
        CHECK(!r[1].e.liveRoomLive() && noAdvice(r[1]));
    }
    {
        Options o;
        o.live_server = false;
        Room r(o, {"alice", "bob"});
        r.startHost();
        for (int i = 0; i < 2; ++i) r[i].e.connect("127.0.0.1", r.server.port(), "anonymous:" + r[i].name, "");
        REQUIRE(r.until([&] { return r[0].e.status() == Status::Ok && r[1].e.status() == Status::Ok; }, 10));
        r.wait(1);                               // no Live client starts (Room::connect would wait for one)
        std::printf("    a server without Live: has %d, switch %d, heard %d\n", r[1].e.liveServerHas(),
                    r[1].e.liveRoomSwitchOn(), r[1].e.liveHeardMs());
        CHECK(!r[1].e.liveServerHas() && !r[1].e.liveRoomSwitchOn() && r[1].e.liveHeardMs() == -1);
        CHECK(!r[1].e.liveRoomLive() && noAdvice(r[1]));
    }
}

// ------------------------------------------------------- the listening file

namespace {

// 909-style drums, synthesized here (no samples), as one repeating pattern
// of two bars of 16ths. alice plays the kick (steps 1, 5, 9 and 13) and the
// snare (5 and 13); bob the closed hi-hat on every 16th (accented on the
// off-beat eighths, 3, 7, 11 and 15), the open hi-hat in place of the
// closed one on step 15 of every second bar, and the clap on 13. Every
// voice starts with a short raised-cosine attack and decays to exactly zero
// as (1 - u/length)^4, flat at its end: there are no cut-offs, and a hit's
// start is the only step in the drums.
double hitEnvelope(double u, double attack, double length) {
    if (u < 0 || u >= length) return 0.0;
    const double a = u < attack ? 0.5 - 0.5 * std::cos(M_PI * u / attack) : 1.0;
    const double d = 1.0 - u / length;
    return a * d * d * d * d;
}

struct Biquad {                                  // RBJ cookbook: a high-pass, or a band-pass of 0 dB peak
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Biquad(bool band, double hz, double q, int rate) {
        const double w = 2 * M_PI * hz / rate, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        if (band) {
            b0 = al / a0, b1 = 0, b2 = -al / a0;
        } else {
            b0 = (1 + c) / 2 / a0, b1 = -(1 + c) / a0, b2 = (1 + c) / 2 / a0;
        }
        a1 = -2 * c / a0, a2 = (1 - al) / a0;
    }
    double operator()(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
};

struct Drums {
    int rate = 48000;
    long origin = 0;                             // the frame where a pattern starts: an interval boundary
    long step = 0;                               // a 16th, in frames
    std::vector<float> alice, bob;               // one pattern each, its tails folded back onto its start
    float peak = 0;                              // of the mix

    Drums(int r, int bpm, long first) : rate(r), origin(first) {
        step = static_cast<long>(std::lround(60.0 * rate / bpm / 4));
        const long period = 32 * step, len = period + rate;   // tails up to a second
        // Noise, filtered once for each voice that uses it.
        std::vector<double> snare_noise(static_cast<std::size_t>(len)), clap_noise(snare_noise), hat_noise(snare_noise);
        Biquad hp_snare(false, 1800, 0.7, rate), bp_clap(true, 1200, 1.5, rate);
        Biquad hp_hat(false, 6500, 0.7, rate), bp_hat(true, 8500, 0.9, rate);
        std::uint32_t seed = 0x2545F491u;
        for (long i = 0; i < len; ++i) {
            seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
            const double w = static_cast<double>(seed) / 2147483648.0 - 1.0;
            const std::size_t k = static_cast<std::size_t>(i);
            snare_noise[k] = hp_snare(w);
            clap_noise[k] = bp_clap(w);
            hat_noise[k] = bp_hat(hp_hat(w));
        }
        std::vector<double> a(static_cast<std::size_t>(len)), b(a);
        auto hit = [&](std::vector<double>& out, long at, double length, const std::function<double(double, long)>& voice) {
            for (long i = 0; i < static_cast<long>(length * rate) && at + i < len; ++i)
                out[static_cast<std::size_t>(at + i)] += voice(static_cast<double>(i) / rate, at + i);
        };
        auto kick = [&](double u, long) {        // 150 Hz falling to 50 Hz over about 30 ms
            const double phase = 2 * M_PI * (50 * u + 100 * 0.010 * (1 - std::exp(-u / 0.010)));
            const double click = u < 0.003 ? 0.5 - 0.5 * std::cos(2 * M_PI * u / 0.003) : 0.0;   // the soft transient
            return 0.18 * std::sin(phase) * hitEnvelope(u, 0.0015, 0.50) +
                   0.036 * click * (0.6 * std::sin(2 * M_PI * 1100 * u) + 0.4 * std::sin(2 * M_PI * 2300 * u));
        };
        auto snare = [&](double u, long n) {     // two tones near 180 and 330 Hz, and high-passed noise
            return 0.06 * (0.6 * std::sin(2 * M_PI * 180 * u) + 0.4 * std::sin(2 * M_PI * 330 * u)) *
                       hitEnvelope(u, 0.001, 0.15) +
                   0.10 * snare_noise[static_cast<std::size_t>(n)] * hitEnvelope(u, 0.001, 0.25);
        };
        auto clap = [&](double u, long n) {      // four 9 ms bursts 10 ms apart, then a tail
            double e = 0.6 * hitEnvelope(u - 0.030, 0.001, 0.16);
            for (int k = 0; k < 4; ++k) e += hitEnvelope(u - 0.010 * k, 0.0005, 0.009);
            return 0.17 * clap_noise[static_cast<std::size_t>(n)] * e;
        };
        auto hat = [&](double length, double level) {
            return [&, length, level](double u, long n) {
                return level * hat_noise[static_cast<std::size_t>(n)] * hitEnvelope(u, 0.0003, length);
            };
        };
        for (int s = 0; s < 32; ++s) {
            const long at = s * step;
            const int q = s % 16;                // 0-based: step 1 is q = 0
            if (q % 4 == 0) hit(a, at, 0.50, kick);
            if (q == 4 || q == 12) hit(a, at, 0.25, snare);
            if (q == 12) hit(b, at, 0.20, clap);
            if (q == 14 && s >= 16) hit(b, at, 0.45, hat(0.45, 0.06));   // the open hat, every second bar
            else hit(b, at, 0.06, hat(0.06, q % 4 == 2 ? 0.07 : 0.03));
        }
        alice.assign(static_cast<std::size_t>(period), 0.0f);
        bob.assign(static_cast<std::size_t>(period), 0.0f);
        for (long i = 0; i < len; ++i) {         // the tails past the pattern's end ring into its start
            alice[static_cast<std::size_t>(i % period)] += static_cast<float>(a[static_cast<std::size_t>(i)]);
            bob[static_cast<std::size_t>(i % period)] += static_cast<float>(b[static_cast<std::size_t>(i)]);
        }
        for (long i = 0; i < period; ++i)
            peak = std::max(peak, std::fabs(alice[static_cast<std::size_t>(i)] + bob[static_cast<std::size_t>(i)]));
    }
    std::size_t at(long n) const {
        const long p = static_cast<long>(alice.size());
        return static_cast<std::size_t>(((n - origin) % p + p) % p);
    }
    float player(int which, long n) const { return (which == 0 ? alice : bob)[at(n)]; }
    float mix(long n) const { return alice[at(n)] + bob[at(n)]; }
    bool hitIn(long from, long to) const {     // a hit starts in [from, to): there is one on every 16th
        const long rel = from - origin, k = rel >= 0 ? (rel + step - 1) / step : -(-rel / step);
        return origin + k * step < to;
    }
};

// Steady tones instead (O11PROTO_LISTENING=tones), where whatever a switch
// or a gap does to the sound is easy to see: alice plays 440 Hz and a quiet
// 3520 Hz (-20 dB, so that a glitch in the treble shows), bob 660 Hz, each
// at about -12 dBFS, from a 50 ms fade-in at frame 0 on, and never stop.
struct Tones {
    int rate = 48000;
    float player(int which, long n) const {
        if (n < 0) return 0.0f;
        const double t = static_cast<double>(n) / rate, in = rate / 20.0;
        const double fade = n < in ? 0.5 - 0.5 * std::cos(M_PI * n / in) : 1.0;
        const double v = which == 0 ? 0.22 * std::sin(2 * M_PI * 440 * t) + 0.022 * std::sin(2 * M_PI * 3520 * t)
                                    : 0.24 * std::sin(2 * M_PI * 660 * t);
        return static_cast<float>(fade * v);
    }
};

}  // namespace

TEST(listening_file) {
    Options o;
    o.sides = 3;
    o.live_on = false;                           // the room plays intervals first
    const char* which = std::getenv("O11PROTO_LISTENING");
    const bool tones = which && std::string(which) == "tones";
    // The drums' pattern starts where the engines' interval clocks start
    // (the first block after they connected, which is later on a slow
    // machine): each bar is an interval, and its first kick falls on the
    // boundary. Until then the players send silence. (With the tones the
    // drums only place the gaps.) Both outlive the room, whose host reads them.
    std::unique_ptr<const Drums> drums_made;
    std::atomic<const Drums*> playing{nullptr};
    const Tones tone{o.rate};
    Room r(o, {"alice", "bob", "carl"});
    if (tones) {
        float peak = 0;
        for (long n = 0; n < o.rate; ++n) peak = std::max(peak, std::fabs(tone.player(0, n) + tone.player(1, n)));
        std::printf("    the tones peak at %.1f and %.1f dBFS, together at %.1f dBFS\n", 20 * std::log10(0.242),
                    20 * std::log10(0.24), 20 * std::log10(peak));
        r[0].signal = [&tone](long n, int) { return tone.player(0, n); };
        r[1].signal = [&tone](long n, int) { return tone.player(1, n); };
    } else {
        for (int p = 0; p < 2; ++p)
            r[p].signal = [&playing, p](long n, int) {
                const Drums* d = playing.load(std::memory_order_acquire);
                return d ? d->player(p, n) : 0.0f;
            };
    }
    LocalChannel lc;                             // carl listens: he sends nothing
    lc.name = "listen";
    lc.mute = true;
    r[2].e.setLocalChannel(0, lc);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.until([&] { return r[0].clock_from >= 0 && r[1].clock_from >= 0 && r[2].clock_from >= 0; }, 3));
    if (r[0].clock_from != r[2].clock_from || r[1].clock_from != r[2].clock_from)
        std::printf("    the interval clocks started apart: alice at %ld, bob at %ld, carl at %ld (the drums at "
                    "carl's)\n",
                    r[0].clock_from.load(), r[1].clock_from.load(), r[2].clock_from.load());
    drums_made.reset(new Drums(o.rate, o.bpm, r[2].clock_from.load()));
    const Drums& drums = *drums_made;
    playing.store(&drums, std::memory_order_release);
    if (!tones) {
        std::printf("    the drums peak at %.1f dBFS\n", 20 * std::log10(drums.peak));
        CHECK(drums.peak < 0.5f);                // under -6 dBFS
    }
    // The room's switch is off: it plays intervals. Once they have the
    // players' audio, alice types !live on, in the middle of an interval,
    // and the room goes Live at the boundary after.
    const long L = intervalLengthFor(o.bpm, o.bpi, o.rate), ms = o.rate / 1000;
    r.wait(4);
    REQUIRE(r.until(
        [&] {
            int p = 0, len = 0;
            r[2].e.position(&p, &len);
            return len > 0 && p > len / 4 && p < len / 2;
        },
        3));
    const long on_at = r.frames.load();
    CHECK(r[0].e.liveCommand("/live on"));
    REQUIRE(r.roomIs(true, 6, {2}));
    long live_at = 0;
    REQUIRE(r.roomSwitch(2, 0, false, &live_at) > 0);
    CHECK_EQ((live_at - drums.origin) % intervalLengthFor(o.bpm, o.bpi, o.rate), 0L);   // the bars line up
    const long start = std::max(0L, live_at - 3L * o.rate / 2);
    // How late carl hears the others, from "The room is Live now, about N ms late."
    const std::string now_live = r[2].findNotice("The room is Live now, about ");
    const std::string late = now_live.empty() ? std::string("?") : now_live.substr(28, now_live.find(" ms", 28) - 28);
    struct Event {
        long frame;
        std::string text;
    };
    std::vector<Event> ev;
    ev.push_back({start, "the room plays intervals (its Live is off)"});
    ev.push_back({on_at, "alice types !live on"});
    ev.push_back({live_at, "the room goes Live"});
    const int gaps[5] = {20, 50, 80, 120, 250};
    auto waitUntil = [&](long frame) { r.until([&] { return r.frames.load() >= frame; }, 30); };
    // Each gap starts a 16th after a beat, where alice plays no hit and her
    // kick (and, on the beats with one, her snare) is decaying.
    const long beat = 4 * drums.step, mid = drums.step;
    long gap_at[5] = {}, conceal_at[5] = {-1, -1, -1, -1, -1};
    const int alice_at_carl = userIndex(r[2], "alice");
    for (int k = 0; k < 5; ++k) {
        const long t = live_at + static_cast<long>((1.5 + 2.0 * k) * o.rate);
        waitUntil(t + ((mid - t) % beat + beat) % beat);
        const long f = r.frames.load();
        gap_at[k] = f;
        r[0].e.setLiveTestSendGap(gaps[k]);
        ev.push_back({f, "a " + std::to_string(gaps[k]) + " ms gap in alice's packets (the ones she sends from now are "
                             "lost; carl hears the players about " + late + " ms late)"});
        // Where carl's concealment of it begins, to the frame (his engine
        // counts the same frames as r.frames).
        waitUntil(f + (gaps[k] + 100) * ms);
        const long c = alice_at_carl >= 0 ? r[2].e.liveTestConcealedFrom(alice_at_carl) : -1;
        if (c > f && c < f + 100 * ms) {
            conceal_at[k] = c;
            ev.push_back({c, "the gap reaches carl: his concealment of alice begins"});
        }
    }
    // dave, a client without Live, joins: the room stays Live, and carl is
    // advised. Just before the boundary 2.5 s later (where the room used to
    // fall back on its own) alice turns Live off: nothing is heard for an
    // interval, while the players upload their first interval after Live;
    // dave leaves; and alice turns Live on again for the boundary 8 s after
    // that.
    waitUntil(live_at + static_cast<long>(11.5 * o.rate));
    Plain dave;
    ev.push_back({r.frames.load(), "dave, a client without Live, joins (the room stays Live)"});
    REQUIRE(dave.join(r, "dave"));
    if (r.until([&] { return r[2].e.liveAdviceReason() == Engine::LiveAdvice::CantLive; }, 3))
        ev.push_back({r.frames.load(), "carl is advised: " + r[2].findNotice("dave can't play Live")});
    // (Others may have trouble too, after a hiccup of the machine's.)
    CHECK(r[2].e.liveAdviceReason() == Engine::LiveAdvice::CantLive && r[2].e.liveAdviceUser().find("dave") == 0);
    // Before a boundary, so that the players upload from it: 200 ms, for the
    // switch to reach them in time on a slow machine too.
    waitUntil(live_at + 14L * o.rate - 200 * ms);
    const long bo = r.block();
    ev.push_back({r.frames.load(), "alice types !live off"});
    CHECK(r[0].e.liveCommand("/live off"));
    REQUIRE(r.roomIs(false, 2, {2}));
    long off = 0;
    REQUIRE(r.roomSwitch(2, bo, true, &off) > 0);
    ev.push_back({off, "the room is on intervals at once; there is no interval audio until the players' first "
                       "interval after Live"});
    r.wait(1);
    ev.push_back({r.frames.load(), "dave leaves"});
    dave.leave(r);
    waitUntil(live_at + static_cast<long>(21.2 * o.rate));
    const long b2 = r.block();
    ev.push_back({r.frames.load(), "alice types !live on"});
    CHECK(r[0].e.liveCommand("/live on"));
    REQUIRE(r.roomIs(true, 6, {2}));
    long again = 0;
    r.roomSwitch(2, b2, false, &again);
    ev.push_back({again, "the room is Live again"});
    waitUntil(again + 3L * o.rate / 2);
    const long end = r.frames.load();
    // The room stayed Live through the gaps and while dave was here.
    const long first = r.roomSwitch(2, live_at / kBlock + 1, true);
    std::printf("    the room %s Live until !live off\n", first >= bo ? "stayed" : "didn't stay");
    CHECK(first >= bo);
    Tally t;
    const Hush h = measureOff(r, 2, bo, "!live off, carl", t, 440, tones);
    if (h.in > 0) ev.push_back({h.boundary >= 0 ? h.boundary : h.in, "the intervals come in, faded in"});
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });

    const std::string name = tones ? "live-transitions-tones" : "live-transitions";
    const std::string wav = name + ".wav", txt = name + ".txt";
    CHECK(writeWav(wav, r[2].heard.get() + start, end - start, o.rate));
    float heard_peak = 0;
    for (long n = start; n < end; ++n) heard_peak = std::max(heard_peak, std::fabs(r[2].heard[n]));
    std::printf("    what carl hears peaks at %.1f dBFS\n", 20 * std::log10(heard_peak));
    if (!tones) CHECK(heard_peak < 0.5f);
    std::FILE* f = std::fopen(txt.c_str(), "w");
    REQUIRE(f);
    if (tones)
        std::fprintf(f, "%s: what carl hears, 48 kHz mono, %.1f s. The music is steady tones: alice plays\n"
                        "440 Hz and a quiet 3520 Hz (-20 dB), bob 660 Hz, each at about -12 dBFS, from a 50 ms\n"
                        "fade-in at the players' frame 0 on, without a break; carl sends nothing. The room is at\n"
                        "%d bpm, the timeline is the drum file's (live-transitions.wav). A slot without audio is\n"
                        "concealed for %.0f x B = %.0f ms from where carl's concealment begins (\"the gap reaches\n"
                        "carl\"), then fades out over %.0f ms; every switch crossfades over %.0f ms. While the room\n"
                        "is Live there is no interval audio: after !live off nothing is heard until the players'\n"
                        "first interval after Live comes in, faded in. The first sample is frame %ld of the\n"
                        "players' clock. Times in seconds from the start.\n\n",
                     wav.c_str(), double(end - start) / o.rate, o.bpm, kLiveConcealBuffers,
                     kLiveConcealBuffers * r[2].e.liveBufferInUseMs(), kLiveFadeMs, kLiveFadeMs, start);
    else
        std::fprintf(f, "%s: what carl hears, 48 kHz mono, %.1f s. The music is 909-style drums, synthesized,\n"
                        "at %d bpm, one bar per interval: alice plays the kick and the snare, bob the closed and open\n"
                        "hi-hats and the clap; carl sends nothing. A slot without audio is concealed for %.0f x B =\n"
                        "%.0f ms from where carl's concealment begins (\"the gap reaches carl\"), then fades out over\n"
                        "%.0f ms; every switch crossfades over %.0f ms. While the room is Live there is no interval\n"
                        "audio: after !live off nothing is heard until the players' first interval after Live comes\n"
                        "in, faded in. The first sample is frame %ld of the players' clock. Times in seconds from the\n"
                        "start.\n\n",
                     wav.c_str(), double(end - start) / o.rate, o.bpm, kLiveConcealBuffers,
                     kLiveConcealBuffers * r[2].e.liveBufferInUseMs(), kLiveFadeMs, kLiveFadeMs, start);
    for (const Event& e : ev) std::fprintf(f, "%7.3f  %s\n", double(e.frame - start) / o.rate, e.text.c_str());
    std::fclose(f);
    std::printf("    wrote %s (%.1f s) and %s:\n", wav.c_str(), double(end - start) / o.rate, txt.c_str());
    for (const Event& e : ev) std::printf("      %7.3f  %s\n", double(e.frame - start) / o.rate, e.text.c_str());

    // Where carl hears each gap (16.6): where his engine says its
    // concealment began; else, as the packets lost are the ones sent in it
    // and a block goes a few ms after it was played, about `d` - 8 ms after
    // the gap was set (give or take a block). Her audio comes back the
    // gap's length later, on the delay it had. Each check takes in 10 ms
    // either side of that.
    const float* x = r[2].heard.get();
    const double hold_ms = kLiveConcealBuffers * r[2].e.liveBufferInUseMs();
    auto concealedAt = [&](int k, long d) { return conceal_at[k] >= 0 ? conceal_at[k] : gap_at[k] + d - 8 * ms; };

    if (tones) {
        // A steady tone goes on through a gap: through one of up to 4 x B
        // alice's 440 Hz stays within 1 dB of its level and the rest (all
        // but the three tones) under -40 dB, from before the concealment
        // to 120 ms after her audio is back; through a longer one it holds
        // for 4 x B, fades, and comes back with nothing over -40 dB. The
        // level is taken over 10 ms, a tone with an envelope that changes
        // in a straight line (over 5 ms the 660 Hz is too close to tell
        // from it), and the rest over every 5 ms, with envelopes that may
        // curve (for the codec's own changes of level from frame to frame).
        const std::vector<double> hz = {440, 3520, 660};
        const int B5 = 5 * static_cast<int>(ms);
        const long d = (now_live.empty() ? 28 : std::atoi(late.c_str())) * ms;
        auto level = [&](long f) {
            double a[3], rest;
            toneFit(x, f, 2 * B5, hz, o.rate, a, &rest, 1);
            return a[0];
        };
        auto restOf = [&](long f) {
            double a[3], rest;
            toneFit(x, f, B5, hz, o.rate, a, &rest);
            return rest;
        };
        std::vector<double> lv;
        for (long f = live_at + 300 * ms; f + 2 * B5 <= live_at + 1300 * ms; f += B5) lv.push_back(level(f));
        std::nth_element(lv.begin(), lv.begin() + static_cast<std::ptrdiff_t>(lv.size() / 2), lv.end());
        const double a440 = lv[lv.size() / 2], rms440 = a440 / std::sqrt(2.0);
        double floor_lo = 100, floor_hi = -100, floor_rest = -200;   // while nothing happens, for comparison
        for (long f = live_at + 300 * ms; f + 2 * B5 <= live_at + 1300 * ms; f += B5) {
            const double db = 20 * std::log10(level(f) / a440);
            floor_lo = std::min(floor_lo, db), floor_hi = std::max(floor_hi, db);
            floor_rest = std::max(floor_rest, 20 * std::log10(restOf(f) / rms440 + 1e-12));
        }
        std::printf("    alice's 440 Hz plays at %.1f dBFS live; while nothing happens, from %+.2f to %+.2f dB of that, "
                    "and the rest at most %.1f dB against it\n",
                    20 * std::log10(a440), floor_lo, floor_hi, floor_rest);
        for (int k = 0; k < 5; ++k) {
            const long c = concealedAt(k, d), back = c + gaps[k] * ms;
            if (!r.calm(c - 200 * ms, back + 300 * ms)) {
                std::printf("    the %d ms gap isn't checked: the machine had a hiccup\n", gaps[k]);
                continue;
            }
            const bool short_gap = gaps[k] <= hold_ms;
            const long held_to = short_gap ? back + 120 * ms : c + static_cast<long>(hold_ms) * ms - 15 * ms;
            double lo = 100, hi = -100, worst = -200, worst_at = 0;
            for (long f = c - 10 * ms; f + B5 <= back + 120 * ms; f += B5) {
                if (f + 2 * B5 <= held_to) {
                    const double db = 20 * std::log10(level(f) / a440 + 1e-12);
                    lo = std::min(lo, db), hi = std::max(hi, db);
                }
                const double rdb = 20 * std::log10(restOf(f) / rms440 + 1e-12);
                if (rdb > worst) worst = rdb, worst_at = r.ms(f - c);
            }
            std::printf("    the %3d ms gap: alice's 440 Hz from %+.2f to %+.2f dB of its level %s, the rest at most "
                        "%.1f dB (%+.0f ms from the concealment)\n",
                        gaps[k], lo, hi, short_gap ? "through it" : "while it holds", worst, worst_at);
            CHECK(lo >= -1.0 && hi <= 1.0);
            CHECK(worst <= -40.0);
        }
        // Every gap longer than 4 x B is held for the same time from where
        // carl's concealment of it began (16.8): his 440 Hz (its envelope over
        // 1/220 s, which takes out the 660 and 3520 Hz) falls to half its
        // level the same time after that, within 0.1 ms.
        const int K = o.rate / 220;
        auto env440 = [&](long f) {
            double re = 0, im = 0;
            for (int i = 0; i < K; ++i) {
                const long n = f - K / 2 + i;
                const double ph = 2 * M_PI * 440.0 * static_cast<double>(n % o.rate) / o.rate;
                re += x[n] * std::cos(ph);
                im -= x[n] * std::sin(ph);
            }
            return 2 * std::hypot(re, im) / K;
        };
        double held_lo = 1e9, held_hi = -1e9;
        for (int k = 0; k < 5; ++k) {
            if (gaps[k] <= hold_ms || conceal_at[k] < 0) continue;
            if (!r.calm(conceal_at[k] - 200 * ms, conceal_at[k] + 300 * ms)) continue;
            const double ref = env440(conceal_at[k] - 20 * ms);
            long f = conceal_at[k] + static_cast<long>(hold_ms - 10) * ms;
            while (f < conceal_at[k] + static_cast<long>(hold_ms + 40) * ms && env440(f) >= 0.5 * ref) ++f;
            const double held = r.ms(f - conceal_at[k]);
            held_lo = std::min(held_lo, held), held_hi = std::max(held_hi, held);
            std::printf("    the %3d ms gap: carl's 440 Hz is down 6 dB %.2f ms after his concealment began (the fade "
                        "starts %.0f ms after it)\n", gaps[k], held, hold_ms);
        }
        if (held_hi >= held_lo) CHECK(held_hi - held_lo <= 0.1);
        // After a gap each player plays on the delay it had before it
        // (16.6), so the two stay in time: each one's delay, from the phase
        // of its tone over 800 ms before the gap and 800 ms from half a
        // second after it, moves by at most 0.1 ms. (Within 1 ms is what
        // matters, but the phase tells a delay only within the tone's
        // period, 2.3 ms at 440 Hz: a block of 5 ms more or less reads as
        // 0.45 ms. A delay that holds moves by none.)
        const double sine[2] = {440, 660};
        for (int k = 0; k < 5; ++k) {
            const long g = gap_at[k], g1 = g + gaps[k] * ms;
            double moved[2];
            for (int p = 0; p < 2; ++p) {
                const double before = sineDelay(x, g - 900 * ms, g - 100 * ms, sine[p], o.rate);
                const double after = sineDelay(x, g1 + 500 * ms, g1 + 1300 * ms, sine[p], o.rate);
                moved[p] = std::remainder(after - before, o.rate / sine[p]) * 1000.0 / o.rate;
            }
            std::printf("    the %3d ms gap: alice's delay moved by %+.3f ms, bob's by %+.3f ms\n", gaps[k], moved[0],
                        moved[1]);
            if (!r.calm(g - 1000 * ms, g1 + 1400 * ms)) {
                std::printf("      (not checked: the machine had a hiccup)\n");
                continue;
            }
            for (int p = 0; p < 2; ++p) CHECK(std::fabs(moved[p]) <= 0.1);
        }
        return;
    }

    // Clicks: no 5 ms block may bend the sound more than 3 times as much as
    // its surroundings do (the median within 200 ms) and as the same drums
    // played straight do there, heard through the path or paths that play
    // then, live or an interval later. The drums' own count within 5 ms of
    // the block, since the codecs spread an attack by a few ms (before it
    // too, as pre-echo), and a block where a hit starts is exempt: how a
    // codec renders an attack depends on how it falls on its frames.
    //
    // Through intervals each player is an interval late on the clocks'
    // own terms: an interval that player began at its boundary plays from
    // carl's next one. So when the interval clocks started apart, each
    // player has a lag of its own there too: an interval plus carl's start
    // less theirs (on a CI runner alice's started 11.25 ms after the
    // others', and her drums were heard 11.25 ms earlier through intervals
    // than bob's). Each is measured on its own, as the live lags are.
    auto material = [&](long n) { return drums.mix(n); };
    const long d_live = bestLag(x, material, live_at + 300 * ms, live_at + 1300 * ms, 0, 100 * ms);
    long d_int[2];
    for (int p = 0; p < 2; ++p) {
        auto played = [&drums, p](long n) { return drums.player(p, n); };
        d_int[p] = bestLag(x, played, start + 100 * ms, live_at - 100 * ms, L - 50 * ms, L + 50 * ms);
    }
    std::printf("    carl hears the players %.1f ms late live; through intervals alice %.1f ms late and bob %.1f ms "
                "(an interval and their clocks' starts against carl's: %.1f and %.1f ms)\n",
                r.ms(d_live), r.ms(d_int[0]), r.ms(d_int[1]), r.ms(L + r[2].clock_from - r[0].clock_from),
                r.ms(L + r[2].clock_from - r[1].clock_from));
    // carl hears each player through a sink of its own, so each player's
    // live delay is measured again for every 100 ms that plays live, within
    // 10 ms of the first.
    const long W = 100 * ms;
    const std::size_t windows = static_cast<std::size_t>((end - start) / W + 1);
    std::vector<long> live_lag[2] = {std::vector<long>(windows, d_live), std::vector<long>(windows, d_live)};
    for (int p = 0; p < 2; ++p) {
        auto played = [&drums, p](long n) { return drums.player(p, n); };
        long last = d_live;
        for (std::size_t k = 0; k < windows; ++k) {
            const long a = start + static_cast<long>(k) * W, e = std::min(end, a + W);
            bool live = false;
            for (long bl = a / kBlock; bl < e / kBlock; ++bl) live = live || r[2].room[bl];
            if (e - a >= W / 2 && live) {
                double c = 0;
                const long d = bestLag(x, played, a, e, d_live - 10 * ms, d_live + 10 * ms, &c);
                if (c > 0.2) last = d;           // else the window has too little of this player's
            }
            live_lag[p][k] = last;
        }
    }
    auto liveLag = [&](int p, long f) {
        return live_lag[p][std::min(static_cast<std::size_t>((f - start) / W), windows - 1)];
    };

    // After a gap each player plays on the delay it had before it (16.6),
    // so alice's kick and snare stay in time with bob's hats: measured over
    // 800 ms before each gap and 800 ms from half a second after it.
    for (int k = 0; k < 5; ++k) {
        const long g = gap_at[k], g1 = g + gaps[k] * ms;
        long before[2], after[2];
        for (int p = 0; p < 2; ++p) {
            auto played = [&drums, p](long n) { return drums.player(p, n); };
            before[p] = bestLag(x, played, g - 900 * ms, g - 100 * ms, d_live - 10 * ms, d_live + 10 * ms);
            after[p] = bestLag(x, played, g1 + 500 * ms, g1 + 1300 * ms, d_live - 10 * ms, d_live + 10 * ms);
        }
        std::printf("    the %3d ms gap: alice %.1f ms late before it, %.1f after; bob %.1f and %.1f\n", gaps[k],
                    r.ms(before[0]), r.ms(after[0]), r.ms(before[1]), r.ms(after[1]));
        const bool calm = r.calm(g - 1000 * ms, g1 + 1400 * ms);
        for (int p = 0; p < 2; ++p)
            if (calm) CHECK(std::labs(after[p] - before[p]) <= ms);
        if (!calm) std::printf("      (not checked: the machine had a hiccup)\n");
    }

    const long B = o.rate / 200;
    auto own = [&](long f, long da, long db) {   // the drums played straight, within 5 ms of the block
        double v = 0, x1 = 0, x2 = 0;
        for (long n = f - 5 * ms - 2; n < f + B + 5 * ms; ++n) {
            const double y = drums.player(0, n - da) + drums.player(1, n - db);
            if (n >= f - 5 * ms) v = std::max(v, std::fabs(y - 2 * x1 + x2));
            x2 = x1, x1 = y;
        }
        return v;
    };
    auto onset = [&](long f, long d) { return drums.hitIn(f - d - ms, f - d + B); };   // in the block, or 1 ms before it
    std::vector<ClickBlock> blocks = clickBlocks(x, start, end, o.rate);
    struct Scored {
        ClickBlock c;
        double own;                              // the drums played straight, through the paths that play
    };
    std::vector<Scored> scored;
    int over = 0, skipped = 0, at_onsets = 0;
    for (const ClickBlock& c : blocks) {
        bool live = false, intervals = false;
        for (long b = (c.frame - 30 * ms) / kBlock; b <= (c.frame + B + 30 * ms) / kBlock; ++b)
            (r[2].room[b] ? live : intervals) = true;
        const long da = liveLag(0, c.frame), db = liveLag(1, c.frame);
        const double mine =
            std::max(live ? own(c.frame, da, db) : 0.0, intervals ? own(c.frame, d_int[0], d_int[1]) : 0.0);
        if (!r.calm(c.frame - 100 * ms, c.frame + 100 * ms)) {
            ++skipped;
            continue;
        }
        if ((live && (onset(c.frame, da) || onset(c.frame, db))) ||
            (intervals && (onset(c.frame, d_int[0]) || onset(c.frame, d_int[1])))) {
            if (c.d2 > 3 * std::max(c.median, mine)) ++at_onsets;
            continue;
        }
        scored.push_back({c, mine});
        if (c.d2 > 3 * std::max(c.median, mine)) {
            ++over;
            std::printf("    a click at %.3f s: %.5f against %.5f around it and %.5f in the drums played straight\n",
                        double(c.frame - start) / o.rate, c.d2, c.median, mine);
        }
    }
    auto against = [](const Scored& k) { return k.c.d2 / std::max(k.c.median, k.own); };   // the limit is 3
    std::sort(scored.begin(), scored.end(), [&](const Scored& a, const Scored& b) { return against(a) > against(b); });
    std::printf("    clicks: %zu blocks of 5 ms checked (%d not, for the machine's hiccups, and the ones where a hit "
                "starts, %d of them past the limit), %d over the limit; the five most against their surroundings "
                "and the drums played straight:\n",
                scored.size(), skipped, at_onsets, over);
    for (std::size_t i = 0; i < std::min<std::size_t>(5, scored.size()); ++i) {
        const Scored& k = scored[i];
        std::printf("      %7.3f s  x%.2f (%.5f against %.5f around it, %.5f in the drums played straight)\n",
                    double(k.c.frame - start) / o.rate, against(k), k.c.d2, k.c.median, k.own);
    }
    CHECK_EQ(over, 0);
}

// An argument runs only the cases whose names contain it.
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    if (argc > 1) {
        auto& c = testing::cases();
        c.erase(std::remove_if(c.begin(), c.end(),
                               [&](const testing::Case& k) { return std::string(k.name).find(argv[1]) == std::string::npos; }),
                c.end());
    }
    return testing::runAll();
}
