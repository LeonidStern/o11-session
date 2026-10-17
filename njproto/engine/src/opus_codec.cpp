// njproto engine - Ogg Opus encoder and decoder (ENGINE.md 6.4 and 8.1).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include <ogg/ogg.h>
#include <opus/opus.h>

#include "njproto/engine/codec.h"

namespace njproto {
namespace engine {

namespace {

const char* kVendor = "njproto engine";

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

// ------------------------------------------------------------- encoder

class OpusEnc : public Encoder {
public:
    OpusEnc(int rate, int channels, int kbps) : Encoder(kFourccOpus, rate, channels, kbps) {
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
        std::random_device rd;
        rng_.seed(rd());
        std::memset(&os_, 0, sizeof(os_));
    }
    ~OpusEnc() override {
        if (stream_open_) ogg_stream_clear(&os_);
        if (enc_) opus_encoder_destroy(enc_);
    }

    void begin() override {
        if (!enc_) return;
        if (stream_open_) ogg_stream_clear(&os_);
        ogg_stream_init(&os_, static_cast<int>(rng_()));
        stream_open_ = true;
        opus_encoder_ctl(enc_, OPUS_RESET_STATE);
        pending_.clear();
        granule_ = 0;
        real_ = 0;
        packetno_ = 0;
        history_.assign(static_cast<std::size_t>(kLpcHistory) * static_cast<std::size_t>(channels_), 0.0f);

        // OpusHead (RFC 7845 5.1): 19 bytes for mapping family 0.
        std::uint8_t head[19];
        std::memcpy(head, "OpusHead", 8);
        head[8] = 1;
        head[9] = static_cast<std::uint8_t>(channels_);
        put16(head + 10, static_cast<std::uint16_t>(pre_skip_));
        put32(head + 12, static_cast<std::uint32_t>(rate_));
        put16(head + 16, 0);                                 // output gain
        head[18] = 0;                                        // mapping family
        packetIn(head, sizeof(head), 0, true, false);
        // OpusTags: vendor string, no comments.
        std::vector<std::uint8_t> tags(8);
        std::memcpy(tags.data(), "OpusTags", 8);
        std::uint8_t len[4];
        put32(len, static_cast<std::uint32_t>(std::strlen(kVendor)));
        tags.insert(tags.end(), len, len + 4);
        tags.insert(tags.end(), kVendor, kVendor + std::strlen(kVendor));
        put32(len, 0);
        tags.insert(tags.end(), len, len + 4);
        packetIn(tags.data(), tags.size(), 0, false, false);
        flushPages();
    }

    void encode(const float* interleaved, int frames) override {
        if (!enc_ || !stream_open_) return;
        std::vector<float> rs;
        resampler_.process(interleaved, frames, rs);
        if (rs.empty()) return;
        real_ += static_cast<long>(rs.size() / static_cast<std::size_t>(channels_));
        pending_.insert(pending_.end(), rs.begin(), rs.end());
        rememberHistory(rs);
        encodeWholeFrames(false);
        writePages();
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
        encodeWholeFrames(true);
        flushPages();
        ogg_stream_clear(&os_);
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

    // Encodes every whole 960-frame block in pending_. With `last`, pending_
    // is a multiple of 960 and the final packet gets granule real+pre_skip
    // and the end-of-stream flag.
    void encodeWholeFrames(bool last) {
        const std::size_t ch = static_cast<std::size_t>(channels_);
        const std::size_t need = static_cast<std::size_t>(kOpusFrame) * ch;
        std::size_t off = 0;
        unsigned char packet[4000];
        while (pending_.size() - off >= need) {
            int n = opus_encode_float(enc_, pending_.data() + off, kOpusFrame, packet, sizeof(packet));
            off += need;
            if (n < 0) break;
            granule_ += kOpusFrame;
            bool eos = last && (pending_.size() - off < need);
            ogg_int64_t gp = eos ? static_cast<ogg_int64_t>(real_ + pre_skip_) : granule_;
            packetIn(packet, static_cast<std::size_t>(n), gp, false, eos);
            if (!last) writePages();
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(off));
    }

    void packetIn(const std::uint8_t* data, std::size_t len, ogg_int64_t granule, bool bos, bool eos) {
        ogg_packet op;
        op.packet = const_cast<unsigned char*>(data);
        op.bytes = static_cast<long>(len);
        op.b_o_s = bos ? 1 : 0;
        op.e_o_s = eos ? 1 : 0;
        op.granulepos = granule;
        op.packetno = packetno_++;
        ogg_stream_packetin(&os_, &op);
    }
    void writePages() {
        ogg_page og;
        while (ogg_stream_pageout(&os_, &og) > 0) {
            emit(og.header, static_cast<std::size_t>(og.header_len));
            emit(og.body, static_cast<std::size_t>(og.body_len));
        }
    }
    void flushPages() {
        ogg_page og;
        while (ogg_stream_flush(&os_, &og) > 0) {
            emit(og.header, static_cast<std::size_t>(og.header_len));
            emit(og.body, static_cast<std::size_t>(og.body_len));
        }
    }

    OpusEncoder* enc_ = nullptr;
    ogg_stream_state os_;
    bool stream_open_ = false;
    int pre_skip_ = 0;
    Resampler resampler_;
    std::vector<float> pending_;       // resampled, interleaved, not yet encoded
    std::vector<float> history_;       // last 1024 frames for the LPC padding
    long granule_ = 0;                 // 48 kHz samples encoded so far
    long real_ = 0;                    // real (unpadded) samples this interval
    ogg_int64_t packetno_ = 0;
    std::mt19937 rng_;
};

// ------------------------------------------------------------- decoder

class OpusDec : public Decoder {
public:
    explicit OpusDec(std::shared_ptr<ByteStream> s) : Decoder(kFourccOpus, std::move(s)) {
        ogg_sync_init(&oy_);
        std::memset(&os_, 0, sizeof(os_));
        rate_ = kOpusRate;
        channels_ = 1;
    }
    ~OpusDec() override {
        if (dec_) opus_decoder_destroy(dec_);
        if (stream_open_) ogg_stream_clear(&os_);
        ogg_sync_clear(&oy_);
    }

