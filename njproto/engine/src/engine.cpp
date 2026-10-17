// njproto engine - connection, upload loop, downloads, clock and mixing.
// Section numbers refer to ENGINE.md.
#include "njproto/engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace njproto {
namespace engine {

namespace {
constexpr int kDownloadTimeoutSeconds = 8;

float clip1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }
}  // namespace

// ------------------------------------------------------ PlaybackResampler

int PlaybackResampler::needed(int srcRate, int dstRate, int len) const {
    if (srcRate == dstRate) return len;
    long num = static_cast<long>(srcRate) * len + phase_num;
    return static_cast<int>(num / dstRate);
}

void PlaybackResampler::advance(int srcRate, int dstRate, int len, int consumed) {
    if (srcRate == dstRate) {
        reset();
        return;
    }
    long num = static_cast<long>(srcRate) * len + phase_num - static_cast<long>(consumed) * dstRate;
    phase_num = num;
    phase = static_cast<double>(phase_num) / dstRate;
}

// ------------------------------------------------------------- lifecycle

Engine::Engine() { wireClient(); }

Engine::~Engine() {
    suppress_ = true;
    client_.disconnect("engine destroyed");
}

void Engine::decayPeak(std::atomic<float>& p, float factor) { p.store(p.load() * factor); }
void Engine::raisePeak(std::atomic<float>& p, float v) {
    if (v > p.load()) p.store(v);
}

// ------------------------------------------------------------ connection

void Engine::wireClient() {
    client_.on_message = [this](const RawMessage&) { ++messages_; };
    client_.on_challenge = [this](const AuthChallenge&) { got_challenge_ = true; };
    client_.on_licence_decision = [this](const std::string& text) {
        return onLicence ? onLicence(text) : LicenceAnswer::Agree;
    };
    client_.on_auth_reply = [this](const AuthReply& r) {
        if (!r.success()) return;                 // the disconnect that follows sets the status
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            username_ = r.text;
        }
        maxchan_.store(r.maxchan);
        logged_in_ = true;
        client_.setChannels(channelList());       // the client sends it right after this callback
        status_.store(Status::Ok);
    };
    client_.on_config = [this](const ConfigChange& c) {
        server_bpm_.store(c.bpm);
        server_bpi_.store(c.bpi);
        tempo_pending_.store(true);
        if (!got_config_) {
            got_config_ = true;
            run_request_.store(true);             // running starts at the next process()
        }
    };
    client_.on_codec_mask = [this](const CodecMask& m) {
        codec_mask_.store(m.codecs);
        room_kbps_.store(static_cast<int>(m.opus_kbps));
        has_mask_.store(true);
    };
    client_.on_userinfo = [this](const UserInfoRecord& rec) { handleUserInfo(rec); };
    client_.on_download_begin = [this](const DownloadBegin& d) { handleDownloadBegin(d); };
    client_.on_download_data = [this](const Guid& g, const std::vector<std::uint8_t>& b) {
        handleDownloadData(g, b, false);
    };
    client_.on_download_end = [this](const Guid& g, bool) { handleDownloadData(g, {}, true); };
    client_.on_chat = [this](const ChatMessage& m) {
        if (m.fields[0] == "SESSION") return;
        if (onChat) onChat(m.fields);
    };
    client_.on_disconnect = [this](const std::string& reason) {
        if (suppress_) return;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            error_text_ = reason;
        }
        if (logged_in_) status_.store(Status::Disconnected);
        else if (got_challenge_) status_.store(Status::InvalidAuth);
        else status_.store(Status::CantConnect);
        run_request_.store(false);
    };
}

void Engine::connect(const std::string& hostport, const std::string& user, const std::string& password) {
    std::string host = hostport;
    std::uint16_t port = kDefaultPort;
    std::size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon) {   // not an IPv6 literal
        host = hostport.substr(0, colon);
        port = static_cast<std::uint16_t>(std::atoi(hostport.c_str() + colon + 1));
    }
    connect(host, port, user, password);
}

void Engine::connect(const std::string& host, std::uint16_t port, const std::string& user, const std::string& password) {
    disconnect();
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        host_name_ = host;
        error_text_.clear();
        username_.clear();
    }
    Client::Options o;
    o.username = user;
    o.password = password;
    o.codec_caps = kCapOpus | kCapFlac;
    o.client_version = kProtocolVersion;
    o.max_server_version = 0x00030000;
    o.login_when_licence_refused = true;
    o.auto_subscribe = false;
    client_.setChannels(channelList());
    if (!client_.connect(host, port, o)) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_text_ = "cannot connect";
        status_.store(Status::CantConnect);
    }
}

void Engine::resetConnectionState() {
    got_challenge_ = logged_in_ = got_config_ = false;
    maxchan_.store(0);
    codec_mask_.store(0);
    has_mask_.store(false);
    room_kbps_.store(0);
    server_bpm_.store(0);
    server_bpi_.store(0);
    active_bpm_.store(120);
    active_bpi_.store(32);
    run_request_.store(false);
    running_.store(false);
    interval_started_.store(false);
    loop_count_.store(0);
    interval_pos_.store(0);
    downloads_.clear();
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (auto& u : users_)
            for (auto& c : u->ch) {
                if (c.current) garbage_.push_back(std::move(c.current));
                for (auto& s : c.slots)
                    if (s.dec) garbage_.push_back(std::move(s.dec));
            }
        users_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        for (auto& kv : locals_) {
            LocalState& ls = *kv.second;
            std::lock_guard<std::mutex> ql(ls.qm);
            ls.queue.clear();
            ls.transmitting = false;
            ls.has_interval = false;
            ls.begin_pending = false;
            ls.skipped = false;
        }
    }
    collectGarbage();
    recomputeAnySolo();
}

