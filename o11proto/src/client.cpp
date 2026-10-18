// o11proto - client state machine. See PROTOCOL.md sections 4-9.
#include "o11proto/client.h"

#include <chrono>
#include <cstring>
#include <random>

#include "o11proto/sha1.h"
#include "socket_compat.h"

namespace o11proto {

bool Client::RemoteUser::anyActive() const {
    for (const auto& c : channels)
        if (c.active) return true;
    return false;
}

// The client's Asio side: one io_context, run by poll() on the caller's
// thread (concurrency hint 1: nothing else runs it, so no strand or lock is
// needed), the connection's read and write waits, and a timer for the
// keepalives and the idle downloads. `gen` counts the connections: a wait
// of an older one ends with operation_aborted when it closes and then does
// nothing, even if its handler runs after the next connect().
struct Client::Net {
    asio::io_context io{1};
    asio::steady_timer timer{io};
    unsigned gen = 0;
    bool reading = false, writing = false, timing = false, pass_due = false;
    // Runs ready handlers, waiting up to `ms` for the first (0: none, < 0:
    // as long as it takes). Bounded, as one pass of a poll() loop was: what is
    // still ready then runs at the next call.
    void run(int ms) {
        if (io.stopped()) io.restart();
        if (ms < 0) io.run_one();
        else if (ms > 0) io.run_one_for(std::chrono::milliseconds(ms));
        for (int i = 0; i < 1024 && io.poll_one(); ++i) {
        }
    }
};

void Client::NetDeleter::operator()(Net* p) const { delete p; }

// How often the housekeeping runs: keepalives are whole seconds, the
// download timeout 8 s.
constexpr auto kClientTick = std::chrono::milliseconds(100);

Client::Net& Client::net() {
    if (!net_) net_.reset(new Net);
    return *net_;
}

Client::~Client() { conn_.close(); }

Guid Client::randomGuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    Guid g;
    std::uint64_t a = gen(), b = gen();
    for (int i = 0; i < 8; ++i) {
        g[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(a >> (8 * i));
        g[static_cast<std::size_t>(i + 8)] = static_cast<std::uint8_t>(b >> (8 * i));
    }
    return g;
}

bool Client::connect(const std::string& host, std::uint16_t port, const Options& options) {
    disconnect("reconnecting");
    opt_ = options;
    state_ = State::Disconnected;
    fail_reason_.clear();
    username_.clear();
    maxchan_ = 0;
    bpm_ = bpi_ = 0;
    has_codec_mask_ = false;
    codec_mask_ = CodecMask();
    users_.clear();
    downloads_.clear();
    keepalive_seconds_ = kDefaultKeepaliveSeconds;
    licence_pending_ = false;
    has_live_ = false;                          // Live state is per connection
    live_ = live::ServerLive();
    server_addr_.clear();
    server_port_ = port;

    Net& n = net();
    asio::error_code ec;
    asio::ip::tcp::resolver resolver(n.io);
    // No flags: every address the name has, as getaddrinfo() gives them
    // (Asio's default, AI_ADDRCONFIG, would hide loopback-only machines').
    const auto found = resolver.resolve(host, std::to_string(port), asio::ip::resolver_base::flags(), ec);
    if (ec || found.empty()) return false;
    // The first address whose non-blocking connect starts (one that fails at
    // once, an IPv6 one without a route, say, gives way to the next).
    for (const auto& entry : found) {
        asio::ip::tcp::socket s(n.io);
        s.open(entry.endpoint().protocol(), ec);
        if (ec) continue;
        s.non_blocking(true, ec);
        if (ec) continue;
        const asio::ip::tcp::endpoint ep = entry.endpoint();
        if (::connect(s.native_handle(), ep.data(), static_cast<socklen_t>(ep.size())) != 0) {
            const int err = compat::lastError();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) continue;
#else
            if (err != EINPROGRESS) continue;
#endif
        }
        conn_.adopt(std::unique_ptr<Connection::Net, Connection::NetDeleter>(new Connection::Net(std::move(s))));
        break;
    }
    if (!conn_.isOpen()) return false;
    ++n.gen;
    n.reading = n.writing = n.timing = n.pass_due = false;
    state_ = State::Connecting;
    // The connect's outcome: writable, or (on Windows, whose select() reports
    // a failed connect only among the exceptions) an error.
    const unsigned g = n.gen;
    asio::ip::tcp::socket& sock = conn_.net()->socket;
    sock.async_wait(asio::socket_base::wait_write, [this, g](const asio::error_code& e) {
        if (!e && net_->gen == g && state_ == State::Connecting) connected();
    });
#ifdef _WIN32
    sock.async_wait(asio::socket_base::wait_error, [this, g](const asio::error_code& e) {
        if (!e && net_->gen == g && state_ == State::Connecting) connected();
    });
#endif
    armTimer();
    return true;
}

