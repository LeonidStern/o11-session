// o11proto engine - the NINJAM client engine (ENGINE.md revision 1).
//
// Two contexts: the audio context calls process(); the control context calls
// run() and everything else. See ENGINE.md section 2. Live mode (section 16)
// adds a network thread of its own, which the application never sees.
#ifndef O11PROTO_ENGINE_H
#define O11PROTO_ENGINE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "o11proto/client.h"
#include "o11proto/engine/codec.h"

namespace o11proto {
namespace engine {

enum class Status { Preconnect, Ok, CantConnect, InvalidAuth, Disconnected };
using LicenceAnswer = Client::LicenceDecision;   // Agree, Refuse, Defer

constexpr int kMonoOutput = 1024;                // outChannel flag: one output channel
constexpr int kDefaultPort = 2049;
constexpr int kUploadQueueLimit = 512;
constexpr int kPrebufferBytes = 8192;
constexpr int kFirstWriteThreshold = 8192;
constexpr int kLaterWriteThreshold = 2048;
constexpr int kCrossfadeFrames = 128;
constexpr double kLiveResumeFadeSeconds = 0.02;   // the first interval uploaded after the room left Live fades in
constexpr int kDropoutSlackFrames = 64;          // late data this close to the interval's end is no dropout (8.7)

// Settings of a local channel (ENGINE.md 5.1).
struct LocalChannel {
    std::string name;
    int source = 0;
    bool stereo = false;
    int bitrate = 64;
    bool broadcast = false;
    float volume = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    int outChannel = 0;
};

// Pan law of ENGINE.md 5.4: gains for a stereo pair.
inline void panGains(float pan, float* left, float* right) {
    *left = pan > 0 ? 1.0f - pan : 1.0f;
    *right = pan < 0 ? 1.0f + pan : 1.0f;
}

struct LiveLayer;                                 // Live mode's state (engine/src/live_layer.h)
struct LiveMix;                                   // a user's channel 0 in the Live mixer (same header)

// Interval length of ENGINE.md 4.3, computed exactly as the reference does.
inline int intervalLengthFor(int bpm, int bpi, int sampleRate) {
    return static_cast<int>(bpi / (bpm / 60.0) * sampleRate);
}

// Linear-interpolating playback resampler state (ENGINE.md 8.5), exposed
// for unit tests. Returns how many source frames the piece consumes.
struct PlaybackResampler {
    // The fractional position is kept as a rational number (phase_num / dstRate)
    // so that an interval whose length maps to a whole number of source frames
    // consumes exactly that many: no accumulated rounding, no time slip.
    long phase_num = 0;
    double phase = 0.0;                         // phase_num / dstRate, for interpolation
    void reset() { phase_num = 0; phase = 0.0; }
    int needed(int srcRate, int dstRate, int len) const;
    void advance(int srcRate, int dstRate, int len, int consumed);
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ---- callbacks (invoked from run())
    std::function<LicenceAnswer(const std::string& text)> onLicence;
    std::function<void(const std::array<std::string, 5>& fields)> onChat;
    std::function<void()> onUsersChanged;

    // ---- connection (control context)
    void connect(const std::string& hostport, const std::string& user, const std::string& password);
    void connect(const std::string& host, std::uint16_t port, const std::string& user, const std::string& password);
    void disconnect();
    bool run();                                 // true: the caller may sleep
    void answerLicence(bool agree);
    Status status() const { return status_.load(); }
    std::string errorText() const;
    std::string username() const;
    std::string hostName() const;
    int maxLocalChannels() const { return maxchan_.load(); }

    // ---- audio context
    void process(const float* const* in, int numIn, float* const* out, int numOut, int frames, int sampleRate);
    void setIntervalEnd(int samples);

    // ---- tempo and clock
    int serverBpm() const { return server_bpm_.load(); }
    int serverBpi() const { return server_bpi_.load(); }
    int bpm() const { return active_bpm_.load(); }
    int bpi() const { return active_bpi_.load(); }
    void position(int* pos, int* len) const;
    int usualIntervalLength() const { return usual_len_.load(); }
    bool isIntervalRunning() const { return running_.load() && interval_started_.load(); }
    int loopCount() const { return loop_count_.load(); }
    int sampleRate() const { return sample_rate_.load(); }

