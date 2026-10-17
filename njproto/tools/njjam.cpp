// njjam - command-line NINJAM client built on the njproto engine, for the
// end-to-end and interoperability tests of ENGINE.md section 13.
//
//   njjam host:port --user NAME [--password P] [--tone HZ | --wav FILE] [--amp A]
//         [--record out.wav] [--seconds N] [--srate N] [--block N] [--kbps N]
//         [--no-flac] [--listen-only] [--toggle-transmit SECONDS] [--two]
//         [--say TEXT]... [--quiet]
//
// A simulated audio device calls process() every block in real time on an
// audio thread; a control thread calls run(). What the (listening) engine
// hears on outputs 0/1 is recorded to a 32-bit float WAV, and at the end
// `bpi=... interval_samples=... peak=... snr=...` is printed, measured over
// the last two intervals against the tone.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "njproto/engine.h"

using namespace njproto;
using namespace njproto::engine;

namespace {

struct Args {
    std::string hostport, user, password, wav, record;
    double tone = 997.0, amp = 0.5, seconds = 12.0, toggle = 0.0;
    int srate = 48000, block = 512, kbps = 128;
    bool no_flac = false, listen_only = false, two = false, quiet = false, have_tone = true;
    std::vector<std::string> say;
};

void usage() {
    std::fprintf(stderr,
        "usage: njjam host:port --user NAME [--password P] [--tone HZ | --wav FILE] [--amp A]\n"
        "       [--record out.wav] [--seconds N] [--srate N] [--block N] [--kbps N] [--no-flac]\n"
        "       [--listen-only] [--toggle-transmit SECONDS] [--two] [--say TEXT]... [--quiet]\n");
}

// ---------------------------------------------------------------- WAV I/O

void put32(std::ofstream& f, std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& f, std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }

bool writeWavFloat(const std::string& path, const std::vector<float>& interleaved, int channels, int rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::uint32_t data_bytes = static_cast<std::uint32_t>(interleaved.size() * 4);
    f.write("RIFF", 4);
    put32(f, 36 + data_bytes);
    f.write("WAVEfmt ", 8);
    put32(f, 16);
    put16(f, 3);                                        // IEEE float
    put16(f, static_cast<std::uint16_t>(channels));
    put32(f, static_cast<std::uint32_t>(rate));
    put32(f, static_cast<std::uint32_t>(rate * channels * 4));
    put16(f, static_cast<std::uint16_t>(channels * 4));
    put16(f, 32);
    f.write("data", 4);
    put32(f, data_bytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), data_bytes);
    return static_cast<bool>(f);
}

// Reads channel 0 of a 16-bit PCM or 32-bit float WAV, resampled linearly to `rate`.
bool readWavMono(const std::string& path, int rate, std::vector<float>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (all.size() < 12 || std::memcmp(all.data(), "RIFF", 4) != 0 || std::memcmp(all.data() + 8, "WAVE", 4) != 0) return false;
    auto u32 = [&](std::size_t o) { std::uint32_t v; std::memcpy(&v, all.data() + o, 4); return v; };
    auto u16 = [&](std::size_t o) { std::uint16_t v; std::memcpy(&v, all.data() + o, 2); return v; };
    std::size_t p = 12;
    int fmt = 0, ch = 0, srate = 0, bits = 0;
    std::vector<float> mono;
    while (p + 8 <= all.size()) {
        std::uint32_t size = u32(p + 4);
        std::size_t body = p + 8;
        if (std::memcmp(all.data() + p, "fmt ", 4) == 0 && size >= 16) {
            fmt = u16(body);
            ch = u16(body + 2);
            srate = static_cast<int>(u32(body + 4));
            bits = u16(body + 14);
        } else if (std::memcmp(all.data() + p, "data", 4) == 0) {
            if (ch < 1) return false;
            std::size_t n = std::min<std::size_t>(size, all.size() - body);
            if (fmt == 3 && bits == 32) {
                std::size_t frames = n / (4u * static_cast<std::size_t>(ch));
                for (std::size_t i = 0; i < frames; ++i) {
                    float v;
                    std::memcpy(&v, all.data() + body + i * 4 * static_cast<std::size_t>(ch), 4);
                    mono.push_back(v);
                }
            } else if (fmt == 1 && bits == 16) {
                std::size_t frames = n / (2u * static_cast<std::size_t>(ch));
                for (std::size_t i = 0; i < frames; ++i)
                    mono.push_back(static_cast<std::int16_t>(u16(body + i * 2 * static_cast<std::size_t>(ch))) / 32768.0f);
            } else {
                return false;
            }
        }
        p = body + size + (size & 1);
    }
    if (srate <= 0 || mono.empty()) return false;
    if (srate == rate) {
        out = mono;
        return true;
    }
    double ratio = static_cast<double>(srate) / rate;
    std::size_t frames = static_cast<std::size_t>(mono.size() / ratio);
    out.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        double x = i * ratio;
        std::size_t i0 = static_cast<std::size_t>(x);
        std::size_t i1 = std::min(i0 + 1, mono.size() - 1);
        out[i] = static_cast<float>(mono[i0] + (mono[i1] - mono[i0]) * (x - static_cast<double>(i0)));
    }
    return true;
}

