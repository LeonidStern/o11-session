// Live mode end to end (ENGINE.md section 16): two engines through
// o11server in real time. An audio thread calls both engines' process()
// every 270 frames at 48 kHz, as a host would; the main thread runs their
// control contexts and the server runs on its own thread. Each engine sends
// short tone bursts at uneven gaps on its channel 0 and finds the other's in
// its output: live they come tens of ms late, through intervals about an
// interval late.
//
// The cases: both hear each other live at low latency; `!live off` brings
// the intervals back (the mixer at once, their audio from the players' first
// interval after Live), and `!live on` brings Live back at a boundary;
// a sender's stall of 1.5 s and packet loss leave the room Live (16.7: its
// switch alone sets its mode) and give both players advice naming the
// sender, which clears once it has been clean for 10 s; `/live buffer 60`
// raises the delay.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "o11proto/engine.h"
#include "o11proto/realtime.h"
#include "o11proto/server.h"

using namespace o11proto;
using namespace o11proto::engine;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 270;                      // not a divisor of the interval (spec test 35)
constexpr long kMaxFrames = 160L * kRate;        // the most one room records
constexpr long kMaxOwn = kMaxFrames + kRate;     // a side's own frames: its clock may run ahead (Side::ppm)
constexpr long kBurst = kRate * 30 / 1000;
constexpr long kWindow = kRate * 300 / 1000;     // a burst is looked for this long after it is sent
constexpr double kAmp = 0.5;
constexpr double kHz = 1000.0;

// The burst starts: from 0.2 s on, at uneven gaps. Their pattern doesn't
// repeat within an interval, so an interval copy of a burst (2 s later)
// lands 220-280 ms into another burst's window, or after it.
std::vector<long> burstStarts() {
    static const double gaps[] = {0.35, 0.47, 0.38, 0.52, 0.41, 0.44, 0.37};
    std::vector<long> v;
    double t = 0.2;
    for (int i = 0; t < kMaxFrames / double(kRate); ++i) {
        v.push_back(static_cast<long>(t * kRate));
        t += gaps[i % 7];
    }
    return v;
}

struct Side {
    Engine e;
    std::string name;
    // A fixed 20 ms jitter buffer, the default before the automatic one:
    // these checks are made for it (the automatic buffer's are in
    // test_engine_live_units and test_engine_live_tone).
    Side() { e.setLiveBufferMs(20); }
    std::unique_ptr<float[]> heard{new float[kMaxOwn]()};             // output 0, at the side's own frames
    std::unique_ptr<std::uint8_t[]> live{new std::uint8_t[kMaxFrames / kBlock + 1]()};   // userLive(0) after each block
    std::unique_ptr<int[]> pos{new int[kMaxFrames / kBlock + 1]()};  // the interval position after each block
    std::atomic<int> stall{0};                   // blocks the host doesn't call process() for
    std::atomic<int> hold{0};                    // blocks the host calls process() for late, then back to back
    // How much faster this side's audio clock runs than the room's, in
    // ppm: its host's blocks are a frame longer as often as that takes.
    // Its own frames (its input, `heard`) are then ahead of the room's by
    // ext[b] after block b.
    std::atomic<int> ppm{0};
    std::unique_ptr<int[]> ext{new int[kMaxFrames / kBlock + 1]()};
    // The room's frame for this side's frame `own`, to a frame.
    long roomFrame(long own) const {
        const long b = std::max(0L, std::min(own / kBlock, kMaxFrames / kBlock));
        return own - ext[b];
    }
    std::mutex m;
    std::vector<std::string> notices;
    std::size_t noticeCount() {
        std::lock_guard<std::mutex> lock(m);
        return notices.size();
    }
    // The first notice from `from` on that contains `text`, or "".
    std::string findNotice(const std::string& text, std::size_t from = 0) {
        std::lock_guard<std::mutex> lock(m);
        for (std::size_t i = from; i < notices.size(); ++i)
            if (notices[i].find(text) != std::string::npos) return notices[i];
        return std::string();
    }
};

struct LiveRoom {
    Server server;
    std::thread server_thread;
    std::atomic<bool> server_stop{false};
    Side side[2];                                // 0 is alice, 1 is bob
    std::vector<long> starts = burstStarts();
    std::atomic<long> frames{0};
    std::atomic<bool> audio_stop{false};
    // The machine's own hiccups: a wake more than 10 ms late. After one the
    // host catches up, which plays the listeners' buffers faster than the
    // audio can arrive, so there may be dropouts; the checks that need a steady
    // stream are made only when there was none.
    std::atomic<long> late_us{0}, late_blocks{0}, slow_us{0};
    std::atomic<long> hiccup_frame{-1};
    std::atomic<int> host_audio_thread{-1};      // whether the host thread became an audio thread (realtime.h)
    std::thread audio;

