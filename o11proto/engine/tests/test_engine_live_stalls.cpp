// This host's gaps end to end (ENGINE.md 16.6, 6.9), in real time through
// o11server: two players on one host thread with 128-frame blocks and the
// automatic buffer's first size, 11 ms (fixed, so that no fallback changes
// a delay: a hole in the sender's audio makes the block its host's first
// block after the stall completes late, and two late dropouts within 10 s
// fall back to the larger buffer, a case of its own), in rooms with Live
// frames of 120 and of 240. The host
// stops calling one player's process(), or both players', for a while, as
// a host that stalls does; each player's engine finds its own gap, and the
// sender's xrun blocks tell the listener of its gap (6.4). A stall that
// both hosts miss alike is not heard: nothing concealed, dry or skipped, no
// dropout, no advice, and each listener's delay after it is the one before
// (to a host block). A stall of one host alone: the listener there skips
// as much of the stream as its host missed, so its delay stays; the
// listener at the other host conceals the hole. Stalls in between (of
// different lengths, or a block apart) conceal no more than the sender's
// audio misses and skip no more than the delay needs. Each player plays
// noise of its own, so that each listener's delay is found by correlation.
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
struct Case {
    const char* what;
    Stall s[2];                                  // alice's host, bob's
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
        const long end = (at + longest + 330) * B;   // ~0.9 s after the stall
        r.until([&] { return r.frames.load() > end; }, 5);
        for (int i = 0; i < 2; ++i) r[i].stall_at.store(-1);
        const long before_from = (at - 200) * B, before_to = (at - 20) * B;
        const long after_from = (at + longest + 130) * B, after_to = (at + longest + 310) * B;
        for (int l = 0; l < 2; ++l) {
            LiveCounters c1;
            counters(l, &c1);
            v[l].d = minus(c1, c0[l]);
            v[l].before = lag(l, before_from, before_to, 0, 9600, &v[l].corr_before);
            v[l].after = lag(l, after_from, after_to, std::max(0L, v[l].before - 960), v[l].before + 960, &v[l].corr_after);
            const double stall_ms = longest * 1000.0 * B / r.opt.rate;
            for (const LiveEvent& e : r[l].e.liveUserEvents(u[l])) {
                if (e.steady_ns < at_ns - 1500000000LL) continue;
                const double t = (e.steady_ns - at_ns) / 1e6;
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
    if (v.stray) std::printf("        a dropout away from the stall (the machine's): not checked\n");
    (void)frame;
}

void stalls(int frame) {
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
    r.wait(2.5);                                 // the automatic buffer settles
    {
        const long now = r.frames.load();
        for (int l = 0; l < 2; ++l) {
            double c = 0;
            const long d = p.lag(l, now - 24000, now - 4800, 0, 9600, &c);
            std::printf("    frame %d: %s hears %s %.1f ms late (correlation %.2f)\n", frame, r[l].name.c_str(),
                        r[1 - l].name.c_str(), d / 48.0, c);
            REQUIRE(c > 0.6);
        }
    }
    const Case cases[] = {
        {"both hosts miss 32 ms", {{0, 12}, {0, 12}}},
        {"both hosts miss 80 ms", {{0, 30}, {0, 30}}},
        {"both hosts miss 32 ms, bob's a block later", {{0, 12}, {1, 12}}},
        {"both hosts miss 32 ms, alice's two blocks later", {{2, 12}, {0, 12}}},
        {"alice's host misses 32 ms", {{0, 12}, {0, 0}}},
        {"bob's host misses 45 ms", {{0, 0}, {0, 17}}},
        {"alice's host misses 32 ms, bob's 45 ms", {{0, 12}, {0, 17}}},
        {"alice's host misses 53 ms, bob's 21 ms within it", {{0, 20}, {6, 8}}},
    };
    for (const Case& c : cases) {
        r.wait(2.0);                             // the last case's dropouts are 2 s old (6.4)
        const bool advice_before = !noAdvice(r[0]) || !noAdvice(r[1]);
        View v[2];
        const bool calm = p.run(c, v);
        std::printf("    frame %d, %s%s\n", frame, c.what, calm ? "" : " (the machine had a hiccup: not checked)");
        for (int l = 0; l < 2; ++l) {
            char who[64];
            std::snprintf(who, sizeof(who), "%s hears %s", r[l].name.c_str(), r[1 - l].name.c_str());
            print(who, frame, v[l]);
        }
        if (!calm) continue;
        for (int l = 0; l < 2; ++l) {
            if (v[l].stray) continue;
            // Listener l, its host's stall `mine`, the sender's `theirs`.
            const Stall mine = c.s[l], theirs = c.s[1 - l];
            const long missed = mine.blocks * 128L, sent = theirs.blocks * 128L;
            const long m0 = mine.from * 128L, m1 = m0 + missed, s0 = theirs.from * 128L, s1 = s0 + sent;
            const long both = mine.blocks && theirs.blocks ? std::max(0L, std::min(m1, s1) - std::max(m0, s0)) : 0;
            const View& w = v[l];
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
                // host's gaps changed. In real time, and so in the cases
                // below, a little more: the frames its host's first block
                // after the stall completes (one or two) come after their
                // turn, late, and the xrun blocks after them.
                CHECK_EQ(w.d.xruns, 1);
                CHECK(w.d.concealed >= sent - frame && w.d.concealed <= sent + 3 * frame);
                CHECK(std::labs(w.after - w.before) <= frame + 128);
            } else {
                // 4: in between.
                CHECK(w.d.concealed <= std::max(0L, sent - missed) + 3 * frame);
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
}

}  // namespace

TEST(host_stalls_frame_120) { stalls(120); }
TEST(host_stalls_frame_240) { stalls(240); }

int main() {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
