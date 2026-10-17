// o11proto engine - Live mode, one sink's playback in the audio context:
// the jitter buffer of 5.9, Opus decoding with the clamp of 5.7, and the
// cubic resampler to the host rate whose speed keeps the buffer's fill
// (6.9, see engine/NOTES.md).
#include "live_sink.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus/opus.h>

namespace o11proto {
namespace engine {

namespace {

constexpr std::int64_t kRequestAgainNs = 10000000;   // 10 ms
constexpr double kFillGain = 0.1;                     // speed change per second of fill error
constexpr double kMaxSpeedChange = 0.005;
constexpr double kFillSmoothing = 0.5;                // seconds
constexpr double kFillSettle = 1.0;                   // seconds of playing before the level is taken
constexpr double kFillDeadBand = 0.002;               // seconds around it

int ceilDiv(double a, double b) { return static_cast<int>(std::ceil(a / b - 1e-9)); }

}  // namespace

void SinkStats::clear(std::uint32_t g) {
    active.store(false);
    active_since_ns.store(0);
    underruns.store(0);
    overruns.store(0);
    lost.store(0);
    xrun_gaps.store(0);
    resent.store(0);
    lost_at_active.store(0);
    resent_at_active.store(0);
    buffer_us.store(-1);
    block.store(0);
    gen.store(g);
}

SinkSizes sinkSizes(double latency_ms, int host_block, int host_rate, int frame) {
    SinkSizes s;
    if (host_rate <= 0) host_rate = 48000;
    if (frame <= 0) frame = 240;
    s.latency48 = ceilDiv(latency_ms * 48.0, 1.0);
    s.latency_blocks = std::max(ceilDiv(latency_ms * 48.0, frame), ceilDiv(64.0 * 48000.0, double(host_rate) * frame));
    const double capacity_ms = std::max(2.0 * latency_ms, latency_ms + 2000.0 * host_block / host_rate + 5.0);
    s.capacity_blocks = std::max(s.latency_blocks, ceilDiv(capacity_ms * 48.0, frame));
    s.latency_blocks = std::min(s.latency_blocks, LiveSink::kMaxCapacity);
    s.capacity_blocks = std::min(s.capacity_blocks, LiveSink::kMaxCapacity);
    return s;
}

void clampDecoded(float* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        float v = s[i];
        if (!(v >= -2.0f)) v = -2.0f;            // NaN too
        else if (v > 2.0f) v = 2.0f;
        s[i] = v;
    }
}

LiveSink::LiveSink() : ring_(new Entry[kRing]) {
    dec_mem_.reset(new unsigned char[static_cast<std::size_t>(opus_decoder_get_size(2))]);
    dec_ = reinterpret_cast<OpusDecoder*>(dec_mem_.get());
    opus_decoder_init(dec_, 48000, 2);
    sizesChanged();
}

LiveSink::~LiveSink() = default;

void LiveSink::sizesChanged() { sizes_ = sinkSizes(latency_ms_, host_block_, host_rate_, block_); }

void LiveSink::configure(double latency_ms, int host_block, int host_rate) {
    if (latency_ms == latency_ms_ && host_block == host_block_ && host_rate == host_rate_) return;
    const bool restart = latency_ms != latency_ms_;
    latency_ms_ = latency_ms;
    host_block_ = host_block;
    host_rate_ = host_rate;
    sizesChanged();
    if (restart && phase_ != Idle && !stopping_) {   // 6.8: a new buffer buffers again
        phase_ = Buffering;
        waited_ = 0;
        stats.active.store(false);
    }
}

void LiveSink::reset() {
    phase_ = Idle;
    stopping_ = false;
    for (int i = 0; i < kRing; ++i) ring_[i].state = Empty;
    next_ = 0;
    newest_ = -1;
    last_played_ = -1;
    far_ = false;
    fifo_n_ = fifo_at_ = 0;
    fill_ = -1;
    stats.active.store(false);
}

void LiveSink::emptyFrom(std::int32_t seq) {
    for (std::int64_t s = next_; s <= newest_ && s < next_ + kRing; ++s) ring_[s & (kRing - 1)].state = Empty;
    next_ = seq;
    newest_ = static_cast<std::int64_t>(seq) - 1;
    last_played_ = newest_;
    far_ = false;
}

void LiveSink::take(std::int32_t seq, const SinkItem& it, std::int64_t now_ns) {
    (void)now_ns;
    if (seq > newest_) {
        for (std::int64_t s = newest_ + 1; s < seq; ++s) {
            Entry& m = ring_[s & (kRing - 1)];
            m.seq = static_cast<std::int32_t>(s);
            m.state = Missing;
            m.requested = false;
            m.asked_ns = 0;
        }
        newest_ = seq;
    }
    Entry& e = ring_[seq & (kRing - 1)];
    if (e.seq == seq && (e.state == Full || e.state == XrunBlock)) return;   // a duplicate
    const bool resent = e.seq == seq && e.state == Missing && e.requested;
    e.seq = seq;
    e.state = it.kind == SinkItem::Xrun ? XrunBlock : Full;
    e.len = it.len;
    if (it.kind == SinkItem::Data) std::memcpy(e.data, it.data, it.len);
    if (resent) stats.resent.fetch_add(1);
    far_ = false;
}

void LiveSink::apply(const SinkItem& it, std::int64_t now_ns) {
    switch (it.kind) {
    case SinkItem::Start:
        block_ = it.block;
        sizesChanged();
        opus_decoder_ctl(dec_, OPUS_RESET_STATE);
        emptyFrom(it.seq);
        phase_ = Buffering;
        stopping_ = false;
        waited_ = 0;
        fifo_n_ = fifo_at_ = 0;
        fill_ = -1;
        stats.block.store(block_);
        stats.active.store(false);
        return;
    case SinkItem::Stop:
        if (phase_ == Idle) return;
        stopping_ = true;
        if (phase_ == Dry || (phase_ == Buffering && heldBlocks() == 0)) {
            phase_ = Idle;                       // the stream ended; not an underrun
            stats.active.store(false);
        } else if (phase_ == Buffering) {
            waited_ = sizes_.latency48;          // gives up waiting: plays out what it has
        }
        return;
    case SinkItem::Reset:
        reset();
        return;
    case SinkItem::Data:
    case SinkItem::Xrun:
        break;
    }
    if (phase_ == Idle || it.len > aoo::kMaxBlockBytes) return;
    std::int32_t seq = it.seq;
    if (seq < next_) {                           // at or before the last block played
        if (last_played_ - seq < (1LL << 30)) return;   // old
        emptyFrom(seq);                          // the stream wrapped
    }
    if (phase_ == Dry) {                         // audio again after running dry
        if (stopping_) return;
        stats.underruns.fetch_add(1);
        emptyFrom(seq);
        phase_ = Buffering;
        waited_ = 0;
    }
    if (seq <= newest_) {
        take(seq, it, now_ns);
        return;
    }
    const std::int64_t ahead = static_cast<std::int64_t>(seq) - newest_;
    if (ahead > sizes_.capacity_blocks) {        // far ahead: a jump only when a second one follows
        const std::int64_t after = static_cast<std::int64_t>(seq) - far_seq_;
        if (!(far_ && after >= 1 && after <= sizes_.capacity_blocks)) {
            far_ = true;
            far_seq_ = seq;
            return;
        }
    } else if (static_cast<std::int64_t>(seq) - next_ + 1 <= sizes_.capacity_blocks) {
        take(seq, it, now_ns);
        return;
    }
    stats.overruns.fetch_add(1);                 // no room: only this block is kept
    stats.active.store(false);
    emptyFrom(seq);
    phase_ = Buffering;
    waited_ = 0;
    take(seq, it, now_ns);
}

void LiveSink::conceal() {
    int n = opus_decode_float(dec_, nullptr, 0, fifo_, block_, 0);
    if (n != block_) std::memset(fifo_, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_));
}