    LiveRoom() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote;
        cfg.allow_codecs = kCapOpus | kCapFlac;
        cfg.opus_kbps = 128;
        cfg.bpm = 120;
        cfg.bpi = 4;                             // 2 s intervals
        cfg.live.mode = LiveMode::On;
        bool ok = server.start(cfg);
        // Live's UDP port has the TCP port's number, which another program on
        // the machine may hold: then another port.
        for (int i = 0; ok && !server.liveUdpOpen() && i < 5; ++i) {
            std::printf("    UDP port %u is taken: the server moves\n", server.port());
            server.stop();
            ok = server.start(cfg);
        }
        CHECK(ok);
        static bool described = false;           // once a process: how this machine schedules threads
        if (!described) {
            described = true;
            std::printf("    timing, the main thread: %s\n", thread::describe().c_str());
            std::thread([] {
                const bool ok = thread::makeAudioThread(static_cast<double>(kBlock) / kRate);
                std::printf("    timing, an audio thread (%s): %s\n", ok ? "made" : "not made",
                            thread::describe().c_str());
            }).join();
        }
        server_thread = std::thread([this] {
            thread::makeNetworkThread();         // it relays the Live datagrams, as the engines' Live threads send them
            while (!server_stop.load()) server.poll(1);
        });
        side[0].name = "alice";
        side[1].name = "bob";
        for (Side& s : side) {
            s.e.setMetronome(0, 0, true, 0);
            LocalChannel lc;
            lc.name = "tone";
            lc.broadcast = true;
            lc.mute = true;                      // no local monitoring: the output is the other's audio only
            s.e.setLocalChannel(0, lc);
            Side* sp = &s;
            s.e.onLiveNotice = [sp](const std::string& text, bool) {
                std::lock_guard<std::mutex> lock(sp->m);
                sp->notices.push_back(text);
            };
        }
        audio = std::thread([this] { audioLoop(); });
    }
    ~LiveRoom() {
        std::printf("    the host: woke at most %.1f ms late (%ld blocks over 10 ms; %s), the longest process() "
                    "%.1f ms\n",
                    late_us.load() / 1000.0, late_blocks.load(),
                    host_audio_thread.load() == 1   ? "an audio thread"
                    : host_audio_thread.load() == 0 ? "not an audio thread"
                                                    : "not started",
                    slow_us.load() / 1000.0);
        for (Side& s : side) std::printf("    %s: %s\n", s.name.c_str(), s.e.liveTestTiming().c_str());
        audio_stop.store(true);
        audio.join();
        for (Side& s : side) s.e.disconnect();
        for (int i = 0; i < 20; ++i) pump();
        server_stop.store(true);
        server_thread.join();
    }

    // What both engines get on their channel 0 at frame `n`: the bursts.
    float signalAt(long n) const {
        const auto it = std::upper_bound(starts.begin(), starts.end(), n);
        if (it == starts.begin()) return 0.0f;
        const long into = n - *(it - 1);
        return into < kBurst ? static_cast<float>(kAmp * std::sin(2 * M_PI * kHz * into / kRate)) : 0.0f;
    }

    // The host: one block of both engines every 270 frames. After a late wake
    // it calls again at once to catch up, as hosts do (the machine's own
    // hiccups are not host gaps, 6.9). A side's `stall` is a host gap: the
    // host doesn't call its process() for those blocks. Its `hold` is a
    // stall of its host's callbacks that catches up: the blocks are held
    // back, then all called back to back.
    void audioLoop() {
        std::vector<float> in(kBlock + 1), out[2][2];
        for (auto& o : out) o[0].assign(kBlock + 1, 0.0f), o[1].assign(kBlock + 1, 0.0f);
        const auto period = std::chrono::nanoseconds(1000000000LL * kBlock / kRate);
        host_audio_thread.store(thread::makeAudioThread(static_cast<double>(kBlock) / kRate) ? 1 : 0);
        struct Held {
            long b, own;
            int n;
        };
        std::vector<Held> held[2];               // the blocks held back
        long own[2] = {0, 0};                    // each side's own frames so far
        double drift[2] = {0, 0};                // and the part of a frame its clock is ahead
        // One block of side `s`: the room's block `b`, `n` frames from its own frame `at`.
        auto play = [&](int s, long b, long at, int n) {
            Side& sd = side[s];
            for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = signalAt(at + i);
            const float* inp[1] = {in.data()};
            float* o[2] = {out[s][0].data(), out[s][1].data()};
            const auto t0 = Clock::now();
            sd.e.process(inp, 1, o, 2, n, kRate);
            const long took =
                static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
            if (took > slow_us.load()) slow_us.store(took);
            std::copy(out[s][0].begin(), out[s][0].begin() + n, sd.heard.get() + at);
            sd.live[b] = sd.e.userLive(0) ? 1 : 0;
            int p = 0, len = 0;
            sd.e.position(&p, &len);
            sd.pos[b] = p;
        };
        auto next = Clock::now();
        while (!audio_stop.load()) {
            std::this_thread::sleep_until(next);
            next += period;
            const auto now = Clock::now();
            const long late = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(now - (next - period)).count());
            if (late > late_us.load()) late_us.store(late);
            if (late > 10000) {
                late_blocks.fetch_add(1);
                hiccup_frame.store(frames.load());
            }
            if (now - next > std::chrono::seconds(1)) next = now;
            const long f = frames.load();
            if (f + kBlock > kMaxFrames) break;
            const long b = f / kBlock;
            for (int s = 0; s < 2; ++s) {
                Side& sd = side[s];
                drift[s] += kBlock * sd.ppm.load() * 1e-6;
                const int n = kBlock + (drift[s] >= 1.0 ? 1 : 0);
                if (n > kBlock) drift[s] -= 1.0;
                const long at = own[s];
                own[s] += n;
                sd.ext[b] = static_cast<int>(own[s] - (f + kBlock));
                if (sd.stall.load() > 0) {
                    sd.stall.fetch_sub(1);
                    sd.live[b] = b > 0 ? sd.live[b - 1] : 0;
                    sd.pos[b] = b > 0 ? sd.pos[b - 1] : 0;
                    continue;
                }
                if (sd.hold.load() > 0) {
                    sd.hold.fetch_sub(1);
                    held[s].push_back({b, at, n});
                    continue;
                }
                for (const Held& h : held[s]) play(s, h.b, h.own, h.n);
                held[s].clear();
                play(s, b, at, n);
            }
            frames.store(f + kBlock);
        }
    }

    void pump() {
        for (Side& s : side) s.e.run();
        for (Side& s : side)
            for (int u = 0; u < s.e.numUsers(); ++u)
                for (int i = 0; i < 32; ++i) {
                    const int ch = s.e.channelAt(u, i);
                    if (ch < 0) break;
                    if (s.e.channelVolume(u, ch) != 1.0f) s.e.setChannelVolume(u, ch, 1.0f);
                }
    }
    bool until(const std::function<bool()>& pred, double seconds) {
        const TimePoint deadline = Clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
        while (!pred()) {
            if (Clock::now() >= deadline) return false;
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
    void wait(double seconds) {
        until([] { return false; }, seconds);
    }

    bool connect() {
        for (Side& s : side) s.e.connect("127.0.0.1", server.port(), "anonymous:" + s.name, "");
        return until(
            [&] {
                for (Side& s : side)
                    if (s.e.status() != Status::Ok || s.e.numUsers() != 1 || !s.e.liveRunning()) return false;
                return true;
            },
            10);
    }
    bool bothLive(double seconds) {
        if (!until([&] { return side[0].e.userLive(0) && side[1].e.userLive(0); }, seconds)) return false;
        wait(0.1);                               // the control contexts see the switch too
        return true;
    }
    bool calmSince(long frame) const { return hiccup_frame.load() < frame; }
    // Both live, and calm, for the last `seconds`.
    bool liveAndCalm(double budget, double seconds = 1.0) {
        const long span = static_cast<long>(seconds * kRate);
        return until(
            [&] {
                const long f = frames.load();
                if (f < span || !calmSince(f - span)) return false;
                for (const Side& s : side)
                    for (long b = (f - span) / kBlock; b < f / kBlock; ++b)
                        if (!s.live[b]) return false;
                return side[0].e.userLive(0) && side[1].e.userLive(0);
            },
            budget);
    }
    // At least `n` bursts heard live by `listener` from frame `from` on.
    std::vector<double> collect(int listener, long from, std::size_t n, double budget, int* missing = nullptr,
                                int* doubles = nullptr) {
        until([&] { return liveDelays(listener, from).size() >= n; }, budget);
        return liveDelays(listener, from, missing, doubles);
    }

    // The delays (ms) of the bursts sent from frame `from` on that `listener`
    // heard while it heard the other live for the whole window; `missing`
    // counts those it didn't find, and `doubles` the windows with more sound
    // after the burst (an interval copy that should have been muted).
    std::vector<double> liveDelays(int listener, long from, int* missing = nullptr, int* doubles = nullptr) {
        const Side& s = side[listener];
        const long done = frames.load();
        std::vector<double> d;
        int miss = 0, dbl = 0;
        for (long st : starts) {
            if (st < from || st + kWindow > done) continue;
            bool all = true;
            for (long b = st / kBlock; b <= (st + kWindow) / kBlock; ++b) all = all && s.live[b];
            if (!all) continue;
            long found = -1;
            for (long m = st; m < st + kWindow; ++m)
                if (std::fabs(s.heard[m]) > 0.12f) {
                    found = m;
                    break;
                }
            if (found < 0) {
                ++miss;
                continue;
            }
            d.push_back((found - st) * 1000.0 / kRate);
            for (long m = found + kBurst + kRate * 15 / 1000; m < st + kWindow; ++m)
                if (std::fabs(s.heard[m]) > 0.12f) {
                    ++dbl;
                    break;
                }
        }
        std::sort(d.begin(), d.end());
        if (missing) *missing = miss;
        if (doubles) *doubles = dbl;
        return d;
    }

    // On the room's clock (Side::ppm): for each burst the other sent from
    // room frame `from` on whose window ends by `to`, when it was sent
    // (s) and how late `listener` heard it (ms, -1 when not at all).
    std::vector<std::pair<double, double>> roomDelays(int listener, long from, long to) const {
        const Side& l = side[listener];
        const Side& o = side[1 - listener];
        std::vector<std::pair<double, double>> v;
        for (long st : starts) {                 // the other's own frames
            const long at = o.roomFrame(st);
            if (at < from || at + kWindow > to || at + kWindow >= kMaxFrames) continue;
            const long m0 = at + l.ext[at / kBlock];
            long found = -1;
            for (long m = m0; m < m0 + kWindow; ++m)
                if (std::fabs(l.heard[m]) > 0.12f) {
                    found = m;
                    break;
                }
            v.push_back({at / double(kRate), found < 0 ? -1.0 : (l.roomFrame(found) - at) * 1000.0 / kRate});
        }
        return v;
    }

    // Whether an interval boundary fell into block `b` (the position after it is at most a block).
    bool boundaryIn(int listener, long b) const { return side[listener].pos[b] <= kBlock; }
    // The first block from `from_block` on where `listener` goes from `was` to the other state, or -1.
    // userLive() changes inside process(), before the audio thread records the block it is in and
    // counts it in `frames`, so a switch the main thread has just seen is looked for two blocks on.
    long switchFrom(int listener, long from_block, bool was) {
        const long recorded = frames.load() + 2 * kBlock;
        const TimePoint deadline = Clock::now() + std::chrono::milliseconds(500);
        while (frames.load() < recorded && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const Side& s = side[listener];
        const long done = frames.load() / kBlock;
        for (long b = std::max(1L, from_block); b < done; ++b)
            if (s.live[b - 1] == (was ? 1 : 0) && s.live[b] != s.live[b - 1]) return b;
        return -1;
    }
    double secondsOf(long blocks) const { return blocks * double(kBlock) / kRate; }
    // The peak of output 0 over `seconds` from frame `from`.
    float peak(int listener, long from, double seconds) const {
        float p = 0;
        const long to = std::min(frames.load(), from + static_cast<long>(seconds * kRate));
        for (long m = from; m < to; ++m) p = std::max(p, std::fabs(side[listener].heard[m]));
        return p;
    }
};

double median(const std::vector<double>& v) { return v.empty() ? -1 : v[v.size() / 2]; }
double p95(const std::vector<double>& v) { return v.empty() ? -1 : v[std::min(v.size() - 1, v.size() * 95 / 100)]; }

}  // namespace

TEST(both_hear_each_other_live) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.bothLive(15));
    // The switch came at a boundary, with its chat line.
    for (int s = 0; s < 2; ++s) {
        const long b = r.switchFrom(s, 0, false);
        REQUIRE(b > 0);
        CHECK(r.boundaryIn(s, b));
    }
    const std::string said = r.side[1].findNotice("The room is Live now, about ");
    std::printf("    %s\n", said.c_str());
    CHECK(said.size() > 0 && said.back() == '.' && said.find(" ms late.") != std::string::npos);
    CHECK(!r.side[0].findNotice("The room is Live now, about ").empty());
    const long from = r.frames.load();
    for (int s = 0; s < 2; ++s) {
        int missing = 0, doubles = 0;
        std::vector<double> d = r.collect(s, from, 15, 40, &missing, &doubles);
        std::printf("    %s hears %s: %zu bursts, %d missing, %d heard twice, median %.1f ms, 95th percentile %.1f ms\n",
                    r.side[s].name.c_str(), r.side[1 - s].name.c_str(), d.size(), missing, doubles, median(d), p95(d));
        CHECK(d.size() >= 15);
        CHECK(missing <= 1);
        CHECK_EQ(doubles, 0);                    // the interval copy is muted while live (6.6)
        CHECK(median(d) > 0 && median(d) < 100);   // spec test 35's bounds
        CHECK(p95(d) < 150);
    }
    if (r.calmSince(from)) {                     // a steady stream stays live
        CHECK(r.side[0].e.userLive(0) && r.side[1].e.userLive(0));
        CHECK(r.side[1].findNotice("Intervals may sound better").empty());   // no advice
        CHECK(r.side[0].findNotice("Intervals may sound better").empty());
    }
}

