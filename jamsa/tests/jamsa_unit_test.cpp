/*
    Jamsa - jamsa_unit_test.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  Tests for the parts of the plug-in that need no network or host: the saved
  state, the players' names, following the host (lining the intervals up with
  its bars, voting for its tempo, the editor's note on its tempo), the
  metronome's click, what the editor's controls show and take, the text
  commands of Engine::command(), and the built-in server's addresses (the
  port mapper's lines, the public address lookup against a server on
  localhost, what the host bar shows). Ported from ninjam/vst3's
  plugin_unit_test.cpp (without its Link, text keys and clipboard checks).

*/

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "click.h"
#include "engine.h"
#include "hostserver.h"
#include "hostsync.h"
#include "uitext.h"

using namespace jamsa;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_failures++; printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static void test_state()
{
  Settings s;
  s.userName = "J\xc3\xb6rg \\ \"tab\"\there\nnewline";
  s.server = "studio.lan:2050";
  s.inputVolume = 0.25f;
  s.metronomeVolume = 1.5f;
  s.metronomeMute = true;
  s.monitor = false;
  s.transmit = false;
  s.editorWidth = 1200;
  s.editorHeight = 800;
  s.opusKbps = 510;
  s.remote["alice/0"].volume = 0.5f;
  s.remote["alice/0"].pan = -0.75f;
  s.remote["bob\tx/1"].mute = true;

  const std::string data = s.serialize();
  Settings r;
  CHECK(r.deserialize(data));
  CHECK_EQ(r.userName, s.userName);
  CHECK_EQ(r.server, s.server);
  CHECK(fabsf(r.inputVolume - 0.25f) < 1e-6f);
  CHECK(fabsf(r.metronomeVolume - 1.5f) < 1e-6f);
  CHECK(r.metronomeMute && !r.monitor && !r.transmit);
  CHECK(r.editorWidth == 1200 && r.editorHeight == 800);
  CHECK_EQ(r.opusKbps, 510);
  CHECK_EQ(r.remote.size(), (size_t)2);
  CHECK(fabsf(r.remote["alice/0"].pan + 0.75f) < 1e-6f);
  CHECK(r.remote["bob\tx/1"].mute);

  // values out of range are clamped, unknown keys skipped, and other data refused
  Settings c;
  CHECK(c.deserialize("NINJAM-VST3-STATE 1\ninput_volume=99000000\nmetronome_volume=-5\nfuture_key=1\nremote=x\t1\n"));
  CHECK(c.inputVolume == 4.0f && c.metronomeVolume == 0.0f && c.remote.empty());
  Settings d;
  d.userName = "keep";
  CHECK(!d.deserialize("garbage"));
  CHECK(!d.deserialize(""));
  CHECK_EQ(d.userName, "keep");

  // a state saved with defaults loads as defaults
  Settings e;
  CHECK(e.deserialize(Settings().serialize()));
  CHECK(e.monitor && e.transmit && e.inputVolume == 1.0f && e.editorWidth == 0);
  CHECK(Settings().metronomeMute); // the metronome starts off
  // the size of the editor before its 720 x 330 layout opens at the new default
  Settings older;
  CHECK(older.deserialize("NINJAM-VST3-STATE 1\neditor_size=960x640\n"));
  CHECK(older.editorWidth == 0 && older.editorHeight == 0);
  CHECK_EQ(e.opusKbps, 510);

  // the Opus bitrate is one of the choices: the nearest one, or the default for junk
  struct { const char *saved; int kbps; } bitrates[] = {
    { "64", 64 }, { "96", 96 }, { "160", 160 }, { "256", 256 }, { "510", 510 }, { "300", 256 }, { "999999999999", 510 },
    { "-5", 64 }, { "abc", 510 }, { "", 510 }, { "128x", 510 }, { "128", 128 },
  };
  for (auto &b : bitrates)
  {
    Settings k;
    CHECK(k.deserialize(std::string("NINJAM-VST3-STATE 1\nopus_kbps=") + b.saved + "\n"));
    CHECK_EQ(k.opusKbps, b.kbps);
  }
  // a state saved before the bitrate was a setting
  Settings old;
  CHECK(old.deserialize("NINJAM-VST3-STATE 1\nuser=x\n"));
  CHECK_EQ(old.opusKbps, 510);

  // Sync (following the host): on by default, and in states saved before it was a setting
  CHECK(Settings().followHostTempo);
  CHECK(old.followHostTempo);
  Settings off;
  off.followHostTempo = false;
  Settings offLoaded;
  CHECK(offLoaded.deserialize(off.serialize()));
  CHECK(!offLoaded.followHostTempo);
  CHECK(e.followHostTempo);
}

static void test_names()
{
  CHECK_EQ(Engine::displayName("alice@1.2.3.x"), "alice");
  CHECK_EQ(Engine::displayName("bob"), "bob");
  // the second of two players with one name ("AnonymousUsers multi")
  CHECK_EQ(Engine::displayName("guest@127.0.0.1.2"), "guest.2");
  CHECK_EQ(Engine::displayName("guest@1.2.3.x.16"), "guest.16");
  CHECK_EQ(Engine::displayName("guest@127.0.0.1"), "guest");
  CHECK_EQ(Engine::displayName("guest@::ffff:127.0.0.1.3"), "guest.3");
  CHECK_EQ(Engine::displayName("guest@::ffff:127.0.0.1"), "guest");
  CHECK_EQ(Engine::remoteKey("Guest@127.0.0.1.2", 0), "guest.2/0");
  CHECK_EQ(Engine::remoteKey("Alice@1.2.3.x", 2), "alice/2");

  // the name a new instance starts with, from the computer account
  CHECK_EQ(Engine::firstName("Leo Stern"), "Leo");
  CHECK_EQ(Engine::firstName("  J\xc3\xb6rg  M\xc3\xbcller "), "J\xc3\xb6rg");
  CHECK_EQ(Engine::firstName("leo"), "leo");
  CHECK_EQ(Engine::firstName("leo@example.com"), "leo");
  CHECK_EQ(Engine::firstName(""), "");
  CHECK_EQ(Engine::firstName(std::string(40, 'a')), std::string(32, 'a'));
  CHECK(!Engine::defaultUserName().empty()); // the test runs as some account
  {
    Engine en;
    const std::string def = en.getSettings().userName;
    CHECK_EQ(def, Engine::defaultUserName());
    Settings saved; // a set saved before a name was typed keeps the default
    en.setSettings(saved);
    CHECK_EQ(en.getSettings().userName, def);
    saved.userName = "typed";
    en.setSettings(saved);
    CHECK_EQ(en.getSettings().userName, "typed");
  }
}

