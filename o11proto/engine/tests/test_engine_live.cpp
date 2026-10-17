// Live mode end to end (ENGINE.md section 16): two engines through
// o11server in real time. An audio thread calls both engines' process()
// every 270 frames at 48 kHz, as a host would; the main thread runs their
// control contexts and the server runs on its own thread. Each engine sends
// short tone bursts at uneven gaps on its channel 0 and finds the other's in
// its output: live they come tens of ms late, through intervals about an
// interval late.
//
// The cases: both hear each other live at low latency; `!live off` brings
// the intervals back at once, and `!live on` brings Live back at a boundary;
// a sender's stall and packet loss make the listener fall back at the next
// boundary, and Live comes back later; `/live buffer 60` raises the delay.
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
#include "o11proto/server.h"

using namespace o11proto;
using namespace o11proto::engine;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 270;                      // not a divisor of the interval (spec test 35)
constexpr long kMaxFrames = 120L * kRate;        // the most one room records
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
    std::unique_ptr<float[]> heard{new float[kMaxFrames]()};          // output 0
    std::unique_ptr<std::uint8_t[]> live{new std::uint8_t[kMaxFrames / kBlock + 1]()};   // userLive(0) after each block
    std::unique_ptr<int[]> pos{new int[kMaxFrames / kBlock + 1]()};  // the interval position after each block
    std::atomic<int> stall{0};                   // blocks the host doesn't call process() for
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
    // audio can arrive, so Live may fall back; the checks that need a steady
    // stream are made only when there was none.
    std::atomic<long> late_us{0}, late_blocks{0}, slow_us{0};
    std::atomic<long> hiccup_frame{-1};
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
        CHECK(server.start(cfg));
        server_thread = std::thread([this] {
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
        std::printf("    the host: woke at most %.1f ms late (%ld blocks over 10 ms), the longest process() %.1f ms\n",
                    late_us.load() / 1000.0, late_blocks.load(), slow_us.load() / 1000.0);
        audio_stop.store(true);
        audio.join();
        for (Side& s : side) s.e.disconnect();
        for (int i = 0; i < 20; ++i) pump();
        server_stop.store(true);
        server_thread.join();
    }

    // The host: one block of both engines every 270 frames. After a late wake
    // it calls again at once to catch up, as hosts do (the machine's own
    // hiccups are not host gaps, 6.9); a stall is a side's `stall`.
    void audioLoop() {
        std::vector<float> in(kBlock), out[2][2];
        for (auto& o : out) o[0].assign(kBlock, 0.0f), o[1].assign(kBlock, 0.0f);
        const auto period = std::chrono::nanoseconds(1000000000LL * kBlock / kRate);
        auto next = Clock::now();
        std::size_t burst = 0;
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
            for (int i = 0; i < kBlock; ++i) {
                const long n = f + i;
                while (burst < starts.size() && n >= starts[burst] + kBurst) ++burst;
                const long into = burst < starts.size() ? n - starts[burst] : -1;
                in[static_cast<std::size_t>(i)] =
                    into >= 0 && into < kBurst ? static_cast<float>(kAmp * std::sin(2 * M_PI * kHz * into / kRate)) : 0.0f;
            }
            const long b = f / kBlock;
            for (int s = 0; s < 2; ++s) {
                Side& sd = side[s];
                if (sd.stall.load() > 0) {
                    sd.stall.fetch_sub(1);
                    sd.live[b] = b > 0 ? sd.live[b - 1] : 0;
                    sd.pos[b] = b > 0 ? sd.pos[b - 1] : 0;
                    continue;
                }
                const float* inp[1] = {in.data()};
                float* o[2] = {out[s][0].data(), out[s][1].data()};
                const auto t0 = Clock::now();
                sd.e.process(inp, 1, o, 2, kBlock, kRate);
                const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
                if (took > slow_us.load()) slow_us.store(took);
                std::copy(out[s][0].begin(), out[s][0].end(), sd.heard.get() + f);
                sd.live[b] = sd.e.userLive(0) ? 1 : 0;
                int p = 0, len = 0;
                sd.e.position(&p, &len);
                sd.pos[b] = p;
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

    // Whether an interval boundary fell into block `b` (the position after it is at most a block).
    bool boundaryIn(int listener, long b) const { return side[listener].pos[b] <= kBlock; }
    // The first block from `from_block` on where `listener` goes from `was` to the other state, or -1.
    long switchFrom(int listener, long from_block, bool was) {
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
    const std::string said = r.side[1].findNotice("You hear alice live now, about ");
    std::printf("    %s\n", said.c_str());
    CHECK(said.size() > 0 && said.back() == '.' && said.find(" ms late.") != std::string::npos);
    CHECK(!r.side[0].findNotice("You hear bob live now, about ").empty());
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
        CHECK(r.side[1].findNotice("is back on intervals").empty());
        CHECK(r.side[0].findNotice("is back on intervals").empty());
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
    r.wait(2.5);
    CHECK(r.peak(1, b * kBlock, 2.5) > 0.3f);    // the intervals play
    CHECK(r.side[1].findNotice("is back on intervals", seen).empty());   // no line when the room's Live went off
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

TEST(sender_stall_falls_back_at_boundary) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    const std::size_t seen = r.side[1].noticeCount();
    const long block = r.frames.load() / kBlock;
    r.side[0].stall.store(kRate / 2 / kBlock);   // alice's host doesn't call for 0.5 s
    REQUIRE(r.until([&] { return !r.side[1].e.userLive(0); }, 5));
    const long b = r.switchFrom(1, block, true);
    REQUIRE(b > 0);
    std::printf("    bob back on intervals %.0f ms after the stall began\n", r.secondsOf(b - block) * 1000);
    CHECK(r.boundaryIn(1, b));
    CHECK(r.until(
        [&] {
            return !r.side[1]
                        .findNotice("alice is back on intervals: their real-time audio stopped or had dropouts.", seen)
                        .empty();
        },
        1));
    // Live again later, from a boundary, after the settle time: the stall's
    // 0.5 s, 1 s of playing well and 3 s of settling at the least.
    REQUIRE(r.until([&] { return r.side[1].e.userLive(0); }, 20));
    const long on = r.switchFrom(1, b, false);
    REQUIRE(on > 0);
    std::printf("    bob hears alice live again %.1f s after the stall began\n", r.secondsOf(on - block));
    CHECK(r.boundaryIn(1, on));
    CHECK(r.secondsOf(on - block) >= 4.5);
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

TEST(packet_loss_falls_back_at_boundary) {
    LiveRoom r;
    REQUIRE(r.connect());
    REQUIRE(r.liveAndCalm(20));
    const std::size_t seen = r.side[1].noticeCount();
    const long block = r.frames.load() / kBlock;
    r.side[1].e.setLiveTestLoss(0.4);            // bob loses 40 % of what reaches him over UDP
    REQUIRE(r.until([&] { return !r.side[1].e.userLive(0); }, 5));
    const long b = r.switchFrom(1, block, true);
    REQUIRE(b > 0);
    std::printf("    bob back on intervals %.0f ms after the loss began\n", r.secondsOf(b - block) * 1000);
    CHECK(r.boundaryIn(1, b));
    CHECK(r.until([&] { return !r.side[1].findNotice("alice is back on intervals", seen).empty(); }, 1));
    r.wait(2.5);
    CHECK(r.peak(1, b * kBlock, 2.5) > 0.3f);    // the intervals play
    if (r.calmSince(block * kBlock)) CHECK(r.side[0].e.userLive(0));   // alice still hears bob live
    const long healed = r.frames.load() / kBlock;
    r.side[1].e.setLiveTestLoss(0);
    REQUIRE(r.until([&] { return r.side[1].e.userLive(0); }, 20));
    const long on = r.switchFrom(1, healed, false);
    REQUIRE(on > 0);
    std::printf("    bob hears alice live again %.1f s after the loss ended\n", r.secondsOf(on - healed));
    CHECK(r.boundaryIn(1, on));
    CHECK(r.secondsOf(on - healed) >= 3.0);      // the settle time
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
    if (calm) {                                  // the buffer change is a gap, not a fall back (6.5)
        CHECK(r.side[1].e.userLive(0));
        CHECK(r.side[1].findNotice("is back on intervals", seen).empty());
    }
    // /live shows the buffer in use and an estimate close to the measured delay.
    seen = r.side[1].noticeCount();
    CHECK(r.side[1].e.liveCommand("/live"));
    const std::string head = r.side[1].findNotice("Live is on. ", seen);
    const std::string line = r.side[1].findNotice("alice: ", seen);
    std::printf("    %s\n    %s\n", head.c_str(), line.c_str());
    CHECK(head.find("Live is on. Jitter buffer 60 ms (/live buffer N to change it). Round trip to the server ") == 0);
    CHECK(line.find(" ms late (buffer 60 ms, network 0 ms, codec 6 ms), ") != std::string::npos);
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
    CHECK(!r.side[1].findNotice("Live's jitter buffer is 60 ms. Type /live buffer 30 to change it (5 to 200).", seen)
               .empty());
    r.side[1].e.liveCommand("/live buffer 30.0");
    CHECK(!r.side[1].findNotice("/live buffer takes a number of ms from 5 to 200, such as /live buffer 30.", seen)
               .empty());
    CHECK_EQ(r.side[1].e.liveBufferMs(), 60);
}

TEST_MAIN()
