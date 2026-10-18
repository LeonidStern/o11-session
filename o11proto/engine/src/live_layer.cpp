// o11proto engine - Live mode's network side and shared state: the 0x07
// state, slots and health (control context), the source and the sinks'
// messages (network thread) and the capture and playback entry point (audio
// context). Section numbers are the live-mode spec's.
#include "live_layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus/opus.h>

namespace o11proto {
namespace engine {

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

std::int64_t steadyNs(TimePoint t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// A full-period generator over 0..2^31-1: ids that look random and don't
// repeat within 2^31 draws (5.9).
std::int32_t nextId(std::uint32_t& x) {
    x = (x * 1103515245u + 12345u) & 0x7fffffffu;
    return static_cast<std::int32_t>(x);
}

}  // namespace

// --------------------------------------------------------------- helpers

int liveBufferNeed(int host_block, int host_rate) {
    if (host_rate <= 0) return kLiveMinBufferMs;
    return static_cast<int>(std::ceil(2000.0 * host_block / host_rate + 5.0 - 1e-9));
}

int liveBufferInUse(int setting_ms, int host_block, int host_rate) {
    return std::min(kLiveMaxBufferMs, std::max({setting_ms, kLiveMinBufferMs, liveBufferNeed(host_block, host_rate)}));
}

int liveLongestGoodBlock(int host_rate) {
    int n = 64;
    while (n < (1 << 20) && liveBufferNeed(n * 2, host_rate) <= kLiveMaxBufferMs) n *= 2;
    return n;
}

bool parseWholeNumber(const std::string& s, long* out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i >= s.size()) return false;
    long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v < 100000000) v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

std::string liveDisplayName(const std::string& name) {
    const std::size_t at = name.find('@');
    if (at == std::string::npos) return name;
    const std::string user = name.substr(0, at);
    std::size_t start = at + 1;
    const std::size_t colon = name.rfind(':');
    if (colon != std::string::npos && colon > at) start = colon + 1;
    const std::string addr = name.substr(start);
    const std::size_t dot = addr.rfind('.');
    const std::size_t digits = dot == std::string::npos ? 0 : addr.size() - dot - 1;
    if (std::count(addr.begin(), addr.end(), '.') == 4 && digits >= 1 && digits <= 2 &&
        std::all_of(addr.begin() + static_cast<std::ptrdiff_t>(dot) + 1, addr.end(),
                    [](char c) { return c >= '0' && c <= '9'; }))
        return user + addr.substr(dot);          // a ".2"-style suffix
    return user;
}

// ------------------------------------------------------------- lifecycle

LiveLayer::LiveLayer() {
    for (int s = 0; s < kLiveSlots; ++s) {
        queue[s].reset(new Spsc<SinkItem, 256>);
        sinks[s].reset(new LiveSink);
        outbuf_[s].reset(new float[2 * kLiveMaxHostBlock]());
        concealed_from[s].store(-1);
        quiet_run_[s] = 1 << 29;
    }
    src_.history.reset(new Source::Sent[kHistory]);
    std::uint32_t seed[2] = {0, 0};
    if (!o11proto::live::systemRandom(seed, sizeof(seed))) {
        const auto t = static_cast<std::uint64_t>(steadyNs(Clock::now()));
        seed[0] = static_cast<std::uint32_t>(t);
        seed[1] = static_cast<std::uint32_t>(t >> 32) ^ 0x5bd1e995u;
    }
    src_.next_stream = seed[0] & 0x7fffffffu;
    src_.next_format = seed[1] & 0x7fffffffu;
    transport_.on_data = [this](std::uint32_t from, const std::uint8_t* p, std::size_t n) { onData(from, p, n); };
    transport_.on_tick = [this] { onTick(); };
    transport_.on_closing = [this] { onClosing(); };
}

LiveLayer::~LiveLayer() {
    transport_.stop();
    if (src_.enc) opus_encoder_destroy(src_.enc);
}

void LiveLayer::bumpGen(int s) { net[s].gen = gen[s].fetch_add(1) + 1; }

void LiveLayer::resetNet() {
    for (int s = 0; s < kLiveSlots; ++s) {
        net[s].clearStream();
        bumpGen(s);
    }
    src_.running = false;
    for (auto& p : peers) p.start_sent = false;
    stop_to.clear();
}

bool LiveLayer::start(const std::string& a, std::uint16_t p, std::string& error) {
    stop();
    // The network thread isn't running: the queues it consumes can be emptied here.
    while (capture.front()) capture.pop();
    while (requests.front()) requests.pop();
    {
        std::lock_guard<std::mutex> lock(net_mutex);
        resetNet();
    }
    o11proto::live::Transport::Options o;
    o.server_address = a;
    o.port = p;
    o.peer_id = msg.peer_id;
    o.key = msg.key;
    o.test_receive_loss = test_loss_;
    addr = a;
    port = p;
    id = msg.peer_id;
    key = msg.key;
    if (!transport_.start(o, error)) return false;
    running.store(true);
    return true;
}

void LiveLayer::stop() {
    running.store(false);
    if (transport_.running()) transport_.stop();   // on_closing sends the source's /stops
    std::lock_guard<std::mutex> lock(net_mutex);
    resetNet();
}

void LiveLayer::forget() {
    stop();
    has_msg = false;
    msg = o11proto::live::ServerLive();
    addr.clear();
    port = 0;
    id = 0;
    key = 0;
    room_on.store(false);
    may_send.store(false);
    room = o11proto::live::LiveRoom();
    has_room = false;
    report_sent = o11proto::live::LiveReport();
    report_ever = false;
    report_at = TimePoint{};
    room_noticed = 0;
    advice_said.clear();
    advice_said_reason = o11proto::live::Advice::None;
    advice_said_count = 0;
    room_due_ns.store(0);
    room_announce.store((++room_seq) << 1, std::memory_order_release);   // intervals, at the next boundary
    start_failed_said = no_answer_said = offer_said = false;
    started_at = retry_at = TimePoint{};
    buffer_changed = false;
    std::lock_guard<std::mutex> lock(net_mutex);
    peers.clear();
    net_room_on = false;
    for (int s = 0; s < kLiveSlots; ++s) {
        const std::uint32_t g = net[s].gen;
        net[s] = NetSlot();
        net[s].gen = g;
        slot_peer[s] = 0;
        slot_name[s].clear();
        slot_health[s] = Health();
    }
}

// ------------------------------------------------- the message and slots

void LiveLayer::apply(const o11proto::live::ServerLive& m) {
    msg = m;
    has_msg = true;
    room_on.store(m.roomOn());
    may_send.store(m.maySend());
    std::lock_guard<std::mutex> lock(net_mutex);
    const bool was_on = net_room_on;
    net_room_on = m.roomOn();
    const int frame = o11proto::live::validFrame(m.frame_samples) ? m.frame_samples : 240;
    const int kbps = std::max(32, std::min(510, static_cast<int>(m.live_kbps)));
    if (kbps != src_.kbps) {
        src_.kbps = kbps;
        src_.kbps_changed = true;
    }
    if (frame != src_.frame) {
        src_.frame = frame;
        src_.frame_changed = true;
    }
    // 6.2: every peer but the client itself and id 0, up to 64, each once.
    std::vector<Peer> next;
    for (const auto& r : m.peers) {
        if (r.id == 0 || r.id == m.peer_id) continue;
        if (next.size() >= static_cast<std::size_t>(kLiveListed)) break;
        bool dup = false;
        for (const auto& q : next) dup |= q.id == r.id;
        if (dup) continue;
        Peer* old = findPeer(r.id);
        Peer p = old ? *old : Peer();
        p.id = r.id;
        p.name = r.name;
        p.bound = (r.flags & o11proto::live::kPeerBound) != 0;
        p.sending = (r.flags & o11proto::live::kPeerSending) != 0;
        p.slot = -1;
        next.push_back(std::move(p));
    }
    for (const auto& old : peers) {             // a sink removed mid-stream gets a /stop
        bool still = false;
        for (const auto& q : next) still |= q.id == old.id;
        if (!still && src_.running && old.start_sent) stop_to.push_back(old.id);
    }
    // A listed peer keeps its slot; a slot whose peer left is freed at once.
    for (int s = 0; s < kLiveSlots; ++s) {
        if (!net[s].peer) continue;
        Peer* p = nullptr;
        for (auto& q : next)
            if (q.id == net[s].peer) p = &q;
        if (p) {
            p->slot = s;
        } else {
            net[s] = NetSlot();
            bumpGen(s);
        }
    }
    // Free slots go to listed peers without one, in list order.
    for (auto& p : next) {
        if (p.slot >= 0) continue;
        for (int s = 0; s < kLiveSlots; ++s)
            if (!net[s].peer) {
                net[s] = NetSlot();
                net[s].peer = p.id;
                bumpGen(s);
                p.slot = s;
                break;
            }
    }
    peers.swap(next);
    if (was_on && !net_room_on) {               // 6.4: the room went off: every source is dropped
        for (int s = 0; s < kLiveSlots; ++s)
            if (net[s].peer) {
                net[s].clearStream();
                bumpGen(s);
            }
    }
    for (int s = 0; s < kLiveSlots; ++s) {
        slot_peer[s] = net[s].peer;
        slot_name[s].clear();
        for (const auto& p : peers)
            if (p.id == net[s].peer) slot_name[s] = p.name;
    }
}

int LiveLayer::slotOf(const std::string& name) const {
    for (int s = 0; s < kLiveSlots; ++s)
        if (slot_peer[s] && slot_name[s] == name) return s;
    return -1;
}

void LiveLayer::health(TimePoint now) {
    const std::int64_t now_ns = steadyNs(now);
    const std::int64_t two_s = 2000000000LL;
    for (int s = 0; s < kLiveSlots; ++s) {
        Health& h = slot_health[s];
        const std::uint32_t g = gen[s].load();
        if (h.gen != g) {
            h = Health();
            h.gen = g;
            h.last_audio_ns = now_ns;
        }
        const SinkStats& st = sinks[s]->stats;
        if (!slot_peer[s] || st.gen.load() != g || !room_on.load() || !running.load()) {
            h.healthy = false;
            h.recent = h.lost2 = 0;
            continue;
        }
        // dropouts: underruns, overruns and xrun gaps
        const std::uint32_t drops = st.underruns.load() + st.overruns.load() + st.xrun_gaps.load();
        const std::uint32_t d = std::min(drops - h.prev_drops, 16u);
        h.prev_drops = drops;
        for (std::uint32_t i = 0; i < d; ++i) h.drops[h.ndrops++ % 16] = now_ns;
        if (h.ndrops > 1 << 20) h.ndrops = 16 + h.ndrops % 16;
        int recent = 0;
        for (int i = 0; i < std::min(h.ndrops, 16); ++i) recent += now_ns - h.drops[i] <= two_s ? 1 : 0;
        // blocks lost for good, per 100 ms, over the last 2 s
        const std::uint32_t lost = st.lost.load();
        const std::uint32_t dl = std::min(lost - h.prev_lost, 1000u);
        h.prev_lost = lost;
        const std::int64_t b = now_ns / 100000000LL;
        if (h.bucket == 0) h.bucket = b;
        for (std::int64_t k = h.bucket + 1; k <= b && k <= h.bucket + 20; ++k) h.lost[k % 20] = 0;
        h.bucket = b;
        h.lost[b % 20] += static_cast<int>(dl);
        int lost2 = 0;
        for (int v : h.lost) lost2 += v;
        const int frame = st.block.load() > 0 ? st.block.load() : 240;
        const int limit = 2880 / frame;          // floor(0.03 x 2 x 48000 / frame)
        const bool active = st.active.load() && now_ns - st.active_since_ns.load() >= 1000000000LL;
        h.recent = recent;
        h.lost2 = lost2;
        h.limit = limit;
        if (st.active.load()) h.last_audio_ns = now_ns;
        h.healthy = active && recent <= 2 && lost2 <= limit;
    }
}

// ENGINE.md 16.7. Clean: Live runs here and every listed player that
// sends sounds clean. Becoming clean takes each such slot healthy (6.4:
// playing for 1 s, at most 2 dropouts and the loss limit in 2 s); a report
// that is clean stays so until a slot has more than 2 dropouts in 2 s, more
// losses than the limit, or no audio for more than 1 s while its player
// still sends and no /stop came. So one gap of up to a second doesn't move
// the room.
o11proto::live::LiveReport LiveLayer::report(TimePoint now) {
    namespace lv = o11proto::live;
    lv::LiveReport r;
    if (sending.load()) r.flags |= lv::kReportSending;
    if (!running.load()) {
        r.flags |= lv::kReportCantLive;
        r.fault = msg.peer_id;
        return r;
    }
    const std::int64_t now_ns = steadyNs(now);
    const bool was_clean = report_ever && report_sent.clean() && !report_sent.cantLive();
    std::vector<std::uint32_t> bad;
    bool cant = false;
    {
        std::lock_guard<std::mutex> lock(net_mutex);
        for (const auto& p : peers) {
            if (p.slot < 0) {                   // more senders than slots: one this client can't hear
                cant |= p.sending;
                continue;
            }
            Health& h = slot_health[p.slot];
            if (p.sending && !h.expected) h.last_audio_ns = std::max(h.last_audio_ns, now_ns);
            h.expected = p.sending;
            if (!p.sending) continue;           // nobody's fault (a stopped host, Transmit off)
            const NetSlot& ns = net[p.slot];
            const bool stopped = ns.has_stream && ns.stopped;
            const bool silent = !stopped && now_ns - h.last_audio_ns > 1000000000LL;
            const bool fails = h.recent > 2 || h.lost2 > h.limit || silent;
            if (was_clean ? fails : !h.healthy) bad.push_back(p.id);
        }
    }
    if (cant) {
        r.flags |= lv::kReportCantLive;
        r.fault = msg.peer_id;
    } else if (bad.empty()) {
        r.flags |= lv::kReportClean;
    } else {
        r.fault = bad.size() == 1 ? bad[0] : msg.peer_id;
    }
    return r;
}

void LiveLayer::applyRoom(const o11proto::live::LiveRoom& m, TimePoint now) {
    room = m;
    has_room = true;
    room_due_ns.store(steadyNs(now) + 500000000LL);
    room_announce.store(((++room_seq) << 1) | (m.live ? 1u : 0u), std::memory_order_release);
}

// ======================================================= network thread

LiveLayer::Peer* LiveLayer::findPeer(std::uint32_t pid) {
    for (auto& p : peers)
        if (p.id == pid) return &p;
    return nullptr;
}

bool LiveLayer::hostRunning(std::int64_t now_ns) const {
    const std::int64_t end = last_block_end_ns.load();
    return end != 0 && now_ns <= end + 250000000LL;
}

void LiveLayer::pushItem(int s, SinkItem::Kind kind, std::int32_t seq, const std::uint8_t* data, std::size_t len,
                         int block) {
    SinkItem* it = queue[s]->back();
    if (!it) return;                            // full: newer ones are dropped
    it->kind = kind;
    it->gen = net[s].gen;
    it->seq = seq;
    it->len = static_cast<std::uint16_t>(len);
    it->block = static_cast<std::uint16_t>(block);
    if (len) std::memcpy(it->data, data, len);
    queue[s]->push();
}

// 6.3, the receive filter.
void LiveLayer::onData(std::uint32_t from, const std::uint8_t* p, std::size_t n) {
    std::lock_guard<std::mutex> lock(net_mutex);
    if (!net_room_on || n == 0 || n > aoo::kMaxMessage) return;
    const TimePoint now = Clock::now();
    Peer* peer = findPeer(from);
    const int slot = peer ? peer->slot : -1;
    const bool host = hostRunning(steadyNs(now));
    if (p[0] & 0x80) {                          // binary: /data and the resend request only
        if (n < 4 || p[1] != 0 || p[2] != aoo::kId || p[3] != aoo::kId) return;
        if (p[0] == aoo::kTypeSink) {
            if (slot >= 0 && host) sinkData(slot, p, n, now);
        } else if (p[0] == aoo::kTypeSource) {
            if (peer) sourceRequest(*peer, p, n, now);
        }
        return;
    }
    aoo::OscMessage m;
    if (!aoo::parseOsc(p, n, m)) return;
    if (m.dest == aoo::Dest::Sink) {
        if (slot >= 0 && host) sinkOsc(slot, m, now);
    } else if (peer) {
        sourceOsc(*peer, m, now);
    }
}

void LiveLayer::sinkData(int s, const std::uint8_t* p, std::size_t n, TimePoint now) {
    aoo::Block b;
    if (!aoo::parseData(p, n, b)) return;
    NetSlot& ns = net[s];
    if (!ns.has_stream || b.stream != ns.stream) {   // a stream it doesn't know: ask for its /start
        if (now - ns.last_start_request >= milliseconds(100)) {
            send(ns.peer, msgbuf_, aoo::writeStartRequest(msgbuf_));
            ns.last_start_request = now;
        }
        return;
    }
    if (ns.stopped || b.channel != 0 || b.split) return;   // split blocks count as lost
    if (!b.xrun && (b.len == 0 || b.len > aoo::kMaxBlockBytes)) return;
    ns.last_data = now;
    if (!ns.got_data) {                         // the first ping goes with the first block
        ns.got_data = true;
        ns.last_ping = TimePoint{};
    }
    pushItem(s, b.xrun ? SinkItem::Xrun : SinkItem::Data, b.seq, b.data, b.xrun ? 0 : b.len, ns.block);
}

void LiveLayer::sinkOsc(int s, const aoo::OscMessage& m, TimePoint now) {
    NetSlot& ns = net[s];
    switch (m.cmd) {
    case aoo::Cmd::Start: {
        aoo::StartInfo si;
        if (!aoo::readStart(m, si)) return;     // a format this client doesn't play
        if (ns.has_stream && si.stream == ns.stream) return;   // already has it: no restart
        ns.clearStream();
        ns.has_stream = true;
        ns.stream = si.stream;
        ns.block = si.block;
        ns.source_latency = si.latency + si.codec_delay;
        ns.last_data = now;
        ns.last_stop_request = now;
        pushItem(s, SinkItem::Start, si.start_seq, nullptr, 0, si.block);
        return;
    }
    case aoo::Cmd::Stop:
        if (m.count >= 2 && m.args[1].tag == 'i' && ns.has_stream && !ns.stopped && m.args[1].i == ns.stream) {
            ns.stopped = true;
            pushItem(s, SinkItem::Stop, 0, nullptr, 0, ns.block);
        }
        return;
    case aoo::Cmd::Ping: {                      // only for a stream it plays, 1 per 100 ms
        if (m.count < 2 || m.args[1].tag != 't' || !ns.has_stream || ns.stopped) return;
        if (now - ns.last_pong < milliseconds(100)) return;
        const std::uint64_t t2 = aoo::ntpWallNow();
        const SinkStats& st = sinks[s]->stats;
        const std::uint32_t lost = st.gen.load() == ns.gen ? st.lost.load() : 0;
        float loss = 0;
        if (ns.pong_t2 != 0) {
            const double dt = aoo::ntpDiffSeconds(t2, ns.pong_t2);
            if (dt > 0) loss = static_cast<float>(std::min(1.0, (lost - ns.pong_lost) * double(ns.block) / (48000.0 * dt)));
        }
        ns.pong_t2 = t2;
        ns.pong_lost = lost;
        send(ns.peer, msgbuf_, aoo::writeSourcePong(msgbuf_, m.args[1].t, t2, aoo::ntpWallNow(), loss));
        ns.last_pong = now;
        return;
    }
    case aoo::Cmd::Pong: {                      // the answer to this sink's ping: the round trip
        if (m.count < 4 || m.args[1].tag != 't' || m.args[2].tag != 't' || m.args[3].tag != 't') return;
        const std::uint64_t t4 = aoo::ntpWallNow();
        const double rtt = aoo::ntpDiffSeconds(t4, m.args[1].t) - aoo::ntpDiffSeconds(m.args[3].t, m.args[2].t);
        if (rtt < 0 || rtt > 10) return;
        ns.rtt_ms = ns.rtt_ms < 0 ? rtt * 1000 : 0.7 * ns.rtt_ms + 0.3 * rtt * 1000;
        return;
    }
    default:
        return;                                 // the OSC form of /data isn't taken here
    }
}

void LiveLayer::sourceRequest(Peer& peer, const std::uint8_t* p, std::size_t n, TimePoint now) {
    if (!src_.running) return;
    std::int32_t stream = 0;
    std::int32_t seqs[aoo::kMaxRequests];
    int count = 0;
    if (!aoo::parseRequest(p, n, stream, seqs, count) || stream != src_.stream) return;
    // 5.10: 0.5 x blocks per second for all sinks (burst 32), 0.25 x per sink (burst 16)
    const double bps = 48000.0 / src_.frame;
    auto refill = [&](double& tokens, TimePoint& at, double rate, double burst) {
        if (at != TimePoint{}) tokens = std::min(burst, tokens + std::chrono::duration<double>(now - at).count() * rate);
        at = now;
    };
    refill(src_.all_tokens, src_.all_refill, 0.5 * bps, 32);
    refill(peer.tokens, peer.refill, 0.25 * bps, 16);
    for (int i = 0; i < count; ++i) {
        const Source::Sent& h = src_.history[seqs[i] % kHistory];
        if (h.seq != seqs[i] || now - h.when > seconds(1)) continue;   // no longer in the history
        if (src_.all_tokens < 1 || peer.tokens < 1) continue;          // over budget: dropped
        send(peer.id, h.bytes, h.len);
        src_.all_tokens -= 1;
        peer.tokens -= 1;
    }
}

void LiveLayer::sourceOsc(Peer& peer, const aoo::OscMessage& m, TimePoint now) {
    switch (m.cmd) {
    case aoo::Cmd::Start:                       // a start request: the running stream's /start
        if (m.count >= 2 && m.args[1].tag == 's' && aoo::versionOk(m.args[1].s, m.args[1].len) && src_.running &&
            now - peer.start_answered >= milliseconds(100)) {
            sendStart(peer);
            peer.start_answered = now;
        }
        return;
    case aoo::Cmd::Stop:                        // a stop request: answered while stopped (quirk 20: 1 per 100 ms)
        if (m.count >= 2 && m.args[1].tag == 'i' && !src_.running && now - peer.stop_answered >= milliseconds(100)) {
            send(peer.id, msgbuf_, aoo::writeStop(msgbuf_, m.args[1].i, src_.seq));
            peer.stop_answered = now;
        }
        return;
    case aoo::Cmd::Ping:
        if (m.count >= 2 && m.args[1].tag == 't' && now - peer.pong_answered >= milliseconds(100)) {
            const std::uint64_t t2 = aoo::ntpWallNow();
            send(peer.id, msgbuf_, aoo::writeSinkPong(msgbuf_, m.args[1].t, t2, aoo::ntpWallNow()));
            peer.pong_answered = now;
        }
        return;
    default:
        return;                                 // pongs to the source and OSC data requests: not used
    }
}

void LiveLayer::onTick() {
    std::lock_guard<std::mutex> lock(net_mutex);
    const TimePoint now = Clock::now();
    drainCapture(now);
    for (std::uint32_t pid : stop_to) send(pid, msgbuf_, aoo::writeStop(msgbuf_, src_.stream, src_.seq));
    stop_to.clear();
    if (src_.running && src_.sent48 > 0) {
        for (auto& p : peers)                   // a sink that became active mid-stream
            if (!p.start_sent) sendStart(p);
        if (now - src_.last_ping >= seconds(1)) {
            const std::size_t len = aoo::writeSinkPing(msgbuf_, aoo::ntpWallNow());
            for (auto& p : peers)
                if (p.start_sent) send(p.id, msgbuf_, len);
            src_.last_ping = now;
        }
    }
    for (int s = 0; s < kLiveSlots; ++s) {
        NetSlot& ns = net[s];
        if (!ns.peer || !ns.has_stream) continue;
        if (now - ns.last_data >= seconds(5)) { // the source timed out
            pushItem(s, SinkItem::Reset, 0, nullptr, 0, ns.block);
            ns.clearStream();
            continue;
        }
        if (ns.stopped) continue;
        if (ns.got_data && now - ns.last_ping >= seconds(1)) {
            send(ns.peer, msgbuf_, aoo::writeSourcePing(msgbuf_, aoo::ntpWallNow()));
            ns.last_ping = now;
        }
        if (now - ns.last_data >= seconds(1) && now - ns.last_stop_request >= seconds(1)) {
            send(ns.peer, msgbuf_, aoo::writeStopRequest(msgbuf_, ns.stream));
            ns.last_stop_request = now;
        }
    }
    sendRequests(now);
}

void LiveLayer::onClosing() {
    std::lock_guard<std::mutex> lock(net_mutex);
    if (src_.running) stopStream();             // quirk 21: the /stops go out before the socket closes
}

void LiveLayer::sendRequests(TimePoint now) {
    std::int32_t seqs[kLiveSlots][aoo::kMaxRequests];
    int count[kLiveSlots] = {};
    for (int k = 0; k < 1024; ++k) {
        RequestItem* r = requests.front();
        if (!r) break;
        const int s = r->slot;
        if (s < kLiveSlots && r->gen == net[s].gen && net[s].has_stream && count[s] < aoo::kMaxRequests)
            seqs[s][count[s]++] = r->seq;
        requests.pop();
    }
    for (int s = 0; s < kLiveSlots; ++s) {
        if (!count[s] || now - net[s].last_request_msg < milliseconds(10)) continue;   // 1 per sink per 10 ms
        send(net[s].peer, msgbuf_, aoo::writeRequest(msgbuf_, net[s].stream, seqs[s], count[s]));
        net[s].last_request_msg = now;
    }
}

// ------------------------------------------------------------- the source

void LiveLayer::drainCapture(TimePoint now) {
    for (int k = 0; k < 1024; ++k) {
        CaptureItem* c = capture.front();
        if (!c) break;
        switch (c->kind) {
        case CaptureItem::Audio: {
            if (!src_.running || src_.frame_changed) {   // a new frame size is a new format and stream
                if (src_.running) stopStream();
                startStream(c->rate, now);
            }
            if (!src_.running) break;
            if (src_.kbps_changed) {            // from the next packet, same stream
                opus_encoder_ctl(src_.enc, OPUS_SET_BITRATE(src_.kbps * 1000));
                src_.kbps_changed = false;
            }
            if (c->rate != src_.in_rate) {
                src_.in_rate = c->rate;
                src_.t = 0;
            }
            for (int i = 0; i < c->frames; ++i) {
                const float l = c->data[2 * i], r = c->data[2 * i + 1];
                if (src_.in_rate == 48000) {
                    src_.pcm[2 * src_.pcm_n] = l;
                    src_.pcm[2 * src_.pcm_n + 1] = r;
                    if (++src_.pcm_n == src_.frame) encodeAndSend(now);
                    continue;
                }
                for (int ch = 0; ch < 2; ++ch) { // cubic, host rate -> 48 kHz
                    src_.x[0][ch] = src_.x[1][ch];
                    src_.x[1][ch] = src_.x[2][ch];
                    src_.x[2][ch] = src_.x[3][ch];
                }
                src_.x[3][0] = l;
                src_.x[3][1] = r;
                const double step = src_.in_rate / 48000.0;
                while (src_.t < 1.0) {
                    const float t = static_cast<float>(src_.t);
                    for (int ch = 0; ch < 2; ++ch) {
                        const float a = src_.x[0][ch], b = src_.x[1][ch], d = src_.x[2][ch], e = src_.x[3][ch];
                        src_.pcm[2 * src_.pcm_n + ch] =
                            b + 0.5f * t * (d - a + t * (2.0f * a - 5.0f * b + 4.0f * d - e + t * (3.0f * (b - d) + e - a)));
                    }
                    if (++src_.pcm_n == src_.frame) encodeAndSend(now);
                    src_.t += step;
                }
                src_.t -= 1.0;
            }
            break;
        }
        case CaptureItem::Stop:
            if (src_.running) stopStream();
            break;
        case CaptureItem::Gap:
            // 5.4: floor(g x 48000 / rate / frame) empty blocks, the rest carried over
            if (src_.running && c->rate > 0) {
                src_.xrun48 += static_cast<double>(c->gap) * 48000.0 / c->rate;
                const int n = static_cast<int>(src_.xrun48 / src_.frame);
                src_.xrun48 -= static_cast<double>(n) * src_.frame;
                for (int i = 0; i < n; ++i) {
                    Source::Sent& h = src_.history[src_.seq % kHistory];
                    h.seq = src_.seq;
                    h.when = now;
                    h.len = static_cast<std::uint16_t>(aoo::writeXrun(h.bytes, src_.stream, src_.seq));
                    send(0, h.bytes, h.len);
                    src_.seq = src_.seq >= aoo::kMaxSequence ? 0 : src_.seq + 1;
                    src_.sent48 += src_.frame;
                }
            }
            break;
        }
        capture.pop();
    }
}

void LiveLayer::startStream(int rate, TimePoint now) {
    if (!src_.enc || src_.enc_frame != src_.frame) {
        if (src_.enc) opus_encoder_destroy(src_.enc);
        int err = 0;
        src_.enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (!src_.enc) return;
        src_.enc_frame = src_.frame;
        src_.format = nextId(src_.next_format); // a new format id each time the format is set
    }
    opus_encoder_ctl(src_.enc, OPUS_RESET_STATE);
    // Opus's prediction from one frame to the next stays on: without it the
    // codec's noise is 9 dB louder on a held tone. A sink primes its
    // decoder after a loss instead (live_sink.cpp, LiveSink::prime()).
    opus_encoder_ctl(src_.enc, OPUS_SET_BITRATE(src_.kbps * 1000));
    src_.kbps_changed = src_.frame_changed = false;
    src_.stream = nextId(src_.next_stream);
    src_.seq = 0;
    src_.start_time = now;
    src_.sent48 = 0;
    src_.in_rate = rate;
    src_.t = 0;
    std::memset(src_.x, 0, sizeof(src_.x));
    src_.pcm_n = 0;
    src_.xrun48 = 0;
    src_.last_ping = TimePoint{};
    for (auto& p : peers) p.start_sent = false;  // each gets /start with the first block
    src_.running = true;
}

void LiveLayer::stopStream() {
    const std::size_t len = aoo::writeStop(msgbuf_, src_.stream, src_.seq);
    for (auto& p : peers) {
        if (p.start_sent) send(p.id, msgbuf_, len);
        p.start_sent = false;
    }
    src_.running = false;
    src_.pcm_n = 0;
}

void LiveLayer::sendStart(Peer& peer) {
    aoo::StartInfo si;
    si.stream = src_.stream;
    si.start_seq = src_.seq;
    si.format = src_.format;
    si.channels = 2;
    si.rate = 48000;
    si.block = src_.frame;
    // the stream time: its start, plus the audio sent so far (quirk 22: at 48 kHz, as sent)
    si.time = aoo::ntpFromSteady(src_.start_time + std::chrono::nanoseconds(src_.sent48 * 1000000000LL / 48000));
    const int rate = src_.in_rate > 0 ? src_.in_rate : 48000;
    const double r = 48000.0 / rate;
    const double resampler = rate % 48000 == 0 ? 0.0 : 2.0;
    si.latency = static_cast<std::int32_t>(std::max(0.0, src_.frame - 64.0 * r) + resampler * r);
    si.codec_delay = aoo::kCodecDelay;
    si.offset = 0;
    send(peer.id, msgbuf_, aoo::writeStart(msgbuf_, si));
    peer.start_sent = true;
}

void LiveLayer::encodeAndSend(TimePoint now) {
    std::uint8_t pkt[1275];
    const int n = opus_encode_float(src_.enc, src_.pcm, src_.frame, pkt, sizeof(pkt));
    src_.pcm_n = 0;
    if (n < 1) return;
    for (auto& p : peers)                       // /start once the stream's first audio is ready
        if (!p.start_sent) sendStart(p);
    Source::Sent& h = src_.history[src_.seq % kHistory];
    h.seq = src_.seq;
    h.when = now;
    h.len = static_cast<std::uint16_t>(aoo::writeData(h.bytes, src_.stream, src_.seq, 48000.0, pkt,
                                                      static_cast<std::size_t>(n)));
    send(0, h.bytes, h.len);                    // once, to the room (5.8)
    src_.seq = src_.seq >= aoo::kMaxSequence ? 0 : src_.seq + 1;
    src_.sent48 += src_.frame;
}

// ======================================================== audio context

void LiveLayer::audioBlock(const float* l, const float* r, bool transmit, int frames, int rate) {
    const std::int64_t now = steadyNs(Clock::now());
    if (rate <= 0 || frames <= 0) return;
    const std::int64_t block0 = frames_;
    frames_ += frames;
    host_rate.store(rate);
    // H: the longest host block of the last 30 s (6.8)
    if (frames >= longest_) {
        longest_ = frames;
        longest_seen_ns_ = now;
    } else if (now - longest_seen_ns_ > 30000000000LL) {
        longest_ = frames;
        longest_seen_ns_ = now;
    }
    host_block.store(longest_);
    // Host gaps (6.9), reported at the first block that doesn't arrive early.
    const std::int64_t dur = static_cast<std::int64_t>(frames) * 1000000000LL / rate;
    int gap = 0;
    if (expect_ns_ != 0) {
        const std::int64_t threshold =
            std::max<std::int64_t>(20000000LL, 2LL * longest_ * 1000000000LL / rate);
        const bool early = prev_dur_ns_ > 0 && now - prev_start_ns_ < prev_dur_ns_ * 9 / 10;
        if (!early) {
            const std::int64_t late = now - expect_ns_;
            if (late > threshold && late_pending_) {
                gap = static_cast<int>(std::min<std::int64_t>(late, 500000000LL) * rate / 1000000000LL);
                late_pending_ = false;
                expect_ns_ = now;
            } else if (late > threshold) {
                late_pending_ = true;
            } else {
                late_pending_ = false;
                expect_ns_ = now;
            }
        }
    } else {
        expect_ns_ = now;
    }
    prev_start_ns_ = now;
    prev_dur_ns_ = dur;
    expect_ns_ += dur;

    // The source (6.7): channel 0 while every gate is open.
    const bool on = room_on.load() && running.load();
    const bool gate = on && may_send.load() && transmit && host_playing.load() && !host_offline.load() && l;
    sending.store(gate);
    if (gap > 0 && gate_open_ && gate) {
        if (CaptureItem* c = capture.back()) {
            c->kind = CaptureItem::Gap;
            c->rate = rate;
            c->gap = gap;
            c->frames = 0;
            capture.push();
        }
    }
    if (gate_open_ && !gate) {
        if (CaptureItem* c = capture.back()) {
            c->kind = CaptureItem::Stop;
            capture.push();
        }
    }
    if (gate) {
        if (!r) r = l;                          // a mono input is sent as L = R
        for (int at = 0; at < frames; at += CaptureItem::kFrames) {
            CaptureItem* c = capture.back();
            if (!c) break;                      // the network thread is behind: dropped
            const int n = std::min(CaptureItem::kFrames, frames - at);
            c->kind = CaptureItem::Audio;
            c->rate = rate;
            c->frames = static_cast<std::uint16_t>(n);
            for (int i = 0; i < n; ++i) {
                c->data[2 * i] = l[at + i];
                c->data[2 * i + 1] = r[at + i];
            }
            capture.push();
        }
    }
    gate_open_ = gate;

    // The sinks (5.9).
    const int latency = latency_ms.load();
    for (int s = 0; s < kLiveSlots; ++s) {
        LiveSink& k = *sinks[s];
        const std::uint32_t g = gen[s].load();
        if (g != audio_gen_[s]) {               // a new peer or a dropped source
            k.drop();
            k.stats.clear(g);
            audio_gen_[s] = g;
        }
        for (int i = 0; i < 256; ++i) {
            SinkItem* it = queue[s]->front();
            if (!it) break;
            if (it->gen == g) k.apply(*it, now);
            queue[s]->pop();
        }
        k.configure(latency, longest_, rate);
        if (gap > 0) k.hostGap(gap, rate);
        avail_[s] = false;
        quiet0_[s] = std::min(quiet_run_[s], 1 << 29);
        first_real_[s] = -1;
        quiet_from_[s] = 0;
        quiet_run_[s] = quiet0_[s] + frames;     // unless it plays its stream's audio
        if (frames > kLiveMaxHostBlock) continue;
        // Also while Live is off: a stream that was just dropped conceals
        // on, so that the mixer can fade it out (16.8).
        avail_[s] = k.render(outbuf_[s].get(), frames, rate, now) && !host_offline.load();
        if (avail_[s] && k.firstReal() >= 0) {
            first_real_[s] = k.firstReal();
            quiet_from_[s] = k.quietFrom();
            quiet_run_[s] = frames - quiet_from_[s];
        }
        if (first_real_[s] >= 0 ? quiet_from_[s] < frames : quiet0_[s] == 0)   // its audio ends here
            concealed_from[s].store(block0 + (first_real_[s] >= 0 ? quiet_from_[s] : 0));
        if (!on) continue;
        std::int32_t seqs[16];
        const int n = k.dueRequests(now, seqs, 16);
        for (int i = 0; i < n; ++i) {
            RequestItem* q = requests.back();
            if (!q) break;
            q->slot = static_cast<std::uint8_t>(s);
            q->gen = g;
            q->seq = seqs[i];
            requests.push();
        }
    }
    last_block_end_ns.store(now + dur);
}

}  // namespace engine
}  // namespace o11proto
