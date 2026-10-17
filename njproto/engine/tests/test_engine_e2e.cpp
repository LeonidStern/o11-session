// ENGINE.md 13.10-13.13: two engines against njserver, simulated clock.
#include <cmath>
#include <vector>

#include "check.h"
#include "njproto/client.h"
#include "njproto/engine.h"
#include "njproto/server.h"

using namespace njproto;
using namespace njproto::engine;

namespace {

double snrAgainstTone(const float* x, std::size_t n, double hz, int rate) {
    double sc = 0, ss = 0, cc = 0, ssn = 0, scn = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double w = 2 * M_PI * hz * static_cast<double>(i) / rate;
        double s = std::sin(w), c = std::cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ssn += s * x[i];
        scn += c * x[i];
    }
    double det = ss * cc - sc * sc;
    double a = (ssn * cc - scn * sc) / det, b = (scn * ss - ssn * sc) / det;
    double sig = 0, noise = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double w = 2 * M_PI * hz * static_cast<double>(i) / rate;
        double fit = a * std::sin(w) + b * std::cos(w);
        sig += fit * fit;
        noise += (x[i] - fit) * (x[i] - fit);
    }
    return noise > 0 ? 10 * std::log10(sig / noise) : 200.0;
}

double goertzelPower(const float* x, std::size_t n, double hz, int rate) {
    double w = 2 * M_PI * hz / rate, c = 2 * std::cos(w), s1 = 0, s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

// Two engines (sender, listener) on one server with a simulated audio clock.
struct Room {
    Server server;
    Engine sender, listener;
    int rate, block;
    double hz = 997.0, amp = 0.5;
    std::function<double()> freq;               // optional per-block tone frequency
    double phase = 0;
    std::vector<float> in, sl, sr, ll, lr;
    std::vector<float> heard;                    // listener output 0
    long frames = 0;
    bool listener_runs = true;

    Room(int rate_, int block_, int bpm = 120, int bpi = 8) : rate(rate_), block(block_) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.allow_codecs = kCapOpus | kCapFlac;
        cfg.opus_kbps = 128;
        cfg.bpm = static_cast<std::uint16_t>(bpm);
        cfg.bpi = static_cast<std::uint16_t>(bpi);
        CHECK(server.start(cfg));
        in.assign(static_cast<std::size_t>(block), 0.0f);
        sl = sr = ll = lr = in;
        sender.setMetronome(0, 0, true, 0);
        listener.setMetronome(0, 0, true, 0);
        LocalChannel lc;
        lc.name = "tone";
        lc.broadcast = true;
        lc.mute = true;
        sender.setLocalChannel(0, lc);
    }
    void control() {
        server.poll(0);
        for (int i = 0; i < 3; ++i) {
            bool idle = sender.run();
            idle = listener.run() && idle;
            if (idle) break;
        }
        for (int u = 0; u < listener.numUsers(); ++u)
            for (int i = 0; i < 32; ++i) {
                int ch = listener.channelAt(u, i);
                if (ch < 0) break;
                if (listener.channelVolume(u, ch) != 1.0f) listener.setChannelVolume(u, ch, 1.0f);
            }
    }
    bool connectBoth() {
        sender.connect("127.0.0.1", server.port(), "anonymous:sender", "");
        listener.connect("127.0.0.1", server.port(), "anonymous:listener", "");
        TimePoint deadline = Clock::now() + std::chrono::seconds(5);
        while (!(sender.status() == Status::Ok && listener.status() == Status::Ok && sender.serverBpi() > 0 &&
                 listener.serverBpi() > 0 && listener.numUsers() == 1)) {
            if (Clock::now() > deadline) return false;
            control();
            server.poll(1);
        }
        control();
        return true;
    }
    // Advances the simulated clock by `seconds` of audio.
    void advance(double seconds) {
        long n = static_cast<long>(seconds * rate);
        for (long done = 0; done < n; done += block) {
            double f = freq ? freq() : hz;
            for (int i = 0; i < block; ++i) {
                in[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(phase));
                phase += 2 * M_PI * f / rate;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
            }
            const float* inp[1] = {in.data()};
            float* so[2] = {sl.data(), sr.data()};
            float* lo[2] = {ll.data(), lr.data()};
            sender.process(inp, 1, so, 2, block, rate);
            if (listener_runs) {
                listener.process(inp, 0, lo, 2, block, rate);
                heard.insert(heard.end(), ll.begin(), ll.end());
            }
            frames += block;
            control();
        }
    }
    int intervalLen() const { return listener.usualIntervalLength(); }
    // SNR and peak over the last two intervals heard.
    void lastTwo(double* snr, float* peak) const {
        std::size_t take = std::min(heard.size(), static_cast<std::size_t>(intervalLen()) * 2);
        const float* p = heard.data() + (heard.size() - take);
        *snr = snrAgainstTone(p, take, hz, rate);
        *peak = 0;
        for (std::size_t i = 0; i < take; ++i) *peak = std::max(*peak, std::fabs(p[i]));
    }
};

}  // namespace

