// njproto - server behaviour. See PROTOCOL.md sections 4-9.
#include "njproto/server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include "njproto/sha1.h"

namespace njproto {

// ------------------------------------------------------------- small helpers

static std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}

static bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

static bool istartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == lower(prefix);
}

static std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Splits "word rest of line" into the first word and the remainder.
static void splitFirst(const std::string& s, std::string& first, std::string& rest) {
    std::string t = trim(s);
    std::size_t sp = t.find_first_of(" \t");
    if (sp == std::string::npos) {
        first = t;
        rest.clear();
    } else {
        first = t.substr(0, sp);
        rest = trim(t.substr(sp + 1));
    }
}

static bool parseInt(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    out = v;
    return true;
}

std::uint32_t parsePrivileges(const std::string& letters) {
    std::uint32_t p = 0;
    for (char ch : letters) {
        switch (ch) {
        case 't': p |= kPrivTopic; break;
        case 'c': p |= kPrivChat; break;
        case 'b': p |= kPrivBpm; break;
        case 'k': p |= kPrivKick; break;
        case 'r': p |= kPrivReserve; break;
        case 'm': p |= kPrivMulti; break;
        case 'h': p |= kPrivHidden; break;
        case 'v': p |= kPrivVote; break;
        case 'p': p |= kPrivSeePrivate; break;
        case 's': p |= kPrivStatus; break;
        case '*': p |= kPrivAll; break;
        default: break;
        }
    }
    return p;
}

std::string privilegesToLetters(std::uint32_t privs) {
    std::string s;
    if (privs & kPrivTopic) s += 't';
    if (privs & kPrivChat) s += 'c';
    if (privs & kPrivBpm) s += 'b';
    if (privs & kPrivKick) s += 'k';
    if (privs & kPrivReserve) s += 'r';
    if (privs & kPrivMulti) s += 'm';
    if (privs & kPrivHidden) s += 'h';
    if (privs & kPrivVote) s += 'v';
    if (privs & kPrivSeePrivate) s += 'p';
    if (privs & kPrivStatus) s += 's';
    return s;
}

bool Server::Channel::sameAs(const Channel& o) const {
    if (active != o.active) return false;
    if (!active) return true;
    return name == o.name && volume == o.volume && pan == o.pan && flags == o.flags;
}

std::string Server::codecName(std::uint32_t bits) {
    if (bits & kCapOpus) return "Opus";
    if (bits & kCapFlac) return "FLAC";
    return "Vorbis";
}

// ------------------------------------------------------------------ lifecycle

Server::~Server() { stop(); }

void Server::log(const std::string& s) const {
    if (cfg_.log) cfg_.log(s);
}

bool Server::start(const ServerConfig& config) {
    stop();
    cfg_ = config;
    bpm_ = cfg_.bpm;
    bpi_ = cfg_.bpi;
    topic_ = cfg_.topic;
    opus_kbps_ = cfg_.opus_kbps;
    if (opus_kbps_ != 0 && (opus_kbps_ < 32 || opus_kbps_ > 510)) {
        log("warning: Opus bitrate " + std::to_string(opus_kbps_) + " is outside 32-510, using 128");
        opus_kbps_ = 128;                       // section 8.4
    }
    if (cfg_.max_channels < 0) cfg_.max_channels = 0;
    if (cfg_.max_channels > kMaxChannels) cfg_.max_channels = kMaxChannels;
    // The chosen codec must be one the server relays; a required codec wins.
    chosen_codec_ = cfg_.chosen_codec;
    if (cfg_.require_codec) {
        cfg_.allow_codecs |= cfg_.require_codec;
        chosen_codec_ = cfg_.require_codec;
    } else if (chosen_codec_ && !(chosen_codec_ & cfg_.allow_codecs)) {
        chosen_codec_ = (cfg_.allow_codecs & kCapOpus) ? kCapOpus
                        : (cfg_.allow_codecs & kCapFlac) ? kCapFlac : 0;
    }
    effective_codec_ = computeEffectiveCodec();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(cfg_.port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!cfg_.bind_address.empty() && inet_pton(AF_INET, cfg_.bind_address.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(fd, 64) != 0) {
        ::close(fd);
        return false;
    }
    setNonBlocking(fd);
    socklen_t len = sizeof(sa);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) == 0) port_ = ntohs(sa.sin_port);
    listen_fd_ = fd;
    log("listening on port " + std::to_string(port_));
    return true;
}

void Server::stop() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    conns_.clear();
    transfers_.clear();
    votes_.clear();
}

std::size_t Server::userCount() const {
    std::size_t n = 0;
    for (const auto& c : conns_)
        if (c->logged_in && !c->hidden()) ++n;
    return n;
}

std::vector<std::string> Server::usernames() const {
    std::vector<std::string> v;
    for (const auto& c : conns_)
        if (c->logged_in) v.push_back(c->name);
    return v;
}

std::vector<Server::Conn*> Server::loggedIn() {
    std::vector<Conn*> v;
    for (auto& c : conns_)
        if (c->logged_in && c->c.isOpen() && !c->close_after_flush) v.push_back(c.get());
    return v;
}

Server::Conn* Server::connById(int id) {
    for (auto& c : conns_)
        if (c->id == id) return c.get();
    return nullptr;
}

