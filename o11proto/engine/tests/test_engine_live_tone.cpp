// Live mode end to end with a steady tone (ENGINE.md 16.6, 16.8; live-mode
// spec 5.9, 6.9): two engines through o11server, each called by an audio
// thread of its own every 128 frames on a clock of its own, as two players
// with audio interfaces of their own are: bob's clock runs `ppm` faster than
// alice's. alice plays a steady 1 kHz sine and bob a 750 Hz one, and each
// records what it hears of the other.
//
// What is heard must be the tone and nothing else: no step in its level or
// its phase larger than steady playback has, whatever the buffer and the
// speed do to keep up with the clocks. The check demodulates the tone heard
// (4 ms Hann windows, 1 ms apart): a window whose level is more than 5% off
// the run's median, or whose phase bends by more than 0.05 rad within 4 ms
// on either side (the phase's second difference: a steady tone heard at a
// slightly different rate has a phase that only drifts, linearly), is a dip.
// A concealment within 250 ms after the machine stalled is the machine's,
// and is reported but not counted: after either host, the server's thread
// (its poll of 1 ms) or a thread scheduled like the Live network threads
// woke more than 3 ms late, or a datagram that thread sends itself over
// the loopback every millisecond took more than 3 ms to arrive. On a loaded
// machine the server's relay thread alone is sometimes not run for tens of
// ms, or the loopback holds every datagram that long, while every thread
// is on time; both listeners then run dry at once. So two listeners that
// begin to conceal within 10 ms of each other are the machine's too: their
// sinks share nothing, and only the way between them could stall both
// (seen here with all of the threads above on time, each stream's blocks
// 15-18 ms late). Each dip is shown with what the listener's Live
// log (Engine::liveUserEvents()) says happened around it, and each run with
// the listener's counts (Engine::liveUserCounters()).
//
// Arguments: a name filter, then the seconds each run records (20).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "check.h"
#include "o11proto/engine.h"
#include "o11proto/realtime.h"
#include "o11proto/server.h"
#include "socket_compat.h"
#include "udp_compat.h"

using namespace o11proto;
using namespace o11proto::engine;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 128;
constexpr double kAmp = 0.5;
constexpr int kWin = kRate * 4 / 1000;           // the demodulation's window
constexpr int kHop = kRate / 1000;
constexpr int kBend = 4;                         // hops either side of the phase's second difference
constexpr double kLevelTol = 0.05;
constexpr double kPhaseTol = 0.05;               // rad
constexpr double kLateMs = 3.0;                  // a wake this late is a stall of the machine's
constexpr double kExcuseMs = 250.0;
constexpr std::int64_t kBothNs = 10000000;       // 10 ms: two listeners concealing "at once"
double g_seconds = 20.0;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Side {
    Engine e;
    std::string name;
    double hz = 1000.0;
    double ppm = 0;                              // its clock, against the room's
    // How its host calls: 0 on every period, 1 two blocks back to back every
    // other period (a device period of 256 frames), 2 each wake up to 2.5 ms
    // late at random.
    int style = 0;
    long max_blocks = 0;
    std::unique_ptr<float[]> heard;              // output 0
    std::unique_ptr<std::int64_t[]> block_ns;    // when each block's process() began (steady clock)
    std::unique_ptr<float[]> late_ms;            // how late its wake was
    std::unique_ptr<std::uint8_t[]> live;        // userLive(0) after each block
    std::atomic<long> blocks{0};
    std::atomic<bool> stop{false};
    std::thread audio;
    // The listener's Live log of the other, as the main thread collects it.
    std::vector<LiveEvent> events;
    std::set<std::tuple<std::int64_t, int, std::int64_t>> seen;
    double fill_lo = 1e9, fill_hi = -1;

    void collect() {
        for (const LiveEvent& ev : e.liveUserEvents(0))
            if (seen.insert(std::make_tuple(ev.steady_ns, static_cast<int>(ev.kind), ev.value)).second)
                events.push_back(ev);
        const double f = e.liveUserBufferMs(0);
        if (f >= 0) {
            fill_lo = std::min(fill_lo, f);
            fill_hi = std::max(fill_hi, f);
        }
    }

    // Its host: a block every 128 frames of its own clock. After a late
    // wake it calls again at once to catch up, as hosts do.
    void audioLoop() {
        std::vector<float> in(kBlock), out0(kBlock), out1(kBlock);
        thread::makeAudioThread(static_cast<double>(kBlock) / kRate);
        const double period = 1e9 * kBlock / (kRate * (1.0 + ppm * 1e-6));
        auto t0 = Clock::now();
        long n = 0;                              // its own frames
        std::uint32_t rnd = 12345u + static_cast<std::uint32_t>(hz);
        for (long k = 0; !stop.load(); ++k) {
            double at = k * period;
            if (style == 1) at = (k - k % 2) * period;
            if (style == 2) {
                rnd = rnd * 1103515245u + 12345u;
                at += 2.5e6 * ((rnd >> 8) & 0xffff) / 65536.0;
            }
            const auto due = t0 + std::chrono::nanoseconds(static_cast<long long>(at));
            std::this_thread::sleep_until(due);
            const auto now = Clock::now();
            const double late = std::chrono::duration<double, std::milli>(now - due).count();
            if (late > 1000) t0 = now - std::chrono::nanoseconds(static_cast<long long>(k * period));
            if (k >= max_blocks) continue;
            for (int i = 0; i < kBlock; ++i) in[static_cast<std::size_t>(i)] = static_cast<float>(kAmp * std::sin(2 * M_PI * hz * double(n + i) / kRate));
            n += kBlock;
            const float* inp[1] = {in.data()};
            float* o[2] = {out0.data(), out1.data()};
            block_ns[k] = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            late_ms[k] = static_cast<float>(late);
            e.process(inp, 1, o, 2, kBlock, kRate);
            std::copy(out0.begin(), out0.end(), heard.get() + k * kBlock);
            live[k] = e.userLive(0) ? 1 : 0;
            blocks.store(k + 1, std::memory_order_release);
        }
    }
};

