/*
    O11 Session - linksync.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. It uses Ableton Link, which is under the GNU
    General Public License, version 2 or later, so the plug-in built with it is GPL too (see
    README.md).
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

#include "linksync.h"

#include <math.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "hostsync.h"

namespace o11 {

// ---------------------------------------------------------------------------
// the beat and phase math
// ---------------------------------------------------------------------------

namespace linkmath {

double intervalBeat(double pos, double len, int bpi)
{
  return len > 0 ? pos / len * bpi : 0.0;
}

double wrapPhase(double beats, double quantum)
{
  if (quantum <= 0) return 0.0;
  double w = fmod(beats + quantum / 2, quantum);
  if (w < 0) w += quantum;
  return w - quantum / 2;
}

double beatsToMs(double beats, double bpm)
{
  return bpm > 0 ? beats * 60000.0 / bpm : 0.0;
}

double msToBeats(double ms, double bpm)
{
  return ms * bpm / 60000.0;
}

Correction phaseCorrection(double errorBeats, double bpm, bool realign, double sinceStepMs)
{
  Correction c;
  const double ms = fabs(beatsToMs(errorBeats, bpm));
  if (realign || ms > JUMP_MS)
  {
    c.kind = Correction::Jump;
    c.shift = errorBeats;
  }
  else if (ms >= DEADBAND_MS && sinceStepMs >= STEP_EVERY_MS)
  {
    c.kind = Correction::Step;
    c.shift = std::min(ms, STEP_MS) * (errorBeats < 0 ? -1 : 1) * bpm / 60000.0;
  }
  return c;
}

double linkGraceMs()
{
  return std::chrono::duration<double, std::milli>(ableton::link::detail::kLocalModGracePeriod).count();
}

double BlockClock::update(double nominalUs, double nowUs, double blockUs)
{
  const double late = nowUs - nominalUs;
  const long long b = (long long)floor(nominalUs / (WINDOW_MS * 1000.0 / BUCKETS));
  if (!m_have || b < m_last)
  {
    // the first block (or the sample count went back): start again
    for (int i = 0; i < BUCKETS; i ++) m_bucket[i] = -1;
    m_have = true;
    m_offset = late;
    m_since = nominalUs;
  }
  m_last = b;
  const int k = (int)(b % BUCKETS);
  if (m_bucket[k] != b)
  {
    m_bucket[k] = b;
    m_min[k] = late;
  }
  else m_min[k] = std::min(m_min[k], late);

  double earliest = late;
  for (int i = 0; i < BUCKETS; i ++)
    if (m_bucket[i] >= 0 && m_bucket[i] > b - BUCKETS) earliest = std::min(earliest, m_min[i]);

  if (nominalUs - m_since < WINDOW_MS * 1000.0) m_offset = std::min(m_offset, earliest); // the first window: the earliest yet
  else if (fabs(earliest - m_offset) >= REJOIN_MS * 1000.0) m_offset = earliest;
  else m_offset += std::min(1.0, blockUs / (SMOOTH_MS * 1000.0)) * (earliest - m_offset);
  return m_offset;
}

bool MoveCounter::add(double seconds)
{
  int n = 0;
  for (int i = 0; i < m_n; i ++)
    if (seconds - m_t[i] < WINDOW) m_t[n++] = m_t[i];
  if (n == MOVES)
  {
    for (int i = 1; i < MOVES; i ++) m_t[i - 1] = m_t[i];
    n--;
  }
  m_t[n++] = seconds;
  m_n = n;
  return m_n >= MOVES;
}

void Calibrator::reset()
{
  *this = Calibrator();
}

void Calibrator::skip()
{
  m_n = 0;
  m_ms = m_sum = 0;
}

bool Calibrator::add(double residualMs, double blockMs)
{
  if (m_gaveUp || blockMs <= 0) return false;
  if (m_n == 0) m_min = m_max = residualMs;
  m_min = std::min(m_min, residualMs);
  m_max = std::max(m_max, residualMs);
  m_sum += residualMs;
  m_n++;
  m_ms += blockMs;
  if (m_ms < WINDOW_MS) return false;

  // a whole round
  const double mean = m_sum / m_n, spread = m_max - m_min;
  skip();
  if (spread > SPREAD_MS) return false; // the host's beat moved (a loop, the playhead): another round
  if (fabs(mean) < MIN_MS)
  {
    m_last = 0; // in line
    return false;
  }
  // the last round moved the offset, and this one is still about as far off on the same side:
  // the host's beat didn't follow Link
  const bool followed = m_last == 0 || (mean < 0) != (m_last < 0) || fabs(mean) <= fabs(m_last) * FOLLOWED;
  const double next = std::min(MAX_MS, std::max(-MAX_MS, m_offset + mean));
  if (!followed || next == m_offset)
  {
    m_gaveUp = true;
    m_last = 0;
    const bool changed = m_offset != 0;
    m_offset = 0;
    return changed;
  }
  m_last = next - m_offset;
  m_offset = next;
  m_rounds++;
  return true;
}

} // namespace linkmath

// ---------------------------------------------------------------------------
// keeping the room's tempo on Link
// ---------------------------------------------------------------------------

void LinkTempoKeeper::reset()
{
  *this = LinkTempoKeeper();
}

void LinkTempoKeeper::voted(double now, int bpm)
{
  m_votedBpm = bpm;
  m_votedAt = now;
}

LinkTempoKeeper::Action LinkTempoKeeper::update(double now, double linkTempo, double roomTempo, int serverBpm, double hostTempo,
                                                double hostChangedAt, size_t peers)
{
  Action a;
  if (m_votedBpm > 0 && roomTempo > 0 && lround(roomTempo) == m_votedBpm) m_votedBpm = 0; // the room took the vote
  // nobody to lead, or Link has the room's tempo, or the room's changes at its next interval
  // (and Link with it)
  if (m_contested || peers == 0 || linkTempo <= 0 || roomTempo <= 0 || fabs(linkTempo - roomTempo) <= SAME ||
      (serverBpm > 0 && serverBpm != lround(roomTempo)))
  {
    m_since = -1;
    return a;
  }
  if (m_since < 0) m_since = now;
  if (now - m_since < DELAY) return a;

  const long n = lround(linkTempo);
  if (hostTempo <= 0 || fabs(hostTempo - linkTempo) > BarSync::SAME_TEMPO) a.why = Action::Foreign;
  else
  {
    // the host follows Link: its tempo moved (the player changed it, or Link's tempo changed
    // and the host followed), or it was there before
    if (now - hostChangedAt < HOLD) return a; // still moving, or the TempoFollower is about to vote
    if (m_votedBpm == n && now - m_votedAt < VOTE_WAIT) return a; // the room decides
    if (m_votedBpm == n) a.why = Action::NotTaken;
    else if (n == lround(roomTempo)) a.why = Action::SameBpm;
    else if (n < TempoFollower::MIN_BPM || n > TempoFollower::MAX_BPM) a.why = Action::OutOfRange;
    else a.why = Action::Earlier;
  }
  a.kind = Action::Restore;
  m_since = -1;
  if (a.why == Action::NotTaken) m_votedBpm = 0;
  m_restores++;

  if (a.why == Action::Foreign || a.why == Action::Earlier)
  {
    int k = 0;
    for (int i = 0; i < m_numForeign; i ++)
      if (now - m_foreign[i] < FOREIGN_WINDOW) m_foreign[k++] = m_foreign[i];
    if (k == FOREIGN_RESTORES + 1)
    {
      for (int i = 1; i < k; i ++) m_foreign[i - 1] = m_foreign[i];
      k--;
    }
    m_foreign[k++] = now;
    m_numForeign = k;
    if (m_numForeign > FOREIGN_RESTORES)
    {
      m_contested = true; // something keeps putting its own tempo on Link
      a.kind = Action::None;
    }
  }
  return a;
}

// ---------------------------------------------------------------------------
// LinkSync
// ---------------------------------------------------------------------------

static const double TEMPO_EPS = 1e-3;      // BPM: Link's tempo is the interval's
static const double ALIGNED_MS = 2.0;

static double steady_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct LinkSync::Impl
{
  ableton::Link *link = nullptr; // while driving
  bool inUse = false;            // under g_mutex

  std::atomic<bool> owned { false }, fresh { false }, restore { false }, contested { false };
  std::atomic<bool> busy { false }; // the audio thread is in process()
  std::atomic<double> lastErrorMs { 0 }, latencyMs { 0 };
  std::atomic<int> jumps { 0 }, steps { 0 };
  std::atomic<bool> aligned { false }, calGaveUp { false };

  // audio thread
  bool started = false;
  double bpm = 0;
  int bpi = 0;
  bool realign = false;
  double lastStepUs = -1e18;   // the last correction
  double lastChangeUs = -1e18; // the last time Link's beat or tempo moved, ours or not
  // when blocks start on Link's clock, by the sample count
  linkmath::BlockClock clock;
  double clockRate = 0;
  // Link's phase after the last block, to tell when something else moves it
  bool haveRef = false;
  double refUs = 0, refPhase = 0, refTempo = 0;
  int refBpi = 0;
  linkmath::MoveCounter moves;
  linkmath::Calibrator calib;
  bool hostWasPlaying = false;
};

static std::mutex g_mutex;
static std::unique_ptr<ableton::Link> g_link; // while an instance uses it (Sync on, in a room)
static LinkSync::Impl *g_driver = nullptr;
static int g_users = 0;
static bool g_unavailable = false;            // Link couldn't start: tried again RETRY_SECONDS later
static double g_enabledAt = 0, g_failedAt = 0;
static const double RETRY_SECONDS = 10.0;

// Stops an instance leading Link (under g_mutex): once this returns, its process() doesn't use
// Link any more. A process() already past its check of `owned` finishes first (a block's work at
// most); one that starts after it sees `owned` false. Both flags are sequentially consistent, so
// one of the two always sees the other's.
static void disown(LinkSync::Impl *d)
{
  d->owned.store(false, std::memory_order_seq_cst);
  while (d->busy.load(std::memory_order_seq_cst)) std::this_thread::yield();
  d->link = nullptr;
  if (g_driver == d) g_driver = nullptr;
}

LinkSync::LinkSync() : m(new Impl) {}

LinkSync::~LinkSync()
{
  std::unique_ptr<ableton::Link> last;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_driver == m.get()) disown(m.get());
    if (m->inUse)
    {
      m->inUse = false;
      if (--g_users == 0) last = std::move(g_link);
    }
  }
  // Link's threads stop here, outside the lock
}

void LinkSync::use(bool on)
{
  std::unique_ptr<ableton::Link> last;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (on == m->inUse) return;
    if (on)
    {
      if (!g_link)
      {
        if (g_unavailable && steady_seconds() - g_failedAt < RETRY_SECONDS) return;
        try
        {
          g_link.reset(new ableton::Link(120.0));
          g_link->enable(true);
          g_enabledAt = steady_seconds();
          g_unavailable = false;
        }
        catch (...)
        {
          g_link.reset();
          g_unavailable = true;
          g_failedAt = steady_seconds();
          return;
        }
      }
      m->inUse = true;
      g_users++;
    }
    else
    {
      if (g_driver == m.get()) disown(m.get());
      m->inUse = false;
      if (--g_users == 0)
      {
        last = std::move(g_link); // not in Link at all
        g_enabledAt = 0;
      }
    }
  }
}

bool LinkSync::inUse() const
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return m->inUse;
}

bool LinkSync::drive()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_driver == m.get()) return true;
  if (g_driver || !g_link || !m->inUse) return false;
  g_driver = m.get();
  m->link = g_link.get();
  m->restore.store(false);
  m->contested.store(false);
  m->aligned.store(false);
  m->fresh.store(true);
  m->owned.store(true, std::memory_order_release);
  return true;
}

void LinkSync::release()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_driver != m.get()) return;
  disown(m.get());
  m->aligned.store(false);
  m->restore.store(false);
}

bool LinkSync::driving() const { return m->owned.load(); }

bool LinkSync::otherInstanceDrives(const LinkSync *self)
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_driver && (!self || g_driver != self->m.get());
}

bool LinkSync::unavailable()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_unavailable;
}

bool LinkSync::enabledInProgram()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_link != nullptr;
}

double LinkSync::enabledFor()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_link ? steady_seconds() - g_enabledAt : 0.0;
}

size_t LinkSync::numPeers() const
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_link && m->inUse ? g_link->numPeers() : 0;
}

double LinkSync::sessionTempo() const
{
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_link && m->inUse ? g_link->captureAppSessionState().tempo() : 0.0;
}

void LinkSync::restoreTempo() { m->restore.store(true); }
bool LinkSync::restorePending() const { return m->restore.load(); }
void LinkSync::setContested() { m->contested.store(true); }
bool LinkSync::contested() const { return m->contested.load(); }

double LinkSync::lastErrorMs() const { return m->lastErrorMs.load(); }
bool LinkSync::aligned() const { return m->aligned.load(); }
int LinkSync::jumps() const { return m->jumps.load(); }
int LinkSync::steps() const { return m->steps.load(); }
double LinkSync::latencyMs() const { return m->latencyMs.load(); }
bool LinkSync::calibrationGaveUp() const { return m->calGaveUp.load(); }

void LinkSync::resetTiming()
{
  m->clock.reset();
  m->haveRef = false;
  m->started = false;
  m->calib.reset();
  m->latencyMs.store(0);
  m->calGaveUp.store(false);
}

// The audio thread: Link's realtime-safe calls only (clock, numPeers, captureAudioSessionState
// and commitAudioSessionState), no locks, no allocations.
bool LinkSync::process(const Block &b, const Interval &iv, const Host &host)
{
  // release() waits while this runs (see disown())
  Impl &d = *m;
  d.busy.store(true, std::memory_order_seq_cst);
  const bool leads = d.owned.load(std::memory_order_seq_cst) && processOwned(b, iv, host);
  d.busy.store(false, std::memory_order_seq_cst);
  return leads;
}

bool LinkSync::processOwned(const Block &b, const Interval &iv, const Host &host)
{
  Impl &d = *m;
  ableton::Link &link = *d.link;
  if (d.fresh.exchange(false))
  {
    d.started = false;
    d.haveRef = false;
    d.lastStepUs = d.lastChangeUs = -1e18;
    d.moves.reset();
    d.calib.skip();
    d.clock.reset();
  }
  if (b.frames <= 0 || b.sampleRate <= 0) return false;

  // when the block's first sample is heard: one block after it starts on Link's clock (by the
  // sample count and the earliest calls), plus what the calibration measured
  const double nowUs = (double)link.clock().micros().count();
  const double blockUs = b.frames * 1e6 / b.sampleRate;
  const double nominalUs = b.sampleTime * 1e6 / b.sampleRate;
  if (b.sampleRate != d.clockRate)
  {
    d.clock.reset();
    d.clockRate = b.sampleRate;
  }
  const double tUs = nominalUs + d.clock.update(nominalUs, nowUs, blockUs) + blockUs + d.calib.offsetMs() * 1000.0;
  const std::chrono::microseconds t((long long)llround(tUs));

  // the host started again: the calibration may work now, if it gave up
  if (host.playing && !d.hostWasPlaying && d.calib.gaveUp()) d.calib.reset();
  d.hostWasPlaying = host.playing;

  if (!iv.running || iv.len <= 0 || iv.bpi <= 0 || iv.bpm <= 0 || d.contested.load(std::memory_order_relaxed))
  {
    if (!iv.running) d.started = false;
    d.haveRef = false;
    d.aligned.store(false, std::memory_order_relaxed);
    d.calib.skip();
    return false;
  }

  const bool leads = link.numPeers() > 0;
  ableton::Link::SessionState state = link.captureAudioSessionState();
  const double q = iv.bpi;
  bool commit = false;

  // did Link's beat move since the last block, other than by its tempo? Then another app moved
  // it (or the plug-in joined another session).
  bool moved = false;
  if (d.haveRef && d.refBpi == iv.bpi && fabs(state.tempo() - d.refTempo) < TEMPO_EPS)
  {
    const double expected = d.refPhase + (tUs - d.refUs) * d.refTempo / 60e6;
    moved = fabs(linkmath::beatsToMs(linkmath::wrapPhase(state.phaseAtTime(t, q) - expected, q), d.refTempo)) > linkmath::JUMP_MS;
  }
  if (d.haveRef && fabs(state.tempo() - d.refTempo) >= TEMPO_EPS) d.lastChangeUs = tUs;

  // the room's tempo goes to Link when the intervals change to it, and when the network thread
  // puts it back
  const bool restore = d.restore.exchange(false);
  if (!d.started || iv.bpm != d.bpm || restore)
  {
    state.setTempo(iv.bpm, t);
    d.realign = true;
    commit = true;
    d.lastChangeUs = tUs;
  }
  if (!d.started || iv.bpi != d.bpi) d.realign = true;
  d.started = true;
  d.bpm = iv.bpm;
  d.bpi = iv.bpi;

  if (fabs(state.tempo() - iv.bpm) > TEMPO_EPS)
  {
    // another peer (Live) changed the tempo: the room votes on it first (TempoFollower), and the
    // beat lines up again at the room's tempo
    d.haveRef = false;
    d.aligned.store(false, std::memory_order_relaxed);
    d.calib.skip();
    if (commit) link.commitAudioSessionState(state);
    return leads;
  }

  if (moved)
  {
    d.lastChangeUs = tUs;
    if (!d.realign && d.moves.add(tUs / 1e6))
    {
      // something keeps moving the beat away from the intervals: don't fight it
      d.contested.store(true);
      d.haveRef = false;
      d.aligned.store(false, std::memory_order_relaxed);
      if (commit) link.commitAudioSessionState(state);
      return false;
    }
  }

  // the interval's beat now, against Link's phase in BPI beats
  const double beat = linkmath::intervalBeat(iv.pos, iv.len, iv.bpi);
  const double e = linkmath::wrapPhase(beat - state.phaseAtTime(t, q), q);
  d.lastErrorMs.store(linkmath::beatsToMs(e, iv.bpm), std::memory_order_relaxed);
  const linkmath::Correction c = linkmath::phaseCorrection(e, iv.bpm, d.realign, (tUs - d.lastStepUs) / 1000.0);
  if (c.kind != linkmath::Correction::None)
  {
    state.forceBeatAtTime(state.beatAtTime(t, q) + c.shift, t, q);
    commit = true;
    (c.kind == linkmath::Correction::Jump ? d.jumps : d.steps).fetch_add(1, std::memory_order_relaxed);
    d.lastStepUs = d.lastChangeUs = tUs;
    d.realign = false;
  }
  const double afterMs = linkmath::beatsToMs(e - c.shift, iv.bpm);
  d.aligned.store(fabs(afterMs) < ALIGNED_MS, std::memory_order_relaxed);

  // When the block is heard: while the host plays at the room's tempo with Link (which it
  // follows when it is Live with Link on), its beat for the block against Link's phase at the
  // time the block is taken to be heard
  if (leads && host.playing && host.hasPosition && fabs(host.tempo - iv.bpm) <= BarSync::SAME_TEMPO &&
      fabs(afterMs) < linkmath::DEADBAND_MS && tUs - d.lastChangeUs >= linkmath::Calibrator::SETTLE_MS * 1000.0)
  {
    const double residual = linkmath::beatsToMs(linkmath::wrapPhase(host.ppq - state.phaseAtTime(t, 1.0), 1.0), iv.bpm);
    if (d.calib.add(residual, blockUs / 1000.0))
    {
      d.latencyMs.store(d.calib.offsetMs(), std::memory_order_relaxed);
      d.calGaveUp.store(d.calib.gaveUp(), std::memory_order_relaxed);
      d.realign = true; // Link lines up at the new time at once
      d.lastChangeUs = tUs;
    }
  }
  else d.calib.skip();

  d.haveRef = true;
  d.refUs = tUs;
  d.refPhase = state.phaseAtTime(t, q);
  d.refTempo = state.tempo();
  d.refBpi = iv.bpi;
  if (commit) link.commitAudioSessionState(state);
  return leads;
}

} // namespace o11
