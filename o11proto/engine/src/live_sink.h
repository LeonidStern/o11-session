// o11proto engine - Live mode, the audio context's side of a stream: the
// lock-free queues between the threads, and one sink's jitter buffer,
// Opus decoder and resampler (live-mode spec 5.9, 6.8, 6.9). Internal.
//
// Everything a sink needs is allocated when it is made; after that nothing
// here allocates, locks or makes a system call.
#ifndef O11PROTO_ENGINE_LIVE_SINK_H
#define O11PROTO_ENGINE_LIVE_SINK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "live_aoo.h"

struct OpusDecoder;

namespace o11proto {
namespace engine {

// Single producer, single consumer, N (a power of two) items allocated once.
template <typename T, std::size_t N>
class Spsc {
public:
    Spsc() : items_(new T[N]) {}
    // Producer: the slot to fill, or null when full; then push().
    T* back() {
        std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= N) return nullptr;
        return &items_[h & (N - 1)];
    }
    void push() { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
    // Consumer: the oldest item, or null when empty; then pop().
    T* front() {
        std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return nullptr;
        return &items_[t & (N - 1)];
    }
    void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
    std::unique_ptr<T[]> items_;
    std::atomic<std::size_t> head_{0}, tail_{0};
};

// Network thread -> audio context, one queue per slot.
struct SinkItem {
    enum Kind : std::uint8_t { Start, Data, Xrun, Stop, Reset };
    Kind kind = Data;
    std::uint32_t gen = 0;                      // the slot's generation it belongs to
    std::int32_t seq = 0;                       // Start: the start sequence
    std::uint16_t len = 0;
    std::uint16_t block = 0;                    // Start: the frame size
    std::uint8_t data[aoo::kMaxBlockBytes];
};

// Audio context -> network thread: the capture of the source.
struct CaptureItem {
    enum Kind : std::uint8_t { Audio, Stop, Gap };
    static constexpr int kFrames = 64;          // AOO's host-side block
    Kind kind = Audio;
    std::uint16_t frames = 0;
    std::int32_t rate = 0;
    std::int32_t gap = 0;                       // Gap: host samples missed
    float data[2 * kFrames];                    // interleaved stereo
};

// Audio context -> network thread: blocks to ask for again.
struct RequestItem {
    std::uint8_t slot = 0;
    std::uint32_t gen = 0;
    std::int32_t seq = 0;
};

// What a sink's audio side reports; written by the audio context only.
struct SinkStats {
    std::atomic<std::uint32_t> gen{0};          // the generation these counts belong to
    std::atomic<bool> active{false};            // playing (not buffering, not dry, not idle)
    std::atomic<std::int64_t> active_since_ns{0};
    std::atomic<std::uint32_t> underruns{0}, overruns{0}, lost{0}, xrun_gaps{0}, resent{0};
    std::atomic<std::uint32_t> lost_at_active{0}, resent_at_active{0};   // the counts when it last went active
    std::atomic<std::int32_t> buffer_us{-1};    // what the buffer held when playback last started
    std::atomic<std::int32_t> block{0};         // the stream's frame size
    void clear(std::uint32_t g);
};

// Sink sizes of 5.9 and 6.8, from the buffer in use `latency_ms` (B), the
// longest host block and the host rate.
struct SinkSizes {
    int latency48 = 0;                          // B in 48 kHz samples
    int latency_blocks = 1;
    int capacity_blocks = 2;
};
SinkSizes sinkSizes(double latency_ms, int host_block, int host_rate, int frame);

// Clamps decoded samples to -2..+2, NaN to -2 (5.7).
void clampDecoded(float* s, std::size_t n);

// Seamless switching (ENGINE.md 16.8). Every crossfade between a user's
// live and interval audio, and every fade of the live audio alone, takes
// kLiveFadeMs. A slot that has no audio (lost, stopped or timed out) is
// concealed for kLiveConcealBuffers times the buffer in use (B) before it
// fades out: 80 ms at the default 20 ms. This is a time of its own, not the
// 4 x B after which buffering counts an underrun.
constexpr double kLiveFadeMs = 20.0;
constexpr double kLiveConcealBuffers = 4.0;

// One sink's playback, audio context only.
class LiveSink {
public:
    static constexpr int kRing = 256;           // blocks; the capacity is capped below it
    static constexpr int kMaxCapacity = 250;

