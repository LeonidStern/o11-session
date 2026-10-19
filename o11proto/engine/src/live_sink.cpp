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
constexpr double kSpeedEpisodeGap = 1.0;              // seconds at the clock's own speed that end a correction (the log)

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
    late.store(0);
    late_missed.store(0);
    concealed48.store(0);
    skips.store(0);
    host_gaps.store(0);
    speed_changes.store(0);
    skipped48.store(0);
    host_gap48.store(0);
    speed_moved_ns.store(0);
    speed_ppm.store(0);
    fell_back.store(false);
    route.store(0);
    for (Event& e : events) e.mark.store(0);
    events_n.store(0);
    gen.store(g);
}

SinkSizes sinkSizes(double latency_ms, int host_block, int host_rate, int frame) {
    SinkSizes s;
    if (host_rate <= 0) host_rate = 48000;
    if (frame <= 0) frame = o11proto::live::kDefaultFrame;
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

void LiveSink::configure(double latency_ms, int host_block, int host_rate, double fallback_ms) {
    if (!(fallback_ms > latency_ms)) fallback_ms = 0;   // a fixed buffer
    if (latency_ms != base_ms_ || fallback_ms != fallback_ms_ || host_block != host_block_ || host_rate != host_rate_) {
        base_ms_ = latency_ms;
        fallback_ms_ = fallback_ms;
        if (fallback_ms_ == 0) fell_back_ = has_dropout_ = false;   // automatic again, it counts afresh
        const double was = latency_ms_;
        latency_ms_ = fell_back_ ? fallback_ms_ : base_ms_;   // a stream that fell back stays there
        host_block_ = host_block;
        host_rate_ = host_rate;
        sizesChanged();
        if (latency_ms_ != was && phase_ != Idle && !stopping_) {   // 6.8: a new buffer buffers again
            phase_ = Buffering;
            waited_ = 0;
            has_align_ = realign_ = false;      // and so has a new delay
            forgetGap();
            fill_ref_ = -1;
            owed48_ = 0;
            stats.active.store(false);
        }
    }
    stats.buffer_ms.store(static_cast<std::int32_t>(std::lround(latency_ms_)), std::memory_order_relaxed);
    stats.fell_back.store(fell_back_, std::memory_order_relaxed);
}

void LiveSink::startTail() { tail48_ = concealSamples(); }

void LiveSink::reset() {
    // After a stream, what was decoded plays out, then the concealment
    // goes on from there, so that the mixer can fade it out without a step.
    const bool tail = phase_ != Idle || tail48_ > 0;
    if (phase_ != Idle) {
        startTail();
        event(last_ns_, LiveEventKind::End, 0);
    }
    started_ = false;
    speed_on_ = false;
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
    absorbed_ = 0;
    forgetGap();
    path_room_ = false;
    path_ns_ = 0;
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
    absorbed_ = 0;
}

void LiveSink::dropBefore(std::int64_t seq) {
    if (seq > next_) advanceTo(seq);
}

void LiveSink::advanceTo(std::int64_t seq) {
    for (std::int64_t s = next_; s < seq && s <= newest_ && s < next_ + kRing; ++s) {
        Entry& e = ring_[s & (kRing - 1)];
        if (e.seq == s && e.state == Absorbed) {
            --absorbed_;
            align_ -= block_;                    // it takes no turn
        }
        e.state = Empty;
    }
    next_ = seq;
    last_played_ = seq - 1;
    if (newest_ < seq - 1) newest_ = seq - 1;
}

std::int64_t LiveSink::blocksOn(std::int64_t count) const {
    std::int64_t s = next_;
    for (;;) {
        if (s > newest_) return s + count;
        while (absorbedAt(s)) ++s;
        if (count == 0) return s;
        ++s;
        --count;
    }
}

std::int64_t LiveSink::due() const {
    const std::int64_t t = turn();
    if (absorbed_ == 0 || t < next_) return t;
    return blocksOn(t - next_);
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
    if (e.seq == seq && (e.state == Full || e.state == XrunBlock || e.state == Absorbed)) return;   // a duplicate
    const bool resent = e.seq == seq && e.state == Missing && e.requested;
    if (e.seq != seq) e.requested = false;      // the entry held another block
    e.seq = seq;
    e.state = it.kind == SinkItem::Xrun ? XrunBlock : Full;
    e.len = it.len;
    if (it.kind == SinkItem::Data) std::memcpy(e.data, it.data, it.len);
    if (resent) stats.resent.fetch_add(1);
    far_ = false;
}

void LiveSink::apply(const SinkItem& it, std::int64_t now_ns) {
    last_ns_ = now_ns;
    switch (it.kind) {
    case SinkItem::Start:
        block_ = it.block;
        fell_back_ = false;                      // a new stream starts at the buffer's first size
        latency_ms_ = base_ms_;
        owed48_ = 0;
        episode_ = counted_ = 0;
        has_dropout_ = concealed_last_ = false;
        stats.buffer_ms.store(static_cast<std::int32_t>(std::lround(latency_ms_)), std::memory_order_relaxed);
        stats.fell_back.store(false, std::memory_order_relaxed);
        sizesChanged();
        opus_decoder_ctl(dec_, OPUS_RESET_STATE);
        fresh_ = it.seq == 0;                    // as its encoder began, unless it began before
        concealer_->clear();                     // a new stream: nothing to go on from
        for (int i = 0; i < kRing; ++i) ring_[i].state = Empty;
        emptyFrom(it.seq);
        event(now_ns, LiveEventKind::Stream, it.seq);
        if (phase_ != Idle && started_) event(now_ns, LiveEventKind::End, 0);
        started_ = false;
        speed_on_ = false;
        phase_ = Buffering;
        stopping_ = false;
        waited_ = 0;
        fifo_n_ = fifo_at_ = 0;
        fill_ = fill_ref_ = -1;
        has_align_ = realign_ = false;          // a new stream finds its own delay
        forgetGap();
        path_room_ = false;
        path_ns_ = 0;
        stats.block.store(block_);
        stats.active.store(false);
        tail48_ = 0;
        return;
    case SinkItem::Stop:
        if (phase_ == Idle) return;
        stopping_ = true;
        if (phase_ == Dry || (phase_ == Buffering && heldBlocks() == 0)) {
            phase_ = Idle;                       // the stream ended; not an underrun
            event(now_ns, LiveEventKind::End, 0);
            startTail();
            stats.active.store(false);
        } else if (phase_ == Buffering) {
            waited_ = sizes_.latency48;          // gives up waiting: plays out what it has
        }
        return;
    case SinkItem::Reset:
        reset();
        return;
    case SinkItem::Path:
        pathChanged(it.seq, it.len != 0, now_ns);
        return;
    case SinkItem::Data:
    case SinkItem::Xrun:
        break;
    }
    if (phase_ == Idle || it.len > aoo::kMaxBlockBytes) return;
    std::int32_t seq = it.seq;
    if (seq < next_) {                           // at or before the last block played
        if (last_played_ - seq < (1LL << 30)) {  // old: late if its turn was concealed
            Entry& e = ring_[seq & (kRing - 1)];
            if (e.seq == seq && e.state == Passed) {
                e.state = Empty;
                stats.late.fetch_add(1);
                stats.late_missed.fetch_add(1);
                event(now_ns, LiveEventKind::Late, lateUs(seq));
                lateDropout(e.episode, e.requested, now_ns);
            }
            return;
        }
        emptyFrom(seq);                          // the stream wrapped
        has_align_ = realign_ = false;          // and its turns are counted afresh
        forgetGap();
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
        if (!realign_) {
            event(now_ns, LiveEventKind::Rebuffer, seq);
            forgetGap();                         // a delay of its own: nothing is owed to the old one
        }
        has_align_ = realign_;
        late48_ = 0;
    }
    // An xrun block while this host's gap is owed: absorbed, it takes no
    // turn (hostGap()). One more than is owed: the sender's gap was the
    // longer, and what it missed beyond this host's is a hole.
    bool absorbing = it.kind == SinkItem::Xrun && gap_open_ && owedNow() > absorbAbove();
    if (it.kind == SinkItem::Xrun && gap_open_ && !absorbing && gap_absorbed_ > 0 && !gap_longer_) {
        gap_longer_ = true;
        unabsorb(absorbAbove());
        absorbing = owedNow() > absorbAbove();
    }
    if (realign_ && !absorbing && seq < due()) {   // too late for its turn
        if (it.kind == SinkItem::Xrun) {         // but it still tells of the sender's gap (6.4)
            if (played48_ - last_xrun48_ > 4800) stats.xrun_gaps.fetch_add(1);
            last_xrun48_ = played48_;
        } else {
            const Entry& m = ring_[seq & (kRing - 1)];
            stats.late.fetch_add(1);
            event(now_ns, LiveEventKind::Late, lateUs(seq));
            lateDropout(episode_, m.seq == seq && m.state == Missing && m.requested, now_ns);
        }
        dropBefore(static_cast<std::int64_t>(seq) + 1);
        return;
    }
    if (seq <= newest_) {
        take(seq, it, now_ns);
        if (absorbing) absorb(seq);
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
        if (absorbing) absorb(seq);
        return;
    }
    stats.overruns.fetch_add(1);                 // no room: only this block is kept
    event(now_ns, LiveEventKind::Overflow, seq);
    stats.active.store(false);
    emptyFrom(seq);
    phase_ = Buffering;
    waited_ = 0;
    has_align_ = realign_ = false;
    forgetGap();
    owed48_ = 0;                                 // buffering afresh gives the buffer in use
    take(seq, it, now_ns);
}

void LiveSink::pathChanged(int route, bool change, std::int64_t now_ns) {
    stats.route.store(static_cast<std::uint8_t>(route), std::memory_order_relaxed);
    event(now_ns, LiveEventKind::Route, route);
    if (!change) return;
    path_ns_ = now_ns;
    path_room_ = true;
}

void LiveSink::conceal() {
    concealer_->next(fifo_, block_);
    if (started_ && phase_ != Idle) stats.concealed48.fetch_add(static_cast<std::uint64_t>(block_), std::memory_order_relaxed);
}

void LiveSink::concealHeard() {
    if (!concealed_last_) ++episode_;
    concealed_last_ = true;
    conceal();
}

// 6.8, the automatic buffer: a late block is a dropout a larger buffer
// would have saved; a lost one (and a block asked for again: it may be the
// answer, which comes a round trip late) is not.
void LiveSink::lateDropout(std::uint32_t episode, bool asked, std::int64_t now_ns) {
    if (asked || !started_ || fell_back_ || !(fallback_ms_ > base_ms_) || episode == 0 || episode == counted_) return;
    counted_ = episode;
    if (has_dropout_ && now_ns - dropout_ns_ <= kFallbackWindowNs) {
        fallBack(now_ns);
        return;
    }
    has_dropout_ = true;
    dropout_ns_ = now_ns;
}

// The larger buffer, at once: its sizes, and the fill the speed keeps. The
// delay grows by the difference while the audio is concealing (growNow(),
// growAlign()): at this dropout when it still conceals, else at the next.
// Until then the speed brings the fill up to the new level, inaudibly, and
// what it does is owed no more.
void LiveSink::fallBack(std::int64_t now_ns) {
    fell_back_ = true;
    const double extra = fallback_ms_ - latency_ms_;
    latency_ms_ = fallback_ms_;
    sizesChanged();
    owed48_ += extra * 48.0;
    if (fill_ref_ >= 0) fill_ref_ += extra / 1000.0;
    stats.buffer_ms.store(static_cast<std::int32_t>(std::lround(latency_ms_)), std::memory_order_relaxed);
    stats.fell_back.store(true, std::memory_order_relaxed);
    event(now_ns, LiveEventKind::Fallback, std::lround(latency_ms_));
}

bool LiveSink::growNow() {
    if (owed48_ * 2 <= block_ || !concealed_last_ || stopping_) return false;
    concealHeard();                              // a block more of it, and the stream waits a block
    owed48_ = std::max(0.0, owed48_ - block_);
    grown(block_);
    return true;
}

void LiveSink::growAlign() {
    if (owed48_ * 2 <= block_ || stopping_) return;
    const std::int64_t k = static_cast<std::int64_t>(std::floor(owed48_ / block_ + 0.5));
    align_ += k * block_;                        // each turn comes that much later
    owed48_ = std::max(0.0, owed48_ - static_cast<double>(k * block_));
    grown(static_cast<int>(k * block_));
}

void LiveSink::grown(int samples48) {
    if (fill_ >= 0) fill_ += samples48 / 48000.0;
    const std::int32_t b = stats.buffer_us.load(std::memory_order_relaxed);
    if (b >= 0) stats.buffer_us.store(b + static_cast<std::int32_t>(static_cast<std::int64_t>(samples48) * 1000000 / 48000));
}

std::int64_t LiveSink::lateUs(std::int64_t seq) const {
    const std::int64_t at48 = played48_ - (fifo_n_ - fifo_at_);   // where playback is now
    return std::max<std::int64_t>(0, at48 - (align_ + seq * block_)) * 1000 / 48;
}

void LiveSink::started(std::int64_t now_ns, std::int64_t value, bool rejoin) {
    phase_ = Playing;
    started_ = true;
    event(now_ns, rejoin ? LiveEventKind::Rejoin : LiveEventKind::Play, value);
    stats.lost_at_active.store(stats.lost.load());
    stats.resent_at_active.store(stats.resent.load());
    stats.active_since_ns.store(now_ns);
    stats.active.store(true);
}

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
    if (gap_open_ && gap_late48_ == 0 && played48_ >= gap_until48_) closeGap(now_ns);
    fifo_at_ = 0;
    fifo_n_ = block_;
    fifo_real_ = false;
    if (phase_ == Buffering) {
        const int held = heldBlocks();
        if (realign_) {                          // after running dry: back on its delay
            growAlign();
            const std::int64_t due = this->due();
            for (std::int64_t s = next_; s < due && s <= newest_ && s < next_ + kRing; ++s) {
                const Entry& e = ring_[s & (kRing - 1)];
                if (e.seq == s && e.state == Full) {
                    stats.late.fetch_add(1);
                    event(now_ns, LiveEventKind::Late, lateUs(s));
                    lateDropout(episode_, e.requested, now_ns);
                }
            }
            dropBefore(std::min(due, newest_ + 1));   // what is too late for its turn goes
            if (next_ == due && next_ <= newest_) {
                realign_ = false;
                started(now_ns, next_, true);    // and the fill it keeps stays the one it settled at
            } else if (newest_ < due && behind48_ < block_ && (late48_ += block_) >= sizes_.latency48) {
                // Late for good (its sender's host stopped, say): it buffers
                // afresh, on a delay of its own. The stream has jumped back,
                // and like a jump ahead (5.9) that counts as an overrun.
                // While this host catches up on a stall of its own, the
                // turns go by faster than the stream can come: not late.
                stats.overruns.fetch_add(1);
                event(now_ns, LiveEventKind::Rebuffer, due);
                realign_ = has_align_ = false;
                forgetGap();
                waited_ = 0;
                owed48_ = 0;                     // buffering afresh gives the buffer in use
            }
        } else if (waited_ >= sizes_.latency48 &&
                   (held - behindBlocks() >= sizes_.latency_blocks || (stopping_ && held > 0))) {
            // More than the latency: the oldest go. While the host catches
            // up on a stall, what it has yet to catch up on stays too: the
            // stream that came meanwhile.
            const int keep = sizes_.latency_blocks + behindBlocks();
            if (held > keep) {
                const std::int64_t skip = held - keep;
                advanceTo(blocksOn(skip));
                if (started_) {                  // buffering again: the position jumps
                    stats.skips.fetch_add(1);
                    stats.skipped48.fetch_add(skip * block_);
                    event(now_ns, LiveEventKind::Skip, skip * block_);
                }
            }
            const int buffered = std::min(std::max(0, heldBlocks() * block_ - behind48_), waited_);
            stats.buffer_us.store(static_cast<std::int32_t>(static_cast<std::int64_t>(buffered) * 1000000 / 48000));
            fill_ = -1;                          // a new estimate, of the fill it already keeps once it has one
            started(now_ns, stats.buffer_us.load(), false);
        }
        if (phase_ == Buffering) {
            if (!realign_ && waited_ >= 4 * sizes_.latency48) {   // gives up: an underrun, and waits again
                stats.underruns.fetch_add(1);
                waited_ = 0;
            }
            if (started_)
                concealHeard();
            else
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
        growAlign();
        concealHeard();
        break;
    case Playing: {
        if (growNow()) break;                    // the fallback's delay, while concealing
        if (absorbedAt(next_)) advanceTo(blocksOn(0));   // xrun blocks absorbed by this host's gap: no turn
        if (next_ > newest_) {                   // empty at a block's turn
            stats.active.store(false);
            if (stopping_) {
                phase_ = Idle;
                event(now_ns, LiveEventKind::End, 0);
                startTail();
                conceal();
                tail48_ -= block_;
                break;
            }
            phase_ = Dry;
            event(now_ns, LiveEventKind::Dry, next_);
            concealHeard();
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
                concealHeard();
            } else {
                concealed_last_ = false;
                if (n < block_) std::memset(fifo_ + 2 * n, 0, sizeof(float) * 2 * static_cast<std::size_t>(block_ - n));
                clampDecoded(fifo_, 2 * static_cast<std::size_t>(block_));
                fifo_real_ = true;
                if (concealer_->concealing() || concealer_->joining())
                    concealer_->join(fifo_, block_);
                else
                    concealer_->played(fifo_, block_);
            }
            e.state = Empty;
        } else if (e.seq == next_ && e.state == XrunBlock) {
            concealHeard();
            if (played48_ - last_xrun48_ > 4800) stats.xrun_gaps.fetch_add(1);   // one gap per 100 ms
            last_xrun48_ = played48_;
            event(now_ns, LiveEventKind::Xrun, next_);
            e.state = Empty;
        } else {
            concealHeard();                      // lost, unless it comes after all (late)
            stats.lost.fetch_add(1);
            event(now_ns, LiveEventKind::Lost, next_);
            if (e.seq != next_) e.requested = false;
            e.seq = static_cast<std::int32_t>(next_);
            e.state = Passed;
            e.episode = episode_;
        }
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

void LiveSink::hostLate(int host_frames, int host_rate, std::int64_t now_ns) {
    if (phase_ == Idle || host_rate <= 0 || !has_align_ || gap_late48_ > 0) return;
    gap_late48_ = std::max<std::int64_t>(1, std::llround(static_cast<double>(host_frames) * 48000.0 / host_rate));
    gap_owed48_ += gap_late48_;
    openGap(now_ns);
}

void LiveSink::hostGap(int host_frames, int host_rate, std::int64_t now_ns) {
    if (phase_ == Idle || host_rate <= 0) return;
    const std::int64_t g48 = std::llround(static_cast<double>(host_frames) * 48000.0 / host_rate);
    if (has_align_) {
        // The stream plays on where it was; what this host missed is owed
        // to its delay, and the sender's xrun blocks may take it back.
        if (started_) stats.host_gaps.fetch_add(1);
        gap_owed48_ += g48 - gap_late48_;
        gap_late48_ = 0;
        gap_confirmed_ = true;
        openGap(now_ns);
        return;
    }
    // A stream still finding its delay (buffering afresh): what is left of
    // the block decoded last goes, and whole blocks, to the nearest of the
    // time the host missed.
    forgetGap();
    const int rest = fifo_n_ - fifo_at_;
    const std::int64_t n = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor((g48 - rest) / static_cast<double>(block_) + 0.5)));
    fifo_at_ = fifo_n_;
    advanceTo(next_ + n);
    align_ -= n * block_;
    if ((n > 0 || rest > 0) && phase_ == Playing) concealer_->cut();   // the stream jumps: joined over
    if (started_) {
        stats.host_gaps.fetch_add(1);
        stats.host_gap48.fetch_add(n * block_ + rest);
        event(now_ns, LiveEventKind::HostGap, n * block_ + rest);
    }
}

void LiveSink::hostGapBack(int host_frames, int host_rate) {
    if (!gap_open_ || !gap_confirmed_ || host_frames <= 0 || host_rate <= 0) return;
    gap_back48_ += std::llround(static_cast<double>(host_frames) * 48000.0 / host_rate);
    unabsorb(absorbAbove());
}

void LiveSink::hostCaughtUp() {
    if (gap_late48_ == 0) return;
    gap_owed48_ -= gap_late48_;                  // nothing missed after all
    gap_late48_ = 0;
    if (!gap_confirmed_) {                       // no gap in this window: nothing is owed
        unabsorb(0);
        forgetGap();
        return;
    }
    unabsorb(absorbAbove());
}

void LiveSink::openGap(std::int64_t now_ns) {
    (void)now_ns;
    if (!gap_open_) gap_absorbed_ = 0;
    gap_open_ = true;
    gap_until48_ = played48_ + kGapWindow48;
    unabsorb(absorbAbove());
    absorbHeld();
}

void LiveSink::absorbHeld() {
    for (std::int64_t s = next_; s <= newest_ && s < next_ + kRing && owedNow() > absorbAbove(); ++s) {
        const Entry& e = ring_[s & (kRing - 1)];
        if (e.seq == s && e.state == XrunBlock) absorb(s);
    }
}

void LiveSink::absorb(std::int64_t seq) {
    Entry& e = ring_[seq & (kRing - 1)];
    if (e.seq != seq || e.state != XrunBlock || owedNow() <= absorbAbove()) return;
    e.state = Absorbed;
    ++absorbed_;
    ++gap_absorbed_;
    gap_owed48_ -= block_;
}

// While a block fewer would still leave no more owed than `above` (the host
// caught up after all, or missed less than it seemed, or the sender's gap
// was the longer), the newest absorbed blocks not yet passed are xrun
// blocks again.
void LiveSink::unabsorb(std::int64_t above) {
    for (std::int64_t s = newest_; s >= next_ && s > newest_ - kRing && owedNow() + block_ <= above && absorbed_ > 0; --s) {
        Entry& e = ring_[s & (kRing - 1)];
        if (e.seq != s || e.state != Absorbed) continue;
        e.state = XrunBlock;
        --absorbed_;
        if (gap_absorbed_ > 0) --gap_absorbed_;
        gap_owed48_ += block_;
    }
}

// The window closes. What the sender's xrun blocks didn't take back, less
// what the host caught up on after its report (hostGapBack()), is
// skipped of the stream's own audio: to the nearest block when none came
// (this host alone missed the time). When xrun blocks came, up to a block
// of what is owed is their rounding (whole frames: the sender carries the
// rest of its gap over to the next) and up to the slack the hosts' measures
// of the gap differing. Skipping that would take the delay below where it
// was, so it stays: what is left after the slack goes in whole blocks,
// rounded down.
void LiveSink::closeGap(std::int64_t now_ns) {
    const std::int64_t owed = gap_owed48_ - gap_back48_;
    const bool absorbed = gap_absorbed_ > 0;
    forgetGap();
    if (owed <= 0) return;
    const std::int64_t n = absorbed ? (owed - gapSlack48()) / block_
                                    : static_cast<std::int64_t>(std::floor(static_cast<double>(owed) / block_ + 0.5));
    if (n > 0) skipBlocks(n, now_ns);
}

void LiveSink::forgetGap() {
    gap_owed48_ = gap_late48_ = gap_back48_ = 0;
    gap_open_ = gap_confirmed_ = gap_longer_ = false;
    gap_absorbed_ = 0;
}

void LiveSink::skipBlocks(std::int64_t n, std::int64_t now_ns) {
    if (phase_ == Idle) return;
    advanceTo(blocksOn(n));
    align_ -= n * block_;                        // the time the host missed: the delay stays
    if (phase_ == Playing) concealer_->cut();    // the stream jumps: joined over
    if (started_) {
        stats.host_gap48.fetch_add(n * block_);
        event(now_ns, LiveEventKind::HostGap, n * block_);
    }
}

bool LiveSink::render(float* out, int frames, int rate, std::int64_t now_ns) {
    last_ns_ = now_ns;
    first_real_ = -1;
    quiet_from_ = 0;
    if (phase_ == Idle && tail48_ <= 0) {
        std::memset(out, 0, sizeof(float) * 2 * static_cast<std::size_t>(frames));
        return false;
    }
    double step = 48000.0 / rate;
    double speed = 0;                            // off the clock's own speed
    double error = 0;
    if (phase_ == Playing) {                     // the speed follows the fill (6.9)
        // While the host catches up on a stall, the stream that came
        // meanwhile isn't fill: it is what the host has yet to play. And
        // what the fill does then (the stream's sender may have stalled
        // too) says nothing of the clocks: the estimate holds until the host
        // has caught up.
        if (behind48_ == 0 && !gap_open_) {
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
        if (fill_ >= 0 && fill_ref_ < 0 && fill_age_ >= kFillSettle) fill_ref_ = fill_ + owed48_ / 48000.0;
        if (fill_ >= 0 && fill_ref_ >= 0 && fill_age_ >= kFillSmoothing) {
            const double e = fill_ - fill_ref_;  // within the dead band the fill's own jitter changes nothing
            const double off = e > kFillDeadBand ? e - kFillDeadBand : (e < -kFillDeadBand ? e + kFillDeadBand : 0.0);
            speed = std::max(-kMaxSpeedChange, std::min(kMaxSpeedChange, kFillGain * off));
            error = e;
            step *= 1.0 + speed;
            // Slower, the fill grows: that much of a fallback's delay is owed no more.
            if (speed < 0 && owed48_ > 0) owed48_ = std::max(0.0, owed48_ + speed * frames * 48000.0 / rate);
        }
        stats.fill_us.store(fill_ >= 0 ? static_cast<std::int32_t>(std::lround(fill_ * 1e6)) : -1);
        // The room a path change gave lasts until what it brought fits.
        if (path_room_ && now_ns - path_ns_ >= 1000000000LL && heldBlocks() < sizes_.capacity_blocks)
            path_room_ = false;
    } else {
        stats.fill_us.store(-1);
    }
    // The log's speed corrections: one from the speed leaving the clock's
    // own to a second after it last came back (at the dead band's edge it
    // comes and goes).
    if (speed != 0) {
        speed_idle_ = 0;
        if (!speed_on_) {
            speed_on_ = true;
            speed_peak_ = 0;
            stats.speed_changes.fetch_add(1);
            event(now_ns, LiveEventKind::SpeedOn, std::lround(error * 1e6));
        }
    } else if (speed_on_ && (speed_idle_ += static_cast<double>(frames) / rate) >= kSpeedEpisodeGap) {
        speed_on_ = false;
        event(now_ns, LiveEventKind::SpeedOff, std::lround(speed_peak_ * 1e6));
    }
    if (speed != 0) {
        if (std::fabs(speed) > std::fabs(speed_peak_)) speed_peak_ = speed;
        speed_moved_ns_ += speed * frames * 1e9 / rate;   // whole ns to the count, the rest carried
        const double whole = std::trunc(speed_moved_ns_);
        speed_moved_ns_ -= whole;
        stats.speed_moved_ns.fetch_add(static_cast<std::int64_t>(whole), std::memory_order_relaxed);
    }
    stats.speed_ppm.store(static_cast<std::int32_t>(std::lround(speed * 1e6)), std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        while (t_ >= 1.0) {
            t_ -= 1.0;
            if (fifo_at_ >= fifo_n_) decodeNext(now_ns + static_cast<std::int64_t>(i) * 1000000000LL / rate);
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