void Engine::disconnect() {
    suppress_ = true;
    client_.disconnect("disconnect");
    suppress_ = false;
    resetConnectionState();
    status_.store(Status::Preconnect);
}

void Engine::answerLicence(bool agree) { client_.answerLicence(agree); }

std::string Engine::errorText() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return error_text_;
}
std::string Engine::username() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return username_;
}
std::string Engine::hostName() const {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return host_name_;
}

bool Engine::run() {
    messages_ = 0;
    if (client_.state() != Client::State::Disconnected && client_.state() != Client::State::Failed)
        client_.poll(0);
    checkDownloadTimeouts();
    bool uploaded = uploadLoop();
    collectGarbage();
    return messages_ == 0 && !uploaded;
}

void Engine::collectGarbage() {
    std::vector<std::unique_ptr<Decoder>> g;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        g.swap(garbage_);
    }
    g.clear();
}

// ------------------------------------------------------- local channels

void Engine::setLocalChannel(int index, const LocalChannel& settings) {
    if (index < 0 || index >= kMaxChannels) return;
    bool announce = false;
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        auto it = locals_.find(index);
        if (it == locals_.end()) {
            it = locals_.emplace(index, std::make_unique<LocalState>()).first;
            announce = true;
        } else if (it->second->s.name != settings.name) {
            announce = true;
        }
        it->second->s = settings;
    }
    recomputeAnySolo();
    if (announce) sendChannelList();
}

bool Engine::localChannel(int index, LocalChannel* out) const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    auto it = locals_.find(index);
    if (it == locals_.end()) return false;
    if (out) *out = it->second->s;
    return true;
}

void Engine::removeLocalChannel(int index) {
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        if (!locals_.erase(index)) return;
    }
    recomputeAnySolo();
    sendChannelList();
}

std::vector<int> Engine::localChannels() const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    std::vector<int> v;
    for (const auto& kv : locals_) v.push_back(kv.first);
    return v;
}

float Engine::localPeak(int index, int side) const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    auto it = locals_.find(index);
    return it == locals_.end() ? 0.0f : it->second->peak[side & 1].load();
}

// Records 0..highest announced index (5.2); indices >= maxchan are not announced.
std::vector<ChannelInfoRecord> Engine::channelList() const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    int limit = maxchan_.load() > 0 ? maxchan_.load() : kMaxChannels;
    int highest = -1;
    for (const auto& kv : locals_)
        if (kv.first < limit) highest = std::max(highest, kv.first);
    std::vector<ChannelInfoRecord> recs;
    for (int i = 0; i <= highest; ++i) {
        ChannelInfoRecord r;
        auto it = locals_.find(i);
        if (it == locals_.end()) r.flags = kChanInactive;
        else r.name = it->second->s.name;
        recs.push_back(r);
    }
    return recs;
}

void Engine::sendChannelList() { client_.setChannels(channelList()); }
void Engine::notifyServerOfChannelChange() { sendChannelList(); }

std::uint32_t Engine::uploadCodec() const {
    if (!has_mask_.load()) return 0;
    std::uint32_t m = codec_mask_.load();
    if ((m & kCapFlac) && prefer_flac_.load()) return kFourccFlac;
    if (m & kCapOpus) return kFourccOpus;
    return 0;
}

int Engine::uploadKbps(int index) const {
    std::lock_guard<std::mutex> lock(local_mutex_);
    auto it = locals_.find(index);
    if (it == locals_.end()) return 0;
    int used = it->second->kbps_in_use.load();
    if (used) return used;
    return room_kbps_.load() ? room_kbps_.load() : it->second->s.bitrate;
}

// --------------------------------------------------------- upload loop

bool Engine::uploadLoop() {
    bool any = false;
    std::vector<UploadBlock> items;
    std::vector<std::pair<int, LocalState*>> chans;
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        for (auto& kv : locals_) chans.emplace_back(kv.first, kv.second.get());
    }
    // Encoding happens without local_mutex_ held; a channel removed meanwhile
    // is only destroyed by removeLocalChannel in this same (control) context.
    for (auto& [index, ls] : chans) {
        items.clear();
        {
            std::lock_guard<std::mutex> ql(ls->qm);
            items.swap(ls->queue);
        }
        if (items.empty()) continue;
        any = true;
        for (auto& b : items) processUploadItem(*ls, index, b);
        std::lock_guard<std::mutex> ql(ls->qm);
        for (auto& b : items)
            if (!b.data.empty() && ls->spare.size() < kUploadQueueLimit) ls->spare.push_back(std::move(b.data));
    }
    return any;
}

