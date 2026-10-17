// njproto - client state machine. See PROTOCOL.md sections 4-9.
#include "njproto/client.h"

#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#include "njproto/sha1.h"
#include "socket_compat.h"

namespace njproto {

bool Client::RemoteUser::anyActive() const {
    for (const auto& c : channels)
        if (c.active) return true;
    return false;
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

    if (!compat::startup()) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    socket_t fd = kInvalidSocket;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kInvalidSocket) continue;
        setNonBlocking(fd);
        int rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
        if (rc == 0 || compat::isInProgress(compat::lastError())) break;
        compat::closeSocket(fd);
        fd = kInvalidSocket;
    }
    freeaddrinfo(res);
    if (fd == kInvalidSocket) return false;
    conn_.adopt(fd);
    state_ = State::Connecting;
    return true;
}

void Client::disconnect(const std::string& reason) {
    if (!conn_.isOpen()) return;
    conn_.flush();          // best effort for anything still queued
    conn_.close();
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
    return true;
}

// ------------------------------------------------------------- event loop

void Client::poll(int timeout_ms) {
    if (!conn_.isOpen()) return;
    pollfd pfd{};
    pfd.fd = conn_.fd();
    pfd.events = POLLIN;
    if (state_ == State::Connecting || conn_.wantWrite()) pfd.events |= POLLOUT;
    int rc = compat::poll(&pfd, 1, timeout_ms);
    if (rc < 0 && !compat::isInterrupted(compat::lastError())) {
        fail("poll failed");
        return;
    }
    if (state_ == State::Connecting && !(rc > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP)))) {
        int err = 0;
        if (compat::connectFailed(conn_.fd(), err)) {   // Windows only; see socket_compat.h
            fail("connect failed: " + compat::errorString(err));
            return;
        }
    }
    if (rc > 0) {
        if (state_ == State::Connecting) {
            if (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) {
                int err = 0;
                socklen_t len = sizeof(err);
                compat::getOpt(conn_.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0) {
                    fail("connect failed: " + compat::errorString(err));
                    return;
                }
                state_ = State::AwaitingChallenge;
                conn_.touchRecv();
            }
        }
        if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            if (!conn_.readAvailable()) {
                // The server closes after a refused login; keep that reason.
                if (state_ == State::Failed) return;
                if (!fail_reason_.empty()) fail(fail_reason_);
                else fail("connection closed by peer");
                return;
            }
            RawMessage m;
            bool error = false;
            while (conn_.isOpen() && conn_.nextMessage(m, error)) handleMessage(m);
            if (error) {
                fail("protocol error from server");
                return;
            }
        }
    }
    if (!conn_.isOpen()) return;
    handleTimers(Clock::now());
    if (conn_.isOpen() && !conn_.flush()) fail("send failed");
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

void Client::runFor(int ms) {
    TimePoint deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (!conn_.isOpen()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        poll(50);
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

}  // namespace njproto