    bool decodeStep() override {
        if (finished_ || bad_) return false;
        std::size_t before = in_.size();
        pullInput();
        if (in_.size() > before) {
            std::size_t n = in_.size() - before;
            char* buf = ogg_sync_buffer(&oy_, static_cast<long>(n));
            std::memcpy(buf, in_.data() + before, n);
            ogg_sync_wrote(&oy_, static_cast<long>(n));
            in_.clear();
        }
        bool progress = false;
        for (;;) {
            ogg_packet op;
            if (stream_open_ && ogg_stream_packetout(&os_, &op) == 1) {
                handlePacket(op);
                progress = true;
                if (finished_ || bad_) return progress;
                continue;
            }
            ogg_page og;
            int r = ogg_sync_pageout(&oy_, &og);
            if (r != 1) {
                if (r == 0 && in_ended_ && !progress) {
                    // No more data will come and no page is pending.
                    if (stream_open_ && ogg_stream_packetout(&os_, &op) != 1) finished_ = true;
                    else if (!stream_open_) finished_ = true;
                }
                return progress;
            }
            if (!stream_open_) {
                ogg_stream_init(&os_, ogg_page_serialno(&og));
                stream_open_ = true;
            }
            if (ogg_page_eos(&og)) {
                eos_known_ = true;
                eos_granule_ = ogg_page_granulepos(&og);
            }
            ogg_stream_pagein(&os_, &og);
            progress = true;
        }
    }

private:
    void handlePacket(const ogg_packet& op) {
        const std::uint8_t* d = op.packet;
        std::size_t n = static_cast<std::size_t>(op.bytes);
        if (packets_ == 0) {
            if (n < 19 || std::memcmp(d, "OpusHead", 8) != 0) {
                bad_ = true;
                return;
            }
            channels_ = d[9];
            pre_skip_ = get16(d + 10);
            std::int16_t gain = static_cast<std::int16_t>(get16(d + 16));
            gain_ = static_cast<float>(std::pow(10.0, gain / (20.0 * 256.0)));
            if (d[18] != 0 || channels_ < 1 || channels_ > 2) {
                bad_ = true;
                return;
            }
            int err = 0;
            dec_ = opus_decoder_create(kOpusRate, channels_, &err);
            if (!dec_) bad_ = true;
            ++packets_;
            return;
        }
        if (packets_ == 1) {
            ++packets_;                               // OpusTags
            headers_done_ = true;
            return;
        }
        ++packets_;
        if (!dec_) return;
        int frames = opus_decode_float(dec_, d, static_cast<opus_int32>(n), pcm_, 5760, 0);
        if (frames <= 0) return;
        const std::size_t ch = static_cast<std::size_t>(channels_);
        if (gain_ != 1.0f)
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * ch; ++i) pcm_[i] *= gain_;
        for (int i = 0; i < frames; ++i) {
            long idx = decoded_ + i;                  // absolute 48 kHz position
            if (idx < pre_skip_) continue;
            const float* f = pcm_ + static_cast<std::size_t>(i) * ch;
            if (eos_known_ && idx >= eos_granule_) lapping_.insert(lapping_.end(), f, f + ch);
            else pushOutput(f, 1);
        }
        decoded_ += frames;
        if (op.e_o_s) finished_ = true;
    }

    ogg_sync_state oy_;
    ogg_stream_state os_;
    bool stream_open_ = false;
    OpusDecoder* dec_ = nullptr;
    int packets_ = 0;
    int pre_skip_ = 0;
    float gain_ = 1.0f;
    long decoded_ = 0;
    bool eos_known_ = false;
    long eos_granule_ = 0;
    bool bad_ = false;
    float pcm_[5760 * 2];
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