// ----------------------------------------------------------------- event loop

void Server::runFor(int ms) {
    TimePoint deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) poll(20);
}

void Server::poll(int timeout_ms) {
    if (listen_fd_ < 0) return;
    std::vector<pollfd> pfds;
    pfds.reserve(conns_.size() + 1);
    pollfd lp{};
    lp.fd = listen_fd_;
    lp.events = POLLIN;
    pfds.push_back(lp);
    for (auto& c : conns_) {
        pollfd p{};
        p.fd = c->c.fd();
        p.events = POLLIN;
        if (c->c.wantWrite()) p.events |= POLLOUT;
        pfds.push_back(p);
    }
    int rc = ::poll(pfds.data(), pfds.size(), timeout_ms);
    if (rc < 0 && errno != EINTR) return;
    if (rc > 0) {
        for (std::size_t i = 1; i < pfds.size(); ++i) {
            Conn& c = *conns_[i - 1];
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!c.c.readAvailable()) {
                    dropConnection(c, "connection closed");
                    continue;
                }
                RawMessage m;
                bool error = false;
                while (c.c.isOpen() && !c.close_after_flush && c.c.nextMessage(m, error))
                    handleMessage(c, m);
                if (error) dropConnection(c, "protocol error");
            }
        }
    }
    handleTimers(Clock::now());

    // Flush everything, then reap closed connections.
    for (auto& c : conns_) {
        if (!c->c.isOpen()) continue;
        if (!c->c.flush()) {
            dropConnection(*c, "send failed");
            continue;
        }
        if (c->close_after_flush && (!c->c.wantWrite() || Clock::now() > c->close_deadline))
            c->c.close();
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const std::unique_ptr<Conn>& c) { return !c->c.isOpen(); }),
                 conns_.end());

    // Accept only after existing connections were serviced and reaped, so a
    // peer that already closed does not count against the pending limits.
    if (rc > 0 && (pfds[0].revents & POLLIN)) acceptPending();
}

void Server::acceptPending() {
    for (;;) {
        sockaddr_storage ss;
        socklen_t len = sizeof(ss);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd < 0) return;
        auto c = std::make_unique<Conn>();
        c->c.adopt(fd);
        c->id = next_id_++;
        c->addr = c->c.peerAddress();
        c->connected_at = Clock::now();

        // Limits on connections that haven't logged in yet (section 4.3).
        // A pending socket whose peer already closed (EOF visible with a
        // non-blocking peek) is about to be reaped and does not count, even
        // when it arrived in the same accept batch as this one.
        int pending = 0, pending_here = 0;
        for (const auto& o : conns_) {
            if (o->logged_in || !o->c.isOpen()) continue;
            std::uint8_t peek;
            if (::recv(o->c.fd(), &peek, 1, MSG_PEEK | MSG_DONTWAIT) == 0) continue;
            ++pending;
            if (o->addr == c->addr) ++pending_here;
        }
        if (pending >= cfg_.max_pending_connections || pending_here >= cfg_.max_pending_per_address) {
            log("refusing connection from " + c->addr + ": too many pending logins");
            continue;                               // unique_ptr closes the socket
        }

        // Challenge: 8 random bytes, caps = licence flag | keepalive << 8.
        static std::random_device rd;
        static std::mt19937 gen(rd());
        for (auto& b : c->challenge) b = static_cast<std::uint8_t>(gen() & 0xFF);
        AuthChallenge ch;
        ch.challenge = c->challenge;
        int k = cfg_.keepalive_seconds;
        if (k < 0 || k > 255) k = 0;
        ch.server_caps = static_cast<std::uint32_t>(k) << 8;
        if (!cfg_.licence.empty()) {
            ch.has_licence = true;
            ch.licence = cfg_.licence;
        }
        ch.protocol_version = kProtocolVersion;
        c->c.send(kMsgAuthChallenge, encode(ch));
        log("connection " + std::to_string(c->id) + " from " + c->addr);
        conns_.push_back(std::move(c));
    }
}

void Server::handleTimers(TimePoint now) {
    for (auto& cp : conns_) {
        Conn& c = *cp;
        if (!c.c.isOpen() || c.close_after_flush) continue;
        if (!c.logged_in) {
            int limit = cfg_.licence.empty() ? cfg_.login_timeout_seconds
                                             : cfg_.licence_login_timeout_seconds;
            if (now - c.connected_at > std::chrono::seconds(limit)) {
                refuseLogin(c, "authorization timeout");
                continue;
            }
        }
        // 45 s keepalive while a licence prompt may be showing (section 2).
        int k = (!c.logged_in && !cfg_.licence.empty()) ? kLicenceKeepaliveSeconds
                                                         : cfg_.keepalive_seconds;
        if (!c.c.keepalive(k, now)) {
            dropConnection(c, "keepalive timeout");
            continue;
        }
    }
    // Idle transfers.
    for (auto it = transfers_.begin(); it != transfers_.end();) {
        if (now - it->second.last > std::chrono::seconds(cfg_.transfer_timeout_seconds))
            it = transfers_.erase(it);
        else
            ++it;
    }
    // Expired votes.
    for (auto& setting : votes_) {
        for (auto it = setting.second.begin(); it != setting.second.end();) {
            if (now - it->second.when > std::chrono::seconds(cfg_.voting_timeout_seconds))
                it = setting.second.erase(it);
            else
                ++it;
        }
    }
}

