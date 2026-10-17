/*
    O11 Session - o11_e2e_test.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  Two engine wrappers (what a plug-in instance runs) in one process: the first
  hosts a room and joins it, the second connects to it. A 997 Hz tone goes into
  the first one's process() with a simulated clock (blocks of 512 at 48 kHz,
  paced at real time, as a host would), and the second one must hear it: SNR
  of at least 35 dB with Opus at the room's bitrate, 60 dB once the room is
  switched to FLAC with "codec flac". Modelled on o11proto's
  engine/tests/test_engine_e2e.cpp, through the plug-in's own text commands.

  Ableton Link, with Sync on in both (as by default): one instance in a
  process leads Link, the first in a room; when it turns Sync off, the other
  leads it. A Link peer in this process (as Live with its Link on) takes the
  room's tempo from the plug-in, also after a vote changes it. The peer's part
  is skipped when Link doesn't see it (no multicast here).

*/

// Link's asio includes the Windows headers: first, and for Windows 7 and later
#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <ableton/Link.hpp>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"

static constexpr double kPi = 3.14159265358979323846; // kPi is not in MSVC's <math.h> by default

using namespace o11;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static double snrAgainstTone(const float *x, size_t n, double hz, int rate)
{
  double sc = 0, ss = 0, cc = 0, ssn = 0, scn = 0;
  for (size_t i = 0; i < n; ++i)
  {
    const double w = 2 * kPi * hz * (double)i / rate;
    const double s = sin(w), c = cos(w);
    ss += s * s;
    cc += c * c;
    sc += s * c;
    ssn += s * x[i];
    scn += c * x[i];
  }
  const double det = ss * cc - sc * sc;
  const double a = (ssn * cc - scn * sc) / det, b = (scn * ss - ssn * sc) / det;
  double sig = 0, noise = 0;
  for (size_t i = 0; i < n; ++i)
  {
    const double w = 2 * kPi * hz * (double)i / rate;
    const double fit = a * sin(w) + b * cos(w);
    sig += fit * fit;
    noise += (x[i] - fit) * (x[i] - fit);
  }
  return noise > 0 ? 10 * log10(sig / noise) : 200.0;
}

// the field of a "status" reply
static std::string field(const std::string &status, const std::string &key)
{
  const size_t at = status.find(key + "=");
  if (at == std::string::npos) return "";
  const size_t end = status.find(' ', at);
  return status.substr(at + key.size() + 1, end == std::string::npos ? std::string::npos : end - at - key.size() - 1);
}

struct Room
{
  Engine host, guest;
  static constexpr int RATE = 48000, BLOCK = 512;
  double hz = 997.0, amp = 0.5, phase = 0;
  std::vector<float> in[2], hostOut[2], guestOut[2];
  std::vector<float> heard; // the guest's left output
  double clock = 0;         // simulated seconds of audio

