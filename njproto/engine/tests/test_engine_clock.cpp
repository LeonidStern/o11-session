// ENGINE.md 13.5-13.9: interval clock, tempo changes, pan law, resampling time.
#include <cmath>
#include <vector>

#include "check.h"
#include "njproto/engine.h"
#include "njproto/server.h"

using namespace njproto;
using namespace njproto::engine;

namespace {

// A server plus one engine driven from this thread.
struct Rig {
    Server server;
    Engine engine;
    std::vector<float> l, r;
    Rig(int bpm, int bpi) {
        ServerConfig cfg;
        cfg.port = 0;
        cfg.allow_anonymous = true;
        cfg.bpm = static_cast<std::uint16_t>(bpm);
        cfg.bpi = static_cast<std::uint16_t>(bpi);
        cfg.users.push_back({"admin", "pw", kPrivAll});
        CHECK(server.start(cfg));
    }
    bool pump(const std::function<bool()>& pred, int timeout_ms = 3000) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (Clock::now() > deadline) return false;
            server.poll(1);
            engine.run();
        }
        return true;
    }
    bool login(const std::string& user, const std::string& pass = "") {
        engine.connect("127.0.0.1", server.port(), user, pass);
        return pump([&] { return engine.status() == Status::Ok && engine.serverBpi() > 0; });
    }
    void process(int frames, int rate) {
        l.assign(static_cast<std::size_t>(frames), 0.0f);
        r.assign(static_cast<std::size_t>(frames), 0.0f);
        float* out[2] = {l.data(), r.data()};
        engine.process(nullptr, 0, out, 2, frames, rate);
    }
};

}  // namespace

TEST(interval_length) {                          // 13.5
    CHECK_EQ(intervalLengthFor(120, 32, 44100), 705600);
    CHECK_EQ(intervalLengthFor(100, 16, 48000), 460800);
    CHECK_EQ(intervalLengthFor(97, 7, 48000), 207835);
    CHECK_EQ(intervalLengthFor(40, 64, 192000), 18432000);
}

TEST(set_interval_end) {                         // 13.6
    Rig rig(120, 32);
    CHECK(rig.login("anonymous:clock"));
    CHECK(!rig.engine.isIntervalRunning());
    rig.process(1000, 44100);                    // running starts; first boundary at the block start
    CHECK(rig.engine.isIntervalRunning());
    CHECK_EQ(rig.engine.loopCount(), 1);
    int pos = 0, len = 0;
    rig.engine.position(&pos, &len);
    CHECK_EQ(pos, 1000);
    CHECK_EQ(len, 705600);
    CHECK_EQ(rig.engine.usualIntervalLength(), 705600);
    rig.engine.setIntervalEnd(100);
    rig.engine.position(&pos, &len);
    CHECK_EQ(len, 1100);
    CHECK_EQ(rig.engine.usualIntervalLength(), 705600);
    rig.process(100, 44100);
    rig.engine.position(&pos, &len);
    CHECK_EQ(pos, 1100);
    CHECK_EQ(rig.engine.loopCount(), 1);
    rig.process(10, 44100);                      // the boundary falls at 1100
    rig.engine.position(&pos, &len);
    CHECK_EQ(rig.engine.loopCount(), 2);
    CHECK_EQ(pos, 10);
    CHECK_EQ(len, 705600);
    CHECK_EQ(rig.engine.usualIntervalLength(), 705600);
    // A block crossing a boundary is split: 705590 more frames end the interval exactly.
    rig.process(705590 + 5, 44100);
    rig.engine.position(&pos, &len);
    CHECK_EQ(rig.engine.loopCount(), 3);
    CHECK_EQ(pos, 5);
    // Ignored while no interval runs or with samples < 1.
    rig.engine.setIntervalEnd(0);
    rig.engine.position(&pos, &len);
    CHECK_EQ(len, 705600);
}

TEST(tempo_change_at_boundary) {                 // 13.7
    Rig rig(120, 8);
    CHECK(rig.login("admin", "pw"));
    CHECK_EQ(rig.engine.serverBpm(), 120);
    CHECK_EQ(rig.engine.bpm(), 120);             // default before running
    CHECK_EQ(rig.engine.bpi(), 32);
    rig.process(512, 48000);
    CHECK_EQ(rig.engine.bpi(), 8);
    CHECK_EQ(rig.engine.usualIntervalLength(), 192000);
    rig.engine.admin("bpm 100");
    CHECK(rig.pump([&] { return rig.engine.serverBpm() == 100; }));
    CHECK_EQ(rig.engine.bpm(), 120);             // active tempo waits for the boundary
    rig.process(192000 - 512, 48000);
    CHECK_EQ(rig.engine.bpm(), 120);
    rig.process(1, 48000);
    CHECK_EQ(rig.engine.bpm(), 100);
    CHECK_EQ(rig.engine.usualIntervalLength(), 230400);
    CHECK_EQ(rig.engine.loopCount(), 2);
    rig.engine.disconnect();
    CHECK(rig.engine.status() == Status::Preconnect);
    CHECK_EQ(rig.engine.serverBpi(), 0);
    rig.process(16, 48000);
    CHECK(!rig.engine.isIntervalRunning());
}

TEST(pan_law) {                                  // 13.8
    float l, r;
    panGains(-1.0f, &l, &r);
    CHECK(l == 1.0f && r == 0.0f);
    panGains(0.5f, &l, &r);
    CHECK(std::fabs(l - 0.5f) < 1e-6f && r == 1.0f);
    panGains(0.0f, &l, &r);
    CHECK(l == 1.0f && r == 1.0f);
}

TEST(playback_resampling_keeps_time) {           // 13.9
    PlaybackResampler rs;
    long consumed = 0;
    int produced = 0;
    while (produced < 441000) {                  // 10 s at 44100
        int n = rs.needed(48000, 44100, 512);
        consumed += n;
        rs.advance(48000, 44100, 512, n);
        produced += 512;
    }
    // scale to exactly 10 s of output
    double expect = 480000.0 * produced / 441000.0;
    std::printf("  consumed %ld source frames for %d output frames (expect %.0f)\n", consumed, produced, expect);
    CHECK(std::fabs(consumed - expect) <= 100);
}

TEST_MAIN()