// -------------------------------------------------------------------- sending

void Server::send(Conn& c, std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (!c.c.isOpen()) return;
    if (!c.c.send(type, payload)) dropConnection(c, "send queue overflow");
}

void Server::sendChat(Conn& c, const ChatMessage& m) { send(c, kMsgChat, encode(m)); }

void Server::sendServerMsg(Conn& c, const std::string& text) {
    sendChat(c, ChatMessage("MSG", "", text));
}

void Server::broadcastChat(const ChatMessage& m, const Conn* except) {
    std::vector<std::uint8_t> p = encode(m);
    for (Conn* c : loggedIn())
        if (c != except) send(*c, kMsgChat, p);
}

void Server::sendConfigToAll() {
    ConfigChange cc;
    cc.bpm = bpm_;
    cc.bpi = bpi_;
    std::vector<std::uint8_t> p = encode(cc);
    for (Conn* c : loggedIn()) send(*c, kMsgConfigChange, p);
}

void Server::sendUserInfoToOthers(const Conn& about, const UserInfoChange& change) {
    if (about.hidden() || change.records.empty()) return;
    std::vector<std::uint8_t> p = encode(change);
    for (Conn* c : loggedIn())
        if (c != &about) send(*c, kMsgUserInfoChange, p);
}

void Server::closeAfterFlush(Conn& c) {
    c.close_after_flush = true;
    c.close_deadline = Clock::now() + std::chrono::seconds(2);
}

void Server::dropConnection(Conn& c, const std::string& why) {
    if (!c.c.isOpen()) return;
    log("dropping " + (c.logged_in ? c.name : std::to_string(c.id)) + ": " + why);
    if (c.logged_in) removeUser(c);
    c.c.close();
}

// Tells the others that a user has gone and forgets its transfers and votes.
void Server::removeUser(Conn& c) {
    if (!c.logged_in) return;
    c.logged_in = false;
    UserInfoChange change;
    bool had_zero = false;
    for (int i = 0; i < kMaxChannels; ++i) {
        const Channel& ch = c.visible[static_cast<std::size_t>(i)];
        if (!ch.active) continue;
        UserInfoRecord r;
        r.active = false;
        r.index = static_cast<std::uint8_t>(i);
        r.username = c.name;
        r.channel = ch.name;
        r.volume = ch.volume;
        r.pan = ch.pan;
        r.flags = ch.flags;
        change.records.push_back(r);
        if (i == 0) had_zero = true;
    }
    if (!had_zero) {
        UserInfoRecord r;                       // plus channel 0, so users with
        r.index = 0;                            // no channel are removed too
        r.username = c.name;
        change.records.insert(change.records.begin(), r);
    }
    sendUserInfoToOthers(c, change);
    if (!c.hidden()) broadcastChat(ChatMessage("PART", c.name), &c);

    for (auto it = transfers_.begin(); it != transfers_.end();) {
        if (it->second.uploader == c.id) {
            it = transfers_.erase(it);
            continue;
        }
        auto& rcv = it->second.receivers;
        rcv.erase(std::remove(rcv.begin(), rcv.end(), c.id), rcv.end());
        ++it;
    }
    for (auto& setting : votes_) setting.second.erase(c.id);
    roomCodecChanged(nullptr, true);            // announced right after the PART
}

// ---------------------------------------------------------------------- login

void Server::handleMessage(Conn& c, const RawMessage& m) {
    c.c.touchRecv();
    if (!c.logged_in) {
        if (m.type == kMsgClientAuthUser) handleAuthUser(c, m.payload);
        return;                                 // everything else waits for login
    }
    switch (m.type) {
    case kMsgClientSetUserMask: handleSetUserMask(c, m.payload); break;
    case kMsgClientSetChannelInfo: handleSetChannelInfo(c, m.payload); break;
    case kMsgClientUploadBegin: handleUploadBegin(c, m.payload); break;
    case kMsgClientUploadWrite: handleUploadWrite(c, m.payload); break;
    case kMsgChat: handleChat(c, m.payload); break;
    case kMsgKeepalive: break;
    default: break;                             // unknown types are ignored
    }
}

void Server::refuseLogin(Conn& c, const std::string& reason) {
    AuthReply r;
    r.flag = 0;
    r.text = reason;
    r.maxchan = 0;
    send(c, kMsgAuthReply, encode(r));
    log("refusing login " + std::to_string(c.id) + ": " + reason);
    closeAfterFlush(c);
}

const UserAccount* Server::findAccount(const std::string& name) const {
    for (const auto& u : cfg_.users)
        if (iequals(u.name, name)) return &u;
    return nullptr;
}

Server::Conn* Server::findUserExact(const std::string& name) {
    for (Conn* c : loggedIn())
        if (iequals(c->name, name)) return c;
    return nullptr;
}

