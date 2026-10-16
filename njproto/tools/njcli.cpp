// njcli - command-line NINJAM client for interoperability tests.
//
//   njcli <host> <port> <user> [pass] [--codecs opus,flac] [--channel NAME]...
//         [--upload FILE [--bpm-interval] [--fourcc XXXX]] [--subscribe-all]
//         [--say TEXT] [--privmsg USER TEXT] [--admin CMD] [--dump DIR]
//         [--seconds N] [--json] [--no-keepalive] [--version HEX] [--refuse-licence]
//
// Every message received is logged as one JSON object per line on stdout.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "njproto/client.h"

using namespace njproto;

static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

static void emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static void usage() {
    std::fprintf(stderr,
        "usage: njcli <host> <port> <user> [pass] [options]\n"
        "  --codecs opus,flac     codec capability bits to announce\n"
        "  --channel NAME         add a channel (repeatable; default: one channel 'njcli')\n"
        "  --no-channels          send an empty channel list\n"
        "  --upload FILE          upload FILE as an interval on channel 0 (once)\n"
        "  --bpm-interval         with --upload: repeat every interval (60*BPI/BPM s)\n"
        "  --fourcc XXXX          codec fourcc for uploads (default from the extension, else OGGv)\n"
        "  --subscribe-all        subscribe to every channel of every user\n"
        "  --say TEXT             send a MSG after login (repeatable)\n"
        "  --privmsg USER TEXT    send a PRIVMSG after login\n"
        "  --admin CMD            send an ADMIN command after login (e.g. 'topic hello')\n"
        "  --dump DIR             save received intervals to DIR/<guid>.<ext>\n"
        "  --seconds N            exit after N seconds (default 5)\n"
        "  --json                 JSON output (the default; accepted for clarity)\n"
        "  --no-keepalive         never send keepalives (for drop tests)\n"
        "  --version HEX          client_version to send (default 0x00020000)\n"
        "  --refuse-licence       do not agree to a licence\n");
}

struct Download {
    std::string user;
    int channel = 0;
    std::uint32_t fourcc = 0;
    std::size_t bytes = 0;
    std::ofstream file;
};

