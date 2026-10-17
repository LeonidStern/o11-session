// o11bench - CPU benchmark of the client engine, for profiling.
//
//   o11bench [--codec opus|flac] [--seconds N] [--srate N] [--block N] [--kbps N]
//
// One sender and one listener engine (as `o11jam --two`) on an in-process
// o11server, with a simulated audio clock: the blocks are processed as fast as
// the machine allows, with the server and both engines' run() polled between
// blocks, so the run is deterministic and a profiler sees only audio work and
// the network round trip, no idle waiting. At the end it prints the process
// CPU time of the audio loop (getrusage; it includes the in-process server,
// which a profiler separates) and the listener's SNR over its last two
// intervals.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include "o11proto/engine.h"
#include "o11proto/server.h"

using namespace o11proto;
using namespace o11proto::engine;

namespace {

double snrAgainstTone(const float* x, std::size_t n, double hz, int rate) {
    double sc = 0, ss = 0, cc = 0, ssn = 0, scn = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double w = 2 * M_PI * hz * static_cast<double>(i) / rate;
        double s = std::sin(w), c = std::cos(w);
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
    for (std::size_t i = 0; i < n; ++i) {
        double w = 2 * M_PI * hz * static_cast<double>(i) / rate;
        double fit = a * std::sin(w) + b * std::cos(w);
        sig += fit * fit;
        noise += (x[i] - fit) * (x[i] - fit);
    }
    return noise > 0 ? 10 * std::log10(sig / noise) : 200.0;
}

double cpuSeconds() {
#ifndef _WIN32
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
#else
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string codec = "opus";
    double seconds = 20.0, hz = 997.0, amp = 0.5;
    int rate = 48000, block = 512, kbps = 510;
    for (int i = 1; i < argc; ++i) {
        std::string o = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", o.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (o == "--codec") codec = val();
        else if (o == "--seconds") seconds = std::atof(val());
        else if (o == "--srate") rate = std::atoi(val());
        else if (o == "--block") block = std::atoi(val());
        else if (o == "--kbps") kbps = std::atoi(val());
        else {
            std::fprintf(stderr, "usage: o11bench [--codec opus|flac] [--seconds N] [--srate N] [--block N] [--kbps N]\n");
            return 2;
        }
    }
    bool flac = codec == "flac";

    Server server;
    ServerConfig cfg;
    cfg.port = 0;
    cfg.allow_anonymous = true;
    cfg.allow_codecs = kCapOpus | (flac ? kCapFlac : 0);
    cfg.require_codec = flac ? kCapFlac : 0;
    cfg.opus_kbps = static_cast<std::uint32_t>(kbps);
    if (!server.start(cfg)) {
        std::fprintf(stderr, "cannot start the server\n");
        return 1;
    }
    Engine sender, listener;
    sender.setMetronome(0, 0, true, 0);
    listener.setMetronome(0, 0, true, 0);
    LocalChannel lc;
    lc.name = "tone";
    lc.bitrate = kbps;
    lc.broadcast = true;
    lc.mute = true;
    sender.setLocalChannel(0, lc);

    auto control = [&] {
        server.poll(0);
        for (int i = 0; i < 3; ++i) {
            bool idle = sender.run();
            idle = listener.run() && idle;
            if (idle) break;
        }
        for (int u = 0; u < listener.numUsers(); ++u)
            for (int i = 0; i < 32; ++i) {
                int ch = listener.channelAt(u, i);
                if (ch < 0) break;
                if (listener.channelVolume(u, ch) != 1.0f) listener.setChannelVolume(u, ch, 1.0f);
            }
    };
    sender.connect("127.0.0.1", server.port(), "anonymous:sender", "");
    listener.connect("127.0.0.1", server.port(), "anonymous:listener", "");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(sender.status() == Status::Ok && listener.status() == Status::Ok && sender.serverBpi() > 0 &&
             listener.serverBpi() > 0 && listener.numUsers() == 1)) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "login timed out\n");
            return 1;
        }
        control();
        server.poll(1);
    }
    control();

    std::vector<float> in(static_cast<std::size_t>(block)), sl(in), sr(in), ll(in), lr(in), heard;
    heard.reserve(static_cast<std::size_t>(seconds * rate) + static_cast<std::size_t>(block));
    double phase = 0, step = 2 * M_PI * hz / rate;
    double cpu0 = cpuSeconds();
    long n = static_cast<long>(seconds * rate);
    for (long done = 0; done < n; done += block) {
        for (int i = 0; i < block; ++i) {
            in[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(phase));
            phase += step;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
        }
        const float* inp[1] = {in.data()};
        float* so[2] = {sl.data(), sr.data()};
        float* lo[2] = {ll.data(), lr.data()};
        sender.process(inp, 1, so, 2, block, rate);
        listener.process(inp, 0, lo, 2, block, rate);
        heard.insert(heard.end(), ll.begin(), ll.end());
        control();
    }
    double cpu = cpuSeconds() - cpu0;
    std::size_t take = std::min(heard.size(), static_cast<std::size_t>(listener.usualIntervalLength()) * 2);
    double snr = snrAgainstTone(heard.data() + heard.size() - take, take, hz, rate);
    std::printf("codec=%s fourcc=%s seconds=%.1f cpu=%.3f snr=%.2f\n", codec.c_str(),
                sender.uploadCodec() == kFourccFlac ? "FLAC" : "OPUr", seconds, cpu, snr);
    sender.disconnect();
    listener.disconnect();
    return 0;
}
