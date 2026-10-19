// Live mode's pieces on their own: the AOO subset on the wire (live-mode
// spec 5.4-5.6), the sink's buffer rules (5.9, tests 40 and 41 at unit
// level), the sizes and buffer figures of 6.8, names and the /live texts
// that need no connection.
#include <algorithm>
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

// The counts and the log a plug-in reads (Engine::liveUserCounters(),
// liveUserEvents()): a block missing at its turn is lost, and late when it
// comes after all; running dry, the blocks that come after their turn are
// late, and the stream comes back on its delay; the concealment is counted
// once the stream has played; the ring keeps the latest 64 events.
TEST(sink_counters_and_log) {
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    REQUIRE(enc != nullptr);
    std::vector<float> pcm(2 * 240);
    long n = 0;
    auto block = [&](std::int32_t seq) {        // a 440 Hz tone, block `seq`
        for (int i = 0; i < 240; ++i, ++n) pcm[2 * i] = pcm[2 * i + 1] = static_cast<float>(0.3f * std::sin(2 * M_PI * 440.0 * n / 48000));
        SinkItem it = item(SinkItem::Data, seq);
        it.len = static_cast<std::uint16_t>(opus_encode_float(enc, pcm.data(), 240, it.data, 1275));
        return it;
    };
    std::vector<SinkItem> blocks;
    for (int s = 0; s < 80; ++s) blocks.push_back(block(s));
    opus_encoder_destroy(enc);
    auto kinds = [](const LiveSink& k, std::uint32_t from) {
        std::vector<LiveEventKind> v;
        for (std::uint32_t i = from; i < k.stats.events_n.load(); ++i) {
            std::int64_t ns = 0, value = 0;
            std::uint8_t kind = 0;
            if (k.stats.readEvent(i, &ns, &kind, &value)) v.push_back(static_cast<LiveEventKind>(kind));
        }
        return v;
    };
    auto has = [](const std::vector<LiveEventKind>& v, LiveEventKind k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    LiveSink k;
    k.configure(20, 240, 48000);
    std::vector<float> out(2 * 240);
    std::int64_t now = 1000;
    k.apply(item(SinkItem::Start, 0), now);
    std::int32_t s = 0;
    for (; s < 20; ++s, now += 5000000) {        // real time
        k.apply(blocks[static_cast<std::size_t>(s)], now);
        k.render(out.data(), 240, 48000, now);
    }
    REQUIRE(k.playing());
    CHECK(has(kinds(k, 0), LiveEventKind::Stream) && has(kinds(k, 0), LiveEventKind::Play));
    CHECK_EQ(k.stats.concealed48.load(), 0u);    // the buffering before it played isn't counted
    // Block 20 doesn't come in time: lost, concealed; then it comes, late.
    std::uint32_t e0 = k.stats.events_n.load();
    for (int i = 0; i < 8; ++i, ++s, now += 5000000) {
        if (s != 20) k.apply(blocks[static_cast<std::size_t>(s)], now);
        k.render(out.data(), 240, 48000, now);
    }
    CHECK_EQ(k.stats.lost.load(), 1u);
    CHECK_EQ(k.stats.concealed48.load(), 240u);
    k.apply(blocks[20], now);
    CHECK_EQ(k.stats.late.load(), 1u);
    CHECK_EQ(k.stats.late_missed.load(), 1u);    // so not lost after all
    std::vector<LiveEventKind> v = kinds(k, e0);
    CHECK(has(v, LiveEventKind::Lost) && has(v, LiveEventKind::Late));
    // A stall of the sender's: dry, its blocks late, back on its delay.
    e0 = k.stats.events_n.load();
    for (int i = 0; i < 8; ++i, now += 5000000) k.render(out.data(), 240, 48000, now);
    CHECK(!k.playing());
    for (int i = 0; i < 9; ++i) k.apply(blocks[static_cast<std::size_t>(s + i)], now);   // they all come at once
    s += 9;
    for (int i = 0; i < 6; ++i, ++s, now += 5000000) {
        k.render(out.data(), 240, 48000, now);
        k.apply(blocks[static_cast<std::size_t>(s)], now);
    }
    CHECK(k.playing());
    v = kinds(k, e0);
    CHECK(has(v, LiveEventKind::Dry) && has(v, LiveEventKind::Late) && has(v, LiveEventKind::Rejoin));
    CHECK(!has(v, LiveEventKind::Rebuffer) && !has(v, LiveEventKind::Skip));
    CHECK_EQ(k.stats.underruns.load(), 1u);
    CHECK(k.stats.late.load() >= 1u + 4u);
    std::printf("    %u late, %u lost (%u came late), %.1f ms concealed, %u underruns\n", k.stats.late.load(),
                k.stats.lost.load(), k.stats.late_missed.load(), k.stats.concealed48.load() / 48.0,
                k.stats.underruns.load());
    // The ring: the latest 64 of them; an older one reads as gone.
    SinkStats st;
    for (int i = 0; i < 100; ++i) st.event(i, static_cast<std::uint8_t>(LiveEventKind::Lost), i);
    std::int64_t ns = 0, value = 0;
    std::uint8_t kind = 0;
    CHECK(!st.readEvent(35, &ns, &kind, &value));
    REQUIRE(st.readEvent(36, &ns, &kind, &value));
    CHECK(ns == 36 && value == 36 && kind == static_cast<std::uint8_t>(LiveEventKind::Lost));
    CHECK(st.readEvent(99, &ns, &kind, &value) && value == 99);
    st.clear(2);
    CHECK(!st.readEvent(99, &ns, &kind, &value));
    CHECK_EQ(st.events_n.load(), 0u);
    CHECK(std::string(liveEventName(LiveEventKind::Late)) == "late");
    CHECK(std::string(liveEventName(LiveEventKind::SpeedOff)) == "speed off");
}

// A stream in real time to one sink: a block of a 440 Hz tone each 5 ms,
// each sent as it is made (unless it is lost, or the sender's link holds it
// with the blocks after it until they all come at once), and one block
// rendered after each.
struct Feed {
    OpusEncoder* enc = nullptr;
    std::vector<float> pcm = std::vector<float>(2 * 240);
    std::vector<float> out = std::vector<float>(2 * 240);
    long n = 0;
    double base = 10, fallback = 20;
    LiveSink k;
    std::int64_t now = 1000;
    std::int32_t seq = 0;
    std::vector<SinkItem> held;
    Feed(double base_ms, double fallback_ms) : base(base_ms), fallback(fallback_ms) {
        int err = 0;
        enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        k.configure(base, 240, 48000, fallback);
        k.apply(item(SinkItem::Start, 0), now);
    }
    ~Feed() {
        if (enc) opus_encoder_destroy(enc);
    }
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;
    SinkItem block(std::int32_t s) {
        for (int i = 0; i < 240; ++i, ++n) pcm[2 * i] = pcm[2 * i + 1] = static_cast<float>(0.3f * std::sin(2 * M_PI * 440.0 * n / 48000));
        SinkItem it = item(SinkItem::Data, s);
        it.len = static_cast<std::uint16_t>(opus_encode_float(enc, pcm.data(), 240, it.data, 1275));
        return it;
    }
    // `blocks` blocks: the first `hold` of them held, then each `lose`th lost.
    void run(int blocks, int hold = 0, int lose = 0) {
        for (int i = 0; i < blocks; ++i) {
            const SinkItem it = block(seq++);
            if (i < hold) {
                held.push_back(it);
            } else {
                for (const SinkItem& h : held) k.apply(h, now);
                held.clear();
                if (!(lose > 0 && it.seq % lose == 0)) k.apply(it, now);
            }
            k.configure(base, 240, 48000, fallback);   // as each audio block does
            k.render(out.data(), 240, 48000, now);
            now += 5000000;
        }
    }
    void seconds(double s, int lose = 0) { run(static_cast<int>(s * 200), 0, lose); }
    int count(LiveEventKind kind) const {
        int c = 0;
        for (std::uint32_t i = 0; i < k.stats.events_n.load(); ++i) {
            std::int64_t ns = 0, value = 0;
            std::uint8_t kd = 0;
            if (k.stats.readEvent(i, &ns, &kd, &value) && kd == static_cast<std::uint8_t>(kind)) ++c;
        }
        return c;
    }
    std::uint64_t concealed() const { return k.stats.concealed48.load() / 240; }   // blocks
};

// The automatic buffer (6.8): 10 ms, and 20 ms from a stream's second late
// dropout within 10 s, grown while that dropout conceals: the concealment is
// that much longer, the delay that much more, and nothing else is heard.
// Loss alone, or a block asked for again that comes late, never falls back;
// nor does a fixed buffer; and a new stream starts at 10 ms again.
TEST(sink_buffer_falls_back_on_late_audio) {
    {
        Feed f(10, 20);
        f.seconds(2);
        REQUIRE(f.k.playing());
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        const int held10 = f.k.heldBlocks();
        // The sender's link holds 4 blocks (20 ms): they come late.
        std::uint64_t c0 = f.concealed();
        f.run(8, 4);
        const std::uint64_t first = f.concealed() - c0;
        CHECK(first > 0 && f.k.stats.late.load() > 0);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 0);
        f.seconds(3);
        CHECK(f.k.playing());
        // The second, 3 s later: it falls back, and its concealment is 10 ms longer.
        c0 = f.concealed();
        f.run(8, 4);
        const std::uint64_t second = f.concealed() - c0;
        f.seconds(2);
        const std::uint64_t after = f.concealed() - c0 - second;
        const int held20 = f.k.heldBlocks();
        std::printf("    concealed %llu blocks at the first late dropout, %llu at the second (it fell back), %llu "
                    "after; %d blocks held at 10 ms, %d at 20 ms; fill %.2f ms\n",
                    static_cast<unsigned long long>(first), static_cast<unsigned long long>(second),
                    static_cast<unsigned long long>(after), held10, held20, f.k.stats.fill_us.load() / 1000.0);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 1);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 20);
        CHECK(f.k.stats.fell_back.load());
        CHECK_EQ(second, first + 2);             // 10 ms more of the same concealment
        CHECK_EQ(after, 0u);                     // and no second one for it
        CHECK_EQ(held20, held10 + 2);            // the delay is 10 ms more
        // At 20 ms the same hold costs less; a third doesn't fall back again.
        c0 = f.concealed();
        f.run(8, 4);
        f.seconds(1);
        CHECK(f.concealed() - c0 < first);
        f.run(8, 2);                             // 10 ms: in time now
        f.seconds(1);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 1);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 20);
        // A new stream starts at 10 ms.
        f.k.apply(item(SinkItem::Start, f.seq), f.now);
        f.k.configure(10, 240, 48000, 20);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        CHECK(!f.k.stats.fell_back.load());
    }
    {   // Two late dropouts more than 10 s apart: no fallback; a third within 10 s of the second: one.
        Feed f(10, 20);
        f.seconds(2);
        f.run(8, 4);
        f.seconds(11);
        f.run(8, 4);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 0);
        f.seconds(9);
        f.run(8, 4);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 1);
    }
    {   // Loss alone: a block in 20 lost for 20 s, and runs of 3.
        Feed f(10, 20);
        f.seconds(2);
        f.seconds(20, 20);
        for (int i = 0; i < 10; ++i) {
            f.run(3, 0, 1);                      // 3 in a row lost: it runs dry
            f.seconds(1);
        }
        std::printf("    loss alone: %u lost, %u underruns, %u late\n", f.k.stats.lost.load(),
                    f.k.stats.underruns.load(), f.k.stats.late.load());
        CHECK(f.k.stats.lost.load() >= 100);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 0);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        // A block missing at its turn, asked for again, that comes after its
        // turn: late, but lost as far as the buffer goes.
        for (int r = 0; r < 3; ++r) {
            const SinkItem missing = f.block(f.seq++);
            f.run(1);                            // the one after it comes
            std::int32_t seqs[16];
            CHECK(f.k.dueRequests(f.now, seqs, 16) >= 1);   // it is asked for
            f.run(2);                            // its turn passes
            f.k.apply(missing, f.now);           // the answer, late
            f.seconds(1);
        }
        CHECK(f.k.stats.late.load() >= 3);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 0);
    }
    {   // A fixed buffer clears a fallback and the late dropouts counted
        // towards it: automatic again, it takes two more within 10 s.
        Feed f(10, 20);
        f.seconds(2);
        f.run(8, 4);
        f.seconds(1);
        f.run(8, 4);                             // the second within 10 s: it falls back
        f.seconds(1);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 1);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 20);
        f.fallback = 0;                          // fixed at 10 ms
        f.seconds(2);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        CHECK(!f.k.stats.fell_back.load());
        f.fallback = 20;                         // automatic again, within 10 s of both
        f.seconds(1);
        CHECK(f.k.playing());
        f.run(8, 4);
        f.seconds(1);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 1);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        f.run(8, 4);
        f.seconds(1);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 2);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 20);
    }
    {   // A fixed buffer never falls back.
        Feed f(10, 0);
        f.seconds(2);
        for (int i = 0; i < 4; ++i) {
            f.run(8, 4);
            f.seconds(1);
        }
        CHECK(f.k.stats.late.load() >= 4);
        CHECK_EQ(f.count(LiveEventKind::Fallback), 0);
        CHECK_EQ(f.k.stats.buffer_ms.load(), 10);
        CHECK(!f.k.stats.fell_back.load());
        CHECK(std::string(liveEventName(LiveEventKind::Fallback)) == "fallback");
    }
}