// Section 9.1 PRIVMSG rule: exact name, or a prefix when the name continues
// with '@' right after it or the prefix itself contains '@'.
Server::Conn* Server::findUserByPrefix(const std::string& prefix, bool& ambiguous) {
    ambiguous = false;
    if (Conn* exact = findUserExact(prefix)) return exact;
    Conn* found = nullptr;
    bool prefix_has_at = prefix.find('@') != std::string::npos;
    for (Conn* c : loggedIn()) {
        if (!istartsWith(c->name, prefix)) continue;
        if (!prefix_has_at && c->name[prefix.size()] != '@') continue;
        if (found) {
            ambiguous = true;
            return nullptr;
        }
        found = c;
    }
    return found;
}

// Section 4.2 anonymous name rewriting.
std::string Server::anonymousName(const std::string& requested, const std::string& addr) const {
    std::string base;
    if (requested.size() > 10 && requested[9] == ':') base = requested.substr(10);
    if (base.empty()) base = "anon";
    else {
        if (base.size() > 16) base.resize(16);
        for (auto& ch : base)
            if (ch == '@' || ch == '.') ch = '_';
    }
    std::string ip = cfg_.mask_addresses ? maskAddress(addr) : addr;
    std::string name = base + "@" + ip;
    for (auto& ch : name) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (!(std::isalnum(u) || ch == '-' || ch == '_' || ch == '@' || ch == '.')) ch = '_';
    }
    if (name.size() > 128) name.resize(128);
    return name;
}

void Server::handleAuthUser(Conn& c, const std::vector<std::uint8_t>& p) {
    AuthUser au;
    if (!decode(p, au)) {
        refuseLogin(c, "invalid authorization reply");
        return;
    }
    if (au.client_version < 0x00020000 || au.client_version > 0x0002ffff) {
        refuseLogin(c, "incorrect client version");
        return;
    }
    if (!cfg_.licence.empty() && !(au.client_caps & kCapLicenceAgreed)) {
        refuseLogin(c, "license not agreed to");
        return;
    }

    std::string name;
    std::uint32_t privs = 0;
    bool anonymous = iequals(au.username, "anonymous") || istartsWith(au.username, "anonymous:");
    if (anonymous) {
        if (!cfg_.allow_anonymous) {
            refuseLogin(c, "invalid login/password");
            return;
        }
        privs = cfg_.anonymous_privs;
        name = anonymousName(au.username, c.addr);
    } else {
        const UserAccount* acct = findAccount(au.username);
        if (!acct || passwordHash(au.username, acct->password, c.challenge) != au.passhash) {
            refuseLogin(c, "invalid login/password");
            return;
        }
        privs = acct->privs;
        name = acct->name;
        if (privs & kPrivStatus) {
            serveStatusLogin(c);
            return;
        }
    }

    if (cfg_.require_codec && (au.client_caps & cfg_.require_codec) != cfg_.require_codec) {
        refuseLogin(c, "this server needs a NINJAM client that plays " + codecName(cfg_.require_codec));
        return;
    }

    // Duplicate names: suffix for "multiple logins", else replace the old one.
    // This runs before the capacity check so that a user reconnecting after a
    // crash is not refused because of their own stale connection (NOTES.md).
    if (findUserExact(name)) {
        if (privs & kPrivMulti) {
            std::string candidate;
            for (int n = 2; n <= 16; ++n) {
                candidate = name + "." + std::to_string(n);
                if (!findUserExact(candidate)) break;
                candidate.clear();
            }
            if (candidate.empty()) {
                refuseLogin(c, "server full");
                return;
            }
            name = candidate;
        } else {
            while (Conn* old = findUserExact(name)) dropConnection(*old, "replaced by a new login");
        }
    }

    // Capacity: the other logged-in, visible users (section 4.3).
    if (static_cast<int>(userCount()) >= cfg_.max_users && !(privs & kPrivReserve)) {
        refuseLogin(c, "server full");
        return;
    }

    finishLogin(c, name, privs, au.client_caps);
}

