// o11proto engine - Live mode, one sink's playback in the audio context:
// the jitter buffer of 5.9, Opus decoding with the clamp of 5.7, the
// concealment (ENGINE.md 16.6), and the cubic resampler to the host rate
// whose speed keeps the buffer's fill (6.9, see engine/NOTES.md).
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
    fill_us.store(-1);
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

// ---------------------------------------------------------- concealment

void Concealer::clear() {
    hist_n_ = 0;
    state_ = Off;
    order_ = 0;
    gain_ = 1;
}

void Concealer::remember(const float* x, int frames) {
    const int n = std::min(frames, kFit);
    for (int c = 0; c < 2; ++c) {
        float* h = hist_[c];
        std::memmove(h, h + n, sizeof(float) * static_cast<std::size_t>(kFit - n));
        for (int i = 0; i < n; ++i) h[kFit - n + i] = x[2 * (frames - n + i) + c];
    }
    hist_n_ = std::min(kFit, hist_n_ + n);
}

double Concealer::dot(const double* a, const double* b, int n) {
    double s[4] = {0, 0, 0, 0};                  // four sums, so that the products don't wait on one
    int i = 0;
    for (; i + 4 <= n; i += 4)
        for (int k = 0; k < 4; ++k) s[k] += a[i + k] * b[i + k];
    for (; i < n; ++i) s[0] += a[i] * b[i];
    return (s[0] + s[1]) + (s[2] + s[3]);
}

void Concealer::fit() {
    done_ = 0;
    gain_ = 1;
    order_ = 0;
    const int n = hist_n_;
    double ms = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = kFit - std::min(n, kGuard); i < kFit; ++i) ms += static_cast<double>(hist_[c][i]) * hist_[c][i];
    ms_ref_ = ms / (2.0 * std::max(1, std::min(n, kGuard)));
    if (n < kMaxLag + 4 * kOrder || ms_ref_ < 1e-14) return;   // too little, or silence: silence
    // One fit for both channels, to their mid (or their side, where that is
    // the louder): it predicts each of them from its own past, at half the
    // cost of a fit each.
    double mid = 0, side = 0;
    for (int i = kFit - n; i < kFit; ++i) {
        const double m = 0.5 * (static_cast<double>(hist_[0][i]) + hist_[1][i]);
        const double d = 0.5 * (static_cast<double>(hist_[0][i]) - hist_[1][i]);
        mid += m * m;
        side += d * d;
    }
    const double sign = side > mid ? -1.0 : 1.0;
    double* x = x_ + kFit - n;
    for (int i = 0; i < n; ++i)
        x[i] = 0.5 * (static_cast<double>(hist_[0][kFit - n + i]) + sign * hist_[1][kFit - n + i]);
    // The least-squares fit (the covariance method) over the samples
    // themselves. From 60 ms it finds a steady tone's frequency far better
    // than Burg's fit (a 440 Hz tone predicted for 80 ms ends about 3
    // degrees off, against more than 90 with Burg's), so the audio that
    // comes back is in phase with the prediction it is crossfaded from; over
    // 30 ms the codec's small changes of level from frame to frame made a
    // held tone fade by up to 1.5 dB in 80 ms, over 60 ms by 0.3. The lags
    // further back hold a low sound (a kick's tail at 50 Hz) that the
    // nearest 24 can't tell from the noise over it: those alone let it die
    // away within 20 ms.
    constexpr int p = kOrder;
    const int rows = n - kMaxLag;
    auto xx = [x](int i, int j) { return x[i] * x[j]; };
    // R[i][j] = sum over t = kMaxLag..n-1 of x[t - lag(i)] x[t - lag(j)]:
    // among the dense lags, the first row in full and the rest from the one
    // above it, by the samples that enter and leave; the rest in full.
    for (int j = 0; j < p; ++j) cov_[0][j] = dot(x + kMaxLag - 1, x + kMaxLag - lag(j), rows);
    for (int i = 0; i + 1 < kDense; ++i)
        for (int j = i; j + 1 < kDense; ++j)
            cov_[i + 1][j + 1] = cov_[i][j] - xx(n - 2 - i, n - 2 - j) + xx(kMaxLag - 2 - i, kMaxLag - 2 - j);
    for (int i = 1; i < p; ++i)
        for (int j = std::max(i, kDense); j < p; ++j) cov_[i][j] = dot(x + kMaxLag - lag(i), x + kMaxLag - lag(j), rows);
    double trace = 0;
    for (int i = 0; i < p; ++i) {
        rhs_[i] = dot(x + kMaxLag, x + kMaxLag - lag(i), rows);
        trace += cov_[i][i];
    }
    // Cholesky, with the diagonal raised by 1e-4 of its mean: that keeps
    // the poles the codec's noise makes inside the unit circle (with a
    // billionth, a kick's tail grew a ring at 150 Hz by 20 dB in 80 ms).
    const double load = 1e-4 * trace / p + 1e-30;
    for (int i = 0; i < p; ++i) {
        for (int j = 0; j <= i; ++j) {
            double v = (j < i ? cov_[j][i] : cov_[i][i] + load);   // the upper triangle holds R
            for (int k = 0; k < j; ++k) v -= chol_[i][k] * chol_[j][k];
            if (j < i) {
                chol_[i][j] = v / chol_[j][j];
            } else if (v > 0) {
                chol_[i][i] = std::sqrt(v);
            } else {
                return;                          // silence
            }
        }
    }
    double* a = a_;
    for (int i = 0; i < p; ++i) {                // L z = r, then L' a = z
        double v = rhs_[i];
        for (int k = 0; k < i; ++k) v -= chol_[i][k] * a[k];
        a[i] = v / chol_[i][i];
    }
    for (int i = p - 1; i >= 0; --i) {
        double v = a[i];
        for (int k = i + 1; k < p; ++k) v -= chol_[k][i] * a[k];
        a[i] = v / chol_[i][i];
    }
    for (int c = 0; c < 2; ++c) {                // y[t - 1 - j] at kRing - 1 - j
        const float* h = hist_[c] + kFit - n;
        for (int j = 0; j < kRing; ++j) y_[c][kRing - 1 - j] = j < n ? h[n - 1 - j] : 0.0;
    }
    at_ = 0;                                     // y_[c][(at_ - k) & (kRing - 1)] is y[t - k]
    order_ = kOrder;
}

