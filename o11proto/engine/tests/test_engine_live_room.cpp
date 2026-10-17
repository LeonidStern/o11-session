// Live mode for the whole room (o11proto/live_room.h, ENGINE.md 16.7 and
// 16.8), end to end in real time through o11server: the room goes Live and
// back as one, from the clients' health reports, and every switch is
// seamless. Each sender plays a steady sine of its own, and each listener's
// output is measured around every switch: the largest step against the
// steady tone's, the longest near-silence, and the 10 ms RMS against the
// steady RMS. The last case writes live-transitions.wav for listening.
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

// A client without Live (an older one): it can't do Live.
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
    int measured = 0, skipped = 0;
    double step = 0, silence = 0, low = 0, high = 0, level = 0;
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

// Both directions, at the boundary and at once, for one host rate and interval codec.
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

    // 1. Intervals to Live, at a boundary.
    const long b0 = r.block();
    REQUIRE(r.roomIs(true, 30));
    std::printf("    the room is Live %.1f s after connecting\n", r.secondsOf(r.block() - b0));
    for (int i = 0; i < 2; ++i) {
        const long s = measureSwitch(r, i, b0, false, label + " " + sideName(r, i) + ", intervals to Live", t);
        REQUIRE(s > 0);
        CHECK(r.boundaryIn(i, s / kBlock));
        if (r.calm(s - 1400 * ms, s + 1400 * ms)) levels.push_back(levelMatch(r, i, s, hz[1 - i]));
    }
    CHECK(!r[1].findNotice("The room is Live now, about ").empty());

    // 2. Live to intervals, at a boundary: an older client joins.
    Plain carol;
    const long b1 = r.block();
    REQUIRE(carol.join(r, "carol"));
    REQUIRE(r.roomIs(false, 6));
    for (int i = 0; i < 2; ++i) {
        const long s = measureSwitch(r, i, b1, true, label + " " + sideName(r, i) + ", Live to intervals", t);
        REQUIRE(s > 0);
        CHECK(r.boundaryIn(i, s / kBlock));
        std::printf("    %s on intervals %.2f s after carol joined\n", sideName(r, i).c_str(),
                    double(s - b1 * kBlock) / rate);
        if (r.calm(s - 1400 * ms, s + 1400 * ms)) levels.push_back(-levelMatch(r, i, s, hz[1 - i]));
    }
    CHECK(r.until([&] { return !r[1].findNotice("The room is on intervals: carol can't do Live.").empty(); }, 1));
    CHECK(r[0].e.liveRoomReason() == Engine::LiveReason::CantLive);
    CHECK(r[0].e.liveRoomReasonUser().find("carol") == 0);

    // 3. Intervals to Live again: it leaves.
    const long b2 = r.block();
    carol.leave(r);
    REQUIRE(r.roomIs(true, 15));
    for (int i = 0; i < 2; ++i) {
        const long s = measureSwitch(r, i, b2, false, label + " " + sideName(r, i) + ", intervals to Live", t);
        REQUIRE(s > 0);
        std::printf("    %s Live %.2f s after carol left\n", sideName(r, i).c_str(), double(s - b2 * kBlock) / rate);
        CHECK(double(s - b2 * kBlock) / rate >= 5.0);
    }

    // 4. Live to intervals at once: `!live off`, in the middle of an
    // interval (and with the room still Live: a hiccup of the machine can
    // have put it on intervals for a while).
    REQUIRE(r.until(
        [&] {
            int p = 0, len = 0;
            r[1].e.position(&p, &len);
            return r[0].e.liveRoomLive() && r[1].e.liveRoomLive() && len > 0 && p > len / 4 && p < len / 2;
        },
        30));
    const long b3 = r.block();
    CHECK(r[0].e.liveCommand("/live off"));
    REQUIRE(r.roomIs(false, 2));
    for (int i = 0; i < 2; ++i) {
        const long s = measureSwitch(r, i, b3, true, label + " " + sideName(r, i) + ", Live to intervals at once", t);
        REQUIRE(s > 0);
        CHECK(!r.boundaryIn(i, s / kBlock));
        std::printf("    %s on intervals %.0f ms after !live off\n", sideName(r, i).c_str(),
                    double(s - b3 * kBlock) * 1000 / rate);
    }
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