void LiveSink::decodeNext(std::int64_t now_ns) {
    fifo_at_ = 0;
    fifo_n_ = block_;
    if (phase_ == Buffering) {
        const int held = heldBlocks();
        if (waited_ >= sizes_.latency48 && (held >= sizes_.latency_blocks || (stopping_ && held > 0))) {
            if (held > sizes_.latency_blocks) {  // more than the latency: the oldest go
                const std::int64_t skip = held - sizes_.latency_blocks;
                for (std::int64_t s = next_; s < next_ + skip; ++s) ring_[s & (kRing - 1)].state = Empty;
                next_ += skip;
                last_played_ = next_ - 1;
            }
            const int buffered = std::min(heldBlocks() * block_, waited_);
            stats.buffer_us.store(static_cast<std::int32_t>(static_cast<std::int64_t>(buffered) * 1000000 / 48000));
            phase_ = Playing;
            fill_ = -1;
            stats.lost_at_active.store(stats.lost.load());
            stats.resent_at_active.store(stats.resent.load());
            stats.active_since_ns.store(now_ns);
            stats.active.store(true);
        } else {
            if (waited_ >= 4 * sizes_.latency48) {   // gives up: an underrun, and waits again
                stats.underruns.fetch_add(1);
                waited_ = 0;
            }
            conceal();
            clampDecoded(fifo_, 2 * static_cast<std::size_t>(block_));
            waited_ += block_;
            played48_ += block_;
            return;
        }
    }
    switch (phase_) {
    case Idle:
        std::memset(fifo_, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_));
        return;
    case Dry:
        conceal();
        break;
    case Playing: {
        if (next_ > newest_) {                   // empty at a block's turn
            stats.active.store(false);
            if (stopping_) {
                phase_ = Idle;
                std::memset(fifo_, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_));
                return;
            }
            phase_ = Dry;
            conceal();
            break;
        }
        Entry& e = ring_[next_ & (kRing - 1)];
        if (e.seq == next_ && e.state == Full) {
            int n = opus_decode_float(dec_, e.data, e.len, fifo_, block_, 0);
            if (n < 0) conceal();
            else if (n < block_)
                std::memset(fifo_ + 2 * n, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_ - n));
        } else if (e.seq == next_ && e.state == XrunBlock) {
            conceal();
            if (played48_ - last_xrun48_ > 4800) stats.xrun_gaps.fetch_add(1);   // one gap per 100 ms
            last_xrun48_ = played48_;
        } else {
            conceal();                           // lost for good
            stats.lost.fetch_add(1);
        }
        e.state = Empty;
        last_played_ = next_;
        ++next_;
        break;
    }
    case Buffering:
        break;
    }
    clampDecoded(fifo_, 2 * static_cast<std::size_t>(block_));
    played48_ += block_;
}