// The click, rendered as the engine does: in blocks of any size that stop at the end of each
// interval. `lengths` are how long the intervals are (one may be shorter or longer than usual,

// the text commands, on an engine that isn't started (its settings take at once)
static void test_commands()
{
  Engine en;
  CHECK_EQ(en.command("name  Alice "), "ok");
  CHECK_EQ(en.getSettings().userName, "Alice");
  CHECK_EQ(en.command("transmit off"), "ok");
  CHECK(!en.getSettings().transmit);
  CHECK_EQ(en.command("transmit 1"), "ok");
  CHECK(en.getSettings().transmit);
  CHECK_EQ(en.command("monitor off"), "ok");
  CHECK(!en.getSettings().monitor);
  CHECK_EQ(en.command("metronome on"), "ok");
  CHECK(!en.getSettings().metronomeMute);
  CHECK_EQ(en.command("metronome off"), "ok");
  CHECK(en.getSettings().metronomeMute);
  CHECK_EQ(en.command("follow off"), "ok");
  CHECK(!en.getSettings().followHostTempo);
  CHECK_EQ(en.command("follow on"), "ok");
  CHECK(en.getSettings().followHostTempo);
  CHECK_EQ(en.command("bitrate 96"), "ok");
  CHECK_EQ(en.getSettings().opusKbps, 96);
  CHECK_EQ(en.command("bitrate 100"), "error: the bitrate is one of 64 96 128 160 256 510");
  CHECK_EQ(en.getSettings().opusKbps, 96);
  CHECK_EQ(en.command("codec mp3"), "error: codec opus or codec flac");
  CHECK_EQ(en.command("vote tempo 100"), "error: vote bpm N, or vote bpi N");
  CHECK_EQ(en.command("vote bpm x"), "error: vote bpm N, or vote bpi N");
  CHECK_EQ(en.command("dance"), "unknown command: dance");
  CHECK_EQ(en.command(""), "unknown command: ");

  // status reads the snapshot the engine's thread publishes: start it for that
  en.start();
  std::string st;
  for (int i = 0; i < 100 && st.find("ownkbps=96") == std::string::npos; i ++)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    st = en.command("status");
  }
  CHECK(st.find("state=disconnected") != std::string::npos);
  CHECK(st.find(" user= ") != std::string::npos); // the name as connected: none yet
  CHECK(st.find("ownkbps=96") != std::string::npos);
  CHECK(st.find("hosting=0") != std::string::npos);
  CHECK(st.find("follow=1") != std::string::npos);
  CHECK(st.find("license=0") != std::string::npos);
  CHECK_EQ(en.command("users"), "");
  CHECK_EQ(en.command("chatlog"), "");
  CHECK_EQ(en.command("nextinterval"), ""); // no host position yet
  CHECK(en.command("synctip").find("Sync with ") == 0);
  en.stop();
}

static bool nearly(double a, double b, double eps = 1e-9) { return fabs(a - b) <= eps; }

static void test_bar_math()
{
  using namespace barmath;
  CHECK(nearly(hostIntervalPos(0, 4, 96000), 0));
  CHECK(nearly(hostIntervalPos(1, 4, 96000), 24000));
  CHECK(nearly(hostIntervalPos(9.5, 4, 96000), 36000)); // bar 3, beat 2 and a half
  CHECK(nearly(hostIntervalPos(-1, 4, 96000), 72000));  // before bar 1 (a count-in)
  CHECK(nearly(hostIntervalPos(16, 16, 96000), 0));
  CHECK(nearly(hostIntervalPos(5, 0, 96000), 0));

  CHECK(nearly(wrapSamples(100, 1000), 100));
  CHECK(nearly(wrapSamples(900, 1000), -100));
  CHECK(nearly(wrapSamples(-900, 1000), 100));
  CHECK(nearly(wrapSamples(500, 1000), -500)); // [-len/2, len/2)

  // the interval playing now ends when the host's next one starts, or the one after,
  // whichever is closer to the usual length
  CHECK_EQ(intervalEnd(1000, 96000, 1000), 95000);    // in line: the usual end
  CHECK_EQ(intervalEnd(1000, 96000, 1500), 94500);    // behind the host: 500 shorter
  CHECK_EQ(intervalEnd(1500, 96000, 1000), 95000);    // ahead of it: 500 longer (1500 + 95000)
  CHECK_EQ(intervalEnd(10, 96000, 95990), 96010);     // the host's next start is 10 samples away: 20 longer instead
  CHECK_EQ(intervalEnd(90000, 96000, 5000), 91000);   // the host's interval just started: 85000 longer
  CHECK_EQ(intervalEnd(95999, 96000, 95999.6), 96000); // less than a sample to the host's start: the one after
  CHECK(intervalEnd(0, 96000, 0) >= 1);
}

// the engine's intervals, with njproto's Engine::setIntervalEnd() as it does it
struct Intervals
{
  int len, cur, pos, count = 0;
  long now = 0;              // samples played
  std::vector<long> starts;  // when each interval started
  Intervals(int len, int pos) : len(len), cur(len), pos(pos) {}
  void setEnd(int samples) { cur = pos + samples; }
  void play(int frames)
  {
    for (int i = 0; i < frames; i ++, now ++)
    {
      if (pos >= cur)
      {
        pos = 0;
        cur = len;
        count++;
        starts.push_back(now);
      }
      pos++;
    }
  }
};

// a host at a tempo, playing (or not) from a place, maybe in a loop
struct Host
{
  HostPosition p;
  double srate = 48000;
  bool hideCycle = false; // loops without saying so: the plug-in doesn't see p.cycle*
  double wobbleMs = 0;    // the position it gives is off by up to this much, at random
  unsigned seed = 1;
  void play(int frames)
  {
    if (!p.playing) return;
    p.ppq += frames / srate * p.tempo / 60;
    if (p.cycleActive && p.ppq >= p.cycleEnd) p.ppq -= p.cycleEnd - p.cycleStart;
  }
  // what the plug-in is told
  HostPosition told()
  {
    HostPosition t = p;
    if (hideCycle) t.cycleActive = false;
    if (wobbleMs > 0)
    {
      seed = seed * 1103515245u + 12345u;
      const double r = ((seed >> 8) & 0xffff) / 65535.0 * 2 - 1; // -1 .. 1
      t.ppq += r * wobbleMs / 60000.0 * p.tempo;
    }
    return t;
  }
};