// Four players: everyone switches at its own next boundary, so all within
// one interval of each other, each one seamless.
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
    const long interval = 2L * o.rate;
    auto all = [&](long from, bool was, const char* what) {
        long lo = -1, hi = -1;
        for (int i = 0; i < 4; ++i) {
            const long s = measureSwitch(r, i, from, was, std::string(what) + ", " + sideName(r, i), t);
            if (s < 0) continue;
            CHECK(r.boundaryIn(i, s / kBlock));
            lo = lo < 0 ? s : std::min(lo, s);
            hi = std::max(hi, s);
        }
        std::printf("    %s: the four switched within %.0f ms of each other (an interval is %.0f ms)\n", what,
                    r.ms(hi - lo), r.ms(interval));
        CHECK(lo >= 0 && hi - lo <= interval);
    };
    const long b0 = r.block();
    REQUIRE(r.roomIs(true, 30));
    all(b0, false, "intervals to Live");
    Plain eve;
    const long b1 = r.block();
    REQUIRE(eve.join(r, "eve"));
    REQUIRE(r.roomIs(false, 6));
    all(b1, true, "Live to intervals");
    const long b2 = r.block();
    eve.leave(r);
    REQUIRE(r.roomIs(true, 15));
    all(b2, false, "intervals to Live again");
    t.print("room of four");
}

// Requirement: a player whose host is stopped sends nothing; that is no
// one's fault, so it doesn't keep the room from Live, and starting and
// stopping again doesn't move it.
TEST(stopped_daw_does_not_block_live) {
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
    std::size_t seen = r[0].noticeCount();
    r[0].e.liveCommand("/live");
    const std::string line = r[0].findNotice("dave: ", seen);
    std::printf("    %s\n", line.c_str());
    CHECK(line.find("dave: not sending") == 0);
    for (int step = 0; step < 2; ++step) {
        const long from = r.block();
        r[2].e.setLiveHostState(step == 0, false);   // it starts, then stops again
        r.wait(4);
        const bool calm = r.calm(from * kBlock, r.frames.load());
        bool stayed = true;
        for (int i = 0; i < 3; ++i) stayed = stayed && r.roomSwitch(i, from, true) < 0;
        std::printf("    dave's host %s: the room %s Live for 4 s%s\n", step == 0 ? "starts" : "stops",
                    stayed ? "stays" : "doesn't stay", calm ? "" : " (the machine had a hiccup)");
        if (calm) CHECK(stayed);
    }
}

// Requirement: a client with Live off keeps the room on intervals, and the
// others are told who.
TEST(client_without_live_keeps_room_on_intervals) {
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
    const long b0 = r.block();
    r.wait(12);
    bool never = true;
    for (int i = 0; i < 2; ++i) never = never && r.roomSwitch(i, b0, false) < 0 && !r[i].e.liveRoomLive();
    CHECK(never);
    for (int i = 0; i < 2; ++i) {
        const std::string n = r[i].findNotice("can't do Live");
        std::printf("    %s: %s\n", r[i].name.c_str(), n.c_str());
        CHECK(n == "The room is on intervals: carol can't do Live.");
        CHECK(r[i].e.liveRoomReason() == Engine::LiveReason::CantLive);
        CHECK(r[i].e.liveRoomReasonUser().find("carol") == 0);
    }
    const std::size_t seen = r[0].noticeCount();
    r[0].e.liveCommand("/live");
    std::printf("    %s\n", r[0].findNotice("The room", seen).c_str());
    CHECK(!r[0].findNotice("The room is on intervals: carol can't do Live.", seen).empty());
    // It leaves: Live after the 5 s.
    const long b1 = r.block();
    r[2].e.disconnect();
    REQUIRE(r.roomIs(true, 15, {0, 1}));
    std::printf("    Live %.1f s after carol left\n", r.secondsOf(r.block() - b1));
}

