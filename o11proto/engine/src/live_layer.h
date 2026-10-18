// o11proto engine - Live mode's state and its network side (live-mode spec
// sections 5 and 6, ENGINE.md section 16). Internal to the engine.
//
// Three threads share it:
// - the control context (Engine::run()) applies each 0x07, starts and stops
//   the transport, hands out the slots and works out their health;
// - the transport's network thread runs the source (encoding, /start,
//   /stop, pings, resends) and the network side of the eight sinks (the
//   receive filter, pongs, start and stop requests, timeouts);
// - the audio context (Engine::process()) captures channel 0 for the
//   source and plays the sinks (live_sink.h).
// The control context and the network thread share `net_mutex`; neither is
// real time. The audio context never locks: it talks to the others through
// the queues and atomics below.
#ifndef O11PROTO_ENGINE_LIVE_LAYER_H
#define O11PROTO_ENGINE_LIVE_LAYER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "live_aoo.h"
#include "live_sink.h"
#include "o11proto/connection.h"
#include "o11proto/live.h"
#include "o11proto/live_room.h"
#include "o11proto/live_transport.h"

struct OpusEncoder;

namespace o11proto {
namespace engine {

constexpr int kLiveSlots = 8;
constexpr int kLiveListed = 64;
constexpr int kLiveAutoBufferMs = 10;        // the automatic buffer (the default): where each stream starts,
constexpr int kLiveFallbackBufferMs = 20;    // and where it falls back to on late audio
constexpr int kLiveMinBufferMs = 5;
constexpr int kLiveMaxBufferMs = 200;
constexpr int kLiveMaxHostBlock = 16384;     // longer host blocks play intervals only
// The network thread's housekeeping wait: the audio thread wakes it for
// each block it queues, and a datagram does (live::Transport::Options).
constexpr int kLiveNetworkWaitMs = 10;

// The buffer in use, B (6.8), and the longest host block that plays well.
int liveBufferInUse(int setting_ms, int host_block, int host_rate);
int liveBufferNeed(int host_block, int host_rate);   // ceil(2000 x H / sr + 5)
int liveLongestGoodBlock(int host_rate);
// `/live buffer` and the saved setting: an optional sign and digits only.
bool parseWholeNumber(const std::string& s, long* out);
// A user name as Live texts show it (6.5).
std::string liveDisplayName(const std::string& name);

// One user's channel 0 in the mixer (audio context; ENGINE.md 16.8): its
// interval audio and its live audio, each through a short delay line so
// that the one coming in can be lined up with the one going out, and the
// fade of each. Made by the control context, used by the audio context.
struct LiveMix {
    static constexpr int kRing = 2048;          // frames per path
    static constexpr int kChunk = 256;          // frames mixed at a time
    static constexpr int kIntervals = 0, kLive = 1;
    std::unique_ptr<float[]> ring[2];           // interleaved stereo, by path
    int w = 0;                                  // where the next frame is written, both paths
    int delay[2] = {0, 0};                      // how far behind each path is read
    float pos[2] = {1.0f, 0.0f};                // fade positions: 0 silent, 1 full
    bool path_live = false;                     // the path that should be heard
    float rho = 0;                              // the paths' correlation during this crossfade
    int quiet = 1 << 30;                        // host frames since the live slot last had audio
    float ibuf[2 * kChunk], lbuf[2 * kChunk];
    LiveMix() {
        ring[0].reset(new float[2 * kRing]());
        ring[1].reset(new float[2 * kRing]());
    }
};

struct LiveLayer {
    LiveLayer();
    ~LiveLayer();
    LiveLayer(const LiveLayer&) = delete;
    LiveLayer& operator=(const LiveLayer&) = delete;

    // ================================================== control context

    // A new 0x07 (a full state).
    void apply(const o11proto::live::ServerLive& m);
    // Starts (or restarts) the transport; false and the reason on failure.
    bool start(const std::string& addr, std::uint16_t port, std::string& error);
    void stop();                                // the source's /stops go out first
    void forget();                              // a new connection: no Live state at all
    void health(TimePoint now);                 // works out each slot's health (6.4)
    int slotOf(const std::string& name) const;  // the slot of the listed peer with this name, or -1
    // The health report to send now (ENGINE.md 16.7), from the last one sent.
    o11proto::live::LiveReport report(TimePoint now);
    // A new room message (0x08): the mixer takes it at its first boundary
    // half a second from now.
    void applyRoom(const o11proto::live::LiveRoom& m, TimePoint now);
    bool serverAnswered() const { return transport_.serverAnswered(); }
    double serverRoundTripMs() const { return transport_.serverRoundTripMs(); }
    void setTestLoss(double f) {                // live::Transport's test hooks
        test_loss_ = f;
        transport_.setTestReceiveLoss(f);
    }
    void setTestSendGap(int ms) { transport_.setTestSendGap(ms); }
    void setTestSendHold(int ms) { transport_.setTestSendHold(ms); }
    live::Transport::Timing networkTiming() const { return transport_.timing(); }