// The connect finished, one way or the other.
void Client::connected() {
    int err = 0;
    socklen_t len = sizeof(err);
    compat::getOpt(conn_.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
#ifdef _WIN32
    if (err == 0) {
        // The error wait can end first only for a failed connect.
        fd_set ex;
        FD_ZERO(&ex);
        FD_SET(conn_.fd(), &ex);
        timeval zero{};
        if (::select(0, nullptr, nullptr, &ex, &zero) > 0 && FD_ISSET(conn_.fd(), &ex)) err = WSAECONNREFUSED;
    }
#endif
    if (err != 0) {
        fail("connect failed: " + compat::errorString(err));
        return;
    }
    state_ = State::AwaitingChallenge;
    conn_.touchRecv();
    asio::error_code ec;
    const asio::ip::tcp::endpoint peer = conn_.net()->socket.remote_endpoint(ec);
    server_addr_ = ec ? std::string() : compat::addressText(peer.address());
    if (server_addr_.find(':') != std::string::npos) server_addr_.clear();   // IPv6: no Live
    readNow();                                  // the challenge may be here already; then the pass
}

void Client::disconnect(const std::string& reason) {
    if (!conn_.isOpen()) return;
    conn_.flush();          // best effort for anything still queued
    conn_.close();          // its waits end with operation_aborted
    if (net_) {
        ++net_->gen;
        net_->reading = net_->writing = false;
        net_->timer.cancel();
        net_->timing = false;
    }
    for (auto& d : downloads_)
        if (on_download_end) on_download_end(d.first, false);
    downloads_.clear();
    State old = state_;
    state_ = State::Disconnected;
    if (old != State::Disconnected && on_disconnect) on_disconnect(reason);
}

void Client::fail(const std::string& reason) {
    fail_reason_ = reason;
    disconnect(reason);
    state_ = State::Failed;
}

bool Client::sendRaw(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (!conn_.isOpen()) return false;
    if (!conn_.send(type, payload)) {
        fail("send queue overflow");
        return false;
    }
    schedulePass();
    return true;
}

// ------------------------------------------------------------- event loop

// Reads what the socket has, handles it, and then waits for more: always in
// that order, as Asio's waits are not speculative (on epoll and kqueue a
// readiness that came before the wait was armed is not reported again).
void Client::readNow() {
    if (!conn_.readAvailable()) {
        // The server closes after a refused login; keep that reason.
        if (state_ == State::Failed) return;
        if (!fail_reason_.empty()) fail(fail_reason_);
        else fail("connection closed by peer");
        return;
    }
    const unsigned g = net_->gen;
    RawMessage msg;
    bool error = false;
    while (conn_.isOpen() && conn_.nextMessage(msg, error)) handleMessage(msg);
    if (error) {
        fail("protocol error from server");
        return;
    }
    if (!conn_.isOpen() || net_->gen != g) return;
    armRead();
    schedulePass();
}

void Client::armRead() {
    Net& n = *net_;
    if (n.reading || !conn_.isOpen()) return;
    n.reading = true;
    const unsigned g = n.gen;
    conn_.net()->socket.async_wait(asio::socket_base::wait_read, [this, g](const asio::error_code& ec) {
        Net& m = *net_;
        if (m.gen != g) return;                 // a closed connection's wait
        m.reading = false;
        if (ec == asio::error::operation_aborted || !conn_.isOpen()) return;
        readNow();
    });
}

void Client::armWrite() {
    Net& n = *net_;
    if (n.writing || !conn_.isOpen()) return;
    n.writing = true;
    const unsigned g = n.gen;
    conn_.net()->socket.async_wait(asio::socket_base::wait_write, [this, g](const asio::error_code& ec) {
        Net& m = *net_;
        if (m.gen != g) return;
        m.writing = false;
        if (ec != asio::error::operation_aborted) schedulePass();   // an error shows in the flush
    });
}

// The housekeeping, every kClientTick while connected: no wait of its own in
// poll(), so a quiet connection still sends its keepalives on time.
void Client::armTimer() {
    Net& n = *net_;
    if (n.timing || !conn_.isOpen()) return;
    n.timing = true;
    const unsigned g = n.gen;
    n.timer.expires_after(kClientTick);
    n.timer.async_wait([this, g](const asio::error_code& ec) {
        Net& m = *net_;
        if (m.gen != g) return;
        m.timing = false;
        if (ec || !conn_.isOpen()) return;
        handleTimers(Clock::now());
        if (!conn_.isOpen() || m.gen != g) return;
        armTimer();
        schedulePass();
    });
}

// What used to end every poll(): the queue goes out once whatever is ready
// has run (one pass for all of it).
void Client::schedulePass() {
    Net& n = net();
    if (n.pass_due || !conn_.isOpen()) return;
    n.pass_due = true;
    const unsigned g = n.gen;
    asio::post(n.io, [this, g] {
        Net& m = *net_;
        if (m.gen != g) return;
        m.pass_due = false;
        pass();
    });
}

void Client::pass() {
    if (!conn_.isOpen() || state_ == State::Connecting) return;
    if (!conn_.flush()) {
        fail("send failed");
        return;
    }
    if (conn_.wantWrite()) armWrite();
}

void Client::poll(int timeout_ms) {
    if (!conn_.isOpen()) return;
    net_->run(timeout_ms);
}

bool Client::waitFor(const std::function<bool()>& pred, int timeout_ms) {
    TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        auto now = Clock::now();
        if (now >= deadline) return false;
        if (!conn_.isOpen()) return pred();
        int slice = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        poll(slice > 50 ? 50 : slice);
    }
    return true;
}

