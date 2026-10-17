// njproto engine - DSP helpers: float conversion, LPC extrapolation, the
// windowed-sinc resampler, and the shared encoder/decoder buffers.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "njproto/engine/codec.h"

namespace njproto {
namespace engine {

// -------------------------------------------------------- float -> int16

std::int16_t floatToInt16(float v, std::mt19937& rng) {
    if (v == 0.0f || std::isnan(v)) return 0;    // digital silence stays exact
    float x = v * 32768.0f;
    // A value that already sits on a 16-bit step is kept as it is, so audio
    // that was quantised before stays bit-exact (ENGINE.md 13.3); anything
    // else gets TPDF dither of 1 LSB.
    if (x != std::floor(x)) {
        std::uniform_real_distribution<float> uni(0.0f, 1.0f);
        x += uni(rng) + uni(rng) - 1.0f;
    }
    long r = std::lround(x);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return static_cast<std::int16_t>(r);
}

// ------------------------------------------------------------------- LPC

void lpcExtrapolate(const float* history, int n, int order, float* out, int count) {
    if (count <= 0) return;
    if (n <= order + 1) {
        std::fill(out, out + count, 0.0f);
        return;
    }
    // Autocorrelation.
    std::vector<double> r(static_cast<std::size_t>(order) + 1, 0.0);
    for (int k = 0; k <= order; ++k) {
        double s = 0;
        for (int i = k; i < n; ++i) s += static_cast<double>(history[i]) * history[i - k];
        r[static_cast<std::size_t>(k)] = s;
    }
    if (r[0] <= 1e-12) {                          // silence
        std::fill(out, out + count, 0.0f);
        return;
    }
    // Mild lag window (Gaussian, ~20 Hz at 48 kHz) and white-noise correction.
    for (int k = 1; k <= order; ++k) {
        double w = 2.0 * M_PI * 20.0 * k / 48000.0;
        r[static_cast<std::size_t>(k)] *= std::exp(-0.5 * w * w);
    }
    r[0] *= 1.0 + 1e-5;
    // Levinson-Durbin.
    std::vector<double> a(static_cast<std::size_t>(order) + 1, 0.0), tmp(a);
    double err = r[0];
    for (int i = 1; i <= order; ++i) {
        double acc = r[static_cast<std::size_t>(i)];
        for (int j = 1; j < i; ++j) acc -= a[static_cast<std::size_t>(j)] * r[static_cast<std::size_t>(i - j)];
        double k = err > 0 ? acc / err : 0.0;
        tmp = a;
        a[static_cast<std::size_t>(i)] = k;
        for (int j = 1; j < i; ++j)
            a[static_cast<std::size_t>(j)] = tmp[static_cast<std::size_t>(j)] - k * tmp[static_cast<std::size_t>(i - j)];
        err *= (1.0 - k * k);
        if (err <= 0) break;
    }
    // Bandwidth expansion for stability.
    double g = 1.0;
    for (int i = 1; i <= order; ++i) {
        g *= 0.998;
        a[static_cast<std::size_t>(i)] *= g;
    }
    // Extrapolate: y[t] = sum a[j] * y[t-j]
    std::vector<double> y(history + n - order, history + n);   // last `order` samples
    for (int t = 0; t < count; ++t) {
        double s = 0;
        for (int j = 1; j <= order; ++j) s += a[static_cast<std::size_t>(j)] * y[y.size() - static_cast<std::size_t>(j)];
        if (s > 4.0) s = 4.0;
        if (s < -4.0) s = -4.0;
        out[t] = static_cast<float>(s);
        y.push_back(s);
    }
}

// ------------------------------------------------------------- resampler

namespace {
constexpr int kTaps = 32;                        // each side
constexpr int kPhases = 512;
}  // namespace

void Resampler::reset(int in_rate, int out_rate, int channels) {
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    channels_ = channels;
    buf_.clear();
    j_ = 0;
    origin_ = 0;
    if (passthrough()) return;
    // Prime with 2*kTaps frames of silence and start kTaps in: the output
    // then lags the input by a constant kTaps input frames and an interval of
    // N frames yields N * out/in output frames (give or take one) instead of
    // losing the filter's lookahead at the end of each interval.
    buf_.assign(static_cast<std::size_t>(2 * kTaps) * static_cast<std::size_t>(channels), 0.0f);
    origin_ = kTaps;
    // Table of windowed sinc: table_[phase * 2*kTaps + t] is the weight of
    // input sample floor(pos) - kTaps + 1 + t for fractional phase/kPhases.
    double cut = std::min(1.0, static_cast<double>(out_rate) / in_rate);   // anti-alias when downsampling
    table_.assign(static_cast<std::size_t>(kPhases + 1) * 2 * kTaps, 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
        double frac = static_cast<double>(p) / kPhases;
        double sum = 0;
        std::vector<double> w(2 * kTaps);
        for (int t = 0; t < 2 * kTaps; ++t) {
            double x = (t - kTaps + 1) - frac;      // distance from the output position
            double s = x == 0.0 ? 1.0 : std::sin(M_PI * cut * x) / (M_PI * cut * x);
            double u = x / kTaps;                   // -1..1
            double win = u <= -1.0 || u >= 1.0 ? 0.0 : 0.42 + 0.5 * std::cos(M_PI * u) + 0.08 * std::cos(2 * M_PI * u);
            w[static_cast<std::size_t>(t)] = s * win * cut;
            sum += w[static_cast<std::size_t>(t)];
        }
        for (int t = 0; t < 2 * kTaps; ++t)         // unity DC gain
            table_[static_cast<std::size_t>(p) * 2 * kTaps + static_cast<std::size_t>(t)] =
                static_cast<float>(w[static_cast<std::size_t>(t)] / sum);
    }
}

void Resampler::process(const float* in, int frames, std::vector<float>& out) {
    const std::size_t ch = static_cast<std::size_t>(channels_);
    if (passthrough()) {
        out.insert(out.end(), in, in + static_cast<std::size_t>(frames) * ch);
        return;
    }
    buf_.insert(buf_.end(), in, in + static_cast<std::size_t>(frames) * ch);
    const std::size_t have = buf_.size() / ch;
    // Output frame j is at input position origin_ + j*in/out and needs input
    // frames floor(pos)-kTaps+1 .. floor(pos)+kTaps.
    while (true) {
        long num = j_ * in_rate_;
        long fl = origin_ + num / out_rate_;
        long base = fl - kTaps + 1;
        if (fl + kTaps >= static_cast<long>(have)) break;
        double frac = static_cast<double>(num % out_rate_) / out_rate_;
        double pf = frac * kPhases;
        int p0 = static_cast<int>(pf);
        float pw = static_cast<float>(pf - p0);
        const float* t0 = &table_[static_cast<std::size_t>(p0) * 2 * kTaps];
        const float* t1 = &table_[static_cast<std::size_t>(p0 + 1) * 2 * kTaps];
        for (std::size_t c = 0; c < ch; ++c) {
            float acc = 0;
            for (int t = 0; t < 2 * kTaps; ++t) {
                long idx = base + t;
                float x = idx < 0 ? 0.0f : buf_[static_cast<std::size_t>(idx) * ch + c];
                acc += x * (t0[t] + (t1[t] - t0[t]) * pw);
            }
            out.push_back(acc);
        }
        if (++j_ == out_rate_) {                    // one second of output: fold the index
            j_ = 0;
            origin_ += in_rate_;
        }
    }
    // Drop history that is no longer needed.
    long keep_from = origin_ + (j_ * in_rate_) / out_rate_ - kTaps;
    if (keep_from > 4096) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(keep_from) * ch));
        origin_ -= keep_from;
    }
}