void Server::finishLogin(Conn& c, const std::string& name, std::uint32_t privs, std::uint32_t caps) {
    c.logged_in = true;
    c.name = name;
    c.privs = privs;
    c.caps = caps;
    c.maxchan = cfg_.max_channels;
    for (auto& ch : c.visible) ch = Channel();
    c.visible[0].active = true;                 // "one empty channel" until SET_CHANNEL_INFO
    c.subs.clear();
    c.has_mask = false;
    log("login " + std::to_string(c.id) + " as " + name + " privs=" + privilegesToLetters(privs));

    // The room codec now includes this client, so its first mask is right;
    // the others hear about a change after the JOIN below. Nothing is
    // announced for the first user of an empty room (8.5).
    bool was_empty = loggedIn().size() == 1;
    std::uint32_t codec_before = effective_codec_;
    effective_codec_ = computeEffectiveCodec();

    AuthReply r;
    r.flag = 1;
    r.text = name;
    r.maxchan = static_cast<std::uint8_t>(c.maxchan);
    send(c, kMsgAuthReply, encode(r));

    ConfigChange cc;
    cc.bpm = bpm_;
    cc.bpi = bpi_;
    send(c, kMsgConfigChange, encode(cc));

    UserInfoChange all;
    for (Conn* o : loggedIn()) {
        if (o == &c || o->hidden()) continue;
        for (int i = 0; i < kMaxChannels; ++i) {
            const Channel& ch = o->visible[static_cast<std::size_t>(i)];
            if (!ch.active) continue;
            UserInfoRecord rec;
            rec.active = true;
            rec.index = static_cast<std::uint8_t>(i);
            rec.volume = ch.volume;
            rec.pan = ch.pan;
            rec.flags = ch.flags;
            rec.username = o->name;
            rec.channel = ch.name;
            all.records.push_back(rec);
        }
    }
    send(c, kMsgUserInfoChange, encode(all));   // sent even when empty

    for (const auto& line : cfg_.motd) sendChat(c, ChatMessage("PRIVMSG", "*", line));
    sendChat(c, ChatMessage("TOPIC", "", topic_));
    if (caps & kCapCodecMask) sendCodecMask(c);

    // Tell the others.
    UserInfoChange join;
    UserInfoRecord rec;
    rec.active = true;
    rec.index = 0;
    rec.username = name;
    join.records.push_back(rec);
    sendUserInfoToOthers(c, join);
    if (!c.hidden()) broadcastChat(ChatMessage("JOIN", name), &c);

    if (effective_codec_ != codec_before) {
        std::uint32_t now = effective_codec_;
        effective_codec_ = codec_before;        // let roomCodecChanged see the change
        roomCodecChanged(&c, !was_empty);
        effective_codec_ = now;
    }
}

// A status account polls the server: it gets the room state and is closed
// without an AUTH_REPLY (section 4.2).
void Server::serveStatusLogin(Conn& c) {
    ConfigChange cc;
    cc.bpm = bpm_;
    cc.bpi = bpi_;
    send(c, kMsgConfigChange, encode(cc));
    UserInfoChange all;
    for (Conn* o : loggedIn()) {
        if (o->hidden()) continue;
        for (int i = 0; i < kMaxChannels; ++i) {
            const Channel& ch = o->visible[static_cast<std::size_t>(i)];
            if (!ch.active) continue;
            UserInfoRecord rec;
            rec.active = true;
            rec.index = static_cast<std::uint8_t>(i);
            rec.volume = ch.volume;
            rec.pan = ch.pan;
            rec.flags = ch.flags;
            rec.username = o->name;
            rec.channel = ch.name;
            all.records.push_back(rec);
        }
    }
    send(c, kMsgUserInfoChange, encode(all));
    sendChat(c, ChatMessage("TOPIC", "", topic_));
    sendChat(c, ChatMessage("USERCOUNT", std::to_string(userCount()), std::to_string(cfg_.max_users)));
    closeAfterFlush(c);
}

// ------------------------------------------------------------------- channels

void Server::handleSetChannelInfo(Conn& c, const std::vector<std::uint8_t>& p) {
    SetChannelInfo sci;
    if (!decode(p, sci)) return;
    std::array<Channel, kMaxChannels> next;
    std::size_t limit = std::min<std::size_t>(sci.channels.size(), static_cast<std::size_t>(c.maxchan));
    bool any_active = false;
    for (std::size_t i = 0; i < limit; ++i) {
        const ChannelInfoRecord& r = sci.channels[i];
        if (!r.active()) continue;
        next[i].active = true;
        next[i].name = r.name;
        next[i].volume = r.volume;
        next[i].pan = r.pan;
        next[i].flags = r.flags;
        any_active = true;
    }
    if (!any_active && !cfg_.allow_hidden_users) next[0].active = true;   // listed with an empty channel 0

    UserInfoChange change;
    for (int i = 0; i < kMaxChannels; ++i) {
        auto idx = static_cast<std::size_t>(i);
        if (next[idx].sameAs(c.visible[idx])) continue;
        UserInfoRecord rec;
        rec.active = next[idx].active;
        rec.index = static_cast<std::uint8_t>(i);
        rec.username = c.name;
        const Channel& src = next[idx].active ? next[idx] : c.visible[idx];
        rec.channel = src.name;
        rec.volume = src.volume;
        rec.pan = src.pan;
        rec.flags = src.flags;
        change.records.push_back(rec);
    }
    c.visible = next;
    sendUserInfoToOthers(c, change);
}

void Server::handleSetUserMask(Conn& c, const std::vector<std::uint8_t>& p) {
    SetUserMask um;
    if (!decode(p, um)) return;
    for (const auto& e : um.entries) {
        std::string key = lower(e.username);
        if (e.mask == 0) {
            c.subs.erase(key);
        } else {
            if (c.subs.size() >= 1024 && !c.subs.count(key)) continue;   // bounded per connection
            c.subs[key] = e.mask;
        }
    }
}

// ------------------------------------------------------------------ intervals