TEST(live_off_falls_back_at_once) {
    LiveRoom r;
    REQUIRE(r.connect());
    // `!live off` early in an interval, so that the next boundary is more than 1.5 s away.
    bool ready = false;
    for (int attempt = 0; attempt < 5 && !ready; ++attempt) {
        REQUIRE(r.liveAndCalm(20));
        ready = r.until(
                    [&] {
                        int p = 0, len = 0;
                        r.side[1].e.position(&p, &len);
                        return len > 0 && p < len / 10;
                    },
                    5) &&
                r.side[1].e.userLive(0);
    }
    REQUIRE(ready);
    const std::size_t seen = r.side[1].noticeCount();
    const long block = r.frames.load() / kBlock;
    const TimePoint t0 = Clock::now();
    CHECK(r.side[0].e.liveCommand("/live off"));
    REQUIRE(r.until([&] { return !r.side[1].e.userLive(0); }, 3));
    const double took = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("    bob back on intervals %.0f ms after !live off\n", took * 1000);
    CHECK(took < 0.5);
    const long b = r.switchFrom(1, block, true);
    REQUIRE(b > 0);
    CHECK(!r.boundaryIn(1, b));                  // in the middle of the interval
    // No interval audio was uploaded while the room was Live, nor in the
    // interval the switch fell in (it began Live), so bob hears the intervals
    // from the second boundary after the switch on: the players' first
    // interval after Live.
    auto boundaryAfter = [&](long from) {        // the first block after `from` with a boundary, or -1
        for (long k = from + 1; k < r.frames.load() / kBlock; ++k)
            if (r.boundaryIn(1, k)) return k;
        return -1L;
    };
    long b2 = -1;
    REQUIRE(r.until(
        [&] {
            const long b1 = boundaryAfter(b);
            b2 = b1 < 0 ? -1 : boundaryAfter(b1);
            return b2 > 0 && r.frames.load() >= b2 * kBlock + kRate / 2;
        },
        6));
    const long from2 = b2 * kBlock + kBlock - r.side[1].pos[b2];   // the boundary's frame
    long first = -1;                             // the first loud sample 100 ms on (after Live's fade-out)
    for (long m = b * kBlock + kRate / 10; m < r.frames.load() && first < 0; ++m)
        if (std::fabs(r.side[1].heard[m]) > 0.3f) first = m;
    std::printf("    bob hears the intervals %.0f ms after the switch (%.0f ms into an interval), the second boundary "
                "after it %.0f ms after it\n", first < 0 ? -1.0 : (first - b * kBlock) * 1000.0 / kRate,
                r.side[1].pos[b] * 1000.0 / kRate, (from2 - b * kBlock) * 1000.0 / kRate);
    CHECK(r.peak(1, from2, 0.5) > 0.3f);         // the intervals play
    CHECK(r.side[1].findNotice("Intervals may sound better", seen).empty());   // no advice when the room went off
    CHECK(!r.side[1].e.userLive(0) && !r.side[0].e.userLive(0));
    std::printf("    %s\n", r.side[1].findNotice("Live", seen).c_str());

    // `!live on`: Live again, from a boundary.
    const long again = r.frames.load() / kBlock;
    CHECK(r.side[1].e.liveCommand("/live on"));
    REQUIRE(r.bothLive(20));
    const long on = r.switchFrom(1, again, false);
    REQUIRE(on > 0);
    CHECK(r.boundaryIn(1, on));
    std::printf("    bob hears alice live again %.1f s after !live on\n", r.secondsOf(on - again));
}

