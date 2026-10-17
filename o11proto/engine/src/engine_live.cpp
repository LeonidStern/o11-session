// o11proto engine - Live mode inside the engine (ENGINE.md section 16):
// the Live client's life, the health report and the room's mode, mixing
// each user's channel 0 from its live and interval audio with seamless
// switches, and the /live texts. The state and the network side are in
// live_layer.cpp. Section numbers are the live-mode spec's.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "live_layer.h"
#include "o11proto/engine.h"

namespace o11proto {
namespace engine {

namespace {

float clip1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

std::int64_t steadyNs(TimePoint t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

std::string num(double v, int decimals = 0) {    // rounded as printf rounds (section 1)
    char b[48];
    std::snprintf(b, sizeof(b), decimals ? "%.1f" : "%.0f", v);
    return b;
}

std::string trimmed(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lowered(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string longBlockText(int host_block, int rate) {
    return "Your DAW's audio buffer (" + std::to_string(host_block) + " samples) needs at least " +
           std::to_string(liveBufferNeed(host_block, rate)) +
           " ms, more than Live can use (200 ms), so expect dropouts: " + std::to_string(liveLongestGoodBlock(rate)) +
           " samples or less plays well.";
}

// Live plays here: the room's switch, the Live client, a place among the
// senders, and the host rendering in real time.
bool liveOn(const LiveLayer& L) {
    return L.room_on.load() && L.running.load() && L.may_send.load() && !L.host_offline.load();
}

}  // namespace

void Engine::LiveDeleter::operator()(LiveLayer* p) const { delete p; }
void Engine::LiveMixDeleter::operator()(LiveMix* p) const { delete p; }

void Engine::liveInit() { live_.reset(new LiveLayer); }

void Engine::liveNotice(const std::string& text, bool error) {
    if (onLiveNotice) onLiveNotice(text, error);
}

// ------------------------------------------------------- control context

void Engine::liveMessage(const live::ServerLive& m) { live_->apply(m); }

void Engine::liveRawMessage(const RawMessage& m) {
    live::LiveRoom r;
    if (m.type != live::kMsgServerLiveRoom || !client_.loggedIn() || !live::decode(m.payload, r)) return;
    live_->applyRoom(r, Clock::now());
}

void Engine::liveReset() { live_->forget(); }

void Engine::setLiveBufferMs(int ms) {
    live_setting_ms_.store(std::max(kLiveMinBufferMs, std::min(kLiveMaxBufferMs, ms)));
}

int Engine::liveBufferInUseMs() const {
    return liveBufferInUse(live_setting_ms_.load(), live_->host_block.load(), live_->host_rate.load());
}

bool Engine::parseLiveBufferSetting(const std::string& value, int* ms) {
    long v = 0;
    if (!parseWholeNumber(value, &v)) return false;
    *ms = static_cast<int>(std::max<long>(kLiveMinBufferMs, std::min<long>(kLiveMaxBufferMs, v)));
    return true;
}

void Engine::setLiveHostState(bool playing, bool offline) {
    live_->host_playing.store(playing);
    live_->host_offline.store(offline);
}

bool Engine::liveRunning() const { return live_->running.load(); }

void Engine::setLiveTestLoss(double fraction) { live_->setTestLoss(fraction); }

void Engine::setLiveTestSendGap(int ms) { live_->setTestSendGap(ms); }

bool Engine::userLive(int user) const {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = userAt(user);
    return u && u->live_now.load();
}

bool Engine::liveRoomLive() const { return live_->room_applied.load() == 1; }

bool Engine::liveServerHas() const { return live_->has_msg && live_->msg.udpOpen(); }

bool Engine::liveRoomSwitchOn() const { return live_->has_msg && live_->msg.roomOn(); }

int Engine::liveHeardMs() const {
    const LiveLayer& L = *live_;
    if (!liveRoomLive()) return -1;
    std::vector<double> v;
    for (int s = 0; s < kLiveSlots; ++s) {
        const SinkStats& st = L.sinks[s]->stats;
        if (!L.slot_peer[s] || st.gen.load() != L.gen[s].load() || !st.active.load()) continue;
        const double ms = liveOneWayMs(s, nullptr, nullptr, nullptr);
        if (ms >= 0) v.push_back(ms);
    }
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const std::size_t k = v.size() / 2;
    const double median = v.size() % 2 ? v[k] : (v[k - 1] + v[k]) / 2;
    return static_cast<int>(std::nearbyint(median));   // rounded as num() rounds
}

Engine::LiveReason Engine::liveRoomReason() const {
    const LiveLayer& L = *live_;
    if (liveRoomLive() || !L.has_msg || !L.msg.roomOn() || !L.has_room || L.room.live) return LiveReason::None;
    switch (L.room.reason) {
    case live::RoomReason::Dropouts: return LiveReason::Dropouts;
    case live::RoomReason::CantLive: return LiveReason::CantLive;
    case live::RoomReason::Joining: return LiveReason::Joining;
    default: return LiveReason::None;
    }
}

std::string Engine::liveRoomReasonUser() const {
    return liveRoomReason() == LiveReason::None ? std::string() : live_->room.user;
}

// The one-way estimate of 6.10 for a slot, or -1 without a round trip.
double Engine::liveOneWayMs(int slot, double* buffer_ms, double* network_ms, double* codec_ms) const {
    LiveLayer& L = *live_;
    const SinkStats& st = L.sinks[slot]->stats;
    double rtt = -1;
    std::int32_t source_latency = 0;
    int block = 240;
    {
        std::lock_guard<std::mutex> lock(L.net_mutex);
        rtt = L.net[slot].rtt_ms;
        source_latency = L.net[slot].source_latency;
        block = L.net[slot].block;
    }
    const double h = L.host_rate.load() > 0 ? L.host_rate.load() : 48000.0;
    const double codec_samples =
        source_latency * h / 48000.0 + std::max(0.0, 64.0 - block * h / 48000.0) + 2.0 * h / 48000.0;
    const double codec = codec_samples / h * 1000.0;
    const double buffer = std::max(0, st.buffer_us.load()) / 1000.0;
    const double network = rtt > 0 ? rtt / 2 : 0;
    if (buffer_ms) *buffer_ms = buffer;
    if (network_ms) *network_ms = network;
    if (codec_ms) *codec_ms = codec;
    if (rtt < 0) return -1;
    return network + codec + buffer + 64.0 / h * 1000.0;
}

void Engine::liveControl() {
    LiveLayer& L = *live_;
    const TimePoint now = Clock::now();

    // 6.1: the Live client runs while connected and the message has UDP open.
    if (client_.loggedIn() && L.has_msg && L.msg.udpOpen()) {
        const std::string addr = client_.serverAddress();
        const std::uint16_t port = L.msg.udp_port ? L.msg.udp_port : client_.serverPort();
        const bool changed = addr != L.addr || port != L.port || L.msg.peer_id != L.id || L.msg.key != L.key;
        if (changed || (!L.running.load() && now >= L.retry_at)) {
            if (changed) L.no_answer_said = false;   // said again after a restart for a new id, key, address or port
            std::string err;
            if (L.start(addr, port, err)) {
                L.started_at = now;
            } else {
                L.retry_at = now + std::chrono::seconds(10);
                if (!L.start_failed_said) {
                    L.start_failed_said = true;
                    liveNotice("Live mode can't start here (" + err + "), so you hear everyone through intervals.", true);
                }
            }
        }
    } else if (L.running.load() || !L.addr.empty()) {
        L.stop();
        L.addr.clear();
        L.port = 0;
    }

    // 6.8: the buffer in use; a change restarts the sinks' buffering.
    const int B = liveBufferInUse(live_setting_ms_.load(), L.host_block.load(), L.host_rate.load());
    if (B != L.latency_ms.load()) {
        L.latency_ms.store(B);
        L.buffer_changed = true;
        L.buffer_changed_at = now;
        for (int s = 0; s < kLiveSlots; ++s) {
            const SinkStats& st = L.sinks[s]->stats;
            L.changed_gen[s] = st.gen.load();
            L.changed_drops[s] = st.underruns.load() + st.overruns.load() + st.lost.load();
        }
    }

    // 6.4: health. Then each user's slot, and a mixer for each user's
    // channel 0 once the server has Live (made here, outside the lock).
    L.health(now);
    const bool room = L.room_on.load() && L.running.load();
    std::size_t need = 0;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (auto& up : users_) {
            RemoteUser& u = *up;
            const int slot = room ? L.slotOf(u.name) : -1;
            u.live_want = slot;
            u.live_want_gen = slot >= 0 ? L.gen[slot].load() : 0;
            if (L.has_msg && !u.live_mix) ++need;
        }
    }
    if (need > 0) {
        std::vector<std::unique_ptr<LiveMix, LiveMixDeleter>> made;
        for (std::size_t i = 0; i < need; ++i) made.emplace_back(new LiveMix);
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (auto& up : users_)
            if (!up->live_mix && !made.empty()) {
                up->live_mix = std::move(made.back());
                made.pop_back();
            }
    }

    // 16.7: the health report, when it changes, at most 4 times a second.
    if (client_.loggedIn() && L.has_msg) {
        const live::LiveReport r = L.report(now);
        if ((!L.report_ever || r != L.report_sent) && now - L.report_at >= std::chrono::milliseconds(250)) {
            client_.sendRaw(live::kMsgClientLiveReport, live::encode(r));
            L.report_sent = r;
            L.report_ever = true;
            L.report_at = now;
        }
    }
    liveRoomNotices();

    // 6.12: the offer, and the lack of an answer.
    if (L.running.load()) {
        const std::string p = std::to_string(L.port);
        if (!L.msg.roomOn() && !L.offer_said) {
            L.offer_said = true;
            liveNotice("This server has Live mode: type !live on, and the O11 Session players in the room hear each "
                       "other in real time (it needs UDP to the server's port " + p + ").", false);
        }
        if (L.msg.roomOn() && !L.serverAnswered() && now - L.started_at >= std::chrono::seconds(5) &&
            !L.no_answer_said) {
            L.no_answer_said = true;
            liveNotice("Live: the server doesn't answer on UDP port " + p +
                       ", so you hear everyone through intervals. A firewall or router may block UDP; whoever "
                       "hosts the server needs UDP port " + p + " forwarded too.", true);
        }
    }
}

// The room's switches as this engine plays them: a line when it goes Live
// or back on intervals, and once per user who can't do Live. None when the
// room's Live goes off: the server says that in the chat.
void Engine::liveRoomNotices() {
    LiveLayer& L = *live_;
    const int applied = L.room_applied.load();
    const std::string& u = L.room.user;
    const bool reasoned = L.has_msg && L.msg.roomOn() && L.has_room && !L.room.live && !u.empty();
    const bool mine = !u.empty() && u == client_.username();
    const std::string who = liveDisplayName(u);
    if (applied != L.room_noticed) {
        if (applied == 1) {
            const int heard = liveHeardMs();
            liveNotice("The room is Live now" +
                           (heard >= 0 ? ", about " + std::to_string(heard) + " ms late." : std::string(".")),
                       false);
            L.cant_said.clear();
        } else if (reasoned && L.room.reason == live::RoomReason::Dropouts) {
            liveNotice(mine ? std::string("The room is back on intervals: your connection had dropouts.")
                            : "The room is back on intervals: " + who + "'s connection had dropouts.",
                       false);
        } else if (reasoned && L.room.reason == live::RoomReason::Joining) {
            liveNotice("The room is back on intervals while " + who + " joins.", false);
        }
        L.room_noticed = applied;
    }
    if (applied == 0 && reasoned && L.room.reason == live::RoomReason::CantLive && u != L.cant_said) {
        L.cant_said = u;
        liveNotice(mine ? std::string("The room is on intervals: your client can't do Live.")
                        : "The room is on intervals: " + who + " can't do Live.",
                   false);
    }
}

// ------------------------------------------------------- `/live` (6.10, 6.11)

bool Engine::liveCommand(const std::string& text) {
    if (text.size() < 5 || lowered(text.substr(0, 5)) != "/live" || (text.size() > 5 && text[5] != ' ')) return false;
    const std::string rest = lowered(trimmed(text.substr(5)));
    if (rest.empty()) {
        liveStatus();
        return true;
    }
    if (rest == "on" || rest == "off") {
        if (status_.load() != Status::Ok || !client_.loggedIn())
            liveNotice("Not connected: Live mode is the room's, so join a room first.", true);
        else
            say("!live " + rest);
        return true;
    }
    if (rest.compare(0, 6, "buffer") == 0) {
        const std::string n = trimmed(rest.substr(6));   // "buffer30" reads as "buffer 30" (quirk 12)
        const int old = live_setting_ms_.load();
        if (n.empty()) {
            liveNotice("Live's jitter buffer is " + std::to_string(old) +
                           " ms. Type /live buffer 30 to change it (5 to 200).",
                       false);
            return true;
        }
        long v = 0;
        if (!parseWholeNumber(n, &v) || v < kLiveMinBufferMs || v > kLiveMaxBufferMs) {
            liveNotice("/live buffer takes a number of ms from 5 to 200, such as /live buffer 30.", true);
            return true;
        }
        setLiveBufferMs(static_cast<int>(v));
        const int H = live_->host_block.load(), sr = live_->host_rate.load();
        const int B = liveBufferInUse(static_cast<int>(v), H, sr);
        std::string s = "Live's jitter buffer is " + std::to_string(v) + " ms now (it was " + std::to_string(old) +
                        "). Less is less late, more has fewer dropouts: type /live to see them.";
        if (liveBufferNeed(H, sr) > kLiveMaxBufferMs)
            s += " " + longBlockText(H, sr);
        else if (B > v)
            s += " Your DAW's audio buffer (" + std::to_string(H) + " samples) needs at least " + std::to_string(B) +
                 " ms, so Live uses " + std::to_string(B) + " ms.";
        liveNotice(s, false);
        return true;
    }
    liveNotice("Type /live to see how the Live players sound, or /live buffer 30 to set Live's jitter buffer (ms).", true);
    return true;
}

void Engine::liveStatus() {
    LiveLayer& L = *live_;
    const int S = live_setting_ms_.load();
    if (status_.load() != Status::Ok || !client_.loggedIn()) {
        liveNotice("Not connected. In a room with Live mode, /live shows how late you hear each O11 Session player. "
                   "Live's jitter buffer is " + std::to_string(S) + " ms (/live buffer N to change it).",
                   false);
        return;
    }
    if (!L.has_msg) return liveNotice("This server doesn't have Live mode, so everyone plays through intervals.", false);
    if (!L.msg.udpOpen())
        return liveNotice("This server's Live mode is off, so everyone plays through intervals.", false);
    if (!L.msg.roomOn())
        return liveNotice("Live is off in this room. Type !live on, and the O11 Session players here hear each other "
                          "in real time.",
                          false);
    if (!L.running.load())
        return liveNotice("Live is on in this room, but yours couldn't start, so you hear everyone through intervals.",
                          false);

    // The head line.
    const int H = L.host_block.load(), sr = L.host_rate.load() > 0 ? L.host_rate.load() : 48000;
    const int B = liveBufferInUse(S, H, sr);
    std::string head = "Live is on. Jitter buffer " + std::to_string(B) + " ms";
    if (liveBufferNeed(H, sr) > B)
        head += ". " + longBlockText(H, sr) + " ";
    else if (B > S)
        head += ": your DAW's audio buffer (" + std::to_string(H) + " samples) needs that much, more than the " +
                std::to_string(S) + " ms you set. ";
    else
        head += " (/live buffer N to change it). ";
    head += L.serverAnswered() ? "Round trip to the server " + num(L.serverRoundTripMs(), 1) + " ms."
                               : std::string("The server doesn't answer over UDP yet.");
    bool transmit = false;
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        auto it = locals_.find(0);
        transmit = it != locals_.end() && it->second->s.broadcast;
    }
    if (!L.msg.maySend())
        head += " The room has no room for more Live players, so you hear everyone, and they hear you, through intervals.";
    else if (!transmit)
        head += " Transmit is off, so you send nothing.";
    else if (!L.host_playing.load())
        head += " The host is stopped, so you send nothing until it plays.";

    // The room's mode first (16.7): everyone plays Live, or everyone intervals.
    const std::string& ru = L.room.user;
    const bool mine = !ru.empty() && ru == client_.username();
    const std::string who = liveDisplayName(ru);
    std::string room;
    if (liveRoomLive())
        room = "The room is Live.";
    else if (L.has_room && L.room.live)
        room = "The room goes Live at the next interval.";
    else if (L.has_room && L.room.reason == live::RoomReason::Dropouts)
        room = "The room is on intervals: " + (mine ? std::string("your") : who + "'s") + " connection had dropouts.";
    else if (L.has_room && L.room.reason == live::RoomReason::CantLive)
        room = "The room is on intervals: " + (mine ? std::string("your client") : who) + " can't do Live.";
    else if (L.has_room && L.room.reason == live::RoomReason::Joining)
        room = "The room is on intervals while " + who + " joins.";
    else
        room = "The room is on intervals until everyone's Live has sounded clean for a few seconds.";
    liveNotice(room, false);
    liveNotice(head, false);

    // One line per listed peer.
    struct Listed {
        std::string name;
        int slot;
        bool sending;
    };
    std::vector<Listed> listed;
    {
        std::lock_guard<std::mutex> lock(L.net_mutex);
        for (const auto& p : L.peers) listed.push_back({p.name, p.slot, p.sending});
    }
    const TimePoint now = Clock::now();
    const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    for (const auto& p : listed) {
        std::string line = liveDisplayName(p.name) + ": ";
        if (p.slot < 0) {
            line += "intervals (O11 Session hears 8 Live players at most).";
            liveNotice(line, false);
            continue;
        }
        const SinkStats& st = L.sinks[p.slot]->stats;
        const bool counts = st.gen.load() == L.gen[p.slot].load();
        if (!counts || !st.active.load()) {
            line += p.sending ? "no real-time audio from them" : "not sending (their host is stopped or Transmit is off)";
        } else {
            bool live_now = false;
            {
                std::lock_guard<std::mutex> lock(remote_mutex_);
                for (auto& up : users_)
                    if (up->name == p.name) live_now = up->live_now.load();
            }
            line += live_now ? "live"
                             : L.slot_health[p.slot].healthy ? "intervals, their real-time audio sounds clean"
                                                             : "intervals, their real-time audio has dropouts";
            double buffer = 0, network = 0, codec = 0;
            double oneway = liveOneWayMs(p.slot, &buffer, &network, &codec);
            if (oneway < 0) oneway = network + codec + buffer + 64.0 / sr * 1000.0;
            const int block = st.block.load() > 0 ? st.block.load() : 240;
            const double since = (now_ns - st.active_since_ns.load()) / 1e9;
            double loss = 0;
            if (since >= block / 48000.0) {
                const double bad = double(st.lost.load() - st.lost_at_active.load()) +
                                   double(st.resent.load() - st.resent_at_active.load());
                loss = std::min(100.0, 100.0 * bad / (since * 48000.0 / block));
            }
            line += ", about " + num(oneway) + " ms late (buffer " + num(buffer) + " ms, network " + num(network) +
                    " ms, codec " + num(codec) + " ms), " + num(loss, 1) + "% of the packets lost";
        }
        const std::uint32_t u = counts ? st.underruns.load() : 0, o = counts ? st.overruns.load() : 0,
                            d = counts ? st.lost.load() : 0;
        const std::uint32_t all = u + o + d;
        const std::string parts = " (" + std::to_string(u) + " ran dry, " + std::to_string(o) + " overflowed, " +
                                  std::to_string(d) + " packets lost for good)";
        if (L.buffer_changed) {
            const std::uint32_t before = L.changed_gen[p.slot] == st.gen.load() ? L.changed_drops[p.slot] : 0;
            const double t = std::chrono::duration<double>(now - L.buffer_changed_at).count();
            line += ". Dropouts: " + std::to_string(all - before) + " in the " + num(t) + " s since the buffer changed, " +
                    std::to_string(all) + " in all" + parts;
        } else {
            line += ". Dropouts: " + std::to_string(all) + parts;
        }
        liveNotice(line + ".", false);
    }
    // Then everyone else in the room.
    std::vector<std::string> others;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (auto& up : users_) {
            bool is_listed = false;
            for (const auto& p : listed) is_listed |= p.name == up->name;
            if (!is_listed) others.push_back(up->name);
        }
    }
    for (const auto& n : others)
        liveNotice(liveDisplayName(n) + ": intervals (not a Live player: an older client, or the room is full)", false);
}

// --------------------------------------------------------- audio context

// At the start of process(), both mutexes held.
void Engine::liveAudioBlock(const float* const* in, int numIn, int frames, int sampleRate) {
    LiveLayer& L = *live_;
    const float* l = nullptr;
    const float* r = nullptr;
    bool transmit = false;
    auto it = locals_.find(0);                   // Live sends channel 0 only (6.7)
    if (it != locals_.end() && maxchan_.load() > 0) {
        const LocalChannel& s = it->second->s;
        transmit = s.broadcast;
        if (s.source >= 0 && s.source < numIn) {
            l = in[s.source];
            r = s.stereo && s.source + 1 < numIn ? in[s.source + 1] : l;
        }
    }
    L.audioBlock(l, r, transmit, frames, sampleRate);
    // 16.8, at once: when Live stops playing here, this mixer plays the
    // room on intervals from this block (each user crossfades).
    if (!liveOn(L)) {
        L.mix_live = false;
        L.room_applied.store(0);
    }
}

// 16.7: the mixer takes the room's latest mode at the first boundary half a
// second after it came, for every user at once; before 8.2's steps.
void Engine::liveBoundary() {
    LiveLayer& L = *live_;
    const std::uint64_t a = L.room_announce.load(std::memory_order_acquire);
    if (a != L.mix_announce && steadyNs(Clock::now()) >= L.room_due_ns.load()) {
        L.mix_announce = a;
        L.mix_mode = (a & 1) != 0;
    }
    L.mix_live = L.mix_mode && liveOn(L);
    L.room_applied.store(L.mix_live ? 1 : 0);
    if (!L.mix_live) return;
    for (auto& u : users_) {
        const int want = u->live_want;
        if (want >= 0 && L.gen[want].load() == u->live_want_gen) {
            u->live_slot = want;
            u->live_gen = u->live_want_gen;
        }
    }
}

// Channel 0's interval audio for `len` frames, as mixRemote() plays it but
// before volume and pan: interleaved stereo into `dst`, silence when there
// is none.
void Engine::liveIntervalAudio(RemoteUser& u, int len, int sampleRate, float* dst) {
    std::fill(dst, dst + 2 * static_cast<std::size_t>(len), 0.0f);
    RemoteChannel& rc = u.ch[0];
    if (!rc.present || rc.ignoresIntervals() || !rc.current) return;
    Decoder& dec = *rc.current;
    const int srcRate = dec.rate();
    const int needed = rc.rs.needed(srcRate, sampleRate, len);
    dec.decodeUntil(static_cast<std::size_t>(needed) + 2);
    if (rc.drop_count > 0) {
        std::size_t drop = std::min(static_cast<std::size_t>(rc.drop_count), dec.available());
        dec.skip(drop);
        rc.drop_count -= static_cast<long>(drop);
        dec.decodeUntil(static_cast<std::size_t>(needed) + 2);
    }
    if (dec.available() < static_cast<std::size_t>(needed)) {
        rc.drop_count += needed - static_cast<long>(dec.available());
        dec.skip(dec.available());
        rc.rs.advance(srcRate, sampleRate, len, needed);
        return;
    }
    const int ch = dec.channels();
    float* src = const_cast<float*>(dec.data());
    const std::size_t avail = dec.available();
    for (std::size_t i = 0; i < static_cast<std::size_t>(needed) * static_cast<std::size_t>(ch); ++i) src[i] = clip1(src[i]);
    const bool same = srcRate == sampleRate;
    const double ratio = static_cast<double>(srcRate) / sampleRate;
    for (int i = 0; i < len; ++i) {
        float s1, s2;
        if (same) {
            const float* f = src + static_cast<std::size_t>(i) * static_cast<std::size_t>(ch);
            s1 = f[0];
            s2 = ch > 1 ? f[1] : f[0];
        } else {
            double p = rc.rs.phase + i * ratio;
            std::size_t i0 = static_cast<std::size_t>(p);
            float frac = static_cast<float>(p - static_cast<double>(i0));
            std::size_t i1 = std::min(i0 + 1, avail - 1);
            if (i0 >= avail) i0 = avail - 1;
            const float* a = src + i0 * static_cast<std::size_t>(ch);
            const float* b = src + i1 * static_cast<std::size_t>(ch);
            s1 = a[0] + (b[0] - a[0]) * frac;
            s2 = ch > 1 ? a[1] + (b[1] - a[1]) * frac : s1;
        }
        dst[2 * i] = s1;
        dst[2 * i + 1] = s2;
    }
    dec.skip(static_cast<std::size_t>(needed));
    rc.rs.advance(srcRate, sampleRate, len, needed);
}

namespace {

// 16.8: before path `in` fades in against the other, it is read so far
// behind that the two line up when they carry the same sound (one held
// note, heard an interval apart and live), and `rho` says how alike they
// are then. Equal-power gains keep the level of two unlike sounds; the
// mixer scales them down by the likeness, so two alike ones keep theirs too.
void lineUp(LiveMix& m, int in, int rate) {
    const int o = 1 - in;
    if (m.pos[in] > 0) return;                   // still heard: its delay stays
    m.delay[in] = 0;
    m.rho = 0;
    if (m.pos[o] <= 0) return;                   // nothing to cross with
    constexpr int R = LiveMix::kRing - 1;
    const int W = std::max(16, std::min(512, rate * 8 / 1000));   // 8 ms compared
    const int T = std::max(8, std::min(256, rate * 4 / 1000));    // over lags up to 4 ms
    float x[512];
    double ex = 0;
    for (int k = 0; k < W; ++k) {
        const int f = (m.w - m.delay[o] - W + k) & R;
        x[k] = m.ring[o][2 * f] + m.ring[o][2 * f + 1];
        ex += static_cast<double>(x[k]) * x[k];
    }
    if (ex < W * 1e-8) return;                   // silence going out
    double r[257];
    double best = -2;
    for (int d = 0; d <= T; ++d) {
        double c = 0, ey = 0;
        for (int k = 0; k < W; ++k) {
            const int f = (m.w - d - W + k) & R;
            const float y = m.ring[in][2 * f] + m.ring[in][2 * f + 1];
            c += static_cast<double>(x[k]) * y;
            ey += static_cast<double>(y) * y;
        }
        r[d] = ey > W * 1e-8 ? c / std::sqrt(ex * ey) : 0.0;
        best = std::max(best, r[d]);
    }
    if (best >= 0.8) {                           // alike: the shortest lag that lines them up
        int lag = 0;
        while (r[lag] < best - 0.02) ++lag;
        m.delay[in] = lag;
        m.rho = static_cast<float>(std::min(1.0, r[lag]));
    } else {                                     // unlike (music an interval apart): no lag
        m.rho = static_cast<float>(std::max(0.0, std::min(1.0, r[0])));
    }
}

}  // namespace

// 6.6 and 16.8: channel 0 of every user with a mixer, from its interval
// audio and its slot's live audio, with channel 0's settings, into its
// pair. The path the room's mode picks is heard; each switch crossfades
// over 20 ms on an equal-power curve, and while the room is Live a slot
// without audio is concealed for kLiveConcealBuffers x B, then fades out,
// and fades in again when its audio comes back. Interval audio is never
// heard while the room is Live.
void Engine::mixLive(float* const* out, int numOut, int len, int at, int sampleRate) {
    LiveLayer& L = *live_;
    const bool any_solo = any_solo_.load();
    const int offset = remote_offset_.load();
    const float step = static_cast<float>(1000.0 / (kLiveFadeMs * sampleRate));
    const int hold = static_cast<int>(kLiveConcealBuffers * L.latency_ms.load() * sampleRate / 1000.0);
    constexpr int R = LiveMix::kRing - 1;
    constexpr float kPi = 3.14159265359f, kHalfPi = 1.57079632679f;
    for (auto& up : users_) {
        RemoteUser& u = *up;
        if (!u.live_mix) continue;
        LiveMix& m = *u.live_mix;
        RemoteChannel& rc = u.ch[0];
        if (!rc.present) {                       // no channel 0: nothing to play, and a fresh start
            m.pos[0] = 1.0f;
            m.pos[1] = 0.0f;
            m.delay[0] = m.delay[1] = 0;
            m.rho = 0;
            m.path_live = false;
            m.quiet = 1 << 30;
            u.live_slot = -1;
            u.live_now.store(false);
            continue;
        }
        const float vol = std::min(4.0f, std::max(0.0f, u.volume * rc.volume));
        const float pan = std::min(1.0f, std::max(-1.0f, u.pan + rc.pan));
        const bool muted = any_solo ? !rc.solo : (rc.mute || u.mute);
        const bool audible = !muted && vol >= 1e-7f && numOut > 0;
        int idx = 0;
        bool mono = false;
        if (numOut > 0) outputPair(rc.outChannel, offset, numOut, &idx, &mono);
        float gl, gr;
        panGains(pan, &gl, &gr);
        float pk0 = 0, pk1 = 0;
        for (int done = 0; done < len;) {
            const int n = std::min(LiveMix::kChunk, len - done);
            liveIntervalAudio(u, n, sampleRate, m.ibuf);
            // The live path: the slot it plays, else the user's slot, so
            // that its audio is there to line up with when it comes in.
            int src = u.live_slot;
            if (src < 0 && u.live_want >= 0 && L.gen[u.live_want].load() == u.live_want_gen) src = u.live_want;
            if (src >= 0 && L.available(src)) {
                const float* p = L.out(src) + 2 * static_cast<std::size_t>(at + done);
                for (int k = 0; k < 2 * n; ++k) m.lbuf[k] = clip1(p[k]);
            } else {
                std::fill(m.lbuf, m.lbuf + 2 * n, 0.0f);
            }
            const bool slot_ok = u.live_slot >= 0 && L.gen[u.live_slot].load() == u.live_gen;
            m.quiet = slot_ok && L.real(u.live_slot) ? 0 : std::min(m.quiet, 1 << 29) + n;
            const bool live = L.mix_live;
            if (live != m.path_live) {
                m.path_live = live;
                lineUp(m, live ? LiveMix::kLive : LiveMix::kIntervals, sampleRate);
            }
            const float target[2] = {live ? 0.0f : 1.0f, live && slot_ok && m.quiet <= hold ? 1.0f : 0.0f};
            for (int k = 0; k < n; ++k) {
                const int f = (m.w + k) & R;
                m.ring[0][2 * f] = m.ibuf[2 * k];
                m.ring[0][2 * f + 1] = m.ibuf[2 * k + 1];
                m.ring[1][2 * f] = m.lbuf[2 * k];
                m.ring[1][2 * f + 1] = m.lbuf[2 * k + 1];
            }
            for (int i = 0; i < n; ++i) {
                float g[2];
                for (int p = 0; p < 2; ++p) {
                    float& q = m.pos[p];
                    if (q < target[p]) q = std::min(target[p], q + step);
                    else if (q > target[p]) q = std::max(target[p], q - step);
                    // Equal power, sin(pi/2 x s) with s a raised cosine of the
                    // position: no step in the slope at either end, where a
                    // plain sin(pi/2 x q) starts or stops with one (a tick).
                    const float sq = 0.5f - 0.5f * std::cos(kPi * q);
                    g[p] = q >= 1.0f ? 1.0f : (q <= 0.0f ? 0.0f : std::sin(kHalfPi * sq));
                }
                if (m.rho > 0 && g[0] > 0 && g[0] < 1 && g[1] > 0 && g[1] < 1) {
                    const float e = g[0] * g[0] + g[1] * g[1];
                    const float k = std::sqrt(e / (e + 2.0f * m.rho * g[0] * g[1]));
                    g[0] *= k;
                    g[1] *= k;
                }
                if (!audible) continue;
                const int f0 = (m.w + i - m.delay[0]) & R, f1 = (m.w + i - m.delay[1]) & R;
                const float s1 = g[0] * m.ring[0][2 * f0] + g[1] * m.ring[1][2 * f1];
                const float s2 = g[0] * m.ring[0][2 * f0 + 1] + g[1] * m.ring[1][2 * f1 + 1];
                float* o0 = out[idx] + done;
                if (mono) {
                    const float v = clip1((s1 + s2) * 0.5f * vol);
                    o0[i] += v;
                    pk0 = std::max(pk0, std::fabs(v));
                } else {
                    const float lv = clip1(s1 * vol * gl), rv = clip1(s2 * vol * gr);
                    o0[i] += lv;
                    out[idx + 1][done + i] += rv;
                    pk0 = std::max(pk0, std::fabs(lv));
                    pk1 = std::max(pk1, std::fabs(rv));
                }
            }
            m.w = (m.w + n) & R;
            if (m.pos[0] <= 0) m.delay[0] = 0;   // silent paths can move without a sound
            if (m.pos[1] <= 0) m.delay[1] = 0;
            if (m.pos[0] <= 0 || m.pos[1] <= 0) m.rho = 0;
            done += n;
        }
        if (!m.path_live && m.pos[1] <= 0) u.live_slot = -1;   // faded out: no longer this user's to play
        u.live_now.store(m.path_live && u.live_slot >= 0);
        if (audible) {
            raisePeak(rc.peak[0], pk0);
            raisePeak(rc.peak[1], mono ? pk0 : pk1);
        }
    }
}

}  // namespace engine
}  // namespace o11proto