TEST(sink_keeps_its_delay_after_a_gap) {        // 16.6: back in time with the other players
    // A sender in real time, 256-frame host blocks against 240-frame
    // blocks, and a gap in its packets: lost (the sink hears nothing more
    // until they go on), or held back (they all come at its end). The
    // blocks held, over whole cycles of the two block sizes (15 host
    // blocks), are the delay; it is the same after the gap as before it,
    // wherever in the cycle the gap begins.
    int worst = 0;
    for (int held_back = 0; held_back < 2; ++held_back) {
        for (int gap_ms : {20, 50, 80, 120, 250}) {
            for (int phase = 0; phase < 15; ++phase) {
                LiveSink k;
                k.configure(20, 256, 48000);
                k.apply(item(SinkItem::Start, 0), 1);
                std::vector<float> out(2 * 256);
                const long g0 = 48000 * 3 / 2 + phase * 256 + 37, g1 = g0 + gap_ms * 48;
                std::int32_t sent = 0, given = 0;
                long before = 0, after = 0;
                const int cycles = 6 * 15;       // 480 ms
                const int b_before = 48000 * 3 / 2 / 256 - cycles, b_after = static_cast<int>(g1 / 256) + 100;
                for (int b = 0; b < b_after + cycles; ++b) {
                    const long t = (b + 1) * 256L;
                    while ((sent + 1) * 240L <= t) {
                        const long at = sent * 240L;
                        if (at >= g0 && at < g1 && !held_back) given = ++sent;   // lost
                        else if (at >= g0 && at < g1) ++sent;                    // waits for the gap's end
                        else {
                            for (; given <= sent; ++given) k.apply(item(SinkItem::Xrun, given), 1);
                            ++sent;
                        }
                    }
                    k.render(out.data(), 256, 48000, 1);
                    if (b >= b_before && b < b_before + cycles) before += k.heldBlocks();
                    if (b >= b_after) after += k.heldBlocks();
                }
                const int moved = static_cast<int>(after - before);   // in 1/90 blocks
                worst = std::max(worst, std::abs(moved));
                if (moved != 0)
                    std::printf("    %s gap of %d ms at phase %d: the delay moved %+.2f ms\n", held_back ? "held-back" : "lost",
                                gap_ms, phase, moved * 5.0 / cycles);
                CHECK(k.playing());
                CHECK_EQ(moved, 0);
                CHECK(k.stats.underruns.load() <= 1u);
                CHECK_EQ(k.stats.overruns.load(), 0u);
                if (gap_ms >= 50) CHECK_EQ(k.stats.underruns.load(), 1u);   // one dropout, not lost blocks
                if (gap_ms >= 50 && !held_back) CHECK_EQ(k.stats.lost.load(), 0u);
            }
        }
    }
    std::printf("    after gaps of 20 to 250 ms, lost or held back: the delay moved at most %.2f ms\n", worst * 5.0 / 90);
}

