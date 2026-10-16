// njproto - single-threaded NINJAM server.
//
// One poll() loop serves every connection. The server never looks inside
// audio data: it only routes BEGIN/WRITE messages to subscribers. Lobby mode
// and session channels are not implemented (see NOTES.md).
#ifndef NJPROTO_SERVER_H
#define NJPROTO_SERVER_H

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "njproto/connection.h"
#include "njproto/message.h"

namespace njproto {

// Account privileges (section 9.6). The letter is what the tools accept.
enum Privilege : std::uint32_t {
    kPrivTopic = 0x001,      // t  set the topic
    kPrivChat = 0x002,       // c  MSG and PRIVMSG
    kPrivBpm = 0x004,        // b  /bpm and /bpi
    kPrivKick = 0x008,       // k  /kick
    kPrivReserve = 0x010,    // r  may log in when the server is full
    kPrivMulti = 0x020,      // m  several logins with the same name
    kPrivHidden = 0x040,     // h  not shown to others and not counted
    kPrivVote = 0x080,       // v  !vote, !codec, !bitrate
    kPrivSeePrivate = 0x100, // p  see private rooms (lobby mode; unused here)
    kPrivStatus = 0x200,     // s  status account (section 4.2)
    kPrivAll = 0x1BF,        // '*' = everything except hidden and status
};
std::uint32_t parsePrivileges(const std::string& letters);   // "tcbkv" or "*"
std::string privilegesToLetters(std::uint32_t privs);

struct UserAccount {
    std::string name;
    std::string password;
    std::uint32_t privs = kPrivChat | kPrivVote;
};

struct ServerConfig {
    std::uint16_t port = 2049;                 // 0 = any free port (see Server::port())
    std::string bind_address;                  // empty = all interfaces
    bool allow_anonymous = false;
    std::uint32_t anonymous_privs = kPrivChat | kPrivVote;   // add kPrivMulti for "AnonymousUsers 2"
    std::vector<UserAccount> users;
    std::uint16_t bpm = 120;
    std::uint16_t bpi = 8;
    std::string topic;
    std::vector<std::string> motd;             // one PRIVMSG * line each
    int keepalive_seconds = kDefaultKeepaliveSeconds;
    int max_users = 16;
    int max_channels = kMaxChannels;           // maxchan told to each client
    std::string licence;                       // empty = no licence prompt
    bool mask_addresses = false;               // 1.2.3.x in anonymous names
    bool allow_hidden_users = false;           // users with no channel are still listed when false
    // Jamsa codec negotiation (section 8)
    std::uint32_t allow_codecs = kCapOpus;     // codecs relayed besides Vorbis
    std::uint32_t require_codec = 0;           // 0 or kCapOpus / kCapFlac
    std::uint32_t chosen_codec = kCapOpus;     // start value of !codec
    std::uint32_t opus_kbps = 128;             // 32-510; 0 = none, clients use their own
    // voting (section 9.4)
    int vote_threshold_percent = 50;           // > 100 disables voting
    int voting_timeout_seconds = 120;
    // limits
    int login_timeout_seconds = 30;
    int licence_login_timeout_seconds = 120;
    int max_pending_connections = 64;
    int max_pending_per_address = 8;
    int transfer_timeout_seconds = kTransferTimeoutSeconds;
    int max_uploads_per_user = 64;
    std::function<void(const std::string&)> log;   // optional
};

class Server {
public:
    Server() = default;
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const ServerConfig& config);
    void stop();
    bool running() const { return listen_fd_ >= 0; }
    std::uint16_t port() const { return port_; }

    // One iteration of the event loop, waiting up to timeout_ms.
    void poll(int timeout_ms);
    void runFor(int ms);

