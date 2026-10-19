// A Live room in real time for the end-to-end tests (ENGINE.md 16.11):
// o11server on its own thread, N engines whose control contexts run on the
// main thread, and one host thread that calls every engine's process() each
// block, as a host would, and records what each one plays.
#ifndef O11PROTO_ENGINE_TESTS_LIVE_HARNESS_H
#define O11PROTO_ENGINE_TESTS_LIVE_HARNESS_H

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

#include "o11proto/engine.h"
#include "o11proto/realtime.h"
#include "o11proto/server.h"

namespace harness {

using namespace o11proto;
using namespace o11proto::engine;

constexpr int kBlock = 270;                      // not a divisor of the interval (spec test 35)
constexpr int kMaxSeconds = 100;                 // the most one room records

struct Side {
    Engine e;
    std::string name;
    // A fixed 20 ms jitter buffer, the default before the automatic one:
    // these checks are made for it (the automatic buffer's are in
    // test_engine_live_units and test_engine_live_tone).
    Side() { e.setLiveBufferMs(20); }
    // What it plays on its channel 0: sample `n` of its own clock at `rate`.
    std::function<float(long n, int rate)> signal;
    std::unique_ptr<float[]> heard;              // output 0
    std::unique_ptr<std::uint8_t[]> live;        // userLive(0) after each block
    std::unique_ptr<std::uint8_t[]> room;        // liveRoomLive() after each block
    std::unique_ptr<int[]> pos;                  // the interval position after each block
    std::atomic<long> clock_from{-1};            // the frame its interval clock started at, -1 before
    std::atomic<int> stall{0};                   // blocks the host doesn't call process() for
    // Or from the room's block `stall_at` on (-1: none), for `stall_len`
    // blocks: so that two sides' stalls begin at the blocks meant.
    std::atomic<long> stall_at{-1};
    std::atomic<int> stall_len{0};
    // The host calls its process() `call_late_us` µs late at the room's
    // block `call_late_at` (-1: never), as a host's callback comes a little
    // late now and then: its clock's reading of when its blocks began is
    // off by as much, and so is its measure of a gap just before or after
    // that block (6.9).
    std::atomic<long> call_late_at{-1};
    std::atomic<int> call_late_us{0};
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
    void printNotices(std::size_t from = 0) {
        std::lock_guard<std::mutex> lock(m);
        for (std::size_t i = from; i < notices.size(); ++i) std::printf("    [%s] %s\n", name.c_str(), notices[i].c_str());
    }
};

struct Options {
    int sides = 2;
    int rate = 48000;
    std::uint32_t codec = kCapOpus;              // the interval codec: kCapOpus or kCapFlac
    int bpm = 120, bpi = 4;                      // 2 s intervals
    bool live_on = true;
    bool live_server = true;                     // false: a server without Live (LiveMode::No)
    bool relay_fallback = false;                 // the server's backend option (LiveConfig)
    int block = kBlock;                          // the host's block, frames
    int live_frame = 0;                          // the room's Live frame (120, 240 or 480; 0: the server's default)
};

struct Room {
    Options opt;
    long max_frames;
    long max_blocks;
    Server server;
    std::thread server_thread;
    std::atomic<bool> server_stop{false};
    std::vector<std::unique_ptr<Side>> side;
    std::atomic<long> frames{0};
    std::atomic<bool> audio_stop{false};
    // The machine's own hiccups: a wake more than 10 ms late. After one the
    // host catches up, which plays the listeners' buffers faster than the
    // audio can arrive; the checks that need a steady stream skip them.
    // The audio thread's wakes don't show every hiccup (it runs as an audio
    // thread, o11proto/realtime.h, as a host's does), so a thread scheduled
    // as the server's is watches too: when it wakes that late, the server
    // may have been as late, and a listener's buffer ran dry.
    //
    // The server's thread here is the test's, and it relays every Live
    // datagram, so it is a network thread (thread::makeNetworkThread(),
    // real-time on macOS), as each engine's Live network thread is. It was
    // latency-critical (user-interactive QoS on macOS), and on a CI runner a
    // thread scheduled so woke up to 42.7 ms late, 18 times over 10 ms in
    // one test: a relay that late runs a 20 ms buffer dry, and three such
    // within 2 s make a clean health report fail (ENGINE.md 16.7), which
    // starts the 10 s before the advice clears again.
    std::atomic<long> late_us{0}, late_blocks{0}, slow_us{0}, sum_us{0}, calls{0}, slow_calls{0};
    std::atomic<long> plain_late_us{0}, plain_late_count{0};
    std::atomic<int> host_audio_thread{-1};      // whether the host thread became an audio thread
    std::atomic<int> server_sched{-1};           // how the server's thread is scheduled (thread::NetworkScheduling)
    std::mutex hm;
    std::vector<long> hiccups;                   // the frames they came at
    std::thread audio, watch;