// The health rule (16.7): a single gap of up to a second fades out and comes
// back and the room stays Live; three gaps within 2 s, or one longer than a
// second, put it on intervals.
TEST(gaps_and_the_health_rule) {
    Options o;
    Room r(o, {"alice", "bob"});
    r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
    r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    r.wait(1);
    const long ms = o.rate / 1000;

    // One gap of 250 ms in alice's packets.
    const long g = r.frames.load();
    r[0].e.setLiveTestSendGap(250);
    r.wait(4);
    const bool calm = r.calm(g - 500 * ms, r.frames.load());
    bool stayed = r.roomSwitch(0, g / kBlock, true) < 0 && r.roomSwitch(1, g / kBlock, true) < 0;
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
                "step x%.2f of the steady tone's; the room %s Live%s\n",
                r.ms(quiet_from - g), r.ms(quiet_to - g), back_step / std::max(1e-9, st.step),
                stayed ? "stays" : "doesn't stay", calm ? "" : " (the machine had a hiccup)");
    if (calm) {
        CHECK(stayed);
        CHECK(quiet_from > g && quiet_to > quiet_from);
        CHECK(back_step <= 1.5 * st.step);
    }

    // Three gaps of 150 ms within 2 s.
    const long g3 = r.block();
    for (int k = 0; k < 3; ++k) {
        r[0].e.setLiveTestSendGap(150);
        r.wait(0.6);
    }
    REQUIRE(r.roomIs(false, 6));
    long s = 0;
    r.roomSwitch(1, g3, true, &s);
    std::printf("    three gaps within 2 s: on intervals %.2f s after the first; \"%s\"\n", double(s - g3 * kBlock) / o.rate,
                r[1].findNotice("The room is back on intervals").c_str());
    CHECK(r[1].e.liveRoomReason() == Engine::LiveReason::Dropouts);
    CHECK(r[1].e.liveRoomReasonUser().find("alice") == 0);

    // Live again after 5 s clean, then one gap of 1.5 s.
    const long b4 = r.block();
    REQUIRE(r.roomIs(true, 20));
    std::printf("    Live again %.1f s after the third gap\n", r.secondsOf(r.block() - b4));
    r.wait(1);
    const long g5 = r.block();
    r[0].e.setLiveTestSendGap(1500);
    REQUIRE(r.roomIs(false, 6));
    r.roomSwitch(1, g5, true, &s);
    std::printf("    a 1.5 s gap: on intervals %.2f s after it began\n", double(s - g5 * kBlock) / o.rate);
}

// A slot lost in the middle of an interval while the room is Live:
// concealment, a 20 ms fade to silence, silence (no interval audio), then
// at the boundary where the room switches, the intervals fade in.
TEST(slot_lost_mid_interval) {
    Options o;
    Room r(o, {"alice", "bob"});
    r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
    r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    r.until(
        [&] {
            int p = 0, len = 0;
            r[1].e.position(&p, &len);
            return len > 0 && p > len / 4 && p < len / 2;
        },
        3);
    const long ms = o.rate / 1000;
    const long g = r.frames.load();
    r[0].e.setLiveTestSendGap(4000);
    REQUIRE(r.roomIs(false, 8, {1}));
    long s = 0;
    REQUIRE(r.roomSwitch(1, g / kBlock, true, &s) > 0);
    r.until([&] { return r.frames.load() > s + 700 * ms; }, 3);
    const float* x = r[1].heard.get();
    const Steady live = steady(x, g - 500 * ms, g);
    // The fade: the 2 ms RMS from -1 dB of the steady RMS to silence.
    long down = -1, quiet = -1;
    for (long i = g; i < s && quiet < 0; i += ms / 2) {
        const double rms = steady(x, i, i + 2 * ms).rms;
        if (down < 0 && rms < live.rms * 0.891) down = i;
        if (rms < 0.001) quiet = i;
    }
    // Anything heard between the end of the fade and the switch: from the
    // buffer, the concealment and the fade on (the concealment itself may
    // have gone quiet before).
    const double B = r[1].e.liveBufferInUseMs();
    const long faded = g + static_cast<long>((B + kLiveConcealBuffers * B + kLiveFadeMs + 20) * ms);
    double loud = 0;
    long loud_at = faded;
    for (long i = std::max(quiet + 2 * ms, faded); quiet > 0 && i < s; ++i)
        if (std::fabs(x[i]) > loud) {
            loud = std::fabs(x[i]);
            loud_at = i;
        }
    const Transition in = transition(x, s, o.rate);
    const bool calm = r.calm(g - 600 * ms, s + 700 * ms);
    std::printf("    alice's packets stop: bob's level falls 1 dB %.0f ms later and is silent %.0f ms later; silent "
                "%.2f s, the loudest sample from %.0f ms on %.5f (at %.0f ms); the room switches %.2f s after the gap began\n",
                r.ms(down - g), r.ms(quiet - g), r.ms(s - quiet) / 1000, r.ms(faded - g), loud, r.ms(loud_at - g),
                r.ms(s - g) / 1000);
    std::printf("    the intervals come in: largest step x%.2f of the steady tone's, level %+.2f dB against Live; "
                "10 ms RMS from 20 ms on %+.2f dB at the least%s\n",
                in.step / std::max(1e-9, in.after.step), dB(in.after.rms / live.rms),
                dB(steady(x, s + 20 * ms, s + 30 * ms).rms / in.after.rms), calm ? "" : " (the machine had a hiccup)");
    if (calm) {
        CHECK(down > g && quiet > down);
        CHECK(r.ms(quiet - g) <= 250);          // the buffer, 80 ms of concealment, the 20 ms fade
        CHECK(loud < 0.001);                     // no interval audio while the room is Live
        CHECK(in.step <= 1.5 * in.after.step);
        CHECK(std::fabs(dB(in.after.rms / live.rms)) <= 0.5);
        CHECK(std::fabs(dB(steady(x, s + 20 * ms, s + 30 * ms).rms / in.after.rms)) <= 1.0);
    }
}