float Concealer::predict(int c) {
    const double* a = a_;
    double* y = y_[c];
    double s = 0;
    for (int i = 0; i < kOrder; ++i) s += a[i] * y[(at_ - lag(i)) & (kRing - 1)];
    s = std::max(-4.0, std::min(4.0, s));
    y[at_ & (kRing - 1)] = s;
    return static_cast<float>(s);
}

void Concealer::next(float* out, int frames) {
    if (state_ == Off) fit();
    state_ = Concealing;
    joined_ = 0;
    if (order_ == 0 || done_ >= limit_ + kGuard + kGoesOn) {
        std::memset(out, 0, sizeof(float) * 2 * static_cast<std::size_t>(frames));
        remember(out, frames);
        done_ += frames;
        return;
    }
    double ms = 0;
    for (int i = 0; i < frames; ++i) {
        out[2 * i] = predict(0);
        out[2 * i + 1] = predict(1);
        at_ = (at_ + 1) & (kRing - 1);
        ms += static_cast<double>(out[2 * i]) * out[2 * i] + static_cast<double>(out[2 * i + 1]) * out[2 * i + 1];
    }
    ms /= 2.0 * frames;
    // Never above the level played last (0.5 dB of the guard's own
    // ripple aside): the gain moves there over this block.
    const double over = ms_ref_ * 1.122;
    const float to = ms > over ? static_cast<float>(std::sqrt(over / ms)) : 1.0f;
    for (int i = 0; i < frames; ++i) {
        const float g = gain_ + (to - gain_) * static_cast<float>(i + 1) / frames;
        out[2 * i] *= g;
        out[2 * i + 1] *= g;
    }
    gain_ = to;
    remember(out, frames);                       // the stream's best guess, heard or not
    if (done_ + frames > limit_)
        for (int i = 0; i < frames; ++i) {
            const float e = ending(done_ + i);
            out[2 * i] *= e;
            out[2 * i + 1] *= e;
        }
    done_ += frames;
}

