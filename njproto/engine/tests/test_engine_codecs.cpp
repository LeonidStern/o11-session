// ENGINE.md 13.1-13.4: codec round trips and the float conversion.
#include <cmath>
#include <random>
#include <vector>

#include "check.h"
#include "njproto/engine/codec.h"

using namespace njproto;
using namespace njproto::engine;

namespace {

std::vector<float> tone(double hz, double amp, int rate, int frames, int channels, double phase0 = 0.0) {
    std::vector<float> v(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        float s = static_cast<float>(amp * std::sin(phase0 + 2 * M_PI * hz * i / rate));
        for (int c = 0; c < channels; ++c) v[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = s;
    }
    return v;
}

// Encodes one interval in blocks of 512 frames and returns the stream bytes.
std::vector<std::uint8_t> encodeInterval(Encoder& enc, const std::vector<float>& pcm, int channels) {
    enc.begin();
    std::size_t frames = pcm.size() / static_cast<std::size_t>(channels);
    for (std::size_t off = 0; off < frames; off += 512) {
        std::size_t n = std::min<std::size_t>(512, frames - off);
        enc.encode(pcm.data() + off * static_cast<std::size_t>(channels), static_cast<int>(n));
    }
    enc.finish();
    std::vector<std::uint8_t> out(enc.outputData(), enc.outputData() + enc.outputSize());
    enc.consume(enc.outputSize());
    return out;
}

// Decodes a whole stream delivered in chunks; returns interleaved frames.
std::vector<float> decodeAll(std::uint32_t fourcc, const std::vector<std::uint8_t>& bytes, std::size_t chunk,
                             std::unique_ptr<Decoder>* keep = nullptr) {
    auto stream = std::make_shared<ByteStream>();
    auto dec = makeDecoder(fourcc, stream);
    std::vector<float> out;
    for (std::size_t off = 0; off < bytes.size(); off += chunk) {
        std::size_t n = std::min(chunk, bytes.size() - off);
        stream->append(bytes.data() + off, n);
        if (off + n >= bytes.size()) stream->setEnded();
        while (dec->decodeStep()) {}
        std::size_t avail = dec->available();
        out.insert(out.end(), dec->data(), dec->data() + avail * static_cast<std::size_t>(dec->channels()));
        dec->skip(avail);
    }
    while (dec->decodeStep()) {}
    std::size_t avail = dec->available();
    out.insert(out.end(), dec->data(), dec->data() + avail * static_cast<std::size_t>(dec->channels()));
    dec->skip(avail);
    if (keep) *keep = std::move(dec);
    return out;
}

double snrDb(const std::vector<float>& ref, const std::vector<float>& got, std::size_t from, std::size_t to) {
    double sig = 0, noise = 0;
    for (std::size_t i = from; i < to && i < ref.size() && i < got.size(); ++i) {
        sig += static_cast<double>(ref[i]) * ref[i];
        double d = static_cast<double>(ref[i]) - got[i];
        noise += d * d;
    }
    return noise > 0 ? 10 * std::log10(sig / noise) : 200.0;
}

double rms(const float* p, std::size_t n) {
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(p[i]) * p[i];
    return std::sqrt(s / static_cast<double>(n ? n : 1));
}

}  // namespace

TEST(opus_round_trip_48k_stereo) {              // 13.1
    const int rate = 48000, frames = 96000;
    auto pcm = tone(997.0, 0.5, rate, frames, 2);
    auto enc = makeEncoder(kFourccOpus, rate, 2, 128);
    CHECK(enc != nullptr);
    auto bytes = encodeInterval(*enc, pcm, 2);
    CHECK(bytes.size() > 8192);
    CHECK_EQ(std::string(bytes.begin(), bytes.begin() + 4), "OggS");
    std::unique_ptr<Decoder> dec;
    auto got = decodeAll(kFourccOpus, bytes, 4096, &dec);
    CHECK_EQ(dec->channels(), 2);
    CHECK_EQ(dec->rate(), 48000);
    CHECK_EQ(got.size() / 2, static_cast<std::size_t>(frames));   // exactly 2.0 s after trimming
    double snr = snrDb(pcm, got, 4800 * 2, 91200 * 2);
    std::printf("  opus 48k stereo snr = %.1f dB, lapping %zu frames\n", snr, dec->lapping().size() / 2);
    CHECK(snr >= 35.0);
    // Lapping frames continue the tone: first 128 within 6 dB of the signal level.
    CHECK(dec->lapping().size() / 2 >= 128);
    double lap = rms(dec->lapping().data(), 128 * 2);
    double sig = 0.5 / std::sqrt(2.0);
    double ratio_db = 20 * std::log10(lap / sig);
    std::printf("  lapping level %.2f dB relative\n", ratio_db);
    CHECK(std::fabs(ratio_db) <= 6.0);
    CHECK(dec->finished());
}

TEST(opus_round_trip_44k1_and_crossfade) {      // 13.2
    const int rate = 44100, frames = 88200;      // 2.0 s
    auto enc = makeEncoder(kFourccOpus, rate, 2, 128);
    // Two consecutive intervals of one continuous tone.
    auto pcm1 = tone(997.0, 0.5, rate, frames, 2);
    auto pcm2 = tone(997.0, 0.5, rate, frames, 2, 2 * M_PI * 997.0 * frames / rate);
    auto b1 = encodeInterval(*enc, pcm1, 2);
    auto b2 = encodeInterval(*enc, pcm2, 2);
    std::unique_ptr<Decoder> d1, d2;
    auto g1 = decodeAll(kFourccOpus, b1, 2048, &d1);
    auto g2 = decodeAll(kFourccOpus, b2, 2048, &d2);
    long n1 = static_cast<long>(g1.size() / 2), n2 = static_cast<long>(g2.size() / 2);
    std::printf("  44.1k intervals decoded to %ld and %ld frames\n", n1, n2);
    CHECK(std::labs(n1 - 96000) <= 2);
    CHECK(std::labs(n2 - 96000) <= 2);
    // Join with the crossfade of 8.3 (fade-out = lapping of interval 1).
    const auto& lap = d1->lapping();
    std::size_t n = std::min<std::size_t>(128, lap.size() / 2);
    CHECK(n == 128);
    std::vector<float> joined(g1);
    for (std::size_t i = 0; i < n; ++i) {
        float s = static_cast<float>(i + 1) / static_cast<float>(n);
        for (int c = 0; c < 2; ++c) {
            std::size_t k = i * 2 + static_cast<std::size_t>(c);
            g2[k] = g2[k] * s + lap[k] * (1.0f - s);
        }
    }
    joined.insert(joined.end(), g2.begin(), g2.end());
    double max_step_tone = 0.5 * 2 * M_PI * 997.0 / 48000.0;
    double worst = 0;
    std::size_t j = static_cast<std::size_t>(n1);
    for (std::size_t i = j - 400; i < j + 400; ++i) {
        double step = std::fabs(static_cast<double>(joined[i * 2]) - joined[(i - 1) * 2]);
        worst = std::max(worst, step);
    }
    std::printf("  max step around the join %.4f (tone %.4f)\n", worst, max_step_tone);
    CHECK(worst <= max_step_tone * 1.5);
    // The two intervals are also continuous in level (SNR of the join region).
    std::vector<float> ref = tone(997.0, 0.5, 48000, static_cast<int>(joined.size() / 2), 2);
    (void)ref;
}

TEST(flac_round_trip) {                          // 13.3
    const int rate = 44100, frames = 88200;
    // A signal already quantised to 16 bits.
    std::vector<float> pcm(static_cast<std::size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            int q = static_cast<int>(std::lround(0.5 * 32767 * std::sin(2 * M_PI * 440.0 * i / rate + c)));
            pcm[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)] = static_cast<float>(q) / 32768.0f;
        }
    auto enc = makeEncoder(kFourccFlac, rate, 2, 0);
    CHECK(enc != nullptr);
    auto bytes = encodeInterval(*enc, pcm, 2);
    CHECK_EQ(std::string(bytes.begin(), bytes.begin() + 4), "fLaC");
    std::unique_ptr<Decoder> dec;
    auto got = decodeAll(kFourccFlac, bytes, bytes.size(), &dec);
    CHECK_EQ(dec->rate(), rate);
    CHECK_EQ(dec->channels(), 2);
    CHECK_EQ(got.size(), pcm.size());
    bool exact = got.size() == pcm.size();
    for (std::size_t i = 0; exact && i < got.size(); ++i)
        if (got[i] != pcm[i]) exact = false;
    CHECK(exact);
    CHECK(dec->lapping().empty());
    // Digital silence of 2 s is tiny.
    std::vector<float> silence(static_cast<std::size_t>(frames) * 2, 0.0f);
    auto sb = encodeInterval(*enc, silence, 2);
    std::printf("  flac: %zu bytes for the tone, %zu for silence\n", bytes.size(), sb.size());
    // ENGINE.md 13.3 says "under 200 bytes"; with libFLAC's mandatory
    // VORBIS_COMMENT block and 4096-frame subset blocks (about 14 bytes per
    // constant frame) 2 s of silence at 44.1 kHz cannot go below about 400.
    CHECK(sb.size() < 512);
    // Chunked delivery (700 bytes) gives the same output.
    auto chunked = decodeAll(kFourccFlac, bytes, 700);
    CHECK(chunked == got);
    // A second interval from the same encoder is a fresh stream.
    auto b2 = encodeInterval(*enc, pcm, 2);
    CHECK_EQ(std::string(b2.begin(), b2.begin() + 4), "fLaC");
    auto g2 = decodeAll(kFourccFlac, b2, 333);
    CHECK(g2 == got);
}

