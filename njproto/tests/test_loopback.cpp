// Client-against-server tests over loopback, following PROTOCOL.md section 13.
// Everything runs in one thread: the harness pumps the server and all clients.
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "njproto/client.h"
#include "njproto/connection.h"
#include "njproto/server.h"
#include "njproto/sha1.h"
#include "socket_compat.h"

using namespace njproto;

// ------------------------------------------------------------------ harness

struct Recorder {
    std::vector<ChatMessage> chats;
    std::vector<UserInfoRecord> infos;
    std::vector<ConfigChange> configs;
    std::vector<CodecMask> masks;
    std::vector<DownloadBegin> begins;
    std::map<Guid, std::vector<std::uint8_t>> data;
    std::vector<std::pair<Guid, bool>> ends;
    std::vector<AuthReply> replies;
    std::vector<std::string> disconnects;

    void attach(Client& c) {
        c.on_chat = [this](const ChatMessage& m) { chats.push_back(m); };
        c.on_userinfo = [this](const UserInfoRecord& r) { infos.push_back(r); };
        c.on_config = [this](const ConfigChange& r) { configs.push_back(r); };
        c.on_codec_mask = [this](const CodecMask& r) { masks.push_back(r); };
        c.on_download_begin = [this](const DownloadBegin& r) { begins.push_back(r); };
        c.on_download_data = [this](const Guid& g, const std::vector<std::uint8_t>& d) {
            auto& v = data[g];
            v.insert(v.end(), d.begin(), d.end());
        };
        c.on_download_end = [this](const Guid& g, bool ok) { ends.emplace_back(g, ok); };
        c.on_auth_reply = [this](const AuthReply& r) { replies.push_back(r); };
        c.on_disconnect = [this](const std::string& s) { disconnects.push_back(s); };
    }
    int countChat(const std::string& cmd, const std::string& f1 = "*", const std::string& f2 = "*") const {
        int n = 0;
        for (const auto& c : chats)
            if (c.fields[0] == cmd && (f1 == "*" || c.fields[1] == f1) && (f2 == "*" || c.fields[2] == f2)) ++n;
        return n;
    }
    bool chatContains(const std::string& cmd, const std::string& needle) const {
        for (const auto& c : chats)
            if (c.fields[0] == cmd && (c.fields[1].find(needle) != std::string::npos ||
                                       c.fields[2].find(needle) != std::string::npos))
                return true;
        return false;
    }
};

struct Harness {
    Server server;
    std::vector<Client*> clients;

    explicit Harness(ServerConfig cfg = ServerConfig()) {
        cfg.port = 0;
        cfg.allow_anonymous = true;
        if (cfg.users.empty()) {
            cfg.users.push_back({"alice", "pw", kPrivAll});
            cfg.users.push_back({"bob", "pw2", kPrivChat | kPrivVote});
        }
        bool ok = server.start(cfg);
        CHECK(ok);
    }
    void add(Client& c) { clients.push_back(&c); }
    void pump(int ms) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(ms);
        do {
            server.poll(2);
            for (Client* c : clients) c->poll(0);
        } while (Clock::now() < deadline);
    }
    bool until(const std::function<bool()>& pred, int timeout_ms = 3000) {
        TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (Clock::now() >= deadline) return false;
            pump(2);
        }
        return true;
    }
    bool login(Client& c, Recorder& rec, const std::string& user, const std::string& pass = "",
               std::uint32_t caps = 0, const std::vector<std::string>& channels = {"ch0"}) {
        rec.attach(c);
        add(c);
        Client::Options o;
        o.username = user;
        o.password = pass;
        o.codec_caps = caps;
        o.auto_subscribe = true;
        std::vector<ChannelInfoRecord> recs;
        for (const auto& n : channels) {
            ChannelInfoRecord r;
            r.name = n;
            recs.push_back(r);
        }
        c.setChannels(recs);
        if (!c.connect("127.0.0.1", server.port(), o)) return false;
        return until([&] { return c.loggedIn() || c.state() == Client::State::Failed; }, 3000) && c.loggedIn();
    }
    bool settle(int ms = 60) {
        pump(ms);
        return true;
    }
};

// A raw peer for tests that need to misbehave at the protocol level.
struct RawPeer {
    Connection c;
    std::vector<RawMessage> received;
    bool closed = false;
    bool connect(std::uint16_t port) {
        compat::startup();
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            compat::closeSocket(fd);
            return false;
        }
        c.adopt(fd);
        return true;
    }
    void poll() {
        if (!c.isOpen()) return;
        if (!c.readAvailable()) {
            closed = true;
            c.close();
            return;
        }
        RawMessage m;
        bool err = false;
        while (c.nextMessage(m, err)) received.push_back(m);
        c.flush();
    }
    const RawMessage* find(std::uint8_t type) const {
        for (const auto& m : received)
            if (m.type == type) return &m;
        return nullptr;
    }
};

static bool pumpRaw(Harness& h, RawPeer& p, const std::function<bool()>& pred, int timeout_ms = 3000) {
    TimePoint deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        h.pump(2);
        p.poll();
    }
    return true;
}

static ServerConfig quickConfig() {
    ServerConfig cfg;
    cfg.keepalive_seconds = 1;
    return cfg;
}

// -------------------------------------------------------------------- tests

