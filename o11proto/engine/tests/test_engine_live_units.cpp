// Live mode's pieces on their own: the AOO subset on the wire (live-mode
// spec 5.4-5.6), the sink's buffer rules (5.9, tests 40 and 41 at unit
// level), the sizes and buffer figures of 6.8, names and the /live texts
// that need no connection.
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <opus/opus.h>

#include "check.h"
#include "live_aoo.h"
#include "live_layer.h"
#include "live_sink.h"
#include "o11proto/engine.h"

using namespace o11proto;
using namespace o11proto::engine;

namespace {

SinkItem item(SinkItem::Kind k, std::int32_t seq, std::uint16_t len = 0, std::uint16_t block = 240) {
    SinkItem it;
    it.kind = k;
    it.seq = seq;
    it.len = len;
    it.block = block;
    std::memset(it.data, 0, sizeof(it.data));
    return it;
}

// A sink with a stream whose `blocks` blocks came in real time, one per
// rendered block: it plays and holds about its latency's blocks.
struct Playing {
    LiveSink sink;
    std::int64_t now = 1;
    std::vector<float> out = std::vector<float>(2 * 512);
    explicit Playing(int blocks = 40) {
        sink.configure(20, 256, 48000);
        sink.apply(item(SinkItem::Start, 0), now);
        for (int s = 0; s < blocks; ++s) {
            sink.apply(item(SinkItem::Xrun, s), now);   // xrun blocks: no Opus needed
            sink.render(out.data(), 240, 48000, now);
        }
    }
};

}  // namespace

// ------------------------------------------------------------ messages

TEST(osc_sizes) {
    std::uint8_t b[1500];
    aoo::StartInfo si;
    si.stream = 7;
    si.block = 240;
    si.latency = 176;
    CHECK_EQ(aoo::writeStart(b, si), 116u);
    CHECK(std::memcmp(b, "/aoo/sink/1/start\0\0\0,isiiiiiisbtiiNNi\0\0\0", 40) == 0);
    CHECK_EQ(aoo::writeStop(b, 7, 3), 44u);
    CHECK_EQ(aoo::writeSinkPing(b, 1), 36u);
    CHECK_EQ(aoo::writeSinkPong(b, 1, 2, 3), 56u);
    CHECK_EQ(aoo::writeSourcePing(b, 1), 32u);
    CHECK_EQ(aoo::writeSourcePong(b, 1, 2, 3, 0.5f), 56u);
    CHECK_EQ(aoo::writeStartRequest(b), 40u);
    CHECK_EQ(aoo::writeStopRequest(b, 9), 28u);
    // The extension blob holds the Opus application 2051.
    std::size_t n = aoo::writeStart(b, si);
    aoo::OscMessage m;
    REQUIRE(aoo::parseOsc(b, n, m));
    CHECK(m.dest == aoo::Dest::Sink && m.cmd == aoo::Cmd::Start && m.count == 16);
    CHECK(m.args[9].tag == 'b' && m.args[9].len == 4 && std::memcmp(m.args[9].s, "\x00\x00\x08\x03", 4) == 0);
    aoo::StartInfo back;
    REQUIRE(aoo::readStart(m, back));
    CHECK(back.stream == 7 && back.block == 240 && back.latency == 176 && back.codec_delay == 120);
}

TEST(osc_parsing_refuses) {
    std::uint8_t b[256];
    std::size_t n = aoo::writeSinkPing(b, 5);
    aoo::OscMessage m;
    CHECK(aoo::parseOsc(b, n, m));
    for (std::size_t cut = 0; cut < n; ++cut) CHECK(!aoo::parseOsc(b, cut, m));   // truncated anywhere
    std::uint8_t bundle[16] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    CHECK(!aoo::parseOsc(bundle, sizeof(bundle), m));
    std::uint8_t two[64];
    std::memcpy(two, b, n);
    two[10] = '2';                               // /aoo/sink/2/ping
    CHECK(!aoo::parseOsc(two, n, m));
    std::memcpy(two, b, n);
    two[n - 9] = 2;                              // the first argument isn't 1
    CHECK(!aoo::parseOsc(two, n, m));
    std::uint8_t invite[] = "/aoo/src/1/invite\0\0\0,i\0\0\0\0\0\1";
    CHECK(!aoo::parseOsc(invite, 28, m));
    std::uint8_t notags[] = "/aoo/src/1/stop\0";
    CHECK(!aoo::parseOsc(notags, 16, m));
    std::uint8_t badblob[] = "/aoo/src/1/stop\0,ib\0\0\0\0\1\0\0\1\0";   // blob longer than the message
    CHECK(!aoo::parseOsc(badblob, 32, m));
}