void Server::handleUploadBegin(Conn& c, const std::vector<std::uint8_t>& p) {
    UploadBegin ub;
    if (!decode(p, ub)) return;
    if (ub.channel >= c.maxchan || ub.channel >= kMaxChannels) return;

    DownloadBegin db;
    db.guid = ub.guid;
    db.estsize = ub.estsize;
    db.fourcc = ub.fourcc;
    db.channel = ub.channel;
    db.username = c.name;
    std::vector<std::uint8_t> real = encode(db);

    DownloadBegin silence;                      // zero GUID, fourcc 0
    silence.channel = ub.channel;
    silence.username = c.name;
    std::vector<std::uint8_t> silent = encode(silence);

    bool is_silence = guidIsZero(ub.guid);
    std::uint32_t need = codecCapForFourcc(ub.fourcc);
    bool relayed = need == 0 || (cfg_.allow_codecs & need) != 0;
    std::string key = lower(c.name);

    Transfer t;
    t.uploader = c.id;
    t.began = t.last = Clock::now();
    for (Conn* o : loggedIn()) {
        auto it = o->subs.find(key);
        if (it == o->subs.end()) continue;
        if (!(it->second & (1u << ub.channel))) continue;
        if (is_silence) {
            send(*o, kMsgDownloadBegin, real);
        } else if (!relayed || (need && !(o->caps & need))) {
            send(*o, kMsgDownloadBegin, silent);    // Jamsa: keep them in time
        } else {
            send(*o, kMsgDownloadBegin, real);
            t.receivers.push_back(o->id);
        }
    }
    if (is_silence || ub.fourcc == 0) return;    // nothing to remember

    // Per-user limit: drop the oldest open upload beyond it.
    int open = 0;
    for (const auto& tr : transfers_)
        if (tr.second.uploader == c.id) ++open;
    while (open >= cfg_.max_uploads_per_user) {
        auto oldest = transfers_.end();
        for (auto it = transfers_.begin(); it != transfers_.end(); ++it)
            if (it->second.uploader == c.id && (oldest == transfers_.end() || it->second.began < oldest->second.began))
                oldest = it;
        if (oldest == transfers_.end()) break;
        transfers_.erase(oldest);
        --open;
    }
    transfers_[ub.guid] = std::move(t);
}

void Server::handleUploadWrite(Conn& c, const std::vector<std::uint8_t>& p) {
    IntervalWrite w;
    if (!decode(p, w)) return;
    auto it = transfers_.find(w.guid);
    if (it == transfers_.end()) return;
    if (it->second.uploader != c.id) return;    // only the uploader may write
    it->second.last = Clock::now();
    for (int id : it->second.receivers) {
        Conn* o = connById(id);
        if (o && o->logged_in) send(*o, kMsgDownloadWrite, p);   // payload forwarded untouched
    }
    if (w.last()) transfers_.erase(it);
}

// ----------------------------------------------------------------------- chat

void Server::handleChat(Conn& c, const std::vector<std::uint8_t>& p) {
    ChatMessage m;
    if (!decode(p, m)) return;                  // includes the trailing-NUL rule
    const std::string& cmd = m.fields[0];
    if (cmd == "MSG") {
        if (!(c.privs & kPrivChat)) {
            sendServerMsg(c, "No MSG permission");
            return;
        }
        const std::string& text = m.fields[1];
        if (!text.empty() && text[0] == '!') {
            handleBangCommand(c, text);
            return;
        }
        broadcastChat(ChatMessage("MSG", c.name, text));
    } else if (cmd == "PRIVMSG") {
        if (!(c.privs & kPrivChat)) {
            sendServerMsg(c, "No PRIVMSG permission");
            return;
        }
        bool ambiguous = false;
        Conn* target = findUserByPrefix(m.fields[1], ambiguous);
        if (ambiguous) {
            sendServerMsg(c, "Username is ambiguous (specify partial IP): " + m.fields[1]);
            return;
        }
        if (!target) {
            sendServerMsg(c, "No such user: " + m.fields[1]);
            return;
        }
        sendChat(*target, ChatMessage("PRIVMSG", c.name, m.fields[2]));
    } else if (cmd == "ADMIN") {
        handleAdminCommand(c, m.fields[1]);
    } else if (cmd == "SESSION") {
        broadcastChat(ChatMessage("SESSION", c.name, m.fields[1], m.fields[2], m.fields[3]), &c);
    }
    // Unknown chat commands are ignored.
}