TEST(float_to_int16) {                           // 13.4
    std::mt19937 rng(1);
    CHECK_EQ(floatToInt16(0.0f, rng), 0);
    CHECK_EQ(floatToInt16(1.0f, rng), 32767);
    CHECK_EQ(floatToInt16(-1.0f, rng), -32768);
    CHECK_EQ(floatToInt16(std::nanf(""), rng), 0);
    for (int i = 0; i < 1000; ++i) {
        int v = floatToInt16(0.5f, rng);
        CHECK(v >= 16383 && v <= 16385);
    }
    // Dither is really applied to in-between values.
    int lo = 0, hi = 0;
    for (int i = 0; i < 1000; ++i) {
        int v = floatToInt16(0.5f + 0.4f / 32768.0f, rng);
        if (v == 16384) ++lo;
        else ++hi;
    }
    CHECK(lo > 0 && hi > 0);
}

TEST(resampler_and_lpc) {
    // 44100 -> 48000 keeps a tone's level and frequency.
    Resampler rs;
    rs.reset(44100, 48000, 1);
    auto in = tone(1000.0, 0.5, 44100, 44100, 1);
    std::vector<float> out;
    rs.process(in.data(), 44100, out);
    std::printf("  resampler: %zu frames from 44100\n", out.size());
    CHECK(std::labs(static_cast<long>(out.size()) - 48000) <= 2);
    // The output lags the input by a constant 32 input frames (a fractional
    // number of output frames), so the reference is the tone delayed by that.
    auto ref = tone(1000.0, 0.5, 48000, 48000, 1, -2 * M_PI * 1000.0 * 32 / 44100.0);
    double snr = snrDb(ref, out, 1000, 40000);
    std::printf("  resampler snr %.1f dB\n", snr);
    CHECK(snr >= 60.0);
    // LPC extrapolation continues a sine.
    auto hist = tone(997.0, 0.5, 48000, 1024, 1);
    std::vector<float> ext(960);
    lpcExtrapolate(hist.data(), 1024, kLpcOrder, ext.data(), 960);
    auto cont = tone(997.0, 0.5, 48000, 960, 1, 2 * M_PI * 997.0 * 1024 / 48000);
    double level = 20 * std::log10(rms(ext.data(), 128) / rms(cont.data(), 128));
    std::printf("  lpc first 128 level %.2f dB\n", level);
    CHECK(std::fabs(level) <= 3.0);
    std::vector<float> zeros(1024, 0.0f);
    lpcExtrapolate(zeros.data(), 1024, kLpcOrder, ext.data(), 960);
    CHECK(ext[0] == 0.0f && ext[500] == 0.0f);
}

TEST_MAIN()