void Engine::processUploadItem(LocalState& ls, int index, UploadBlock& b) {
    if (index >= maxchan_.load()) return;
    if (!client_.loggedIn()) return;
    switch (b.kind) {
    case UploadBlock::Audio: {
        if (!ls.has_interval) {
            ls.has_interval = true;
            std::uint32_t fourcc = uploadCodec();
            if (fourcc == 0) {
                ls.skipped = true;                // no codec: the whole interval is dropped
                return;
            }
            ls.skipped = false;
            int kbps = room_kbps_.load() ? room_kbps_.load() : ls.s.bitrate;
            if (!ls.enc || ls.enc->fourcc() != fourcc || ls.enc->rate() != b.rate ||
                ls.enc->channels() != b.channels || ls.enc->kbps() != kbps)
                ls.enc = makeEncoder(fourcc, b.rate, b.channels, kbps);
            if (!ls.enc) {
                ls.skipped = true;
                return;
            }
            ls.kbps_in_use.store(kbps);
            ls.enc->begin();
            ls.guid = Client::randomGuid();
            ls.fourcc = fourcc;
            ls.begin_pending = true;
        }
        if (ls.skipped) return;
        ls.enc->encode(b.data.data(), static_cast<int>(b.data.size() / static_cast<std::size_t>(b.channels)));
        sendWrites(ls, index, false);
        break;
    }
    case UploadBlock::End:
        if (!ls.has_interval) return;
        ls.has_interval = false;
        if (ls.skipped) return;
        ls.enc->finish();
        sendWrites(ls, index, true);
        break;
    case UploadBlock::Silence:
        client_.sendSilence(static_cast<std::uint8_t>(index));
        break;
    }
}

// Sends WRITEs while enough output is ready (6.1 steps 2 and 3).
void Engine::sendWrites(LocalState& ls, int index, bool final) {
    auto sendBegin = [&] {
        if (!ls.begin_pending) return;
        UploadBegin ub;
        ub.guid = ls.guid;
        ub.fourcc = ls.fourcc;
        ub.channel = static_cast<std::uint8_t>(index);
        client_.sendRaw(kMsgClientUploadBegin, encode(ub));
        ls.begin_pending = false;
    };
    for (;;) {
        std::size_t size = ls.enc->outputSize();
        std::size_t threshold = ls.begin_pending ? kFirstWriteThreshold : kLaterWriteThreshold;
        if (!final && size < threshold) return;
        if (final && size == 0) break;
        std::size_t n = std::min<std::size_t>(size, kMaxWriteChunk);
        sendBegin();
        IntervalWrite w;
        w.guid = ls.guid;
        w.data.assign(ls.enc->outputData(), ls.enc->outputData() + n);
        ls.enc->consume(n);
        w.flags = (final && ls.enc->outputSize() == 0) ? 1 : 0;
        client_.sendRaw(kMsgClientUploadWrite, encode(w));
        if (w.flags) return;
    }
    // final with nothing left: BEGIN (if still pending) and an empty end WRITE
    sendBegin();
    IntervalWrite w;
    w.guid = ls.guid;
    w.flags = 1;
    client_.sendRaw(kMsgClientUploadWrite, encode(w));
}

// ------------------------------------------------------- remote users

Engine::RemoteUser* Engine::findUser(const std::string& name) {
    for (auto& u : users_)
        if (u->name == name) return u.get();
    return nullptr;
}

Engine::RemoteUser* Engine::userAt(int i) const {
    if (i < 0 || static_cast<std::size_t>(i) >= users_.size()) return nullptr;
    return users_[static_cast<std::size_t>(i)].get();
}

void Engine::discardChannelIntervals(RemoteChannel& rc) {
    if (rc.current) garbage_.push_back(std::move(rc.current));
    rc.has_current = false;
    for (auto& s : rc.slots) {
        if (s.dec) garbage_.push_back(std::move(s.dec));
        s.used = s.silent = false;
    }
    rc.fade.clear();
    rc.drop_count = 0;
}

void Engine::sendUserMask(RemoteUser& u) { client_.subscribe(u.name, u.submask); }

void Engine::handleUserInfo(const UserInfoRecord& rec) {
    if (rec.index >= kMaxChannels) return;
    bool send_mask = false;
    std::string name;
    std::uint32_t mask = 0;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        RemoteUser* u = findUser(rec.username);
        if (rec.active) {
            if (!u) {
                users_.push_back(std::make_unique<RemoteUser>());
                u = users_.back().get();
                u->name = rec.username;
            }
            RemoteChannel& rc = u->ch[rec.index];
            bool was_present = rc.present;
            std::uint8_t old_flags = rc.flags;
            rc.present = true;
            rc.name = rec.channel;
            rc.flags = rec.flags;
            if (was_present && ((old_flags ^ rec.flags) & (kChanVoice | kChanSession))) discardChannelIntervals(rc);
            if (auto_subscribe_.load()) {
                u->submask |= 1u << rec.index;
                rc.subscribed = true;
                send_mask = true;
            }
            if (!was_present && !rc.has_pair) assignOutputPair(*u, rc);
            name = u->name;
            mask = u->submask;
        } else if (u) {
            RemoteChannel& rc = u->ch[rec.index];
            rc.name.clear();
            rc.present = false;
            rc.subscribed = false;
            rc.solo = false;
            u->submask &= ~(1u << rec.index);
            discardChannelIntervals(rc);
            bool any = false;
            for (const auto& c : u->ch) any = any || c.present;
            if (!any) {
                for (auto it = users_.begin(); it != users_.end(); ++it)
                    if (it->get() == u) {
                        users_.erase(it);
                        break;
                    }
            }
        }
    }
    recomputeAnySolo();
    if (send_mask) client_.subscribe(name, mask);
    users_changed_.store(true);
    if (onUsersChanged) onUsersChanged();
}

