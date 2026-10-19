// This host's gaps end to end (ENGINE.md 16.6, 6.9), in real time through
// o11server: two players on one host thread with 128-frame blocks and the
// automatic buffer's first size, 11 ms (fixed, so that no fallback changes
// a delay: a hole in the sender's audio makes the block its host's first
// block after the stall completes late, and two late dropouts within 10 s
// fall back to the larger buffer, a case of its own), in rooms with Live
// frames of 120 and of 240, each case in a room of its own. The host
// stops calling one player's process(), or both players', for a while, as
// a host that stalls does; each player's engine finds its own gap, and the
// sender's xrun blocks tell the listener of its gap (6.4). A stall that
// both hosts miss alike is not heard: nothing concealed, dry or skipped, no
// dropout, no advice, and each listener's delay after it is the one before
// (to a host block). A stall of one host alone: the listener there skips
// as much of the stream as its host missed, so its delay stays; the
// listener at the other host conceals the hole. Stalls in between (of
// different lengths, or a block apart) conceal no more than the sender's
// audio misses and skip no more than the delay needs. A callback a little
// late next to a stall puts that host's measure of its gap off: the
// sender's xrun blocks a frame fewer or more than the stall's. Each player
// plays noise of its own, so that each listener's delay is found by
// correlation.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "live_harness.h"

using namespace harness;

namespace {

// Noise of its own for each player (`seed`), averaged over 6 samples: a
// sound whose delay has one sharp correlation.
float noise(long n, std::uint32_t seed) {
    float s = 0;
    for (int k = 0; k < 6; ++k) {
        std::uint32_t x = static_cast<std::uint32_t>(n - k) * 2654435761u ^ seed;
        x ^= x >> 15;
        x *= 2246822519u;
        x ^= x >> 13;
        x *= 3266489917u;
        x ^= x >> 16;
        s += static_cast<float>(x) / 4294967296.0f - 0.5f;
    }
    return 0.15f * s;
}
const std::uint32_t kSeed[2] = {0x9e3779b9u, 0x7f4a7c15u};

int userIndex(Side& s, const std::string& name) {
    for (int u = 0; u < s.e.numUsers(); ++u)
        if (s.e.userName(u).find(name) == 0) return u;
    return -1;
}
bool noAdvice(Side& s) {
    return s.e.liveAdviceReason() == Engine::LiveAdvice::None && s.e.liveAdviceUser().empty() &&
           s.e.liveAdviceCount() == 0;
}

// What a listener's stream of one sender counted over a stall.
struct Delta {
    long concealed = 0, underruns = 0, overflows = 0, xruns = 0, late = 0, lost = 0, skips = 0, host_gaps = 0,
         host_gap_frames = 0;
};
Delta minus(const LiveCounters& a, const LiveCounters& b) {
    Delta d;
    d.concealed = static_cast<long>(a.concealed_frames - b.concealed_frames);
    d.underruns = static_cast<long>(a.underruns) - static_cast<long>(b.underruns);
    d.overflows = static_cast<long>(a.overflows) - static_cast<long>(b.overflows);
    d.xruns = static_cast<long>(a.xrun_gaps) - static_cast<long>(b.xrun_gaps);
    d.late = static_cast<long>(a.late) - static_cast<long>(b.late);
    d.lost = static_cast<long>(a.lost) - static_cast<long>(b.lost);
    d.skips = static_cast<long>(a.skips) - static_cast<long>(b.skips);
    d.host_gaps = static_cast<long>(a.host_gaps) - static_cast<long>(b.host_gaps);
    d.host_gap_frames = static_cast<long>(a.host_gap_frames - b.host_gap_frames);
    return d;
}

struct Stall {
    int from = 0, blocks = 0;                    // from the stall's first block on (0 blocks: none)
};
// One host's call a little late, so that its measure of its gap is off
// (live_harness.h, Side::call_late_at): `side`'s call of the block `block`
// blocks from the stall's first (-1: the one before it), `us` µs late.
struct Late {
    int side = -1, block = 0, us = 0;
};
struct Case {
    const char* what;
    Stall s[2];                                  // alice's host, bob's
    Late late = {};
};

// One listener's view of one stall: listener `l` hears sender `1 - l`.
struct View {
    Delta d;
    long before = 0, after = 0;
    double corr_before = 0, corr_after = 0;
    std::string log;                             // the stream's events from the stall on (ms after it began)
    // A dropout away from the stall (in the 1.5 s before it, or more than
    // 150 ms after it ended): the machine's, and the counts aren't the
    // stall's alone.
    bool stray = false;
    // The stall's own dropout runs from its first dropout event on to the
    // rejoin after it, and the xrun blocks played then: `stall_late`, the
    // frames that came late in it. A dropout that starts after that (a run
    // dry, a block lost or late) is one of its own: a frame held up on its
    // way (a thread woken late, under calm()'s 10 ms), which the stall's
    // move of the delay (lower by what the xrun blocks fall short of it)
    // may have made late. Its late frames are counted apart, `later_late`,
    // when it is a plain one, closed by a rejoin; after an overflow, a
    // rebuffer, a skip or an xrun in it, or with the counts and the log
    // apart, all are the stall's. What it concealed is not: a late frame's
    // turn was concealed, so concealed less the late frames, all of them,
    // nets it out.
    long stall_late = 0, later = 0, later_late = 0;
    // The sender's blocks from the first after its stall to the one that
    // reported its gap (6.9: the first that didn't come early), and the
    // most frames they complete: the frames it sent before its xrun blocks.
    long report_blocks = 0, report_frames = 0;
};

struct Players {
    Room& r;
    int frame;
    int u[2] = {-1, -1};                         // at listener l, the sender's user index
    explicit Players(Room& room, int f) : r(room), frame(f) {}

