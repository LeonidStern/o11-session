// o11proto engine - Live mode, the audio context's side of a stream: the
// lock-free queues between the threads, and one sink's jitter buffer,
// Opus decoder and resampler (live-mode spec 5.9, 6.8, 6.9). Internal.
//
// Everything a sink needs is allocated when it is made; after that nothing
// here allocates, locks or makes a system call.
#ifndef O11PROTO_ENGINE_LIVE_SINK_H
#define O11PROTO_ENGINE_LIVE_SINK_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "live_aoo.h"

struct OpusDecoder;
struct OpusEncoder;

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
    std::atomic<std::int32_t> fill_us{-1};      // while playing: the buffer's smoothed fill now
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
// fades out: 80 ms at the default 20 ms, from the frame where the
// concealment began (LiveSink::quietFrom()). This is a time of its own, not
// the 4 x B after which buffering counts an underrun.
constexpr double kLiveFadeMs = 20.0;
constexpr double kLiveConcealBuffers = 4.0;
// The concealment goes on for this much more than the mixer's hold and fade.
constexpr double kLiveConcealSpareMs = 10.0;

// The concealment (16.6). Where a slot has no audio of its stream (a block
// lost for good, an xrun block, the stream dry or buffering again, the time
// after it ended), what was played goes on by linear prediction: each
// sample from the kDense before it and kSparse further back (to 8 ms),
// fitted by least squares to the last 60 ms played (as the interval padding
// continues an interval, dsp.cpp), one fit for both channels. A steady
// sound goes on at its level and in its phase, a decaying one decays, noise
// dies away, and nothing comes out louder than the last 5 ms played. After
// `limit` samples of it (the time the mixer plays a concealment before it
// has faded it out) it ends in silence, though the prediction goes on
// unheard for kGoesOn more: what is fitted to and what primes the decoder
// is the stream's best guess (the prediction, and the audio decoded before
// it is crossfaded), not what was heard. When the stream's audio comes
// back, the sink first primes its decoder (LiveSink::prime()), and the
// concealment crossfades into the audio over kFade. Opus's own concealment
// isn't used: at 5 ms frames it fades a held note within 40 ms and makes a
// burst of its own where the loss begins.
class Concealer {
public:
    static constexpr int kDense = 24, kSparse = 8, kOrder = kDense + kSparse;
    static constexpr int kMaxLag = 384;
    static constexpr int kRing = 512;           // a power of two above kMaxLag
    static constexpr int kFit = 2880;           // 60 ms at 48 kHz
    static constexpr int kGuard = 240;          // the level it keeps under: the last 5 ms
    static constexpr int kFade = 240;
    static constexpr int kGoesOn = 48000;       // 1 s

    void clear();                               // nothing played yet: concealment is silence
    void setLimit(int samples) { limit_ = samples; }
    void next(float* out, int frames);          // `frames` of concealment, interleaved stereo
    // The stream's audio jumps (the host missed some): joined like audio
    // that comes back after concealment.
    void cut();
    bool concealing() const { return state_ == Concealing; }   // the block played last was concealment
    bool joining() const { return state_ == Joining; }
    // `decoded` (from a decoder primed for it) joined to the concealment, in
    // place; again for the next block until the crossfade is done.
    void join(float* decoded, int frames);
    void played(const float* x, int frames) { remember(x, frames); }   // every block decoded and not joined
    // The last `frames` of the best guess (at most kFit), interleaved
    // stereo, silence before the first.
    void recent(float* out, int frames) const;
    // The next `frames` of the prediction, as if it went on unheard, without
    // taking them (at most kRing - kMaxLag).
    void peek(float* out, int frames);

private:
    enum State { Off, Concealing, Joining };
    void remember(const float* x, int frames);  // to the history
    void fit();
    float predict(int c);                       // the next sample of channel c, unscaled
    static double dot(const double* a, const double* b, int n);
    // After the limit, down to silence over kGuard samples.
    float ending(int t) const {
        return t < limit_ ? 1.0f : std::max(0.0f, 1.0f - static_cast<float>(t - limit_) / kGuard);
    }