    // ---- codecs
    std::uint32_t codecMask() const { return codec_mask_.load(); }
    int roomOpusKbps() const { return room_kbps_.load(); }
    std::uint32_t uploadCodec() const;          // fourcc of the next interval, 0 for none
    int uploadKbps(int index) const;
    void preferFlac(bool v) { prefer_flac_.store(v); }
    bool preferFlac() const { return prefer_flac_.load(); }

    // ---- local channels
    void setLocalChannel(int index, const LocalChannel& settings);
    bool localChannel(int index, LocalChannel* out) const;
    void removeLocalChannel(int index);
    std::vector<int> localChannels() const;
    void notifyServerOfChannelChange();
    float localPeak(int index, int side) const;

    // ---- remote users and channels
    int numUsers() const;
    std::string userName(int user) const;
    float userVolume(int user) const;
    float userPan(int user) const;
    bool userMute(int user) const;
    void setUserVolume(int user, float v);
    void setUserPan(int user, float v);
    void setUserMute(int user, bool v);
    int channelAt(int user, int i) const;       // wire index of the i-th present channel, or -1
    std::string channelName(int user, int ch) const;
    bool channelSubscribed(int user, int ch) const;
    float channelVolume(int user, int ch) const;
    float channelPan(int user, int ch) const;
    bool channelMute(int user, int ch) const;
    bool channelSolo(int user, int ch) const;
    int channelOutChannel(int user, int ch) const;
    int channelFlags(int user, int ch) const;
    float channelPeak(int user, int ch, int side) const;     // what is heard (8.6)
    float channelLevel(int user, int ch, int side) const;    // the same, also while muted or soloed away (8.6)
    long channelDropouts(int user, int ch) const;            // intervals not played in full on time (8.7)
    void setChannelSubscribed(int user, int ch, bool v);
    void setChannelVolume(int user, int ch, float v);
    void setChannelPan(int user, int ch, float v);
    void setChannelMute(int user, int ch, bool v);
    void setChannelSolo(int user, int ch, bool v);
    void setChannelOutChannel(int user, int ch, int v);
    bool hasUserInfoChanged();                  // clears the flag
    void setAutoSubscribe(bool v) { auto_subscribe_.store(v); }
    void setRemoteChannelOffset(int v) { remote_offset_.store(v); }
    void setLocalChannelOffset(int v) { local_offset_.store(v); }
    void autoOutputPairs(int mode, int numOut);
    bool anySolo() const { return any_solo_.load(); }

    // ---- master and metronome
    void setMaster(float volume, float pan, bool mute);
    float masterPeak(int side) const { return master_peak_[side & 1].load(); }
    void setMetronome(float volume, float pan, bool mute, int outChannel);

    // ---- chat
    void say(const std::string& text);
    void privateMessage(const std::string& user, const std::string& text);
    void admin(const std::string& command);
    void sendChat(const std::array<std::string, 5>& fields);