void Server::handleBangCommand(Conn& c, const std::string& text) {
    std::string word, rest;
    splitFirst(text, word, rest);
    word = lower(word);
    if (word == "!topic") {
        sendChat(c, ChatMessage("TOPIC", "", topic_));
    } else if (word == "!vote") {
        std::string setting, value;
        splitFirst(rest, setting, value);
        handleVote(c, lower(setting), value);
    } else if (word == "!codec") {
        // Texts from section 8.5; all are MSG lines with an empty username.
        std::string want = lower(trim(rest));
        std::uint32_t bits = 0;
        bool known = want == "opus" || want == "flac" || want == "vorbis";
        if (want == "opus") bits = kCapOpus;
        else if (want == "flac") bits = kCapFlac;
        if (!known) {
            std::string s = "[server] room audio is " + codecName(effective_codec_);
            Conn* blocker = effective_codec_ != chosen_codec_ ? userLacking(chosen_codec_) : nullptr;
            if (blocker)
                s += " (" + codecName(chosen_codec_) + " was chosen, but " + displayName(blocker) + "'s client can't play it)";
            sendServerMsg(c, s + ". " + codecChoicesText());
            return;
        }
        if (!(c.privs & kPrivVote)) {
            sendServerMsg(c, "[server] you don't have permission to change the room's audio");
            return;
        }
        bool allowed = bits == 0 ? cfg_.require_codec == 0 : (cfg_.allow_codecs & bits) != 0;
        if (!allowed) {
            sendServerMsg(c, "[server] this server doesn't allow " + codecName(bits));
            return;
        }
        chosen_codec_ = bits;
        effective_codec_ = computeEffectiveCodec();
        std::string s;
        if (effective_codec_ == bits) {
            s = "[server] " + c.name + " switched the room's audio to " + codecName(bits);
        } else if (Conn* w = userLacking(bits)) {
            s = "[server] " + c.name + " chose " + codecName(bits) + ", but " + displayName(w) +
                "'s client can't play it, so the room uses " + codecName(effective_codec_) + " until they leave";
        } else {
            s = "[server] " + c.name + " chose " + codecName(bits) + ", but the room uses " + codecName(effective_codec_);
        }
        broadcastChat(ChatMessage("MSG", "", s));
        sendCodecMaskToAll();
    } else if (word == "!bitrate") {
        std::string arg = trim(rest);
        long n = 0;
        bool valid = parseInt(arg, n) && n >= 32 && n <= 510;
        if (arg.empty() || !valid) {
            if (opus_kbps_)
                sendServerMsg(c, "[server] the room's Opus bitrate is " + std::to_string(opus_kbps_) +
                                     " kbps. Change it with !bitrate N, from 32 to 510");
            else
                sendServerMsg(c, "[server] the room has no Opus bitrate: everyone sends their own. "
                                 "Set one with !bitrate N, from 32 to 510");
            return;
        }
        if (!(c.privs & kPrivVote)) {
            sendServerMsg(c, "[server] you don't have permission to change the room's audio");
            return;
        }
        opus_kbps_ = static_cast<std::uint32_t>(n);
        std::string s = "[server] " + c.name + " set the room's Opus bitrate to " + std::to_string(n) + " kbps";
        if (!(effective_codec_ & kCapOpus)) s += ", for when the room plays Opus";
        broadcastChat(ChatMessage("MSG", "", s));
        sendCodecMaskToAll();
    } else {
        sendChat(c, ChatMessage("PRIVMSG", "*",
                                "Unknown !command. Commands available: !vote, !topic, !codec, !bitrate"));
    }
}

void Server::handleVote(Conn& c, const std::string& setting, const std::string& value) {
    // Texts from section 9.4; these three go to the sender only.
    if (cfg_.vote_threshold_percent > 100) {
        sendServerMsg(c, "[voting system] Voting not enabled");
        return;
    }
    if (!(c.privs & kPrivVote)) {
        sendServerMsg(c, "[voting system] No vote permission");
        return;
    }
    long n;
    bool is_bpm = setting == "bpm";
    bool is_bpi = setting == "bpi";
    if (!(is_bpm || is_bpi) || !parseInt(value, n) || (is_bpm && (n < 40 || n > 400)) ||
        (is_bpi && (n < 2 || n > 64))) {
        sendServerMsg(c, "[voting system] !vote requires <bpm|bpi> <value> parameters");
        return;
    }
    TimePoint now = Clock::now();
    auto& votes = votes_[setting];
    votes[c.id] = Vote{static_cast<int>(n), now};

    // Count the live votes per value and find the leader.
    std::map<int, int> tally;
    for (const auto& v : votes)
        if (now - v.second.when <= std::chrono::seconds(cfg_.voting_timeout_seconds)) ++tally[v.second.value];
    int lead_value = static_cast<int>(n), lead_count = 0;
    for (const auto& t : tally)
        if (t.second > lead_count) {
            lead_count = t.second;
            lead_value = t.first;
        }
    std::size_t visible = userCount();
    int needed = static_cast<int>(std::lround(cfg_.vote_threshold_percent * static_cast<double>(visible) / 100.0));
    if (needed < 1) needed = 1;
    std::string label = is_bpm ? "BPM" : "BPI";
    if (lead_count >= needed) {
        if (is_bpm) bpm_ = static_cast<std::uint16_t>(lead_value);
        else bpi_ = static_cast<std::uint16_t>(lead_value);
        votes.clear();
        broadcastChat(ChatMessage("MSG", "", "[voting system] setting " + label + " to " + std::to_string(lead_value)));
        sendConfigToAll();
    } else {
        broadcastChat(ChatMessage("MSG", "",
                                  "[voting system] leading candidate: " + std::to_string(lead_count) + "/" +
                                      std::to_string(needed) + " votes for " + std::to_string(lead_value) + " " + label +
                                      " [each vote expires in " + std::to_string(cfg_.voting_timeout_seconds) + "s]"));
    }
}