void Engine::recomputeAnySolo() {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        for (const auto& kv : locals_) any = any || kv.second->s.solo;
    }
    if (!any) {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        for (const auto& u : users_)
            for (const auto& c : u->ch) any = any || (c.present && c.solo);
    }
    any_solo_.store(any);
}

// Automatic output pair (8.4); remote_mutex_ held.
void Engine::assignOutputPair(RemoteUser& u, RemoteChannel& rc) {
    if (auto_pairs_mode_ == 0 || auto_pairs_outs_ < 4) return;
    if (auto_pairs_mode_ == 2) {
        for (const auto& c : u.ch)
            if (&c != &rc && c.has_pair) {
                rc.outChannel = c.outChannel;
                rc.has_pair = true;
                return;
            }
    }
    int best = -1, best_use = 0;
    for (int p = 2; p + 2 <= auto_pairs_outs_; p += 2) {
        int use = 0;
        for (const auto& ou : users_)
            for (const auto& c : ou->ch)
                if (c.has_pair && c.outChannel == p) use += c.present ? 16 : 1;
        if (use == 0) {
            best = p;
            break;
        }
        if (best < 0 || use < best_use) {
            best = p;
            best_use = use;
        }
    }
    if (best >= 0) {
        rc.outChannel = best;
        rc.has_pair = true;
    }
}

void Engine::autoOutputPairs(int mode, int numOut) {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    auto_pairs_mode_ = mode;
    auto_pairs_outs_ = numOut;
}

bool Engine::hasUserInfoChanged() { return users_changed_.exchange(false); }

int Engine::numUsers() const {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    return static_cast<int>(users_.size());
}

#define NJ_USER_GET(type, name, field, def)                                \
    type Engine::name(int user) const {                                    \
        std::lock_guard<std::mutex> lock(remote_mutex_);                   \
        RemoteUser* u = userAt(user);                                      \
        return u ? u->field : (def);                                       \
    }
NJ_USER_GET(std::string, userName, name, std::string())
NJ_USER_GET(float, userVolume, volume, 0.0f)
NJ_USER_GET(float, userPan, pan, 0.0f)
NJ_USER_GET(bool, userMute, mute, false)
#undef NJ_USER_GET

#define NJ_USER_SET(name, field, type)                                     \
    void Engine::name(int user, type v) {                                  \
        std::lock_guard<std::mutex> lock(remote_mutex_);                   \
        if (RemoteUser* u = userAt(user)) u->field = v;                    \
    }
NJ_USER_SET(setUserVolume, volume, float)
NJ_USER_SET(setUserPan, pan, float)
NJ_USER_SET(setUserMute, mute, bool)
#undef NJ_USER_SET

int Engine::channelAt(int user, int i) const {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = userAt(user);
    if (!u) return -1;
    int n = 0;
    for (int c = 0; c < kMaxChannels; ++c)
        if (u->ch[static_cast<std::size_t>(c)].present && n++ == i) return c;
    return -1;
}

#define NJ_CHAN_GET(type, name, expr, def)                                 \
    type Engine::name(int user, int ch) const {                            \
        std::lock_guard<std::mutex> lock(remote_mutex_);                   \
        RemoteUser* u = userAt(user);                                      \
        if (!u || ch < 0 || ch >= kMaxChannels) return (def);              \
        const RemoteChannel& c = u->ch[static_cast<std::size_t>(ch)];      \
        return expr;                                                       \
    }
NJ_CHAN_GET(std::string, channelName, c.name, std::string())
NJ_CHAN_GET(bool, channelSubscribed, c.subscribed, false)
NJ_CHAN_GET(float, channelVolume, c.volume, 0.0f)
NJ_CHAN_GET(float, channelPan, c.pan, 0.0f)
NJ_CHAN_GET(bool, channelMute, c.mute, false)
NJ_CHAN_GET(bool, channelSolo, c.solo, false)
NJ_CHAN_GET(int, channelOutChannel, c.outChannel, 0)
NJ_CHAN_GET(int, channelFlags, c.flags, 0)
#undef NJ_CHAN_GET

float Engine::channelPeak(int user, int ch, int side) const {
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = userAt(user);
    if (!u || ch < 0 || ch >= kMaxChannels) return 0.0f;
    return u->ch[static_cast<std::size_t>(ch)].peak[side & 1].load();
}

#define NJ_CHAN_SET(name, field, type)                                     \
    void Engine::name(int user, int ch, type v) {                          \
        std::lock_guard<std::mutex> lock(remote_mutex_);                   \
        RemoteUser* u = userAt(user);                                      \
        if (u && ch >= 0 && ch < kMaxChannels) u->ch[static_cast<std::size_t>(ch)].field = v; \
    }