struct ToneRoom {
    Server server;
    std::thread server_thread, watch;
    std::atomic<bool> server_stop{false}, watch_stop{false};
    Side side[2];                                // alice, bob
    std::mutex hm;
    std::vector<std::pair<std::int64_t, double>> plain_late;   // a network-like thread's late wakes (ns, ms)
    std::vector<std::pair<std::int64_t, double>> loop_late;    // its loopback datagrams that came late (ns, ms)
    std::vector<std::pair<std::int64_t, double>> server_slow;  // the server's late polls (end ns, ms past its 1 ms)

    ToneRoom(double ppm, double seconds) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.anonymous_privs = kPrivChat | kPrivVote;
        cfg.allow_codecs = kCapOpus | kCapFlac;
        cfg.opus_kbps = 128;
        cfg.bpm = 120;
        cfg.bpi = 4;
        cfg.live.mode = LiveMode::On;
        bool ok = server.start(cfg);
        for (int i = 0; ok && !server.liveUdpOpen() && i < 5; ++i) {
            std::printf("    UDP port %u is taken: the server moves\n", server.port());
            server.stop();
            ok = server.start(cfg);
        }
        CHECK(ok);
        server_thread = std::thread([this] {
            thread::makeLatencyCritical();
            while (!server_stop.load()) {
                const auto t0 = Clock::now();
                server.poll(1);
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count() - 1.0;
                if (ms > kLateMs) {              // a poll of 1 ms that took far longer: the relay was not run
                    std::lock_guard<std::mutex> lock(hm);
                    server_slow.push_back({nowNs(), ms});
                }
            }
        });
        watch = std::thread([this] {
            thread::makeLatencyCritical();
            // A datagram to itself over the loopback every millisecond,
            // stamped with when it went: how long the loopback took.
            socket_t fd = udp::open(1 << 16, false);
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(0x7f000001);
            socklen_t len = sizeof(sa);
            udp::Addr self;
            const bool probe = fd != kInvalidSocket && ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0 &&
                               ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0;
            self.ip = sa.sin_addr.s_addr;
            self.port = sa.sin_port;
            while (!watch_stop.load()) {
                const auto t = Clock::now() + std::chrono::milliseconds(1);
                std::this_thread::sleep_until(t);
                const double late = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
                if (late > kLateMs) {
                    std::lock_guard<std::mutex> lock(hm);
                    plain_late.push_back({nowNs(), late});
                }
                if (!probe) continue;
                for (;;) {
                    std::uint8_t b[16];
                    std::size_t n = 0;
                    bool trunc = false, has_local = false;
                    udp::Addr from, local;
                    if (udp::receive(fd, b, sizeof(b), n, trunc, from, local, has_local) != udp::Recv::Datagram) break;
                    if (n != 8) continue;
                    std::int64_t sent = 0;
                    std::memcpy(&sent, b, 8);
                    const std::int64_t at = nowNs();
                    const double took = (at - sent) / 1e6;
                    if (took > kLateMs) {
                        std::lock_guard<std::mutex> lock(hm);
                        loop_late.push_back({at, took});
                    }
                }
                const std::int64_t sent = nowNs();
                std::uint8_t b[8];
                std::memcpy(b, &sent, 8);
                udp::sendTo(fd, b, sizeof(b), self, 0, false);
            }
            if (fd != kInvalidSocket) compat::closeSocket(fd);
        });
        const char* names[2] = {"alice", "bob"};
        for (int i = 0; i < 2; ++i) {
            Side& s = side[i];
            s.name = names[i];
            s.hz = i == 0 ? 1000.0 : 750.0;
            s.ppm = i == 0 ? 0.0 : ppm;
            if (const char* st = std::getenv("O11_TONE_STYLE")) s.style = std::atoi(st);
            s.max_blocks = static_cast<long>((seconds + 40.0) * kRate / kBlock);
            s.heard.reset(new float[static_cast<std::size_t>(s.max_blocks * kBlock)]());
            s.block_ns.reset(new std::int64_t[static_cast<std::size_t>(s.max_blocks)]());
            s.late_ms.reset(new float[static_cast<std::size_t>(s.max_blocks)]());
            s.live.reset(new std::uint8_t[static_cast<std::size_t>(s.max_blocks)]());
            s.e.setMetronome(0, 0, true, 0);
            LocalChannel lc;
            lc.name = "tone";
            lc.broadcast = true;
            lc.mute = true;                      // no local monitoring: the output is the other's audio only
            s.e.setLocalChannel(0, lc);
        }
        for (Side& s : side) s.audio = std::thread([&s] { s.audioLoop(); });
    }
    ~ToneRoom() {
        for (Side& s : side) s.stop.store(true);
        for (Side& s : side) s.audio.join();
        watch_stop.store(true);
        watch.join();
        for (Side& s : side) s.e.disconnect();
        for (int i = 0; i < 20; ++i) pump();
        server_stop.store(true);
        server_thread.join();
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
        for (Side& s : side) s.collect();
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
        return until([&] { return side[0].e.userLive(0) && side[1].e.userLive(0); }, seconds);
    }
    // The machine's stalls that reach into the `ms` before steady time
    // `ns`: a host's wake, the server's or the network-like thread's, or
    // that thread's loopback datagram, more than kLateMs late (a wake at w,
    // d late, is a stall from w - d to w). The worst of alice's host, bob's,
    // the thread's (its wake or its datagram) and the server's, in ms.
    void stallsBefore(std::int64_t ns, double ms, double worst[4]) {
        const std::int64_t from = ns - static_cast<std::int64_t>(ms * 1e6);
        for (int i = 0; i < 2; ++i) {
            Side& s = side[i];
            worst[i] = 0;
            const long n = s.blocks.load(std::memory_order_acquire);
            for (long k = 0; k < n; ++k) {
                const std::int64_t w = s.block_ns[k], d = static_cast<std::int64_t>(s.late_ms[k] * 1e6);
                if (w >= from && w - d <= ns) worst[i] = std::max(worst[i], static_cast<double>(s.late_ms[k]));
            }
        }
        worst[2] = worst[3] = 0;
        std::lock_guard<std::mutex> lock(hm);
        for (const auto* v : {&plain_late, &loop_late})
            for (const auto& p : *v)
                if (p.first >= from && p.first - static_cast<std::int64_t>(p.second * 1e6) <= ns)
                    worst[2] = std::max(worst[2], p.second);
        for (const auto& p : server_slow)
            if (p.first >= from && p.first - static_cast<std::int64_t>(p.second * 1e6) <= ns) worst[3] = std::max(worst[3], p.second);
    }
};