    // ---- Live mode (section 16): real-time audio over UDP on servers that
    // have it, in place of each Live player's channel 0 intervals.
    // Notices come from run(); `error` marks lines shown in the error style.
    std::function<void(const std::string& text, bool error)> onLiveNotice;
    void setLiveEnabled(bool v) { live_enabled_.store(v); }   // announce Live at the next connect() (default on)
    bool liveEnabled() const { return live_enabled_.load(); }
    void setLiveBufferMs(int ms);               // the jitter buffer setting, 5..200 (default 20)
    int liveBufferMs() const { return live_setting_ms_.load(); }
    int liveBufferInUseMs() const;              // B: the setting, raised for long host blocks
    // The saved `live_buffer_ms=` value: false unless a whole number; clamped to 5..200.
    static bool parseLiveBufferSetting(const std::string& value, int* ms);
    bool liveCommand(const std::string& text);  // "/live ...": answered here (true), never sent
    void setLiveHostState(bool playing, bool offline);   // the host's transport and offline rendering
    bool liveRunning() const;                   // the Live client (its UDP socket) runs
    bool userLive(int user) const;              // this user's channel 0 is heard live now
    // The room has one mode for everyone, set by its `!live` switch alone
    // (o11proto/live_room.h): Live, with no interval audio at all (none is
    // uploaded or relayed), or intervals. liveRoomLive() says this engine
    // plays the room Live now (taken at an interval boundary, and false at
    // once when the switch goes off or Live can't play here). A player who
    // can't play Live neither hears nor is heard while the room is Live.
    // Then the server's advice says who has trouble: liveAdviceReason() why
    // (dropouts, or can't play Live), liveAdviceUser() whose (the worst, or
    // the first; "" with no advice) and liveAdviceCount() how many players
    // have trouble, the one named among them (0 with no advice). A UI may
    // suggest switching the room to intervals then; notices say when the
    // advice appears and clears.
    enum class LiveAdvice { None, Dropouts, CantLive };
    bool liveRoomLive() const;
    LiveAdvice liveAdviceReason() const;
    std::string liveAdviceUser() const;
    int liveAdviceCount() const;
    // For a UI, from the control context: the latest SERVER_LIVE says the
    // server has Live with its UDP port open (liveServerHas()) and the
    // room's `!live` switch is on (liveRoomSwitchOn()); liveHeardMs() is
    // about how late this listener hears the Live players (the median of
    // the 6.10 estimates of the slots playing live, rounded; the N of "The
    // room is Live now, about N ms late."), -1 while the room isn't Live or
    // there is no estimate yet.
    bool liveServerHas() const;
    bool liveRoomSwitchOn() const;
    int liveHeardMs() const;
    // For a UI or a log, per user: how full the jitter buffer of this
    // user's Live stream is now (its fill smoothed over 0.5 s, which its
    // speed keeps at the level buffering gave), and how late this listener
    // hears the user now (the one-way estimate of 6.10 with that fill in
    // place of what the buffer held when playback started), in ms; -1
    // while the user's stream isn't playing here (or, for the second,
    // without a round trip yet).
    double liveUserBufferMs(int user) const;
    double liveUserHeardMs(int user) const;
    void setLiveTestLoss(double fraction);      // test hook: drop this share of the datagrams received
    void setLiveTestSendGap(int ms);            // test hook: the Live data sent in the next `ms` is lost
    void setLiveTestSendHold(int ms);           // test hook: ... is held back, then sent all at once
    // Test hook: the frame (counted from this engine's first process()
    // block) where this user's live audio last went into concealment, -1
    // for never.
    long liveTestConcealedFrom(int user) const;
    // For diagnostics: the Live network thread's scheduling and timed waits
    // (live::Transport::timing()), one line.
    std::string liveTestTiming() const;

private:
    struct UploadBlock {
        enum Kind { Audio, End, Silence } kind = Audio;
        int channels = 1;
        int rate = 48000;
        bool fresh = false;                     // after a pause (the room was Live): a new encoder, nothing carried over
        std::vector<float> data;                // interleaved
    };
    struct LocalState {
        LocalChannel s;
        bool transmitting = false;              // audio context
        bool live_paused = false;               // audio context: not transmitting while the room is Live
        int fade_in = 0, fade_len = 0;          // audio context: the interval fades in (frames to go, of)
        std::atomic<float> peak[2]{{0.0f}, {0.0f}};
        std::mutex qm;                          // upload queue
        std::vector<UploadBlock> queue;
        std::vector<std::vector<float>> spare;  // recycled block buffers
        // control-context encoder state
        std::unique_ptr<Encoder> enc;
        bool has_interval = false;
        bool skipped = false;
        bool begin_pending = false;
        Guid guid{};
        std::uint32_t fourcc = 0;
        std::atomic<int> kbps_in_use{0};
    };
    struct Slot {
        bool used = false;
        bool silent = false;
        bool failed = false;                    // a download no decoder could be made for (8.7)
        std::unique_ptr<Decoder> dec;
    };
    struct RemoteChannel {
        bool present = false;
        std::string name;
        std::uint8_t flags = 0;
        bool subscribed = false;
        float volume = 0.25f;
        float pan = 0.0f;
        bool mute = false;
        bool solo = false;
        int outChannel = 0;
        bool has_pair = false;
        std::atomic<float> peak[2]{{0.0f}, {0.0f}};
        std::atomic<float> level[2]{{0.0f}, {0.0f}};    // peak as if not muted
        std::atomic<long> dropouts{0};          // since the channel appeared
        // playback (audio context)
        bool has_current = false;
        std::unique_ptr<Decoder> current;       // null while silent
        Slot slots[2];
        long drop_count = 0;                    // frames a late stream owes, skipped when they arrive (8.5)
        bool audio_expected = false;            // the last interval was audio, so another is due (8.7)
        bool first_audio = false;               // the current interval is audio after none was due
        bool dropped = false;                   // a dropout was counted in this interval
        PlaybackResampler rs;
        std::vector<float> fade;                // interleaved fade-out
        int fade_channels = 0;
        bool ignoresIntervals() const { return (flags & (kChanVoice | kChanSession)) != 0; }
    };
    struct LiveMixDeleter {
        void operator()(LiveMix* p) const;
    };
    struct RemoteUser {
        std::string name;
        float volume = 1.0f;
        float pan = 0.0f;
        bool mute = false;
        std::uint32_t submask = 0;
        std::array<RemoteChannel, kMaxChannels> ch;
        // Live mode: the user's slot as the control context sees it (and
        // its generation; -1 when it has none here or its player can't do
        // Live), the one the mixer plays, taken at a boundary, and channel
        // 0's two paths in the mixer (null until the server has Live).
        int live_want = -1;
        std::uint32_t live_want_gen = 0;
        int live_slot = -1;
        std::uint32_t live_gen = 0;
        std::atomic<bool> live_now{false};
        std::unique_ptr<LiveMix, LiveMixDeleter> live_mix;
    };
    struct Download {
        std::string user;
        int channel = 0;
        std::uint32_t fourcc = 0;
        std::shared_ptr<ByteStream> stream;
        TimePoint last;
        bool playable = false;
    };