  Room()
  {
    for (auto &v : in) v.assign(BLOCK, 0.0f);
    for (auto &v : hostOut) v.assign(BLOCK, 0.0f);
    for (auto &v : guestOut) v.assign(BLOCK, 0.0f);
    host.setupProcessing(RATE, BLOCK);
    guest.setupProcessing(RATE, BLOCK);
    host.start();
    guest.start();
  }
  ~Room()
  {
    guest.stop();
    host.stop();
  }
  bool waitFor(Engine &e, const char *key, const std::string &value, double seconds)
  {
    const double deadline = now_seconds() + seconds;
    while (now_seconds() < deadline)
    {
      if (field(e.command("status"), key) == value) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  status: %s\n", e.command("status").c_str());
    return false;
  }
  // `seconds` of audio through both, at real time (the network threads run meanwhile)
  void advance(double seconds)
  {
    const double start = now_seconds();
    const long n = (long)(seconds * RATE);
    for (long done = 0; done < n; done += BLOCK)
    {
      for (int i = 0; i < BLOCK; i ++)
      {
        in[0][(size_t)i] = in[1][(size_t)i] = (float)(amp * sin(phase));
        phase += 2 * kPi * hz / RATE;
        if (phase > 2 * kPi) phase -= 2 * kPi;
      }
      float *ins[2] = { in[0].data(), in[1].data() };
      float *ho[2] = { hostOut[0].data(), hostOut[1].data() };
      float *go[2] = { guestOut[0].data(), guestOut[1].data() };
      host.process(ins, 2, ho, 2, BLOCK);
      guest.process(nullptr, 0, go, 2, BLOCK);
      heard.insert(heard.end(), guestOut[0].begin(), guestOut[0].end());
      clock += (double)BLOCK / RATE;
      // keep to real time, so the network side sees what a host would give it
      const double wall = now_seconds() - start, audio = (double)(done + BLOCK) / RATE;
      if (audio > wall) std::this_thread::sleep_for(std::chrono::duration<double>(audio - wall));
    }
  }
  int intervalLength()
  {
    return atoi(field(guest.command("status"), "len").c_str());
  }
  // SNR and peak over the last two intervals heard
  void lastTwo(double *snr, float *peak)
  {
    const size_t take = std::min(heard.size(), (size_t)intervalLength() * 2);
    const float *p = heard.data() + (heard.size() - take);
    *snr = snrAgainstTone(p, take, hz, RATE);
    *peak = 0;
    for (size_t i = 0; i < take; ++i) *peak = std::max(*peak, fabsf(p[i]));
  }
};

int main()
{
  Room room;
  // the host: a room on this computer (120 BPM, 4 BPI), joined at once
  CHECK(room.host.command("name Host") == "ok");
  CHECK(room.host.command("host on") == "ok");
  CHECK(room.waitFor(room.host, "state", "connected", 10));
  const std::string hostStatus = room.host.command("status");
  const int port = atoi(field(hostStatus, "port").c_str());
  printf("  host: %s\n", hostStatus.c_str());
  CHECK(port > 0); // any free port (O11_HOST_PORT=0)
  CHECK(field(hostStatus, "hosting") == "1");
  CHECK(field(hostStatus, "codec") == "Opus");
  CHECK(field(hostStatus, "upload") == "OPUr");
  CHECK(field(hostStatus, "bpi") == "4");
  CHECK(field(hostStatus, "roomkbps") == "510"); // the host's own bitrate becomes the room's
  // the router and the "what's my IP" service are left alone (O11_PORTMAP and O11_WHATSMYIP are off)
  CHECK(field(hostStatus, "public") == "-");
  CHECK(room.waitFor(room.host, "lookedup", "-", 5));
  CHECK(field(room.host.command("status"), "joinedfrominternet") == "0");

  // the guest joins; it hears the host, who plays alone on a player output
  CHECK(room.guest.command("connect 127.0.0.1:" + std::to_string(port) + " Guest") == "ok");
  CHECK(room.waitFor(room.guest, "state", "connected", 10));
  CHECK(room.waitFor(room.guest, "users", "1", 10));
  CHECK(room.waitFor(room.host, "users", "1", 10));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const std::string users = room.guest.command("users");
  printf("  guest sees: %s", users.c_str());
  CHECK(users.find("main vol=1.000") != std::string::npos); // 0 dB for a new player
  CHECK(users.find("shown=Host") != std::string::npos);
  const std::string chat = room.guest.command("chatlog");
  CHECK(chat.find("Host plays alone on the output Player 1") != std::string::npos);
  CHECK(chat.find("Connected to 127.0.0.1:") != std::string::npos);

  // Opus, at the room's bitrate
  room.advance(12.0);
  double snr = 0;
  float peak = 0;
  room.lastTwo(&snr, &peak);
  printf("  opus: snr %.1f dB, peak %.3f\n", snr, peak);
  CHECK(snr >= 35.0);
  CHECK(peak >= 0.45f && peak <= 0.55f);
  CHECK(field(room.guest.command("status"), "audio") == "1");
  CHECK(field(room.guest.command("status"), "kbps") == "510");
  CHECK(room.guest.command("users").find("peak=0.") != std::string::npos);

  // the guest switches the room to FLAC: the interval already begun stays Opus, so after 20 s
  // the last two intervals are FLAC following FLAC
  CHECK(room.guest.command("codec flac") == "ok");
  CHECK(room.waitFor(room.host, "codec", "FLAC", 10));
  CHECK(room.waitFor(room.guest, "upload", "FLAC", 10));
  room.advance(20.0);
  room.lastTwo(&snr, &peak);
  printf("  flac: snr %.1f dB, peak %.3f\n", snr, peak);
  CHECK(snr >= 60.0);
  CHECK(peak >= 0.45f && peak <= 0.55f);

  // the room's bitrate, changed by the guest, reaches the host
  CHECK(room.guest.command("bitrate 128") == "ok");
  CHECK(room.waitFor(room.host, "roomkbps", "128", 10));

  // Link: the host, first in the room, leads it; the guest, in the same process, can't
  const bool haveLink = field(room.host.command("status"), "link") != "unavailable";
  if (!haveLink) printf("  Link couldn't start here: its checks are skipped\n");
  else
  {
    CHECK(room.waitFor(room.host, "link", "leading", 5));
    CHECK(room.waitFor(room.guest, "link", "other", 5));
    CHECK(room.host.command("synctip").find("Turn on Link in your DAW if it has Link: ") == 0); // no other Link app yet
    CHECK(room.guest.command("synctip").find(". Another O11 Session in this set leads Link") != std::string::npos);

    // a Link peer at its own tempo, as Live with its Link on: it gets the room's
    ableton::Link peer(133.0);
    peer.enable(true);
    bool seen = false;
    for (int i = 0; i < 30 && !seen; i ++)
    {
      room.advance(0.2);
      seen = peer.numPeers() > 0 && field(room.host.command("status"), "peers") == "1";
    }
    if (!seen) printf("  the plug-in's Link didn't see the peer (no multicast here?): its checks are skipped\n");
    else
    {
      auto peerTempo = [&]() { return peer.captureAppSessionState().tempo(); };
      for (int i = 0; i < 50 && fabs(peerTempo() - 120.0) > 1e-6; i ++) room.advance(0.2);
      room.advance(1.0);
      const std::string st = room.host.command("status");
      printf("  link: %s, the peer at %.3f BPM\n", st.substr(st.find(" link=")).c_str(), peerTempo());
      CHECK(fabs(peerTempo() - 120.0) < 1e-6);
      CHECK(field(st, "leads") == "1");
      CHECK(field(st, "linkaligned") == "1");
      CHECK(atoi(field(st, "jumps").c_str()) >= 1);
      CHECK(room.host.command("chatlog").find("Link: The DAW takes the room's tempo (120 BPM), and its bars start with the intervals.") !=
            std::string::npos);

      // the room's vote reaches the peer through Link, at the next interval
      CHECK(room.guest.command("vote bpm 100") == "ok");
      for (int i = 0; i < 60 && fabs(peerTempo() - 100.0) > 1e-6; i ++) room.advance(0.2);
      CHECK(fabs(peerTempo() - 100.0) < 1e-6);
      CHECK(field(room.host.command("status"), "bpm") == "100.0");
    }

    // Sync off in the host: it leaves Link, and the guest leads it
    CHECK(room.host.command("follow off") == "ok");
    CHECK(room.waitFor(room.host, "link", "off", 5));
    CHECK(room.waitFor(room.guest, "link", "leading", 5));
    if (seen)
    {
      room.advance(1.0);
      CHECK(fabs(peer.captureAppSessionState().tempo() - 100.0) < 1e-6);
      CHECK(room.waitFor(room.guest, "leads", "1", 5));
    }
    CHECK(room.host.command("follow on") == "ok");
    CHECK(room.waitFor(room.host, "link", "other", 5));
    peer.enable(false);
  }

  // transmit off: the host sends nothing from the next interval on, and the guest hears silence
  CHECK(room.host.command("transmit off") == "ok");
  room.advance(2.0 * room.intervalLength() / Room::RATE + 1.0);
  room.heard.clear();
  room.advance(1.0 * room.intervalLength() / Room::RATE);
  CHECK(std::all_of(room.heard.begin(), room.heard.end(), [](float v) { return v == 0.0f; }));

  // leaving: the guest disconnects, the host stops hosting
  CHECK(room.guest.command("disconnect") == "ok");
  CHECK(room.waitFor(room.guest, "state", "disconnected", 5));
  CHECK(room.waitFor(room.host, "users", "0", 10));
  CHECK(room.host.command("host off") == "ok");
  CHECK(room.waitFor(room.host, "hosting", "0", 10));
  CHECK(field(room.host.command("status"), "state") == "disconnected");

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