// The tone as `l` heard it, frames [from, to) of its output: each window's
// level and phase.
struct Demod {
    std::vector<double> level, phase;            // per hop
    long from = 0;
};
Demod demodulate(const Side& l, double hz, long from, long to) {
    Demod d;
    d.from = from;
    static std::vector<double> w;
    if (w.empty()) {
        w.resize(kWin);
        for (int i = 0; i < kWin; ++i) w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kWin);
    }
    double wsum = 0;
    for (double v : w) wsum += v;
    for (long at = from; at + kWin <= to; at += kHop) {
        std::complex<double> z = 0;
        for (int i = 0; i < kWin; ++i) {
            const double t = 2 * M_PI * hz * double(at + i) / kRate;
            z += w[static_cast<std::size_t>(i)] * l.heard[at + i] * std::complex<double>(std::cos(t), -std::sin(t));
        }
        d.level.push_back(2.0 * std::abs(z) / wsum);
        d.phase.push_back(std::arg(z));
    }
    return d;
}

double wrapPi(double a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a < -M_PI) a += 2 * M_PI;
    return a;
}

struct Dip {
    long at = 0, end = 0;                        // frames of the listener's output
    double low = 1, high = 1, bend = 0;          // its level against the median, its largest bend
};

struct Result {
    std::vector<Dip> dips;
    double median = 0;
    double steady_level = 0, steady_bend = 0;    // the largest of each outside dips
    double bend_p999 = 0;
};