// runs `seconds` of blocks through BarSync; returns the interval starts (samples) and checks
// each one that started after `alignedFrom` sits where the host is at a multiple of bpi beats
struct Run
{
  BarSync sync;
  Host host;
  Intervals iv;
  double bpm;
  int bpi, frames = 512;
  double hostBeatsAtMove = 0; long moveSample = 0; // to find where the host is, without its loops
  Run(double bpm, int bpi, int startPos)
    : iv((int)((double)bpi / (bpm / 60.0) * 48000), startPos), bpm(bpm), bpi(bpi) {}
  void move(double ppq)
  {
    host.p.ppq = ppq;
    hostBeatsAtMove = ppq;
    moveSample = iv.now;
  }
  // the host's beat at sample n, as if it never looped
  double beatAt(long n) const { return hostBeatsAtMove + (n - moveSample) / host.srate * host.p.tempo / 60; }
  void run(double seconds)
  {
    for (long n = 0; n < seconds * host.srate / frames; n ++)
    {
      BarSync::Interval i;
      i.running = true;
      i.pos = iv.pos;
      i.len = iv.len;
      i.bpi = bpi;
      i.bpm = bpm;
      i.count = iv.count;
      const int end = sync.process(host.told(), frames, host.srate, i);
      if (end > 0) iv.setEnd(end);
      iv.play(frames);
      const double was = host.p.ppq;
      host.play(frames);
      if (host.hideCycle && host.p.ppq < was && !wrapped)
      {
        // the first time round a loop the host doesn't declare is a move for the plug-in
        wrapped = true;
        hostBeatsAtMove = host.p.ppq;
        moveSample = iv.now;
      }
    }
  }
  bool wrapped = false;
  // the largest distance (ms) from a host interval start, of the interval starts after sample `from`
  double worstMs(long from) const
  {
    double worst = 0;
    for (long st : iv.starts)
      if (st >= from)
      {
        const double b = fmod(beatAt(st), bpi);
        const double off = std::min(b, bpi - b) * 60000.0 / host.p.tempo;
        worst = std::max(worst, off);
      }
    return worst;
  }
  int startsAfter(long from) const { return (int)std::count_if(iv.starts.begin(), iv.starts.end(), [&](long s) { return s >= from; }); }
  // are the intervals that started after `from` all of the usual length?
  bool regular(long from) const { return odd(from) == 0; }
  // how many intervals that started after `from` were of another length (what the others hear)
  int odd(long from = 0) const
  {
    int n = 0;
    for (size_t k = 1; k < iv.starts.size(); k ++)
      if (iv.starts[k - 1] >= from && iv.starts[k] - iv.starts[k - 1] != iv.len) n++;
    return n;
  }
};

static void test_bar_sync()
{
  // The host starts playing in the middle of bar 3, at the room's tempo: the interval playing
  // then ends early or late, and from the next one on they start on the host's bars.
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = true;
    r.host.p.tempo = 100;
    r.run(3); // stopped: nothing moves
    CHECK(r.sync.state() == BarSync::Stopped);
    CHECK_EQ(r.sync.corrections(), 0);
    CHECK(r.regular(0));
    r.host.p.playing = true;
    r.move(9.37);
    const long start = r.iv.now;
    r.run(0.1);
    CHECK(r.sync.state() == BarSync::Lining);
    r.run(30);
    CHECK(r.sync.state() == BarSync::Aligned);
    CHECK_EQ(r.sync.corrections(), 1);
    const long firstInLine = r.iv.starts.empty() ? 0 : *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > start; });
    CHECK(firstInLine - start <= (long)(r.iv.len * 1.5)); // within an interval and a half
    CHECK(r.startsAfter(firstInLine) >= 10);
    CHECK(r.worstMs(firstInLine) < 1.0 / 48); // within a sample
    CHECK(r.regular(firstInLine));
    printf("bars: in line %.2f s after the host started, worst %.4f ms\n", (firstInLine - start) / 48000.0, r.worstMs(firstInLine));

    // the host loops over 6 beats (not a multiple of BPI): the intervals play on as they were
    const int fixes = r.sync.corrections();
    r.host.p.cycleActive = true;
    r.host.p.cycleStart = 40;
    r.host.p.cycleEnd = 46;
    r.move(40);
    r.run(0.05);
    CHECK_EQ(r.sync.corrections(), fixes + 1); // moved into the loop: in line from there
    const long looping = r.iv.now;
    r.run(40); // more than ten loops
    CHECK_EQ(r.sync.corrections(), fixes + 1);
    CHECK(r.regular(looping));
    CHECK(r.worstMs(looping + r.iv.len * 2) < 1.0 / 48);
    r.host.p.cycleActive = false;

    // the playhead moves: in line again from the next interval
    r.move(101.5);
    const long moved = r.iv.now;
    r.run(20);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.sync.state() == BarSync::Aligned);
    const long next = *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > moved; });
    CHECK(r.worstMs(next) < 1.0 / 48);

    // stopped: the intervals play on, as they are
    r.host.p.playing = false;
    const long stopped = r.iv.now;
    r.run(20);
    CHECK(r.sync.state() == BarSync::Stopped);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.regular(stopped));

    // another tempo than the room's: left alone
    r.host.p.playing = true;
    r.host.p.tempo = 101;
    r.move(3);
    const long other = r.iv.now;
    r.run(20);
    CHECK(r.sync.state() == BarSync::OtherTempo);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.regular(other));

    // the interval each fix shortens or lengthens is one the room hears: one for each, no other
    CHECK_EQ(r.odd(), r.sync.corrections());
  }

  // A loop the host doesn't declare (no loop in the play head's position): the first time round
  // is a move (one fix, in line from there), then each time is the same jump back, a loop
  // (no fix, the intervals play on as they were).
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = true;
    r.host.p.tempo = 100;
    r.run(3);
    r.host.p.playing = true;
    r.host.hideCycle = r.host.p.cycleActive = true;
    r.host.p.cycleStart = 40;
    r.host.p.cycleEnd = 46;
    r.move(37); // a beat before the loop
    r.run(0.1);
    CHECK_EQ(r.sync.corrections(), 1); // started
    r.run(5.6); // 9 beats to the loop's end
    CHECK(r.wrapped);
    CHECK_EQ(r.sync.corrections(), 2); // the first time round
    const long wrapped = r.moveSample;
    r.run(60); // sixteen times round
    CHECK_EQ(r.sync.corrections(), 2);
    const long next = *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > wrapped; });
    CHECK(r.regular(next));
    CHECK(r.worstMs(next) < 1.0 / 48); // in line with the host as if it hadn't looped since the first time
    CHECK_EQ(r.odd(), 2);
    printf("bars: a loop the host doesn't declare, %d times round: %d fixes, %d intervals of another length\n",
           (int)(60 / (6 * 60.0 / 100)), r.sync.corrections(), r.odd());

    // a move to another place is a move again, not a loop
    r.host.hideCycle = r.host.p.cycleActive = false;
    r.move(101.5);
    r.run(10);
    CHECK_EQ(r.sync.corrections(), 3);
    CHECK_EQ(r.odd(), 3);
  }

  // A host whose position wobbles (up to 2 ms off, at random, less than a move): one fix
  // when it starts, then none, so the room doesn't get an interval of another length now and then.
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = r.host.p.playing = true;
    r.host.p.tempo = 100;
    r.host.wobbleMs = 2.0;
    r.move(9.37);
    r.run(60 * 5);
    printf("bars: a host 2 ms off at random, 5 minutes: %d fixes, worst %.3f ms\n", r.sync.corrections(), r.worstMs(r.iv.len * 2));
    CHECK_EQ(r.sync.corrections(), 1);
    CHECK(r.worstMs(r.iv.len * 2) < 2.1);
  }

  // the engine's interval is a whole number of samples (0.63 short of 16 beats at 95 BPM): it
  // drifts from the host's bars by about a sample an interval, and gets a sample-sized fix now
  // and then
  {
    Run r(95, 16, 0);
    r.host.p.hasPosition = r.host.p.playing = true;
    r.host.p.tempo = 95;
    r.move(0);
    r.run(15);
    const long from = r.iv.now;
    r.run(60 * 20); // twenty minutes
    printf("bars at 95 BPM, 16 BPI, 20 minutes: %d fixes, worst %.3f ms\n", r.sync.corrections(), r.worstMs(from));
    CHECK(r.sync.corrections() <= 3);
    CHECK(r.worstMs(from) <= BarSync::TOLERANCE_MS + 0.05);
    CHECK_EQ(r.odd(), r.sync.corrections()); // each one a single interval of another length
  }
}