int main(int argc, char** argv) {
    if (argc < 4) {
        usage();
        return 2;
    }
    std::string host = argv[1];
    auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    Client::Options opt;
    opt.username = argv[3];
    int i = 4;
    if (i < argc && std::strncmp(argv[i], "--", 2) != 0) opt.password = argv[i++];

    std::vector<std::string> channels;
    bool no_channels = false;
    std::string upload_file, dump_dir, fourcc_text;
    bool bpm_interval = false, subscribe_all = false;
    std::vector<ChatMessage> after_login;
    double seconds = 5;
    auto need = [&](int& k) -> const char* {
        if (k + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value\n", argv[k]);
            std::exit(2);
        }
        return argv[++k];
    };
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--codecs") {
            std::stringstream ss(need(i));
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (item == "opus") opt.codec_caps |= kCapOpus;
                else if (item == "flac") opt.codec_caps |= kCapFlac;
            }
        } else if (a == "--channel") channels.push_back(need(i));
        else if (a == "--no-channels") no_channels = true;
        else if (a == "--upload") upload_file = need(i);
        else if (a == "--bpm-interval") bpm_interval = true;
        else if (a == "--fourcc") fourcc_text = need(i);
        else if (a == "--subscribe-all") subscribe_all = true;
        else if (a == "--say") after_login.push_back(ChatMessage("MSG", need(i)));
        else if (a == "--privmsg") {
            std::string u = need(i);
            after_login.push_back(ChatMessage("PRIVMSG", u, need(i)));
        } else if (a == "--admin") after_login.push_back(ChatMessage("ADMIN", need(i)));
        else if (a == "--dump") dump_dir = need(i);
        else if (a == "--seconds") seconds = std::atof(need(i));
        else if (a == "--json") { /* default */ }
        else if (a == "--no-keepalive") opt.send_keepalives = false;
        else if (a == "--version") opt.client_version = static_cast<std::uint32_t>(std::strtoul(need(i), nullptr, 16));
        else if (a == "--refuse-licence" || a == "--refuse-license") opt.agree_licence = false;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    opt.auto_subscribe = subscribe_all;
    if (!dump_dir.empty() && ::mkdir(dump_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "cannot create %s: %s\n", dump_dir.c_str(), std::strerror(errno));
        return 2;
    }

    std::vector<std::uint8_t> upload_bytes;
    std::uint32_t upload_fourcc = kFourccVorbis;
    if (!upload_file.empty()) {
        std::ifstream f(upload_file, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", upload_file.c_str());
            return 2;
        }
        upload_bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        std::size_t dot = upload_file.rfind('.');
        std::string ext = dot == std::string::npos ? "" : upload_file.substr(dot + 1);
        if (ext == "opus") upload_fourcc = kFourccOpus;
        else if (ext == "flac") upload_fourcc = kFourccFlac;
        if (!fourcc_text.empty()) upload_fourcc = fourccFromString(fourcc_text);
    }

    Client client;
    std::map<Guid, Download> downloads;
    bool login_ok = false;

    client.on_message = [](const RawMessage& m) {
        switch (m.type) {
        case kMsgAuthChallenge: case kMsgAuthReply: case kMsgConfigChange: case kMsgUserInfoChange:
        case kMsgDownloadBegin: case kMsgDownloadWrite: case kMsgCodecMask: case kMsgChat:
            return;                                         // logged by the decoded handlers
        case kMsgKeepalive:
            emit("{\"type\":\"keepalive\"}");
            return;
        default:
            emit("{\"type\":\"unknown\",\"id\":" + std::to_string(m.type) + ",\"len\":" + std::to_string(m.payload.size()) + "}");
        }
    };
    client.on_challenge = [](const AuthChallenge& ch) {
        emit("{\"type\":\"auth_challenge\",\"server_caps\":" + std::to_string(ch.server_caps) +
             ",\"protocol_version\":" + std::to_string(ch.protocol_version) +
             ",\"keepalive\":" + std::to_string(ch.keepaliveSeconds()) +
             ",\"licence\":" + (ch.has_licence ? jsonStr(ch.licence) : "null") + "}");
    };
    client.on_auth_reply = [&](const AuthReply& r) {
        login_ok = r.success();
        emit("{\"type\":\"auth_reply\",\"success\":" + std::string(r.success() ? "true" : "false") +
             ",\"text\":" + jsonStr(r.text) + ",\"maxchan\":" + std::to_string(r.maxchan) + "}");
    };
    client.on_config = [](const ConfigChange& c) {
        emit("{\"type\":\"config\",\"bpm\":" + std::to_string(c.bpm) + ",\"bpi\":" + std::to_string(c.bpi) + "}");
    };
    client.on_userinfo = [](const UserInfoRecord& r) {
        emit("{\"type\":\"userinfo\",\"active\":" + std::string(r.active ? "true" : "false") +
             ",\"user\":" + jsonStr(r.username) + ",\"index\":" + std::to_string(r.index) +
             ",\"channel\":" + jsonStr(r.channel) + ",\"volume\":" + std::to_string(r.volume) +
             ",\"pan\":" + std::to_string(r.pan) + ",\"flags\":" + std::to_string(r.flags) + "}");
    };
    client.on_download_begin = [&](const DownloadBegin& d) {
        bool silence = guidIsZero(d.guid);
        emit("{\"type\":\"download_begin\",\"guid\":" + jsonStr(guidToHex(d.guid)) +
             ",\"user\":" + jsonStr(d.username) + ",\"channel\":" + std::to_string(d.channel) +
             ",\"fourcc\":" + jsonStr(fourccToString(d.fourcc)) + ",\"estsize\":" + std::to_string(d.estsize) +
             ",\"silence\":" + (silence ? "true" : "false") + "}");
        if (silence || d.fourcc == 0) return;
        Download& dl = downloads[d.guid];
        dl.user = d.username;
        dl.channel = d.channel;
        dl.fourcc = d.fourcc;
        dl.bytes = 0;
        if (!dump_dir.empty()) {
            std::string ext = d.fourcc == kFourccOpus ? "opus" : d.fourcc == kFourccFlac ? "flac" : "ogg";
            dl.file.open(dump_dir + "/" + guidToHex(d.guid) + "." + ext, std::ios::binary);
        }
    };
    client.on_download_data = [&](const Guid& g, const std::vector<std::uint8_t>& data) {
        auto it = downloads.find(g);
        if (it == downloads.end()) return;
        it->second.bytes += data.size();
        if (it->second.file.is_open())
            it->second.file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    client.on_download_end = [&](const Guid& g, bool complete) {
        auto it = downloads.find(g);
        if (it == downloads.end()) return;
        emit("{\"type\":\"download_end\",\"guid\":" + jsonStr(guidToHex(g)) +
             ",\"user\":" + jsonStr(it->second.user) + ",\"channel\":" + std::to_string(it->second.channel) +
             ",\"fourcc\":" + jsonStr(fourccToString(it->second.fourcc)) +
             ",\"bytes\":" + std::to_string(it->second.bytes) + ",\"ended\":" + (complete ? "true" : "false") + "}");
        downloads.erase(it);
    };
    client.on_chat = [](const ChatMessage& c) {
        std::string s = "{\"type\":\"chat\",\"command\":" + jsonStr(c.fields[0]) + ",\"fields\":[";
        for (std::size_t k = 1; k < c.fields.size(); ++k) s += (k > 1 ? "," : "") + jsonStr(c.fields[k]);
        emit(s + "]}");
    };
    client.on_codec_mask = [](const CodecMask& m) {
        emit("{\"type\":\"codec_mask\",\"codecs\":" + std::to_string(m.codecs) + ",\"opus_kbps\":" + std::to_string(m.opus_kbps) + "}");
    };
    client.on_disconnect = [](const std::string& reason) {
        emit("{\"type\":\"disconnect\",\"reason\":" + jsonStr(reason) + "}");
    };

    if (!no_channels) {
        if (channels.empty()) channels.push_back("njcli");
        std::vector<ChannelInfoRecord> recs;
        for (const auto& name : channels) {
            ChannelInfoRecord r;
            r.name = name;
            recs.push_back(r);
        }
        client.setChannels(recs);
    }

    if (!client.connect(host, port, opt)) {
        std::fprintf(stderr, "cannot connect to %s:%u\n", host.c_str(), port);
        return 1;
    }

    // Wait for the login to settle, then do the one-shot actions.
    client.waitFor([&] { return client.loggedIn() || client.state() == Client::State::Failed; }, 10000);
    if (client.loggedIn()) {
        for (const auto& m : after_login) client.sendChat(m);
    }

    TimePoint start = Clock::now();
    TimePoint deadline = start + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
    TimePoint next_upload = start;
    bool uploaded_once = false;
    while (Clock::now() < deadline && client.state() != Client::State::Failed &&
           client.state() != Client::State::Disconnected) {
        client.poll(20);
        if (client.loggedIn() && !upload_bytes.empty()) {
            TimePoint now = Clock::now();
            if (bpm_interval) {
                if (client.hasConfig() && now >= next_upload) {
                    double len = 60.0 * client.bpi() / client.bpm();
                    next_upload = now + std::chrono::milliseconds(static_cast<long>(len * 1000));
                    Guid g = client.beginInterval(0, upload_fourcc);
                    client.writeInterval(g, upload_bytes.data(), upload_bytes.size(), true);
                    emit("{\"type\":\"upload\",\"guid\":" + jsonStr(guidToHex(g)) + ",\"bytes\":" + std::to_string(upload_bytes.size()) + "}");
                }
            } else if (!uploaded_once) {
                uploaded_once = true;
                Guid g = client.beginInterval(0, upload_fourcc);
                client.writeInterval(g, upload_bytes.data(), upload_bytes.size(), true);
                emit("{\"type\":\"upload\",\"guid\":" + jsonStr(guidToHex(g)) + ",\"bytes\":" + std::to_string(upload_bytes.size()) + "}");
            }
        }
    }
    if (client.state() == Client::State::Failed)
        emit("{\"type\":\"exit\",\"reason\":" + jsonStr(client.failReason()) + "}");
    client.disconnect("exiting");
    return login_ok ? 0 : 1;
}