    LiveSink();
    ~LiveSink();
    LiveSink(const LiveSink&) = delete;
    LiveSink& operator=(const LiveSink&) = delete;

    // The buffer in use and the host's blocks. A new latency restarts the
    // buffering (6.8).
    void configure(double latency_ms, int host_block, int host_rate);
    void reset();                               // no stream
    // The stream was dropped (its slot's generation changed): what it holds
    // plays out, then the concealment, for the mixer to fade out (16.8).
    void drop();
    void apply(const SinkItem& it, std::int64_t now_ns);
    void hostGap(int host_frames, int host_rate);   // skip that much of the stream (6.9)
    // Renders `frames` frames at `rate` into `out` (interleaved stereo).
    // False when there is no stream: then `out` is silence. After a stream
    // ended or was dropped, the decoder's concealment goes on long enough
    // for the mixer to fade it out (tailing()), and that counts as a stream.
    bool render(float* out, int frames, int rate, std::int64_t now_ns);
    // Incomplete blocks (all but the newest) not asked for in the last
    // 10 ms, at most `max`; returns how many were written to `seqs`.
    int dueRequests(std::int64_t now_ns, std::int32_t* seqs, int max);

    bool hasStream() const { return phase_ != Idle; }
    bool playing() const { return phase_ == Playing; }
    bool tailing() const { return phase_ == Idle && tail48_ > 0; }
    int heldBlocks() const { return newest_ >= next_ ? static_cast<int>(newest_ - next_ + 1) : 0; }
    const SinkSizes& sizes() const { return sizes_; }
    SinkStats stats;

private:
    enum Phase { Idle, Buffering, Playing, Dry };
    enum State : std::uint8_t { Empty, Missing, Full, XrunBlock };
    struct Entry {
        std::int32_t seq = -1;
        State state = Empty;
        bool requested = false;
        std::uint16_t len = 0;
        std::int64_t asked_ns = 0;
        std::uint8_t data[aoo::kMaxBlockBytes];
    };

    void emptyFrom(std::int32_t seq);           // forget every block, go on from `seq`
    void take(std::int32_t seq, const SinkItem& it, std::int64_t now_ns);
    void decodeNext(std::int64_t now_ns);       // one block into the 48 kHz queue
    void conceal();
    void sizesChanged();
    void startTail();                           // a stream just went: concealment to fade out

    std::unique_ptr<Entry[]> ring_;
    std::unique_ptr<unsigned char[]> dec_mem_;
    OpusDecoder* dec_ = nullptr;
    Phase phase_ = Idle;
    bool stopping_ = false;                     // /stop came: play out, then end
    int block_ = 240;
    // Sequences in 64 bits, so that a jump near 0x7fffffff can't overflow.
    std::int64_t next_ = 0;                     // the next sequence to play
    std::int64_t newest_ = -1;                  // the newest block taken
    std::int64_t last_played_ = -1;
    bool far_ = false;
    std::int64_t far_seq_ = 0;
    int waited_ = 0;                            // 48 kHz samples of concealment while buffering
    double latency_ms_ = 20;
    int host_block_ = 0, host_rate_ = 48000;
    SinkSizes sizes_;
    float fifo_[2 * 480];                       // decoded 48 kHz frames
    int fifo_n_ = 0, fifo_at_ = 0;
    float x_[4][2] = {};                        // the cubic resampler's window
    double t_ = 0;                              // its position between x_[1] and x_[2]
    double fill_ = -1;                          // smoothed buffer fill, seconds; -1: a new estimate
    double fill_ref_ = -1;                      // the fill it keeps, once settled
    double fill_age_ = 0;                       // seconds of this estimate
    std::int64_t played48_ = 0;
    std::int64_t last_xrun48_ = -(1LL << 40);
    int tail48_ = 0;                            // 48 kHz samples of concealment left after the stream
};

}  // namespace engine
}  // namespace o11proto

#endif  // O11PROTO_ENGINE_LIVE_SINK_H