TEST(two_engines_opus_then_flac_48k) {           // 13.10
    Room room(48000, 512);
    CHECK(room.connectBoth());
    CHECK_EQ(room.listener.numUsers(), 1);
    CHECK_EQ(room.listener.channelAt(0, 0), 0);
    CHECK_EQ(room.listener.channelName(0, 0), "tone");
    CHECK_EQ(room.sender.uploadCodec(), kFourccOpus);
    CHECK_EQ(room.sender.roomOpusKbps(), 128);
    room.advance(12.0);
    CHECK_EQ(room.sender.uploadKbps(0), 128);
    double snr;
    float peak;
    room.lastTwo(&snr, &peak);
    std::printf("  opus 48k: snr %.1f dB, peak %.3f\n", snr, peak);
    CHECK(snr >= 35.0);
    CHECK(peak >= 0.45f && peak <= 0.55f);
    CHECK(room.listener.channelPeak(0, 0, 0) > 0.3f);

    room.sender.say("!codec flac");
    // The interval already begun stays Opus and reaches the listener one
    // interval later, so after 20 s the last two intervals are FLAC following
    // FLAC (an Opus-to-FLAC boundary still carries the Opus crossfade).
    room.advance(20.0);
    CHECK_EQ(room.sender.uploadCodec(), kFourccFlac);
    room.lastTwo(&snr, &peak);
    std::printf("  flac 48k: snr %.1f dB, peak %.3f\n", snr, peak);
    CHECK(snr >= 60.0);
    CHECK(peak >= 0.45f && peak <= 0.55f);
}

TEST(two_engines_opus_44k1) {                    // 13.11
    Room room(44100, 512);
    CHECK(room.connectBoth());
    room.advance(12.0);
    double snr;
    float peak;
    room.lastTwo(&snr, &peak);
    std::printf("  opus 44.1k: snr %.1f dB, peak %.3f\n", snr, peak);
    CHECK(snr >= 30.0);
    CHECK(peak >= 0.45f && peak <= 0.55f);
}