// All-to-all: a gap just after a path change, to direct or to the relay,
// is one like any other: the stream keeps its delay (16.6) and conceals no
// more than without the change. (It buffered afresh within 2 s of a
// change, and a 25 ms gap concealed 30 ms instead of 10: the dip of
// test_engine_live_paths after `!live direct` on a loaded machine.)
TEST(sink_path_change_and_a_late_block) {
    std::uint64_t plain = 0;
    for (int to : {0, 1, 2}) {                   // no change, to direct, to the relay
        Feed f(20, 20);
        f.seconds(3);
        long before = 0, after = 0;
        for (int i = 0; i < 48; ++i) {           // 240 ms
            f.run(1);
            before += f.k.heldBlocks();
        }
        if (to) f.k.apply(item(SinkItem::Path, to, 1), f.now);
        f.seconds(0.5);
        const int rebuffers = f.count(LiveEventKind::Rebuffer);
        const std::uint64_t concealed = f.concealed();
        f.run(20, 5);                            // 25 ms held back, then all at once
        f.seconds(1);
        for (int i = 0; i < 48; ++i) {
            f.run(1);
            after += f.k.heldBlocks();
        }
        const int rebuffered = f.count(LiveEventKind::Rebuffer) - rebuffers;
        const std::uint64_t c = f.concealed() - concealed;
        std::printf("    %s, a 25 ms gap: %d buffering afresh, %llu blocks concealed, the delay moved %+.2f ms\n",
                    to == 0 ? "no path change" : to == 1 ? "after a change to direct" : "after a change to the relay",
                    rebuffered, static_cast<unsigned long long>(c), (after - before) * 5.0 / 48);
        CHECK(f.k.playing());
        CHECK(std::labs(after - before) <= 48);  // within a block
        if (to == 0) plain = c;
        CHECK_EQ(rebuffered, 0);
        CHECK_EQ(c, plain);
    }
}