    explicit Room(const Options& o, const std::vector<std::string>& names) : opt(o) {
        max_frames = static_cast<long>(kMaxSeconds) * opt.rate;
        max_blocks = max_frames / opt.block + 1;
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote;
        cfg.allow_codecs = kCapOpus | kCapFlac;
        cfg.chosen_codec = opt.codec;
        cfg.opus_kbps = 128;
        cfg.bpm = static_cast<std::uint16_t>(opt.bpm);
        cfg.bpi = static_cast<std::uint16_t>(opt.bpi);
        cfg.live.mode = !opt.live_server ? LiveMode::No : opt.live_on ? LiveMode::On : LiveMode::Yes;
        cfg.live.relay_fallback = opt.relay_fallback;
        if (opt.live_frame) cfg.live.frame = opt.live_frame;
        bool ok = server.start(cfg);
        // Live's UDP port has the TCP port's number, which another program on
        // the machine may hold: then another port.
        for (int i = 0; ok && opt.live_server && !server.liveUdpOpen() && i < 5; ++i) {
            std::printf("    UDP port %u is taken: the server moves\n", server.port());
            server.stop();
            ok = server.start(cfg);
        }
        if (!ok) std::printf("    the server didn't start\n");
        describeTiming(opt.rate);
        server_thread = std::thread([this] {
            server_sched.store(static_cast<int>(thread::makeNetworkThread()));
            while (!server_stop.load()) server.poll(1);
        });
        for (int i = 0; i < opt.sides; ++i) {
            side.emplace_back(new Side);
            Side& s = *side.back();
            s.name = i < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(i)] : "p" + std::to_string(i);
            s.heard.reset(new float[static_cast<std::size_t>(max_frames)]());
            s.live.reset(new std::uint8_t[static_cast<std::size_t>(max_blocks)]());
            s.room.reset(new std::uint8_t[static_cast<std::size_t>(max_blocks)]());
            s.pos.reset(new int[static_cast<std::size_t>(max_blocks)]());
            s.e.setMetronome(0, 0, true, 0);
            LocalChannel lc;
            lc.name = "tone";
            lc.broadcast = true;
            lc.mute = true;                      // no local monitoring: the output is the others' audio only
            s.e.setLocalChannel(0, lc);
            Side* sp = &s;
            s.e.onLiveNotice = [sp](const std::string& text, bool) {
                std::lock_guard<std::mutex> lock(sp->m);
                sp->notices.push_back(text);
            };
        }
    }
    // Once a process, how this machine schedules the threads (realtime.h):
    // the main thread, and threads made as the host's and the network's are.
    static void describeTiming(int rate) {
        static bool done = false;
        if (done) return;
        done = true;
        std::printf("    timing, the main thread: %s\n", thread::describe().c_str());
        std::thread([rate] {
            const bool ok = thread::makeAudioThread(static_cast<double>(kBlock) / rate);
            std::printf("    timing, an audio thread (%s): %s\n", ok ? "made" : "not made",
                        thread::describe().c_str());
        }).join();
        std::thread([] {
            const bool ok = thread::makeLatencyCritical();
            std::printf("    timing, a latency-critical thread (%s): %s\n", ok ? "made" : "not made",
                        thread::describe().c_str());
        }).join();
        std::thread([] {
            const thread::NetworkScheduling n = thread::makeNetworkThread();
            std::printf("    timing, a network thread (%s): %s\n",
                        n == thread::NetworkScheduling::Realtime          ? "real-time"
                        : n == thread::NetworkScheduling::LatencyCritical ? "latency-critical"
                                                                          : "not made",
                        thread::describe().c_str());
        }).join();
    }