TEST(login_anonymous_and_bad_password) {        // acceptance 1
    Harness h(quickConfig());
    Client a;
    Recorder ra;
    CHECK(h.login(a, ra, "anonymous:test"));
    CHECK_EQ(a.username(), "test@127.0.0.1");
    CHECK_EQ(a.maxchan(), 32);
    CHECK(a.hasConfig());
    CHECK_EQ(a.bpm(), 120);
    CHECK_EQ(a.bpi(), 8);
    CHECK_EQ(ra.countChat("TOPIC", ""), 1);
    CHECK_EQ(h.server.userCount(), 1u);

    Client b;
    Recorder rb;
    CHECK(!h.login(b, rb, "alice", "wrong"));
    CHECK(b.state() == Client::State::Failed);
    CHECK_EQ(b.failReason(), "invalid login/password");
    CHECK_EQ(rb.replies.size(), 1u);
    CHECK(!rb.replies[0].success());
    CHECK(h.until([&] { return h.server.connectionCount() == 1; }));  // socket closed

    Client c;
    Recorder rc;
    CHECK(h.login(c, rc, "alice", "pw"));
    CHECK_EQ(c.username(), "alice");
    // An unknown account is refused as well.
    Client d;
    Recorder rd;
    CHECK(!h.login(d, rd, "nobody", "x"));
    CHECK_EQ(d.failReason(), "invalid login/password");
}

TEST(anonymous_name_rewriting) {
    ServerConfig cfg = quickConfig();
    cfg.mask_addresses = true;
    Harness h(cfg);
    Client a, b, c;
    Recorder ra, rb, rc;
    CHECK(h.login(a, ra, "anonymous"));
    CHECK_EQ(a.username(), "anon@127.0.0.x");
    CHECK(h.login(b, rb, "anonymous:a.very.long@name.indeed!!"));
    CHECK_EQ(b.username(), "a_very_long_name@127.0.0.x");        // cut to 16, @ and . -> _
    // Same anonymous name twice: by default the new login replaces the old one.
    CHECK(h.login(c, rc, "anonymous"));
    CHECK_EQ(c.username(), "anon@127.0.0.x");
    CHECK(h.until([&] { return a.state() == Client::State::Failed; }));
    CHECK(h.until([&] { return rb.countChat("PART", "anon@127.0.0.x") == 1; }));
    CHECK(c.loggedIn());

    // With the multiple-logins privilege for anonymous users (--anon-multi)
    // the second login gets ".2".
    ServerConfig multi = quickConfig();
    multi.anonymous_privs |= kPrivMulti;
    Harness h2(multi);
    Client d, e;
    Recorder rd, re;
    CHECK(h2.login(d, rd, "anonymous"));
    CHECK(h2.login(e, re, "anonymous"));
    CHECK_EQ(e.username(), "anon@127.0.0.1.2");
    CHECK(d.loggedIn());
}

TEST(version_licence_and_timeout_refusals) {   // acceptance 2
    ServerConfig cfg = quickConfig();
    cfg.licence = "agree to this";
    cfg.licence_login_timeout_seconds = 1;
    cfg.login_timeout_seconds = 1;
    Harness h(cfg);

    // Old protocol version.
    Client a;
    Recorder ra;
    ra.attach(a);
    h.add(a);
    Client::Options o;
    o.username = "anonymous:v";
    o.client_version = 0x00010000;
    CHECK(a.connect("127.0.0.1", h.server.port(), o));
    CHECK(h.until([&] { return a.state() == Client::State::Failed; }));
    CHECK_EQ(a.failReason(), "incorrect client version");

    // Licence not agreed: a raw peer sends AUTH_USER without bit 0.
    RawPeer p;
    CHECK(p.connect(h.server.port()));
    CHECK(pumpRaw(h, p, [&] { return p.find(kMsgAuthChallenge) != nullptr; }));
    AuthChallenge ch;
    CHECK(decode(p.find(kMsgAuthChallenge)->payload, ch));
    CHECK(ch.has_licence);
    CHECK_EQ(ch.licence, "agree to this");
    AuthUser au;
    au.username = "anonymous:raw";
    au.passhash = passwordHash(au.username, "", ch.challenge);
    au.client_caps = 0;
    p.c.send(kMsgClientAuthUser, encode(au));
    CHECK(pumpRaw(h, p, [&] { return p.find(kMsgAuthReply) != nullptr; }));
    AuthReply r;
    CHECK(decode(p.find(kMsgAuthReply)->payload, r));
    CHECK(!r.success());
    CHECK_EQ(r.text, "license not agreed to");
    CHECK(pumpRaw(h, p, [&] { return p.closed; }));

    // The client side refuses a licence when its callback says no.
    Client b;
    Recorder rb;
    rb.attach(b);
    h.add(b);
    b.on_licence = [](const std::string&) { return false; };
    Client::Options ob;
    ob.username = "anonymous:nolic";
    CHECK(b.connect("127.0.0.1", h.server.port(), ob));
    CHECK(h.until([&] { return b.state() == Client::State::Failed; }));
    CHECK_EQ(b.failReason(), "licence not agreed");

    // No AUTH_USER at all: authorization timeout (shortened to 1 s here).
    RawPeer q;
    CHECK(q.connect(h.server.port()));
    CHECK(pumpRaw(h, q, [&] { return q.find(kMsgAuthReply) != nullptr; }, 4000));
    CHECK(decode(q.find(kMsgAuthReply)->payload, r));
    CHECK_EQ(r.text, "authorization timeout");
    CHECK(pumpRaw(h, q, [&] { return q.closed; }));
}

TEST(licence_agreed_logs_in) {
    ServerConfig cfg = quickConfig();
    cfg.licence = "ok?";
    Harness h(cfg);
    Client a;
    Recorder ra;
    std::string seen;
    a.on_licence = [&](const std::string& t) {
        seen = t;
        return true;
    };
    CHECK(h.login(a, ra, "anonymous:yes"));
    CHECK_EQ(seen, "ok?");
}

