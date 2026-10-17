// njproto engine - encoders, decoders and the DSP helpers they need.
// Written from ENGINE.md sections 6.4 and 8.1 and the public headers of
// libopus, libogg and libFLAC.
#ifndef NJPROTO_ENGINE_CODEC_H
#define NJPROTO_ENGINE_CODEC_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include "njproto/message.h"

namespace njproto {
namespace engine {

constexpr int kOpusRate = 48000;
constexpr int kOpusFrame = 960;            // 20 ms
constexpr int kLpcOrder = 24;
constexpr int kLpcHistory = 1024;
constexpr int kPaddingFullLevel = 128;

// ---------------------------------------------------------------- DSP bits

// Float to 16-bit as in ENGINE.md 6.4: exact 0 (and NaN) -> 0, else x 32768,
// TPDF dither of 1 LSB, round to nearest, clip.
std::int16_t floatToInt16(float v, std::mt19937& rng);

// Levinson-Durbin fit of `order` coefficients to `n` samples (with a mild lag
// window and bandwidth expansion), then `count` extrapolated samples that
// continue `history`. Silence extrapolates to silence.
void lpcExtrapolate(const float* history, int n, int order, float* out, int count);

// Windowed-sinc resampler for interleaved float audio whose state persists
// across calls (ENGINE.md 6.4). Output lags the input by a few samples.
class Resampler {
public:
    void reset(int in_rate, int out_rate, int channels);
    bool passthrough() const { return in_rate_ == out_rate_; }
    // Appends the resampled frames to `out` (interleaved).
    void process(const float* in, int frames, std::vector<float>& out);

private:
    int in_rate_ = 0, out_rate_ = 0, channels_ = 1;
    // Output frame j sits at input position origin_ + j * in / out, kept as
    // integers so that the frame count per interval has no rounding drift.
    long j_ = 0;                           // next output frame index (mod out_rate_)
    long origin_ = 0;                      // input-frame position of output frame 0, relative to buf_
    std::vector<float> buf_;               // interleaved input history
    std::vector<float> table_;             // tap table, phases x taps
};

// ----------------------------------------------------------------- encoder

// One encoder per local channel; kept across intervals, restarted per interval.
class Encoder {
public:
    virtual ~Encoder() = default;
    std::uint32_t fourcc() const { return fourcc_; }
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    int kbps() const { return kbps_; }

    virtual void begin() = 0;                                  // new stream
    virtual void encode(const float* interleaved, int frames) = 0;
    virtual void finish() = 0;                                 // end the stream

    // Encoded bytes ready to send; consume() removes the first n.
    std::size_t outputSize() const { return out_.size() - out_pos_; }
    const std::uint8_t* outputData() const { return out_.data() + out_pos_; }
    void consume(std::size_t n);

protected:
    Encoder(std::uint32_t fourcc, int rate, int channels, int kbps)
        : fourcc_(fourcc), rate_(rate), channels_(channels), kbps_(kbps) {}
    void emit(const std::uint8_t* data, std::size_t n);
    std::uint32_t fourcc_;
    int rate_, channels_, kbps_;

private:
    std::vector<std::uint8_t> out_;
    std::size_t out_pos_ = 0;
};

// Returns nullptr for an unsupported fourcc.
std::unique_ptr<Encoder> makeEncoder(std::uint32_t fourcc, int rate, int channels, int kbps);

// ----------------------------------------------------------------- decoder

// Bytes of one download, appended by the control context and taken by the
// decoder (in whichever context decodes) under a short lock.
class ByteStream {
public:
    void append(const std::uint8_t* data, std::size_t n);
    void append(const std::vector<std::uint8_t>& v) { append(v.data(), v.size()); }
    void setEnded();
    // Moves pending bytes into `into` (appending); returns whether the stream
    // has ended (no more bytes will ever come).
    bool take(std::vector<std::uint8_t>& into);
    bool ended() const;
    std::size_t total() const;             // bytes appended so far

private:
    mutable std::mutex m_;
    std::vector<std::uint8_t> pending_;
    bool ended_ = false;
    std::size_t total_ = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    std::uint32_t fourcc() const { return fourcc_; }
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    bool headersDone() const { return headers_done_; }

    std::size_t available() const { return (out_.size() - rd_) / static_cast<std::size_t>(channels_); }
    const float* data() const { return out_.data() + rd_; }   // interleaved frames
    void skip(std::size_t frames);
    // Decodes some more. Returns false when nothing could be decoded now
    // (waiting for data, or the stream is finished / bad).
    virtual bool decodeStep() = 0;
    // Decode until available() >= frames or no progress is possible.
    void decodeUntil(std::size_t frames);
    bool finished() const { return finished_; }  // stream ended and fully decoded
    const std::vector<float>& lapping() const { return lapping_; }

protected:
    Decoder(std::uint32_t fourcc, std::shared_ptr<ByteStream> stream)
        : fourcc_(fourcc), stream_(std::move(stream)) {}
    // Pulls new input; returns the number of new bytes.
    std::size_t pullInput();
    void pushOutput(const float* interleaved, std::size_t frames);

    std::uint32_t fourcc_;
    std::shared_ptr<ByteStream> stream_;
    std::vector<std::uint8_t> in_;
    bool in_ended_ = false;
    int rate_ = kOpusRate;
    int channels_ = 1;
    bool headers_done_ = false;
    bool finished_ = false;
    std::vector<float> lapping_;

private:
    std::vector<float> out_;
    std::size_t rd_ = 0;
};

std::unique_ptr<Decoder> makeDecoder(std::uint32_t fourcc, std::shared_ptr<ByteStream> stream);

}  // namespace engine
}  // namespace njproto

#endif  // NJPROTO_ENGINE_CODEC_H
