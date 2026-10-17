// ENGINE.md 13.10-13.13: two engines against o11server, simulated clock;
// and the level meters and dropout counts of 8.6 and 8.7.
#include <cmath>
#include <vector>

#include "check.h"
#include "o11proto/client.h"
#include "o11proto/engine.h"
#include "o11proto/server.h"

using namespace o11proto;
using namespace o11proto::engine;

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


// A listener engine fed by a plain o11proto client that uploads FLAC
// intervals of a tone exactly when a test says so, so the test decides what
// arrives when against the listener's simulated audio clock. The room runs
// 1 s intervals (120 BPM, 2 BPI); the sender has channels 0 "tone" and 1 "two".
struct Feed {
    static constexpr int kLen = 48000;          // one interval at 48 kHz
    static constexpr int kQuarter = kLen / 4;
    Server server;
    Client sender;
    Engine listener;
    std::unique_ptr<Encoder> enc;
    const int rate = 48000, block = 480;
    double hz = 997.0, amp = 0.5, phase = 0;
    int sender_topics = 0, listener_topics = 0;
    int sent_for = 0;                           // stream(): the last listener interval fed
    float heard_peak = 0;                       // largest output sample since the test reset it
    std::vector<float> l, r;

    Feed() {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.allow_codecs = kCapOpus | kCapFlac;
        cfg.bpm = 120;
        cfg.bpi = 2;
        CHECK(server.start(cfg));
        listener.setMetronome(0, 0, true, 0);
        listener.onChat = [this](const std::array<std::string, 5>& f) {
            if (f[0] == "TOPIC") ++listener_topics;
        };
        sender.on_chat = [this](const ChatMessage& m) {
            if (m.fields[0] == "TOPIC") ++sender_topics;
        };
        enc = makeEncoder(kFourccFlac, rate, 1, 0);
        l.assign(static_cast<std::size_t>(block), 0.0f);
        r = l;
    }
    bool pumpUntil(const std::function<bool()>& pred) {
        TimePoint deadline = Clock::now() + std::chrono::seconds(5);
        while (!pred()) {
            if (Clock::now() > deadline) return false;
            server.poll(1);
            sender.poll(0);
            listener.run();
        }
        return true;
    }
    // Returns once the listener has handled everything the sender sent: the
    // server answers the sender's !topic after what the sender sent before
    // it, and the listener's !topic after what it forwarded to the listener.
    bool sync() {
        int s = sender_topics, t = listener_topics;
        sender.say("!topic");
        if (!pumpUntil([&] { return sender_topics > s; })) return false;
        listener.say("!topic");
        return pumpUntil([&] { return listener_topics > t; });
    }
    bool connect() {
        ChannelInfoRecord a, b;
        a.name = "tone";
        b.name = "two";
        sender.setChannels({a, b});
        Client::Options o;
        o.username = "anonymous:feed";
        o.codec_caps = kCapOpus | kCapFlac;
        if (!sender.connect("127.0.0.1", server.port(), o)) return false;
        listener.connect("127.0.0.1", server.port(), "anonymous:ear", "");
        if (!pumpUntil([&] {
                return sender.loggedIn() && listener.status() == Status::Ok && listener.serverBpi() > 0 &&
                       listener.numUsers() == 1 && listener.channelSubscribed(0, 0) && listener.channelSubscribed(0, 1);
            }))
            return false;
        listener.setChannelVolume(0, 0, 1.0f);
        listener.setChannelVolume(0, 1, 1.0f);
        return sync();                          // the subscription has reached the server
    }
    int pos() const {
        int p = 0, n = 0;
        listener.position(&p, &n);
        return p;
    }
    bool before(int loop, int at) const {
        return listener.loopCount() < loop || (listener.loopCount() == loop && pos() < at);
    }
    void playBlock() {
        float* out[2] = {l.data(), r.data()};
        listener.process(nullptr, 0, out, 2, block, rate);
        for (int i = 0; i < block; ++i)
            heard_peak = std::max(heard_peak, std::max(std::fabs(l[static_cast<std::size_t>(i)]), std::fabs(r[static_cast<std::size_t>(i)])));
        server.poll(0);
        sender.poll(0);
        listener.run();
    }
    // Plays the listener's clock up to `at` frames into interval `loop`.
    void playTo(int loop, int at) {
        while (before(loop, at)) playBlock();
    }
    // The same, uploading a whole interval of the tone a quarter into each
    // listener interval on the way, so each plays in the next.
    void stream(int loop, int at) {
        while (before(loop, at)) {
            if (listener.loopCount() > sent_for && pos() >= kQuarter) {
                sendWhole();
                CHECK(sync());
                sent_for = listener.loopCount();
            }
            playBlock();
        }
    }
    // The next `frames` of the tone as one FLAC stream.
    std::vector<std::uint8_t> flac(int frames) {
        std::vector<float> pcm(static_cast<std::size_t>(frames));
        for (auto& v : pcm) {
            v = static_cast<float>(amp * std::sin(phase));
            phase += 2 * M_PI * hz / rate;
        }
        enc->begin();
        enc->encode(pcm.data(), frames);
        enc->finish();
        std::vector<std::uint8_t> out(enc->outputData(), enc->outputData() + enc->outputSize());
        enc->consume(enc->outputSize());
        return out;
    }
    // Uploads the first `upto` bytes of an interval, with the end flag when that is all of it.
    Guid send(std::uint8_t channel, std::uint32_t fourcc, const std::vector<std::uint8_t>& bytes, std::size_t upto) {
        Guid g = sender.beginInterval(channel, fourcc);
        sender.writeInterval(g, bytes.data(), upto, upto == bytes.size());
        return g;
    }
    void sendWhole() {
        std::vector<std::uint8_t> b = flac(kLen);
        send(0, kFourccFlac, b, b.size());
    }
    long dropouts(int ch = 0) const { return listener.channelDropouts(0, ch); }
};

}  // namespace