TEST(version_rule) {
    auto ok = [](const char* s) { return aoo::versionOk(s, std::strlen(s)); };
    CHECK(ok("2.0-test4"));
    CHECK(ok("2.1"));
    CHECK(ok("02.5"));
    CHECK(!ok("2"));
    CHECK(!ok("2."));
    CHECK(!ok(".2"));
    CHECK(!ok("3.0"));
    CHECK(!ok("2.x"));
}

TEST(start_clamps_and_filter) {                  // 5.6 clamps (test 42) and the 6.3 format checks
    std::uint8_t b[256];
    aoo::StartInfo si;
    si.stream = 1;
    si.latency = 0x7fffffff;
    si.codec_delay = 0x7fffffff;
    si.offset = 0x7fffffff;
    std::size_t n = aoo::writeStart(b, si);
    aoo::OscMessage m;
    REQUIRE(aoo::parseOsc(b, n, m));
    aoo::StartInfo r;
    REQUIRE(aoo::readStart(m, r));
    CHECK_EQ(r.latency, 480000);
    CHECK_EQ(r.codec_delay, 4800);
    CHECK_EQ(r.offset, 4800);
    auto refused = [&](aoo::StartInfo bad) {
        std::size_t k = aoo::writeStart(b, bad);
        aoo::OscMessage mm;
        aoo::StartInfo out;
        return aoo::parseOsc(b, k, mm) && !aoo::readStart(mm, out);
    };
    aoo::StartInfo x = si;
    x.rate = 44100;
    CHECK(refused(x));
    x = si;
    x.channels = 3;
    CHECK(refused(x));
    x = si;
    x.block = 200;                               // only 120, 240 and 480 decode (quirk 25)
    CHECK(refused(x));
    x = si;
    x.stream = -1;
    CHECK(refused(x));
}

TEST(binary_data) {                              // 5.4
    std::uint8_t b[1500];
    const std::uint8_t pkt[3] = {0xec, 1, 2};
    std::size_t n = aoo::writeData(b, 0x1234, 5, 48000.0, pkt, 3);
    REQUIRE_EQ(n, 27u);
    const std::uint8_t head[24] = {0x81, 0, 1, 1, 0, 0, 0x12, 0x34, 0, 0, 0, 5, 0, 1, 0, 3,
                                   0x40, 0xe7, 0x70, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(b, head, 24) == 0);
    aoo::Block blk;
    REQUIRE(aoo::parseData(b, n, blk));
    CHECK(blk.stream == 0x1234 && blk.seq == 5 && blk.len == 3 && blk.data[0] == 0xec && !blk.xrun && !blk.split);
    // The spec's example rate, 47999.99999...
    const std::uint8_t rate[8] = {0x40, 0xe7, 0x6f, 0xff, 0xff, 0xff, 0x75, 0x38};
    std::memcpy(b + 16, rate, 8);
    CHECK(aoo::parseData(b, n, blk));
    CHECK(!aoo::parseData(b, 26, blk));          // shorter than its size
    std::uint64_t nan_bits = 0x7ff8000000000000ULL;
    aoo::putBE64(b + 16, nan_bits);
    CHECK(!aoo::parseData(b, n, blk));
    aoo::putBE64(b + 16, 0);
    CHECK(!aoo::parseData(b, n, blk));           // rate 0
    n = aoo::writeXrun(b, 3, 9);
    REQUIRE_EQ(n, 16u);
    CHECK(b[13] == 0x08);
    REQUIRE(aoo::parseData(b, n, blk));
    CHECK(blk.xrun && blk.len == 0 && blk.seq == 9);
    // Stream messages are skipped before the packet.
    std::uint8_t sm[40] = {0x81, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0x04, 0, 10, 0, 0, 0, 8,
                           0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0xBB};
    REQUIRE(aoo::parseData(sm, 30, blk));
    CHECK(blk.len == 2 && blk.data[0] == 0xAA);
    sm[19] = 6;                                  // not a multiple of 4
    CHECK(!aoo::parseData(sm, 30, blk));
    // Split blocks are recognised (and treated as lost by the sink).
    std::uint8_t sp[32] = {0x81, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0x02, 0, 4, 0, 0, 0, 8, 0, 2, 0, 0, 1, 2, 3, 4};
    REQUIRE(aoo::parseData(sp, 28, blk));
    CHECK(blk.split);
    std::uint8_t neg[16] = {0x81, 0, 1, 1, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!aoo::parseData(neg, 16, blk));        // a negative stream id
}