// ------------------------------------------------------- encoder buffer

void Encoder::emit(const std::uint8_t* data, std::size_t n) {
    out_.insert(out_.end(), data, data + n);
}

void Encoder::consume(std::size_t n) {
    out_pos_ += n;
    if (out_pos_ >= out_.size()) {
        out_.clear();
        out_pos_ = 0;
    } else if (out_pos_ > 65536) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_pos_));
        out_pos_ = 0;
    }
}

// ------------------------------------------------------------ ByteStream

void ByteStream::append(const std::uint8_t* data, std::size_t n) {
    std::lock_guard<std::mutex> lock(m_);
    pending_.insert(pending_.end(), data, data + n);
    total_ += n;
}

void ByteStream::setEnded() {
    std::lock_guard<std::mutex> lock(m_);
    ended_ = true;
}

bool ByteStream::take(std::vector<std::uint8_t>& into) {
    std::lock_guard<std::mutex> lock(m_);
    if (!pending_.empty()) {
        into.insert(into.end(), pending_.begin(), pending_.end());
        pending_.clear();
    }
    return ended_;
}

bool ByteStream::ended() const {
    std::lock_guard<std::mutex> lock(m_);
    return ended_;
}

std::size_t ByteStream::total() const {
    std::lock_guard<std::mutex> lock(m_);
    return total_;
}

// --------------------------------------------------------------- Decoder

std::size_t Decoder::pullInput() {
    std::size_t before = in_.size();
    in_ended_ = stream_->take(in_) || in_ended_;
    return in_.size() - before;
}

void Decoder::pushOutput(const float* interleaved, std::size_t frames) {
    out_.insert(out_.end(), interleaved, interleaved + frames * static_cast<std::size_t>(channels_));
}

void Decoder::skip(std::size_t frames) {
    rd_ += frames * static_cast<std::size_t>(channels_);
    if (rd_ > out_.size()) rd_ = out_.size();
    if (rd_ == out_.size()) {
        out_.clear();
        rd_ = 0;
    } else if (rd_ > 1u << 16) {
        out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(rd_));
        rd_ = 0;
    }
}

void Decoder::decodeUntil(std::size_t frames) {
    while (available() < frames && !finished_) {
        if (!decodeStep()) break;
    }
}

}  // namespace engine
}  // namespace njproto