TEST(userinfo_join_and_part) {                  // acceptance 3
    Harness h(quickConfig());
    Client a, b;
    Recorder ra, rb;
    CHECK(h.login(a, ra, "anonymous:first", "", 0, {"guitar", "voice"}));
    h.settle();
    CHECK(h.login(b, rb, "anonymous:second"));
    h.settle();
    // b saw first's channels in the login USERINFO.
    CHECK_EQ(b.users().count("first@127.0.0.1"), 1u);
    const auto& u = b.users().at("first@127.0.0.1");
    CHECK(u.channels[0].active);
    CHECK_EQ(u.channels[0].name, "guitar");
    CHECK_EQ(u.channels[1].name, "voice");
    CHECK(!u.channels[2].active);
    // a saw JOIN, the new user's placeholder channel 0 (empty name), and then
    // the single changed record once b's SET_CHANNEL_INFO arrived.
    CHECK_EQ(ra.countChat("JOIN", "second@127.0.0.1"), 1);
    CHECK(h.until([&] { return a.users().count("second@127.0.0.1") == 1 &&
                               a.users().at("second@127.0.0.1").channels[0].name == "ch0"; }));
    std::vector<UserInfoRecord> about_second;
    for (const auto& r : ra.infos)
        if (r.username == "second@127.0.0.1") about_second.push_back(r);
    CHECK_EQ(about_second.size(), 2u);
    CHECK(about_second[0].active && about_second[0].index == 0 && about_second[0].channel == "");
    CHECK(about_second[1].active && about_second[1].index == 0 && about_second[1].channel == "ch0");

    // Changing one channel sends just that record.
    std::size_t before = ra.infos.size();
    std::vector<ChannelInfoRecord> chs(2);
    chs[0].name = "ch0";
    chs[1].name = "extra";
    b.setChannels(chs);
    CHECK(h.until([&] { return ra.infos.size() > before; }));
    h.settle();
    CHECK_EQ(ra.infos.size(), before + 1);
    CHECK_EQ(ra.infos.back().index, 1);
    CHECK_EQ(ra.infos.back().channel, "extra");

    // Removing the channel list entirely: others still see channel 0 (empty name).
    before = ra.infos.size();
    b.setChannels({});
    CHECK(h.until([&] { return ra.infos.size() >= before + 2; }));
    CHECK(a.users().at("second@127.0.0.1").channels[0].active);
    CHECK_EQ(a.users().at("second@127.0.0.1").channels[0].name, "");
    CHECK(!a.users().at("second@127.0.0.1").channels[1].active);

    // Disconnect: PART and inactive records, user removed.
    b.disconnect();
    CHECK(h.until([&] { return ra.countChat("PART", "second@127.0.0.1") == 1; }));
    CHECK(h.until([&] { return a.users().count("second@127.0.0.1") == 0; }));
    CHECK(!ra.infos.back().active);
    CHECK_EQ(h.server.userCount(), 1u);
}

TEST(interval_forwarding_and_subscription) {   // acceptance 4, 5, 6
    ServerConfig cfg = quickConfig();
    cfg.max_channels = 4;
    Harness h(cfg);
    Client up, sub, nosub;
    Recorder ru, rs, rn;
    CHECK(h.login(up, ru, "anonymous:up"));
    CHECK(h.login(sub, rs, "anonymous:sub"));
    CHECK(h.login(nosub, rn, "anonymous:nosub"));
    h.settle();
    nosub.subscribe("up@127.0.0.1", 0);           // explicit unsubscribe
    h.settle();

    std::vector<std::uint8_t> payload(20000);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 31 + 7);
    Guid g = up.beginInterval(1, kFourccVorbis);
    up.writeInterval(g, payload.data(), payload.size(), true);
    CHECK(h.until([&] { return !rs.ends.empty(); }));
    CHECK_EQ(rs.begins.size(), 1u);
    CHECK(rs.begins[0].guid == g);
    CHECK_EQ(rs.begins[0].username, "up@127.0.0.1");
    CHECK_EQ(rs.begins[0].channel, 1);
    CHECK_EQ(rs.begins[0].fourcc, kFourccVorbis);
    CHECK(rs.data[g] == payload);                 // bytes unchanged and in order
    CHECK(rs.ends[0].second);                     // ended with the end flag
    h.settle();
    CHECK(rn.begins.empty());                     // the unsubscribed client got nothing
    CHECK(rn.data.empty());

    // A WRITE from another connection for this GUID is dropped (5).
    Guid g2 = up.beginInterval(0, kFourccVorbis);
    std::vector<std::uint8_t> part = {1, 2, 3};
    up.writeInterval(g2, part.data(), part.size(), false);
    h.settle();
    IntervalWrite forged;
    forged.guid = g2;
    forged.flags = 1;
    forged.data = {9, 9, 9};
    nosub.sendRaw(kMsgClientUploadWrite, encode(forged));
    h.settle();
    CHECK(rs.data[g2] == part);
    CHECK_EQ(rs.ends.size(), 1u);                 // not ended by the forgery
    std::vector<std::uint8_t> rest = {4, 5};
    up.writeInterval(g2, rest.data(), rest.size(), true);
    CHECK(h.until([&] { return rs.ends.size() == 2; }));
    std::vector<std::uint8_t> expect = {1, 2, 3, 4, 5};
    CHECK(rs.data[g2] == expect);

    // Channel 32 and channels beyond maxchan are ignored (6).
    std::size_t begins_before = rs.begins.size();
    UploadBegin bad;
    bad.guid = Client::randomGuid();
    bad.fourcc = kFourccVorbis;
    bad.channel = 32;
    up.sendRaw(kMsgClientUploadBegin, encode(bad));
    bad.channel = 4;                              // maxchan is 4 -> indices 0-3 only
    up.sendRaw(kMsgClientUploadBegin, encode(bad));
    Guid g3 = up.beginInterval(3, kFourccVorbis); // still allowed
    up.endInterval(g3);
    CHECK(h.until([&] { return rs.ends.size() == 3; }));
    CHECK_EQ(rs.begins.size(), begins_before + 1);
    CHECK_EQ(rs.begins.back().channel, 3);

    // Silence BEGIN is relayed with a zero GUID.
    up.sendSilence(0);
    CHECK(h.until([&] { return rs.begins.size() == begins_before + 2; }));
    CHECK(guidIsZero(rs.begins.back().guid));
    CHECK_EQ(rs.begins.back().fourcc, 0u);
    CHECK_EQ(rs.begins.back().username, "up@127.0.0.1");

    // A subscription mask limits channels.
    sub.subscribe("UP@127.0.0.1", 1u << 2);      // case-insensitive, channel 2 only
    h.settle();
    begins_before = rs.begins.size();
    Guid g4 = up.beginInterval(0, kFourccVorbis);
    up.endInterval(g4);
    Guid g5 = up.beginInterval(2, kFourccVorbis);
    up.endInterval(g5);
    CHECK(h.until([&] { return rs.begins.size() > begins_before; }));
    h.settle();
    CHECK_EQ(rs.begins.size(), begins_before + 1);
    CHECK(rs.begins.back().guid == g5);
}

