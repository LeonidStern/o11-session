// o11proto engine - Live mode inside the engine (ENGINE.md section 16):
// the Live client's life, switching each user between Live and intervals,
// mixing the slots into channel 0's outputs, and the /live texts. The state
// and the network side are in live_layer.cpp. Section numbers are the
// live-mode spec's.
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

constexpr auto kSettle = std::chrono::seconds(3);
constexpr auto kSettleWindow = std::chrono::seconds(60);
constexpr auto kGrace = std::chrono::milliseconds(2500);

}  // namespace

void Engine::LiveDeleter::operator()(LiveLayer* p) const { delete p; }

void Engine::liveInit() { live_.reset(new LiveLayer); }

void Engine::liveNotice(const std::string& text, bool error) {
    if (onLiveNotice) onLiveNotice(text, error);
}

// ------------------------------------------------------- control context

void Engine::liveMessage(const live::ServerLive& m) { live_->apply(m); }

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

bool Engine::userLive(int user) const {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = userAt(user);
    return u && u->live_now.load();
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
        L.grace_until = now + kGrace;
        for (int s = 0; s < kLiveSlots; ++s) {
            const SinkStats& st = L.sinks[s]->stats;
            L.changed_gen[s] = st.gen.load();
            L.changed_drops[s] = st.underruns.load() + st.overruns.load() + st.lost.load();
        }
    }

    // 6.4 and 6.5: health, then what each user should hear.
    L.health(now);
    const bool room = L.room_on.load() && L.running.load();
    std::vector<std::pair<std::string, bool>> changes;   // the mixer's switches since the last pass
    std::vector<int> change_slots;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (auto& up : users_) {
            RemoteUser& u = *up;
            LiveLayer::UserState& st = L.users[u.name];
            const int slot = room ? L.slotOf(u.name) : -1;
            const bool healthy = slot >= 0 && L.slot_health[slot].healthy;
            if (healthy && !st.healthy) st.healthy_since = now;
            st.healthy = healthy;
            const bool settling = st.fell_back && now - st.fell_back_at < kSettleWindow;
            bool want = healthy && now - st.healthy_since >= (settling ? std::chrono::duration_cast<Clock::duration>(kSettle)
                                                                         : Clock::duration::zero());
            if (!want && st.want && slot >= 0 && st.want_slot == slot && now < L.grace_until) want = true;
            if (st.want && !want && L.room_on.load()) {
                st.fell_back = true;
                st.fell_back_at = now;
            }
            st.want = want;
            st.want_slot = want ? slot : -1;
            u.live_want = want ? slot : -1;
            u.live_want_gen = want ? L.gen[slot].load() : 0;
            const bool live_now = u.live_now.load();
            if (live_now != st.live_now) {
                st.live_now = live_now;
                changes.emplace_back(u.name, live_now);
                change_slots.push_back(slot);
            }
        }
        if (L.users.size() > users_.size() + 16) {   // forget users who left
            for (auto it = L.users.begin(); it != L.users.end();) {
                bool present = false;
                for (auto& up : users_) present |= up->name == it->first;
                it = present ? std::next(it) : L.users.erase(it);
            }
        }
    }
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const std::string who = liveDisplayName(changes[i].first);
        if (changes[i].second) {
            const double ms = change_slots[i] >= 0 ? liveOneWayMs(change_slots[i], nullptr, nullptr, nullptr) : -1;
            liveNotice("You hear " + who + " live now" + (ms >= 0 ? ", about " + num(ms) + " ms late." : "."), false);
        } else if (L.room_on.load()) {
            liveNotice(who + " is back on intervals: their real-time audio stopped or had dropouts.", false);
        }
    }

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
    liveNotice(head, false);

    // One line per listed peer.
    struct Listed {
        std::string name;
        int slot;
    };
    std::vector<Listed> listed;
    {
        std::lock_guard<std::mutex> lock(L.net_mutex);
        for (const auto& p : L.peers) listed.push_back({p.name, p.slot});
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
            line += "intervals (no real-time audio from them)";
        } else {
            bool live_now = false;
            {
                std::lock_guard<std::mutex> lock(remote_mutex_);
                for (auto& up : users_)
                    if (up->name == p.name) live_now = up->live_now.load();
            }
            line += live_now ? "live"
                             : L.slot_health[p.slot].healthy ? "live from the next interval"
                                                             : "intervals (their real-time audio has dropouts)";
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
    // 6.5, "at once": a slot that is no longer the user's stops its Live.
    const bool on = L.room_on.load() && L.running.load();
    for (auto& u : users_) {
        if (u->live_slot < 0) continue;
        if (!on || L.gen[u->live_slot].load() != u->live_gen) {
            u->live_slot = -1;
            u->live_now.store(false);
        }
    }
}

// 6.5: the mixer takes the desired state at a boundary, before 8.2's steps.
void Engine::liveBoundary() {
    LiveLayer& L = *live_;
    const bool on = L.room_on.load() && L.running.load();
    for (auto& u : users_) {
        const int want = u->live_want;
        if (on && want >= 0 && L.gen[want].load() == u->live_want_gen) {
            u->live_slot = want;
            u->live_gen = u->live_want_gen;
        } else {
            u->live_slot = -1;
        }
        u->live_now.store(u->live_slot >= 0 && u->ch[0].present);
    }
}

bool Engine::liveCovers(const RemoteUser& u) const {
    return u.live_slot >= 0 && u.ch[0].present && live_->available(u.live_slot);
}

// 6.6: the slot's stereo audio with channel 0's settings, into channel 0's pair.
void Engine::mixLive(float* const* out, int numOut, int len, int at, int sampleRate) {
    (void)sampleRate;
    if (numOut == 0) return;
    const bool any_solo = any_solo_.load();
    const int offset = remote_offset_.load();
    for (auto& up : users_) {
        RemoteUser& u = *up;
        if (!liveCovers(u)) continue;
        RemoteChannel& rc = u.ch[0];
        const float vol = std::min(4.0f, std::max(0.0f, u.volume * rc.volume));
        const float pan = std::min(1.0f, std::max(-1.0f, u.pan + rc.pan));
        const bool muted = any_solo ? !rc.solo : (rc.mute || u.mute);
        if (muted || vol <= 1e-7f) continue;
        const float* src = live_->out(u.live_slot) + 2 * static_cast<std::size_t>(at);
        int idx;
        bool mono;
        outputPair(rc.outChannel, offset, numOut, &idx, &mono);
        float gl, gr;
        panGains(pan, &gl, &gr);
        float pk0 = 0, pk1 = 0;
        for (int i = 0; i < len; ++i) {
            const float l = clip1(src[2 * i]), r = clip1(src[2 * i + 1]);
            pk0 = std::max(pk0, std::fabs(l * vol));
            pk1 = std::max(pk1, std::fabs(r * vol));
            if (mono) {
                out[idx][i] += clip1(l * vol * 0.5f * gl) + clip1(r * vol * 0.5f * gr);
            } else {
                out[idx][i] += clip1(l * vol * gl);
                out[idx + 1][i] += clip1(r * vol * gr);
            }
        }
        raisePeak(rc.peak[0], mono ? std::max(pk0, pk1) : pk0);
        raisePeak(rc.peak[1], mono ? std::max(pk0, pk1) : pk1);
    }
}

}  // namespace engine
}  // namespace o11proto
