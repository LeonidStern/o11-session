// o11proto - Live mode for the whole room: CLIENT_LIVE_REPORT (0x85),
// SERVER_LIVE_ROOM (0x08) and the server's advice (live_room.h).
#include "o11proto/live_room.h"

#include <algorithm>

#include "o11proto/live.h"
#include "o11proto/message.h"

namespace o11proto {
namespace live {

// ----------------------------------------------------------------- 0x85

std::vector<std::uint8_t> encode(const LiveReport& r) {
    Writer w;
    w.u8(kLiveReportVersion);
    w.u8(r.flags);
    w.u16(0);
    w.u32(r.fault);
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, LiveReport& out) {
    if (p.size() < kLiveReportSize || p[0] == 0) return false;
    out = LiveReport();
    out.version = p[0];
    out.flags = p[1];
    out.fault = getLE32(p.data() + 4);
    return true;
}

// ----------------------------------------------------------------- 0x08

std::vector<std::uint8_t> encode(const LiveRoom& m) {
    Writer w;
    w.u8(kLiveRoomVersion);
    w.u8(m.live ? 1 : 0);
    w.u8(static_cast<std::uint8_t>(m.advice));
    w.u8(0);
    w.str(m.user);
    w.u16(m.count);
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, LiveRoom& out) {
    if (p.size() < kLiveRoomFixed || p[0] == 0) return false;
    out = LiveRoom();
    out.version = p[0];
    out.live = p[1] == 1;
    out.advice = p[2] <= 2 ? static_cast<Advice>(p[2]) : Advice::None;
    std::size_t end = kLiveRoomFixed;
    while (end < p.size() && p[end] != 0) ++end;
    out.user.assign(reinterpret_cast<const char*>(p.data() + kLiveRoomFixed), end - kLiveRoomFixed);
    out.count = out.advice != Advice::None ? 1 : 0;
    if (out.version >= 2 && end + 3 <= p.size()) out.count = getLE16(p.data() + end + 1);
    return true;
}

// ---------------------------------------------------------- the advice

const RoomAdvice::State& RoomAdvice::update(double now, bool live, const std::vector<Player>& players) {
    if (!live) {
        trouble_.clear();
        state_ = State();
        return state_;
    }
    auto index = [&](std::uint32_t id) {
        for (std::size_t i = 0; i < players.size(); ++i)
            if (players[i].id == id) return static_cast<int>(i);
        return -1;
    };
    auto joining = [&](const Player& p) {
        return now - p.joined < kJoinGraceSeconds && (!p.was_bound || !p.has_report || !p.was_clean);
    };
    // Each player's trouble now, and how bad it is.
    const int everyone = static_cast<int>(players.size());
    std::vector<Advice> what(players.size(), Advice::None);
    std::vector<int> worst(players.size(), 0);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const Player& p = players[i];
        if (p.listener) continue;
        const bool late = now - p.joined >= kJoinGraceSeconds;
        if (!p.live_client || !p.sender || (p.has_report && p.report.cantLive()) ||
            (late && (!p.was_bound || !p.has_report))) {
            what[i] = Advice::CantLive;
            worst[i] = everyone;
        } else if (p.was_bound && !p.bound) {
            what[i] = Advice::Dropouts;
            worst[i] = everyone;
        }
    }
    for (const Player& q : players) {
        if (q.listener || !q.has_report || q.report.clean() || joining(q)) continue;
        const int f = index(q.report.fault ? q.report.fault : q.id);
        if (f < 0) continue;                     // someone who left
        const Player& p = players[static_cast<std::size_t>(f)];
        if (p.listener || joining(p)) continue;
        if (p.id != q.id && !(p.has_report && p.report.sending())) continue;
        if (what[static_cast<std::size_t>(f)] == Advice::None) what[static_cast<std::size_t>(f)] = Advice::Dropouts;
        if (worst[static_cast<std::size_t>(f)] < everyone) ++worst[static_cast<std::size_t>(f)];
    }
    // Trouble begins at once and ends after kCleanSeconds without, or when
    // the player leaves.
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (what[i] == Advice::None) continue;
        auto it = std::find_if(trouble_.begin(), trouble_.end(), [&](const Trouble& t) { return t.id == players[i].id; });
        if (it == trouble_.end()) it = trouble_.insert(trouble_.end(), Trouble{players[i].id, what[i], -1});
        it->advice = what[i];
        it->clean_since = -1;
    }
    for (auto it = trouble_.begin(); it != trouble_.end();) {
        const int i = index(it->id);
        if (i >= 0 && what[static_cast<std::size_t>(i)] != Advice::None) {
            ++it;
            continue;
        }
        if (it->clean_since < 0) it->clean_since = now;
        if (i < 0 || now - it->clean_since >= kCleanSeconds) it = trouble_.erase(it);
        else ++it;
    }
    // The one to name: the worst trouble now, else the first in the room of
    // those still to clear.
    state_ = State();
    int best = -1, best_worst = -1;
    for (std::size_t i = 0; i < players.size(); ++i) {
        auto it = std::find_if(trouble_.begin(), trouble_.end(), [&](const Trouble& t) { return t.id == players[i].id; });
        if (it == trouble_.end()) continue;
        const int w = it->clean_since < 0 ? worst[i] : -1;
        if (best < 0 || w > best_worst) {
            best = static_cast<int>(i);
            best_worst = w;
            state_.advice = it->advice;
            state_.user = it->id;
        }
    }
    state_.count = static_cast<int>(trouble_.size());
    return state_;
}

}  // namespace live
}  // namespace o11proto