TEST(chat_broadcast_and_malformed) {            // acceptance 7
    ServerConfig cfg = quickConfig();
    cfg.anonymous_privs |= kPrivMulti;            // for the ambiguous-prefix case
    Harness h(cfg);
    Client a, b;
    Recorder ra, rb;
    CHECK(h.login(a, ra, "anonymous:a"));
    CHECK(h.login(b, rb, "anonymous:b"));
    h.settle();
    a.say("hello room");
    CHECK(h.until([&] { return rb.countChat("MSG", "a@127.0.0.1", "hello room") == 1; }));
    CHECK(h.until([&] { return ra.countChat("MSG", "a@127.0.0.1", "hello room") == 1; }));  // sender gets it back

    // Chat without the final NUL is ignored.
    std::vector<std::uint8_t> raw = {'M', 'S', 'G', 0, 'd', 'r', 'o', 'p', 'p', 'e', 'd'};   // no final NUL
    a.sendRaw(kMsgChat, raw);
    h.settle(100);
    CHECK_EQ(rb.countChat("MSG", "*", "dropped"), 0);
    CHECK(a.loggedIn());

    // PRIVMSG with the prefix rule.
    a.privmsg("b", "psst");
    CHECK(h.until([&] { return rb.countChat("PRIVMSG", "a@127.0.0.1", "psst") == 1; }));
    CHECK_EQ(ra.countChat("PRIVMSG", "a@127.0.0.1", "psst"), 0);
    a.privmsg("nobody", "x");
    CHECK(h.until([&] { return ra.countChat("MSG", "", "No such user: nobody") == 1; }));
    Client b2;
    Recorder rb2;
    CHECK(h.login(b2, rb2, "anonymous:b"));
    h.settle();
    CHECK_EQ(b2.username(), "b@127.0.0.1.2");
    a.privmsg("b", "which?");
    CHECK(h.until([&] { return ra.chatContains("MSG", "Username is ambiguous"); }));
    a.privmsg("b@127.0.0.1.2", "exact");
    CHECK(h.until([&] { return rb2.countChat("PRIVMSG", "a@127.0.0.1", "exact") == 1; }));

    // Unknown ! command and !topic.
    a.say("!bogus");
    CHECK(h.until([&] { return ra.chatContains("PRIVMSG", "Unknown !command"); }));
    a.say("!topic");
    CHECK(h.until([&] { return ra.countChat("TOPIC", "") == 2; }));
}

TEST(admin_commands_and_privileges) {
    Harness h(quickConfig());
    Client admin, anon;
    Recorder ra, rn;
    CHECK(h.login(admin, ra, "alice", "pw"));
    CHECK(h.login(anon, rn, "anonymous:x"));
    h.settle();
    anon.admin("topic nope");
    CHECK(h.until([&] { return rn.countChat("MSG", "", "No TOPIC permission") == 1; }));
    anon.admin("bpm 100");
    CHECK(h.until([&] { return rn.countChat("MSG", "", "No BPM/BPI permission") == 1; }));
    anon.admin("kick alice");
    CHECK(h.until([&] { return rn.countChat("MSG", "", "No KICK permission") == 1; }));
    anon.admin("frobnicate");
    CHECK(h.until([&] { return rn.chatContains("MSG", "Valid commands:"); }));

    admin.admin("topic new topic");
    CHECK(h.until([&] { return rn.countChat("TOPIC", "alice", "new topic") == 1; }));
    CHECK_EQ(h.server.topic(), "new topic");
    admin.admin("bpm 90");
    CHECK(h.until([&] { return anon.bpm() == 90; }));
    CHECK(h.until([&] { return rn.countChat("MSG", "", "alice sets BPM to 90") == 1; }));
    admin.admin("bpi 16");
    CHECK(h.until([&] { return anon.bpi() == 16 && admin.bpi() == 16; }));
    admin.admin("bpm 5");                          // out of range
    CHECK(h.until([&] { return ra.countChat("MSG", "", "BPM parameter must be between 20 and 400") == 1; }));
    admin.admin("bpi 1");
    CHECK(h.until([&] { return ra.countChat("MSG", "", "BPI parameter must be between 2 and 1024") == 1; }));
    CHECK_EQ(h.server.bpm(), 90);
    h.settle();
    CHECK(!rn.chatContains("MSG", "parameter must be"));   // sender only

    admin.admin("kick nobody");
    CHECK(h.until([&] { return ra.countChat("MSG", "", "User \"nobody\" not found!") == 1; }));
    admin.admin("kick x*");
    CHECK(h.until([&] { return ra.countChat("MSG", "", "User x@127.0.0.1 kicked by alice") == 1; }));
    CHECK(h.until([&] { return anon.state() == Client::State::Failed; }));
    CHECK(h.until([&] { return ra.countChat("PART", "x@127.0.0.1") == 1; }));
    CHECK(h.until([&] { return h.server.connectionCount() == 1; }));
    CHECK_EQ(h.server.userCount(), 1u);
    CHECK(admin.loggedIn());
}