// Runs the io_context until a timer of `ms` ends (whether or not anything
// is connected meanwhile).
void Client::runFor(int ms) {
    Net& n = net();
    asio::steady_timer until(n.io, std::chrono::milliseconds(ms));
    bool done = false;
    until.async_wait([&done](const asio::error_code&) { done = true; });
    while (!done) {
        if (n.io.stopped()) n.io.restart();
        n.io.run_one();
    }
}

void Client::handleTimers(TimePoint now) {
    // Keepalive: 45 s while a licence prompt is up, else the server's value.
    int k = licence_pending_ ? kLicenceKeepaliveSeconds : keepalive_seconds_;
    if (state_ != State::Connecting) {
        auto interval = std::chrono::seconds(k);
        if (now - conn_.lastRecv() > 3 * interval) {
            fail("keepalive timeout");
            return;
        }
        if (opt_.send_keepalives && now - conn_.lastSend() >= interval) conn_.sendKeepalive();
    }
    // Downloads that went quiet for 8 s are abandoned.
    for (auto it = downloads_.begin(); it != downloads_.end();) {
        if (now - it->second > std::chrono::seconds(kTransferTimeoutSeconds)) {
            Guid g = it->first;
            it = downloads_.erase(it);
            if (on_download_end) on_download_end(g, false);
        } else {
            ++it;
        }
    }
}

// --------------------------------------------------------- message handling