    ~Room() {
        const int ss = server_sched.load();
        std::printf("    the host: woke at most %.1f ms late (%ld blocks over 10 ms; %s), the longest process() "
                    "%.1f ms, the mean %.3f ms, %ld over 2 ms; a thread scheduled as the server's (%s) woke at most "
                    "%.1f ms late (%ld times over 10 ms)\n",
                    late_us.load() / 1000.0, late_blocks.load(),
                    host_audio_thread.load() == 1   ? "an audio thread"
                    : host_audio_thread.load() == 0 ? "not an audio thread"
                                                    : "not started",
                    slow_us.load() / 1000.0, calls.load() ? sum_us.load() / 1000.0 / calls.load() : 0.0,
                    slow_calls.load(),
                    ss == static_cast<int>(thread::NetworkScheduling::Realtime)          ? "real-time"
                    : ss == static_cast<int>(thread::NetworkScheduling::LatencyCritical) ? "latency-critical"
                                                                                         : "unchanged",
                    plain_late_us.load() / 1000.0, plain_late_count.load());
        if (!side.empty()) std::printf("    %s: %s\n", side[0]->name.c_str(), side[0]->e.liveTestTiming().c_str());
        audio_stop.store(true);
        if (audio.joinable()) audio.join();
        if (watch.joinable()) watch.join();
        for (auto& s : side) s->e.disconnect();
        for (int i = 0; i < 20; ++i) pump();
        server_stop.store(true);
        server_thread.join();
    }
    Side& operator[](int i) { return *side[static_cast<std::size_t>(i)]; }
    void startHost() {
        audio = std::thread([this] { audioLoop(); });
        watch = std::thread([this] { watchLoop(); });
    }