TEST(voting) {                                   // acceptance 8
    Harness h(quickConfig());
    Client a, b, c;
    Recorder ra, rb, rc;
    CHECK(h.login(a, ra, "anonymous:a"));
    CHECK(h.login(b, rb, "anonymous:b"));
    CHECK(h.login(c, rc, "anonymous:c"));
    h.settle();
    // Three users at 50 % -> 2 votes needed (1.5 rounds to 2).
    a.say("!vote bpm 100");
    CHECK(h.until([&] { return rb.chatContains("MSG", "leading candidate: 1/2 votes for 100 BPM"); }));
    CHECK_EQ(h.server.bpm(), 120);
    b.say("!vote bpm 100");
    CHECK(h.until([&] { return rc.countChat("MSG", "", "[voting system] setting BPM to 100") == 1; }));
    CHECK(h.until([&] { return a.bpm() == 100 && b.bpm() == 100 && c.bpm() == 100; }));
    CHECK_EQ(rc.configs.size(), 2u);
    // Out of range values are refused to the sender only.
    c.say("!vote bpm 999");
    CHECK(h.until([&] { return rc.countChat("MSG", "", "[voting system] !vote requires <bpm|bpi> <value> parameters") == 1; }));
    h.settle();
    CHECK(!ra.chatContains("MSG", "!vote requires"));
    // BPI votes too.
    a.say("!vote bpi 16");
    c.say("!vote bpi 16");
    CHECK(h.until([&] { return b.bpi() == 16; }));
}

TEST(codec_mask_and_silence_substitution) {     // acceptance 9
    ServerConfig cfg = quickConfig();
    cfg.allow_codecs = kCapOpus | kCapFlac;
    Harness h(cfg);
    Client opus, old, flac;
    Recorder ro, rl, rf;
    CHECK(h.login(opus, ro, "anonymous:opus", "", kCapOpus | kCapFlac));
    CHECK(h.until([&] { return ro.masks.size() == 1; }));
    CHECK_EQ(ro.masks[0].codecs, kCapOpus);
    CHECK_EQ(ro.masks[0].opus_kbps, 128u);        // default room bitrate
    CHECK(opus.hasCodecMask());
    h.settle();
    CHECK_EQ(ro.countChat("MSG", ""), 0);         // nothing announced for the first user

    // A client without codec bits moves the room to Vorbis.
    CHECK(h.login(old, rl, "anonymous:old"));
    CHECK(h.until([&] { return ro.masks.size() == 2; }));
    CHECK_EQ(ro.masks[1].codecs, 0u);
    const std::string fell = "[server] room audio is now Vorbis, because old@127.0.0.1's client can't play Opus";
    CHECK(h.until([&] { return ro.countChat("MSG", "", fell) == 1; }));
    CHECK(h.until([&] { return rl.countChat("MSG", "", fell) == 1; }));
    h.settle();
    CHECK(rl.masks.empty());                      // never sent to clients without codec bits
    opus.say("!codec");
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] room audio is Vorbis (Opus was chosen, but old@127.0.0.1's client can't play it). Change it with !codec opus, !codec flac or !codec vorbis") == 1; }));

    // An Opus interval reaches only Opus-capable clients; others get silence.
    Guid g = opus.beginInterval(0, kFourccOpus);
    std::vector<std::uint8_t> bytes = {1, 2, 3, 4};
    opus.writeInterval(g, bytes.data(), bytes.size(), true);
    CHECK(h.until([&] { return !rl.begins.empty(); }));
    CHECK(guidIsZero(rl.begins[0].guid));
    CHECK_EQ(rl.begins[0].fourcc, 0u);
    CHECK_EQ(rl.begins[0].channel, 0);
    CHECK_EQ(rl.begins[0].username, "opus@127.0.0.1");
    h.settle();
    CHECK(rl.data.empty());
    // A Vorbis interval reaches everyone.
    Guid g2 = opus.beginInterval(0, kFourccVorbis);
    opus.writeInterval(g2, bytes.data(), bytes.size(), true);
    CHECK(h.until([&] { return rl.ends.size() == 1; }));
    CHECK(rl.data[g2] == bytes);

    // When the old client leaves the room goes back to Opus.
    old.disconnect();
    CHECK(h.until([&] { return ro.masks.size() == 3; }));
    CHECK_EQ(ro.masks[2].codecs, kCapOpus);
    CHECK_EQ(ro.countChat("MSG", "", "[server] room audio is back to Opus"), 1);

    // A FLAC-only client joins: Opus is no longer common to everyone, FLAC is
    // (and the server relays it), so the room falls to FLAC.
    CHECK(h.login(flac, rf, "anonymous:flac", "", kCapFlac));
    CHECK(h.until([&] { return ro.masks.size() == 4; }));
    CHECK_EQ(ro.masks[3].codecs, kCapFlac);
    CHECK(h.until([&] { return rf.masks.size() == 1 && rf.masks[0].codecs == kCapFlac; }));
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] room audio is now FLAC, because flac@127.0.0.1's client can't play Opus") == 1; }));
    // An Opus interval now: the FLAC client gets silence.
    Guid g3 = opus.beginInterval(0, kFourccOpus);
    opus.writeInterval(g3, bytes.data(), bytes.size(), true);
    CHECK(h.until([&] { return !rf.begins.empty(); }));
    CHECK(guidIsZero(rf.begins[0].guid));
    flac.disconnect();
    CHECK(h.until([&] { return ro.masks.size() == 5 && ro.masks[4].codecs == kCapOpus; }));

    // !codec vorbis is a choice; !codec opus switches back; the chat line
    // comes before the mask.
    opus.say("!codec vorbis");
    CHECK(h.until([&] { return ro.masks.size() == 6 && ro.masks[5].codecs == 0; }));
    CHECK_EQ(ro.countChat("MSG", "", "[server] opus@127.0.0.1 switched the room's audio to Vorbis"), 1);
    opus.say("!codec opus");
    CHECK(h.until([&] { return ro.masks.size() == 7 && ro.masks[6].codecs == kCapOpus; }));
    CHECK_EQ(ro.countChat("MSG", "", "[server] opus@127.0.0.1 switched the room's audio to Opus"), 1);
    // A !codec that leaves the effective codec unchanged: chat line, no mask.
    opus.say("!codec opus");
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] opus@127.0.0.1 switched the room's audio to Opus") == 2; }));
    h.settle();
    CHECK_EQ(ro.masks.size(), 7u);
    opus.say("!bitrate 128");                      // same bitrate: no mask either
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] opus@127.0.0.1 set the room's Opus bitrate to 128 kbps") == 1; }));
    h.settle();
    CHECK_EQ(ro.masks.size(), 7u);
    opus.say("!codec");
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] room audio is Opus. Change it with !codec opus, !codec flac or !codec vorbis") == 1; }));
    opus.say("!codec mp3");                        // unknown argument: same answer
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] room audio is Opus. Change it with !codec opus, !codec flac or !codec vorbis") == 2; }));

    // Choosing a codec that someone present can't play.
    Client old2;
    Recorder rl2;
    CHECK(h.login(old2, rl2, "anonymous:old2"));
    h.settle();
    opus.say("!codec flac");
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] opus@127.0.0.1 chose FLAC, but old2@127.0.0.1's client can't play it, so the room uses Vorbis until they leave") == 1; }));
    CHECK_EQ(h.server.effectiveCodec(), 0u);
    old2.disconnect();
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] room audio is back to FLAC") == 1; }));

    // Without the vote privilege the command is refused.
    ServerConfig cfg2 = quickConfig();
    cfg2.users.push_back({"novote", "n", kPrivChat});
    Harness h2(cfg2);
    Client nv;
    Recorder rnv;
    CHECK(h2.login(nv, rnv, "novote", "n", kCapOpus));
    nv.say("!codec flac");
    CHECK(h2.until([&] { return rnv.countChat("MSG", "", "[server] you don't have permission to change the room's audio") == 1; }));
    nv.say("!bitrate 64");
    CHECK(h2.until([&] { return rnv.countChat("MSG", "", "[server] you don't have permission to change the room's audio") == 2; }));
    CHECK_EQ(h2.server.opusKbps(), 128u);
}