NJ_CHAN_SET(setChannelVolume, volume, float)
NJ_CHAN_SET(setChannelPan, pan, float)
NJ_CHAN_SET(setChannelMute, mute, bool)
NJ_CHAN_SET(setChannelOutChannel, outChannel, int)
#undef NJ_CHAN_SET

void Engine::setChannelSolo(int user, int ch, bool v) {
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        RemoteUser* u = userAt(user);
        if (u && ch >= 0 && ch < kMaxChannels) u->ch[static_cast<std::size_t>(ch)].solo = v;
    }
    recomputeAnySolo();
}

void Engine::setChannelSubscribed(int user, int ch, bool v) {
    std::string name;
    std::uint32_t mask = 0;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        RemoteUser* u = userAt(user);
        if (!u || ch < 0 || ch >= kMaxChannels) return;
        RemoteChannel& rc = u->ch[static_cast<std::size_t>(ch)];
        rc.subscribed = v;
        if (v) u->submask |= 1u << ch;
        else {
            u->submask &= ~(1u << ch);
            discardChannelIntervals(rc);
        }
        name = u->name;
        mask = u->submask;
    }
    client_.subscribe(name, mask);
}

// ------------------------------------------------------------ downloads

void Engine::queueInterval(RemoteChannel& rc, Slot&& slot) {
    Slot& target = rc.slots[0].used ? rc.slots[1] : rc.slots[0];
    if (target.dec) garbage_.push_back(std::move(target.dec));
    target = std::move(slot);
    target.used = true;
}

void Engine::handleDownloadBegin(const DownloadBegin& d) {
    if (d.channel >= kMaxChannels) return;
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = findUser(d.username);
    if (!u) return;
    RemoteChannel& rc = u->ch[d.channel];
    if (guidIsZero(d.guid)) {
        if (rc.ignoresIntervals()) return;
        Slot s;
        s.silent = true;
        queueInterval(rc, std::move(s));
        return;
    }
    if (d.fourcc == 0) return;                    // legacy cache replay: ignored
    Download dl;
    dl.user = d.username;
    dl.channel = d.channel;
    dl.fourcc = d.fourcc;
    dl.stream = std::make_shared<ByteStream>();
    dl.last = Clock::now();
    downloads_[d.guid] = std::move(dl);
}

void Engine::handleDownloadData(const Guid& g, const std::vector<std::uint8_t>& bytes, bool last) {
    auto it = downloads_.find(g);
    if (it == downloads_.end()) return;
    Download& dl = it->second;
    if (!bytes.empty()) dl.stream->append(bytes);
    dl.last = Clock::now();
    if (last) dl.stream->setEnded();
    if (!dl.playable && (dl.stream->total() > static_cast<std::size_t>(kPrebufferBytes) || last)) makePlayable(g, dl);
    if (last) downloads_.erase(it);
}

// 7.3: create and prime the decoder, then queue it (control context).
void Engine::makePlayable(const Guid&, Download& dl) {
    dl.playable = true;
    // Create and prime the decoder before taking remote_mutex_, so the audio
    // context never waits on codec work.
    std::unique_ptr<Decoder> dec = makeDecoder(dl.fourcc, dl.stream);
    if (!dec) return;
    dec->decodeUntil(1);
    std::lock_guard<std::mutex> lock(remote_mutex_);
    RemoteUser* u = findUser(dl.user);
    if (!u) return;
    RemoteChannel& rc = u->ch[static_cast<std::size_t>(dl.channel)];
    if (rc.ignoresIntervals()) return;
    Slot s;
    s.dec = std::move(dec);
    queueInterval(rc, std::move(s));
}

void Engine::checkDownloadTimeouts() {
    TimePoint now = Clock::now();
    for (auto it = downloads_.begin(); it != downloads_.end();) {
        if (now - it->second.last > std::chrono::seconds(kDownloadTimeoutSeconds)) {
            it->second.stream->setEnded();        // a playable one plays what arrived
            it = downloads_.erase(it);
        } else {
            ++it;
        }
    }
}

// ------------------------------------------------------- master, metronome

void Engine::setMaster(float volume, float pan, bool mute) {
    master_vol_.store(volume);
    master_pan_.store(pan);
    master_mute_.store(mute);
}

void Engine::setMetronome(float volume, float pan, bool mute, int outChannel) {
    metro_vol_.store(volume);
    metro_pan_.store(pan);
    metro_mute_.store(mute);
    metro_out_.store(outChannel);
}

// ----------------------------------------------------------------- chat

void Engine::say(const std::string& text) { client_.say(text); }
void Engine::privateMessage(const std::string& user, const std::string& text) { client_.privmsg(user, text); }
void Engine::admin(const std::string& command) { client_.admin(command); }
void Engine::sendChat(const std::array<std::string, 5>& f) {
    client_.sendChat(ChatMessage(f[0], f[1], f[2], f[3], f[4]));
}

// ========================================================== audio context

void Engine::position(int* pos, int* len) const {
    if (pos) *pos = running_.load() ? interval_pos_.load() : 0;
    if (len) *len = interval_len_.load();
}

void Engine::setIntervalEnd(int samples) {
    if (!running_.load() || !interval_started_.load() || samples < 1) return;
    interval_len_.store(interval_pos_.load() + samples);
}