// ------------------------------------------------------------- analysis

// SNR of `x` against the best-fitting sinusoid at `hz` (phase and amplitude free).
// Steps a unit phasor (s, c) by the angle (cw, sw), renormalising now and then.
struct Phasor {
    double s = 0.0, c = 1.0, cw, sw;
    long n = 0;
    Phasor(double hz, int rate) : cw(std::cos(2 * M_PI * hz / rate)), sw(std::sin(2 * M_PI * hz / rate)) {}
    void step() {
        double ns = s * cw + c * sw;
        c = c * cw - s * sw;
        s = ns;
        if (++n == 4096) {
            double g = 1.0 / std::sqrt(s * s + c * c);
            s *= g;
            c *= g;
            n = 0;
        }
    }
};

double snrAgainstTone(const float* x, std::size_t n, double hz, int rate) {
    if (n == 0) return 0;
    double sc = 0, ss = 0, cc = 0, ssn = 0, scn = 0;
    Phasor ph(hz, rate);
    for (std::size_t i = 0; i < n; ++i, ph.step()) {
        double s = ph.s, c = ph.c;
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ssn += s * x[i];
        scn += c * x[i];
    }
    double det = ss * cc - sc * sc;
    if (std::fabs(det) < 1e-12) return 0;
    double a = (ssn * cc - scn * sc) / det, b = (scn * ss - ssn * sc) / det;
    double sig = 0, noise = 0;
    Phasor ph2(hz, rate);
    for (std::size_t i = 0; i < n; ++i, ph2.step()) {
        double fit = a * ph2.s + b * ph2.c;
        sig += fit * fit;
        noise += (x[i] - fit) * (x[i] - fit);
    }
    if (noise <= 0) return 200.0;
    return 10.0 * std::log10(sig / noise);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    Args a;
    a.hostport = argv[1];
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n", argv[i]);
            std::exit(2);
        }
        return argv[++i];
    };
    for (int i = 2; i < argc; ++i) {
        std::string o = argv[i];
        if (o == "--user") a.user = need(i);
        else if (o == "--password") a.password = need(i);
        else if (o == "--tone") { a.tone = std::atof(need(i)); a.have_tone = true; }
        else if (o == "--wav") { a.wav = need(i); a.have_tone = false; }
        else if (o == "--amp") a.amp = std::atof(need(i));
        else if (o == "--record") a.record = need(i);
        else if (o == "--seconds") a.seconds = std::atof(need(i));
        else if (o == "--srate") a.srate = std::atoi(need(i));
        else if (o == "--block") a.block = std::atoi(need(i));
        else if (o == "--kbps") a.kbps = std::atoi(need(i));
        else if (o == "--no-flac") a.no_flac = true;
        else if (o == "--listen-only") a.listen_only = true;
        else if (o == "--toggle-transmit") a.toggle = std::atof(need(i));
        else if (o == "--two") a.two = true;
        else if (o == "--say") a.say.push_back(need(i));
        else if (o == "--quiet") a.quiet = true;
        else if (o == "-h" || o == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (a.user.empty()) {
        std::fprintf(stderr, "--user is required\n");
        return 2;
    }
    std::vector<float> wav;
    if (!a.wav.empty() && !readWavMono(a.wav, a.srate, wav)) {
        std::fprintf(stderr, "cannot read %s (16-bit PCM or 32-bit float WAV)\n", a.wav.c_str());
        return 2;
    }

    // In --two mode the first engine sends and the second listens; otherwise
    // the single engine does both (unless --listen-only).
    Engine sender, listener_engine;
    Engine& listener = a.two ? listener_engine : sender;
    auto setup = [&](Engine& e, const std::string& user, bool transmit) {
        e.preferFlac(!a.no_flac);
        e.setMetronome(0.0f, 0.0f, true, 0);
        if (transmit) {
            LocalChannel lc;
            lc.name = "njjam";
            lc.bitrate = a.kbps;
            lc.broadcast = true;
            lc.mute = true;                    // no local monitoring in the recording
            e.setLocalChannel(0, lc);
        }
        e.onChat = [&, user](const std::array<std::string, 5>& f) {
            if (!a.quiet) std::fprintf(stderr, "[%s] %s %s %s\n", user.c_str(), f[0].c_str(), f[1].c_str(), f[2].c_str());
        };
        e.connect(a.hostport, user, a.password);
    };
    setup(sender, a.user, !a.listen_only);
    if (a.two) setup(listener_engine, a.user + "_l", false);

    // ---- audio thread: a simulated device at srate / block
    std::atomic<bool> stop{false};
    std::vector<float> recording;              // interleaved stereo
    recording.reserve(static_cast<std::size_t>((a.seconds + 1.0) * a.srate) * 2);
    std::thread audio([&] {
        const int n = a.block;
        std::vector<float> inbuf(static_cast<std::size_t>(n)), l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
        std::vector<float> sl(static_cast<std::size_t>(n)), sr(static_cast<std::size_t>(n));
        const float* in[1] = {inbuf.data()};
        float* out[2] = {l.data(), r.data()};
        float* sout[2] = {sl.data(), sr.data()};
        // The tone is a rotating phasor (one complex multiply per sample)
        // rather than a sin() per sample; renormalised now and then.
        Phasor tone_ph(a.tone, a.srate);
        std::size_t wav_pos = 0;
        long frames_done = 0;
        bool broadcast = true;
        auto t0 = std::chrono::steady_clock::now();
        while (!stop.load()) {
            for (int i = 0; i < n; ++i) {
                if (a.have_tone) {
                    inbuf[static_cast<std::size_t>(i)] = static_cast<float>(a.amp * tone_ph.s);
                    tone_ph.step();
                } else {
                    inbuf[static_cast<std::size_t>(i)] = wav.empty() ? 0.0f : wav[wav_pos++ % wav.size()];
                }
            }
            double t = static_cast<double>(frames_done) / a.srate;
            if (a.toggle > 0 && !a.listen_only) {
                bool want = !(t >= a.toggle && t < 2 * a.toggle);
                if (want != broadcast) {
                    broadcast = want;
                    LocalChannel lc;
                    if (sender.localChannel(0, &lc)) {
                        lc.broadcast = want;
                        sender.setLocalChannel(0, lc);
                    }
                    if (!a.quiet) std::fprintf(stderr, "transmit %s at %.2f s\n", want ? "on" : "off", t);
                }
            }
            if (a.two) {
                sender.process(in, 1, sout, 2, n, a.srate);
                listener.process(in, 0, out, 2, n, a.srate);
            } else {
                sender.process(in, 1, out, 2, n, a.srate);
            }
            std::size_t at = recording.size();
            recording.resize(at + static_cast<std::size_t>(n) * 2);
            for (int i = 0; i < n; ++i) {
                recording[at + static_cast<std::size_t>(i) * 2] = l[static_cast<std::size_t>(i)];
                recording[at + static_cast<std::size_t>(i) * 2 + 1] = r[static_cast<std::size_t>(i)];
            }
            frames_done += n;
            auto next = t0 + std::chrono::microseconds(static_cast<long>(frames_done * 1000000.0 / a.srate));
            std::this_thread::sleep_until(next);
        }
    });

    // ---- control loop (this thread)
    auto start = std::chrono::steady_clock::now();
    bool said = false;
    bool reported_status = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(a.seconds)) {
        bool idle = sender.run();
        if (a.two) idle = listener.run() && idle;
        // Hear everyone at full level (the engine's default remote volume is 0.25).
        for (int u = 0; u < listener.numUsers(); ++u)
            for (int i = 0; i < 32; ++i) {
                int ch = listener.channelAt(u, i);
                if (ch < 0) break;
                if (listener.channelVolume(u, ch) != 1.0f) listener.setChannelVolume(u, ch, 1.0f);
            }
        Status st = sender.status();
        if (st == Status::Ok && !said) {
            said = true;
            for (const auto& s : a.say) sender.say(s);
        }
        if (!reported_status && (st == Status::CantConnect || st == Status::InvalidAuth || st == Status::Disconnected)) {
            reported_status = true;
            std::fprintf(stderr, "connection failed: %s\n", sender.errorText().c_str());
            break;
        }
        // ENGINE.md asks for run() at least every 50 ms; the upload queue holds
        // about 5 s, so 10 ms between idle polls is plenty.
        if (idle) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    stop.store(true);
    audio.join();
    int bpi = listener.serverBpi();              // read before disconnect() clears them
    int ilen = listener.usualIntervalLength();
    sender.disconnect();
    if (a.two) listener.disconnect();

    // ---- report over the last two intervals
    if (ilen <= 0) ilen = intervalLengthFor(listener.serverBpm() > 0 ? listener.serverBpm() : 120,
                                            bpi > 0 ? bpi : 8, a.srate);
    // Trailing digital silence (the other side left before this run ended)
    // is not part of the measurement window.
    std::size_t frames = recording.size() / 2;
    while (frames > 0 && recording[(frames - 1) * 2] == 0.0f && recording[(frames - 1) * 2 + 1] == 0.0f) --frames;
    std::size_t take = std::min(frames, static_cast<std::size_t>(ilen) * 2);
    std::vector<float> left(take);
    for (std::size_t i = 0; i < take; ++i) left[i] = recording[(frames - take + i) * 2];
    float peak = 0;
    for (float v : left) peak = std::max(peak, std::fabs(v));
    double snr = a.have_tone ? snrAgainstTone(left.data(), left.size(), a.tone, a.srate) : 0.0;
    std::printf("bpi=%d interval_samples=%d peak=%.4f snr=%.2f\n", bpi, ilen, peak, snr);
    if (!a.record.empty() && !writeWavFloat(a.record, recording, 2, a.srate))
        std::fprintf(stderr, "cannot write %s\n", a.record.c_str());
    return reported_status ? 1 : 0;
}