    // control
    void wireClient();
    void resetConnectionState();
    void handleUserInfo(const UserInfoRecord& rec);
    void handleDownloadBegin(const DownloadBegin& d);
    void handleDownloadData(const Guid& g, const std::vector<std::uint8_t>& bytes, bool last);
    void makePlayable(const Guid& g, Download& dl);
    void queueInterval(RemoteChannel& rc, Slot&& slot);
    void checkDownloadTimeouts();
    bool uploadLoop();
    void processUploadItem(LocalState& ls, int index, UploadBlock& b);
    void sendWrites(LocalState& ls, int index, bool final);
    std::vector<ChannelInfoRecord> channelList() const;
    void sendChannelList();
    void sendUserMask(RemoteUser& u);
    void discardChannelIntervals(RemoteChannel& rc);
    void recomputeAnySolo();
    void assignOutputPair(RemoteUser& u, RemoteChannel& rc);
    RemoteUser* findUser(const std::string& name);
    RemoteUser* userAt(int i) const;
    void collectGarbage();

    // audio
    void boundary(int sampleRate);
    void uploadBoundary();
    void newIntervalStep();
    void mixRemote(float* const* out, int numOut, int len, int sampleRate);
    void mixLocal(const float* const* in, int numIn, float* const* out, int numOut, int len, int sampleRate);
    void mixMetronome(float* const* out, int numOut, int len, int sampleRate);
    void applyMaster(float* const* out, int numOut, int len, int sampleRate);
    void outputPair(int outChannel, int offset, int numOut, int* idx, bool* mono) const;
    bool isDropout(const RemoteChannel& rc, const Decoder& dec, int srcRate, int sampleRate) const;
    bool clockSteady() const;
    static void countDropout(RemoteChannel& rc);
    static void decayPeak(std::atomic<float>& p, float factor);
    static void raisePeak(std::atomic<float>& p, float v);