    void watchLoop() {
        thread::makeNetworkThread();             // as the server's thread is
        while (!audio_stop.load()) {
            const auto t = Clock::now() + std::chrono::milliseconds(1);
            std::this_thread::sleep_until(t);
            const long late = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count());
            if (late > plain_late_us.load()) plain_late_us.store(late);
            if (late > 10000) {
                plain_late_count.fetch_add(1);
                std::lock_guard<std::mutex> lock(hm);
                hiccups.push_back(frames.load());
            }
        }
    }

    // The host: one block of every engine every opt.block frames. After a
    // late wake it calls again at once to catch up, as hosts do; a stall is
    // a side's `stall`, or its `stall_at` and `stall_len`.
    void audioLoop() {
        const int n = opt.sides;
        const int B = opt.block;
        std::vector<float> in(static_cast<std::size_t>(B));
        std::vector<std::vector<float>> out(static_cast<std::size_t>(2 * n), std::vector<float>(static_cast<std::size_t>(B), 0.0f));
        const auto period = std::chrono::nanoseconds(1000000000LL * B / opt.rate);
        host_audio_thread.store(thread::makeAudioThread(static_cast<double>(B) / opt.rate) ? 1 : 0);
        auto next = Clock::now();
        while (!audio_stop.load()) {
            std::this_thread::sleep_until(next);
            next += period;
            const auto now = Clock::now();
            const long late =
                static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(now - (next - period)).count());
            if (late > late_us.load()) late_us.store(late);
            if (late > 10000) {
                late_blocks.fetch_add(1);
                std::lock_guard<std::mutex> lock(hm);
                hiccups.push_back(frames.load());
            }
            if (now - next > std::chrono::seconds(1)) next = now;
            const long f = frames.load();
            if (f + B > max_frames) break;
            const long b = f / B;
            for (int s = 0; s < n; ++s) {
                Side& sd = *side[static_cast<std::size_t>(s)];
                const long stall_at = sd.stall_at.load(std::memory_order_acquire);
                const bool stalled = stall_at >= 0 && b >= stall_at && b < stall_at + sd.stall_len.load();
                if (stalled || sd.stall.load() > 0) {
                    if (!stalled) sd.stall.fetch_sub(1);
                    sd.live[b] = b > 0 ? sd.live[b - 1] : 0;
                    sd.room[b] = b > 0 ? sd.room[b - 1] : 0;
                    sd.pos[b] = b > 0 ? sd.pos[b - 1] : 0;
                    continue;
                }
                for (int i = 0; i < B; ++i) in[static_cast<std::size_t>(i)] = sd.signal ? sd.signal(f + i, opt.rate) : 0.0f;
                const float* inp[1] = {in.data()};
                float* o[2] = {out[static_cast<std::size_t>(2 * s)].data(), out[static_cast<std::size_t>(2 * s + 1)].data()};
                if (b == sd.call_late_at.load(std::memory_order_acquire)) {
                    const auto until = Clock::now() + std::chrono::microseconds(sd.call_late_us.load());
                    while (Clock::now() < until) {
                    }
                }
                const auto t0 = Clock::now();
                sd.e.process(inp, 1, o, 2, B, opt.rate);
                const long took =
                    static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
                if (took > slow_us.load()) slow_us.store(took);
                sum_us.fetch_add(took);
                calls.fetch_add(1);
                if (took > 2000) slow_calls.fetch_add(1);
                std::copy(o[0], o[0] + B, sd.heard.get() + f);
                sd.live[b] = sd.e.userLive(0) ? 1 : 0;
                sd.room[b] = sd.e.liveRoomLive() ? 1 : 0;
                int p = 0, len = 0;
                sd.e.position(&p, &len);
                sd.pos[b] = p;
                if (p > 0 && sd.clock_from.load(std::memory_order_relaxed) < 0)
                    sd.clock_from.store(f, std::memory_order_release);   // running starts with a block (ENGINE.md 4.2)
            }
            frames.store(f + B);
        }
    }

    void pump() {
        for (auto& s : side) s->e.run();
        for (auto& s : side)
            for (int u = 0; u < s->e.numUsers(); ++u)
                for (int i = 0; i < 32; ++i) {
                    const int ch = s->e.channelAt(u, i);
                    if (ch < 0) break;
                    if (s->e.channelVolume(u, ch) != 1.0f) s->e.setChannelVolume(u, ch, 1.0f);
                }
        if (extra) extra();
    }
    std::function<void()> extra;                 // more to run with every pump (a plain client)
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

    bool connect(int first = 0, int count = -1) {
        if (count < 0) count = opt.sides - first;
        for (int i = first; i < first + count; ++i)
            (*this)[i].e.connect("127.0.0.1", server.port(), "anonymous:" + (*this)[i].name, "");
        return until(
            [&] {
                for (int i = first; i < first + count; ++i) {
                    Side& s = (*this)[i];
                    if (s.e.status() != Status::Ok || (s.e.liveEnabled() && !s.e.liveRunning())) return false;
                }
                return true;
            },
            10);
    }
    bool allConnectedSee(int users) {
        return until(
            [&] {
                for (auto& s : side)
                    if (s->e.status() == Status::Ok && s->e.numUsers() != users) return false;
                return true;
            },
            10);
    }
    // Every side in `which` (all when empty) plays the room as `live`.
    bool roomIs(bool live, double seconds, std::vector<int> which = {}) {
        if (which.empty())
            for (int i = 0; i < opt.sides; ++i) which.push_back(i);
        const bool ok = until(
            [&] {
                for (int i : which)
                    if ((*this)[i].e.liveRoomLive() != live) return false;
                return true;
            },
            seconds);
        if (ok) {
            wait(0.05);                          // the host thread has recorded it
            return ok;
        }
        std::printf("    the room isn't %s after %.0f s; /live says:\n", live ? "Live" : "on intervals", seconds);
        for (int i : which) {
            Side& sd = (*this)[i];
            const std::size_t seen = sd.noticeCount();
            sd.e.liveCommand("/live");
            wait(0.05);
            sd.printNotices(seen);
        }
        return ok;
    }
    long block() const { return frames.load() / opt.block; }
    double secondsOf(long blocks) const { return blocks * double(opt.block) / opt.rate; }
    double ms(long frame_count) const { return frame_count * 1000.0 / opt.rate; }
    bool calm(long from, long to) {              // no hiccup between these frames
        std::lock_guard<std::mutex> lock(hm);
        for (long h : hiccups)
            if (h >= from && h <= to) return false;
        return true;
    }
    // The first block from `from_block` on where `s` plays the room as the
    // other mode than `was`, or -1; `frame` is where in it the switch fell.
    long roomSwitch(int s, long from_block, bool was, long* frame = nullptr) {
        const Side& sd = (*this)[s];
        const long done = block();
        for (long b = std::max(1L, from_block); b < done; ++b)
            if (sd.room[b - 1] == (was ? 1 : 0) && sd.room[b] != sd.room[b - 1]) {
                if (frame) *frame = b * opt.block + (sd.pos[b] <= opt.block ? opt.block - sd.pos[b] : 0);
                return b;
            }
        return -1;
    }
    // Whether an interval boundary fell into block `b` of `s`.
    bool boundaryIn(int s, long b) { return (*this)[s].pos[b] <= opt.block; }
};