// The room's mode is its switch alone (16.7): a stall or loss gives advice,
// to both players, and the room stays Live.
struct StaysLive {
    LiveRoom& r;
    bool stayed = true;
    // The room is still Live for both, and each still hears the other live.
    void look() {
        for (Side& s : r.side) stayed = stayed && s.e.liveRoomLive() && s.e.userLive(0);
    }
    bool advised(int s) const {
        return r.side[s].e.liveAdviceReason() == Engine::LiveAdvice::Dropouts &&
               r.side[s].e.liveAdviceUser().find("alice") == 0;   // alice@127.0.0.x
    }
    bool none(int s) const { return r.side[s].e.liveAdviceReason() == Engine::LiveAdvice::None; }
    bool until(const std::function<bool()>& pred, double seconds) {
        return r.until([&] { look(); return pred(); }, seconds);
    }
};

TEST(sender_stall_gives_advice_and_stays_live) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    StaysLive w{r};
    const std::size_t seen[2] = {r.side[0].noticeCount(), r.side[1].noticeCount()};
    const long block = r.frames.load() / kBlock;
    const Server::IntervalTraffic t0 = r.server.intervalTraffic();
    r.side[0].stall.store(kRate * 3 / 2 / kBlock);   // alice's host doesn't call for 1.5 s
    REQUIRE(w.until([&] { return w.advised(1) && w.advised(0); }, 6));
    std::printf("    the advice names alice %.0f ms after the stall began (%d with trouble)\n",
                r.secondsOf(r.frames.load() / kBlock - block) * 1000, r.side[1].e.liveAdviceCount());
    CHECK(r.side[1].e.liveAdviceCount() >= 1);
    CHECK(w.until(
        [&] {
            return !r.side[1].findNotice("alice's connection has dropouts", seen[1]).empty() &&
                   !r.side[0].findNotice("Your connection has dropouts", seen[0]).empty();
        },
        1));
    const std::string said = r.side[1].findNotice("alice's connection has dropouts", seen[1]);
    std::printf("    %s\n", said.c_str());
    const std::string tail = ": Intervals may sound better (switch with /live off).";
    CHECK(said.size() > tail.size() && said.compare(said.size() - tail.size(), tail.size(), tail) == 0);
    // It clears once alice has been clean for 10 s: the stall's 1.5 s and 10 s at the least.
    REQUIRE(w.until([&] { return w.none(1) && w.none(0); }, 25));
    const double cleared = r.secondsOf(r.frames.load() / kBlock - block);
    std::printf("    the advice clears %.1f s after the stall began\n", cleared);
    CHECK(cleared >= 11.5);
    CHECK(w.until([&] { return !r.side[1].findNotice("alice's connection is clean again.", seen[1]).empty(); }, 1));
    CHECK(w.stayed);                             // the room stayed Live throughout
    const Server::IntervalTraffic t1 = r.server.intervalTraffic();
    CHECK_EQ(t1.from_live - t0.from_live, 0u);   // and no interval audio was sent or relayed
    CHECK_EQ(t1.relayed - t0.relayed, 0u);
    // Spec test 43: after the stall the buffer figure is still at most B + a frame + a host block.
    r.wait(0.2);
    const std::size_t at = r.side[1].noticeCount();
    r.side[1].e.liveCommand("/live");
    const std::string line = r.side[1].findNotice("alice: ", at);
    std::printf("    %s\n", line.c_str());
    int buffer = -1;
    const std::size_t k = line.find("(buffer ");
    if (k != std::string::npos) std::sscanf(line.c_str() + k, "(buffer %d ms", &buffer);
    CHECK(buffer >= 20 && buffer <= 31);
}