void Engine::outputPair(int outChannel, int offset, int numOut, int* idx, bool* mono) const {
    int i = (outChannel + offset) & 1023;
    bool m = (outChannel & kMonoOutput) != 0 || numOut < 2;
    int width = m ? 1 : 2;
    if (i + width > numOut) i = std::max(0, numOut - width);
    *idx = i;
    *mono = m;
}

void Engine::process(const float* const* in, int numIn, float* const* out, int numOut, int frames, int sampleRate) {
    sample_rate_.store(sampleRate);
    for (int c = 0; c < numOut; ++c) std::memset(out[c], 0, sizeof(float) * static_cast<std::size_t>(frames));

    std::lock_guard<std::mutex> rl(remote_mutex_);
    std::lock_guard<std::mutex> ll(local_mutex_);

    if (!run_request_.load() && running_.load()) {   // disconnected or lost
        running_.store(false);
        interval_started_.store(false);
        for (auto& kv : locals_) kv.second->transmitting = false;
    }
    if (run_request_.load() && !running_.load()) {
        running_.store(true);
        interval_started_.store(false);          // first boundary at the start of this block
        loop_count_.store(0);
    }

    if (!running_.load()) {
        mixLocal(in, numIn, out, numOut, frames, sampleRate);
        applyMaster(out, numOut, frames, sampleRate);
        return;
    }

    int pos = 0;
    while (pos < frames) {
        if (!interval_started_.load() || interval_pos_.load() >= interval_len_.load()) boundary(sampleRate);
        int piece = std::min(frames - pos, interval_len_.load() - interval_pos_.load());
        if (piece <= 0) piece = frames - pos;   // defensive, cannot happen with len >= 1
        // Offset views of the buffers for this piece.
        const float** inp = reinterpret_cast<const float**>(alloca(sizeof(float*) * static_cast<std::size_t>(std::max(numIn, 1))));
        float** outp = reinterpret_cast<float**>(alloca(sizeof(float*) * static_cast<std::size_t>(std::max(numOut, 1))));
        for (int c = 0; c < numIn; ++c) inp[c] = in[c] + pos;
        for (int c = 0; c < numOut; ++c) outp[c] = out[c] + pos;
        mixRemote(outp, numOut, piece, sampleRate);
        mixLocal(inp, numIn, outp, numOut, piece, sampleRate);
        mixMetronome(outp, numOut, piece, sampleRate);
        interval_pos_.store(interval_pos_.load() + piece);
        pos += piece;
    }
    applyMaster(out, numOut, frames, sampleRate);
}

void Engine::boundary(int sampleRate) {
    if (tempo_pending_.load() || !interval_started_.load()) {
        if (server_bpm_.load() > 0 && server_bpi_.load() > 0) {
            active_bpm_.store(server_bpm_.load());
            active_bpi_.store(server_bpi_.load());
        }
        tempo_pending_.store(false);
    }
    int len = intervalLengthFor(active_bpm_.load(), active_bpi_.load(), sampleRate);
    if (len < 1) len = 1;
    usual_len_.store(len);
    interval_len_.store(len);
    interval_pos_.store(0);
    interval_started_.store(true);
    loop_count_.store(loop_count_.load() + 1);
    beat_len_ = std::max(1, len / std::max(1, active_bpi_.load()));
    next_beat_ = 0;
    beat_index_ = 0;
    uploadBoundary();
    newIntervalStep();
}

// 6.3, local_mutex_ held.
void Engine::uploadBoundary() {
    int maxchan = maxchan_.load();
    for (auto& kv : locals_) {
        if (kv.first >= maxchan) continue;
        LocalState& ls = *kv.second;
        bool was = ls.transmitting;
        ls.transmitting = ls.s.broadcast;
        std::lock_guard<std::mutex> ql(ls.qm);
        if (was && ls.queue.size() < static_cast<std::size_t>(kUploadQueueLimit)) {
            UploadBlock b;
            b.kind = UploadBlock::End;
            ls.queue.push_back(std::move(b));
        }
        if (was && !ls.transmitting && ls.queue.size() < static_cast<std::size_t>(kUploadQueueLimit)) {
            UploadBlock b;
            b.kind = UploadBlock::Silence;
            ls.queue.push_back(std::move(b));
        }
    }
}

