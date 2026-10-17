// njproto engine - native FLAC encoder and decoder (ENGINE.md 6.4 and 8.1).
#include <algorithm>
#include <cstring>
#include <random>

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include "njproto/engine/codec.h"

namespace njproto {
namespace engine {

namespace {

constexpr std::size_t kMinHeaderBytes = 42;   // fLaC + block header + STREAMINFO

// ------------------------------------------------------------- encoder

class FlacEnc : public Encoder {
public:
    FlacEnc(int rate, int channels, int kbps) : Encoder(kFourccFlac, rate, channels, kbps) {
        enc_ = FLAC__stream_encoder_new();
        std::random_device rd;
        rng_.seed(rd());
    }
    ~FlacEnc() override {
        if (enc_) {
            if (open_) FLAC__stream_encoder_finish(enc_);
            FLAC__stream_encoder_delete(enc_);
        }
    }

    void begin() override {
        if (!enc_) return;
        if (open_) FLAC__stream_encoder_finish(enc_);
        FLAC__stream_encoder_set_channels(enc_, static_cast<unsigned>(channels_));
        FLAC__stream_encoder_set_bits_per_sample(enc_, 16);
        FLAC__stream_encoder_set_sample_rate(enc_, static_cast<unsigned>(rate_));
        FLAC__stream_encoder_set_compression_level(enc_, 5);
        FLAC__stream_encoder_set_verify(enc_, false);
        FLAC__stream_encoder_set_streamable_subset(enc_, FLAC__format_sample_rate_is_subset(static_cast<unsigned>(rate_)));
        FLAC__stream_encoder_set_total_samples_estimate(enc_, 0);
        open_ = FLAC__stream_encoder_init_stream(enc_, writeCb, nullptr, nullptr, nullptr, this) ==
                FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    }

    void encode(const float* interleaved, int frames) override {
        if (!open_) return;
        buf_.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_));
        for (std::size_t i = 0; i < buf_.size(); ++i) buf_[i] = floatToInt16(interleaved[i], rng_);
        FLAC__stream_encoder_process_interleaved(enc_, buf_.data(), static_cast<unsigned>(frames));
    }

    void finish() override {
        if (!open_) return;
        FLAC__stream_encoder_finish(enc_);   // writes the last, shorter frame
        open_ = false;
    }

private:
    static FLAC__StreamEncoderWriteStatus writeCb(const FLAC__StreamEncoder*, const FLAC__byte buffer[],
                                                  size_t bytes, uint32_t, uint32_t, void* client) {
        static_cast<FlacEnc*>(client)->emit(buffer, bytes);
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoder* enc_ = nullptr;
    bool open_ = false;
    std::vector<FLAC__int32> buf_;
    std::mt19937 rng_;
};

// ------------------------------------------------------------- decoder

// libFLAC cannot resume a frame it ran out of data in, so the decoder keeps
// the whole input, aborts the read when starved, flushes the library and
// retries from the frame's start once more bytes have arrived.
class FlacDec : public Decoder {
public:
    explicit FlacDec(std::shared_ptr<ByteStream> s) : Decoder(kFourccFlac, std::move(s)) {
        dec_ = FLAC__stream_decoder_new();
        rate_ = 44100;
        channels_ = 1;
        if (dec_) {
            FLAC__stream_decoder_set_md5_checking(dec_, false);
            inited_ = FLAC__stream_decoder_init_stream(dec_, readCb, nullptr, tellCb, nullptr, nullptr, writeCb,
                                                       metaCb, errorCb, this) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
        }
    }
    ~FlacDec() override {
        if (dec_) {
            FLAC__stream_decoder_finish(dec_);
            FLAC__stream_decoder_delete(dec_);
        }
    }