TEST(transmit_off_and_on) {                      // 13.12
    Room room(48000, 512);
    // A raw observer subscribed to the sender counts zero-GUID BEGINs.
    Client observer;
    int silence_begins = 0, real_begins = 0;
    observer.on_download_begin = [&](const DownloadBegin& d) {
        if (guidIsZero(d.guid)) ++silence_begins;
        else ++real_begins;
    };
    Client::Options o;
    o.username = "anonymous:observer";
    o.auto_subscribe = true;
    o.codec_caps = kCapOpus | kCapFlac;           // else the room would fall back to "no codec"
    CHECK(observer.connect("127.0.0.1", room.server.port(), o));
    CHECK(room.connectBoth());
    for (int i = 0; i < 200 && !observer.loggedIn(); ++i) {
        observer.poll(5);
        room.control();
    }
    CHECK(observer.loggedIn());
    auto step = [&](double s) {
        double chunk = 0.25;
        for (double t = 0; t < s; t += chunk) {
            room.advance(chunk);
            observer.poll(0);
        }
    };
    step(6.0);                                    // one and a half intervals of audio
    LocalChannel lc;
    CHECK(room.sender.localChannel(0, &lc));
    lc.broadcast = false;
    room.sender.setLocalChannel(0, lc);
    int before = real_begins;
    int loop_at_off = room.sender.loopCount();
    step(4.0);                                    // the current interval ends whole
    CHECK(room.sender.loopCount() > loop_at_off);
    step(8.0);                                    // off for two more intervals
    CHECK_EQ(silence_begins, 1);
    CHECK_EQ(real_begins, before + 0);            // nothing more than the interval already begun
    // While off, after the listener's current interval ended, its output is exactly 0.
    std::size_t ilen = static_cast<std::size_t>(room.intervalLen());
    const float* tail = room.heard.data() + room.heard.size() - ilen;
    bool all_zero = true;
    for (std::size_t i = 0; i < ilen; ++i) all_zero = all_zero && tail[i] == 0.0f;
    CHECK(all_zero);
    lc.broadcast = true;
    room.sender.setLocalChannel(0, lc);
    step(14.0);                                   // 32 s in all: whole listener intervals
    CHECK(real_begins >= before + 2);
    CHECK_EQ(silence_begins, 1);
    double snr;
    float peak;
    room.lastTwo(&snr, &peak);
    std::printf("  after resume: snr %.1f dB, peak %.3f\n", snr, peak);
    CHECK(snr >= 35.0);
    observer.disconnect();
}

TEST(stalled_listener_plays_newest) {            // 13.13
    Room room(48000, 512);
    // The sender's tone frequency names its interval: 300 + 50 * loopCount.
    room.freq = [&] { return 300.0 + 50.0 * room.sender.loopCount(); };
    CHECK(room.connectBoth());
    room.advance(10.0);
    room.listener_runs = false;                   // process() not called for 20 s
    room.advance(20.0);
    int newest = room.sender.loopCount();         // intervals up to this one were sent
    room.listener_runs = true;
    // Resume: finish the interval that was playing, then look at the next two.
    int pos = 0, len = 0;
    room.listener.position(&pos, &len);
    room.heard.clear();
    room.advance(static_cast<double>(len - pos) / room.rate);
    room.heard.clear();
    std::size_t ilen = static_cast<std::size_t>(room.intervalLen());
    room.advance(2.0 * static_cast<double>(ilen) / room.rate);
    CHECK(room.heard.size() >= 2 * ilen);
    auto identify = [&](const float* p, std::size_t n) {
        int best = -1;
        double best_p = 0;
        for (int k = 1; k <= newest + 2; ++k) {
            double pw = goertzelPower(p, n, 300.0 + 50.0 * k, room.rate);
            if (pw > best_p) {
                best_p = pw;
                best = k;
            }
        }
        return best;
    };
    int first = identify(room.heard.data() + 1000, ilen - 2000);
    int second = identify(room.heard.data() + ilen + 1000, ilen - 2000);
    // One more interval: it must be a fresh one, not a stale one drained from a
    // deeper queue.
    room.heard.clear();
    room.advance(static_cast<double>(ilen) / room.rate);
    int third = identify(room.heard.data() + 1000, ilen - 2000);
    std::printf("  newest sent interval %d; played %d, %d then %d\n", newest, first, second, third);
    // The queue keeps two entries (7.3): slot 0 holds the first interval that
    // arrived during the stall, slot 1 the newest. So at most one stale
    // interval plays, the newest follows within two intervals, and nothing
    // between them is ever heard.
    CHECK(second >= newest - 1);
    CHECK(third > newest);
    CHECK(first != second);
}