TEST(packet_loss_gives_advice_and_stays_live) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    StaysLive w{r};
    const std::size_t seen = r.side[1].noticeCount();
    const long block = r.frames.load() / kBlock;
    const Server::IntervalTraffic t0 = r.server.intervalTraffic();
    r.side[1].e.setLiveTestLoss(0.4);            // bob loses 40 % of what reaches him over UDP
    // Bob can't tell his own loss from alice's: with one other player, he names her.
    REQUIRE(w.until([&] { return w.advised(1) && w.advised(0); }, 5));
    std::printf("    the advice names alice %.0f ms after the loss began\n",
                r.secondsOf(r.frames.load() / kBlock - block) * 1000);
    CHECK(w.until([&] { return !r.side[1].findNotice("alice's connection has dropouts", seen).empty(); }, 1));
    w.until([] { return false; }, 2.5);          // the loss goes on, and the room stays Live
    CHECK(w.advised(1));
    const long healed = r.frames.load() / kBlock;
    r.side[1].e.setLiveTestLoss(0);
    REQUIRE(w.until([&] { return w.none(1) && w.none(0); }, 25));
    const double cleared = r.secondsOf(r.frames.load() / kBlock - healed);
    std::printf("    the advice clears %.1f s after the loss ended\n", cleared);
    CHECK(cleared >= 10.0);                      // 10 s clean
    CHECK(w.until([&] { return !r.side[1].findNotice("alice's connection is clean again.", seen).empty(); }, 1));
    CHECK(w.stayed);
    const Server::IntervalTraffic t1 = r.server.intervalTraffic();
    CHECK_EQ(t1.from_live - t0.from_live, 0u);
    CHECK_EQ(t1.relayed - t0.relayed, 0u);
}