static void test_tempo_follower()
{
  TempoFollower f;
  // not in a room yet: the host's tempo is known, and waits for one
  CHECK_EQ(f.update(0, 100, 0), 0);
  CHECK_EQ(f.update(2, 100, 0), 0);
  CHECK(f.waiting());
  // joins a room at 120: one vote for 100
  f.arm();
  CHECK_EQ(f.update(3, 100, 120), 100);
  CHECK_EQ(f.update(3.05, 100, 120), 0);
  CHECK_EQ(f.update(4, 100, 100), 0);
  // someone else changes the room: no vote back
  for (double t = 5; t < 30; t += 0.05) CHECK_EQ(f.update(t, 100, 90), 0);
  // the host's tempo is dragged to 109.6: one vote, for 110, a second after it stops
  int votes = 0;
  double when = 0;
  const double drag[] = { 104, 106.5, 108, 109.6 };
  for (int i = 0; i < 4; i ++)
    for (double t = 30 + i * 0.2; t < 30 + (i + 1) * 0.2 - 1e-9; t += 0.05)
      if (f.update(t, drag[i], 90)) votes++;
  for (double t = 30.8; t < 35; t += 0.05)
  {
    const int bpm = f.update(t, 109.6, 90);
    if (bpm)
    {
      votes++;
      when = t;
      CHECK_EQ(bpm, 110);
    }
  }
  CHECK_EQ(votes, 1);
  CHECK(when >= 30.6 + TempoFollower::QUIET - 1e-6 && when < 30.6 + TempoFollower::QUIET + 0.11);
  // the room doesn't take it: no vote again until the host's tempo changes
  for (double t = 35; t < 60; t += 0.05) CHECK_EQ(f.update(t, 109.6, 90), 0);
  // a tempo that rounds to the room's, or that no room can have: no vote
  for (double t = 60; t < 63; t += 0.05) CHECK_EQ(f.update(t, 90.3, 90), 0);
  for (double t = 63; t < 66; t += 0.05) CHECK_EQ(f.update(t, 30, 90), 0);
  for (double t = 66; t < 69; t += 0.05) CHECK_EQ(f.update(t, 420, 90), 0);
  // the host stops saying its tempo for a while: nothing
  for (double t = 69; t < 72; t += 0.05) CHECK_EQ(f.update(t, 0, 90), 0);
  // following again (the toggle): one vote for the host's tempo, at once, as it is still
  CHECK_EQ(f.update(72, 420, 90), 0);
  CHECK_EQ(f.update(74, 128, 90), 0);
  CHECK_EQ(f.update(75.5, 128, 90), 128);
  f.arm();
  CHECK_EQ(f.update(76, 128, 90), 128);
  CHECK_EQ(f.update(77, 128, 90), 0);
  CHECK_EQ(f.votes(), 4);
}