// Two engines that each send a tone and listen to the other, with the second
// engine's interval clock offset from the first's (it starts `offset` seconds
// later), so each receives the other's intervals while they are still arriving.
static void duplexRoom(std::uint32_t codec, int rate, double offset, double min_snr) {
    Server server;
    ServerConfig cfg;
    cfg.port = 0;
    cfg.allow_anonymous = true;
    cfg.allow_codecs = kCapOpus | kCapFlac;
    cfg.chosen_codec = codec;
    cfg.opus_kbps = 128;
    cfg.bpm = 120;
    cfg.bpi = 8;
    CHECK(server.start(cfg));
    Engine e[2];
    const double hz[2] = {997.0, 1499.0};
    for (int k = 0; k < 2; ++k) {
        e[k].setMetronome(0, 0, true, 0);
        LocalChannel lc;
        lc.name = "tone";
        lc.broadcast = true;
        lc.mute = true;
        e[k].setLocalChannel(0, lc);
        e[k].connect("127.0.0.1", server.port(), k == 0 ? "anonymous:a" : "anonymous:b", "");
    }
    auto control = [&] {
        server.poll(0);
        for (int i = 0; i < 3; ++i) {
            bool idle = e[0].run();
            idle = e[1].run() && idle;
            if (idle) break;
        }
        for (int k = 0; k < 2; ++k)
            for (int u = 0; u < e[k].numUsers(); ++u)
                for (int i = 0; i < 32; ++i) {
                    int ch = e[k].channelAt(u, i);
                    if (ch < 0) break;
                    if (e[k].channelVolume(u, ch) != 1.0f) e[k].setChannelVolume(u, ch, 1.0f);
                }
    };
    TimePoint deadline = Clock::now() + std::chrono::seconds(5);
    while (!(e[0].status() == Status::Ok && e[1].status() == Status::Ok && e[0].serverBpi() > 0 &&
             e[1].serverBpi() > 0 && e[0].numUsers() == 1 && e[1].numUsers() == 1)) {
        CHECK(Clock::now() < deadline);
        control();
        server.poll(1);
    }
    CHECK_EQ(e[0].uploadCodec(), codec == kCapFlac ? kFourccFlac : kFourccOpus);
    const int block = 512;
    std::vector<float> in[2], l(block), r(block);
    std::vector<float> heard[2];
    double phase[2] = {0, 0};
    long frames = 0;
    const long start1 = static_cast<long>(offset * rate);
    const long total = static_cast<long>(30.0 * rate);
    for (int k = 0; k < 2; ++k) in[k].assign(block, 0.0f);
    while (frames < total) {
        for (int k = 0; k < 2; ++k) {
            if (k == 1 && frames < start1) continue;
            for (int i = 0; i < block; ++i) {
                in[k][static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(phase[k]));
                phase[k] += 2 * M_PI * hz[k] / rate;
                if (phase[k] > 2 * M_PI) phase[k] -= 2 * M_PI;
            }
            const float* inp[1] = {in[k].data()};
            float* out[2] = {l.data(), r.data()};
            e[k].process(inp, 1, out, 2, block, rate);
            heard[k].insert(heard[k].end(), l.begin(), l.end());
        }
        frames += block;
        control();
    }
    std::size_t ilen = static_cast<std::size_t>(e[0].usualIntervalLength());
    for (int k = 0; k < 2; ++k) {
        // The last two whole intervals of this engine's own clock.
        std::size_t n = heard[k].size();
        std::size_t whole = (n / ilen) * ilen;
        const float* p = heard[k].data() + (whole - 2 * ilen);
        double snr = snrAgainstTone(p, 2 * ilen, hz[1 - k], rate);
        float peak = 0;
        for (std::size_t i = 0; i < 2 * ilen; ++i) peak = std::max(peak, std::fabs(p[i]));
        std::printf("  %s offset %.1f s: engine %d hears snr %.1f dB, peak %.3f\n",
                    codec == kCapFlac ? "flac" : "opus", offset, k, snr, peak);
        CHECK(snr >= min_snr);
        CHECK(peak >= 0.45f && peak <= 0.55f);
    }
}

TEST(duplex_flac_offset_clocks) {
    duplexRoom(kCapFlac, 48000, 1.0, 60.0);
    duplexRoom(kCapFlac, 48000, 3.0, 60.0);
}

TEST(duplex_opus_offset_clocks) {
    duplexRoom(kCapOpus, 48000, 1.0, 35.0);
}

TEST_MAIN()