    o11proto::live::ServerLive msg;
    bool has_msg = false;
    std::string addr;                           // what the running transport was started with
    std::uint16_t port = 0;
    std::uint32_t id = 0;
    std::uint64_t key = 0;
    struct Health {
        std::uint32_t gen = 0;
        std::uint32_t prev_drops = 0, prev_lost = 0;
        std::int64_t drops[16] = {};            // the last dropout times, ns
        int ndrops = 0;
        int lost[20] = {};                      // lost blocks per 100 ms
        std::int64_t bucket = 0;                // the current 100 ms bucket
        int recent = 0;                         // dropouts in the last 2 s
        int lost2 = 0, limit = 0;               // blocks lost for good in the last 2 s, and how many may be
        std::int64_t last_audio_ns = 0;         // when it last played, or the peer last began to send
        bool expected = false;                  // the peer sends: its slot should play
        bool healthy = false;                   // 6.4: playing for 1 s with few dropouts and losses
    };
    Health slot_health[kLiveSlots];
    std::uint32_t slot_peer[kLiveSlots] = {};   // control's copy of the slots
    std::string slot_name[kLiveSlots];
    // The room's mode and the advice (live_room.h): the latest room message,
    // the health report last sent and the notices said (engine_live.cpp).
    o11proto::live::LiveRoom room;
    bool has_room = false;
    std::uint64_t room_seq = 0;
    o11proto::live::LiveReport report_sent;
    bool report_ever = false;
    TimePoint report_at{};
    int room_noticed = 0;                       // the applied mode the notices last spoke of
    std::string advice_said;                    // whose advice the notices last spoke of ("" for none)
    o11proto::live::Advice advice_said_reason = o11proto::live::Advice::None;
    int advice_said_count = 0;
    bool start_failed_said = false, no_answer_said = false, offer_said = false;
    TimePoint started_at{}, retry_at{};
    bool buffer_changed = false;                // B changed during this connection
    TimePoint buffer_changed_at{};
    std::uint32_t changed_gen[kLiveSlots] = {}, changed_drops[kLiveSlots] = {};

    // ===================================================== shared state

    std::atomic<bool> room_on{false}, may_send{false}, running{false};
    std::atomic<bool> host_playing{true}, host_offline{false};
    std::atomic<std::uint32_t> gen[kLiveSlots]{};   // bumped by control: a slot's new peer or a dropped source
    std::atomic<int> latency_ms{kLiveAutoBufferMs};   // B, control -> audio
    // B and the automatic buffer's fallback (0: a fixed buffer), as one
    // word (B | fallback << 16) so the audio context reads them together.
    std::atomic<std::uint32_t> buffers{kLiveAutoBufferMs | (kLiveFallbackBufferMs << 16)};
    std::atomic<int> host_block{0}, host_rate{0};        // H and sr, audio -> control
    // The host frame (from the first block) where each slot's audio last
    // went into concealment, -1 for never: audio -> a test.
    std::atomic<std::int64_t> concealed_from[kLiveSlots];
    std::atomic<std::int64_t> last_block_end_ns{0};      // audio -> network (6.3, item 6)
    std::atomic<bool> sending{false};                    // the source's gate is open (audio -> control)
    // The room's mode: control announces (a count and the mode in bit 0,
    // and when it is due), the mixer applies at a boundary and says so.
    std::atomic<std::uint64_t> room_announce{0};
    std::atomic<std::int64_t> room_due_ns{0};
    std::atomic<int> room_applied{0};                    // 1: the mixer plays the room Live

    std::unique_ptr<Spsc<SinkItem, 256>> queue[kLiveSlots];
    Spsc<CaptureItem, 1024> capture;
    Spsc<RequestItem, 1024> requests;
    std::unique_ptr<LiveSink> sinks[kLiveSlots];         // audio context; stats read anywhere

    // ===================================================== audio context

    // Captures channel 0 (l and r, or null without input) and plays the sinks.
    void audioBlock(const float* l, const float* r, bool transmit, int frames, int rate);
    bool available(int s) const { return avail_[s]; }
    // Host frames since slot s last played its stream's own audio (not
    // concealment or silence), as of frame i of this host block, counting i.
    int quietAt(int s, int i) const {
        if (first_real_[s] < 0 || i < first_real_[s]) return quiet0_[s] + i + 1;
        return i < quiet_from_[s] ? 0 : i + 1 - quiet_from_[s];
    }
    const float* out(int s) const { return outbuf_[s].get(); }
    // The mixer's room state (engine_live.cpp).
    std::uint64_t mix_announce = 0;             // the announcement it took last
    bool mix_mode = false;                      // what that said
    bool mix_live = false;                      // what it plays: the mode, while Live runs here

    // ================================================= network thread