    // Introspection, mostly for tests and the status line.
    std::size_t userCount() const;              // logged-in, non-hidden
    std::size_t connectionCount() const { return conns_.size(); }
    std::vector<std::string> usernames() const;
    std::uint16_t bpm() const { return bpm_; }
    std::uint16_t bpi() const { return bpi_; }
    const std::string& topic() const { return topic_; }
    std::uint32_t effectiveCodec() const { return effective_codec_; }
    std::uint32_t opusKbps() const { return opus_kbps_; }

private:
    struct Channel {
        bool active = false;
        std::string name;
        std::int16_t volume = 0;
        std::int8_t pan = 0;
        std::uint8_t flags = 0;
        bool sameAs(const Channel& o) const;
    };
    struct Conn {
        Connection c;
        int id = 0;
        std::string addr;
        TimePoint connected_at;
        std::array<std::uint8_t, 8> challenge{};
        bool logged_in = false;
        bool close_after_flush = false;
        TimePoint close_deadline;
        std::string name;                       // effective username
        std::uint32_t privs = 0;
        std::uint32_t caps = 0;                 // client_caps
        int maxchan = 0;
        std::array<Channel, kMaxChannels> visible;     // what the others are told
        std::map<std::string, std::uint32_t> subs;     // lower(username) -> mask
        bool has_mask = false;                         // last CODEC_MASK sent (8.1)
        CodecMask last_mask;
        bool hidden() const { return (privs & kPrivHidden) != 0; }
    };
    struct Transfer {
        int uploader = 0;
        std::vector<int> receivers;             // connection ids
        TimePoint began;
        TimePoint last;
    };
    struct Vote {
        int value = 0;
        TimePoint when;
    };

    void log(const std::string& s) const;
    void acceptPending();
    void handleTimers(TimePoint now);
    void handleMessage(Conn& c, const RawMessage& m);
    void handleAuthUser(Conn& c, const std::vector<std::uint8_t>& p);
    void handleSetChannelInfo(Conn& c, const std::vector<std::uint8_t>& p);
    void handleSetUserMask(Conn& c, const std::vector<std::uint8_t>& p);
    void handleUploadBegin(Conn& c, const std::vector<std::uint8_t>& p);
    void handleUploadWrite(Conn& c, const std::vector<std::uint8_t>& p);
    void handleChat(Conn& c, const std::vector<std::uint8_t>& p);
    void handleBangCommand(Conn& c, const std::string& text);
    void handleAdminCommand(Conn& c, const std::string& text);
    void handleVote(Conn& c, const std::string& setting, const std::string& value);

    void refuseLogin(Conn& c, const std::string& reason);
    void finishLogin(Conn& c, const std::string& name, std::uint32_t privs, std::uint32_t caps);
    void serveStatusLogin(Conn& c);
    std::string anonymousName(const std::string& requested, const std::string& addr) const;
    const UserAccount* findAccount(const std::string& name) const;
    Conn* findUserExact(const std::string& name);
    Conn* findUserByPrefix(const std::string& prefix, bool& ambiguous);
    std::vector<Conn*> loggedIn();
    Conn* connById(int id);

    void send(Conn& c, std::uint8_t type, const std::vector<std::uint8_t>& payload);
    void sendChat(Conn& c, const ChatMessage& m);
    void broadcastChat(const ChatMessage& m, const Conn* except = nullptr);
    void sendServerMsg(Conn& c, const std::string& text);        // MSG "" text
    void dropConnection(Conn& c, const std::string& why);        // immediate
    void closeAfterFlush(Conn& c);
    void removeUser(Conn& c);                                    // PART etc.
    void sendConfigToAll();
    void sendUserInfoToOthers(const Conn& about, const UserInfoChange& change);

    std::uint32_t computeEffectiveCodec() const;
    // Recomputes the effective codec after a join or leave; when it changed,
    // sends the 8.5 announcement to everyone and the mask to codec clients
    // (except `already_told`, which got its mask at login).
    void roomCodecChanged(const Conn* already_told, bool announce);
    Conn* userLacking(std::uint32_t bits);       // someone without these caps
    std::string displayName(const Conn* c) const; // "someone" for hidden users
    std::string codecChoicesText() const;
    void sendCodecMask(Conn& c);
    void sendCodecMaskToAll(const Conn* except = nullptr);
    static std::string codecName(std::uint32_t bits);

    ServerConfig cfg_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    int next_id_ = 1;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::map<Guid, Transfer> transfers_;
    std::uint16_t bpm_ = 120, bpi_ = 8;
    std::string topic_;
    std::uint32_t chosen_codec_ = kCapOpus;
    std::uint32_t effective_codec_ = kCapOpus;
    std::uint32_t opus_kbps_ = 0;
    std::map<std::string, std::map<int, Vote>> votes_;   // "bpm"/"bpi" -> conn id -> vote
};

}  // namespace njproto

#endif  // NJPROTO_SERVER_H