void Concealer::cut() {
    if (state_ == Off) fit();
    state_ = Concealing;
    joined_ = 0;
}

void Concealer::recent(float* out, int frames) const {
    for (int i = 0; i < frames; ++i) {
        const int back = frames - i;             // 1: the newest
        const bool have = back <= std::min(hist_n_, kFit);
        out[2 * i] = have ? hist_[0][kFit - back] : 0.0f;
        out[2 * i + 1] = have ? hist_[1][kFit - back] : 0.0f;
    }
}

void Concealer::peek(float* out, int frames) {
    const int at = at_;
    const bool on = state_ != Off && order_ > 0 && done_ < limit_ + kGuard + kGoesOn;
    for (int i = 0; i < frames; ++i) {
        float e[2] = {0.0f, 0.0f};
        if (on) {
            e[0] = predict(0) * gain_;
            e[1] = predict(1) * gain_;
            at_ = (at_ + 1) & (kRing - 1);
        }
        out[2 * i] = e[0];
        out[2 * i + 1] = e[1];
    }
    // What predict() wrote is written again, the same, when join() takes
    // it; and what it overwrote is further back than the furthest lag.
    at_ = at;
}

void Concealer::join(float* decoded, int frames) {
    remember(decoded, frames);
    if (state_ == Off) return;
    state_ = Joining;
    constexpr float kPi = 3.14159265359f;
    for (int i = 0; i < frames; ++i) {
        const int j = joined_ + i;
        if (j >= kFade) break;
        float e[2] = {0.0f, 0.0f};
        if (order_ > 0 && done_ < limit_ + kGuard) {
            const float g = gain_ * ending(done_++);
            e[0] = predict(0) * g;
            e[1] = predict(1) * g;
            at_ = (at_ + 1) & (kRing - 1);
        }
        const float w = 0.5f - 0.5f * std::cos(kPi * (static_cast<float>(j) + 0.5f) / kFade);
        for (int c = 0; c < 2; ++c) decoded[2 * i + c] = e[c] + w * (decoded[2 * i + c] - e[c]);
    }
    joined_ += frames;
    if (joined_ >= kFade) state_ = Off;
}

// ------------------------------------------------------------------ sink

LiveSink::LiveSink() : ring_(new Entry[kRing]), concealer_(new Concealer) {
    dec_mem_.reset(new unsigned char[static_cast<std::size_t>(opus_decoder_get_size(2))]);
    dec_ = reinterpret_cast<OpusDecoder*>(dec_mem_.get());
    opus_decoder_init(dec_, 48000, 2);
    // The primer codes for the decoder's state only, so it may take its
    // time over nothing but the pitch filter (which needs complexity 5)
    // and the energies; its bitrate makes them as exact as a stream's.
    enc_mem_.reset(new unsigned char[static_cast<std::size_t>(opus_encoder_get_size(2))]);
    enc_ = reinterpret_cast<OpusEncoder*>(enc_mem_.get());
    opus_encoder_init(enc_, 48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(256000));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
    opus_int32 la = 120;
    if (opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&la)) == OPUS_OK && la >= 0 && la <= 128) lookahead_ = la;
    sizesChanged();
}

LiveSink::~LiveSink() = default;

void LiveSink::sizesChanged() {
    sizes_ = sinkSizes(latency_ms_, host_block_, host_rate_, block_);
    concealer_->setLimit(concealSamples());
}

int LiveSink::concealSamples() const {
    return static_cast<int>((kLiveConcealBuffers * latency_ms_ + kLiveFadeMs + kLiveConcealSpareMs) * 48.0);
}

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
        has_align_ = realign_ = false;          // and so has a new delay
        fill_ref_ = -1;
        stats.active.store(false);
    }
}

void LiveSink::startTail() { tail48_ = concealSamples(); }

