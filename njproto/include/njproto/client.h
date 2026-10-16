// njproto - non-blocking NINJAM client state machine.
//
// The caller drives it from a loop with poll(); every incoming message is
// delivered through a callback. Audio encoding is out of scope: the caller
// hands in opaque encoded bytes per interval and receives the bytes of
// other users' intervals as they arrive.
#ifndef NJPROTO_CLIENT_H
#define NJPROTO_CLIENT_H

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "njproto/connection.h"
#include "njproto/message.h"

namespace njproto {

class Client {
public:
    enum class State {
        Disconnected,       // nothing open
        Connecting,         // TCP connect in progress
        AwaitingChallenge,  // connected, waiting for AUTH_CHALLENGE
        AwaitingReply,      // AUTH_USER sent, waiting for AUTH_REPLY
        LoggedIn,
        Failed,             // closed after a refused login or an error (see failReason())
    };

    struct Options {
        std::string username;
        std::string password;
        std::uint32_t codec_caps = 0;                  // kCapOpus | kCapFlac
        std::uint32_t client_version = kProtocolVersion;
        bool agree_licence = true;                     // default answer when on_licence is unset
        bool send_keepalives = true;                   // false only for tests
        bool auto_subscribe = false;                   // subscribe to every user seen (mask ~0)
    };

    struct RemoteChannel {
        bool active = false;
        std::string name;
        std::int16_t volume = 0;
        std::int8_t pan = 0;
        std::uint8_t flags = 0;
    };
    struct RemoteUser {
        std::string name;
        std::array<RemoteChannel, kMaxChannels> channels;
        bool anyActive() const;
    };

    // Callbacks. All optional. They are invoked from inside poll().
    std::function<bool(const std::string& licence)> on_licence;   // return true to agree
    std::function<void(const AuthChallenge&)> on_challenge;
    std::function<void(const AuthReply&)> on_auth_reply;
    std::function<void(const ConfigChange&)> on_config;
    std::function<void(const UserInfoRecord&)> on_userinfo;      // one call per record
    std::function<void(const DownloadBegin&)> on_download_begin;
    std::function<void(const Guid&, const std::vector<std::uint8_t>&)> on_download_data;
    std::function<void(const Guid&, bool complete)> on_download_end;  // false = timed out
    std::function<void(const ChatMessage&)> on_chat;
    std::function<void(const CodecMask&)> on_codec_mask;
    std::function<void(const RawMessage&)> on_message;           // every message, before decoding
    std::function<void(const std::string& reason)> on_disconnect;

    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Starts a non-blocking connect. Returns false if the address can't be
    // resolved or no socket can be made.
    bool connect(const std::string& host, std::uint16_t port, const Options& options);
    void disconnect(const std::string& reason = "closed by caller");

    // One iteration of the event loop: waits up to timeout_ms for socket
    // activity, then processes it and the timers.
    void poll(int timeout_ms);
    // Blocking helpers for tools and tests: poll until pred() is true (returns
    // true) or the timeout passes (returns false); or for a fixed time.
    bool waitFor(const std::function<bool()>& pred, int timeout_ms);
    void runFor(int ms);

    State state() const { return state_; }
    bool loggedIn() const { return state_ == State::LoggedIn; }
    const std::string& username() const { return username_; }   // effective name after login
    int maxchan() const { return maxchan_; }
    std::uint16_t bpm() const { return bpm_; }
    std::uint16_t bpi() const { return bpi_; }
    bool hasConfig() const { return bpm_ != 0; }
    bool hasCodecMask() const { return has_codec_mask_; }
    const CodecMask& codecMask() const { return codec_mask_; }
    const std::map<std::string, RemoteUser>& users() const { return users_; }
    const std::string& failReason() const { return fail_reason_; }
    int keepaliveSeconds() const { return keepalive_seconds_; }

    // Sends (or, before login, remembers) the full channel list (section 6.1).
    void setChannels(const std::vector<ChannelInfoRecord>& channels);
    const std::vector<ChannelInfoRecord>& channels() const { return channels_; }
    // Subscribes to the channels in `mask` of `username`; 0 unsubscribes.
    void subscribe(const std::string& username, std::uint32_t mask);

    // Intervals (section 7). beginInterval makes a random GUID and sends the
    // BEGIN; writeInterval splits the bytes into WRITEs of at most 9216 bytes
    // and sets the end flag on the last one when `last` is true; endInterval
    // sends an empty final WRITE; sendSilence sends a zero-GUID BEGIN.
    Guid beginInterval(std::uint8_t channel, std::uint32_t fourcc, std::uint32_t estsize = 0);
    void writeInterval(const Guid& guid, const std::uint8_t* data, std::size_t len, bool last);
    void endInterval(const Guid& guid);
    void sendSilence(std::uint8_t channel);

    // Chat (section 9).
    void say(const std::string& text);                            // MSG
    void privmsg(const std::string& user, const std::string& text);
    void admin(const std::string& command);                       // ADMIN, no slash
    void sendChat(const ChatMessage& m);

    // Picks the codec for the next interval from the latest mask (8.3).
    std::uint32_t preferredFourcc(bool want_flac = false) const;

    bool sendRaw(std::uint8_t type, const std::vector<std::uint8_t>& payload);

    static Guid randomGuid();

private:
    void fail(const std::string& reason);
    void handleMessage(const RawMessage& m);
    void handleChallenge(const std::vector<std::uint8_t>& p);
    void handleAuthReply(const std::vector<std::uint8_t>& p);
    void handleUserInfo(const std::vector<std::uint8_t>& p);
    void handleTimers(TimePoint now);
    void sendChannels();

    Connection conn_;
    Options opt_;
    State state_ = State::Disconnected;
    std::string username_;
    std::string fail_reason_;
    int maxchan_ = 0;
    int keepalive_seconds_ = kDefaultKeepaliveSeconds;
    bool licence_pending_ = false;
    std::uint16_t bpm_ = 0, bpi_ = 0;
    bool has_codec_mask_ = false;
    CodecMask codec_mask_;
    std::vector<ChannelInfoRecord> channels_;
    std::map<std::string, RemoteUser> users_;
    std::map<Guid, TimePoint> downloads_;   // open downloads, last activity
};

}  // namespace njproto

#endif  // NJPROTO_CLIENT_H