// ------------------------------------------------------------ analysis

struct Steady {
    double rms = 0;
    double step = 0;                             // the largest sample-to-sample step
};

inline Steady steady(const float* x, long from, long to) {
    Steady st;
    double e = 0;
    for (long i = from; i < to; ++i) {
        e += double(x[i]) * x[i];
        if (i > from) st.step = std::max(st.step, double(std::fabs(x[i] - x[i - 1])));
    }
    st.rms = std::sqrt(e / std::max(1L, to - from));
    return st;
}

inline double dB(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

// The amplitude of the `hz` component over [from, to) (a single DFT bin).
inline double toneAmplitude(const float* x, long from, long to, double hz, int rate) {
    double re = 0, im = 0;
    for (long i = from; i < to; ++i) {
        const double w = 2 * M_PI * hz * i / rate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / std::max(1L, to - from);
}

// Around a switch at frame `s`: the steady sound before and after, and over
// the transition the largest step, the longest near-silence and how far the
// 10 ms RMS strays from the steady RMS.
struct Transition {
    Steady before, after;
    double step = 0;                             // the transition's largest step
    double step_ratio = 0;                       // over the steady sound's largest step
    double silence_ms = 0;                       // the longest run below -60 dBFS
    double rms_low_db = 0, rms_high_db = 0;      // the 10 ms RMS against the steady RMS, extremes
    double level_db = 0;                         // after against before
};

inline Transition transition(const float* x, long s, int rate) {
    Transition t;
    const long ms = rate / 1000;
    t.before = steady(x, s - 600 * ms, s - 100 * ms);
    t.after = steady(x, s + 100 * ms, s + 600 * ms);
    const long from = s - 50 * ms, to = s + 100 * ms;
    long run = 0, longest = 0;
    for (long i = from; i < to; ++i) {
        t.step = std::max(t.step, double(std::fabs(x[i] - x[i - 1])));
        run = std::fabs(x[i]) < 0.001f ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    t.silence_ms = longest * 1000.0 / rate;
    t.step_ratio = t.step / std::max(1e-9, std::max(t.before.step, t.after.step));
    t.rms_low_db = 1e9;
    t.rms_high_db = -1e9;
    for (long w = s - 40 * ms; w + 10 * ms <= s + 70 * ms; w += ms) {
        const double r = steady(x, w, w + 10 * ms).rms;
        const double d = dB(r / std::max(1e-12, t.before.rms));
        t.rms_low_db = std::min(t.rms_low_db, d);
        t.rms_high_db = std::max(t.rms_high_db, d);
    }
    t.level_db = dB(t.after.rms / std::max(1e-12, t.before.rms));
    return t;
}

inline void print(const char* what, const Transition& t) {
    std::printf("    %s: steady step %.4f/%.4f, transition step %.4f (x%.2f), longest near-silence %.2f ms, "
                "10 ms RMS %+.2f..%+.2f dB, level after %+.2f dB\n",
                what, t.before.step, t.after.step, t.step, t.step_ratio, t.silence_ms, t.rms_low_db, t.rms_high_db,
                t.level_db);
}

inline bool seamless(const Transition& t) {
    return t.step_ratio <= 1.5 && t.silence_ms <= 1.0 && t.rms_low_db >= -1.0 && t.rms_high_db <= 1.0;
}

// Clicks: for each 5 ms block from `from`, the largest second difference
// |x[n] - 2 x[n-1] + x[n-2]| (a step, or a bend, in the sound), against the
// median of the blocks within 200 ms either side of it.
struct ClickBlock {
    long frame = 0;                              // where the block starts
    double d2 = 0;
    double median = 0;
};
inline double secondDifference(const float* x, long from, long to) {
    double m = 0;
    for (long n = from; n < to; ++n) m = std::max(m, double(std::fabs(x[n] - 2 * x[n - 1] + x[n - 2])));
    return m;
}
inline std::vector<ClickBlock> clickBlocks(const float* x, long from, long to, int rate) {
    const long B = rate / 200, around = 40;      // 5 ms blocks; 200 ms either side
    std::vector<ClickBlock> v;
    for (long f = std::max(from, 2L); f + B <= to; f += B) {
        ClickBlock c;
        c.frame = f;
        c.d2 = secondDifference(x, f, f + B);
        v.push_back(c);
    }
    std::vector<double> w;
    for (std::size_t i = 0; i < v.size(); ++i) {
        w.clear();
        const std::size_t lo = i >= static_cast<std::size_t>(around) ? i - around : 0;
        const std::size_t hi = std::min(v.size(), i + around + 1);
        for (std::size_t k = lo; k < hi; ++k) w.push_back(v[k].d2);
        std::nth_element(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(w.size() / 2), w.end());
        v[i].median = std::max(1e-9, w[w.size() / 2]);
    }
    return v;
}

// How far behind `ref` the sound in x[from, to) is, within [lo, hi]
// frames: the lag with the best normalized correlation.
inline long bestLag(const float* x, const std::function<float(long)>& ref, long from, long to, long lo, long hi,
                    double* correlation = nullptr) {
    long best = lo;
    double best_c = -2;
    std::vector<float> r(static_cast<std::size_t>(to - from + hi - lo));
    for (long k = 0; k < static_cast<long>(r.size()); ++k) r[static_cast<std::size_t>(k)] = ref(from - hi + k);
    for (long d = lo; d <= hi; ++d) {
        double c = 0, ex = 0, er = 0;
        for (long n = from; n < to; ++n) {
            const double a = x[n], b = r[static_cast<std::size_t>(n - d - (from - hi))];
            c += a * b;
            ex += a * a;
            er += b * b;
        }
        const double v = ex > 0 && er > 0 ? c / std::sqrt(ex * er) : 0;
        if (v > best_c) {
            best_c = v;
            best = d;
        }
    }
    if (correlation) *correlation = best_c;
    return best;
}

// Steady tones in x[from, from + n): a least-squares fit of each frequency
// with an envelope of its own (a polynomial in time of degree `degree`, so
// that a fade or a slow change of phase is part of the tone); the
// amplitudes at the middle and the RMS of what is left, the rest.
inline void toneFit(const float* x, long from, int n, const std::vector<double>& hz, int rate, double* amp, double* rest,
                    int degree = 2) {
    const int q = degree + 1, p = 2 * q * static_cast<int>(hz.size());
    std::vector<double> m(static_cast<std::size_t>(p * p), 0.0), v(static_cast<std::size_t>(p), 0.0), col(v);
    auto basis = [&](int i) {
        const double c = i - (n - 1) / 2.0, t = c / (n / 2.0);
        for (std::size_t k = 0; k < hz.size(); ++k) {
            const double ph = 2 * M_PI * hz[k] * c / rate;
            double e = 1;
            for (int j = 0; j < q; ++j, e *= t) {
                col[2 * q * k + 2 * j] = std::cos(ph) * e;
                col[2 * q * k + 2 * j + 1] = std::sin(ph) * e;
            }
        }
    };
    for (int i = 0; i < n; ++i) {
        basis(i);
        for (int r = 0; r < p; ++r) {
            v[static_cast<std::size_t>(r)] += col[static_cast<std::size_t>(r)] * x[from + i];
            for (int c = 0; c < p; ++c) m[static_cast<std::size_t>(r * p + c)] += col[static_cast<std::size_t>(r)] * col[static_cast<std::size_t>(c)];
        }
    }
    for (int c = 0; c < p; ++c) {                // Gauss-Jordan with partial pivoting
        int piv = c;
        for (int r = c + 1; r < p; ++r)
            if (std::fabs(m[static_cast<std::size_t>(r * p + c)]) > std::fabs(m[static_cast<std::size_t>(piv * p + c)])) piv = r;
        for (int k = 0; k < p; ++k) std::swap(m[static_cast<std::size_t>(c * p + k)], m[static_cast<std::size_t>(piv * p + k)]);
        std::swap(v[static_cast<std::size_t>(c)], v[static_cast<std::size_t>(piv)]);
        const double d = m[static_cast<std::size_t>(c * p + c)];
        if (std::fabs(d) < 1e-12) continue;
        for (int k = 0; k < p; ++k) m[static_cast<std::size_t>(c * p + k)] /= d;
        v[static_cast<std::size_t>(c)] /= d;
        for (int r = 0; r < p; ++r) {
            const double f = r == c ? 0.0 : m[static_cast<std::size_t>(r * p + c)];
            if (f == 0) continue;
            for (int k = 0; k < p; ++k) m[static_cast<std::size_t>(r * p + k)] -= f * m[static_cast<std::size_t>(c * p + k)];
            v[static_cast<std::size_t>(r)] -= f * v[static_cast<std::size_t>(c)];
        }
    }
    double e2 = 0;
    for (int i = 0; i < n; ++i) {
        basis(i);
        double y = 0;
        for (int r = 0; r < p; ++r) y += col[static_cast<std::size_t>(r)] * v[static_cast<std::size_t>(r)];
        e2 += (x[from + i] - y) * (x[from + i] - y);
    }
    for (std::size_t k = 0; k < hz.size(); ++k) amp[k] = std::hypot(v[2 * q * k], v[2 * q * k + 1]);
    *rest = std::sqrt(e2 / n);
}

// The delay of a steady sine, sin(2 pi hz n / rate) where it is played,
// in x[from, to): from its phase (a Hann window), in frames from 0 up to
// its period. A delay longer by whole periods looks the same.
inline double sineDelay(const float* x, long from, long to, double hz, int rate) {
    const double w = 2 * M_PI * hz / rate;
    double s = 0, c = 0;
    for (long n = from; n < to; ++n) {
        const double win = 0.5 - 0.5 * std::cos(2 * M_PI * (static_cast<double>(n - from) + 0.5) / (to - from));
        s += win * x[n] * std::sin(w * static_cast<double>(n));
        c += win * x[n] * std::cos(w * static_cast<double>(n));
    }
    const double wd = std::atan2(-c, s);         // A sin(w (n - d)) = A (sin wn cos wd - cos wn sin wd)
    return (wd < 0 ? wd + 2 * M_PI : wd) / w;
}

// ------------------------------------------------------------ WAV

inline bool writeWav(const std::string& path, const float* x, long n, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](std::uint32_t v) {
        unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                              static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto u16 = [&](std::uint16_t v) {
        unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    const std::uint32_t bytes = static_cast<std::uint32_t>(n) * 2;
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);                                      // PCM
    u16(1);                                      // mono
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 2);
    u16(2);
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (long i = 0; i < n; ++i) {
        const float v = std::max(-1.0f, std::min(1.0f, x[i]));
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(v * 32767.0f))));
    }
    std::fclose(f);
    return true;
}

}  // namespace harness

#endif  // O11PROTO_ENGINE_TESTS_LIVE_HARNESS_H