TEST(require_codec) {
    ServerConfig cfg = quickConfig();
    cfg.require_codec = kCapOpus;
    cfg.allow_codecs = kCapOpus | kCapFlac;
    Harness h(cfg);
    Client old, good;
    Recorder ro, rg;
    CHECK(!h.login(old, ro, "anonymous:old"));
    CHECK_EQ(old.failReason(), "this server needs a NINJAM client that plays Opus");
    CHECK(h.login(good, rg, "anonymous:good", "", kCapOpus));
    CHECK(h.until([&] { return rg.masks.size() == 1 && rg.masks[0].codecs == kCapOpus; }));
    good.say("!codec vorbis");
    CHECK(h.until([&] { return rg.countChat("MSG", "", "[server] this server doesn't allow Vorbis") == 1; }));
    CHECK_EQ(h.server.effectiveCodec(), kCapOpus);
    good.say("!codec");
    CHECK(h.until([&] { return rg.countChat("MSG", "", "[server] room audio is Opus. Change it with !codec opus or !codec flac") == 1; }));
    // Switching between the allowed codecs is still possible under RequireCodec.
    good.say("!codec flac");
    CHECK(h.until([&] { return rg.countChat("MSG", "", "[server] good@127.0.0.1 chose FLAC, but good@127.0.0.1's client can't play it, so the room uses Opus until they leave") == 1; }));
    Client both;
    Recorder rb;
    CHECK(h.login(both, rb, "anonymous:both", "", kCapOpus | kCapFlac));
    both.say("!codec flac");
    CHECK(h.until([&] { return rb.countChat("MSG", "", "[server] both@127.0.0.1 chose FLAC, but good@127.0.0.1's client can't play it, so the room uses Opus until they leave") == 1; }));
    good.disconnect();
    CHECK(h.until([&] { return rb.countChat("MSG", "", "[server] room audio is back to FLAC") == 1; }));
    CHECK_EQ(h.server.effectiveCodec(), kCapFlac);

    // A server that relays nothing beyond Vorbis: mask 0 for everyone.
    ServerConfig none = quickConfig();
    none.allow_codecs = 0;
    Harness h2(none);
    Client v;
    Recorder rv;
    CHECK(h2.login(v, rv, "anonymous:v", "", kCapOpus | kCapFlac));
    CHECK(h2.until([&] { return rv.masks.size() == 1; }));
    CHECK_EQ(rv.masks[0].codecs, 0u);
    v.say("!codec opus");
    CHECK(h2.until([&] { return rv.countChat("MSG", "", "[server] this server doesn't allow Opus") == 1; }));
}