static void test_host_note()
{
  CHECK_EQ(shortHostName("Ableton Live 12 Suite"), "Live");
  CHECK_EQ(shortHostName(" Live "), "Live");
  CHECK_EQ(shortHostName("Bitwig Studio"), "Bitwig Studio");
  CHECK_EQ(shortHostName(""), "The DAW");
  CHECK_EQ(shortHostName("A very long host name that goes on and on"), "A very long host name that goes ");
  CHECK_EQ(shortHostName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x\xc3\xa9\xc3\xa9"),
           "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x");

  CHECK_EQ(bpmText(120), "120");
  CHECK_EQ(bpmText(109.6), "109.6");
  CHECK_EQ(bpmText(120.25), "120.25");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, true), "Live is at 120 BPM, the room at 95: set Live to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, false),
           "Live is at 120 BPM, the room at 95: set Live to 95, or turn on Sync");
  CHECK_EQ(hostTempoNote("Bitwig Studio", 95.3, 95, true), "Bitwig Studio is at 95.3 BPM, the room at 95: set its tempo to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 95.004, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 0, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 120, 0, true), "");

  // the transport bar's name for the host
  CHECK_EQ(hostChipName("Ableton Live 12 Suite"), "Live");
  CHECK_EQ(hostChipName("REAPER"), "REAPER");
  CHECK_EQ(hostChipName("Bitwig Studio"), "Host");
  CHECK_EQ(hostChipName(""), "Host");
  CHECK_EQ(hostChipName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"), "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");

  // where the next interval starts, in the host's bars
  CHECK_EQ(nextIntervalText("Live", 32, 4, 4), "Next interval at Live bar 9");
  CHECK_EQ(nextIntervalText("Live", 31.9999999, 0, 0), "Next interval at Live bar 9"); // 4/4 when the host doesn't say
  CHECK_EQ(nextIntervalText("Live", 34, 4, 4), "Next interval at Live bar 9, beat 3");
  CHECK_EQ(nextIntervalText("Host", 0, 3, 4), "Next interval at Host bar 1");
  CHECK_EQ(nextIntervalText("Live", 9, 6, 8), "Next interval at Live bar 4"); // 6/8: a bar is 3 quarter notes
  CHECK_EQ(nextIntervalText("Live", 10, 6, 8), "Next interval at Live bar 4, beat 3");
  CHECK_EQ(nextIntervalText("Live", 33.25, 4, 4), "Next interval at Live bar 9, beat 2");
  int bar = 0, beat = 0;
  bool onBeat = true;
  barAndBeat(33.25, 4, 4, &bar, &beat, &onBeat);
  CHECK(bar == 9 && beat == 2 && !onBeat);
}



static std::vector<float> render_clicks(double sr, const std::vector<int> &lengths, int usual, int bpi, float volume,
                                        std::vector<long> &onsets, std::vector<bool> &accents)
{
  long total = 0;
  for (int l : lengths) total += l;
  std::vector<float> left(total, 0.0f), right(total, 0.0f);
  Click click;
  click.setSampleRate(sr);
  unsigned seed = 12345;
  long t = 0;
  for (size_t iv = 0; iv < lengths.size(); iv ++)
  {
    for (int k = 0; k < bpi; k ++)
    {
      const long p = (long)llround((double)k * usual / bpi);
      if (p < lengths[iv])
      {
        onsets.push_back(t + p);
        accents.push_back(k == 0);
      }
    }
    for (int pos = 0; pos < lengths[iv]; )
    {
      seed = seed * 1103515245u + 12345u;
      const int n = std::min(1 + (int)((seed >> 16) % 700), lengths[iv] - pos);
      click.render(&left[t], &right[t], n, pos, usual, bpi, volume);
      pos += n;
      t += n;
    }
  }
  CHECK(left == right);
  return left;
}

// how much of x[from, to) is at hz (Goertzel), per sample
static double tone_power(const std::vector<float> &x, long from, long to, double hz, double sr)
{
  const double w = 2 * 3.14159265358979 * hz / sr, c = 2 * cos(w);
  double s1 = 0, s2 = 0;
  for (long i = from; i < to; i ++)
  {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)(to - from);
}

static void test_click()
{
  // the click itself
  CHECK_EQ(Click::sample(true, 0, 48000), 0.0);
  CHECK_EQ(Click::sample(false, 0, 48000), 0.0);
  CHECK_EQ(Click::sample(true, Click::length(48000), 48000), 0.0);
  CHECK_EQ(Click::length(48000), 1920);
  CHECK_EQ(Click::length(44100), 1764);
  CHECK_EQ(Click::length(96000), 3840);

  const float volume = 0.5f;
  struct Heard { double peakMs, quietMs, energy, accentPeak, beatPeak; };
  std::vector<Heard> heard;
  for (double sr : { 44100.0, 48000.0, 96000.0 })
  {
    // 120 BPM, 4 beats: three intervals of 2 seconds, but the second ends 0.7 seconds
    // early (it lines up with the host's bars), and the third 0.3 seconds late
    const int usual = (int)(2.0 * sr), bpi = 4;
    const std::vector<int> lengths = { usual, usual - (int)(0.7 * sr), usual + (int)(0.3 * sr) };
    std::vector<long> onsets;
    std::vector<bool> accents;
    const std::vector<float> x = render_clicks(sr, lengths, usual, bpi, volume, onsets, accents);
    CHECK_EQ(onsets.size(), (size_t)11); // 4 + 3 (the fourth beat of the short one never comes) + 4
    const int len = Click::length(sr);
    const double slope = 2 * 3.14159265358979 * Click::ACCENT_HZ / sr; // how fast a full-level sine at 1500 Hz can change

    // each click starts on its beat, from silence, and ends in silence: nothing sounds anywhere else
    std::vector<bool> inClick(x.size(), false);
    Heard h = { 0, 0, 0, 0, 1 };
    for (size_t b = 0; b < onsets.size(); b ++)
    {
      const long p = onsets[b];
      for (long i = p; i < std::min<long>(p + len, (long)x.size()); i ++) inClick[i] = true;
      CHECK(p == 0 || x[p - 1] == 0.0f); // silence before
      CHECK_EQ(x[p], 0.0f);              // the attack starts at 0 on the beat's sample
      CHECK(x[p + 1] != 0.0f);           // and sounds from the next one
      CHECK(fabsf(x[p + 1]) < 0.001f * volume && fabsf(x[p + 2]) < 0.004f * volume); // no step at the start
      CHECK(fabsf(x[p + len - 1]) < 1e-4f * volume);                                  // nor at the end
      CHECK(p + len >= (long)x.size() || x[p + len] == 0.0f);
      double worstStep = 0, peak = 0;
      long peakAt = p;
      for (long i = p; i < p + len; i ++)
      {
        worstStep = std::max(worstStep, (double)fabsf(x[i + 1] - x[i]));
        if (fabsf(x[i]) > peak) { peak = fabsf(x[i]); peakAt = i; }
      }
      CHECK(worstStep <= volume * slope * 1.02); // no step anywhere: only the sine's own slope

      // beat 1 of each interval is the accent: 1500 Hz and louder; the others 1000 Hz at 60%
      const double hi = tone_power(x, p, p + len, Click::ACCENT_HZ, sr), lo = tone_power(x, p, p + len, Click::BEAT_HZ, sr);
      if (accents[b])
      {
        CHECK(hi > lo * 20);
        CHECK(peak > 0.9 * volume && peak <= volume);
        h.accentPeak = peak;
        h.peakMs = (peakAt - p) * 1000.0 / sr;
        long quiet = p;
        for (long i = p; i < p + len; i ++)
          if (fabsf(x[i]) > peak * 0.001) quiet = i;
        h.quietMs = (quiet - p) * 1000.0 / sr;
        h.energy = 0;
        for (long i = p; i < p + len; i ++) h.energy += x[i] * (double)x[i] / sr;
      }
      else
      {
        CHECK(lo > hi * 20);
        CHECK(peak > 0.54 * volume && peak <= 0.6 * volume);
        h.beatPeak = std::min(h.beatPeak, peak);
      }
    }
    for (size_t i = 0; i < x.size(); i ++)
      if (!inClick[i] && x[i] != 0.0f)
      {
        CHECK(!"a sound outside the clicks");
        break;
      }
    printf("click at %.0f Hz: accent peak %.3f at %.2f ms, -60 dB at %.1f ms, energy %.3g; other beats peak %.3f\n", sr, h.accentPeak,
           h.peakMs, h.quietMs, h.energy, h.beatPeak);
    heard.push_back(h);

    // muted (volume 0): nothing
    std::vector<long> o2;
    std::vector<bool> a2;
    const std::vector<float> silent = render_clicks(sr, { usual }, usual, bpi, 0.0f, o2, a2);
    CHECK(std::all_of(silent.begin(), silent.end(), [](float v) { return v == 0.0f; }));
  }

  // it sounds the same at 44.1, 48 and 96 kHz
  for (const Heard &h : heard)
  {
    CHECK(fabs(h.peakMs - heard[1].peakMs) < 0.35);   // within half a period of 1500 Hz
    CHECK(fabs(h.quietMs - heard[1].quietMs) < 0.35);
    CHECK(fabs(h.energy / heard[1].energy - 1) < 0.01);
    CHECK(fabs(h.accentPeak - heard[1].accentPeak) < 0.01);
  }
  CHECK(heard[1].quietMs > 30 && heard[1].quietMs < 40); // -60 dB in 30 to 40 ms
}

static void test_uitext()
{
  using namespace uitext;

  // dragging the BPM box: 2 pixels a BPM, 8 with Shift; whole steps only, toward the start
  CHECK_EQ(dragBpm(120, 0, false), 120);
  CHECK_EQ(dragBpm(120, 1.9, false), 120);
  CHECK_EQ(dragBpm(120, 2, false), 121);
  CHECK_EQ(dragBpm(120, -2, false), 119);
  CHECK_EQ(dragBpm(120, -3.9, false), 119);
  CHECK_EQ(dragBpm(120, 20, false), 130);
  CHECK_EQ(dragBpm(120, 20, true), 122);
  CHECK_EQ(dragBpm(120, -7.9, true), 120);
  CHECK_EQ(dragBpm(120, 1000, false), BPM_MAX);
  CHECK_EQ(dragBpm(120, -1000, false), BPM_MIN);
  CHECK_EQ(dragBpm(300, 0, false), BPM_MAX); // a room faster than the box goes: the box's range

  // typing in it
  int bpm = 0;
  CHECK(parseBpm("125", &bpm) && bpm == 125);
  CHECK(parseBpm(" 125.4 ", &bpm) && bpm == 125);
  CHECK(parseBpm("125.5", &bpm) && bpm == 126);
  CHECK(parseBpm("95,5", &bpm) && bpm == 96);
  CHECK(parseBpm("100 bpm", &bpm) && bpm == 100);
  CHECK(parseBpm("100BPM", &bpm) && bpm == 100);
  CHECK(parseBpm(".9", &bpm) && bpm == BPM_MIN);
  CHECK(parseBpm("20", &bpm) && bpm == BPM_MIN);
  CHECK(parseBpm("999", &bpm) && bpm == BPM_MAX);
  CHECK(parseBpm("99999999999999", &bpm) && bpm == BPM_MAX);
  bpm = 77;
  CHECK(!parseBpm("", &bpm));
  CHECK(!parseBpm("   ", &bpm));
  CHECK(!parseBpm("fast", &bpm));
  CHECK(!parseBpm("120 beats", &bpm));
  CHECK(!parseBpm("-120", &bpm));
  CHECK(!parseBpm(".", &bpm));
  CHECK(!parseBpm("1e3", &bpm));
  CHECK_EQ(bpm, 77);

  CHECK_EQ(bpmBoxText(120), std::string("120.00"));
  CHECK_EQ(bpmBoxText(95.456), std::string("95.46"));
  CHECK_EQ(bpmBoxText(239.999), std::string("240.00"));
  CHECK_EQ(bpmBoxText(0), std::string("\xe2\x80\x94"));
  CHECK_EQ(bpmBoxText(-5), std::string("\xe2\x80\x94"));

  CHECK_EQ(dbText(1.0f), std::string("0.0 dB"));
  CHECK_EQ(dbText(0.9999f), std::string("0.0 dB"));
  CHECK_EQ(dbText(2.0f), std::string("+6.0 dB"));
  CHECK_EQ(dbText(0.25f), std::string("-12.0 dB"));
  CHECK_EQ(dbText(0.5f), std::string("-6.0 dB"));
  CHECK_EQ(dbText(0.989f), std::string("-0.1 dB"));
  CHECK_EQ(dbText(0.0f), std::string("-inf dB"));
  CHECK_EQ(dbText(-1.0f), std::string("-inf dB"));

  // the meter: nothing for silence, all seven at 0 dB, the yellow two only near the top
  CHECK_EQ(litSegments(0.0f), 0);
  CHECK_EQ(litSegments(0.001f), 0);          // -60 dB
  CHECK_EQ(litSegments(0.004f), 1);          // -48 dB
  CHECK_EQ(litSegments(0.1f), 3);            // -20 dB
  CHECK_EQ(litSegments(0.5f), 5);            // -6 dB: all the green ones
  CHECK_EQ(litSegments(0.6f), 6);            // -4.4 dB
  CHECK_EQ(litSegments(0.95f), 7);           // -0.4 dB
  CHECK_EQ(litSegments(1.0f), METER_SEGMENTS);
  CHECK_EQ(litSegments(4.0f), METER_SEGMENTS);
  for (int i = 1; i < 1000; i++) CHECK(litSegments(i / 1000.0f) >= litSegments((i - 1) / 1000.0f));

  CHECK_EQ(beatText(0, 48000, 16), std::string("1 / 16"));
  CHECK_EQ(beatText(47999, 48000, 16), std::string("16 / 16"));
  CHECK_EQ(beatText(48000, 48000, 16), std::string("16 / 16"));
  CHECK_EQ(beatText(6000, 48000, 16), std::string("3 / 16"));
  CHECK_EQ(beatText(5999, 48000, 16), std::string("2 / 16"));
  CHECK_EQ(beatText(0, 0, 16), std::string("\xe2\x80\x93 / 16"));
  CHECK_EQ(beatText(0, 48000, 0), std::string("\xe2\x80\x93 / \xe2\x80\x93"));

  CHECK_EQ(kbpsText(128), std::string("128k"));

  // the BPI list holds the room's usual choices, in order
  CHECK_EQ((int)(sizeof(BPI_CHOICES) / sizeof(BPI_CHOICES[0])), NUM_BPI_CHOICES);
  for (int i = 1; i < NUM_BPI_CHOICES; i++) CHECK(BPI_CHOICES[i] > BPI_CHOICES[i - 1]);
  CHECK_EQ(BPI_CHOICES[0], 2); // the server's least
  CHECK_EQ(BPI_CHOICES[NUM_BPI_CHOICES - 1], 64); // and its most
}

// the port mapper's lines and the server's, as HostServer reads them
static void test_portmap_messages()
{
  HostServer::State st;
  std::map<std::string, std::string> conns;
  bool show = false;
  HostServer::parseLogLine("[portmap] asking the router to forward TCP port 2049 to this machine", st, &show, &conns);
  CHECK(show && st.portmapPending && st.publicAddress.empty());

  HostServer::parseLogLine("[portmap] UPnP: people can connect to 203.0.113.7:2049", st, &show, &conns);
  CHECK(show && !st.portmapPending);
  CHECK_EQ(st.publicAddress, "203.0.113.7:2049");
  CHECK(st.portmapNote.empty());

  HostServer::parseLogLine("[portmap] lost the forward for TCP port 2049, trying again", st, &show, &conns);
  CHECK(st.publicAddress.empty() && st.portmapPending);
  CHECK_EQ(st.portmapNote, "lost the forward for TCP port 2049, trying again");

  HostServer::parseLogLine("[portmap] NAT-PMP: router forwards TCP port 2049 to this machine, but the router's own internet address is 100.64.0.9, "
                           "which is not public. Your internet provider probably uses carrier-grade NAT.", st, &show, &conns);
  CHECK(st.publicAddress.empty() && !st.portmapPending);
  CHECK(st.portmapNote.find("not public") != std::string::npos);
  CHECK(st.portmapNote.compare(0, 8, "NAT-PMP:") == 0);

  HostServer::parseLogLine("[portmap] could not find a router that supports UPnP or NAT-PMP. Forward TCP port 2049 on your router "
                           "to this machine by hand, or turn on UPnP in the router's settings. Will keep trying every 5 minutes.", st, &show, &conns);
  CHECK(show && st.publicAddress.empty() && !st.portmapPending);
  CHECK(st.portmapNote.compare(0, 24, "could not find a router ") == 0);

  HostServer::parseLogLine("[portmap] NAT-PMP: people can connect to 198.51.100.4:2050.", st, &show, &conns);
  CHECK_EQ(st.publicAddress, "198.51.100.4:2050"); // without the full stop
  HostServer::parseLogLine("[portmap] UPnP: removed the forward for TCP port 2049", st, &show, &conns);
  CHECK(st.publicAddress.empty() && !st.portmapPending);

  // the server's lines: only errors and warnings are shown
  HostServer::parseLogLine("listening on port 2049", st, &show, &conns);
  CHECK(!show);
  HostServer::parseLogLine("warning: Opus bitrate 9 is outside 32-510, using 128", st, &show, &conns);
  CHECK(show);
  HostServer::parseLogLine("Error: something", st, &show, &conns);
  CHECK(show);

  // a player who joins from the internet shows the port is open, one from the local network doesn't
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("connection 1 from 192.168.1.20", st, &show, &conns);
  HostServer::parseLogLine("login 1 as Ana privs=cv", st, &show, &conns);
  CHECK(!st.joinedFromInternet && !show);
  HostServer::parseLogLine("connection 2 from 127.0.0.1", st, &show, &conns);
  HostServer::parseLogLine("login 2 as Bo privs=cv", st, &show, &conns);
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("connection 3 from 203.0.113.9", st, &show, &conns);
  HostServer::parseLogLine("refusing login 3: invalid login/password", st, &show, &conns);
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("login 3 as Cy privs=cv", st, &show, &conns);
  CHECK(st.joinedFromInternet && !show);
  CHECK(conns.empty()); // forgotten once logged in
  HostServer::parseLogLine("login 9 as Nobody privs=cv", st, &show, &conns); // never seen connecting
  CHECK(!show);

  CHECK(HostServer::isPublicIPv4("89.114.98.123"));
  CHECK(HostServer::isPublicIPv4("172.32.0.1"));
  CHECK(HostServer::isPublicIPv4("100.128.0.1"));
  for (const char *a : { "10.0.0.5", "172.16.0.1", "172.31.255.255", "192.168.1.1", "127.0.0.1", "169.254.3.4",
                         "100.64.0.9", "100.127.255.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "", "1.2.3", "a.b.c.d" })
    CHECK(!HostServer::isPublicIPv4(a));

  // the public address from a "what's my IP" service
  std::string ip;
  CHECK(HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n89.114.98.123\n", &ip));
  CHECK_EQ(ip, "89.114.98.123");
  CHECK(HostServer::parseWhatsMyIpReply("HTTP/1.0 200 OK\r\n\r\n203.0.113.7", &ip));
  CHECK_EQ(ip, "203.0.113.7");
  ip = "kept";
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 503 Service Unavailable\r\n\r\n1.2.3.4\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n<html>1.2.3.4</html>", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n1.2.3.256\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n1.2.3\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n2001:db8::1\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("", &ip));
  CHECK_EQ(ip, "kept");
}

// A "what's my IP" service on localhost: answers each connection with reply and closes. Stops
// after `answers` connections.
struct FakeWhatsMyIp
{
#ifdef _WIN32
  typedef SOCKET sock_t;
#else
  typedef int sock_t;
#endif
  sock_t listener;
  int port = 0;
  std::thread thread;

  // listening=false holds the port without listening: a connection to it is refused at once,
  // and nothing else can take the port meanwhile (Windows hands out ports in order, so a
  // "port + 1" guess could be the very socket that connects to it)
  FakeWhatsMyIp(const std::string &reply, int answers, bool listening = true)
  {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001u);
    a.sin_port = 0;
    socklen_t len = sizeof(a);
    if (bind(listener, (sockaddr *)&a, sizeof(a)) == 0 && (!listening || listen(listener, 4) == 0) && getsockname(listener, (sockaddr *)&a, &len) == 0)
      port = ntohs(a.sin_port);
    thread = std::thread([this, reply, answers]() {
      for (int i = 0; i < answers; i ++)
      {
        const sock_t c = accept(listener, nullptr, nullptr);
#ifdef _WIN32
        if (c == INVALID_SOCKET) break;
#else
        if (c < 0) break;
#endif
        char buf[1024];
        recv(c, buf, sizeof(buf), 0); // the request
        send(c, reply.data(), (int)reply.size(), 0);
#ifdef _WIN32
        closesocket(c);
#else
        close(c);
#endif
      }
    });
  }
  ~FakeWhatsMyIp()
  {
    thread.join();
#ifdef _WIN32
    closesocket(listener);
#else
    close(listener);
#endif
  }
  std::string spec(const char *path = "/") const { return "127.0.0.1:" + std::to_string(port) + path; }
};

static void test_public_address_lookup()
{
  std::atomic<bool> stop { false };
  {
    FakeWhatsMyIp service("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n89.114.98.123\n", 1);
    CHECK(service.port > 0);
    CHECK_EQ(HostServer::whatsMyIp(service.spec("/ip"), &stop), "89.114.98.123");
  }
  {
    // a service that doesn't give an address
    FakeWhatsMyIp service("HTTP/1.1 200 OK\r\n\r\n<html>hello</html>\n", 1);
    CHECK_EQ(HostServer::whatsMyIp(service.spec(), &stop), "");
  }
  // "off" asks nobody, and a service nothing listens on gives nothing (at once: the connection is refused)
  CHECK_EQ(HostServer::whatsMyIp("off", &stop), "");
  {
    FakeWhatsMyIp closed("", 0, false);
    CHECK(closed.port > 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(HostServer::whatsMyIp(closed.spec(), &stop), "");
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
  }
  // stopped before it starts
  std::atomic<bool> stopped { true };
  CHECK_EQ(HostServer::whatsMyIp("127.0.0.1:1/", &stopped), "");
}

// what the host bar shows, from what the server knows
static void test_host_bar()
{
  using uitext::HostBar;
  uitext::HostInfo h;
  h.port = 2049;

  // nothing known at all
  HostBar b = uitext::hostBar(h);
  CHECK_EQ(b.addressKey, "LAN");
  CHECK(b.address.empty());
  CHECK_EQ(b.placeholder, "not known");
  CHECK(b.port == HostBar::Forward);
  CHECK_EQ(b.portText, "Forward port 2049");
  CHECK(b.portTip.find("(TCP) to this computer in your router's settings") != std::string::npos);
  CHECK(b.portTip.find("The router said") == std::string::npos);

  // just started: the LAN address while the router and the service are asked
  h.localAddress = "192.168.1.20:2049";
  h.portmapPending = true;
  h.lookupPending = true;
  b = uitext::hostBar(h);
  CHECK_EQ(b.addressKey, "LAN");
  CHECK_EQ(b.address, "192.168.1.20:2049");
  CHECK(b.port == HostBar::Asking);
  CHECK_EQ(b.portText, "Asking the router\xe2\x80\xa6");
  CHECK(b.addressTip.find("being looked up") != std::string::npos);
  h.localAddress.clear();
  b = uitext::hostBar(h);
  CHECK(b.address.empty());
  CHECK_EQ(b.placeholder, "looking it up\xe2\x80\xa6");
  h.localAddress = "192.168.1.20:2049";

  // the mapper's success line: the router's address, port open
  h.publicAddress = "203.0.113.7:2049";
  h.portmapPending = false;
  b = uitext::hostBar(h);
  CHECK_EQ(b.addressKey, "WAN");
  CHECK_EQ(b.address, "203.0.113.7:2049");
  CHECK(b.port == HostBar::Open);
  CHECK_EQ(b.portText, "Port open");
  CHECK(b.portTip.find("join at 203.0.113.7:2049") != std::string::npos);
  CHECK(b.addressTip.find("192.168.1.20:2049") != std::string::npos);
  // the router's address wins over the service's
  h.lookedUpAddress = "198.51.100.4:2049";
  h.lookupPending = false;
  CHECK_EQ(uitext::hostBar(h).address, "203.0.113.7:2049");

  // the mapper's private-address line: forwarded to a router that isn't on the internet
  h.publicAddress.clear();
  h.portmapNote = "NAT-PMP: router forwards TCP port 2049 to this machine, but the router's own internet address is 100.64.0.9, which is not public.";
  b = uitext::hostBar(h);
  CHECK_EQ(b.addressKey, "WAN"); // the service's address: the port may be forwarded by hand
  CHECK_EQ(b.address, "198.51.100.4:2049");
  CHECK(b.port == HostBar::Forward);
  CHECK_EQ(b.portText, "Forward port 2049");
  CHECK(b.portTip.find("(TCP) to this computer (192.168.1.20)") != std::string::npos);
  CHECK(b.portTip.find("join at 198.51.100.4:2049") != std::string::npos);
  CHECK(b.portTip.find("(The router said: NAT-PMP: router forwards") != std::string::npos);
  CHECK(b.addressTip.find("once port 2049 is forwarded") != std::string::npos);

  // the lookup's result alone (no router, or JAMSA_PORTMAP=off)
  h.portmapNote.clear();
  b = uitext::hostBar(h);
  CHECK_EQ(b.address, "198.51.100.4:2049");
  CHECK(b.port == HostBar::Forward);
  CHECK(b.portTip.find("The router said") == std::string::npos);

  // the mapper lost the forward: asking again, the service's address stays
  h.portmapPending = true;
  b = uitext::hostBar(h);
  CHECK_EQ(b.address, "198.51.100.4:2049");
  CHECK(b.port == HostBar::Asking);
  h.portmapPending = false;

  // no router, the service didn't answer: the LAN address, forward by hand
  h.lookedUpAddress.clear();
  b = uitext::hostBar(h);
  CHECK_EQ(b.addressKey, "LAN");
  CHECK_EQ(b.address, "192.168.1.20:2049");
  CHECK_EQ(b.placeholder, "not known");
  CHECK(b.port == HostBar::Forward);
  CHECK(b.portTip.find("join at") == std::string::npos);

  // someone joined from the internet: the port is open whatever the router said
  h.joinedFromInternet = true;
  b = uitext::hostBar(h);
  CHECK(b.port == HostBar::Open);
  CHECK(b.portTip.find("A player joined from the internet, so port 2049 is open") == 0);
  h.lookedUpAddress = "198.51.100.4:2049";
  b = uitext::hostBar(h);
  CHECK(b.portTip.find("players join at 198.51.100.4:2049") != std::string::npos);
  CHECK(b.addressTip.find("once port") == std::string::npos);
}

int main()
{
  test_state();
  test_names();
  test_commands();
  test_portmap_messages();
  test_public_address_lookup();
  test_host_bar();
  test_bar_math();
  test_bar_sync();
  test_tempo_follower();
  test_host_note();
  test_click();
  test_uitext();

  // the same in a locale that writes 95,5
  const char *loc = nullptr;
  for (const char *name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252" })
    if ((loc = setlocale(LC_ALL, name))) break;
  if (loc)
  {
    printf("again with the locale %s\n", loc);
    test_state();
    test_uitext();
    setlocale(LC_ALL, "C");
  }
  else printf("no comma locale here; skipped the locale checks\n");

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