// 8.2, remote_mutex_ held.
void Engine::newIntervalStep() {
    for (auto& u : users_) {
        for (auto& rc : u->ch) {
            if (rc.ignoresIntervals()) continue;
            // 1. remember the fade-out
            rc.fade.clear();
            rc.fade_channels = 0;
            if (rc.current) {
                int ch = rc.current->channels();
                const auto& lap = rc.current->lapping();
                std::size_t want = static_cast<std::size_t>(kCrossfadeFrames) * static_cast<std::size_t>(ch);
                rc.fade.insert(rc.fade.end(), lap.begin(), lap.begin() + static_cast<std::ptrdiff_t>(std::min(lap.size(), want)));
                if (rc.fade.size() < want) {
                    std::size_t more = std::min((want - rc.fade.size()) / static_cast<std::size_t>(ch), rc.current->available());
                    const float* d = rc.current->data();
                    rc.fade.insert(rc.fade.end(), d, d + more * static_cast<std::size_t>(ch));
                }
                rc.fade_channels = ch;
            }
            // 2. drop the current interval
            if (rc.current) garbage_.push_back(std::move(rc.current));
            rc.has_current = false;
            // 3./4. advance the queue
            if (rc.slots[0].used) {
                if (rc.subscribed && rc.present) {
                    rc.has_current = true;
                    rc.current = std::move(rc.slots[0].dec);
                } else if (rc.slots[0].dec) {
                    garbage_.push_back(std::move(rc.slots[0].dec));
                }
            }
            rc.slots[0] = std::move(rc.slots[1]);
            rc.slots[1] = Slot();
            // 5. drop count
            rc.drop_count = 0;
            rc.rs.reset();
            // 6. fade-in
            if (rc.current && !rc.fade.empty() && rc.fade_channels == rc.current->channels()) {
                int ch = rc.current->channels();
                std::size_t n = rc.fade.size() / static_cast<std::size_t>(ch);
                rc.current->decodeUntil(n);
                std::size_t have = std::min(n, rc.current->available());
                float* d = const_cast<float*>(rc.current->data());
                for (std::size_t i = 0; i < have; ++i) {
                    float s = static_cast<float>(i + 1) / static_cast<float>(n);
                    for (int c = 0; c < ch; ++c) {
                        std::size_t k = i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c);
                        d[k] = d[k] * s + rc.fade[k] * (1.0f - s);
                    }
                }
            }
        }
    }
}

// 8.5, remote_mutex_ held.
void Engine::mixRemote(float* const* out, int numOut, int len, int sampleRate) {
    float decay = static_cast<float>(std::pow(0.015625, static_cast<double>(len) / sampleRate));
    bool any_solo = any_solo_.load();
    int offset = remote_offset_.load();
    for (auto& u : users_) {
        for (auto& rc : u->ch) {
            if (!rc.present) continue;
            decayPeak(rc.peak[0], decay);
            decayPeak(rc.peak[1], decay);
            if (rc.ignoresIntervals() || !rc.current) continue;
            float vol = std::min(4.0f, std::max(0.0f, u->volume * rc.volume));
            float pan = std::min(1.0f, std::max(-1.0f, u->pan + rc.pan));
            bool muted = any_solo ? !rc.solo : (rc.mute || u->mute);
            Decoder& dec = *rc.current;
            int srcRate = dec.rate();
            int needed = rc.rs.needed(srcRate, sampleRate, len);
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
                continue;
            }
            if (!muted && vol >= 1e-7f && numOut > 0) {
                int ch = dec.channels();
                float* src = const_cast<float*>(dec.data());
                std::size_t avail = dec.available();
                for (std::size_t i = 0; i < static_cast<std::size_t>(needed) * static_cast<std::size_t>(ch); ++i) src[i] = clip1(src[i]);
                int idx;
                bool mono;
                outputPair(rc.outChannel, offset, numOut, &idx, &mono);
                float gl, gr;
                panGains(pan, &gl, &gr);
                float pk0 = 0, pk1 = 0;
                bool same = srcRate == sampleRate;
                double ratio = static_cast<double>(srcRate) / sampleRate;
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
                    if (mono) {
                        float v = clip1((s1 + s2) * 0.5f * vol);
                        out[idx][i] += v;
                        pk0 = std::max(pk0, std::fabs(v));
                    } else {
                        float l = clip1(s1 * vol * gl), r = clip1(s2 * vol * gr);
                        out[idx][i] += l;
                        out[idx + 1][i] += r;
                        pk0 = std::max(pk0, std::fabs(l));
                        pk1 = std::max(pk1, std::fabs(r));
                    }
                }
                raisePeak(rc.peak[0], pk0);
                raisePeak(rc.peak[1], mono ? pk0 : pk1);
            }
            dec.skip(static_cast<std::size_t>(needed));
            rc.rs.advance(srcRate, sampleRate, len, needed);
        }
    }
}