void LiveSink::reset() {
    // After a stream, what was decoded plays out, then the concealment
    // goes on from there, so that the mixer can fade it out without a step.
    const bool tail = phase_ != Idle || tail48_ > 0;
    if (phase_ != Idle) startTail();
    phase_ = Idle;
    stopping_ = false;
    for (int i = 0; i < kRing; ++i) ring_[i].state = Empty;
    next_ = 0;
    newest_ = -1;
    last_played_ = -1;
    far_ = false;
    if (!tail) fifo_n_ = fifo_at_ = 0;
    fill_ = fill_ref_ = -1;
    has_align_ = realign_ = false;
    stats.active.store(false);
}

void LiveSink::drop() {
    if (phase_ == Playing && !stopping_) {
        stopping_ = true;                        // as after a /stop
        return;
    }
    reset();
}

void LiveSink::emptyFrom(std::int32_t seq) {
    for (std::int64_t s = next_; s <= newest_ && s < next_ + kRing; ++s) ring_[s & (kRing - 1)].state = Empty;
    next_ = seq;
    newest_ = static_cast<std::int64_t>(seq) - 1;
    last_played_ = newest_;
    far_ = false;
}

void LiveSink::dropBefore(std::int64_t seq) {
    for (std::int64_t s = next_; s < seq && s <= newest_ && s < next_ + kRing; ++s)
        ring_[s & (kRing - 1)].state = Empty;
    if (seq <= next_) return;
    next_ = seq;
    last_played_ = seq - 1;
    if (newest_ < seq - 1) newest_ = seq - 1;
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
        fresh_ = it.seq == 0;                    // as its encoder began, unless it began before
        concealer_->clear();                     // a new stream: nothing to go on from
        emptyFrom(it.seq);
        phase_ = Buffering;
        stopping_ = false;
        waited_ = 0;
        fifo_n_ = fifo_at_ = 0;
        fill_ = fill_ref_ = -1;
        has_align_ = realign_ = false;          // a new stream finds its own delay
        stats.block.store(block_);
        stats.active.store(false);
        tail48_ = 0;
        return;
    case SinkItem::Stop:
        if (phase_ == Idle) return;
        stopping_ = true;
        if (phase_ == Dry || (phase_ == Buffering && heldBlocks() == 0)) {
            phase_ = Idle;                       // the stream ended; not an underrun
            startTail();
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
        has_align_ = realign_ = false;          // and its turns are counted afresh
    }
    if (phase_ == Dry) {                         // audio again after running dry
        if (stopping_) return;
        stats.underruns.fetch_add(1);
        emptyFrom(seq);
        phase_ = Buffering;
        waited_ = 0;
        // Back on the delay the stream had, so that it stays in time with
        // the other players (16.6): what comes too late for its turn goes,
        // the rest waits for its turn. Only a stream more than the capacity
        // ahead of it buffers afresh.
        realign_ = has_align_ && seq - turn() <= room();
        has_align_ = realign_;
        late48_ = 0;
    }
    if (realign_ && seq < turn()) {              // too late for its turn
        if (it.kind == SinkItem::Xrun) {         // but it still tells of the sender's gap (6.4)
            if (played48_ - last_xrun48_ > 4800) stats.xrun_gaps.fetch_add(1);
            last_xrun48_ = played48_;
        }
        dropBefore(static_cast<std::int64_t>(seq) + 1);
        return;
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
    } else if (static_cast<std::int64_t>(seq) - next_ + 1 <= room()) {
        take(seq, it, now_ns);
        return;
    }
    stats.overruns.fetch_add(1);                 // no room: only this block is kept
    stats.active.store(false);
    emptyFrom(seq);
    phase_ = Buffering;
    waited_ = 0;
    has_align_ = realign_ = false;
    take(seq, it, now_ns);
}

void LiveSink::conceal() { concealer_->next(fifo_, block_); }