TEST(jitter_buffer_setting_takes_effect) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    CHECK_EQ(r.side[1].e.liveBufferInUseMs(), 20);
    std::vector<double> before = r.collect(1, r.frames.load(), 10, 30);
    std::size_t seen = r.side[1].noticeCount();
    const long changed = r.frames.load();
    CHECK(r.side[1].e.liveCommand("/live buffer 60"));
    CHECK(!r.side[1].findNotice("Live's jitter buffer is 60 ms now (it was 20). Less is less late, more has fewer "
                                "dropouts: type /live to see them.", seen)
               .empty());
    CHECK_EQ(r.side[1].e.liveBufferMs(), 60);
    CHECK(r.until([&] { return r.side[1].e.liveBufferInUseMs() == 60; }, 1));
    r.wait(1);                                   // the sinks buffer again
    std::vector<double> after = r.collect(1, r.frames.load(), 10, 30);
    const double rise = median(after) - median(before);
    std::printf("    delay at 20 ms: median %.1f ms (%zu bursts); at 60 ms: median %.1f ms (%zu bursts), +%.1f ms\n",
                median(before), before.size(), median(after), after.size(), rise);
    CHECK(before.size() >= 10 && after.size() >= 10);
    CHECK(rise >= 28 && rise <= 52);             // 40 ms more, as spec test 38 has 20 ms more give 12-28
    const bool calm = r.calmSince(changed);
    if (calm) {                                  // the buffer change is a gap, not trouble (6.5)
        CHECK(r.side[1].e.userLive(0));
        CHECK(r.side[1].findNotice("Intervals may sound better", seen).empty());
        if (!r.side[1].findNotice("Intervals may sound better", seen).empty()) {
            std::lock_guard<std::mutex> lock(r.side[1].m);
            for (std::size_t i = seen; i < r.side[1].notices.size(); ++i) std::printf("    [bob] %s\n", r.side[1].notices[i].c_str());
        }
    }
    // /live shows the buffer in use and an estimate close to the measured delay.
    seen = r.side[1].noticeCount();
    CHECK(r.side[1].e.liveCommand("/live"));
    const std::string head = r.side[1].findNotice("Live is on. ", seen);
    const std::string line = r.side[1].findNotice("alice: ", seen);
    std::printf("    %s\n    %s\n", head.c_str(), line.c_str());
    CHECK(head.find("Live is on. Jitter buffer 60 ms (/live buffer N to change it). Round trip to the server ") == 0);
    // The buffer and the codec as they are. The network figure is half the
    // round trip measured over the loopback (0 ms here, 1 ms on the macOS
    // runner), so any number.
    {
        const std::string pre = " ms late (buffer 60 ms, network ";
        const std::size_t b = line.find(pre);
        std::size_t d = b == std::string::npos ? line.size() : b + pre.size();
        const std::size_t digits = d;
        while (d < line.size() && line[d] >= '0' && line[d] <= '9') ++d;
        CHECK(b != std::string::npos && d > digits && line.compare(d, 18, " ms, codec 6 ms), ") == 0);
    }
    CHECK(line.find(" s since the buffer changed, ") != std::string::npos);
    if (calm) {
        CHECK(line.find("alice: live, about ") == 0);
        CHECK(line.find("0.0% of the packets lost. Dropouts: 0 in the ") != std::string::npos);
    }
    int estimate = -1;
    const std::size_t k = line.find(", about ");
    if (k != std::string::npos) std::sscanf(line.c_str() + k, ", about %d ms late", &estimate);
    std::printf("    estimate %d ms, heard %.1f ms\n", estimate, median(after));
    CHECK(median(after) >= estimate - 5 && median(after) <= estimate + 10);   // spec test 38's bound
    // `/live buffer` without a number, and a bad one.
    seen = r.side[1].noticeCount();
    r.side[1].e.liveCommand("/live buffer");
    CHECK(!r.side[1].findNotice("Live's jitter buffer is 60 ms. Type /live buffer 30 to change it (5 to 200), or /live "
                                "buffer auto.", seen)
               .empty());
    r.side[1].e.liveCommand("/live buffer 30.0");
    CHECK(!r.side[1].findNotice("/live buffer takes a number of ms from 5 to 200, such as /live buffer 30, or auto.", seen)
               .empty());
    CHECK_EQ(r.side[1].e.liveBufferMs(), 60);
}