    bool counters(int l, LiveCounters* c) { return u[l] >= 0 && r[l].e.liveUserCounters(u[l], c); }
    long lag(int l, long from, long to, long lo, long hi, double* corr) {
        const std::uint32_t seed = kSeed[1 - l];
        return bestLag(r[l].heard.get(), [seed](long n) { return noise(n, seed); }, from, to, lo, hi, corr);
    }
    // The sender's (side `s`) blocks from `e`, its first after its stall,
    // up to the first that came at least 90 % of a block after the one
    // before it, so the one that reported its gap (6.9). A block that
    // comes earlier (its host caught up on a late wake, or its callback
    // jittered) puts the report off by a block, and the frames that block
    // completes go before the xrun blocks. Read by this clock, a block
    // within 0.1 ms of the line counts as early: the engine reads its own.
    long reportBlocks(int s, long e) {
        const std::int64_t dur = r.opt.block * 1000000000LL / r.opt.rate;
        const std::int64_t* called = r[s].called.get();
        long j = e + 1;
        while (j < std::min(e + 64, r.max_blocks) && called[j] > 0 && called[j - 1] > 0 &&
               called[j] - called[j - 1] < dur * 9 / 10 + 100000)
            ++j;
        return j - e;
    }
    // Runs one case; false when the machine had a hiccup around it.
    bool run(const Case& c, View v[2]) {
        const long B = r.opt.block;
        const long now_block = r.block();
        const long at = now_block + 60;          // 160 ms from now
        const std::int64_t at_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() +
            (at - now_block) * B * 1000000000LL / r.opt.rate;
        int longest = 0;
        for (int i = 0; i < 2; ++i) longest = std::max(longest, c.s[i].from + c.s[i].blocks);
        LiveCounters c0[2];
        for (int l = 0; l < 2; ++l) counters(l, &c0[l]);
        for (int i = 0; i < 2; ++i) {
            if (c.s[i].blocks == 0) continue;
            r[i].stall_len.store(c.s[i].blocks);
            r[i].stall_at.store(at + c.s[i].from, std::memory_order_release);
        }
        if (c.late.side >= 0) {
            r[c.late.side].call_late_us.store(c.late.us);
            r[c.late.side].call_late_at.store(at + c.late.block, std::memory_order_release);
        }
        const long end = (at + longest + 330) * B;   // ~0.9 s after the stall
        r.until([&] { return r.frames.load() > end; }, 5);
        for (int i = 0; i < 2; ++i) {
            r[i].stall_at.store(-1);
            r[i].call_late_at.store(-1);
        }
        const long before_from = (at - 200) * B, before_to = (at - 20) * B;
        const long after_from = (at + longest + 130) * B, after_to = (at + longest + 310) * B;
        for (int l = 0; l < 2; ++l) {
            LiveCounters c1;
            counters(l, &c1);
            v[l].d = minus(c1, c0[l]);
            v[l].before = lag(l, before_from, before_to, 0, 9600, &v[l].corr_before);
            v[l].after = lag(l, after_from, after_to, std::max(0L, v[l].before - 960), v[l].before + 960, &v[l].corr_after);
            const double stall_ms = longest * 1000.0 * B / r.opt.rate;
            const Stall theirs = c.s[1 - l];
            if (theirs.blocks > 0) {
                v[l].report_blocks = reportBlocks(1 - l, at + theirs.from + theirs.blocks);
                v[l].report_frames = (frame - 1 + v[l].report_blocks * B) / frame;
            }
            int state = 0;                       // 0: before the stall's dropout, 1: in it, 2: after it, 3: in one of its own
            bool plain = true;
            for (const LiveEvent& e : r[l].e.liveUserEvents(u[l])) {
                if (e.steady_ns < at_ns - 1500000000LL) continue;
                const double t = (e.steady_ns - at_ns) / 1e6;
                if (t >= -5) {
                    const bool own = state >= 2;
                    if (own && state == 2 &&
                        (e.kind == LiveEventKind::Dry || e.kind == LiveEventKind::Lost || e.kind == LiveEventKind::Late)) {
                        state = 3;
                        ++v[l].later;
                    }
                    switch (e.kind) {
                    case LiveEventKind::Dry:
                    case LiveEventKind::Lost:
                        if (state == 0) state = 1;
                        break;
                    case LiveEventKind::Late:
                        if (state == 0) state = 1;
                        if (own) ++v[l].later_late;
                        else ++v[l].stall_late;
                        break;
                    case LiveEventKind::Rejoin:
                        if (state == 1 || state == 3) state = 2;
                        break;
                    case LiveEventKind::Xrun:
                        if (state == 3) plain = false;
                        break;
                    case LiveEventKind::Overflow:
                    case LiveEventKind::Rebuffer:
                    case LiveEventKind::Skip:
                        plain = false;
                        break;
                    default:
                        break;
                    }
                }
                switch (e.kind) {
                case LiveEventKind::Dry:
                case LiveEventKind::Lost:
                case LiveEventKind::Late:
                case LiveEventKind::Xrun:
                case LiveEventKind::Overflow:
                case LiveEventKind::Rebuffer:
                case LiveEventKind::Skip:
                    if (t < -5 || t > stall_ms + 150) v[l].stray = true;
                    break;
                default:
                    break;
                }
                if (t < -20) continue;
                char b[96];
                std::snprintf(b, sizeof(b), "%s%s %lld at %+.1f", v[l].log.empty() ? "" : "; ", liveEventName(e.kind),
                              static_cast<long long>(e.value), (e.steady_ns - at_ns) / 1e6);
                v[l].log += b;
            }
            if (state == 3 || v[l].stall_late + v[l].later_late != v[l].d.late) plain = false;
            if (!plain) {
                v[l].later = v[l].later_late = 0;
                v[l].stall_late = v[l].d.late;
            }
        }
        return r.calm(before_from - r.opt.rate / 2, r.frames.load());
    }
};

void print(const char* who, int frame, const View& v) {
    std::printf("      %s: delay %ld -> %ld frames (correlation %.2f, %.2f); concealed %ld, dry %ld, xruns %ld, late %ld, "
                "lost %ld, skips %ld, host gaps %ld (%ld frames skipped), overflows %ld\n",
                who, v.before, v.after, v.corr_before, v.corr_after, v.d.concealed, v.d.underruns, v.d.xruns, v.d.late,
                v.d.lost, v.d.skips, v.d.host_gaps, v.d.host_gap_frames, v.d.overflows);
    if (!v.log.empty()) std::printf("        the log: %s\n", v.log.c_str());
    if (v.report_blocks > 0)
        std::printf("        the sender reported its gap at its block %ld after the stall: up to %ld frames before its xrun "
                    "blocks; %ld came late in the stall's dropout\n",
                    v.report_blocks + 1, v.report_frames, v.stall_late);
    if (v.later > 0)
        std::printf("        then %ld dropout%s of its own (the machine's): %ld late, counted apart\n", v.later,
                    v.later == 1 ? "" : "s", v.later_late);
    if (v.stray) std::printf("        a dropout away from the stall (the machine's): not checked\n");
    (void)frame;
}

// Each case in a room of its own. A stall that one host alone misses moves
// a listener's delay: a listener whose sender's host stalled conceals the
// hole and the frames that came late before it (as before this host's gaps
// changed), and one whose own host stalled skips the time missed to the
// nearest block. The delay a listener is left with may be lower by as much
// as the xrun blocks fall short of the stall, which a frame held up on its
// way a little later (a thread woken a few ms late) may then find: a
// dropout of its own, counted apart (View). The speed then takes the
// delay back to its level over tens of seconds (6.9: 0.1 of the fill's
// error a second, past a 2 ms dead band).
// A case after it in the same room found the delay some ms up and still
// moving: a few hundred ppm slide it 5-10 frames over the half second its
// lag is found from, which smears the noise's correlation (sharp to a few
// frames) to 0.6 or less; and what each case left added up.
void stallCase(int frame, const Case& c) {
    Options o;
    o.block = 128;
    o.live_frame = frame;
    Room r(o, {"alice", "bob"});
    for (int i = 0; i < 2; ++i) {
        const std::uint32_t seed = kSeed[i];
        r[i].signal = [seed](long n, int) { return noise(n, seed); };
        r[i].e.setLiveBufferMs(11);              // the automatic buffer's at 128-frame blocks (6.8), fixed
    }
    r.startHost();
    REQUIRE(r.connect());
    REQUIRE(r.roomIs(true, 30));
    Players p(r, frame);
    REQUIRE(r.until([&] { return (p.u[0] = userIndex(r[0], "bob")) >= 0 && (p.u[1] = userIndex(r[1], "alice")) >= 0; }, 5));
    r.wait(2.5);                                 // the buffer settles
    {
        const long now = r.frames.load();
        for (int l = 0; l < 2; ++l) {
            double cr = 0;
            const long d = p.lag(l, now - 24000, now - 4800, 0, 9600, &cr);
            std::printf("    frame %d, %s: %s hears %s %.1f ms late (correlation %.2f)\n", frame, c.what,
                        r[l].name.c_str(), r[1 - l].name.c_str(), d / 48.0, cr);
            REQUIRE(cr > 0.6);
        }
    }
    r.wait(2.0);                                 // the buffering's dropouts, if any, are 2 s old (6.4)
    const bool advice_before = !noAdvice(r[0]) || !noAdvice(r[1]);
    View v[2];
    const bool calm = p.run(c, v);
    std::printf("    frame %d, %s%s\n", frame, c.what, calm ? "" : " (the machine had a hiccup: not checked)");
    for (int l = 0; l < 2; ++l) {
        char who[64];
        std::snprintf(who, sizeof(who), "%s hears %s", r[l].name.c_str(), r[1 - l].name.c_str());
        print(who, frame, v[l]);
    }
    if (!calm) return;
    for (int l = 0; l < 2; ++l) {
        if (v[l].stray) continue;
        // Listener l, its host's stall `mine`, the sender's `theirs`.
        const Stall mine = c.s[l], theirs = c.s[1 - l];
        const long missed = mine.blocks * 128L, sent = theirs.blocks * 128L;
        const long m0 = mine.from * 128L, m1 = m0 + missed, s0 = theirs.from * 128L, s1 = s0 + sent;
        const long both = mine.blocks && theirs.blocks ? std::max(0L, std::min(m1, s1) - std::max(m0, s0)) : 0;
        const View& w = v[l];
        // What is concealed less the frames that came late, all of them: a
        // dropout of its own after the stall nets out (View). And the frames
        // that came late in the stall's own dropout.
        const long net = w.d.concealed - w.d.late * frame, late = w.stall_late;
        CHECK(w.corr_before > 0.6 && w.corr_after > 0.6);
        CHECK_EQ(w.d.overflows, 0);
        CHECK_EQ(w.d.lost, 0);
        CHECK_EQ(w.d.skips, 0);
        CHECK_EQ(w.d.host_gaps, mine.blocks ? 1 : 0);
        if (mine.blocks && mine.blocks == theirs.blocks && both > 0) {
            // 1: alike, also a block or two apart: not heard.
            CHECK_EQ(w.d.concealed, 0);
            CHECK_EQ(w.d.underruns, 0);
            CHECK_EQ(w.d.xruns, 0);
            CHECK_EQ(w.d.late, 0);
            CHECK_EQ(w.d.host_gap_frames, 0);
            CHECK(std::labs(w.after - w.before) <= 128);
        } else if (mine.blocks && !theirs.blocks) {
            // 2: this host's alone: as much skipped, the delay kept.
            CHECK_EQ(w.d.concealed, 0);
            CHECK_EQ(w.d.underruns, 0);
            CHECK(std::labs(w.d.host_gap_frames - missed) <= frame / 2);
            CHECK(std::labs(w.after - w.before) <= frame / 2 + 128);
        } else if (!mine.blocks && theirs.blocks) {
            // 3: the sender's alone: the hole concealed, as before this
            // host's gaps changed: the sender's xrun blocks (5.4: whole
            // frames of the gap it measured, rounded down, the rest carried
            // over to its next gap) and, in real time, and so in the cases
            // below, the frames the sender sent before them: those its
            // host's blocks after the stall complete until one reports the
            // gap (one or two, more when blocks came early). They come
            // after their turn, late. The listener's delay moves by as much
            // as the xrun blocks fall short of the stall or pass it: what
            // is concealed less the late frames is the stall and the
            // delay's move, to a ms (the speed may slide the delay
            // meanwhile). The sender's measure is its clock's reading of
            // when its blocks began (6.9), off by as much as a callback
            // came late, up to a host block: its xrun blocks may be a frame
            // fewer than the stall's, or (with a carry) one more, and the
            // hole concealed and the delay's move up to a frame and a host
            // block short of it or past it (a callback under 1 ms late made
            // 8 of 240 frames, 1920, for the 2176 frames of 45 ms).
            CHECK_EQ(w.d.xruns, 1);
            CHECK(late <= w.report_frames);
            CHECK(std::labs(net - (sent + (w.after - w.before))) <= 48);
            CHECK(net >= sent - frame - 128 && net <= sent + frame + 128);
            CHECK(std::labs(w.after - w.before) <= frame + 128);
        } else {
            // 4: in between. What is concealed less the frames that came
            // late (as in 3) is no more than the sender's audio misses, to
            // the xrun blocks' whole frames and the hosts' measures.
            CHECK(late <= w.report_frames);
            CHECK(net <= std::max(0L, sent - missed) + 2 * frame);
            CHECK(w.d.host_gap_frames <= std::max(0L, missed - sent) + frame);
            CHECK(std::labs(w.after - w.before) <= frame + 2 * 128);
        }
    }
    // No dropout, so no advice (16.7).
    if (c.s[0].blocks && c.s[0].blocks == c.s[1].blocks && !advice_before && !v[0].stray && !v[1].stray) {
        const bool none = noAdvice(r[0]) && noAdvice(r[1]);
        std::printf("      advice: %s\n", none ? "none" : "some");
        CHECK(none);
    }
}

void stalls(int frame) {
    const Case cases[] = {
        {"both hosts miss 32 ms", {{0, 12}, {0, 12}}},
        {"both hosts miss 80 ms", {{0, 30}, {0, 30}}},
        {"both hosts miss 32 ms, bob's a block later", {{0, 12}, {1, 12}}},
        {"both hosts miss 32 ms, alice's two blocks later", {{2, 12}, {0, 12}}},
        {"alice's host misses 32 ms", {{0, 12}, {0, 0}}},
        {"bob's host misses 45 ms", {{0, 0}, {0, 17}}},
        {"alice's host misses 32 ms, bob's 45 ms", {{0, 12}, {0, 17}}},
        {"alice's host misses 53 ms, bob's 21 ms within it", {{0, 20}, {6, 8}}},
        // Bob's callback 1 ms late at the block before the stall: its
        // measure of the gap 1 ms short, its xrun blocks a frame fewer than
        // the stall's (45 ms: 16 frames over whole frames of 120 and 240),
        // as a callback a little late made them on a CI runner. At the
        // second block after the stall: 1 ms long, a frame more (35 ms:
        // 104 frames over whole frames of 120, 224 over those of 240).
        {"bob's host misses 45 ms, its measure 1 ms short", {{0, 0}, {0, 17}}, {1, -1, 1000}},
        {"bob's host misses 35 ms, its measure 1 ms long", {{0, 0}, {0, 13}}, {1, 13 + 1, 1000}},
        {"both hosts miss 45 ms, bob's measure 1 ms short", {{0, 17}, {0, 17}}, {1, -1, 1000}},
    };
    for (const Case& c : cases) stallCase(frame, c);
}

}  // namespace

TEST(host_stalls_frame_120) { stalls(120); }
TEST(host_stalls_frame_240) { stalls(240); }

int main() {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