void LiveSink::hostGap(int host_frames, int host_rate) {
    if (phase_ == Idle || host_rate <= 0) return;
    const double g48 = static_cast<double>(host_frames) * 48000.0 / host_rate;
    const std::int64_t n = static_cast<std::int64_t>(g48 / block_ + 0.5);
    fifo_at_ = fifo_n_;
    for (std::int64_t s = next_; s < next_ + n && s <= newest_; ++s) ring_[s & (kRing - 1)].state = Empty;
    next_ += n;
    last_played_ = next_ - 1;
    if (newest_ < next_ - 1) newest_ = next_ - 1;
    fill_ = -1;                                  // the clock estimate restarts
}

bool LiveSink::render(float* out, int frames, int rate, std::int64_t now_ns) {
    if (phase_ == Idle) {
        std::memset(out, 0, sizeof(float) * 2 * static_cast<std::size_t>(frames));
        return false;
    }
    double step = 48000.0 / rate;
    if (phase_ == Playing) {                     // the speed follows the fill (6.9)
        const double fill = (heldBlocks() * block_ + (fifo_n_ - fifo_at_)) / 48000.0;
        if (fill_ < 0) {                         // a new estimate: from this fill
            fill_ = fill;
            fill_ref_ = -1;
            fill_age_ = 0;
        } else {
            fill_ += (fill - fill_) * std::min(1.0, frames / (rate * kFillSmoothing));
        }
        // The level it settles at after a second is the one to keep: the
        // speed only makes up for the clocks' difference, so the stream
        // plays at its own pitch and the delay stays what buffering gave.
        fill_age_ += static_cast<double>(frames) / rate;
        if (fill_ref_ < 0 && fill_age_ >= kFillSettle) fill_ref_ = fill_;
        if (fill_ref_ >= 0) {                    // within the dead band the fill's own jitter changes nothing
            const double e = fill_ - fill_ref_;
            const double off = e > kFillDeadBand ? e - kFillDeadBand : (e < -kFillDeadBand ? e + kFillDeadBand : 0.0);
            step *= 1.0 + std::max(-kMaxSpeedChange, std::min(kMaxSpeedChange, kFillGain * off));
        }
    }
    for (int i = 0; i < frames; ++i) {
        while (t_ >= 1.0) {
            t_ -= 1.0;
            if (fifo_at_ >= fifo_n_) decodeNext(now_ns);
            for (int c = 0; c < 2; ++c) {
                x_[0][c] = x_[1][c];
                x_[1][c] = x_[2][c];
                x_[2][c] = x_[3][c];
                x_[3][c] = fifo_[2 * fifo_at_ + c];
            }
            ++fifo_at_;
        }
        const float t = static_cast<float>(t_);
        for (int c = 0; c < 2; ++c) {            // Catmull-Rom between x_[1] and x_[2]
            const float a = x_[0][c], b = x_[1][c], d = x_[2][c], e = x_[3][c];
            out[2 * i + c] = b + 0.5f * t * (d - a + t * (2.0f * a - 5.0f * b + 4.0f * d - e + t * (3.0f * (b - d) + e - a)));
        }
        t_ += step;
    }
    return true;
}

int LiveSink::dueRequests(std::int64_t now_ns, std::int32_t* seqs, int max) {
    int n = 0;
    if (phase_ == Idle) return 0;
    for (std::int64_t s = next_; s < newest_ && n < max; ++s) {
        Entry& e = ring_[s & (kRing - 1)];
        if (e.seq != s || e.state != Missing || (e.requested && now_ns - e.asked_ns < kRequestAgainNs)) continue;
        e.asked_ns = now_ns;
        e.requested = true;
        seqs[n++] = static_cast<std::int32_t>(s);
    }
    return n;
}

}  // namespace engine
}  // namespace o11proto