void Client::handleMessage(const RawMessage& m) {
    if (on_message) on_message(m);
    switch (m.type) {
    case kMsgAuthChallenge:
        handleChallenge(m.payload);
        break;
    case kMsgAuthReply:
        handleAuthReply(m.payload);
        break;
    case kMsgConfigChange: {
        ConfigChange c;
        if (!decode(m.payload, c)) return;
        bpm_ = c.bpm;
        bpi_ = c.bpi;
        if (on_config) on_config(c);
        break;
    }
    case kMsgUserInfoChange:
        handleUserInfo(m.payload);
        break;
    case kMsgDownloadBegin: {
        DownloadBegin d;
        if (!decode(m.payload, d)) return;
        if (!guidIsZero(d.guid) && d.fourcc != 0) downloads_[d.guid] = Clock::now();
        if (on_download_begin) on_download_begin(d);
        break;
    }
    case kMsgDownloadWrite: {
        IntervalWrite w;
        if (!decode(m.payload, w)) return;
        auto it = downloads_.find(w.guid);
        if (it == downloads_.end()) return;     // unknown or timed-out transfer
        it->second = Clock::now();
        if (on_download_data && !w.data.empty()) on_download_data(w.guid, w.data);
        if (w.last()) {
            downloads_.erase(it);
            if (on_download_end) on_download_end(w.guid, true);
        }
        break;
    }
    case kMsgCodecMask: {
        CodecMask cm;
        if (!decode(m.payload, cm)) return;
        codec_mask_ = cm;
        has_codec_mask_ = true;
        if (on_codec_mask) on_codec_mask(cm);
        break;
    }
    case kMsgChat: {
        ChatMessage c;
        if (!decode(m.payload, c)) return;
        if (on_chat) on_chat(c);
        break;
    }
    case kMsgKeepalive:
        break;
    case live::kMsgServerLive: {
        live::ServerLive l;
        if (!opt_.live || state_ != State::LoggedIn || !live::decode(m.payload, l)) return;
        live_ = std::move(l);
        has_live_ = true;
        if (on_live) on_live(live_);
        break;
    }
    default:
        break;                                  // unknown types are ignored
    }
}

void Client::handleChallenge(const std::vector<std::uint8_t>& p) {
    if (state_ != State::AwaitingChallenge) return;
    AuthChallenge ch;
    if (!decode(p, ch)) {
        fail("malformed AUTH_CHALLENGE");
        return;
    }
    if (on_challenge) on_challenge(ch);
    if (ch.protocol_version < 0x00020000 || ch.protocol_version >= opt_.max_server_version) {
        fail("server is incorrect protocol version");
        return;
    }
    keepalive_seconds_ = ch.keepaliveSeconds();
    challenge_ = ch.challenge;
    if (!ch.has_licence) {
        sendLogin(false);
        return;
    }
    licence_pending_ = true;
    LicenceDecision d;
    if (on_licence_decision) d = on_licence_decision(ch.licence);
    else if (on_licence) d = on_licence(ch.licence) ? LicenceDecision::Agree : LicenceDecision::Refuse;
    else d = opt_.agree_licence ? LicenceDecision::Agree : LicenceDecision::Refuse;
    if (d == LicenceDecision::Defer) {
        state_ = State::AwaitingLicence;       // keepalives continue at 45 s
        return;
    }
    sendLogin(d == LicenceDecision::Agree);
}

void Client::answerLicence(bool agree) {
    if (state_ != State::AwaitingLicence) return;
    sendLogin(agree);
}

void Client::sendLogin(bool licence_agreed) {
    std::uint32_t caps = opt_.codec_caps & kCapCodecMask;
    if (opt_.live) caps |= live::kCapLive;
    if (opt_.live && opt_.live_direct) caps |= live::kCapLiveDirect;
    if (licence_pending_) {
        if (!licence_agreed && !opt_.login_when_licence_refused) {
            fail("licence not agreed");
            return;
        }
        if (licence_agreed) caps |= kCapLicenceAgreed;
    }
    AuthUser au;
    au.passhash = passwordHash(opt_.username, opt_.password, challenge_);
    au.username = opt_.username;
    au.client_caps = caps;
    au.client_version = opt_.client_version;
    if (sendRaw(kMsgClientAuthUser, encode(au))) state_ = State::AwaitingReply;
}

void Client::handleAuthReply(const std::vector<std::uint8_t>& p) {
    AuthReply r;
    if (!decode(p, r)) {
        fail("malformed AUTH_REPLY");
        return;
    }
    licence_pending_ = false;
    if (on_auth_reply) on_auth_reply(r);
    if (!r.success()) {
        // The server closes the socket next; remember why.
        fail_reason_ = r.has_text && !r.text.empty() ? r.text : "login refused";
        return;
    }
    username_ = r.text;
    maxchan_ = r.maxchan;
    state_ = State::LoggedIn;
    sendChannels();
}