Result findDips(const Side& l, double hz, long from, long to) {
    Result r;
    const Demod d = demodulate(l, hz, from, to);
    const std::size_t n = d.level.size();
    if (n < 2 * kBend + 2) return r;
    std::vector<double> s = d.level;
    std::nth_element(s.begin(), s.begin() + static_cast<long>(n / 2), s.end());
    r.median = s[n / 2];
    std::vector<double> bend(n, 0.0);
    for (std::size_t k = kBend; k + kBend < n; ++k)
        bend[k] = std::fabs(wrapPi(d.phase[k + kBend] - d.phase[k]) - wrapPi(d.phase[k] - d.phase[k - kBend]));
    std::vector<double> b2 = bend;
    std::sort(b2.begin(), b2.end());
    r.bend_p999 = b2[std::min(n - 1, n * 999 / 1000)];
    std::vector<bool> bad(n, false);
    for (std::size_t k = 0; k < n; ++k) {
        const double lv = r.median > 0 ? d.level[k] / r.median : 0;
        bad[k] = std::fabs(lv - 1.0) > kLevelTol || bend[k] > kPhaseTol;
    }
    for (std::size_t k = 0; k < n;) {
        if (!bad[k]) {
            const double lv = r.median > 0 ? d.level[k] / r.median : 0;
            r.steady_level = std::max(r.steady_level, std::fabs(lv - 1.0));
            r.steady_bend = std::max(r.steady_bend, bend[k]);
            ++k;
            continue;
        }
        Dip dip;
        std::size_t j = k, last = k;
        while (j < n && j <= last + 8) {         // flagged windows less than 8 ms apart are one dip
            if (bad[j]) {
                last = j;
                const double lv = r.median > 0 ? d.level[j] / r.median : 0;
                dip.low = std::min(dip.low, lv);
                dip.high = std::max(dip.high, lv);
                dip.bend = std::max(dip.bend, bend[j]);
            }
            ++j;
        }
        dip.at = from + static_cast<long>(k) * kHop;
        dip.end = from + static_cast<long>(last) * kHop + kWin;
        r.dips.push_back(dip);
        k = last + 1;
    }
    return r;
}

// The steady time of the listener's output frame `f`.
std::int64_t frameNs(const Side& l, long f) {
    const long k = f / kBlock;
    return l.block_ns[k] + static_cast<std::int64_t>((f - k * kBlock) * 1e9 / kRate);
}

void printCounters(const Side& l) {
    LiveCounters c;
    if (!l.e.liveUserCounters(0, &c)) {
        std::printf("    %s: no counters\n", l.name.c_str());
        return;
    }
    std::printf("    %s's counts: %u late, %u lost, %.1f ms concealed, %u underruns, %u overflows, %u xruns, %u skips "
                "(%lld frames), %u host gaps, %u speed changes (moved %.2f ms, now %+.0f ppm), fill %.2f ms "
                "(%.2f-%.2f ms in the run)\n",
                l.name.c_str(), c.late, c.lost, c.concealed_frames / 48.0, c.underruns, c.overflows, c.xrun_gaps, c.skips,
                static_cast<long long>(c.skipped_frames), c.host_gaps, c.speed_changes, c.speed_moved_ms,
                c.speed_ppm, c.fill_ms, l.fill_lo, l.fill_hi);
}