    Client client_;
    mutable std::mutex local_mutex_;
    mutable std::mutex remote_mutex_;
    mutable std::mutex text_mutex_;
    std::map<int, std::unique_ptr<LocalState>> locals_;
    std::vector<std::unique_ptr<RemoteUser>> users_;
    std::vector<std::unique_ptr<Decoder>> garbage_;
    std::map<Guid, Download> downloads_;

    std::atomic<Status> status_{Status::Preconnect};
    std::string error_text_, username_, host_name_;
    bool got_challenge_ = false, logged_in_ = false, suppress_ = false, got_config_ = false;
    int messages_ = 0;
    std::atomic<int> maxchan_{0};
    std::atomic<bool> users_changed_{false}, auto_subscribe_{true}, prefer_flac_{true}, any_solo_{false};
    std::atomic<std::uint32_t> codec_mask_{0};
    std::atomic<bool> has_mask_{false};
    std::atomic<int> room_kbps_{0};
    std::atomic<int> remote_offset_{0}, local_offset_{0};
    int auto_pairs_mode_ = 0, auto_pairs_outs_ = 0;

    // clock (audio context, atomics for queries)
    std::atomic<bool> run_request_{false}, running_{false}, interval_started_{false}, tempo_pending_{false};
    std::atomic<int> server_bpm_{0}, server_bpi_{0}, active_bpm_{120}, active_bpi_{32};
    std::atomic<int> interval_pos_{0}, interval_len_{0}, usual_len_{0}, loop_count_{0}, sample_rate_{0};
    int beat_len_ = 0, next_beat_ = 0, beat_index_ = 0, click_pos_ = -1, click_len_ = 0;
    bool click_first_ = false;
    int clock_grace_ = 0;                       // samples left in which dropouts aren't counted (8.7)
    std::atomic<bool> clock_moved_{false};      // setIntervalEnd moved the end of this interval

    // master / metronome settings (atomics: set from control, read in audio)
    std::atomic<float> master_vol_{1.0f}, master_pan_{0.0f};
    std::atomic<bool> master_mute_{false};
    std::atomic<float> master_peak_[2]{{0.0f}, {0.0f}};
    std::atomic<float> metro_vol_{0.5f}, metro_pan_{0.0f};
    std::atomic<bool> metro_mute_{false};
    std::atomic<int> metro_out_{0};

    std::vector<float> mix_tmp_;

    // Live mode (engine_live.cpp, live_layer.cpp).
    struct LiveDeleter {
        void operator()(LiveLayer* p) const;
    };
    void liveInit();
    void liveMessage(const live::ServerLive& m);
    void liveRawMessage(const RawMessage& m);   // the room message (0x08)
    void liveRoomNotices();
    void liveControl();                         // run()
    void liveReset();
    void liveStatus();
    void liveNotice(const std::string& text, bool error);
    double liveOneWayMs(int slot, double* buffer_ms, double* network_ms, double* codec_ms) const;
    void liveAudioBlock(const float* const* in, int numIn, int frames, int sampleRate);   // process()
    void liveBoundary();                        // boundary(), before uploadBoundary() and newIntervalStep()
    bool liveHoldsIntervals() const;            // audio context: the room is Live, so nothing is uploaded
    void mixLive(float* const* out, int numOut, int len, int at, int sampleRate);   // channel 0 of Live users
    void liveIntervalAudio(RemoteUser& u, int len, int sampleRate, float* dst);
    std::atomic<bool> live_enabled_{true};
    std::atomic<int> live_setting_ms_{20};
    std::unique_ptr<LiveLayer, LiveDeleter> live_;   // last: its network thread stops first
};

}  // namespace engine
}  // namespace o11proto

#endif  // O11PROTO_ENGINE_H
