// njserver - command-line NINJAM server built on njproto.
//
//   njserver <port> [--anon] [--user name:pass:privs]... [--bpm N] [--bpi N]
//            [--allow-codecs opus,flac] [--require-codec opus] [--opus-kbps N]
//            [--keepalive N] [--max-users N] [--topic TEXT] [--motd LINE]
//            [--licence FILE] [--mask-ip] [--vote-threshold PCT] [--quiet]
//
// Privilege letters: t topic, c chat, b bpm/bpi, k kick, r reserved slot,
// m multiple logins, h hidden, v vote, p see private rooms, s status account,
// * all of t..p.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "njproto/server.h"

using namespace njproto;

static volatile std::sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static void usage() {
    std::fprintf(stderr,
        "usage: njserver <port> [options]\n"
        "  --anon                 allow anonymous logins\n"
        "  --anon-multi           anonymous users may log in several times with one name\n"
        "  --user name:pass:privs add an account (repeatable)\n"
        "  --bpm N  --bpi N       initial tempo (default 120 / 8)\n"
        "  --allow-codecs LIST    codecs relayed besides Vorbis: opus,flac or none (default opus)\n"
        "  --require-codec C      refuse clients that cannot play opus|flac (C is always allowed)\n"
        "  --opus-kbps N|none     room Opus bitrate 32-510 (default 128; none = clients choose)\n"
        "  --keepalive N          keepalive interval in seconds (default 3)\n"
        "  --max-users N          user limit (default 16)\n"
        "  --max-pending T P      not-yet-logged-in connections: total and per address (64 8)\n"
        "  --max-channels N       channels per user, 0-32 (default 32)\n"
        "  --topic TEXT           initial topic\n"
        "  --motd LINE            message of the day line (repeatable)\n"
        "  --licence FILE         licence text the client must agree to\n"
        "  --mask-ip              show anonymous users as name@1.2.3.x\n"
        "  --vote-threshold PCT   percent of users for a vote to pass (default 50; >100 disables)\n"
        "  --login-timeout N      seconds to wait for AUTH_USER (default 30)\n"
        "  --quiet                no log output\n"
        "privileges: t topic, c chat, b bpm/bpi, k kick, r reserve, m multi-login,\n"
        "            h hidden, v vote, p private rooms, s status account, * all\n");
}

static bool parseCodecs(const std::string& list, std::uint32_t& out) {
    out = 0;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item == "opus") out |= kCapOpus;
        else if (item == "flac") out |= kCapFlac;
        else if (item.empty() || item == "vorbis" || item == "none") continue;   // Vorbis only
        else return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    ServerConfig cfg;
    cfg.port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    bool quiet = false;
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n", argv[i]);
            std::exit(2);
        }
        return argv[++i];
    };
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--anon") cfg.allow_anonymous = true;
        else if (a == "--anon-multi") cfg.anonymous_privs |= kPrivMulti;
        else if (a == "--max-pending") {
            cfg.max_pending_connections = std::atoi(need(i));
            cfg.max_pending_per_address = std::atoi(need(i));
        } else if (a == "--user") {
            std::string spec = need(i);
            std::size_t p1 = spec.find(':');
            std::size_t p2 = p1 == std::string::npos ? p1 : spec.find(':', p1 + 1);
            if (p1 == std::string::npos) {
                std::fprintf(stderr, "--user wants name:pass:privs\n");
                return 2;
            }
            UserAccount u;
            u.name = spec.substr(0, p1);
            u.password = p2 == std::string::npos ? spec.substr(p1 + 1) : spec.substr(p1 + 1, p2 - p1 - 1);
            u.privs = p2 == std::string::npos ? parsePrivileges("cv") : parsePrivileges(spec.substr(p2 + 1));
            cfg.users.push_back(u);
        } else if (a == "--bpm") cfg.bpm = static_cast<std::uint16_t>(std::atoi(need(i)));
        else if (a == "--bpi") cfg.bpi = static_cast<std::uint16_t>(std::atoi(need(i)));
        else if (a == "--allow-codecs") {
            if (!parseCodecs(need(i), cfg.allow_codecs)) {
                std::fprintf(stderr, "unknown codec in --allow-codecs\n");
                return 2;
            }
        } else if (a == "--require-codec") {
            if (!parseCodecs(need(i), cfg.require_codec) || cfg.require_codec == 0) {
                std::fprintf(stderr, "--require-codec wants opus or flac\n");
                return 2;
            }
        } else if (a == "--opus-kbps") {
            std::string v = need(i);
            if (v == "none") cfg.opus_kbps = 0;
            else {
                long n = std::atol(v.c_str());
                if (n < 32 || n > 510) {
                    std::fprintf(stderr, "warning: --opus-kbps %s is outside 32-510, using 128\n", v.c_str());
                    n = 128;
                }
                cfg.opus_kbps = static_cast<std::uint32_t>(n);
            }
        }
        else if (a == "--keepalive") cfg.keepalive_seconds = std::atoi(need(i));
        else if (a == "--max-users") cfg.max_users = std::atoi(need(i));
        else if (a == "--max-channels") cfg.max_channels = std::atoi(need(i));
        else if (a == "--topic") cfg.topic = need(i);
        else if (a == "--motd") cfg.motd.push_back(need(i));
        else if (a == "--licence" || a == "--license") {
            std::ifstream f(need(i));
            if (!f) {
                std::fprintf(stderr, "cannot read licence file\n");
                return 2;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            cfg.licence = ss.str();
        } else if (a == "--mask-ip") cfg.mask_addresses = true;
        else if (a == "--vote-threshold") cfg.vote_threshold_percent = std::atoi(need(i));
        else if (a == "--login-timeout") cfg.login_timeout_seconds = std::atoi(need(i));
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!quiet) cfg.log = [](const std::string& s) { std::fprintf(stderr, "[njserver] %s\n", s.c_str()); };

    Server server;
    if (!server.start(cfg)) {
        std::fprintf(stderr, "cannot listen on port %u: %s\n", cfg.port, std::strerror(errno));
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    if (!quiet) std::fprintf(stderr, "[njserver] ready on port %u\n", server.port());
    while (!g_stop) server.poll(200);
    server.stop();
    return 0;
}
