// njproto engine - raw Opus (`OPUr`) encoder and decoder (ENGINE.md 6.4 and
// 8.1, PROTOCOL.md 7.8). The stream is the engine's own framing, no Ogg:
//   header  u8 version=1, u8 channels, u16 pre_skip, u32 input rate, u32 0
//   packets u16 length (1..65535), then one 960-sample Opus packet
//   trailer u16 0, u32 real (48 kHz samples after the pre-skip)
// All little-endian.
#include <algorithm>
#include <cmath>
#include <cstring>

#include <opus/opus.h>

#include "njproto/engine/codec.h"

namespace njproto {
namespace engine {

namespace {

constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kTrailerBytes = 6;
constexpr std::size_t kMaxPacketBytes = 65535;

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// ------------------------------------------------------------- encoder

class OpusEnc : public Encoder {
public:
    OpusEnc(int rate, int channels, int kbps) : Encoder(kFourccOpusRaw, rate, channels, kbps) {
        int err = 0;
        enc_ = opus_encoder_create(kOpusRate, channels, OPUS_APPLICATION_AUDIO, &err);
        if (enc_) {
            opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(10));
            opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
            opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(1));
            opus_encoder_ctl(enc_, OPUS_SET_BITRATE(kbps * 1000));
            opus_int32 la = 0;
            opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&la));
            pre_skip_ = static_cast<int>(la);
        }
        resampler_.reset(rate, kOpusRate, channels);
    }
    ~OpusEnc() override {
        if (enc_) opus_encoder_destroy(enc_);
    }

    void begin() override {
        if (!enc_) return;
        stream_open_ = true;
        opus_encoder_ctl(enc_, OPUS_RESET_STATE);
        pending_.clear();
        real_ = 0;
        history_.assign(static_cast<std::size_t>(kLpcHistory) * static_cast<std::size_t>(channels_), 0.0f);

        std::uint8_t head[kHeaderBytes];
        head[0] = 1;                                         // version
        head[1] = static_cast<std::uint8_t>(channels_);
        put16(head + 2, static_cast<std::uint16_t>(pre_skip_));
        put32(head + 4, static_cast<std::uint32_t>(rate_));  // input rate, informational
        put32(head + 8, 0);                                  // reserved
        emit(head, sizeof(head));
    }

    void encode(const float* interleaved, int frames) override {
        if (!enc_ || !stream_open_) return;
        std::vector<float> rs;
        resampler_.process(interleaved, frames, rs);
        if (rs.empty()) return;
        real_ += static_cast<long>(rs.size() / static_cast<std::size_t>(channels_));
        pending_.insert(pending_.end(), rs.begin(), rs.end());
        rememberHistory(rs);
        encodeWholeFrames();
    }

    void finish() override {
        if (!enc_ || !stream_open_) return;
        // Pad to the next multiple of 960 of real + pre_skip with an LPC
        // extrapolation (full level for 128 samples, then a raised-cosine fade).
        long total = real_ + pre_skip_;
        long target = ((total + kOpusFrame - 1) / kOpusFrame) * kOpusFrame;
        long pad = target - real_;
        if (pad > 0) {
            const std::size_t ch = static_cast<std::size_t>(channels_);
            std::vector<float> ext(static_cast<std::size_t>(pad) * ch);
            std::vector<float> hist(kLpcHistory), one(static_cast<std::size_t>(pad));
            for (std::size_t c = 0; c < ch; ++c) {
                for (int i = 0; i < kLpcHistory; ++i) hist[static_cast<std::size_t>(i)] = history_[static_cast<std::size_t>(i) * ch + c];
                lpcExtrapolate(hist.data(), kLpcHistory, kLpcOrder, one.data(), static_cast<int>(pad));
                for (long i = 0; i < pad; ++i) {
                    float g = 1.0f;
                    if (i >= kPaddingFullLevel && pad > kPaddingFullLevel) {
                        double u = static_cast<double>(i - kPaddingFullLevel) / static_cast<double>(pad - kPaddingFullLevel);
                        g = static_cast<float>(0.5 * (1.0 + std::cos(M_PI * u)));
                    }
                    ext[static_cast<std::size_t>(i) * ch + c] = one[static_cast<std::size_t>(i)] * g;
                }
            }
            pending_.insert(pending_.end(), ext.begin(), ext.end());
        }
        encodeWholeFrames();
        pending_.clear();
        std::uint8_t trailer[kTrailerBytes];
        put16(trailer, 0);
        put32(trailer + 2, static_cast<std::uint32_t>(real_));
        emit(trailer, sizeof(trailer));
        stream_open_ = false;
    }