TEST(resend_request) {                           // 5.5
    std::uint8_t b[300];
    const std::int32_t seqs[1] = {1401};
    std::size_t n = aoo::writeRequest(b, 0x54992012, seqs, 1);
    const std::uint8_t want[20] = {0x80, 0, 1, 1, 0x54, 0x99, 0x20, 0x12, 0, 0, 0, 1, 0, 0, 5, 0x79, 0xff, 0xff, 0, 0};
    REQUIRE_EQ(n, 20u);
    CHECK(std::memcmp(b, want, 20) == 0);
    std::int32_t stream, got[32];
    int count;
    REQUIRE(aoo::parseRequest(b, n, stream, got, count));
    CHECK(stream == 0x54992012 && count == 1 && got[0] == 1401);
    aoo::putBE32(b + 8, 2);                      // more requests than it holds
    CHECK(!aoo::parseRequest(b, n, stream, got, count));
    aoo::putBE32(b + 8, 0xffffffff);             // a negative count
    CHECK(!aoo::parseRequest(b, n, stream, got, count));
    std::int32_t many[40];
    for (int i = 0; i < 40; ++i) many[i] = i;
    n = aoo::writeRequest(b, 1, many, 36);
    REQUIRE(aoo::parseRequest(b, n, stream, got, count));
    CHECK_EQ(count, 32);                         // the first 32
}

// ------------------------------------------------------------- the sink

TEST(sink_sizes) {                               // 5.9's examples
    SinkSizes a = sinkSizes(20, 270, 48000, 240);
    CHECK_EQ(a.latency_blocks, 4);
    CHECK_EQ(a.capacity_blocks, 8);
    CHECK_EQ(sinkSizes(20, 512, 48000, 240).capacity_blocks, 10);
    SinkSizes c = sinkSizes(91, 2048, 48000, 240);
    CHECK_EQ(c.capacity_blocks, 37);
    CHECK_EQ(c.latency_blocks, 19);
    CHECK_EQ(liveBufferInUse(20, 2048, 48000), 91);   // test 39
    CHECK_EQ(liveBufferInUse(20, 270, 48000), 20);
    CHECK_EQ(liveBufferInUse(20, 1024, 48000), 48);
    CHECK_EQ(liveBufferInUse(500, 64, 48000), 200);
    CHECK_EQ(liveLongestGoodBlock(48000), 4096);
    CHECK_EQ(liveLongestGoodBlock(44100), 4096);
    CHECK_EQ(liveLongestGoodBlock(96000), 8192);
}