void Server::handleAdminCommand(Conn& c, const std::string& text) {
    std::string word, rest;
    splitFirst(text, word, rest);
    word = lower(word);
    if (word == "topic") {
        if (rest.empty()) {
            sendChat(c, ChatMessage("TOPIC", "", topic_));
            return;
        }
        if (!(c.privs & kPrivTopic)) {
            sendServerMsg(c, "No TOPIC permission");
            return;
        }
        topic_ = rest;
        broadcastChat(ChatMessage("TOPIC", c.name, topic_));
    } else if (word == "kick") {
        if (!(c.privs & kPrivKick)) {
            sendServerMsg(c, "No KICK permission");
            return;
        }
        // `name*` kicks every user with that prefix; a plain name kicks the exact one.
        std::vector<Conn*> targets;
        if (!rest.empty() && rest.back() == '*') {
            std::string prefix = rest.substr(0, rest.size() - 1);
            for (Conn* o : loggedIn())
                if (istartsWith(o->name, prefix)) targets.push_back(o);
        } else if (Conn* exact = findUserExact(rest)) {
            targets.push_back(exact);
        }
        if (targets.empty()) {
            sendServerMsg(c, "User \"" + rest + "\" not found!");
            return;
        }
        for (Conn* target : targets) {
            broadcastChat(ChatMessage("MSG", "", "User " + target->name + " kicked by " + c.name));
            removeUser(*target);
            closeAfterFlush(*target);
        }
    } else if (word == "bpm" || word == "bpi") {
        if (!(c.privs & kPrivBpm)) {
            sendServerMsg(c, "No BPM/BPI permission");
            return;
        }
        long n;
        bool is_bpm = word == "bpm";
        if (!parseInt(rest, n) || (is_bpm && (n < 20 || n > 400)) || (!is_bpm && (n < 2 || n > 1024))) {
            sendServerMsg(c, is_bpm ? "BPM parameter must be between 20 and 400"
                                    : "BPI parameter must be between 2 and 1024");
            return;
        }
        if (is_bpm) bpm_ = static_cast<std::uint16_t>(n);
        else bpi_ = static_cast<std::uint16_t>(n);
        broadcastChat(ChatMessage("MSG", "", c.name + " sets " + (is_bpm ? "BPM" : "BPI") + " to " + std::to_string(n)));
        sendConfigToAll();
    } else if (word == "stat") {
        sendChat(c, ChatMessage("PRIVMSG", "*",
                                "users: " + std::to_string(userCount()) + "/" + std::to_string(cfg_.max_users) +
                                    ", bpm " + std::to_string(bpm_) + ", bpi " + std::to_string(bpi_) +
                                    ", audio " + codecName(effective_codec_)));
    } else if (word == "join") {
        sendChat(c, ChatMessage("PRIVMSG", "*", "this server has no private rooms"));
    } else {
        sendServerMsg(c, "Valid commands: /topic, /kick, /bpm, /bpi, /stat, /join");
    }
}

// --------------------------------------------------------- codec negotiation

std::uint32_t Server::computeEffectiveCodec() const {
    std::uint32_t everyone = 0xFFFFFFFFu;
    bool any = false;
    for (const auto& c : conns_) {
        if (!c->logged_in) continue;
        everyone &= c->caps;
        any = true;
    }
    if (!any) return chosen_codec_;
    if (chosen_codec_ == 0) return 0;                       // Vorbis chosen
    if (everyone & chosen_codec_) return chosen_codec_;
    if ((everyone & kCapOpus) && (cfg_.allow_codecs & kCapOpus)) return kCapOpus;
    if ((everyone & kCapFlac) && (cfg_.allow_codecs & kCapFlac)) return kCapFlac;
    return 0;
}

Server::Conn* Server::userLacking(std::uint32_t bits) {
    if (bits == 0) return nullptr;              // everyone plays Vorbis
    for (Conn* c : loggedIn())
        if (!(c->caps & bits)) return c;
    return nullptr;
}

std::string Server::displayName(const Conn* c) const {
    return c->hidden() ? "someone" : c->name;
}

std::string Server::codecChoicesText() const {
    return cfg_.require_codec ? "Change it with !codec opus or !codec flac"
                              : "Change it with !codec opus, !codec flac or !codec vorbis";
}

// Called after a JOIN or PART (section 8.5 announcements).
void Server::roomCodecChanged(const Conn* already_told, bool announce) {
    std::uint32_t next = computeEffectiveCodec();
    if (next == effective_codec_) return;
    effective_codec_ = next;
    if (announce) {
        std::string s;
        if (next == chosen_codec_) {
            s = "[server] room audio is back to " + codecName(chosen_codec_);
        } else if (Conn* w = userLacking(chosen_codec_)) {
            s = "[server] room audio is now " + codecName(next) + ", because " + displayName(w) +
                "'s client can't play " + codecName(chosen_codec_);
        } else {
            s = "[server] room audio is now " + codecName(next);
        }
        broadcastChat(ChatMessage("MSG", "", s));
    }
    sendCodecMaskToAll(already_told);
}

// Only when the codec or bitrate differs from the last mask this client got.
void Server::sendCodecMask(Conn& c) {
    CodecMask cm;
    cm.codecs = effective_codec_;
    cm.opus_kbps = opus_kbps_;
    if (c.has_mask && c.last_mask.codecs == cm.codecs && c.last_mask.opus_kbps == cm.opus_kbps) return;
    c.has_mask = true;
    c.last_mask = cm;
    send(c, kMsgCodecMask, encode(cm));
}

void Server::sendCodecMaskToAll(const Conn* except) {
    for (Conn* c : loggedIn())
        if (c != except && (c->caps & kCapCodecMask)) sendCodecMask(*c);
}

}  // namespace njproto