TEST(bitrate) {                                  // acceptance 10
    ServerConfig cfg = quickConfig();
    cfg.opus_kbps = 96;
    Harness h(cfg);
    Client a, b, old;
    Recorder ra, rb, ro;
    CHECK(h.login(a, ra, "anonymous:a", "", kCapOpus));
    CHECK(h.login(b, rb, "anonymous:b", "", kCapOpus | kCapFlac));
    CHECK(h.login(old, ro, "anonymous:old"));
    CHECK(h.until([&] { return ra.masks.size() >= 1 && ra.masks[0].opus_kbps == 96; }));
    a.say("!bitrate 128");
    CHECK(h.until([&] { return a.codecMask().opus_kbps == 128 && b.codecMask().opus_kbps == 128; }));
    CHECK_EQ(h.server.opusKbps(), 128u);
    // The room plays Vorbis (old client present), hence the "for when" wording.
    CHECK(h.until([&] { return ro.countChat("MSG", "", "[server] a@127.0.0.1 set the room's Opus bitrate to 128 kbps, for when the room plays Opus") == 1; }));
    std::size_t na = ra.masks.size(), nb = rb.masks.size();
    const std::string status = "[server] the room's Opus bitrate is 128 kbps. Change it with !bitrate N, from 32 to 510";
    a.say("!bitrate 9999");
    CHECK(h.until([&] { return ra.countChat("MSG", "", status) == 1; }));
    h.settle();
    CHECK(!rb.chatContains("MSG", "Change it with !bitrate"));
    CHECK_EQ(ra.masks.size(), na);
    CHECK_EQ(rb.masks.size(), nb);
    CHECK(ro.masks.empty());
    b.say("!bitrate");
    CHECK(h.until([&] { return rb.countChat("MSG", "", status) == 1; }));
    old.disconnect();
    CHECK(h.until([&] { return h.server.effectiveCodec() == kCapOpus; }));
    b.say("!bitrate 64");
    CHECK(h.until([&] { return ra.countChat("MSG", "", "[server] b@127.0.0.1 set the room's Opus bitrate to 64 kbps") == 1; }));
    CHECK(h.until([&] { return a.codecMask().opus_kbps == 64; }));

    // No bitrate configured at all.
    ServerConfig none = quickConfig();
    none.opus_kbps = 0;
    Harness h2(none);
    Client n;
    Recorder rn;
    CHECK(h2.login(n, rn, "anonymous:n", "", kCapOpus));
    CHECK(h2.until([&] { return rn.masks.size() == 1 && rn.masks[0].opus_kbps == 0; }));
    n.say("!bitrate");
    CHECK(h2.until([&] { return rn.countChat("MSG", "", "[server] the room has no Opus bitrate: everyone sends their own. Set one with !bitrate N, from 32 to 510") == 1; }));
    // An out-of-range configured value falls back to 128.
    ServerConfig bad = quickConfig();
    bad.opus_kbps = 1000;
    Harness h3(bad);
    CHECK_EQ(h3.server.opusKbps(), 128u);
}

TEST(keepalive_drop) {                           // acceptance 11
    Harness h(quickConfig());                     // k = 1 s -> drop after 3 s
    Client quiet, chatty;
    Recorder rq, rc;
    CHECK(h.login(chatty, rc, "anonymous:chatty"));
    rq.attach(quiet);
    h.add(quiet);
    Client::Options o;
    o.username = "anonymous:quiet";
    o.send_keepalives = false;
    CHECK(quiet.connect("127.0.0.1", h.server.port(), o));
    CHECK(h.until([&] { return quiet.loggedIn(); }));
    CHECK_EQ(quiet.keepaliveSeconds(), 1);
    TimePoint t0 = Clock::now();
    CHECK(h.until([&] { return quiet.state() == Client::State::Failed; }, 6000));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(elapsed > 2000 && elapsed < 5000);
    CHECK(chatty.loggedIn());                     // the one sending keepalives stays
    CHECK(h.until([&] { return rc.countChat("PART", "quiet@127.0.0.1") == 1; }));
    h.pump(1500);
    CHECK(chatty.loggedIn());
}

TEST(server_full_reserve_and_replacement) {
    ServerConfig cfg = quickConfig();
    cfg.max_users = 1;
    cfg.users.push_back({"vip", "v", kPrivChat | kPrivReserve});
    cfg.users.push_back({"solo", "s", kPrivChat});
    Harness h(cfg);
    Client a, b, vip;
    Recorder ra, rb, rv;
    CHECK(h.login(a, ra, "anonymous:a"));
    CHECK(!h.login(b, rb, "anonymous:b"));
    CHECK_EQ(b.failReason(), "server full");
    CHECK(h.login(vip, rv, "vip", "v"));          // reserved slot gets in
    CHECK_EQ(h.server.userCount(), 2u);

    // Without the multi-login privilege a second login replaces the first.
    vip.disconnect();
    a.disconnect();
    CHECK(h.until([&] { return h.server.userCount() == 0; }));
    Client s1, s2;
    Recorder r1, r2;
    CHECK(h.login(s1, r1, "solo", "s"));
    CHECK(h.login(s2, r2, "solo", "s"));
    CHECK(h.until([&] { return s1.state() == Client::State::Failed; }));
    CHECK(s2.loggedIn());
    CHECK_EQ(h.server.userCount(), 1u);
}

TEST(pending_connection_limit) {
    ServerConfig cfg = quickConfig();
    cfg.max_pending_per_address = 2;
    Harness h(cfg);
    RawPeer p1, p2, p3;
    CHECK(p1.connect(h.server.port()));
    CHECK(p2.connect(h.server.port()));
    CHECK(pumpRaw(h, p1, [&] { return p1.find(kMsgAuthChallenge) != nullptr; }));
    CHECK(pumpRaw(h, p2, [&] { return p2.find(kMsgAuthChallenge) != nullptr; }));
    CHECK(p3.connect(h.server.port()));
    CHECK(pumpRaw(h, p3, [&] { return p3.closed; }));
    CHECK(p3.received.empty());
    CHECK_EQ(h.server.connectionCount(), 2u);
}

