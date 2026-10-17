// o11proto - Live mode for the whole room: CLIENT_LIVE_REPORT (0x85),
// SERVER_LIVE_ROOM (0x08) and the server's rule (live_room.h).
#include "o11proto/live_room.h"

#include <map>

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
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.u8(0);
    w.str(m.user);
    return w.data();
}

bool decode(const std::vector<std::uint8_t>& p, LiveRoom& out) {
    if (p.size() < kLiveRoomFixed || p[0] == 0) return false;
    out = LiveRoom();
    out.version = p[0];
    out.live = p[1] == 1;
    out.reason = p[2] <= 3 ? static_cast<RoomReason>(p[2]) : RoomReason::None;
    std::size_t end = kLiveRoomFixed;
    while (end < p.size() && p[end] != 0) ++end;
    out.user.assign(reinterpret_cast<const char*>(p.data() + kLiveRoomFixed), end - kLiveRoomFixed);
    return true;
}

// ------------------------------------------------------------ the rule

const RoomRule::State& RoomRule::update(double now, bool room_on, const std::vector<Player>& players) {
    while (!fallbacks_.empty() && now - fallbacks_.front() > kRepeatWindowSeconds) fallbacks_.erase(fallbacks_.begin());
    if (!room_on) {
        state_ = State();
        ok_since_ = -1;
        return state_;
    }
    bool blocked = false;
    State block;
    auto blockBy = [&](RoomReason r, std::uint32_t id) {
        blocked = true;
        block.reason = r;
        block.user = id;
    };
    auto find = [&](std::uint32_t id) -> const Player* {
        for (const auto& p : players)
            if (p.id == id) return &p;
        return nullptr;
    };
    // 1. Someone who can't do Live: no Live client, no place among the
    // senders, its own Live can't, or still not bound or silent after the
    // grace.
    for (const auto& p : players) {
        if (p.listener) continue;
        const bool late = now - p.joined >= kJoinGraceSeconds;
        if (!p.live_client || !p.sender || (p.has_report && p.report.cantLive()) ||
            (late && (!p.was_bound || !p.has_report))) {
            blockBy(RoomReason::CantLive, p.id);
            break;
        }
    }
    // 2. A binding that was lost.
    for (const auto& p : players) {
        if (blocked) break;
        if (!p.listener && p.was_bound && !p.bound) blockBy(RoomReason::Dropouts, p.id);
    }
    // 3. Someone still joining: within the grace, and not yet bound, or
    // not yet clean once.
    for (const auto& p : players) {
        if (blocked) break;
        if (!p.listener && now - p.joined < kJoinGraceSeconds && (!p.was_bound || !p.has_report || !p.was_clean))
            blockBy(RoomReason::Joining, p.id);
    }
    // 4. Reports that don't sound clean: the user they name most often,
    // the earliest in the room on a tie. A player who doesn't send is no
    // one's fault but the reporter's own.
    if (!blocked) {
        std::map<std::uint32_t, int> named;
        for (const auto& p : players) {
            if (p.listener || !p.has_report || p.report.clean()) continue;
            const std::uint32_t f = p.report.fault ? p.report.fault : p.id;
            const Player* q = find(f);
            if (!q || q->listener) continue;           // someone who left
            if (q->id != p.id && !(q->has_report && q->report.sending())) continue;
            ++named[f];
        }
        int most = 0;
        for (const auto& p : players) {
            auto it = named.find(p.id);
            if (it != named.end() && it->second > most) {
                most = it->second;
                block.user = p.id;
            }
        }
        if (most > 0) blockBy(RoomReason::Dropouts, block.user);
    }

    if (blocked) {
        ok_since_ = -1;
        if (state_.live && block.reason == RoomReason::Dropouts) fallbacks_.push_back(now);
        state_.live = false;
        state_.reason = block.reason;
        state_.user = block.user;
        return state_;
    }
    if (ok_since_ < 0) ok_since_ = now;
    if (!state_.live && state_.user && !find(state_.user)) {   // the reason user left: no one's to name
        state_.reason = RoomReason::None;
        state_.user = 0;
    }
    if (!state_.live) {
        const double settle = fallbacks_.size() >= 2 ? kRepeatSettleSeconds : kSettleSeconds;
        if (now - ok_since_ >= settle) state_ = State{true, RoomReason::None, 0};
    }
    return state_;
}

}  // namespace live
}  // namespace o11proto