    std::mutex net_mutex;                       // control <-> network
    struct Peer {                               // a listed peer, also an active sink of the source
        std::uint32_t id = 0;
        std::string name;
        bool bound = false;
        int slot = -1;
        bool sending = false;                   // 0x07's kPeerSending
        bool start_sent = false;
        TimePoint pong_answered{}, start_answered{}, stop_answered{};
        double tokens = 16;
        TimePoint refill{};
    };
    struct NetSlot {                            // a slot's network side
        std::uint32_t peer = 0;
        std::uint32_t gen = 0;
        bool has_stream = false;
        bool stopped = false;
        bool got_data = false;
        std::int32_t stream = -1;
        int block = 240;
        std::int32_t source_latency = 0;        // the /start's latency + codec delay, 48 kHz
        TimePoint last_data{}, last_ping{}, last_stop_request{}, last_start_request{}, last_pong{}, last_request_msg{};
        double rtt_ms = -1;                     // smoothed
        std::uint64_t pong_t2 = 0;
        std::uint32_t pong_lost = 0;
        void clearStream() {
            has_stream = stopped = got_data = false;
            stream = -1;
            source_latency = 0;
        }
    };
    std::vector<Peer> peers;                    // the listed peers, in the message's order
    NetSlot net[kLiveSlots];
    bool net_room_on = false;
    std::vector<std::uint32_t> stop_to;         // peers no longer listed that get a /stop

    void onData(std::uint32_t from, const std::uint8_t* p, std::size_t n);
    void onTick();
    void onClosing();

private:
    // network thread, net_mutex held
    Peer* findPeer(std::uint32_t id);
    void send(std::uint32_t dest, const std::uint8_t* p, std::size_t n) { transport_.send(dest, p, n); }
    bool hostRunning(std::int64_t now_ns) const;
    void sinkData(int s, const std::uint8_t* p, std::size_t n, TimePoint now);
    void sinkOsc(int s, const aoo::OscMessage& m, TimePoint now);
    void sourceRequest(Peer& peer, const std::uint8_t* p, std::size_t n, TimePoint now);
    void sourceOsc(Peer& peer, const aoo::OscMessage& m, TimePoint now);
    void pushItem(int s, SinkItem::Kind kind, std::int32_t seq, const std::uint8_t* data, std::size_t len, int block);
    void drainCapture(TimePoint now);
    void startStream(int rate, TimePoint now);
    void stopStream();
    void encodeAndSend(TimePoint now);
    void sendStart(Peer& peer);
    void sendRequests(TimePoint now);
    void resetNet();                            // every slot's network side; the audio side follows the gens
    void bumpGen(int s);

    // the source (network thread)
    struct Source {
        OpusEncoder* enc = nullptr;
        int enc_frame = 0;
        int kbps = 128, frame = 240;            // from 0x07
        bool kbps_changed = false, frame_changed = false;
        bool running = false;
        std::int32_t stream = 0, format = 0, seq = 0;
        std::uint32_t next_stream = 0, next_format = 0;
        TimePoint start_time{};
        std::int64_t sent48 = 0;
        int in_rate = 0;
        double t = 0;
        float x[4][2] = {};
        float pcm[2 * 480] = {};
        int pcm_n = 0;
        double xrun48 = 0;
        double all_tokens = 32;
        TimePoint all_refill{};
        TimePoint last_ping{};
        struct Sent {
            std::int32_t seq = -1;
            TimePoint when{};
            std::uint16_t len = 0;
            std::uint8_t bytes[24 + aoo::kMaxBlockBytes];
        };
        std::unique_ptr<Sent[]> history;        // 1 s of blocks, by sequence
    } src_;
    static constexpr int kHistory = 512;
    std::uint8_t msgbuf_[aoo::kMaxMessage + 64];

    // audio context
    std::uint32_t audio_gen_[kLiveSlots] = {};
    bool avail_[kLiveSlots] = {};
    int quiet0_[kLiveSlots] = {}, first_real_[kLiveSlots] = {}, quiet_from_[kLiveSlots] = {};
    int quiet_run_[kLiveSlots] = {};            // quietAt() after the block
    std::int64_t frames_ = 0;                   // host frames since the first block
    std::unique_ptr<float[]> outbuf_[kLiveSlots];
    bool gate_open_ = false;
    std::int64_t expect_ns_ = 0, prev_start_ns_ = 0, prev_dur_ns_ = 0;
    bool late_pending_ = false;
    // The host's schedule: when this block is due on it, held through a
    // late block and the blocks that catch up on it (audioBlock()).
    std::int64_t sched_ns_ = 0;
    bool sched_late_ = false;
    int longest_ = 0;
    std::int64_t longest_seen_ns_ = 0;

    double test_loss_ = 0;
    o11proto::live::Transport transport_;       // last: its thread stops first
};

}  // namespace engine
}  // namespace o11proto

#endif  // O11PROTO_ENGINE_LIVE_LAYER_H
