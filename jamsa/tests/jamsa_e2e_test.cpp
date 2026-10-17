/*
    Jamsa - jamsa_e2e_test.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  Two engine wrappers (what a plug-in instance runs) in one process: the first
  hosts a room and joins it, the second connects to it. A 997 Hz tone goes into
  the first one's process() with a simulated clock (blocks of 512 at 48 kHz,
  paced at real time, as a host would), and the second one must hear it: SNR
  of at least 35 dB with Opus at the room's bitrate, 60 dB once the room is
  switched to FLAC with "codec flac". Modelled on njproto's
  engine/tests/test_engine_e2e.cpp, through the plug-in's own text commands.

*/

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

using namespace jamsa;

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
  CHECK(port >= 2049);
  CHECK(field(hostStatus, "hosting") == "1");
  CHECK(field(hostStatus, "codec") == "Opus");
  CHECK(field(hostStatus, "upload") == "OPUr");
  CHECK(field(hostStatus, "bpi") == "4");
  CHECK(field(hostStatus, "roomkbps") == "510"); // the host's own bitrate becomes the room's

  // the guest joins; it hears the host, who plays alone on a player output
  CHECK(room.guest.command("connect localhost:" + std::to_string(port) + " Guest") == "ok");
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
  CHECK(chat.find("Connected to localhost:") != std::string::npos);

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