// A host whose process() calls stall and then catch up, back to back,
// missed nothing (6.9): it sends what it captured meanwhile all at once,
// and plays the stream that came meanwhile as it catches up. Each
// listener's delay to the other is the same after each stall as before, to
// the frame: alice's own stall holds what she hears longer, without
// overflowing her buffer into a new delay (or taking a new level for it to
// keep), and bob, who ran dry while she was away, goes on at the delay he
// had.
TEST(host_stalls_keep_the_delay) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    r.wait(2);                                    // each buffer's level is taken after 1 s of playing
    const long t0 = r.frames.load();
    r.wait(3);
    const char* who[2] = {"alice hears bob", "bob hears alice"};
    double before[2], fill_before[2];
    for (int s = 0; s < 2; ++s) {
        before[s] = median(r.liveDelays(s, t0));
        fill_before[s] = r.side[s].e.liveUserBufferMs(0);
        std::printf("    %s: %.2f ms (the estimate %.1f ms, the buffer %.2f ms)\n", who[s], before[s],
                    r.side[s].e.liveUserHeardMs(0), fill_before[s]);
        REQUIRE(before[s] > 0);
        CHECK(fill_before[s] > 0);
        CHECK(r.side[s].e.liveUserHeardMs(0) > fill_before[s]);
    }
    for (int ms : {30, 60, 80}) {
        const int n = (ms * kRate / 1000 + kBlock - 1) / kBlock;
        const long at = r.frames.load();
        r.side[0].hold.store(n);                  // alice's host: n blocks late, then back to back
        r.wait(3);
        const long settled = r.frames.load();
        double fill[2];
        for (int s = 0; s < 2; ++s) fill[s] = r.side[s].e.liveUserBufferMs(0);
        r.wait(1.5);
        const bool calm = r.calmSince(at - 3 * kRate);
        for (int s = 0; s < 2; ++s) {
            const std::vector<double> d = r.liveDelays(s, settled);
            double worst = 0;
            for (double x : d) worst = std::max(worst, std::fabs(x - before[s]));
            std::printf("    alice's host %d ms late: %s from 3 s on: %zu bursts, %.2f to %.2f ms "
                        "(the buffer %.2f ms)%s\n", n * kBlock * 1000 / kRate, who[s], d.size(),
                        d.empty() ? -1.0 : d.front(), d.empty() ? -1.0 : d.back(), fill[s], calm ? "" : " (not calm)");
            CHECK(d.size() >= 2);
            if (!calm) continue;
            CHECK(worst <= 1.0);                  // back within 1 ms, and it stays there
            CHECK(std::fabs(fill[s] - fill_before[s]) <= 1.0);
        }
    }
    // Neither buffer overflowed: no new delay at all.
    for (int s = 0; s < 2; ++s) {
        const std::size_t seen = r.side[s].noticeCount();
        CHECK(r.side[s].e.liveCommand("/live"));
        r.wait(0.2);
        const std::string line = r.side[s].findNotice("Dropouts:", seen);
        std::printf("    [%s] %s\n", r.side[s].name.c_str(), line.c_str());
        CHECK(!line.empty());
        if (r.calmSince(t0)) CHECK(line.find(" 0 overflowed") != std::string::npos);
    }
}

