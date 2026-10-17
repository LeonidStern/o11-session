/*
    Jamsa - engine.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <lm.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include "njproto/engine.h"
#include "hostserver.h"

namespace jamsa {

using njproto::engine::LocalChannel;
using njproto::engine::Status;

static const size_t MAX_CHAT_LINES = 1000;
static const char *LOCAL_CHANNEL_NAME = "main";
static const double LICENSE_WAIT_SECONDS = 60.0;
static const double PUBLISH_INTERVAL = 0.03;
static const double SYNC_NOTE_DELAY = 3.0; // the host's tempo differs this long before the note shows
static const int MIX_OUTS = 2 + 2 * Engine::NUM_PLAYER_OUTS;

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string trim(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') a++;
  while (b > a && (unsigned char)s[b-1] <= ' ') b--;
  return s.substr(a, b - a);
}

static std::string fourcc_text(unsigned int f)
{
  std::string s;
  for (int i = 0; i < 4; i ++) s += (char)((f >> (8*i)) & 0xff);
  return s;
}

static std::string codec_name(unsigned int mask)
{
  if (mask & njproto::kCapFlac) return "FLAC";
  if (mask & njproto::kCapOpus) return "Opus";
  return "Vorbis";
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const char *STATE_HEADER = "NINJAM-VST3-STATE 1"; // the format of the plug-in before this one

static std::string escape(const std::string &s)
{
  std::string o;
  for (char c : s)
  {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else o += c;
  }
  return o;
}

static std::string unescape(const std::string &s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); i ++)
  {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    const char c = s[++i];
    o += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
  }
  return o;
}

// numbers are stored as millionths, so the state reads the same in every locale
static std::string num(float v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", (long)lround(v * 1000000.0));
  return buf;
}

static bool read_num(const std::string &s, float *out)
{
  if (s.empty()) return false;
  char *end = nullptr;
  const long v = strtol(s.c_str(), &end, 10);
  if (!end || *end) return false;
  *out = (float)(v / 1000000.0);
  return true;
}

static float clamp_volume(float v) { return v != v ? 1.0f : std::min(std::max(v, 0.0f), 4.0f); }
static float clamp_pan(float v) { return v != v ? 0.0f : std::min(std::max(v, -1.0f), 1.0f); }

constexpr int Settings::OPUS_KBPS[];

int Settings::nearestOpusKbps(int kbps)
{
  int best = DEFAULT_OPUS_KBPS;
  for (int k : OPUS_KBPS)
    if (abs(k - kbps) < abs(best - kbps)) best = k;
  return best;
}

std::string Settings::serialize() const
{
  std::string s = std::string(STATE_HEADER) + "\n";
  s += "user=" + escape(userName) + "\n";
  s += "server=" + escape(server) + "\n";
  s += "input_volume=" + num(inputVolume) + "\n";
  s += "metronome_volume=" + num(metronomeVolume) + "\n";
  s += std::string("metronome_mute=") + (metronomeMute ? "1" : "0") + "\n";
  s += std::string("monitor=") + (monitor ? "1" : "0") + "\n";
  s += std::string("transmit=") + (transmit ? "1" : "0") + "\n";
  s += "opus_kbps=" + std::to_string(opusKbps) + "\n";
  s += std::string("follow_host_tempo=") + (followHostTempo ? "1" : "0") + "\n";
  if (editorWidth > 0 && editorHeight > 0) s += "editor_window=" + std::to_string(editorWidth) + "x" + std::to_string(editorHeight) + "\n";
  for (auto &r : remote)
    s += "remote=" + escape(r.first) + "\t" + num(r.second.volume) + "\t" + num(r.second.pan) + "\t" + (r.second.mute ? "1" : "0") + "\n";
  return s;
}

bool Settings::deserialize(const std::string &data)
{
  if (data.compare(0, strlen(STATE_HEADER), STATE_HEADER) != 0) return false;
  Settings s;
  size_t pos = 0;
  while (pos < data.size())
  {
    size_t eol = data.find('\n', pos);
    if (eol == std::string::npos) eol = data.size();
    const std::string line = data.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    float v;
    if (key == "user") s.userName = unescape(value);
    else if (key == "server") s.server = unescape(value);
    else if (key == "input_volume" && read_num(value, &v)) s.inputVolume = clamp_volume(v);
    else if (key == "metronome_volume" && read_num(value, &v)) s.metronomeVolume = clamp_volume(v);
    else if (key == "metronome_mute") s.metronomeMute = value == "1";
    else if (key == "monitor") s.monitor = value == "1";
    else if (key == "transmit") s.transmit = value == "1";
    else if (key == "follow_host_tempo") s.followHostTempo = value == "1";
    else if (key == "opus_kbps")
    {
      char *end = nullptr;
      const long k = strtol(value.c_str(), &end, 10);
      if (end && !*end && !value.empty()) s.opusKbps = nearestOpusKbps((int)std::min(std::max(k, 0L), 100000L));
    }
    // "editor_size" was the size of the editor before its 720 x 330 layout: not used
    else if (key == "editor_window")
    {
      int w = 0, h = 0;
      if (sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w < 10000 && h < 10000)
      {
        s.editorWidth = w;
        s.editorHeight = h;
      }
    }
    else if (key == "remote")
    {
      // name \t volume \t pan \t mute (the name is escaped, so it holds no tab)
      std::vector<std::string> f;
      size_t a = 0;
      for (;;)
      {
        const size_t t = value.find('\t', a);
        f.push_back(value.substr(a, t == std::string::npos ? std::string::npos : t - a));
        if (t == std::string::npos) break;
        a = t + 1;
      }
      if (f.size() < 4) continue;
      Remote r;
      float vol, pan;
      if (!read_num(f[1], &vol) || !read_num(f[2], &pan)) continue;
      r.volume = clamp_volume(vol);
      r.pan = clamp_pan(pan);
      r.mute = f[3] == "1";
      if (s.remote.size() < 500) s.remote[unescape(f[0])] = r;
    }
  }
  *this = s;
  return true;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

std::string Engine::displayName(const std::string &user)
{
  const size_t at = user.find('@');
  if (at == std::string::npos) return user;
  // A server that lets one name in more than once ("AnonymousUsers multi") calls the second
  // "name@1.2.3.4.2": keep the ".2", so the two can be told apart.
  const size_t colon = user.rfind(':'), from = colon != std::string::npos && colon > at ? colon + 1 : at + 1;
  const size_t dot = user.rfind('.');
  if (std::count(user.begin() + (long)from, user.end(), '.') == 4 && dot + 1 < user.size() && dot + 4 > user.size() &&
      std::all_of(user.begin() + (long)dot + 1, user.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return user.substr(0, at) + user.substr(dot);
  return user.substr(0, at);
}

std::string Engine::firstName(const std::string &fullName)
{
  std::string n = trim(fullName);
  n = n.substr(0, n.find_first_of(" \t,@")); // a full name's first word; a login that is an email address, its name
  if (n.size() > 32) n.resize(32);
  return n;
}

#if defined(_WIN32)
static std::string utf8(const wchar_t *w)
{
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}
#endif

std::string Engine::defaultUserName()
{
  std::string full, login;
#if defined(_WIN32)
  // a domain account's display name, else a local or Microsoft account's full name
  wchar_t buf[256];
  ULONG n = 256;
  if (GetUserNameExW(NameDisplay, buf, &n)) full = utf8(buf);
  wchar_t name[257];
  DWORD ln = 257;
  if (GetUserNameW(name, &ln))
  {
    login = utf8(name);
    USER_INFO_10 *info = nullptr;
    if (trim(full).empty() && NetUserGetInfo(nullptr, name, 10, (LPBYTE *)&info) == NERR_Success && info)
    {
      if (info->usri10_full_name) full = utf8(info->usri10_full_name);
      NetApiBufferFree(info);
    }
  }
#else
  // the account's full name is the first field of its GECOS ("Leo Stern,,,")
  struct passwd pw, *found = nullptr;
  char buf[4096];
  if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &found) == 0 && found)
  {
    if (pw.pw_gecos) full = std::string(pw.pw_gecos).substr(0, std::string(pw.pw_gecos).find(','));
    if (pw.pw_name) login = pw.pw_name;
  }
#endif
  const std::string first = firstName(full);
  return first.empty() ? firstName(login) : first;
}

std::string Engine::remoteKey(const std::string &user, int channel)
{
  std::string n = displayName(user);
  for (auto &c : n) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return n + "/" + std::to_string(channel);
}

Engine::Engine()
{
  m_settings.userName = defaultUserName();
  m_client.reset(new njproto::engine::Engine);
  m_client->onLicence = [this](const std::string &text) {
    // from run() on the network thread: the UI answers (answerLicense), or the wait runs out
    m_licenseAnswer = 0;
    m_licenseDeadline = now_seconds() + LICENSE_WAIT_SECONDS;
    m_licenseWaiting = true;
    {
      std::lock_guard<std::mutex> lock(m_snapMutex);
      m_snap.licensePending = true;
      m_snap.licenseText = text;
    }
    addChat(ChatLine::System, "", "The server asks you to accept its license.");
    return njproto::engine::LicenceAnswer::Defer;
  };
  m_client->onChat = [this](const std::array<std::string, 5> &fields) { onChat(fields); };

  // our input, in stereo (source channels 0 and 1); the plug-in monitors it itself, so the
  // engine's monitoring of it is muted
  LocalChannel lc;
  lc.name = LOCAL_CHANNEL_NAME;
  lc.source = 0;
  lc.stereo = true;
  lc.bitrate = m_settings.opusKbps;
  lc.broadcast = m_settings.transmit;
  lc.mute = true;
  m_client->setLocalChannel(0, lc);
  // each player (all their channels) on an output pair of their own, after the main pair: the
  // least used one when they join
  m_client->autoOutputPairs(2, MIX_OUTS);
  // the plug-in plays its own metronome (click.h)
  m_client->setMetronome(0.0f, 0.0f, true, 0);
  applyLocalSettings();

  setupProcessing(48000, 1024);
}

Engine::~Engine()
{
  stop();
}

void Engine::start()
{
  if (m_running) return;
  m_quit = false;
  m_running = true;
  m_thread = std::thread([this]() { threadProc(); });
}

void Engine::stop()
{
  if (!m_running) return;
  m_quit = true;
  m_licenseAnswer = -1;
  m_cmdCv.notify_all();
  if (m_thread.joinable()) m_thread.join();
  m_running = false;

  // what the network thread would have done on its way out
  m_client->disconnect();
  m_licenseWaiting = false;
  if (m_hosting) HostServer::instance().stop(this);
  m_hosting = false;
}

// ---------------------------------------------------------------------------
// audio thread
// ---------------------------------------------------------------------------

void Engine::setupProcessing(double sampleRate, int maxBlockSize)
{
  m_sampleRate = std::max(1, (int)lround(sampleRate));
  m_maxBlock = std::max(32, std::min(maxBlockSize, 1 << 16));
  m_bufL.assign((size_t)m_maxBlock, 0.0f);
  m_bufR.assign((size_t)m_maxBlock, 0.0f);
  m_spare.assign((size_t)m_maxBlock, 0.0f);
  m_monL.assign((size_t)m_maxBlock, 0.0f);
  m_monR.assign((size_t)m_maxBlock, 0.0f);
  m_mix.assign((size_t)MIX_OUTS, std::vector<float>((size_t)m_maxBlock, 0.0f));
  m_barSync.reset();
  m_click.setSampleRate(m_sampleRate);
}

void Engine::process(float **in, int numIn, float **out, int numOut, int frames, const HostTiming *timing,
                     const PlayerOuts *playerOuts)
{
  if (frames <= 0 || numOut < 1 || !out || !out[0]) return;
  m_processCalls.fetch_add(1, std::memory_order_relaxed);
  const float gain = m_inputGain.load(std::memory_order_relaxed);
  const int srate = m_sampleRate.load(std::memory_order_relaxed);
  float *inL = numIn > 0 && in ? in[0] : nullptr;
  float *inR = numIn > 1 && in ? in[1] : inL;
  m_hostTempo.store(timing && timing->position.tempo > 0 ? timing->position.tempo : 0.0, std::memory_order_relaxed);

  // a stopped host: we send silence (and the network thread stops sending at the next interval)
  const bool stopped = timing && timing->transport && !timing->offline && !timing->position.playing;
  m_hostStopped.store(stopped, std::memory_order_relaxed);
  const bool monitor = m_monitor.load(std::memory_order_relaxed) && !m_client->anySolo();

  // the host's bars: the interval playing now may end a little early or late, so the next
  // ones start on them
  if (m_barReset.exchange(false, std::memory_order_acquire)) m_barSync.reset();
  if (!m_follow.load(std::memory_order_relaxed) || !timing) m_barSync.reset();
  else if (!timing->offline)
  {
    BarSync::Interval iv;
    int length = 0;
    iv.running = m_client->isIntervalRunning();
    m_client->position(&iv.pos, &length);
    iv.len = m_client->usualIntervalLength();
    iv.bpi = m_client->bpi();
    iv.bpm = m_client->bpm();
    iv.count = m_client->loopCount();
    const int end = m_barSync.process(timing->position, frames, srate, iv);
    if (end > 0) m_client->setIntervalEnd(end);
  }
  // where the next interval starts in the host, while they are (or get) in line
  double nextPpq = -1;
  if (timing && (m_barSync.state() == BarSync::Aligned || m_barSync.state() == BarSync::Lining) && srate > 0)
  {
    const HostPosition &p = timing->position;
    int pos = 0, length = 0;
    m_client->position(&pos, &length);
    nextPpq = p.ppq + (length - pos) / (double)srate * p.tempo / 60.0;
    if (p.cycleActive && p.cycleEnd > p.cycleStart && p.ppq < p.cycleEnd && nextPpq >= p.cycleEnd)
      nextPpq = p.cycleStart + fmod(nextPpq - p.cycleStart, p.cycleEnd - p.cycleStart);
  }
  m_nextIntervalPpq.store(nextPpq, std::memory_order_relaxed);
  if (timing) m_timeSig.store(timing->position.timeSigNum > 0 && timing->position.timeSigDen > 0
                              ? timing->position.timeSigNum * 256 + timing->position.timeSigDen : 0, std::memory_order_relaxed);
  m_barState.store(m_barSync.state(), std::memory_order_relaxed);
  m_barErrorMs.store(m_barSync.errorMs(), std::memory_order_relaxed);
  m_barFixes.store(m_barSync.corrections(), std::memory_order_relaxed);

  // the engine clears its outputs before it reads its inputs, so it gets a copy of the input
  const float click = m_clickVolume.load(std::memory_order_relaxed);
  for (int done = 0; done < frames; )
  {
    int n = std::min(frames - done, m_maxBlock);
    // a piece doesn't go over the end of the interval playing, so the click knows which
    // interval (and tempo) each sample is in
    int pos = 0, length = 0;
    m_client->position(&pos, &length);
    if (m_client->isIntervalRunning() && pos < length) n = std::min(n, length - pos);
    for (int i = 0; i < n; i ++)
    {
      m_monL[(size_t)i] = inL ? inL[done + i] * gain : 0.0f;
      m_monR[(size_t)i] = inR ? inR[done + i] * gain : 0.0f;
      m_bufL[(size_t)i] = stopped ? 0.0f : m_monL[(size_t)i];
      m_bufR[(size_t)i] = stopped ? 0.0f : m_monR[(size_t)i];
    }
    const float *ins[2] = { m_bufL.data(), m_bufR.data() };
    float *mix[MIX_OUTS];
    for (int c = 0; c < MIX_OUTS; c ++) mix[c] = m_mix[(size_t)c].data();
    m_client->process(ins, 2, mix, MIX_OUTS, n, srate);

    // the main output: everyone, and your monitoring (the engine's own is muted, so it goes on
    // while nothing is sent); each player output: that player alone
    float *outs[2] = { out[0] + done, numOut > 1 && out[1] ? out[1] + done : m_spare.data() };
    for (int i = 0; i < n; i ++)
    {
      float l = mix[0][i], r = mix[1][i];
      for (int p = 0; p < NUM_PLAYER_OUTS; p ++)
      {
        l += mix[2 + 2 * p][i];
        r += mix[3 + 2 * p][i];
      }
      if (monitor)
      {
        l += m_monL[(size_t)i];
        r += m_monR[(size_t)i];
      }
      outs[0][i] = l;
      outs[1][i] = r;
    }
    if (playerOuts)
      for (int p = 0; p < NUM_PLAYER_OUTS; p ++)
      {
        float **bus = playerOuts->bus[p];
        if (!bus) continue;
        for (int c = 0; c < playerOuts->channels[p]; c ++)
          if (bus[c]) memcpy(bus[c] + done, mix[2 + 2 * p + (c & 1)], sizeof(float) * (size_t)n);
      }

    // the metronome, on the beats of the interval these samples played in
    if (m_client->isIntervalRunning())
    {
      m_client->position(&pos, &length);
      m_click.render(outs[0], numOut > 1 && out[1] ? outs[1] : nullptr, n, pos - n, m_client->usualIntervalLength(),
                     m_client->bpi(), click);
    }
    else m_click.reset();
    done += n;
  }
}

// ---------------------------------------------------------------------------
// any thread
// ---------------------------------------------------------------------------

void Engine::getSnapshot(Snapshot &out) const
{
  std::lock_guard<std::mutex> lock(m_snapMutex);
  out = m_snap;
}

unsigned long long Engine::getChat(std::vector<ChatLine> &out, size_t maxLines) const
{
  std::lock_guard<std::mutex> lock(m_chatMutex);
  const size_t n = std::min(maxLines, m_chat.size());
  out.assign(m_chat.end() - (long)n, m_chat.end());
  return m_chatRevision;
}

Settings Engine::getSettings() const
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  return m_settings;
}

void Engine::setSettings(const Settings &s)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    const std::string name = m_settings.userName;
    m_settings = s;
    if (m_settings.userName.empty()) m_settings.userName = name; // a set saved before you typed one keeps the default
    m_settingsRevision ++;
  }
  m_inputGain = s.inputVolume;
  if (m_follow.exchange(s.followHostTempo) != s.followHostTempo) m_barReset = true;
  post([this]() {
    applyLocalSettings();
    m_appliedRemote.clear(); // channels already here get the loaded settings too
  });
}

void Engine::post(std::function<void()> fn)
{
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_commands.push_back(std::move(fn));
  }
  m_cmdCv.notify_all();
}

void Engine::addChat(ChatLine::Kind kind, const std::string &who, const std::string &text)
{
  std::lock_guard<std::mutex> lock(m_chatMutex);
  ChatLine l;
  l.kind = kind;
  l.who = who;
  l.text = text;
  m_chat.push_back(l);
  while (m_chat.size() > MAX_CHAT_LINES) m_chat.pop_front();
  m_chatRevision ++;
}

void Engine::connect(const std::string &server, const std::string &userName)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.server = trim(server);
    m_settings.userName = trim(userName);
  }
  const std::string s = trim(server), u = trim(userName);
  post([this, s, u]() { doConnect(s, u); });
}

void Engine::disconnect()
{
  if (m_licenseWaiting) m_licenseAnswer = -1; // the network thread is waiting for an answer
  post([this]() { doDisconnect(nullptr); });
}

void Engine::vote(const std::string &what, int value)
{
  sendChat("!vote " + what + " " + std::to_string(value));
}

void Engine::sendChat(const std::string &text)
{
  const std::string t = trim(text);
  if (t.empty()) return;
  post([this, t]() {
    if (m_state != ConnState::Connected)
    {
      addChat(ChatLine::Error, "", "Not connected, so nobody can read that.");
      return;
    }
    auto starts = [&t](const char *p) {
      const size_t n = strlen(p);
      if (t.size() < n) return false;
      for (size_t i = 0; i < n; i ++)
        if (tolower((unsigned char)t[i]) != p[i]) return false;
      return true;
    };
    if (t[0] != '/' || starts("/me "))
    {
      m_client->say(t);
    }
    else if (starts("/topic ") || starts("/kick ") || starts("/bpm ") || starts("/bpi "))
    {
      m_client->admin(t.substr(1));
    }
    else if (starts("/admin "))
    {
      m_client->admin(trim(t.substr(7)));
    }
    else if (starts("/msg "))
    {
      const std::string rest = trim(t.substr(5));
      const size_t sp = rest.find(' ');
      const std::string who = rest.substr(0, sp), what = sp == std::string::npos ? "" : trim(rest.substr(sp));
      if (who.empty() || what.empty()) addChat(ChatLine::Error, "", "/msg needs a name and a message.");
      else
      {
        m_client->privateMessage(who, what);
        addChat(ChatLine::Private, "-> " + who, what);
      }
    }
    else addChat(ChatLine::Error, "", "Unknown command. Commands: /me, /msg name text, /topic, /bpm, /bpi, /kick, /admin; !vote bpm 120, !vote bpi 16, !codec opus|flac and !bitrate 128 go to the server as typed.");
  });
}

void Engine::answerLicense(bool accept)
{
  m_licenseAnswer = accept ? 1 : -1;
  m_cmdCv.notify_all();
}

void Engine::setHosting(bool on)
{
  post([this, on]() { doSetHosting(on); });
}

void Engine::setUserName(const std::string &name)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.userName = trim(name);
}

void Engine::setServerText(const std::string &server)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.server = trim(server);
}

void Engine::setInputVolume(float v)
{
  v = clamp_volume(v);
  m_inputGain = v;
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.inputVolume = v;
}

void Engine::setTransmit(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.transmit = on;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setMonitor(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.monitor = on;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setMetronome(float volume, bool mute)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.metronomeVolume = clamp_volume(volume);
    m_settings.metronomeMute = mute;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setOpusKbps(int kbps)
{
  const int k = Settings::nearestOpusKbps(kbps);
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.opusKbps = k;
  }
  post([this, k]() {
    // a server that sets the room's bitrate: the room's changes, for everyone
    const int room = m_state == ConnState::Connected ? m_client->roomOpusKbps() : 0;
    if (room > 0 && room != k) m_client->say("!bitrate " + std::to_string(k));
    applyLocalSettings();
  });
}

void Engine::setRoomCodec(const std::string &codec)
{
  if (codec != "opus" && codec != "flac") return;
  post([this, codec]() {
    if (m_state == ConnState::Connected) m_client->say("!codec " + codec);
  });
}

int Engine::sendKbps()
{
  const int room = m_state == ConnState::Connected ? m_client->roomOpusKbps() : 0;
  return room > 0 ? room : getSettings().opusKbps;
}

void Engine::applySendKbps()
{
  // the engine starts a new encoder at the next interval when the bitrate changes
  const int want = sendKbps();
  LocalChannel lc;
  if (m_client->localChannel(0, &lc) && lc.bitrate != want)
  {
    lc.bitrate = want;
    m_client->setLocalChannel(0, lc);
  }
}

void Engine::setEditorSize(int width, int height)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.editorWidth = width;
  m_settings.editorHeight = height;
}

void Engine::setFollowHostTempo(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    if (m_settings.followHostTempo == on) return;
    m_settings.followHostTempo = on;
  }
  m_follow = on;
  m_barReset = true;
  post([this, on]() {
    // a vote for the host's tempo, if the room's differs
    if (on) m_tempoFollower.arm();
  });
}

void Engine::setHostName(const std::string &name)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_hostName = name;
}

void Engine::setRemote(const std::string &user, int channel, RemoteField field, float value)
{
  const std::string key = remoteKey(user, channel);
  if (field != RemoteSolo)
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    Settings::Remote &r = m_settings.remote[key];
    if (field == RemoteVolume) r.volume = clamp_volume(value);
    else if (field == RemotePan) r.pan = clamp_pan(value);
    else r.mute = value > 0.5f;
  }
  {
    // the UI sees its change at once, not a frame later
    std::lock_guard<std::mutex> lock(m_snapMutex);
    for (auto &u : m_snap.users)
      if (u.name == user)
        for (auto &c : u.channels)
          if (c.index == channel)
          {
            if (field == RemoteVolume) c.volume = value;
            else if (field == RemotePan) c.pan = value;
            else if (field == RemoteMute) c.mute = value > 0.5f;
            else c.solo = value > 0.5f;
          }
  }
  post([this, user, channel, field, value]() {
    const int u = findUser(user);
    if (u < 0) return;
    switch (field)
    {
      case RemoteVolume: m_client->setChannelVolume(u, channel, clamp_volume(value)); break;
      case RemotePan: m_client->setChannelPan(u, channel, clamp_pan(value)); break;
      case RemoteMute: m_client->setChannelMute(u, channel, value > 0.5f); break;
      case RemoteSolo: m_client->setChannelSolo(u, channel, value > 0.5f); break;
    }
  });
}

// ---------------------------------------------------------------------------
// network thread
// ---------------------------------------------------------------------------

void Engine::runCommands()
{
  for (;;)
  {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(m_cmdMutex);
      if (m_commands.empty()) return;
      fn = std::move(m_commands.front());
      m_commands.pop_front();
    }
    fn();
  }
}

void Engine::threadProc()
{
  while (!m_quit)
  {
    runCommands();

    for (int i = 0; i < 100 && !m_quit; i ++)
      if (m_client->run()) break; // true: nothing more to do for now

    updateLicense();
    updateConnection();
    applySendKbps(); // the room's bitrate may have changed
    applyBroadcast(); // the host may have started or stopped
    followHostTempo();

    const double t = now_seconds();
    if (t - m_lastPublish >= PUBLISH_INTERVAL)
    {
      m_lastPublish = t;
      publish();
    }

    std::unique_lock<std::mutex> lock(m_cmdMutex);
    if (m_commands.empty() && !m_quit) m_cmdCv.wait_for(lock, std::chrono::milliseconds(10));
  }
}

void Engine::applyLocalSettings()
{
  const Settings s = getSettings();
  m_inputGain = s.inputVolume;
  m_clickVolume = s.metronomeMute ? 0.0f : s.metronomeVolume;
  // the engine monitors the input itself (process()), so it goes on while the host is stopped
  m_monitor = s.monitor;

  applySendKbps();
  applyBroadcast();
}

void Engine::applyBroadcast()
{
  const bool want = getSettings().transmit && !m_hostStopped.load(std::memory_order_relaxed);
  LocalChannel lc;
  // from the next interval (the channel stays, so the others keep their settings for it)
  if (m_client->localChannel(0, &lc) && lc.broadcast != want)
  {
    lc.broadcast = want;
    m_client->setLocalChannel(0, lc);
  }
}

void Engine::doConnect(const std::string &serverIn, const std::string &userIn)
{
  std::string server = trim(serverIn);
  if (server.empty())
  {
    addChat(ChatLine::Error, "", "Type a server as host:port, or host one with \"Host a server\".");
    return;
  }
  if (server.find(':') == std::string::npos) server += ":2049";

  std::string user = trim(userIn);
  for (auto &c : user) if (c == '@' || c == ':' || (unsigned char)c < ' ') c = '_';
  if (user.empty()) user = "guest";
  if (user.size() > 32) user.resize(32);

  if (m_state == ConnState::Connecting || m_state == ConnState::Connected) m_client->disconnect();
  m_licenseWaiting = false;

  m_server = server;
  m_appliedRemote.clear();
  m_warnedCantSend = false;
  m_state = ConnState::Connecting;
  m_connectTime = now_seconds();
  m_statusText = "Connecting to " + server + "...";
  addChat(ChatLine::System, "", m_statusText);

  // anonymous login, which most servers allow
  m_client->connect(server, "anonymous:" + user, "");
}

void Engine::doDisconnect(const char *why)
{
  if (m_state == ConnState::Connecting || m_state == ConnState::Connected)
  {
    m_client->disconnect();
    addChat(ChatLine::System, "", why ? why : "Disconnected.");
  }
  if (m_licenseWaiting)
  {
    m_licenseWaiting = false;
    std::lock_guard<std::mutex> lock(m_snapMutex);
    m_snap.licensePending = false;
    m_snap.licenseText.clear();
  }
  m_state = ConnState::Disconnected;
  m_statusText = "Not connected";
}

// the licence the server asks us to accept: the UI's answer, or none within the wait
void Engine::updateLicense()
{
  if (!m_licenseWaiting) return;
  const int answer = m_licenseAnswer.load();
  if (answer == 0 && now_seconds() < m_licenseDeadline && !m_quit) return;
  const bool accepted = answer == 1;
  m_licenseWaiting = false;
  {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    m_snap.licensePending = false;
    m_snap.licenseText.clear();
  }
  if (!accepted) addChat(ChatLine::System, "", "You did not accept the license, so the server will not let you in.");
  m_client->answerLicence(accepted);
}

void Engine::updateConnection()
{
  if (m_state != ConnState::Connecting && m_state != ConnState::Connected) return;
  const Status st = m_client->status();
  if (st == Status::Ok)
  {
    if (m_state == ConnState::Connecting && m_client->serverBpi() > 0)
    {
      m_state = ConnState::Connected;
      m_connectedTime = now_seconds();
      m_statusText = "Connected to " + m_server + " as " + displayName(m_client->username());
      addChat(ChatLine::System, "", m_statusText);
      // the room gets the host's tempo, if they differ
      m_tempoFollower.arm();
    }
    return;
  }
  if (st == Status::Preconnect) return;

  const std::string err = m_client->errorText();
  std::string text;
  if (st == Status::CantConnect) text = "Could not connect to " + m_server;
  else if (st == Status::InvalidAuth) text = m_server + " refused the login";
  else text = (m_state == ConnState::Connected ? "Lost the connection to " : "Could not connect to ") + m_server;
  if (!err.empty()) text += ": " + err;
  m_client->disconnect();
  m_licenseWaiting = false;
  m_state = ConnState::Failed;
  m_statusText = text;
  addChat(ChatLine::Error, "", text);
}

// The plug-in sends Opus or FLAC only. A server too old to know them (it sends no codec
// mask), or a room switched to Vorbis, can't hear it, and its players sound silent here.
void Engine::checkCanSend(bool canSend)
{
  // the codec mask comes just after the login; give it a moment
  if (now_seconds() - m_connectedTime < 1.5) return;
  if (!canSend && !m_warnedCantSend)
  {
    m_warnedCantSend = true;
    addChat(ChatLine::Error, "", "This server doesn't support Opus (its room plays Vorbis), so you can't send here, and the other "
                                 "players sound silent. If the server knows Opus, type !codec opus to switch the room.");
  }
  else if (canSend && m_warnedCantSend)
  {
    m_warnedCantSend = false;
    addChat(ChatLine::System, "", "The room plays " + codec_name(m_client->codecMask()) + " now, so you're sending again.");
  }
}

// votes the room to the host's tempo when it changes (see TempoFollower)
void Engine::followHostTempo()
{
  if (!getSettings().followHostTempo) return;
  const double t = now_seconds();
  // the host's tempo, while it runs the audio
  const double tempo = t - m_lastAudio < 1.0 ? m_hostTempo.load(std::memory_order_relaxed) : 0.0;
  const int room = m_state == ConnState::Connected ? m_client->serverBpm() : 0;
  const int bpm = m_tempoFollower.update(t, tempo, room);
  if (bpm <= 0) return;
  char buf[256];
  snprintf(buf, sizeof(buf), "!vote bpm %d", bpm);
  m_client->say(buf);
  std::string host;
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    host = shortHostName(m_hostName);
  }
  snprintf(buf, sizeof(buf), "%s is at %s BPM, so you vote the room to %d (!vote bpm %d).", host.c_str(), bpmText(tempo).c_str(), bpm, bpm);
  addChat(ChatLine::System, "", buf);
}

void Engine::doSetHosting(bool on)
{
  if (on == m_hosting) return;
  m_hostBusy = true;
  publish();

  if (on)
  {
    std::string err;
    if (HostServer::instance().start(this, getSettings().opusKbps, &err))
    {
      m_hosting = true;
      m_hostLogSerial = 0;
      m_hostPort = HostServer::instance().state().port;
      addChat(ChatLine::System, "", "Hosting a server on port " + std::to_string(m_hostPort) + ".");
      // 127.0.0.1, not localhost: the server listens on IPv4, and localhost may resolve to ::1 first
      doConnect("127.0.0.1:" + std::to_string(m_hostPort), getSettings().userName);
    }
    else addChat(ChatLine::Error, "", err);
  }
  else
  {
    if (m_server == "127.0.0.1:" + std::to_string(m_hostPort)) doDisconnect("Disconnected from your server.");
    addChat(ChatLine::System, "", "Stopping the server...");
    publish();
    HostServer::instance().stop(this);
    m_hosting = false;
    m_hostPort = 0;
    addChat(ChatLine::System, "", "The server is stopped.");
  }
  m_hostBusy = false;
}

int Engine::findUser(const std::string &name) const
{
  const int n = m_client->numUsers();
  for (int u = 0; u < n; u ++)
    if (m_client->userName(u) == name) return u;
  return -1;
}

void Engine::updateUsers(Snapshot &snap)
{
  m_client->hasUserInfoChanged();
  Settings settings;
  bool haveSettings = false;

  std::string signature;
  const int n = m_client->numUsers();
  for (int u = 0; u < n; u ++)
  {
    RemoteUserView uv;
    uv.name = m_client->userName(u);
    uv.displayName = displayName(uv.name);
    signature += uv.name + "\n";
    for (int i = 0; ; i ++)
    {
      const int ch = m_client->channelAt(u, i);
      if (ch < 0) break;
      RemoteChannelView cv;
      cv.index = ch;
      const std::string key = remoteKey(uv.name, ch);
      if (!m_appliedRemote.count(key))
      {
        // a channel we haven't seen this session gets what was saved for it, else 0 dB (the
        // engine's own default is -12 dB)
        m_appliedRemote.insert(key);
        if (!haveSettings)
        {
          settings = getSettings();
          haveSettings = true;
        }
        auto it = settings.remote.find(key);
        if (it != settings.remote.end())
        {
          m_client->setChannelVolume(u, ch, it->second.volume);
          m_client->setChannelPan(u, ch, it->second.pan);
          m_client->setChannelMute(u, ch, it->second.mute);
        }
        else m_client->setChannelVolume(u, ch, 1.0f);
      }
      cv.name = m_client->channelName(u, ch);
      cv.volume = m_client->channelVolume(u, ch);
      cv.pan = m_client->channelPan(u, ch);
      cv.mute = m_client->channelMute(u, ch);
      cv.solo = m_client->channelSolo(u, ch);
      const int outch = m_client->channelOutChannel(u, ch);
      cv.playerOut = outch >= 2 && !(outch & 1024) ? outch / 2 : 0;
      if (cv.playerOut > 0)
      {
        int &said = m_playerOutSaid[uv.name];
        if (said != cv.playerOut)
        {
          said = cv.playerOut;
          addChat(ChatLine::System, "", uv.displayName + " plays alone on the output Player " + std::to_string(cv.playerOut));
        }
      }
      cv.peak[0] = m_client->channelPeak(u, ch, 0);
      cv.peak[1] = m_client->channelPeak(u, ch, 1);
      signature += std::to_string(ch) + ":" + cv.name + "\n";
      uv.channels.push_back(cv);
    }
    snap.users.push_back(uv);
  }

  {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    // compare with what was published, by structure
    std::string old;
    for (auto &u : m_snap.users)
    {
      old += u.name + "\n";
      for (auto &c : u.channels) old += std::to_string(c.index) + ":" + c.name + "\n";
    }
    if (old != signature) m_usersRevision ++;
  }
  snap.usersRevision = m_usersRevision;
}

void Engine::pollHosting(Snapshot &snap)
{
  HostServer::State st = HostServer::instance().state();
  if (m_hosting && st.owner == this)
  {
    snap.hosting = true;
    snap.hostPort = st.port;
    snap.hostPublicAddress = st.publicAddress;
    snap.hostLocalAddress = st.localAddress;
    snap.hostNote = st.portmapNote;
    snap.hostPortmapPending = st.portmapPending;
    snap.hostLookedUpAddress = st.lookedUpAddress;
    snap.hostLookupPending = st.lookupPending;
    snap.hostJoinedFromInternet = st.joinedFromInternet;
    snap.hostUsers = st.users;
    for (const std::string &m : HostServer::instance().messagesSince(&m_hostLogSerial))
      addChat(ChatLine::Server, "", m);
  }
  else if (st.owner && st.owner != this) snap.hostedElsewherePort = st.port;
  snap.hostBusy = m_hostBusy;
}

void Engine::publish()
{
  Snapshot snap;
  snap.state = m_state;
  snap.status = m_statusText.empty() ? "Not connected" : m_statusText;
  snap.server = m_server;
  snap.opusKbps = getSettings().opusKbps;
  snap.sendKbps = sendKbps();
  if (m_state == ConnState::Connected || m_state == ConnState::Connecting)
  {
    snap.userName = displayName(m_client->username());
    // what the server set: bpi() changes only at the next interval, and only while the host runs the audio
    snap.bpm = (float)m_client->serverBpm();
    snap.bpi = m_client->serverBpi();
    m_client->position(&snap.intervalPos, &snap.intervalLength);
    snap.intervalBeats = m_client->bpi();
    if (m_state == ConnState::Connected)
    {
      snap.roomCodec = codec_name(m_client->codecMask());
      snap.roomOpusKbps = m_client->roomOpusKbps();
      const unsigned int upload = m_client->uploadCodec();
      if (upload) snap.uploadCodec = fourcc_text(upload);
      snap.canSend = upload != 0;
      checkCanSend(upload != 0);
    }
    updateUsers(snap);
  }
  else
  {
    m_playerOutSaid.clear();
    std::lock_guard<std::mutex> lock(m_snapMutex);
    if (!m_snap.users.empty()) m_usersRevision ++;
    snap.usersRevision = m_usersRevision;
  }
  const unsigned int calls = m_processCalls.load(std::memory_order_relaxed);
  const double t = now_seconds();
  if (calls != m_seenProcessCalls)
  {
    m_seenProcessCalls = calls;
    m_lastAudio = t;
  }
  snap.audioRunning = t - m_lastAudio < 1.0;
  snap.localPeak[0] = m_client->localPeak(0, 0);
  snap.localPeak[1] = m_client->localPeak(0, 1);
  snap.sendPaused = snap.audioRunning && m_hostStopped.load(std::memory_order_relaxed) && getSettings().transmit;

  // following the host, and its tempo against the room's
  std::string hostName;
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    snap.followTempo = m_settings.followHostTempo;
    hostName = m_hostName;
  }
  snap.hostLabel = shortHostName(hostName);
  snap.hostChip = hostChipName(hostName);
  {
    const int ts = m_timeSig.load(std::memory_order_relaxed);
    snap.timeSigNum = ts / 256;
    snap.timeSigDen = ts % 256;
  }
  snap.hostTempo = snap.audioRunning ? m_hostTempo.load(std::memory_order_relaxed) : 0.0;
  snap.barState = snap.followTempo && m_state == ConnState::Connected ? (BarSync::State)m_barState.load(std::memory_order_relaxed) : BarSync::Off;
  // where the next interval starts in the host's bars, while they line up
  snap.nextIntervalPpq = snap.barState == BarSync::Aligned || snap.barState == BarSync::Lining
                         ? m_nextIntervalPpq.load(std::memory_order_relaxed) : -1;
  snap.barErrorMs = m_barErrorMs.load(std::memory_order_relaxed);
  snap.barFixes = m_barFixes.load(std::memory_order_relaxed);
  snap.tempoVotes = m_tempoFollower.votes();

  const int roomBpm = m_state == ConnState::Connected ? m_client->serverBpm() : 0;
  // not while the room's tempo changes to the host's (at the next interval)
  const bool differs = snap.hostTempo > 0 && roomBpm > 0 && fabs(snap.hostTempo - roomBpm) > BarSync::SAME_TEMPO &&
                       fabs(snap.hostTempo - m_client->bpm()) > BarSync::SAME_TEMPO;
  if (!differs) m_noteSince = -1;
  else if (m_noteSince < 0) m_noteSince = t;
  if (differs && t - m_noteSince >= SYNC_NOTE_DELAY)
    snap.syncNote = hostTempoNote(hostName, snap.hostTempo, roomBpm, snap.followTempo);

  pollHosting(snap);

  {
    std::lock_guard<std::mutex> lock(m_chatMutex);
    snap.chatRevision = m_chatRevision;
  }
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    snap.settingsRevision = m_settingsRevision;
  }

  std::lock_guard<std::mutex> lock(m_snapMutex);
  snap.licensePending = m_snap.licensePending;
  snap.licenseText = m_snap.licenseText;
  m_snap = std::move(snap);
}

void Engine::onChat(const std::array<std::string, 5> &fields)
{
  const std::string &cmd = fields[0], &p1 = fields[1], &p2 = fields[2];

  if (cmd == "MSG")
  {
    if (p1.empty()) addChat(ChatLine::Server, "", p2);
    else if (p2.compare(0, 4, "/me ") == 0) addChat(ChatLine::Action, displayName(p1), p2.substr(4));
    else addChat(ChatLine::Message, displayName(p1), p2);
  }
  else if (cmd == "PRIVMSG")
  {
    addChat(ChatLine::Private, displayName(p1), p2);
  }
  else if (cmd == "TOPIC")
  {
    if (p1.empty()) { if (!p2.empty()) addChat(ChatLine::Topic, "", "Topic: " + p2); }
    else addChat(ChatLine::Topic, "", displayName(p1) + " sets the topic to: " + p2);
  }
  else if (cmd == "JOIN" && !p1.empty())
  {
    addChat(ChatLine::System, "", displayName(p1) + " joined");
  }
  else if (cmd == "PART" && !p1.empty())
  {
    addChat(ChatLine::System, "", displayName(p1) + " left");
  }
}

// ---------------------------------------------------------------------------
// text commands
// ---------------------------------------------------------------------------

static const char *state_name(ConnState s)
{
  switch (s)
  {
    case ConnState::Connecting: return "connecting";
    case ConnState::Connected: return "connected";
    case ConnState::Failed: return "failed";
    default: return "disconnected";
  }
}

static const char *bar_state_name(BarSync::State s)
{
  switch (s)
  {
    case BarSync::NoPosition: return "noposition";
    case BarSync::Stopped: return "stopped";
    case BarSync::OtherTempo: return "othertempo";
    case BarSync::Lining: return "lining";
    case BarSync::Aligned: return "aligned";
    default: return "off";
  }
}

std::string Engine::command(const std::string &lineIn)
{
  const std::string line = trim(lineIn);
  const size_t sp = line.find(' ');
  const std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : trim(line.substr(sp));

  if (verb == "connect")
  {
    // connect host:port [name]
    const size_t s2 = arg.find(' ');
    const std::string server = arg.substr(0, s2), name = s2 == std::string::npos ? getSettings().userName : trim(arg.substr(s2));
    connect(server, name);
    return "ok";
  }
  if (verb == "disconnect") { disconnect(); return "ok"; }
  if (verb == "host") { setHosting(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "chat") { sendChat(arg); return "ok"; }
  if (verb == "license") { answerLicense(arg == "accept"); return "ok"; }
  if (verb == "transmit") { setTransmit(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "monitor") { setMonitor(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "name") { setUserName(arg); return "ok"; }
  if (verb == "bitrate")
  {
    const int k = atoi(arg.c_str());
    if (Settings::nearestOpusKbps(k) != k) return "error: the bitrate is one of 64 96 128 160 256 510";
    setOpusKbps(k);
    return "ok";
  }
  if (verb == "codec")
  {
    if (arg != "opus" && arg != "flac") return "error: codec opus or codec flac";
    setRoomCodec(arg);
    return "ok";
  }
  if (verb == "follow") { setFollowHostTempo(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "vote")
  {
    // what the editor's BPM and BPI boxes do
    const size_t s2 = arg.find(' ');
    const std::string what = arg.substr(0, s2);
    const int value = s2 == std::string::npos ? 0 : atoi(arg.c_str() + s2 + 1);
    if ((what != "bpm" && what != "bpi") || value <= 0) return "error: vote bpm N, or vote bpi N";
    vote(what, value);
    return "ok";
  }
  if (verb == "nextinterval")
  {
    Snapshot s;
    getSnapshot(s);
    return s.nextIntervalPpq < 0 ? std::string() : nextIntervalText(s.hostChip, s.nextIntervalPpq, s.timeSigNum, s.timeSigDen);
  }
  if (verb == "syncnote")
  {
    Snapshot s;
    getSnapshot(s);
    return s.syncNote;
  }
  if (verb == "synctip")
  {
    Snapshot s;
    getSnapshot(s);
    return syncTooltip(s.hostLabel);
  }
  if (verb == "metronome")
  {
    const Settings st = getSettings();
    setMetronome(st.metronomeVolume, !(arg == "on" || arg == "1"));
    return "ok";
  }
  if (verb == "status")
  {
    Snapshot s;
    getSnapshot(s);
    char buf[1536];
    int n = snprintf(buf, sizeof(buf), "state=%s server=%s user=%s bpm=%.1f bpi=%d pos=%d len=%d codec=%s upload=%s kbps=%d users=%d hosting=%d port=%d public=%s local=%s license=%d audio=%d",
                     state_name(s.state), s.server.c_str(), s.userName.c_str(), (double)s.bpm, s.bpi, s.intervalPos, s.intervalLength,
                     s.roomCodec.empty() ? "-" : s.roomCodec.c_str(), s.uploadCodec.empty() ? "-" : s.uploadCodec.c_str(),
                     s.sendKbps, (int)s.users.size(), s.hosting ? 1 : 0, s.hostPort,
                     s.hostPublicAddress.empty() ? (s.hostPortmapPending ? "?" : "-") : s.hostPublicAddress.c_str(),
                     s.hostLocalAddress.empty() ? "-" : s.hostLocalAddress.c_str(), s.licensePending ? 1 : 0, s.audioRunning ? 1 : 0);
    if (n > 0 && n < (int)sizeof(buf))
      snprintf(buf + n, sizeof(buf) - (size_t)n, " follow=%d hosttempo=%.3f bars=%s barerr=%.3f barfixes=%d tempovotes=%d ownkbps=%d roomkbps=%d",
               s.followTempo ? 1 : 0, s.hostTempo, bar_state_name(s.barState), s.barErrorMs, s.barFixes, s.tempoVotes,
               s.opusKbps, s.roomOpusKbps);
    // the public address from the "what's my IP" service ("?" while it is asked), and whether a
    // player joined from the internet (so the port is open, whatever the router said)
    std::string out = buf;
    out += " lookedup=" + (s.hostLookedUpAddress.empty() ? std::string(s.hostLookupPending ? "?" : "-") : s.hostLookedUpAddress);
    out += std::string(" joinedfrominternet=") + (s.hostJoinedFromInternet ? "1" : "0");
    return out;
  }
  if (verb == "users")
  {
    Snapshot s;
    getSnapshot(s);
    std::string out;
    for (auto &u : s.users)
      for (auto &c : u.channels)
      {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%d %s vol=%.3f pan=%.3f mute=%d solo=%d peak=%.4f shown=%s\n", u.name.c_str(), c.index,
                 c.name.c_str(), (double)c.volume, (double)c.pan, c.mute ? 1 : 0, c.solo ? 1 : 0, (double)std::max(c.peak[0], c.peak[1]),
                 u.displayName.c_str());
        out += buf;
      }
    return out;
  }
  if (verb == "chatlog")
  {
    std::vector<ChatLine> lines;
    getChat(lines, 200);
    std::string out;
    for (auto &l : lines) out += (l.who.empty() ? "" : "<" + l.who + "> ") + l.text + "\n";
    return out;
  }
  return "unknown command: " + verb;
}

} // namespace jamsa