    bool decodeStep() override {
        if (finished_ || bad_ || !inited_) return false;
        std::size_t got = pullInput();
        if (!headers_done_) {
            if (in_.size() < kMinHeaderBytes) {
                if (in_ended_) finished_ = true;
                return false;
            }
            starved_ = false;
            consumed_ = 0;
            if (!FLAC__stream_decoder_process_until_end_of_metadata(dec_) || !headers_done_) {
                if (starved_ && !in_ended_) {
                    FLAC__stream_decoder_reset(dec_);      // retry from the start later
                    headers_done_ = false;
                    return false;
                }
                bad_ = true;
                return false;
            }
            got = 1;                                       // headers were progress
        }
        bool progress = got > 0;
        while (true) {
            if (consumed_ >= in_.size() && !in_ended_) break;   // nothing new to read
            if (stalled_ && !new_data_since_stall_) break;
            FLAC__uint64 frame_start = consumed_;
            FLAC__stream_decoder_get_decode_position(dec_, &frame_start);
            starved_ = false;
            wrote_ = false;
            FLAC__bool ok = FLAC__stream_decoder_process_single(dec_);
            FLAC__StreamDecoderState st = FLAC__stream_decoder_get_state(dec_);
            if (starved_) {
                if (in_ended_) {                           // truncated last frame
                    finished_ = true;
                    break;
                }
                FLAC__stream_decoder_flush(dec_);
                consumed_ = static_cast<std::size_t>(frame_start);
                stalled_ = true;
                new_data_since_stall_ = false;
                break;
            }
            stalled_ = false;
            if (wrote_) progress = true;
            if (!ok || st == FLAC__STREAM_DECODER_END_OF_STREAM || st == FLAC__STREAM_DECODER_ABORTED ||
                st == FLAC__STREAM_DECODER_OGG_ERROR || st == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR) {
                finished_ = true;
                break;
            }
            if (!wrote_ && consumed_ >= in_.size()) break;
        }
        return progress;
    }

private:
    static FLAC__StreamDecoderReadStatus readCb(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
                                                void* client) {
        auto* self = static_cast<FlacDec*>(client);
        std::size_t avail = self->in_.size() - self->consumed_;
        if (avail == 0) {
            *bytes = 0;
            if (self->in_ended_) return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
            self->starved_ = true;
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        std::size_t n = std::min(*bytes, avail);
        std::memcpy(buffer, self->in_.data() + self->consumed_, n);
        self->consumed_ += n;
        *bytes = n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }
    static FLAC__StreamDecoderTellStatus tellCb(const FLAC__StreamDecoder*, FLAC__uint64* pos, void* client) {
        *pos = static_cast<FlacDec*>(client)->consumed_;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }
    static void metaCb(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* m, void* client) {
        auto* self = static_cast<FlacDec*>(client);
        if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
        self->rate_ = static_cast<int>(m->data.stream_info.sample_rate);
        self->src_channels_ = static_cast<int>(m->data.stream_info.channels);
        self->channels_ = std::min(2, self->src_channels_);
        self->bits_ = static_cast<int>(m->data.stream_info.bits_per_sample);
        self->headers_done_ = true;
    }
    static FLAC__StreamDecoderWriteStatus writeCb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                                  const FLAC__int32* const buffer[], void* client) {
        auto* self = static_cast<FlacDec*>(client);
        self->wrote_ = true;
        const std::size_t ch = static_cast<std::size_t>(self->channels_);
        unsigned n = frame->header.blocksize;
        FLAC__uint64 sample = frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER
                                  ? static_cast<FLAC__uint64>(frame->header.number.frame_number) * self->first_blocksize(frame)
                                  : frame->header.number.sample_number;
        // A gap (a damaged frame was dropped) is filled with silence, at most 5 s.
        if (sample > self->expected_) {
            FLAC__uint64 gap = sample - self->expected_;
            FLAC__uint64 limit = static_cast<FLAC__uint64>(self->rate_) * 5;
            if (gap > limit) gap = limit;
            std::vector<float> z(static_cast<std::size_t>(gap) * ch, 0.0f);
            self->pushOutput(z.data(), static_cast<std::size_t>(gap));
        }
        self->expected_ = sample + n;
        int bits = frame->header.bits_per_sample ? static_cast<int>(frame->header.bits_per_sample) : self->bits_;
        float scale = 1.0f / static_cast<float>(1u << (bits - 1));
        float* dst = self->reserveOutput(n);
        if (ch == 1) {
            const FLAC__int32* src = buffer[0];
            for (unsigned i = 0; i < n; ++i) dst[i] = static_cast<float>(src[i]) * scale;
        } else {
            const FLAC__int32* s0 = buffer[0];
            const FLAC__int32* s1 = buffer[1];
            for (unsigned i = 0; i < n; ++i) {
                dst[static_cast<std::size_t>(i) * 2] = static_cast<float>(s0[i]) * scale;
                dst[static_cast<std::size_t>(i) * 2 + 1] = static_cast<float>(s1[i]) * scale;
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }
    static void errorCb(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

    unsigned first_blocksize(const FLAC__Frame* frame) {
        // With fixed block sizes every frame but the last has the first's size.
        if (!first_bs_) first_bs_ = frame->header.blocksize;
        return first_bs_;
    }

    FLAC__StreamDecoder* dec_ = nullptr;
    bool inited_ = false;
    bool bad_ = false;
    bool starved_ = false;
    bool stalled_ = false;
    bool new_data_since_stall_ = true;
    bool wrote_ = false;
    std::size_t consumed_ = 0;
    int src_channels_ = 1;
    int bits_ = 16;
    unsigned first_bs_ = 0;
    FLAC__uint64 expected_ = 0;

    // decodeStep uses pullInput through this wrapper so stalls notice new data.
    std::size_t pullInput() {
        std::size_t n = Decoder::pullInput();
        if (n) new_data_since_stall_ = true;
        return n;
    }
};

}  // namespace

std::unique_ptr<Encoder> makeOpusEncoder(int rate, int channels, int kbps);
std::unique_ptr<Decoder> makeOpusDecoder(std::shared_ptr<ByteStream> s);

std::unique_ptr<Encoder> makeEncoder(std::uint32_t fourcc, int rate, int channels, int kbps) {
    if (fourcc == kFourccOpusRaw) return makeOpusEncoder(rate, channels, kbps);
    if (fourcc == kFourccFlac) return std::make_unique<FlacEnc>(rate, channels, kbps);
    return nullptr;
}

std::unique_ptr<Decoder> makeDecoder(std::uint32_t fourcc, std::shared_ptr<ByteStream> s) {
    if (fourcc == kFourccOpusRaw) return makeOpusDecoder(std::move(s));
    if (fourcc == kFourccFlac) return std::make_unique<FlacDec>(std::move(s));
    return nullptr;
}

}  // namespace engine
}  // namespace njproto