// 5.3 and 5.4, local_mutex_ held.
void Engine::mixLocal(const float* const* in, int numIn, float* const* out, int numOut, int len, int sampleRate) {
    float decay = static_cast<float>(std::pow(0.015625, static_cast<double>(len) / sampleRate));
    bool any_solo = any_solo_.load();
    int offset = local_offset_.load();
    int maxchan = maxchan_.load();
    if (static_cast<int>(mix_tmp_.size()) < len) mix_tmp_.assign(static_cast<std::size_t>(len), 0.0f);
    const float* zeros = mix_tmp_.data();
    for (auto& kv : locals_) {
        LocalState& ls = *kv.second;
        const LocalChannel& s = ls.s;
        const float* src1 = s.source >= 0 && s.source < numIn ? in[s.source] : zeros;
        const float* src2 = s.stereo ? (s.source + 1 < numIn && s.source >= 0 ? in[s.source + 1] : src1) : src1;
        // peak meter from the source, always
        float pk1 = 0, pk2 = 0;
        for (int i = 0; i < len; ++i) {
            pk1 = std::max(pk1, std::fabs(src1[i]));
            pk2 = std::max(pk2, std::fabs(src2[i]));
        }
        decayPeak(ls.peak[0], decay);
        decayPeak(ls.peak[1], decay);
        raisePeak(ls.peak[0], pk1);
        raisePeak(ls.peak[1], pk2);
        // capture for upload
        if (running_.load() && ls.transmitting && kv.first < maxchan) {
            std::lock_guard<std::mutex> ql(ls.qm);
            if (ls.queue.size() < static_cast<std::size_t>(kUploadQueueLimit)) {
                UploadBlock b;
                b.kind = UploadBlock::Audio;
                b.channels = s.stereo ? 2 : 1;
                b.rate = sampleRate;
                if (!ls.spare.empty()) {
                    b.data = std::move(ls.spare.back());
                    ls.spare.pop_back();
                }
                b.data.resize(static_cast<std::size_t>(len) * static_cast<std::size_t>(b.channels));
                if (s.stereo) {
                    for (int i = 0; i < len; ++i) {
                        b.data[static_cast<std::size_t>(i) * 2] = src1[i];
                        b.data[static_cast<std::size_t>(i) * 2 + 1] = src2[i];
                    }
                } else {
                    std::copy(src1, src1 + len, b.data.begin());
                }
                ls.queue.push_back(std::move(b));
            }
        }
        // monitoring
        bool active = (!any_solo && !s.mute) || s.solo;
        if (!active || numOut == 0) continue;
        int idx;
        bool mono;
        outputPair(s.outChannel, offset, numOut, &idx, &mono);
        float gl, gr;
        panGains(s.pan, &gl, &gr);
        if (mono) {
            for (int i = 0; i < len; ++i) out[idx][i] += (src1[i] + src2[i]) * 0.5f * s.volume;
        } else {
            for (int i = 0; i < len; ++i) {
                out[idx][i] += src1[i] * s.volume * gl;
                out[idx + 1][i] += src2[i] * s.volume * gr;
            }
        }
    }
}

// Section 9.
void Engine::mixMetronome(float* const* out, int numOut, int len, int sampleRate) {
    float vol = metro_mute_.load() ? 0.0f : metro_vol_.load();
    int bpi = active_bpi_.load();
    click_len_ = sampleRate / 100;
    int idx = 0;
    bool mono = true;
    if (numOut > 0) outputPair(metro_out_.load(), 0, numOut, &idx, &mono);
    float gl, gr;
    panGains(metro_pan_.load(), &gl, &gr);
    int ipos = interval_pos_.load();
    if (vol < 0.0001f || numOut == 0) {
        // Nothing audible: only keep the beat clock in step, without a per-sample loop.
        while (beat_index_ < bpi && next_beat_ < ipos + len) {
            ++beat_index_;
            next_beat_ += beat_len_;
        }
        click_pos_ = -1;
        return;
    }
    for (int i = 0; i < len; ++i) {
        int ip = ipos + i;
        if (ip == next_beat_ && beat_index_ < bpi) {
            click_pos_ = 0;
            click_first_ = beat_index_ == 0;
            ++beat_index_;
            next_beat_ += beat_len_;
        }
        if (click_pos_ >= 0) {
            if (click_pos_ >= click_len_) {
                click_pos_ = -1;
            } else if (vol >= 0.0001f && numOut > 0) {
                double k = click_pos_;
                float v = click_first_ ? static_cast<float>(std::sin(k * 6000.0 / sampleRate))
                                       : 0.25f * static_cast<float>(std::sin(2.0 * k * 6000.0 / sampleRate));
                v *= vol;
                if (mono) out[idx][i] += v;
                else {
                    out[idx][i] += v * gl;
                    out[idx + 1][i] += v * gr;
                }
                ++click_pos_;
            } else {
                ++click_pos_;
            }
        }
    }
}

// 8.5 last paragraph and 8.6.
void Engine::applyMaster(float* const* out, int numOut, int len, int sampleRate) {
    float decay = static_cast<float>(std::pow(0.015625, static_cast<double>(len) / sampleRate));
    decayPeak(master_peak_[0], decay);
    decayPeak(master_peak_[1], decay);
    if (numOut == 0) return;
    float vol = master_mute_.load() ? 0.0f : master_vol_.load();
    float gl, gr;
    panGains(master_pan_.load(), &gl, &gr);
    float pk0 = 0, pk1 = 0;
    float g0 = vol * gl, g1 = vol * gr;
    if (g0 == 1.0f) {                            // unity: meter only, leave the samples alone
        for (int i = 0; i < len; ++i) pk0 = std::max(pk0, std::fabs(out[0][i]));
    } else {
        for (int i = 0; i < len; ++i) {
            out[0][i] *= g0;
            pk0 = std::max(pk0, std::fabs(out[0][i]));
        }
    }
    if (numOut > 1) {
        if (g1 == 1.0f) {
            for (int i = 0; i < len; ++i) pk1 = std::max(pk1, std::fabs(out[1][i]));
        } else {
            for (int i = 0; i < len; ++i) {
                out[1][i] *= g1;
                pk1 = std::max(pk1, std::fabs(out[1][i]));
            }
        }
    }
    raisePeak(master_peak_[0], pk0);
    raisePeak(master_peak_[1], pk1);
}

}  // namespace engine
}  // namespace njproto