TEST(sink_far_block) {                           // test 41 at unit level
    Playing p(40);
    CHECK(p.sink.playing());
    const std::uint32_t over = p.sink.stats.overruns.load();
    p.sink.apply(item(SinkItem::Xrun, 39 + 1000), p.now);    // one stray block: nothing changes
    p.sink.apply(item(SinkItem::Xrun, 40), p.now);
    CHECK_EQ(p.sink.stats.overruns.load(), over);
    CHECK(p.sink.playing());
    p.sink.apply(item(SinkItem::Xrun, 40 + 1000), p.now);    // a real jump: two in a row
    p.sink.apply(item(SinkItem::Xrun, 40 + 1001), p.now);
    CHECK_EQ(p.sink.stats.overruns.load(), over + 1);
    p.sink.apply(item(SinkItem::Xrun, 41), p.now);           // blocks from before the jump
    p.sink.apply(item(SinkItem::Xrun, 42), p.now);
    CHECK_EQ(p.sink.stats.overruns.load(), over + 1);
    CHECK_EQ(p.sink.heldBlocks(), 1);                         // only the jump's block, then 1041
    p.sink.apply(item(SinkItem::Xrun, 1042), p.now);
    CHECK_EQ(p.sink.heldBlocks(), 2);
}

TEST(sink_overrun_and_wrap) {
    Playing p(10);
    const std::uint32_t over = p.sink.stats.overruns.load();
    const int cap = p.sink.sizes().capacity_blocks;
    const int held = p.sink.heldBlocks();
    // Within the capacity of the newest block, but more room than is free: an overrun.
    REQUIRE(held >= 1 && held < cap);
    p.sink.apply(item(SinkItem::Xrun, 9 + cap), p.now);
    CHECK_EQ(p.sink.stats.overruns.load(), over + 1);
    CHECK_EQ(p.sink.heldBlocks(), 1);
    CHECK(!p.sink.playing());                    // and it buffers again
    // A wrap: a block 2^30 below the last one played.
    LiveSink w;
    w.configure(20, 256, 48000);
    w.apply(item(SinkItem::Start, 0x7ffffff8), 1);
    for (std::int32_t s = 0x7ffffff8; s <= 0x7ffffffe; ++s) w.apply(item(SinkItem::Xrun, s), 1);
    std::vector<float> out(2 * 4096);
    w.render(out.data(), 4096, 48000, 1);        // plays them (and runs dry)
    CHECK_EQ(w.heldBlocks(), 0);
    w.apply(item(SinkItem::Xrun, 0x7ffffffe), 1);   // old
    CHECK_EQ(w.heldBlocks(), 0);
    w.apply(item(SinkItem::Xrun, 0), 1);
    CHECK_EQ(w.heldBlocks(), 1);
    CHECK_EQ(w.stats.overruns.load(), 0u);
}

TEST(sink_missing_blocks_and_requests) {
    Playing p(10);
    p.sink.apply(item(SinkItem::Xrun, 13), p.now);   // 10, 11, 12 missing
    std::int32_t seqs[16];
    int n = p.sink.dueRequests(p.now, seqs, 16);
    REQUIRE_EQ(n, 3);
    CHECK(seqs[0] == 10 && seqs[2] == 12);
    CHECK_EQ(p.sink.dueRequests(p.now + 5000000, seqs, 16), 0);    // not again within 10 ms
    CHECK_EQ(p.sink.dueRequests(p.now + 11000000, seqs, 16), 3);
    p.sink.apply(item(SinkItem::Xrun, 11), p.now);  // a resend arrives
    CHECK_EQ(p.sink.stats.resent.load(), 1u);
    const std::uint32_t lost = p.sink.stats.lost.load();
    std::vector<float> out(2 * 2048);
    p.sink.render(out.data(), 2048, 48000, p.now);  // 10 and 12 play as lost
    CHECK_EQ(p.sink.stats.lost.load(), lost + 2);
}