TEST(two_engines_opus_then_flac_48k) {           // 13.10
    Room room(48000, 512);
    CHECK(room.connectBoth());
    CHECK_EQ(room.listener.numUsers(), 1);
    CHECK_EQ(room.listener.channelAt(0, 0), 0);
    CHECK_EQ(room.listener.channelName(0, 0), "tone");
    CHECK_EQ(room.sender.uploadCodec(), kFourccOpusRaw);
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
    CHECK_EQ(room.listener.channelLevel(0, 0, 0), room.listener.channelPeak(0, 0, 0));
    CHECK_EQ(room.listener.channelDropouts(0, 0), 0);

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
    CHECK_EQ(room.listener.channelDropouts(0, 0), 0);
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
    CHECK_EQ(room.listener.channelDropouts(0, 0), 0);
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
    CHECK_EQ(room.listener.channelDropouts(0, 0), 0);   // the silence BEGIN is no dropout
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
    // The intervals replaced in the queue during the stall are the listener's
    // own loss, not the channel's: no dropouts.
    CHECK_EQ(room.listener.channelDropouts(0, 0), 0);
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
    CHECK_EQ(e[0].uploadCodec(), codec == kCapFlac ? kFourccFlac : kFourccOpusRaw);
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
        CHECK_EQ(e[k].channelDropouts(0, 0), 0);
    }
}

TEST(duplex_flac_offset_clocks) {
    duplexRoom(kCapFlac, 48000, 1.0, 60.0);
    duplexRoom(kCapFlac, 48000, 3.0, 60.0);
}

TEST(duplex_opus_offset_clocks) {
    duplexRoom(kCapOpus, 48000, 1.0, 35.0);
}