TEST(sink_marks_where_its_concealment_begins) {   // 16.8: the mixer's hold counts from that frame
    // A sender in real time, a tone in 240-frame Opus blocks, against
    // 270-frame host blocks, and a lost gap. The frame where the sink's
    // concealment begins (quietFrom(), firstReal()) is where the first lost
    // block's turn comes, on the stream's delay, wherever in a host block
    // that falls; and its audio comes back the lost blocks' length later.
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    REQUIRE(enc != nullptr);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    std::vector<float> pcm(2 * 240), out(2 * 270);
    long first_delay = -1;
    int runs = 0, off = 0, wrong_length = 0;
    for (int gap_ms : {50, 120, 250}) {
        for (int phase = 0; phase < 9; ++phase) {   // 9 blocks of 240 against 8 of 270: every place in a host block
            LiveSink k;
            k.configure(20, 270, 48000);
            k.apply(item(SinkItem::Start, 0), 1);
            opus_encoder_ctl(enc, OPUS_RESET_STATE);
            const long g0 = 48000 + phase * 240 + 37, g1 = g0 + gap_ms * 48;
            const long s0 = (g0 + 239) / 240, s1 = (g1 + 239) / 240;   // the first lost block, the first after
            std::int32_t sent = 0;
            long onset = -1, back = -1;
            bool was_real = false;
            for (long b = 0; b < (g1 + 24000) / 270; ++b) {
                while ((sent + 1) * 240L <= (b + 1) * 270L) {
                    for (int i = 0; i < 240; ++i)
                        pcm[2 * i] = pcm[2 * i + 1] =
                            0.3f * static_cast<float>(std::sin(2 * M_PI * 440 * (sent * 240.0 + i) / 48000));
                    SinkItem it = item(SinkItem::Data, sent);
                    const int n = opus_encode_float(enc, pcm.data(), 240, it.data, 1275);
                    REQUIRE(n > 0);
                    it.len = static_cast<std::uint16_t>(n);
                    if (sent * 240L < g0 || sent * 240L >= g1) k.apply(it, 1);
                    ++sent;
                }
                k.render(out.data(), 270, 48000, 1);
                const int first = k.firstReal(), from = k.quietFrom();
                if (onset < 0 && was_real && from < 270) onset = b * 270 + from;
                if (onset >= 0 && back < 0 && b * 270 + 270 > onset && first >= 0 && b * 270 + first > onset)
                    back = b * 270 + first;
                was_real = first >= 0 && from == 270;
            }
            ++runs;
            const long delay = onset - s0 * 240;
            if (first_delay < 0) first_delay = delay;
            if (onset < 0 || delay != first_delay) {
                ++off;
                std::printf("    a %d ms gap at phase %d: the concealment began at %ld, %ld frames after the first "
                            "lost block's start (%ld before)\n", gap_ms, phase, onset, delay, first_delay);
            }
            if (back - onset != (s1 - s0) * 240) {
                ++wrong_length;
                std::printf("    a %d ms gap at phase %d: concealed for %ld frames, not %ld\n", gap_ms, phase,
                            back - onset, (s1 - s0) * 240);
            }
        }
    }
    std::printf("    %d gaps: the concealment began %ld frames after the first lost block's start in the stream every "
                "time but %d, and lasted the lost blocks' length every time but %d\n", runs, first_delay, off,
                wrong_length);
    CHECK(first_delay > 0);
    CHECK_EQ(off, 0);
    CHECK_EQ(wrong_length, 0);
    opus_encoder_destroy(enc);
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

TEST(sink_conceals_after_the_stream) {         // 16.8: what the mixer fades out
    for (int how = 0; how < 2; ++how) {          // a /stop, then a dropped stream
        Playing p(10);
        std::vector<float> out(2 * 240);
        if (how == 0) {
            p.sink.apply(item(SinkItem::Stop, 0), p.now);
            for (int i = 0; i < 40 && p.sink.hasStream(); ++i) p.sink.render(out.data(), 240, 48000, p.now);
        } else {
            p.sink.reset();
        }
        CHECK(!p.sink.hasStream());
        CHECK(p.sink.tailing());
        int blocks = 0;
        while (p.sink.render(out.data(), 240, 48000, p.now) && blocks < 1000) ++blocks;
        const double ms = blocks * 5.0;
        std::printf("    %s: concealment for %.0f ms after the stream\n", how == 0 ? "/stop" : "dropped", ms);
        // 4 x B (80 ms at 20 ms) and the 20 ms fade, with up to 10 ms to spare
        CHECK(ms >= kLiveConcealBuffers * 20 + kLiveFadeMs && ms <= kLiveConcealBuffers * 20 + kLiveFadeMs + 15);
        CHECK(!p.sink.tailing());
    }
    Playing p(10);                               // a new stream ends the concealment
    p.sink.reset();
    p.sink.apply(item(SinkItem::Start, 100), p.now);
    CHECK(!p.sink.tailing() && p.sink.hasStream());
}

TEST(sink_follows_clock_drift) {                // 6.9: a sender whose clock runs 200 ppm fast or slow
    for (double drift : {200e-6, -200e-6}) {
        LiveSink k;
        k.configure(20, 256, 48000);
        k.apply(item(SinkItem::Start, 0), 1);
        std::vector<float> out(2 * 256);
        std::int32_t sent = 0;
        int lo = 1 << 30, hi = 0;
        const int blocks = 48000 * 150 / 256;   // 150 s of host blocks
        for (int b = 0; b < blocks; ++b) {
            const double t = (b + 1) * 256.0 / 48000.0;
            const std::int32_t due = static_cast<std::int32_t>(t * 48000.0 * (1.0 + drift) / 240.0);
            while (sent < due) k.apply(item(SinkItem::Xrun, sent++), 1);
            k.render(out.data(), 256, 48000, 1);
            if (b * 256 > 48000 * 10) {          // after the first 10 s
                lo = std::min(lo, k.heldBlocks());
                hi = std::max(hi, k.heldBlocks());
            }
        }
        std::printf("    drift %+.0f ppm: held %d..%d blocks, %u underruns, %u overruns\n", drift * 1e6, lo, hi,
                    k.stats.underruns.load(), k.stats.overruns.load());
        CHECK(k.playing());
        CHECK_EQ(k.stats.underruns.load(), 0u);
        CHECK_EQ(k.stats.overruns.load(), 0u);
        CHECK(lo >= 2 && hi <= 6);               // without the correction 150 s would move it by 30 ms (6 blocks)
    }
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
    CHECK(Engine::parseLiveBufferSetting("auto", &ms) && ms == Engine::kLiveBufferAutomatic);
    ms = 7;
    CHECK(Engine::parseLiveBufferSetting("0", &ms) && ms == Engine::kLiveBufferAutomatic);
    ms = 7;
    CHECK(!Engine::parseLiveBufferSetting("Auto", &ms) && ms == 7);
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
             "Live's jitter buffer is automatic: 10 ms, or 20 ms for a player whose audio comes late (/live buffer N "
             "to fix it).");
    REQUIRE(e.liveCommand("/LIVE On"));
    CHECK(lines.back().first == "Not connected: Live mode is the room's, so join a room first." && lines.back().second);
    CHECK(e.liveBufferAutomatic());
    CHECK_EQ(e.liveBufferMs(), Engine::kLiveBufferAutomatic);
    CHECK_EQ(e.liveBufferInUseMs(), 10);
    REQUIRE(e.liveCommand("/live buffer"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is automatic: 10 ms, or 20 ms for a player whose audio comes "
                                 "late. Type /live buffer 30 to fix it (5 to 200).");
    REQUIRE(e.liveCommand("/live buffer 40"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is 40 ms now (it was automatic). Less is less late, more has "
                                 "fewer dropouts: type /live to see them.");
    CHECK_EQ(e.liveBufferMs(), 40);
    CHECK(!e.liveBufferAutomatic());
    REQUIRE(e.liveCommand("/live buffer"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is 40 ms. Type /live buffer 30 to change it (5 to 200), or "
                                 "/live buffer auto.");
    REQUIRE(e.liveCommand("/live"));
    CHECK_EQ(lines.back().first,
             "Not connected. In a room with Live mode, /live shows how late you hear each O11 Session player. "
             "Live's jitter buffer is 40 ms (/live buffer N to change it).");
    REQUIRE(e.liveCommand("/live buffer30"));    // quirk 12
    CHECK_EQ(e.liveBufferMs(), 30);
    CHECK_EQ(lines.back().first, "Live's jitter buffer is 30 ms now (it was 40). Less is less late, more has fewer "
                                 "dropouts: type /live to see them.");
    REQUIRE(e.liveCommand("/live buffer 30.0"));
    CHECK(lines.back().first == "/live buffer takes a number of ms from 5 to 200, such as /live buffer 30, or auto." &&
          lines.back().second);
    REQUIRE(e.liveCommand("/live buffers"));
    CHECK(lines.back().second);
    REQUIRE(e.liveCommand("/live buffer 201"));
    CHECK_EQ(e.liveBufferMs(), 30);
    REQUIRE(e.liveCommand("/live buffer 0"));    // automatic is "auto"
    CHECK(lines.back().second);
    CHECK_EQ(e.liveBufferMs(), 30);
    REQUIRE(e.liveCommand("/live buffer auto"));
    CHECK_EQ(lines.back().first, "Live's jitter buffer is automatic now (it was 30 ms): 10 ms, or 20 ms for a player "
                                 "whose audio comes late.");
    CHECK(e.liveBufferAutomatic());
    e.setLiveBufferMs(3);
    CHECK_EQ(e.liveBufferMs(), 5);
    e.setLiveBufferAutomatic();
    CHECK(e.liveBufferAutomatic());
    CHECK_EQ(e.liveBufferInUseMs(), 10);
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
    CHECK_EQ(lines.back(), "Live's jitter buffer is 30 ms now (it was automatic). Less is less late, more has fewer "
                           "dropouts: type /live to see them. Your DAW's audio buffer (2048 samples) needs at least 91 "
                           "ms, so Live uses 91 ms.");
    REQUIRE(e.liveCommand("/live buffer auto"));
    CHECK_EQ(lines.back(), "Live's jitter buffer is automatic now (it was 30 ms): 10 ms, or 20 ms for a player whose "
                           "audio comes late. Your DAW's audio buffer (2048 samples) needs at least 91 ms, so Live uses "
                           "91 ms for everyone.");
    {   // 256 samples: the automatic buffer starts higher, and still falls back to 20 ms
        Engine e2;
        e2.onLiveNotice = [&](const std::string& t, bool) { lines.push_back(t); };
        e2.process(nullptr, 0, outs, 2, 256, 48000);
        CHECK_EQ(e2.liveBufferInUseMs(), 16);
        REQUIRE(e2.liveCommand("/live buffer auto"));
        CHECK(lines.back().find(" Your DAW's audio buffer (256 samples) needs at least 16 ms, so Live starts at 16 "
                                "ms.") != std::string::npos);
    }
    e.process(nullptr, 0, outs, 2, 8192, 48000);
    REQUIRE(e.liveCommand("/live buffer 30"));
    CHECK(lines.back().find("Your DAW's audio buffer (8192 samples) needs at least 347 ms, more than Live can use "
                            "(200 ms), so expect dropouts: 4096 samples or less plays well.") != std::string::npos);
}

// ------------------------------------------------------------ host stalls

namespace {

// A sender and a receiver whose hosts each call a block of 128 frames at
// 48 kHz, on one clock (the frame T), with the sender's blocks reaching
// the receiver `net` frames after they are made (plus up to `jitter` more,
// at random). A host's stall is a run of blocks it isn't called for: the
// time it misses for good, as a host does when its device runs on without
// it. Each reports it as LiveLayer::audioBlock() does: the first block
// after the stall is late (the receiver's sink is told so, hostLate(),
// and holds what came meanwhile, hostBehind()), the second confirms the
// gap: the receiver's sink is told (hostGap()), and the sender sends its
// xrun blocks (5.4: whole frames, the rest carried over), then the block's
// audio. Or the sender's report comes at a later block (`report`), as when
// the blocks before it came early (6.9): the frames those complete go
// before the xrun blocks. The sender plays a noise-like signal, sig(T),
// coded by Opus at the room's frame; the receiver's output is kept by T.
struct StallSim {
    static constexpr int kHost = 128;
    static constexpr long kLen = 48000 * 4;      // 4 s
    int frame = 240;
    int net = 24, jitter = 0;
    struct Stall {
        long from = -1;                          // the first block the host doesn't call (-1: none)
        int blocks = 0;
        int off = 0;                             // frames its measure of the gap is off by
    };
    Stall send_stall, recv_stall;
    LiveSink k;
    OpusEncoder* enc = nullptr;
    std::vector<float> sig = std::vector<float>(kLen + 48000);
    std::vector<float> out = std::vector<float>(kLen, 0.0f);
    std::vector<char> played = std::vector<char>(kLen, 0), captured = std::vector<char>(kLen, 0);
    struct Wire {
        long at;
        SinkItem it;
    };
    std::vector<Wire> wire;
    std::mt19937 rng{7};
    // The sender's source.
    std::vector<float> pcm = std::vector<float>(2 * 480);
    int pcm_n = 0;
    std::int32_t seq = 0;
    double xrun48 = 0;
    int xruns_sent = 0;
    int report = 2;                              // the sender's block after its stall that reports its gap (from 1)
    int pre_report = 0;                          // the frames it sent after its stall before its xrun blocks

    explicit StallSim(int frame_size) : frame(frame_size) {
        int err = 0;
        enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
        // Noise through two one-pole low-passes, so the delay found by
        // correlation is one and sharp.
        std::uint32_t x = 12345;
        double a = 0, b = 0;
        for (float& v : sig) {
            x = x * 1664525u + 1013904223u;
            a += 0.25 * ((static_cast<double>(x >> 8) / (1u << 24) - 0.5) - a);
            b += 0.25 * (a - b);
            v = static_cast<float>(2.0 * b);
        }
        k.configure(11, kHost, 48000, 20);        // the automatic buffer at 128-frame host blocks (6.8)
    }
    ~StallSim() { opus_encoder_destroy(enc); }
    StallSim(const StallSim&) = delete;
    StallSim& operator=(const StallSim&) = delete;

    static bool stalled(const Stall& s, long b) { return s.from >= 0 && b >= s.from && b < s.from + s.blocks; }
    // 0: an ordinary block; 1: the first after a stall the layer notices
    // (over 20 ms: 8 blocks or more); 2: the second.
    static int after(const Stall& s, long b) {
        if (s.from < 0 || s.blocks * kHost <= 960) return 0;
        return b == s.from + s.blocks ? 1 : b == s.from + s.blocks + 1 ? 2 : 0;
    }
    void send(long t, const SinkItem& it) {
        const long at = t + net + (jitter > 0 ? static_cast<long>(rng() % static_cast<unsigned>(jitter + 1)) : 0);
        auto pos = std::upper_bound(wire.begin(), wire.end(), at, [](long v, const Wire& w) { return v < w.at; });
        wire.insert(pos, Wire{at, it});
    }
    void sender(long b) {
        const long t = b * kHost;
        if (b == 0) send(t, item(SinkItem::Start, 0, 0, static_cast<std::uint16_t>(frame)));
        if (stalled(send_stall, b)) return;
        const long end = send_stall.from + send_stall.blocks;   // its first block after the stall
        const bool gap = send_stall.from >= 0 && send_stall.blocks * kHost > 960;
        if (gap && b == end + report - 1) {      // the gap: whole xrun blocks, the rest carried over
            xrun48 += send_stall.blocks * kHost + send_stall.off;
            const int n = static_cast<int>(xrun48 / frame);
            xrun48 -= static_cast<double>(n) * frame;
            for (int i = 0; i < n; ++i) send(t, item(SinkItem::Xrun, seq++));
            xruns_sent += n;
        }
        for (int i = 0; i < kHost; ++i) {
            captured[static_cast<std::size_t>(t + i)] = 1;
            pcm[2 * pcm_n] = pcm[2 * pcm_n + 1] = sig[static_cast<std::size_t>(t + i)];
            if (++pcm_n == frame) {
                if (gap && b >= end && b < end + report - 1) ++pre_report;
                SinkItem it = item(SinkItem::Data, seq++);
                it.len = static_cast<std::uint16_t>(opus_encode_float(enc, pcm.data(), frame, it.data, 1275));
                send(t, it);
                pcm_n = 0;
            }
        }
    }
    void receiver(long b) {
        const long t = b * kHost;
        if (stalled(recv_stall, b)) return;
        const std::int64_t now = 1000 + t * 1000000000LL / 48000;
        const int gap = recv_stall.blocks * kHost + recv_stall.off;
        const int a = after(recv_stall, b);
        if (a == 1) k.hostLate(gap, 48000, now);
        if (a == 2) k.hostGap(gap, 48000, now);
        k.hostBehind(recv_stall.from >= 0 && b == recv_stall.from + recv_stall.blocks ? recv_stall.blocks * kHost : 0);
        while (!wire.empty() && wire.front().at <= t) {
            k.apply(wire.front().it, now);
            wire.erase(wire.begin());
        }
        k.configure(11, kHost, 48000, 20);
        std::vector<float> o(2 * kHost);
        k.render(o.data(), kHost, 48000, now);
        for (int i = 0; i < kHost; ++i) out[static_cast<std::size_t>(t + i)] = o[2 * static_cast<std::size_t>(i)];
        std::fill(played.begin() + t, played.begin() + t + kHost, 1);
    }
    void run(long to_block) {
        for (long b = blocks_run; b < to_block; ++b) {
            sender(b);
            receiver(b);
        }
        blocks_run = to_block;
    }
    long blocks_run = 0;

    // How late the receiver plays what the sender played, in frames, over
    // its output's frames [from, from + 1920): the lag of the best
    // correlation, and the correlation.
    long delay(long from, double* corr = nullptr) const {
        long best = -1;
        double best_c = -2;
        for (long lag = 0; lag <= 4800; ++lag) {
            double c = 0, ex = 0, es = 0;
            for (long n = from; n < from + 1920; n += 2) {
                const double x = out[static_cast<std::size_t>(n)], y = sig[static_cast<std::size_t>(n - lag)];
                c += x * y;
                ex += x * x;
                es += y * y;
            }
            const double v = ex > 0 && es > 0 ? c / std::sqrt(ex * es) : 0;
            if (v > best_c) {
                best_c = v;
                best = lag;
            }
        }
        if (corr) *corr = best_c;
        return best;
    }
};

struct Counts {
    std::uint64_t concealed = 0;
    std::uint32_t underruns = 0, overruns = 0, xruns = 0, lost = 0, late = 0, skips = 0, host_gaps = 0;
    std::int64_t host_gap48 = 0;
    static Counts of(const SinkStats& s) {
        Counts c;
        c.concealed = s.concealed48.load();
        c.underruns = s.underruns.load();
        c.overruns = s.overruns.load();
        c.xruns = s.xrun_gaps.load();
        c.lost = s.lost.load();
        c.late = s.late.load();
        c.skips = s.skips.load();
        c.host_gaps = s.host_gaps.load();
        c.host_gap48 = s.host_gap48.load();
        return c;
    }
    Counts operator-(const Counts& o) const {
        Counts c;
        c.concealed = concealed - o.concealed;
        c.underruns = underruns - o.underruns;
        c.overruns = overruns - o.overruns;
        c.xruns = xruns - o.xruns;
        c.lost = lost - o.lost;
        c.late = late - o.late;
        c.skips = skips - o.skips;
        c.host_gaps = host_gaps - o.host_gaps;
        c.host_gap48 = host_gap48 - o.host_gap48;
        return c;
    }
};

// One stall of each host (blocks from the block `at`, -1 for none) in a
// stream of `frame`-frame blocks: what the receiver's sink counted over
// it, and its delay before and after it (frames).
struct StallResult {
    Counts c;
    long before = 0, after = 0;
    double corr_before = 0, corr_after = 0;
    int xruns_sent = 0;
    int pre_report = 0;                          // the frames the sender sent after its stall before its xrun blocks
    bool playing = false;
};

StallResult runStall(int frame, long send_at, int send_blocks, long recv_at, int recv_blocks, int net = 24,
                     int jitter = 0, int send_off = 0, int recv_off = 0, int send_carry = 0, int report = 2) {
    StallSim sim(frame);
    sim.report = report;
    sim.xrun48 = send_carry;                     // what the sender carried over from gaps before
    sim.net = net;
    sim.jitter = jitter;
    sim.send_stall = {send_at, send_blocks, send_off};
    sim.recv_stall = {recv_at, recv_blocks, recv_off};
    const long first = std::min(send_at < 0 ? 1L << 30 : send_at, recv_at < 0 ? 1L << 30 : recv_at);
    sim.run(first - 40);                         // 2 s or so: playing, its fill's level kept
    StallResult r;
    r.before = sim.delay((first - 60) * StallSim::kHost, &r.corr_before);
    const Counts c0 = Counts::of(sim.k.stats);
    sim.run(first + 290);                        // 0.6 s after the stall began
    r.c = Counts::of(sim.k.stats) - c0;
    r.after = sim.delay((first + 270) * StallSim::kHost, &r.corr_after);
    r.xruns_sent = sim.xruns_sent;
    r.pre_report = sim.pre_report;
    r.playing = sim.k.playing();
    return r;
}

void printStall(const char* what, int frame, const StallResult& r) {
    std::printf("    frame %d, %s: delay %ld -> %ld frames (correlation %.3f, %.3f); concealed %llu, dry %u, "
                "xruns %u (%d sent), lost %u, late %u, skips %u, host gaps %u (%lld frames skipped), overflows %u\n",
                frame, what, r.before, r.after, r.corr_before, r.corr_after,
                static_cast<unsigned long long>(r.c.concealed), r.c.underruns, r.c.xruns, r.xruns_sent, r.c.lost,
                r.c.late, r.c.skips, r.c.host_gaps, static_cast<long long>(r.c.host_gap48), r.c.overruns);
}

}  // namespace

// ENGINE.md 16.6, this host's gaps, at frames of 120 and 240 against host
// blocks of 128 frames. 1: a stall that both hosts miss alike is not
// heard: the receiver plays the sender's audio on without a break (nothing
// concealed, dry or skipped, no dropout counted) and its delay after is
// the one before. 2: when only the receiver's host missed time, it skips
// as much of the stream (to the nearest block), so its delay stays. 3:
// when only the sender's did, the hole is real: concealed, as long as it
// was. 4: in between (different lengths, one host a block or two after
// the other, stalls that overlap in part), no more is concealed than the
// sender's audio really misses, and no more skipped than the delay needs.
TEST(sink_host_stalls_alike_are_not_heard) {
    for (int frame : {120, 240}) {
        for (int blocks : {8, 12, 17, 30, 75}) {   // 21 to 200 ms
            for (int phase = 0; phase < 3; ++phase) {   // where in the sender's frame the stall falls
                const long at = 700 + phase;
                const StallResult r = runStall(frame, at, blocks, at, blocks);
                char what[80];
                std::snprintf(what, sizeof(what), "both hosts miss %.1f ms (phase %d)", blocks * 128 / 48.0, phase);
                printStall(what, frame, r);
                CHECK(r.playing);
                CHECK(r.corr_before > 0.9 && r.corr_after > 0.9);
                CHECK_EQ(r.c.concealed, 0u);
                CHECK_EQ(r.c.underruns, 0u);
                CHECK_EQ(r.c.xruns, 0u);
                CHECK_EQ(r.c.lost, 0u);
                CHECK_EQ(r.c.late, 0u);
                CHECK_EQ(r.c.skips, 0u);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.host_gap48, 0);
                CHECK_EQ(r.c.host_gaps, 1u);
                CHECK(r.xruns_sent > 0);
                CHECK(std::labs(r.after - r.before) <= 2);   // the same delay, to the codec's rounding
            }
        }
        // The hosts' measures of the gap differ, by up to a host block
        // (each its own clock's reading of when its blocks began), and the
        // sender carries over the rest of its gaps before, up to a frame.
        for (int carry : {0, frame / 2, frame - 8}) {
            for (int off : {-120, -40, -10, 0, 10, 40, 120}) {
                const StallResult r = runStall(frame, 700, 12, 700, 12, 24, 0, 0, off, carry);
                char what[96];
                std::snprintf(what, sizeof(what), "both hosts miss 32 ms, the sender's carry %d, the receiver's measure %+d",
                              carry, off);
                printStall(what, frame, r);
                CHECK_EQ(r.c.concealed, 0u);
                CHECK_EQ(r.c.underruns, 0u);
                CHECK_EQ(r.c.xruns, 0u);
                CHECK_EQ(r.c.host_gap48, 0);
                CHECK(std::labs(r.after - r.before) <= 2);
            }
        }
        // Over a slower path, with jitter: the xrun blocks come later.
        for (int net : {240, 720}) {
            const StallResult r = runStall(frame, 700, 12, 700, 12, net, 144);
            char what[80];
            std::snprintf(what, sizeof(what), "both hosts miss 32 ms, a path of %d to %d ms", net / 48, (net + 144) / 48);
            printStall(what, frame, r);
            CHECK_EQ(r.c.concealed, 0u);
            CHECK_EQ(r.c.underruns, 0u);
            CHECK_EQ(r.c.xruns, 0u);
            CHECK_EQ(r.c.host_gap48, 0);
            CHECK(std::labs(r.after - r.before) <= 2);
        }
    }
}

TEST(sink_host_stalls_of_one_host) {
    for (int frame : {120, 240}) {
        for (int blocks : {8, 12, 17, 30, 75}) {
            const double ms = blocks * 128 / 48.0;
            for (int phase = 0; phase < 3; ++phase) {
                const long at = 700 + phase;
                char what[80];
                // 2: the receiver's host alone: the stream skips the time missed, to the nearest block.
                StallResult r = runStall(frame, -1, 0, at, blocks);
                std::snprintf(what, sizeof(what), "the receiver's host misses %.1f ms (phase %d)", ms, phase);
                printStall(what, frame, r);
                const long missed = blocks * 128L;
                CHECK(r.playing);
                CHECK_EQ(r.c.concealed, 0u);
                CHECK_EQ(r.c.underruns, 0u);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.lost, 0u);
                CHECK_EQ(r.c.host_gaps, 1u);
                CHECK(std::labs(r.c.host_gap48 - missed) <= frame / 2);
                CHECK(std::labs(r.after - r.before) <= frame / 2);   // under a host block
                // 3: the sender's host alone: the hole is concealed, as long as it was.
                r = runStall(frame, at, blocks, -1, 0);
                std::snprintf(what, sizeof(what), "the sender's host misses %.1f ms (phase %d)", ms, phase);
                printStall(what, frame, r);
                CHECK(r.playing);
                CHECK_EQ(r.c.xruns, 1u);
                CHECK_EQ(r.c.host_gaps, 0u);
                CHECK_EQ(r.c.host_gap48, 0);
                CHECK(r.c.concealed >= static_cast<std::uint64_t>(r.xruns_sent * frame));
                CHECK(r.c.concealed <= static_cast<std::uint64_t>(missed + 2 * frame));
                CHECK(std::labs(r.after - r.before) <= frame);
            }
        }
        // 3, the sender's measure of its gap off by up to a host block (its
        // clock's reading of when its blocks began: a callback a little
        // late before the stall makes it short, one late after it long),
        // and a carry from its gaps before: its xrun blocks are whole
        // frames of what it measured, rounded down (5.4), here one fewer or
        // one more than the stall's own frames. The listener conceals just
        // those, and the frame that came late after them if one did, and
        // its delay moves by what they and the stall differ by: as before
        // this host's gaps changed (the sender's alone is no host gap here).
        // The hole concealed is never shorter than the stall by more than a
        // frame and the host block the measure may be short by.
        for (int blocks : {13, 17}) {           // 35 ms (1664 frames), 45 ms (2176)
            const long missed = blocks * 128L;
            for (int off : {-120, -40, -17, 17, 40, 120}) {
                for (int path = 0; path < 2; ++path) {
                    const int net = path == 0 ? 24 : 240, carry = path == 0 ? 0 : frame / 2;
                    for (int phase = 0; phase < 3; ++phase) {
                        const StallResult r = runStall(frame, 700 + phase, blocks, -1, 0, net, 0, off, 0, carry);
                        char what[112];
                        std::snprintf(what, sizeof(what), "the sender's host misses %.1f ms, its measure %+d, carry %d, path %d (phase %d)",
                                      blocks * 128 / 48.0, off, carry, net, phase);
                        printStall(what, frame, r);
                        const long xrun = static_cast<long>(r.xruns_sent) * frame;
                        CHECK(r.playing);
                        CHECK(r.corr_before > 0.9 && r.corr_after > 0.9);
                        CHECK_EQ(r.c.xruns, 1u);
                        CHECK_EQ(r.c.host_gaps, 0u);
                        CHECK_EQ(r.c.lost, 0u);
                        CHECK_EQ(r.c.overruns, 0u);
                        CHECK_EQ(r.c.skips, 0u);
                        CHECK(r.c.late <= static_cast<std::uint32_t>(128 / frame + 1));
                        CHECK_EQ(static_cast<long>(r.c.concealed), xrun + static_cast<long>(r.c.late) * frame);
                        CHECK(std::labs((r.after - r.before) - (xrun - missed)) <= 8);
                        CHECK(static_cast<long>(r.c.concealed) >= missed - frame - 128);
                        CHECK(static_cast<long>(r.c.concealed) <= missed + 3 * frame);
                        CHECK(std::labs(r.after - r.before) <= frame + 128);
                    }
                }
            }
        }
        // 3, the sender's report of its gap put off by a block or two (6.9:
        // the first block after the stall that doesn't come early reports
        // it; one that comes early, its host catching up on a late wake or
        // its callback jittering, puts it off): the frames its blocks
        // complete until then go before its xrun blocks, their turns come
        // in the hole, and they come late. The listener conceals their
        // turns as well as the xrun blocks: the hole is as long as with the
        // report on time, and the delay moves as much.
        for (int report : {3, 4}) {
            const long missed = 17 * 128L;
            for (int phase = 0; phase < 3; ++phase) {
                const StallResult r = runStall(frame, 700 + phase, 17, -1, 0, 24, 0, 0, 0, 0, report);
                char what[112];
                std::snprintf(what, sizeof(what), "the sender's host misses 45.3 ms, its report at its block %d after it (phase %d)",
                              report, phase);
                printStall(what, frame, r);
                const long xrun = static_cast<long>(r.xruns_sent) * frame;
                CHECK(r.playing);
                CHECK_EQ(r.c.xruns, 1u);
                CHECK_EQ(r.c.lost, 0u);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.skips, 0u);
                CHECK(r.pre_report >= (report - 1) * 128 / frame);   // the blocks before the report complete frames
                CHECK_EQ(static_cast<long>(r.c.late), static_cast<long>(r.pre_report));
                CHECK_EQ(static_cast<long>(r.c.concealed), xrun + static_cast<long>(r.c.late) * frame);
                CHECK(std::labs((r.after - r.before) - (xrun - missed)) <= 8);
            }
        }
    }
}