TEST(sink_underrun_and_buffer_figure) {         // 5.9, 6.10 (test 43's bound at unit level)
    Playing p(10);
    std::vector<float> out(2 * 4096);
    p.sink.render(out.data(), 4096, 48000, p.now);  // runs dry
    CHECK(!p.sink.playing());
    const std::uint32_t under = p.sink.stats.underruns.load();
    for (int s = 10; s < 300; ++s) p.sink.apply(item(SinkItem::Xrun, s), p.now);   // a burst after a stall
    CHECK_EQ(p.sink.stats.underruns.load(), under + 1);
    std::int32_t seq = 300;                      // then real time again
    for (int i = 0; i < 20 && !p.sink.playing(); ++i) {
        p.sink.apply(item(SinkItem::Xrun, seq++), p.now);
        p.sink.render(out.data(), 240, 48000, p.now);
    }
    CHECK(p.sink.playing());
    CHECK_EQ(p.sink.stats.underruns.load(), under + 1);
    const int figure_ms = p.sink.stats.buffer_us.load() / 1000;
    std::printf("    buffer figure after a burst %d ms\n", figure_ms);
    CHECK(figure_ms >= 20 && figure_ms <= 20 + 5 + 6);
    CHECK(p.sink.heldBlocks() <= p.sink.sizes().latency_blocks);
}

TEST(sink_stop_and_reset) {
    Playing p(10);
    p.sink.apply(item(SinkItem::Stop, 0), p.now);
    std::vector<float> out(2 * 4096);
    CHECK(p.sink.render(out.data(), 2048, 48000, p.now));   // plays out what it has
    p.sink.render(out.data(), 2048, 48000, p.now);
    CHECK(!p.sink.hasStream());
    CHECK_EQ(p.sink.stats.underruns.load(), 0u);            // an end, not an underrun
    p.sink.apply(item(SinkItem::Xrun, 30), p.now);
    CHECK(!p.sink.hasStream());
}

TEST(decoder_clamp) {                            // test 40
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    REQUIRE(enc != nullptr);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    std::vector<float> pcm(2 * 240);
    std::mt19937 rng(12345);
    LiveSink sink;
    sink.configure(20, 240, 48000);
    sink.apply(item(SinkItem::Start, 0), 1);
    OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
    double phase = 0;
    bool bounded = true, finite = true;
    std::vector<float> out(2 * 240), d(2 * 240);
    for (int k = 0; k < 200; ++k) {
        for (int i = 0; i < 240; ++i) {
            pcm[2 * i] = pcm[2 * i + 1] = static_cast<float>(std::sin(phase));
            phase += 2 * M_PI * 1000 / 48000;
        }
        SinkItem it = item(SinkItem::Data, k);
        int n = opus_encode_float(enc, pcm.data(), 240, it.data, 1275);
        REQUIRE(n > 0);
        it.len = static_cast<std::uint16_t>(n);
        if (k % 10 == 0)
            for (int f = 0; f < 8; ++f) it.data[rng() % n] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        // decoded as the sink decodes, concealing the dropped ones
        int got = k % 20 == 0 ? opus_decode_float(dec, nullptr, 0, d.data(), 240, 0)
                              : opus_decode_float(dec, it.data, it.len, d.data(), 240, 0);
        if (got > 0) {
            clampDecoded(d.data(), 2 * static_cast<std::size_t>(got));
            for (int i = 0; i < 2 * got; ++i) {
                bounded &= d[i] >= -2.0f && d[i] <= 2.0f;
                finite &= std::isfinite(d[i]);
            }
        }
        if (k % 20 != 0) sink.apply(it, 1);
        sink.render(out.data(), 240, 48000, 1);
        for (float v : out) finite &= std::isfinite(v) && std::fabs(v) <= 2.5f;   // the cubic may ring a little
    }
    CHECK(bounded);
    CHECK(finite);
    float nan_and_big[4] = {std::nanf(""), 70000.0f, -9.0f, 0.5f};
    clampDecoded(nan_and_big, 4);
    CHECK(nan_and_big[0] == -2.0f && nan_and_big[1] == 2.0f && nan_and_big[2] == -2.0f && nan_and_big[3] == 0.5f);
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
}

// ---------------------------------------------------------------- texts

TEST(display_names) {                            // 6.5
    CHECK_EQ(liveDisplayName("bob@1.2.3.x.2"), "bob.2");
    CHECK_EQ(liveDisplayName("bob@1.2.3.4.2"), "bob.2");
    CHECK_EQ(liveDisplayName("bob@1.2.3.x"), "bob");
    CHECK_EQ(liveDisplayName("bob@1.2.3.x.123"), "bob");
    CHECK_EQ(liveDisplayName("bob"), "bob");
    CHECK_EQ(liveDisplayName("bob@host:1.2.3.x.7"), "bob.7");
}