// A long run on two clocks 200 ppm apart (alice's runs fast), with host
// stalls that catch up, of either host or both, 20 to 170 ms long, and two
// 300 ms network outages both ways: one that loses what is sent, one that
// holds it back and then lets it all through. Each listener's delay to the
// other is back within 1 ms of where it was 3 s after each stall, and 1 s
// after the network comes back, and no burst is heard earlier than the
// buffer (20 ms) after the sender played it. On these clocks the speed
// makes up for the 200 ppm with the fill a few ms off the level it keeps,
// which takes it some 30 s to reach, so the delays still move a little at
// first.
TEST(long_run_stalls_and_outages_on_clocks_200_ppm_apart) {
    LiveRoom r;
    r.side[0].ppm.store(200);
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    const long t0 = r.frames.load();
    const char* who[2] = {"alice hears bob", "bob hears alice"};
    // The drift control takes up the clocks' 200 ppm with an offset of the
    // buffer's fill, which it builds again after each fresh buffering (a
    // machine's hiccup can run a buffer dry), while the delay moves by up to
    // the 0.2 ms a second the clocks part by. What the events check is that
    // a stall leaves the delay where it was, so they start once both delays
    // have held still (the medians of the last two 2.5 s within 0.25 ms of
    // each other), and an event before which the delay was still moving
    // (over 0.2 ms in its 2.5 s) is reported but not held to that.
    auto still = [&] {
        const long now = r.frames.load();
        for (int l = 0; l < 2; ++l) {
            std::vector<double> half[2];
            for (int h = 0; h < 2; ++h)
                for (const auto& x : r.roomDelays(l, now - (2 - h) * 5 * kRate / 2, now - (1 - h) * 5 * kRate / 2))
                    if (x.second >= 0) half[h].push_back(x.second);
            if (half[0].empty() || half[1].empty()) return false;
            for (auto& v : half) std::sort(v.begin(), v.end());
            if (std::fabs(median(half[0]) - median(half[1])) > 0.25) return false;
        }
        return true;
    };
    r.wait(10);
    bool settled = still();
    for (int i = 0; i < 50 && !settled; ++i) {
        r.wait(1);
        settled = still();
    }
    std::printf("    the delays held still %.1f s after the start\n", r.secondsOf((r.frames.load() - t0) / kBlock));
    CHECK(settled);
    struct Event {
        int host;                                 // 0 alice, 1 bob, 2 both
        int ms;
        int net;                                  // 0 a host stall, 1 an outage that loses, 2 one that holds
    };
    const Event events[] = {{0, 20, 0}, {1, 50, 0}, {0, 80, 0}, {2, 120, 0}, {1, 170, 0}, {2, 300, 1}, {2, 300, 2}};
    int held = 0, looked = 0;
    for (const Event& ev : events) {
        const long at = r.frames.load();
        for (int s = 0; s < 2; ++s) {
            if (ev.host != s && ev.host != 2) continue;
            if (ev.net == 1) r.side[s].e.setLiveTestSendGap(ev.ms);
            else if (ev.net == 2) r.side[s].e.setLiveTestSendHold(ev.ms);
            else r.side[s].hold.store((ev.ms * kRate / 1000 + kBlock - 1) / kBlock);
        }
        const double back = ev.net ? ev.ms / 1000.0 + 1.0 : 3.0;   // when it should be back
        r.wait(5);
        const long to = r.frames.load();
        const bool calm = r.calmSince(at - 3 * kRate);
        const char* hosts[3] = {"alice's host", "bob's host", "both hosts"};
        char what[64];
        if (ev.net)
            std::snprintf(what, sizeof(what), "the network %s for %d ms", ev.net == 1 ? "lost all" : "held all", ev.ms);
        else
            std::snprintf(what, sizeof(what), "%s %d ms late", hosts[ev.host], ev.ms);
        for (int l = 0; l < 2; ++l) {
            std::vector<double> before;
            for (const auto& x : r.roomDelays(l, at - 5 * kRate / 2, at))
                if (x.second >= 0) before.push_back(x.second);
            std::sort(before.begin(), before.end());
            const double pre = median(before);
            double lo = 1e9, hi = -1e9;
            int n = 0;
            for (const auto& x : r.roomDelays(l, at + static_cast<long>(back * kRate), to)) {
                if (x.second < 0) continue;
                lo = std::min(lo, x.second);
                hi = std::max(hi, x.second);
                ++n;
            }
            const bool moving = before.size() >= 2 && before.back() - before.front() > 0.2;
            std::printf("    %s: %s %.2f ms before, %.2f to %.2f ms from %.1f s on (%d bursts; the buffer %.2f ms)"
                        "%s%s\n",
                        what, who[l], pre, lo, hi, back, n, r.side[l].e.liveUserBufferMs(0), calm ? "" : " (not calm)",
                        moving ? " (the delay was moving)" : "");
            CHECK(before.size() >= 3);
            CHECK(n >= 2);
            ++looked;
            if (!calm || moving) continue;
            ++held;
            CHECK(lo >= pre - 1.0 && hi <= pre + 1.0);
        }
    }
    std::printf("    %d of %d held to the delay before\n", held, looked);
    CHECK(r.frames.load() + kBlock <= kMaxFrames);   // the room recorded it all
    // Never earlier than the buffer, from the start of the run on.
    for (int l = 0; l < 2; ++l) {
        double lowest = 1e9;
        for (const auto& x : r.roomDelays(l, t0, r.frames.load()))
            if (x.second >= 0) lowest = std::min(lowest, x.second);
        std::printf("    %s: the earliest %.2f ms\n", who[l], lowest);
        CHECK(lowest >= 20.0);
    }
    for (int s = 0; s < 2; ++s) {
        const std::size_t seen = r.side[s].noticeCount();
        CHECK(r.side[s].e.liveCommand("/live"));
        r.wait(0.2);
        const std::string line = r.side[s].findNotice("Dropouts:", seen);
        std::printf("    [%s] %s\n", r.side[s].name.c_str(), line.c_str());
        CHECK(!line.empty());
        if (r.calmSince(t0)) CHECK(line.find(" 0 overflowed") != std::string::npos);
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    if (argc > 1) {
        auto& c = testing::cases();
        const std::string want = argv[1];         // only the cases whose names contain it
        auto other = [&](const testing::Case& k) { return std::string(k.name).find(want) == std::string::npos; };
        c.erase(std::remove_if(c.begin(), c.end(), other), c.end());
    }
    return testing::runAll();
}