bool concealing(LiveEventKind k) {               // the stream's own audio isn't played there
    switch (k) {
    case LiveEventKind::Lost:
    case LiveEventKind::Dry:
    case LiveEventKind::Xrun:
    case LiveEventKind::HostGap:
    case LiveEventKind::Overflow:
    case LiveEventKind::Rebuffer:
    case LiveEventKind::Skip: return true;
    default: return false;
    }
}

void printEvent(const LiveEvent& ev, std::int64_t t0) {
    std::printf(" %s %lld at %+.1f ms;", liveEventName(ev.kind), static_cast<long long>(ev.value), (ev.steady_ns - t0) / 1e6);
}

struct RunCounts {
    int dips = 0, dips_unexplained = 0;          // a dip without a concealment there: a correction heard
    int concealments = 0, concealments_unexplained = 0;   // one without the machine stalling before it
};

// One run: the clocks `ppm` apart, `seconds` recorded once both have heard
// each other live for 3 s. A dip must come with a concealment in the
// listener's log (from 150 ms before it to 20 ms into it), and every
// concealment must follow a stall of the machine's (kExcuseMs before it).
RunCounts toneRun(double ppm, double seconds) {
    RunCounts n;
    std::printf("  the clocks %.1f ppm apart, %.0f s:\n", ppm, seconds);
    ToneRoom r(ppm, seconds);
    if (!r.connect() || !r.bothLive(20)) {
        CHECK(!"both connected and live");
        n.dips_unexplained = -1;
        return n;
    }
    r.wait(3.0);
    long from[2], to[2];
    for (int i = 0; i < 2; ++i) from[i] = r.side[i].blocks.load() * kBlock;
    for (Side& s : r.side) s.events.clear(), s.fill_lo = 1e9, s.fill_hi = -1;
    r.wait(seconds);
    for (int i = 0; i < 2; ++i) to[i] = r.side[i].blocks.load() * kBlock;
    for (int i = 0; i < 2; ++i) {
        Side& l = r.side[i];
        const Side& o = r.side[1 - i];
        bool all_live = true;
        for (long k = from[i] / kBlock; k < to[i] / kBlock; ++k) all_live = all_live && l.live[k];
        CHECK(all_live);
        const std::int64_t t_from = frameNs(l, from[i]), t_to = frameNs(l, to[i] - 1);
        const Result res = findDips(l, o.hz, from[i], to[i]);
        for (const Dip& d : res.dips) {
            const std::int64_t t = frameNs(l, d.at);
            bool explained = false;
            for (const LiveEvent& ev : l.events) {
                const double ms = (ev.steady_ns - t) / 1e6;
                if (ms >= -150 && ms <= 20 && (concealing(ev.kind) || ev.kind == LiveEventKind::Rejoin)) explained = true;
            }
            ++n.dips;
            n.dips_unexplained += explained ? 0 : 1;
            std::printf("    %s hears a dip at %.3f s, %.1f ms: level %.2f-%.2f, bend %.2f rad%s; the log:", l.name.c_str(),
                        (t - t_from) / 1e9, (d.end - d.at) * 1000.0 / kRate, d.low, d.high, d.bend,
                        explained ? "" : " (NO CONCEALMENT THERE)");
            int shown = 0;
            for (const LiveEvent& ev : l.events) {
                const double ms = (ev.steady_ns - t) / 1e6;
                if (ms < -150 || ms > 30) continue;
                printEvent(ev, t);
                ++shown;
            }
            std::printf("%s\n", shown ? "" : " nothing");
        }
        std::printf("    %s: the tone at %.3f, steady within %.3f of it and bending at most %.3f rad (99.9%%: %.3f); %d dips\n",
                    l.name.c_str(), res.median, res.steady_level, res.steady_bend, res.bend_p999,
                    static_cast<int>(res.dips.size()));
        for (const LiveEvent& ev : l.events) {
            if (!concealing(ev.kind) || ev.steady_ns < t_from || ev.steady_ns > t_to) continue;
            double worst[4];
            r.stallsBefore(ev.steady_ns, kExcuseMs, worst);
            // Both listeners concealing at once (their sinks share nothing):
            // the way between them stalled, though no thread watched did.
            bool both = false;
            for (const LiveEvent& e2 : o.events)
                both |= concealing(e2.kind) && std::llabs(e2.steady_ns - ev.steady_ns) <= kBothNs;
            const bool machine =
                worst[0] > kLateMs || worst[1] > kLateMs || worst[2] > kLateMs || worst[3] > kLateMs || both;
            ++n.concealments;
            n.concealments_unexplained += machine ? 0 : 1;
            std::printf("    %s conceals at %.3f s (%s %lld) after alice's host woke %.1f ms late, bob's %.1f, a network-like "
                        "thread or its loopback %.1f, the server %.1f%s%s; then:",
                        l.name.c_str(), (ev.steady_ns - t_from) / 1e9, liveEventName(ev.kind),
                        static_cast<long long>(ev.value), worst[0], worst[1], worst[2], worst[3],
                        both ? ", and so did the other listener" : "", machine ? "" : " (NO STALL BEFORE IT)");
            for (const LiveEvent& e2 : l.events) {
                const double ms = (e2.steady_ns - ev.steady_ns) / 1e6;
                if (ms > 0 && ms <= 100 && (e2.kind == LiveEventKind::Late || e2.kind == LiveEventKind::Rejoin ||
                                            concealing(e2.kind)))
                    printEvent(e2, ev.steady_ns);
            }
            std::printf("\n");
        }
        int kinds[16] = {};
        for (const LiveEvent& ev : l.events) kinds[static_cast<int>(ev.kind) & 15]++;
        std::printf("    %s's log in the run:", l.name.c_str());
        for (int k = 1; k < 16; ++k)
            if (kinds[k]) std::printf(" %d %s;", kinds[k], liveEventName(static_cast<LiveEventKind>(k)));
        std::printf("\n");
        printCounters(l);
        CHECK(res.median > 0.8 * kAmp && res.median < 1.2 * kAmp);
    }
    float worst[2] = {0, 0};
    for (int i = 0; i < 2; ++i)
        for (long k = from[i] / kBlock; k < to[i] / kBlock; ++k) worst[i] = std::max(worst[i], r.side[i].late_ms[k]);
    double srv = 0;
    int srv_n = 0;
    {
        std::lock_guard<std::mutex> lock(r.hm);
        for (const auto& p : r.server_slow) srv = std::max(srv, p.second), ++srv_n;
    }
    double loop = 0;
    int loop_n = 0;
    {
        std::lock_guard<std::mutex> lock(r.hm);
        for (const auto& p : r.loop_late) loop = std::max(loop, p.second), ++loop_n;
    }
    std::printf("    the hosts woke at most %.1f and %.1f ms late; a network-like thread %d times over %.0f ms, its "
                "loopback %d times (at most %.1f ms); the server %d times, at most %.1f ms; %d dips (%d without a "
                "concealment), %d concealments (%d without a stall)\n",
                worst[0], worst[1], static_cast<int>(r.plain_late.size()), kLateMs, loop_n, loop, srv_n, srv, n.dips,
                n.dips_unexplained, n.concealments, n.concealments_unexplained);
    return n;
}

}  // namespace

// The clocks as one, 50 ppm and 200 ppm apart: a steady tone without a dip.
TEST(steady_tone_without_dips) {
    std::vector<double> ppms = {0.0, 50.0, 200.0};
    if (const char* v = std::getenv("O11_TONE_PPM")) ppms = {std::atof(v)};
    for (double ppm : ppms) {
        const RunCounts n = toneRun(ppm, g_seconds);
        CHECK(n.dips_unexplained == 0);
        CHECK(n.concealments_unexplained == 0);
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    if (argc > 2) g_seconds = std::max(5.0, std::atof(argv[2]));
    if (argc > 1) {
        auto& c = testing::cases();
        const std::string want = argv[1];         // only the cases whose names contain it
        auto other = [&](const testing::Case& k) { return std::string(k.name).find(want) == std::string::npos; };
        c.erase(std::remove_if(c.begin(), c.end(), other), c.end());
    }
    return testing::runAll();
}