// The audio comes back after concealment. The stream's encoder codes each
// frame's energies from those of the frame before (and filters by the pitch
// it found), so a decoder that only starts afresh decodes the first frames
// far too loud or too soft: a burst of noise, and a held note that dips by
// 8 dB. So the decoder is brought to the state it would have had if the
// stream had been what the concealment guessed: an encoder of the sink's
// own codes the last 1024 samples of that guess (CELT's longest pitch
// period) and some, up to now and on by the encoder's lookahead, and the
// decoder decodes them; then the block. After a short gap the guess is
// what was heard; after a long one it is the prediction that went on
// unheard, so that a held note comes back at its level (16.6).
void LiveSink::prime() {
    const int frame = block_;
    const int frames = std::min(kMaxPrime / frame, (kPrimeHistory + 2 * lookahead_ + frame - 1) / frame);
    const int total = frames * frame;
    concealer_->recent(prime_, total - lookahead_);
    concealer_->peek(prime_ + 2 * (total - lookahead_), lookahead_);
    opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    opus_decoder_ctl(dec_, OPUS_RESET_STATE);
    unsigned char packet[aoo::kMaxBlockBytes];
    for (int k = 0; k < frames; ++k) {
        const int n = opus_encode_float(enc_, prime_ + 2 * k * frame, frame, packet, sizeof(packet));
        if (n <= 0 || opus_decode_float(dec_, packet, n, fifo_, frame, 0) < 0) break;
    }
}

void LiveSink::decodeNext(std::int64_t now_ns) {
    fifo_at_ = 0;
    fifo_n_ = block_;
    fifo_real_ = false;
    if (phase_ == Buffering) {
        const int held = heldBlocks();
        if (realign_) {                          // after running dry: back on its delay
            const std::int64_t due = turn();
            dropBefore(std::min(due, newest_ + 1));   // what is too late for its turn goes
            if (next_ == due && next_ <= newest_) {
                realign_ = false;
                phase_ = Playing;                // and the fill it keeps stays the one it settled at
                stats.lost_at_active.store(stats.lost.load());
                stats.resent_at_active.store(stats.resent.load());
                stats.active_since_ns.store(now_ns);
                stats.active.store(true);
            } else if (newest_ < due && behind48_ < block_ && (late48_ += block_) >= sizes_.latency48) {
                // Late for good (its sender's host stopped, say): it buffers
                // afresh, on a delay of its own. The stream has jumped back,
                // and like a jump ahead (5.9) that counts as an overrun.
                // While this host catches up on a stall of its own, the
                // turns go by faster than the stream can come: not late.
                stats.overruns.fetch_add(1);
                realign_ = has_align_ = false;
                waited_ = 0;
            }
        } else if (waited_ >= sizes_.latency48 &&
                   (held - behindBlocks() >= sizes_.latency_blocks || (stopping_ && held > 0))) {
            // More than the latency: the oldest go. While the host catches
            // up on a stall, what it has yet to catch up on stays too: the
            // stream that came meanwhile.
            const int keep = sizes_.latency_blocks + behindBlocks();
            if (held > keep) {
                const std::int64_t skip = held - keep;
                for (std::int64_t s = next_; s < next_ + skip; ++s) ring_[s & (kRing - 1)].state = Empty;
                next_ += skip;
                last_played_ = next_ - 1;
            }
            const int buffered = std::min(std::max(0, heldBlocks() * block_ - behind48_), waited_);
            stats.buffer_us.store(static_cast<std::int32_t>(static_cast<std::int64_t>(buffered) * 1000000 / 48000));
            phase_ = Playing;
            fill_ = -1;                          // a new estimate, of the fill it already keeps once it has one
            stats.lost_at_active.store(stats.lost.load());
            stats.resent_at_active.store(stats.resent.load());
            stats.active_since_ns.store(now_ns);
            stats.active.store(true);
        }
        if (phase_ == Buffering) {
            if (!realign_ && waited_ >= 4 * sizes_.latency48) {   // gives up: an underrun, and waits again
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
        if (tail48_ > 0) {                       // after the stream: concealment to fade out
            conceal();
            tail48_ -= block_;
            break;
        }
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
                startTail();
                conceal();
                tail48_ -= block_;
                break;
            }
            phase_ = Dry;
            conceal();
            break;
        }
        align_ = played48_ - next_ * block_;
        has_align_ = true;
        Entry& e = ring_[next_ & (kRing - 1)];
        if (e.seq == next_ && e.state == Full) {
            if (concealer_->concealing() && !(fresh_ && next_ == 0)) prime();
            fresh_ = false;
            int n = opus_decode_float(dec_, e.data, e.len, fifo_, block_, 0);
            if (n < 0) {
                conceal();
            } else {
                if (n < block_) std::memset(fifo_ + 2 * n, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_ - n));
                clampDecoded(fifo_, 2 * static_cast<std::size_t>(block_));
                fifo_real_ = true;
                if (concealer_->concealing() || concealer_->joining())
                    concealer_->join(fifo_, block_);
                else
                    concealer_->played(fifo_, block_);
            }
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
    // What is left of the block decoded last goes, and whole blocks, to
    // the nearest of the time the host missed: the delay stays, and so
    // does the fill (to half a block), with the level it keeps.
    const int rest = fifo_n_ - fifo_at_;
    const double g48 = static_cast<double>(host_frames) * 48000.0 / host_rate - rest;
    const std::int64_t n = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(g48 / block_ + 0.5)));
    fifo_at_ = fifo_n_;
    for (std::int64_t s = next_; s < next_ + n && s <= newest_; ++s) ring_[s & (kRing - 1)].state = Empty;
    next_ += n;
    last_played_ = next_ - 1;
    if (newest_ < next_ - 1) newest_ = next_ - 1;
    align_ -= n * block_;                        // the time the host missed: the delay stays
    if ((n > 0 || rest > 0) && phase_ == Playing) concealer_->cut();   // the stream jumps: joined over
}