TEST(buffer_setting_parsing) {                   // 6.8, test 38's state part
    int ms = 0;
    CHECK(Engine::parseLiveBufferSetting("500", &ms) && ms == 200);
    CHECK(Engine::parseLiveBufferSetting("+30", &ms) && ms == 30);
    CHECK(Engine::parseLiveBufferSetting("030", &ms) && ms == 30);
    CHECK(Engine::parseLiveBufferSetting("-5", &ms) && ms == 5);
    ms = 7;
    CHECK(!Engine::parseLiveBufferSetting("30.0", &ms) && ms == 7);
    CHECK(!Engine::parseLiveBufferSetting("", &ms));
    CHECK(!Engine::parseLiveBufferSetting("abc", &ms));
}

TEST(live_command_without_connection) {          // 6.11
    Engine e;
    std::vector<std::pair<std::string, bool>> lines;
    e.onLiveNotice = [&](const std::string& t, bool err) { lines.emplace_back(t, err); };
    CHECK(!e.liveCommand("/lively"));
    CHECK(!e.liveCommand("hello"));
    REQUIRE(e.liveCommand("/live"));
    CHECK_EQ(lines.back().first,
             "Not connected. In a room with Live mode, /live shows how late you hear each O11 Session player. "
             "Live's jitter buffer is 20 ms (/live buffer N to change it).");
    REQUIRE(e.liveCommand("/LIVE On"));
    CHECK(lines.back().first == "Not connected: Live mode is the room's, so join a room first." && lines.back().second);
    REQUIRE(e.liveCommand("/live buffer"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is 20 ms. Type /live buffer 30 to change it (5 to 200).");
    REQUIRE(e.liveCommand("/live buffer 40"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is 40 ms now (it was 20). Less is less late, more has fewer "
                                 "dropouts: type /live to see them.");
    CHECK_EQ(e.liveBufferMs(), 40);
    REQUIRE(e.liveCommand("/live buffer30"));    // quirk 12
    CHECK_EQ(e.liveBufferMs(), 30);
    REQUIRE(e.liveCommand("/live buffer 30.0"));
    CHECK(lines.back().first == "/live buffer takes a number of ms from 5 to 200, such as /live buffer 30." &&
          lines.back().second);
    REQUIRE(e.liveCommand("/live buffers"));
    CHECK(lines.back().second);
    REQUIRE(e.liveCommand("/live buffer 201"));
    CHECK_EQ(e.liveBufferMs(), 30);
    REQUIRE(e.liveCommand("/live what"));
    CHECK(lines.back().first ==
              "Type /live to see how the Live players sound, or /live buffer 30 to set Live's jitter buffer (ms)." &&
          lines.back().second);
}

TEST(long_host_blocks) {                         // the buffer texts with a 2048-sample host
    Engine e;
    std::vector<std::string> lines;
    e.onLiveNotice = [&](const std::string& t, bool) { lines.push_back(t); };
    std::vector<float> l(8192), r(8192);
    float* outs[2] = {l.data(), r.data()};
    e.process(nullptr, 0, outs, 2, 2048, 48000);
    CHECK_EQ(e.liveBufferInUseMs(), 91);
    REQUIRE(e.liveCommand("/live buffer 30"));
    CHECK_EQ(lines.back(), "Live's jitter buffer is 30 ms now (it was 20). Less is less late, more has fewer dropouts: "
                           "type /live to see them. Your DAW's audio buffer (2048 samples) needs at least 91 ms, so "
                           "Live uses 91 ms.");
    e.process(nullptr, 0, outs, 2, 8192, 48000);
    REQUIRE(e.liveCommand("/live buffer 30"));
    CHECK(lines.back().find("Your DAW's audio buffer (8192 samples) needs at least 347 ms, more than Live can use "
                            "(200 ms), so expect dropouts: 4096 samples or less plays well.") != std::string::npos);
}

int main() {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