// 16.2: what a UI reads from the control context. liveServerHas() and
// liveRoomSwitchOn() follow the latest SERVER_LIVE, and liveHeardMs() is the
// N of "The room is Live now, about N ms late." while the room is Live.
TEST(accessors_for_a_ui) {
    {
        Options o;
        Room r(o, {"alice", "bob"});
        r[0].signal = [](long n, int rt) { return tone(n, rt, 500); };
        r[1].signal = [](long n, int rt) { return tone(n, rt, 700); };
        CHECK(!r[1].e.liveServerHas() && !r[1].e.liveRoomSwitchOn() && r[1].e.liveHeardMs() == -1);
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
        CHECK(r[0].e.liveCommand("/live off"));
        CHECK(r.until([&] { return !r[1].e.liveRoomSwitchOn(); }, 5));
        CHECK(r.roomIs(false, 5));
        std::printf("    after !live off: has %d, switch %d, heard %d\n", r[1].e.liveServerHas(),
                    r[1].e.liveRoomSwitchOn(), r[1].e.liveHeardMs());
        CHECK(r[1].e.liveServerHas());
        CHECK(r[1].e.liveHeardMs() == -1);
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

}  // namespace

TEST(listening_file) {
    Options o;
    o.sides = 3;
    // The engines' interval clocks start after the first host block, so the
    // pattern starts there: each bar is an interval, and its first kick
    // falls on the boundary.
    const Drums drums(o.rate, o.bpm, kBlock);
    Room r(o, {"alice", "bob", "carl"});
    std::printf("    the drums peak at %.1f dBFS\n", 20 * std::log10(drums.peak));
    CHECK(drums.peak < 0.5f);                    // under -6 dBFS
    r[0].signal = [&drums](long n, int) { return drums.player(0, n); };
    r[1].signal = [&drums](long n, int) { return drums.player(1, n); };
    LocalChannel lc;                             // carl listens: he sends nothing
    lc.name = "listen";
    lc.mute = true;
    r[2].e.setLocalChannel(0, lc);
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30, {2}));
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
    ev.push_back({start, "the room plays intervals"});
    ev.push_back({live_at, "the room goes Live"});
    const int gaps[5] = {20, 50, 80, 120, 250};
    auto waitUntil = [&](long frame) { r.until([&] { return r.frames.load() >= frame; }, 30); };
    // Each gap starts a 16th after a beat, where alice plays no hit and her
    // kick is decaying. (One that starts right after an attack makes
    // libopus's concealment repeat the attack once a pitch period, a buzz of
    // the codec's own that isn't measured here.)
    const long beat = 4 * drums.step, mid = drums.step;
    for (int k = 0; k < 5; ++k) {
        const long t = live_at + static_cast<long>((1.5 + 2.0 * k) * o.rate);
        waitUntil(t + ((mid - t) % beat + beat) % beat);
        const long f = r.frames.load();
        r[0].e.setLiveTestSendGap(gaps[k]);
        ev.push_back({f, "a " + std::to_string(gaps[k]) + " ms gap in alice's packets (sent now, heard about " + late +
                             " ms later)"});
    }
    waitUntil(live_at + static_cast<long>(11.5 * o.rate));
    Plain dave;
    const long b = r.block();
    ev.push_back({r.frames.load(), "dave, a client without Live, joins"});
    REQUIRE(dave.join(r, "dave"));
    REQUIRE(r.roomIs(false, 6, {2}));
    long back = 0;
    r.roomSwitch(2, b, true, &back);
    ev.push_back({back, "the room is back on intervals (dave can't do Live)"});
    r.wait(1);
    const long b2 = r.block();
    ev.push_back({r.frames.load(), "dave leaves"});
    dave.leave(r);
    REQUIRE(r.roomIs(true, 15, {2}));
    long again = 0;
    r.roomSwitch(2, b2, false, &again);
    ev.push_back({again, "the room is Live again"});
    waitUntil(again + 3L * o.rate / 2);
    const long end = r.frames.load();
    const long first = r.roomSwitch(2, live_at / kBlock + 1, true);   // the room stayed Live through the gaps
    std::printf("    the gaps %s the room Live\n", first >= b ? "kept" : "didn't keep");
    if (first < b) {
        std::printf("    it went on intervals %.3f s from the start:\n", double(first * kBlock - start) / o.rate);
        for (int i = 0; i < 3; ++i) r[i].printNotices();
    }
    CHECK(first >= b || !r.calm(live_at, first * kBlock));   // unless the machine had a hiccup by then

    const std::string wav = "live-transitions.wav", txt = "live-transitions.txt";
    CHECK(writeWav(wav, r[2].heard.get() + start, end - start, o.rate));
    float heard_peak = 0;
    for (long n = start; n < end; ++n) heard_peak = std::max(heard_peak, std::fabs(r[2].heard[n]));
    std::printf("    what carl hears peaks at %.1f dBFS\n", 20 * std::log10(heard_peak));
    CHECK(heard_peak < 0.5f);
    std::FILE* f = std::fopen(txt.c_str(), "w");
    REQUIRE(f);
    std::fprintf(f, "%s: what carl hears, 48 kHz mono, %.1f s. The music is 909-style drums, synthesized,\n"
                    "at %d bpm, one bar per interval: alice plays the kick and the snare, bob the closed and open\n"
                    "hi-hats and the clap; carl sends nothing. A slot without audio is concealed for %.0f x B =\n"
                    "%.0f ms, then fades out over %.0f ms; every switch crossfades over %.0f ms. The first sample is\n"
                    "frame %ld of the players' clock. Times in seconds from the start.\n\n",
                 wav.c_str(), double(end - start) / o.rate, o.bpm, kLiveConcealBuffers,
                 kLiveConcealBuffers * r[2].e.liveBufferInUseMs(), kLiveFadeMs, kLiveFadeMs, start);
    for (const Event& e : ev) std::fprintf(f, "%7.3f  %s\n", double(e.frame - start) / o.rate, e.text.c_str());
    std::fclose(f);
    std::printf("    wrote %s (%.1f s) and %s:\n", wav.c_str(), double(end - start) / o.rate, txt.c_str());
    for (const Event& e : ev) std::printf("      %7.3f  %s\n", double(e.frame - start) / o.rate, e.text.c_str());

    // Clicks: no 5 ms block may bend the sound more than 3 times as much as
    // its surroundings do (the median within 200 ms) and as the same drums
    // played straight do there, heard through the path or paths that play
    // then, live or an interval later. The drums' own count within 5 ms of
    // the block, since the codecs spread an attack by a few ms (before it
    // too, as pre-echo), and a block where a hit starts is exempt: how a
    // codec renders an attack depends on how it falls on its frames.
    const float* x = r[2].heard.get();
    const long L = intervalLengthFor(o.bpm, o.bpi, o.rate), ms = o.rate / 1000;
    auto material = [&](long n) { return drums.mix(n); };
    const long d_live = bestLag(x, material, live_at + 300 * ms, live_at + 1300 * ms, 0, 100 * ms);
    const long d_int = bestLag(x, material, start + 100 * ms, live_at - 100 * ms, L - 50 * ms, L + 50 * ms);
    std::printf("    carl hears the players %.1f ms late live, %.1f ms through intervals\n", r.ms(d_live), r.ms(d_int));
    // carl hears each player through a sink of its own, whose delay moves by
    // a host block or so when the player's audio comes back after a gap and
    // the buffer fills again. So each player's live delay is measured again
    // for every 100 ms that plays live, within 10 ms of the first.
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
        const double mine = std::max(live ? own(c.frame, da, db) : 0.0, intervals ? own(c.frame, d_int, d_int) : 0.0);
        if (!r.calm(c.frame - 100 * ms, c.frame + 100 * ms)) {
            ++skipped;
            continue;
        }
        if ((live && (onset(c.frame, da) || onset(c.frame, db))) || (intervals && onset(c.frame, d_int))) {
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
    if (first >= b || r.calm(live_at, first * kBlock))
        CHECK_EQ(over, 0);
    else                                         // dropouts of the machine's own made the room fall back
        std::printf("    the clicks aren't checked: the machine's hiccups put the room on intervals\n");
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