private:
    void rememberHistory(const std::vector<float>& rs) {
        const std::size_t ch = static_cast<std::size_t>(channels_);
        std::size_t hframes = static_cast<std::size_t>(kLpcHistory);
        std::size_t nf = rs.size() / ch;
        if (nf >= hframes) {
            std::copy(rs.end() - static_cast<std::ptrdiff_t>(hframes * ch), rs.end(), history_.begin());
        } else {
            std::size_t keep = hframes - nf;
            std::copy(history_.begin() + static_cast<std::ptrdiff_t>(nf * ch), history_.end(), history_.begin());
            std::copy(rs.begin(), rs.end(), history_.begin() + static_cast<std::ptrdiff_t>(keep * ch));
        }
    }

    // Encodes every whole 960-frame block in pending_ and writes each packet
    // as soon as it is produced, with its u16 length in front.
    void encodeWholeFrames() {
        const std::size_t ch = static_cast<std::size_t>(channels_);
        const std::size_t need = static_cast<std::size_t>(kOpusFrame) * ch;
        std::size_t off = 0;
        unsigned char packet[4000];
        while (pending_.size() - off >= need) {
            int n = opus_encode_float(enc_, pending_.data() + off, kOpusFrame, packet, sizeof(packet));
            off += need;
            if (n <= 0 || static_cast<std::size_t>(n) > kMaxPacketBytes) break;
            std::uint8_t len[2];
            put16(len, static_cast<std::uint16_t>(n));
            emit(len, sizeof(len));
            emit(packet, static_cast<std::size_t>(n));
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(off));
    }

    OpusEncoder* enc_ = nullptr;
    bool stream_open_ = false;
    int pre_skip_ = 0;
    Resampler resampler_;
    std::vector<float> pending_;       // resampled, interleaved, not yet encoded
    std::vector<float> history_;       // last 1024 frames for the LPC padding
    long real_ = 0;                    // real (unpadded) samples this interval
};

// ------------------------------------------------------------- decoder

// The padding after `real` is shorter than one packet (pad - pre_skip < 960),
// so the lapping frames always lie inside the last packet. The decoder
// therefore holds back the output of the most recent packet until it sees
// what follows: another packet (the held one is whole output), the trailer
// (split at `real` into output and lapping) or the end of the input without
// a trailer (whole output, no lapping).
class OpusDec : public Decoder {
public:
    explicit OpusDec(std::shared_ptr<ByteStream> s) : Decoder(kFourccOpusRaw, std::move(s)) {
        rate_ = kOpusRate;
        channels_ = 1;
    }
    ~OpusDec() override {
        if (dec_) opus_decoder_destroy(dec_);
    }