TEST(level_while_muted) {                        // 8.6
    Feed f;
    REQUIRE(f.connect());
    f.stream(2, 40000);                          // the tone plays from L2 on
    float peak = f.listener.channelPeak(0, 0, 0);
    std::printf("  heard: peak %.3f, level %.3f\n", peak, f.listener.channelLevel(0, 0, 0));
    CHECK(peak > 0.45f && peak < 0.55f);
    CHECK(f.heard_peak > 0.45f);
    for (int side = 0; side < 2; ++side)           // the same measurement while heard
        CHECK_EQ(f.listener.channelLevel(0, 0, side), f.listener.channelPeak(0, 0, side));
    // Channel muted, user muted, another channel soloed: nothing is heard and
    // the peak falls away, but the level stays at the tone's.
    auto silenced = [&](const char* what, int loop) {
        f.heard_peak = 0;
        f.stream(loop, 40000);                   // 1 s silenced
        float pk = f.listener.channelPeak(0, 0, 0);
        float lv0 = f.listener.channelLevel(0, 0, 0), lv1 = f.listener.channelLevel(0, 0, 1);
        std::printf("  %s: peak %.4f, level %.3f %.3f\n", what, pk, lv0, lv1);
        CHECK_EQ(f.heard_peak, 0.0f);
        CHECK(pk < 0.01f);
        CHECK(lv0 > 0.45f && lv0 < 0.55f);
        CHECK(lv1 > 0.45f && lv1 < 0.55f);
    };
    f.listener.setChannelMute(0, 0, true);
    silenced("channel muted", 3);
    f.listener.setChannelMute(0, 0, false);
    f.listener.setUserMute(0, true);
    silenced("user muted", 4);
    f.listener.setUserMute(0, false);
    f.listener.setChannelSolo(0, 1, true);
    CHECK(f.listener.anySolo());
    silenced("other channel soloed", 5);
    f.listener.setChannelSolo(0, 1, false);
    // Heard again, at half volume and panned hard left, muted or not: volume
    // and pan count in the level as in the peak.
    f.listener.setChannelVolume(0, 0, 0.5f);
    f.listener.setChannelPan(0, 0, -1.0f);
    f.stream(7, 40000);
    float lv0 = f.listener.channelLevel(0, 0, 0), lv1 = f.listener.channelLevel(0, 0, 1);
    CHECK(lv0 > 0.22f && lv0 < 0.28f);
    CHECK(lv1 < 0.01f);
    CHECK_EQ(lv0, f.listener.channelPeak(0, 0, 0));
    f.listener.setChannelMute(0, 0, true);
    f.stream(8, 40000);
    lv0 = f.listener.channelLevel(0, 0, 0);
    lv1 = f.listener.channelLevel(0, 0, 1);
    std::printf("  half volume, hard left, muted: peak %.4f, level %.3f %.4f\n", f.listener.channelPeak(0, 0, 0), lv0, lv1);
    CHECK(lv0 > 0.22f && lv0 < 0.28f);
    CHECK(lv1 < 0.01f);
    CHECK(f.listener.channelPeak(0, 0, 0) < 0.01f);
    CHECK_EQ(f.dropouts(), 0);
}