TEST(sink_host_stalls_in_between) {
    for (int frame : {120, 240}) {
        struct Case {
            const char* what;
            int send_from, send_blocks, recv_from, recv_blocks;
        };
        const Case cases[] = {
            {"the sender 32 ms, the receiver 45 ms", 0, 12, 0, 17},
            {"the sender 45 ms, the receiver 32 ms", 0, 17, 0, 12},
            {"the sender 32 ms, the receiver 80 ms", 0, 12, 0, 30},
            {"the sender 80 ms, the receiver 32 ms", 0, 30, 0, 12},
            {"both 32 ms, the receiver a block later", 0, 12, 1, 12},
            {"both 32 ms, the receiver two blocks later", 0, 12, 2, 12},
            {"both 32 ms, the sender a block later", 1, 12, 0, 12},
            {"both 32 ms, the sender two blocks later", 2, 12, 0, 12},
            {"both 32 ms, overlapping 16 ms, the sender first", 0, 12, 6, 12},
            {"both 32 ms, overlapping 16 ms, the receiver first", 6, 12, 0, 12},
            {"the sender 53 ms, the receiver 21 ms within it", 0, 20, 6, 8},
            {"the receiver 53 ms, the sender 21 ms within it", 6, 8, 0, 20},
            {"the sender 45 ms, the receiver 32 ms from 21 ms in", 0, 17, 8, 12},
        };
        for (const Case& cs : cases) {
            const long at = 700;
            const StallResult r = runStall(frame, at + cs.send_from, cs.send_blocks, at + cs.recv_from, cs.recv_blocks);
            printStall(cs.what, frame, r);
            const long sent = cs.send_blocks * 128L, missed = cs.recv_blocks * 128L;
            // The time the sender's audio misses beyond what this host
            // missed too, and the time this host missed beyond the
            // sender's: what is concealed and what is skipped, each to a
            // block or so (the xrun blocks are whole frames).
            const long s0 = cs.send_from * 128L, s1 = s0 + sent, r0 = cs.recv_from * 128L, r1 = r0 + missed;
            const long both = std::max(0L, std::min(s1, r1) - std::max(s0, r0));
            (void)both;
            const long conceal_max = std::max(0L, sent - missed) + 2 * frame;
            const long skip_max = std::max(0L, missed - sent) + frame;
            CHECK(r.playing);
            CHECK(r.corr_after > 0.9);
            CHECK(static_cast<long>(r.c.concealed) <= std::min(sent + frame, conceal_max));
            CHECK(r.c.host_gap48 <= skip_max);
            CHECK_EQ(r.c.overruns, 0u);
            CHECK_EQ(r.c.lost, 0u);
            CHECK(std::labs(r.after - r.before) <= frame + 128);
        }
        // The hosts' measures of the gaps differ, by up to a host block:
        // what is skipped still takes the delay no lower than it was by a
        // frame (the sender's xrun blocks are whole frames, rounded down),
        // nor leaves it higher than a frame and the slack each way.
        for (int c : {0, 2, 11}) {
            const Case& cs = cases[c];
            for (int off : {-120, -40, 40, 120}) {
                const long at = 700;
                const StallResult r =
                    runStall(frame, at + cs.send_from, cs.send_blocks, at + cs.recv_from, cs.recv_blocks, 24, 0, 0, off);
                char what[112];
                std::snprintf(what, sizeof(what), "%s, the receiver's measure %+d", cs.what, off);
                printStall(what, frame, r);
                const long sent = cs.send_blocks * 128L, missed = cs.recv_blocks * 128L;
                CHECK(r.playing);
                CHECK(r.corr_after > 0.9);
                CHECK(r.c.host_gap48 <= std::max(0L, missed - sent) + frame);
                CHECK(r.after - r.before > -frame);
                CHECK(r.after - r.before <= frame + 2 * 128);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.lost, 0u);
            }
        }
        // And the sender's measure off: its xrun blocks a frame fewer or
        // more than its stall's. What is concealed and skipped stays
        // within the bounds above; the delay may go lower by as much as the
        // xrun blocks fall short of the stall (a frame and the host block
        // its measure may be short by), as when the sender's host alone
        // stalled.
        for (int c : {0, 1, 2, 3, 11}) {
            const Case& cs = cases[c];
            for (int off : {-120, -40, 40, 120}) {
                const long at = 700;
                const StallResult r =
                    runStall(frame, at + cs.send_from, cs.send_blocks, at + cs.recv_from, cs.recv_blocks, 24, 0, off, 0);
                char what[112];
                std::snprintf(what, sizeof(what), "%s, the sender's measure %+d", cs.what, off);
                printStall(what, frame, r);
                const long sent = cs.send_blocks * 128L, missed = cs.recv_blocks * 128L;
                CHECK(r.playing);
                CHECK(r.corr_after > 0.9);
                CHECK(static_cast<long>(r.c.concealed) <= std::max(0L, sent - missed) + 2 * frame);
                CHECK(r.c.host_gap48 <= std::max(0L, missed - sent) + frame);
                CHECK(r.after - r.before > -(frame + 128));
                CHECK(r.after - r.before <= frame + 2 * 128);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.lost, 0u);
            }
        }
        // And the sender's report put off by a block or two: the frames
        // sent before its xrun blocks may come late (where this host's
        // stall was the shorter); what is concealed less them stays within
        // the bound above, and the delay and what is skipped as well.
        for (int c : {0, 1, 3}) {
            const Case& cs = cases[c];
            for (int report : {3, 4}) {
                const long at = 700;
                const StallResult r = runStall(frame, at + cs.send_from, cs.send_blocks, at + cs.recv_from, cs.recv_blocks, 24,
                                               0, 0, 0, 0, report);
                char what[112];
                std::snprintf(what, sizeof(what), "%s, the sender's report at its block %d after it", cs.what, report);
                printStall(what, frame, r);
                const long sent = cs.send_blocks * 128L, missed = cs.recv_blocks * 128L;
                CHECK(r.playing);
                CHECK(r.corr_after > 0.9);
                CHECK(static_cast<long>(r.c.late) <= static_cast<long>(r.pre_report));
                CHECK(static_cast<long>(r.c.concealed) - static_cast<long>(r.c.late) * frame <=
                      std::max(0L, sent - missed) + 2 * frame);
                CHECK(r.c.host_gap48 <= std::max(0L, missed - sent) + frame);
                CHECK(r.after - r.before > -frame);
                CHECK(r.after - r.before <= frame + 2 * 128);
                CHECK_EQ(r.c.overruns, 0u);
                CHECK_EQ(r.c.lost, 0u);
            }
        }
    }
}

int main() {
    std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);
    return testing::runAll();
}