void Client::handleUserInfo(const std::vector<std::uint8_t>& p) {
    UserInfoChange u;
    if (!decode(p, u)) return;
    for (const auto& rec : u.records) {
        if (rec.index >= kMaxChannels) continue;
        auto it = users_.find(rec.username);
        if (rec.active) {
            bool is_new = it == users_.end();
            if (is_new) {
                it = users_.emplace(rec.username, RemoteUser()).first;
                it->second.name = rec.username;
            }
            RemoteChannel& ch = it->second.channels[rec.index];
            ch.active = true;
            ch.name = rec.channel;
            ch.volume = rec.volume;
            ch.pan = rec.pan;
            ch.flags = rec.flags;
            if (is_new && opt_.auto_subscribe) subscribe(rec.username, 0xFFFFFFFFu);
        } else if (it != users_.end()) {
            it->second.channels[rec.index].active = false;
            if (!it->second.anyActive()) users_.erase(it);
        }
        if (on_userinfo) on_userinfo(rec);
    }
}

// ------------------------------------------------------------------ sending

void Client::setChannels(const std::vector<ChannelInfoRecord>& channels) {
    channels_ = channels;
    if (channels_.size() > static_cast<std::size_t>(kMaxChannels)) channels_.resize(kMaxChannels);
    if (loggedIn()) sendChannels();
}

void Client::sendChannels() {
    SetChannelInfo sci;
    sci.channels = channels_;
    sendRaw(kMsgClientSetChannelInfo, encode(sci));
}

void Client::subscribe(const std::string& username, std::uint32_t mask) {
    SetUserMask um;
    um.entries.push_back({username, mask});
    sendRaw(kMsgClientSetUserMask, encode(um));
}

Guid Client::beginInterval(std::uint8_t channel, std::uint32_t fourcc, std::uint32_t estsize) {
    UploadBegin b;
    b.guid = randomGuid();
    b.estsize = estsize;
    b.fourcc = fourcc;
    b.channel = channel;
    sendRaw(kMsgClientUploadBegin, encode(b));
    return b.guid;
}

void Client::writeInterval(const Guid& guid, const std::uint8_t* data, std::size_t len, bool last) {
    std::size_t off = 0;
    do {
        std::size_t n = len - off;
        if (n > kMaxWriteChunk) n = kMaxWriteChunk;
        IntervalWrite w;
        w.guid = guid;
        w.data.assign(data + off, data + off + n);
        off += n;
        w.flags = (last && off >= len) ? 1 : 0;
        sendRaw(kMsgClientUploadWrite, encode(w));
    } while (off < len);
}

void Client::endInterval(const Guid& guid) {
    IntervalWrite w;
    w.guid = guid;
    w.flags = 1;
    sendRaw(kMsgClientUploadWrite, encode(w));
}

void Client::sendSilence(std::uint8_t channel) {
    UploadBegin b;                 // zero GUID, fourcc 0 = silence this interval
    b.channel = channel;
    sendRaw(kMsgClientUploadBegin, encode(b));
}

void Client::sendChat(const ChatMessage& m) { sendRaw(kMsgChat, encode(m)); }
void Client::say(const std::string& text) { sendChat(ChatMessage("MSG", text)); }
void Client::privmsg(const std::string& user, const std::string& text) {
    sendChat(ChatMessage("PRIVMSG", user, text));
}
void Client::admin(const std::string& command) { sendChat(ChatMessage("ADMIN", command)); }

std::uint32_t Client::preferredFourcc(bool want_flac) const {
    if (!has_codec_mask_) return kFourccVorbis;
    if (want_flac && (codec_mask_.codecs & kCapFlac) && (opt_.codec_caps & kCapFlac))
        return kFourccFlac;
    if ((codec_mask_.codecs & kCapOpus) && (opt_.codec_caps & kCapOpus)) return kFourccOpus;
    if ((codec_mask_.codecs & kCapFlac) && (opt_.codec_caps & kCapFlac)) return kFourccFlac;
    return kFourccVorbis;
}

}  // namespace o11proto