    bool decodeStep() override {
        if (finished_ || bad_) return false;
        pullInput();
        bool progress = false;
        for (;;) {
            if (!headers_done_) {
                if (in_.size() - pos_ < kHeaderBytes) break;
                if (!parseHeader(in_.data() + pos_)) {
                    bad_ = true;
                    break;
                }
                pos_ += kHeaderBytes;
                progress = true;
                continue;
            }
            if (in_.size() - pos_ < 2) break;
            std::size_t len = get16(in_.data() + pos_);
            if (len == 0) {
                if (in_.size() - pos_ < kTrailerBytes) break;
                long real = static_cast<long>(get32(in_.data() + pos_ + 2));
                pos_ += kTrailerBytes;
                releaseHeld(pre_skip_ + real);
                finished_ = true;
                progress = true;
                break;
            }
            if (in_.size() - pos_ < 2 + len) break;
            const std::uint8_t* pkt = in_.data() + pos_ + 2;
            pos_ += 2 + len;
            progress = true;
            if (!decodePacket(pkt, len)) {
                bad_ = true;
                break;
            }
        }
        if (!finished_ && !bad_ && in_ended_) {
            // The loop stopped on incomplete data and no more will come (no
            // trailer): play to the last whole packet, no trimming.
            releaseHeld(-1);
            finished_ = true;
            progress = true;
        }
        if (bad_) releaseHeld(-1);
        if (pos_ > 0) {
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(pos_));
            pos_ = 0;
        }
        return progress;
    }

private:
    bool parseHeader(const std::uint8_t* h) {
        if (h[0] != 1) return false;
        int ch = h[1];
        if (ch < 1 || ch > 2) return false;
        channels_ = ch;
        pre_skip_ = get16(h + 2);
        int err = 0;
        dec_ = opus_decoder_create(kOpusRate, channels_, &err);
        if (!dec_) return false;
        headers_done_ = true;
        return true;
    }

    // Decodes one packet into held_, first releasing the packet held before it.
    bool decodePacket(const std::uint8_t* d, std::size_t n) {
        const std::size_t ch = static_cast<std::size_t>(channels_);
        int frames = opus_decode_float(dec_, d, static_cast<opus_int32>(n), pcm_, kMaxFrames, 0);
        if (frames <= 0) return false;
        releaseHeld(-1);
        held_start_ = decoded_;
        held_.assign(pcm_, pcm_ + static_cast<std::size_t>(frames) * ch);
        decoded_ += frames;
        return true;
    }

    // Outputs the held packet's frames from pre_skip on; with real_end >= 0
    // the frames at index >= real_end become the lapping frames instead.
    void releaseHeld(long real_end) {
        if (held_.empty()) return;
        const std::size_t ch = static_cast<std::size_t>(channels_);
        long end = held_start_ + static_cast<long>(held_.size() / ch);
        long first = std::max(held_start_, static_cast<long>(pre_skip_));
        long cut = real_end < 0 ? end : std::min(end, std::max(first, real_end));
        if (cut > first)
            pushOutput(held_.data() + static_cast<std::size_t>(first - held_start_) * ch, static_cast<std::size_t>(cut - first));
        if (end > cut) {
            const float* f = held_.data() + static_cast<std::size_t>(cut - held_start_) * ch;
            lapping_.insert(lapping_.end(), f, f + static_cast<std::size_t>(end - cut) * ch);
        }
        held_.clear();
    }

    static constexpr int kMaxFrames = 5760;   // 120 ms, the longest Opus packet
    OpusDecoder* dec_ = nullptr;
    std::size_t pos_ = 0;                     // consumed bytes of in_
    int pre_skip_ = 0;
    long decoded_ = 0;                        // 48 kHz frames decoded so far
    long held_start_ = 0;                     // stream index of held_[0]
    std::vector<float> held_;                 // the last decoded packet, not yet output
    bool bad_ = false;
    float pcm_[kMaxFrames * 2];
};

}  // namespace

std::unique_ptr<Encoder> makeOpusEncoder(int rate, int channels, int kbps) {
    return std::make_unique<OpusEnc>(rate, channels, kbps);
}
std::unique_ptr<Decoder> makeOpusDecoder(std::shared_ptr<ByteStream> s) {
    return std::make_unique<OpusDec>(std::move(s));
}

}  // namespace engine
}  // namespace njproto