    float hist_[2][kFit] = {};                  // the newest at the end
    int hist_n_ = 0;
    State state_ = Off;
    int order_ = 0;                             // 0: silence
    static int lag(int i) {                     // 1..kDense, then 32, 48, 64, 96, 128, 192, 256, 384
        static constexpr int kFar[kSparse] = {32, 48, 64, 96, 128, 192, 256, 384};
        return i < kDense ? i + 1 : kFar[i - kDense];
    }
    double a_[kOrder] = {};                     // y[t] = sum over i of a[i] y[t - lag(i)]
    double x_[kFit] = {};                       // what is fitted: the mid, or the side
    double y_[2][kRing] = {};                   // the samples predicted, a ring
    int at_ = 0;
    double ms_ref_ = 0;                         // the mean square of the last kGuard played
    float gain_ = 1;
    int done_ = 0;                              // samples of this concealment
    int joined_ = 0;
    int limit_ = 4800;
    double cov_[kOrder][kOrder] = {}, chol_[kOrder][kOrder] = {}, rhs_[kOrder] = {};
};

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
    // Before each block's apply(): how far (48 kHz samples) the host's
    // block is behind its schedule, catching up on a stall. The stream went
    // on arriving meanwhile, so the buffer holds that much more without
    // being any later: it has room for it, and its fill and its start leave
    // it out.
    void hostBehind(int samples48) { behind48_ = std::max(0, samples48); }
    // Renders `frames` frames at `rate` into `out` (interleaved stereo).
    // False when there is no stream: then `out` is silence. After a stream
    // ended or was dropped, the concealment goes on long enough
    // for the mixer to fade it out (tailing()), and that counts as a stream.
    bool render(float* out, int frames, int rate, std::int64_t now_ns);
    // Incomplete blocks (all but the newest) not asked for in the last
    // 10 ms, at most `max`; returns how many were written to `seqs`.
    int dueRequests(std::int64_t now_ns, std::int32_t* seqs, int max);

    // Of the last render's frames: the first that played the stream's own
    // audio (decoded, not concealment), -1 for none, and the one where the
    // concealment it ends with began (`frames` when it ends with audio).
    int firstReal() const { return first_real_; }
    int quietFrom() const { return quiet_from_; }
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
    void dropBefore(std::int64_t seq);          // the blocks before `seq` go unplayed
    std::int64_t turn() const { return (played48_ - align_) / block_; }   // the block whose turn it is on align_
    int behindBlocks() const { return (behind48_ + block_ / 2) / block_; }
    // How many blocks from next_ it may hold: the capacity, and what came
    // while the host was behind.
    std::int64_t room() const {
        const int extra = behind48_ > 0 ? (behind48_ + block_ - 1) / block_ + 1 : 0;
        return std::min(kRing - 2, sizes_.capacity_blocks + extra);
    }
    void decodeNext(std::int64_t now_ns);       // one block into the 48 kHz queue
    void conceal();
    void prime();
    int concealSamples() const;                 // after a stream: B x kLiveConcealBuffers, the fade, 10 ms
    void sizesChanged();
    void startTail();                           // a stream just went: concealment to fade out

    std::unique_ptr<Entry[]> ring_;
    std::unique_ptr<unsigned char[]> dec_mem_;
    OpusDecoder* dec_ = nullptr;
    // The decoder's primer: an encoder of our own, and what it codes.
    static constexpr int kPrimeHistory = 1024;  // CELT's longest pitch period
    static constexpr int kMaxPrime = 2 * 960;
    std::unique_ptr<unsigned char[]> enc_mem_;
    OpusEncoder* enc_ = nullptr;
    int lookahead_ = 120;
    float prime_[2 * kMaxPrime];
    bool fresh_ = false;                        // the decoder is in the state the stream began in
    std::unique_ptr<Concealer> concealer_;
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
    bool fifo_real_ = false;                    // the queue holds a decoded block, not concealment
    float x_[4][2] = {};                        // the cubic resampler's window
    bool xr_[4] = {};                           // and whether each of its samples was the stream's own
    int first_real_ = -1, quiet_from_ = 0;
    double t_ = 0;                              // its position between x_[1] and x_[2]
    double fill_ = -1;                          // smoothed buffer fill, seconds; -1: a new estimate
    // The fill it keeps, taken once settled after the stream's first
    // buffering (or a new buffer setting), and kept through its later ones.
    double fill_ref_ = -1;
    double fill_age_ = 0;                       // seconds of this estimate
    int behind48_ = 0;                          // hostBehind()
    std::int64_t played48_ = 0;
    // The delay the stream plays at: block s is decoded when played48_ is
    // align_ + s x block_. After running dry, the stream comes back on it.
    std::int64_t align_ = 0;
    bool has_align_ = false;                    // the stream has played, so align_ holds
    bool realign_ = false;                      // buffering again: play next_ at its time on align_
    int late48_ = 0;                            // 48 kHz samples of that with nothing in time for its turn
    std::int64_t last_xrun48_ = -(1LL << 40);
    int tail48_ = 0;                            // 48 kHz samples of concealment left after the stream
};

}  // namespace engine
}  // namespace o11proto

#endif  // O11PROTO_ENGINE_LIVE_SINK_H