TEST(protocol_errors_close_connection) {
    Harness h(quickConfig());
    Client a;
    Recorder ra;
    CHECK(h.login(a, ra, "anonymous:a"));
    RawPeer p;
    CHECK(p.connect(h.server.port()));
    CHECK(pumpRaw(h, p, [&] { return p.find(kMsgAuthChallenge) != nullptr; }));
    // Oversized payload length in the header.
    std::vector<std::uint8_t> bad = {0x80, 0x01, 0x40, 0x00, 0x00};
    compat::send(p.c.fd(), bad.data(), bad.size(), 0);
    CHECK(pumpRaw(h, p, [&] { return p.closed; }));
    CHECK(a.loggedIn());
    // Messages before login other than AUTH_USER are ignored, not fatal.
    RawPeer q;
    CHECK(q.connect(h.server.port()));
    CHECK(pumpRaw(h, q, [&] { return q.find(kMsgAuthChallenge) != nullptr; }));
    q.c.send(kMsgChat, encode(ChatMessage("MSG", "early")));
    h.pump(100);
    q.poll();
    CHECK(!q.closed);
    CHECK_EQ(ra.countChat("MSG", "*", "early"), 0);
}

TEST(status_account_and_motd) {
    ServerConfig cfg = quickConfig();
    cfg.users.push_back({"status", "st", kPrivStatus});
    cfg.motd = {"welcome", "enjoy"};
    cfg.topic = "jam";
    Harness h(cfg);
    Client a;
    Recorder ra;
    CHECK(h.login(a, ra, "anonymous:a"));
    CHECK_EQ(ra.countChat("PRIVMSG", "*", "welcome"), 1);
    CHECK_EQ(ra.countChat("PRIVMSG", "*", "enjoy"), 1);
    CHECK_EQ(ra.countChat("TOPIC", "", "jam"), 1);

    Client st;
    Recorder rs;
    CHECK(!h.login(st, rs, "status", "st"));      // closed without AUTH_REPLY
    CHECK(rs.replies.empty());
    CHECK_EQ(rs.countChat("USERCOUNT", "1", "16"), 1);
    CHECK_EQ(rs.configs.size(), 1u);
    CHECK_EQ(rs.countChat("TOPIC", "", "jam"), 1);
    CHECK(h.until([&] { return h.server.connectionCount() == 1; }));
}

TEST(transfer_limits_and_timeouts) {
    ServerConfig cfg = quickConfig();
    cfg.max_uploads_per_user = 2;
    cfg.transfer_timeout_seconds = 1;
    Harness h(cfg);
    Client up, sub;
    Recorder ru, rs;
    CHECK(h.login(up, ru, "anonymous:up"));
    CHECK(h.login(sub, rs, "anonymous:sub"));
    h.settle();
    Guid g1 = up.beginInterval(0, kFourccVorbis);
    Guid g2 = up.beginInterval(0, kFourccVorbis);
    Guid g3 = up.beginInterval(0, kFourccVorbis);   // g1 is dropped (oldest of 3)
    h.settle();
    std::vector<std::uint8_t> d = {1};
    up.writeInterval(g1, d.data(), d.size(), true);
    up.writeInterval(g2, d.data(), d.size(), true);
    up.writeInterval(g3, d.data(), d.size(), true);
    CHECK(h.until([&] { return rs.ends.size() == 2; }));
    h.settle();
    CHECK_EQ(rs.ends.size(), 2u);
    CHECK(rs.data.count(g1) == 0);
    // An idle transfer is forgotten after the timeout; its later WRITE is dropped.
    Guid g4 = up.beginInterval(0, kFourccVorbis);
    h.pump(1500);
    up.writeInterval(g4, d.data(), d.size(), true);
    h.settle(100);
    CHECK(rs.data.count(g4) == 0);
    CHECK_EQ(rs.ends.size(), 2u);
}

TEST(session_relay_and_hidden_user) {
    ServerConfig cfg = quickConfig();
    cfg.users.push_back({"ghost", "g", kPrivChat | kPrivHidden});
    Harness h(cfg);
    Client a, b, ghost;
    Recorder ra, rb, rg;
    CHECK(h.login(a, ra, "anonymous:a", "", kCapOpus));
    CHECK(h.login(ghost, rg, "ghost", "g"));
    CHECK(h.login(b, rb, "anonymous:b"));
    h.settle();
    CHECK_EQ(h.server.userCount(), 2u);
    CHECK_EQ(ra.countChat("JOIN", "ghost"), 0);
    // A hidden user is named "someone" in codec announcements.
    CHECK_EQ(ra.countChat("MSG", "", "[server] room audio is now Vorbis, because someone's client can't play Opus"), 1);
    CHECK_EQ(b.users().count("ghost"), 0u);
    CHECK_EQ(b.users().count("a@127.0.0.1"), 1u);
    ghost.say("boo");
    CHECK(h.until([&] { return ra.countChat("MSG", "ghost", "boo") == 1; }));   // chat still works
    a.sendChat(ChatMessage("SESSION", "00112233445566778899aabbccddeeff", "0", "12.5 2.0"));
    CHECK(h.until([&] { return rb.countChat("SESSION", "a@127.0.0.1") == 1; }));
    CHECK_EQ(rb.chats.back().fields[2], "00112233445566778899aabbccddeeff");
    CHECK_EQ(rb.chats.back().fields[4], "12.5 2.0");
    h.settle();
    CHECK_EQ(ra.countChat("SESSION"), 0);
}

TEST_MAIN()