bool LiveSink::render(float* out, int frames, int rate, std::int64_t now_ns) {
    first_real_ = -1;
    quiet_from_ = 0;
    if (phase_ == Idle && tail48_ <= 0) {
        std::memset(out, 0, sizeof(float) * 2 * static_cast<std::size_t>(frames));
        return false;
    }
    double step = 48000.0 / rate;
    if (phase_ == Playing) {                     // the speed follows the fill (6.9)
        // While the host catches up on a stall, the stream that came
        // meanwhile isn't fill: it is what the host has yet to play. And
        // what the fill does then (the stream's sender may have stalled
        // too) says nothing of the clocks: the estimate holds until the host
        // has caught up.
        if (behind48_ == 0) {
            const double fill = (heldBlocks() * block_ + (fifo_n_ - fifo_at_)) / 48000.0;
            if (fill_ < 0) {                     // a new estimate: from this fill
                fill_ = fill;
                fill_age_ = 0;
            } else {
                fill_ += (fill - fill_) * std::min(1.0, frames / (rate * kFillSmoothing));
            }
            fill_age_ += static_cast<double>(frames) / rate;
        }
        // The level it settles at after a second is the one to keep: the
        // speed only makes up for the clocks' difference, so the stream
        // plays at its own pitch and the delay stays what buffering gave.
        // It is kept for the stream's life: after it buffers again (it
        // overflowed, or ran dry and was late for good), the speed brings
        // the delay back to it.
        if (fill_ >= 0 && fill_ref_ < 0 && fill_age_ >= kFillSettle) fill_ref_ = fill_;
        if (fill_ >= 0 && fill_ref_ >= 0 && fill_age_ >= kFillSmoothing) {
            const double e = fill_ - fill_ref_;  // within the dead band the fill's own jitter changes nothing
            const double off = e > kFillDeadBand ? e - kFillDeadBand : (e < -kFillDeadBand ? e + kFillDeadBand : 0.0);
            step *= 1.0 + std::max(-kMaxSpeedChange, std::min(kMaxSpeedChange, kFillGain * off));
        }
        stats.fill_us.store(fill_ >= 0 ? static_cast<std::int32_t>(std::lround(fill_ * 1e6)) : -1);
    } else {
        stats.fill_us.store(-1);
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
            xr_[0] = xr_[1];
            xr_[1] = xr_[2];
            xr_[2] = xr_[3];
            xr_[3] = fifo_real_;
            ++fifo_at_;
        }
        if (xr_[1]) {                            // this frame is mostly x_[1]
            if (first_real_ < 0) first_real_ = i;
            quiet_from_ = i + 1;
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