TEST(dropouts_per_channel) {                     // 8.7
    Feed f;
    REQUIRE(f.connect());
    const int q = Feed::kQuarter, len = Feed::kLen;
    CHECK_EQ(f.dropouts(), 0);
    // Normal playback: an interval arriving during listener interval j plays
    // whole in j+1 (L2-L4 here). The one for L4 is 3 frames short, as the
    // rounding between two sample rates can make it: no dropout either.
    f.stream(3, q);
    std::vector<std::uint8_t> nearly = f.flac(len - 3);
    f.send(0, kFourccFlac, nearly, nearly.size());
    CHECK(f.sync());
    f.playTo(4, len);
    CHECK(f.heard_peak > 0.45f && f.heard_peak < 0.55f);
    CHECK_EQ(f.dropouts(), 0);

    // Late: nothing arrived during L4, so L5 starts with nothing; the interval
    // comes during L5 and plays in L6.
    f.playTo(5, q);
    CHECK_EQ(f.dropouts(), 1);
    f.sendWhole();
    CHECK(f.sync());
    f.heard_peak = 0;
    f.playTo(6, len);
    CHECK(f.heard_peak > 0.45f);
    CHECK_EQ(f.dropouts(), 1);

    // Missing: nothing during L6 and L7, so L7 and L8 each start with nothing.
    f.playTo(8, q);
    CHECK_EQ(f.dropouts(), 3);
    f.sendWhole();
    CHECK(f.sync());

    // Ran out: the interval for L10 holds half an interval of audio.
    f.playTo(9, q);
    std::vector<std::uint8_t> half = f.flac(len / 2);
    f.send(0, kFourccFlac, half, half.size());
    CHECK(f.sync());
    f.playTo(10, len / 2 - 2000);
    CHECK_EQ(f.dropouts(), 3);
    f.playTo(10, q * 3);
    CHECK_EQ(f.dropouts(), 4);

    // Late data: the interval for L12 stops after its first 9000 bytes (enough
    // to start playing) and the rest comes late in L12. The stream runs dry
    // over many pieces and catches up once the rest is there: one dropout.
    f.sent_for = 9;
    f.stream(11, q);                             // the interval for L11 goes out on time
    std::vector<std::uint8_t> part = f.flac(len);
    std::printf("  one interval of the tone is %zu bytes of FLAC\n", part.size());
    CHECK(part.size() > 9000 * 2);
    Guid g = f.send(0, kFourccFlac, part, 9000);
    CHECK(f.sync());
    f.playTo(12, len * 5 / 8);
    CHECK_EQ(f.dropouts(), 5);
    f.sender.writeInterval(g, part.data() + 9000, part.size() - 9000, true);
    f.sendWhole();                               // and the interval for L13, on time
    CHECK(f.sync());
    f.heard_peak = 0;
    f.playTo(12, len * 7 / 8);
    CHECK(f.heard_peak > 0.45f);                 // caught up within the interval
    f.playTo(13, len);
    CHECK_EQ(f.dropouts(), 5);

    // Can't be decoded: a Vorbis interval (no decoder in this engine) for L14.
    std::vector<std::uint8_t> junk(10000, 0x5a);
    f.send(0, kFourccVorbis, junk, junk.size());
    CHECK(f.sync());
    f.playTo(14, q);
    CHECK_EQ(f.dropouts(), 6);

    // A deliberate silent interval is no dropout, and nothing is due after it.
    f.sender.sendSilence(0);
    CHECK(f.sync());
    f.playTo(17, len);
    CHECK_EQ(f.dropouts(), 6);

    // Unsubscribed, nothing plays and nothing counts; subscribed again,
    // nothing is due until audio comes.
    f.sent_for = 17;
    f.stream(19, q);                             // audio again in L19
    f.listener.setChannelSubscribed(0, 0, false);
    CHECK(f.sync());
    f.playTo(20, q);
    f.sendWhole();                               // not forwarded to an unsubscribed listener
    CHECK(f.sync());
    f.playTo(21, q);
    f.listener.setChannelSubscribed(0, 0, true);
    CHECK(f.sync());
    f.playTo(23, len);
    CHECK_EQ(f.dropouts(), 6);

    // Per channel, counted from the channel's appearance: channel 1 never
    // sent anything; one undecodable interval counts there only, and the
    // count starts again when the channel goes and comes back.
    CHECK_EQ(f.dropouts(1), 0);
    f.send(1, kFourccVorbis, junk, junk.size());
    CHECK(f.sync());
    f.playTo(24, q);
    CHECK_EQ(f.dropouts(1), 1);
    CHECK_EQ(f.dropouts(0), 6);
    ChannelInfoRecord a, b;
    a.name = "tone";
    b.name = "two";
    f.sender.setChannels({a});
    CHECK(f.sync());
    CHECK_EQ(f.listener.channelAt(0, 1), -1);
    f.sender.setChannels({a, b});
    CHECK(f.sync());
    CHECK_EQ(f.listener.channelAt(0, 1), 1);
    CHECK_EQ(f.dropouts(1), 0);
    CHECK_EQ(f.dropouts(0), 6);
    std::printf("  dropouts: channel 0 %ld, channel 1 %ld\n", f.dropouts(0), f.dropouts(1));
}

TEST_MAIN()
