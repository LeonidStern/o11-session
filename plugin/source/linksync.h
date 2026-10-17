/*
    O11 Session - linksync.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. It uses Ableton Link, which is under the GNU
    General Public License, version 2 or later, so the plug-in built with it is GPL too (see
    README.md).
*/

/*

  Ableton Link, while "Sync" is on: the room leads Live through Link. A VST3
  plug-in can't set its host's tempo, but with Link on in Live, Live follows
  the Link session, and the plug-in leads it:

  - tempo: the room's BPM goes to Link when the intervals change to it (a
    vote by anyone), so Live follows. A tempo change in Live still votes the
    room (the TempoFollower, on the host's tempo, as without Link); if the
    room doesn't take it, or Link's tempo comes from elsewhere (another app,
    or Live's own session when the plug-in joined it), Link gets the room's
    tempo back (LinkTempoKeeper).

  - phase: the interval starts sit on Link beats that are multiples of BPI
    (the quantum is BPI), so Live's bars start with the intervals (where BPI
    is a multiple of Live's quantum, its launch quantization). NINJAM's
    interval phase is each client's own, so the plug-in moves Link's timeline
    to its intervals: at once when it starts, when the BPM or BPI changes, or
    when the timeline jumped, and otherwise by small steps as the audio clock
    drifts from Link's clock. While Link leads Live's bars, the plug-in
    doesn't move its intervals to the host's bars (BarSync), so the two don't
    chase each other.

  - when a block is heard: the plug-in can't know the host's output latency,
    so it starts from "one block after process() is called" (by the earliest
    calls of the last two seconds: a call is often late, on a busy machine,
    but never early; linkmath::BlockClock) and, while Live
    plays at the room's tempo, measures how far Live's beat (the block's
    ppq, which Live keeps on Link's phase) is from the interval's beat in the
    same block, and moves the time it aligns Link at by that much
    (linkmath::Calibrator), so Live's beats and the plug-in's click coincide.

  Link is one per program: an instance in a room with Sync on turns it on,
  and one of them leads it (the first to get there); the others leave it
  alone and line their intervals up with the host's bars as without Link.
  Out of a room, or with Sync off, an instance isn't in Link: Live, turning
  its Link on, would take Link's tempo, with no room to lead it. Start/stop
  sync stays off: the plug-in never starts or stops Live's transport.

  If another app keeps moving Link's beat or tempo away from the room (another
  O11 Session on the network, in another room or with intervals of its own), the
  plug-in stops leading Link (contested) rather than fight over it, until it
  leaves the room or Sync goes off and on.

  The audio thread uses Link's realtime-safe calls only (captureAudioSessionState
  and commitAudioSessionState): no locks, no allocations.

*/

#ifndef _O11_LINKSYNC_H_
#define _O11_LINKSYNC_H_

#include <stddef.h>
#include <stdint.h>
#include <memory>

namespace o11 {

// The beat and phase math, apart from Link so the tests can check it
namespace linkmath {

// beats into the interval at sample pos, of an interval of len samples and bpi beats
double intervalBeat(double pos, double len, int bpi);

// a difference of phases (beats), wrapped into [-quantum/2, quantum/2)
double wrapPhase(double beats, double quantum);

double beatsToMs(double beats, double bpm);
double msToBeats(double ms, double bpm);

// Under DEADBAND_MS the phase is left alone; up to JUMP_MS it is moved by steps of at most
// STEP_MS, one every STEP_EVERY_MS at most; beyond, or to start, it is moved at once.
// STEP_EVERY_MS is longer than Link's grace period for a change made here (linkGraceMs(): for
// that long Link shows an app its own change rather than the network's), so between two steps
// the plug-in sees Live's changes (its tempo) before it commits again, and doesn't undo them.
constexpr double DEADBAND_MS = 0.5, STEP_MS = 2.0, STEP_EVERY_MS = 1500.0, JUMP_MS = 30.0;
double linkGraceMs(); // Link's own (1 second in Link 4.1)

struct Correction
{
  enum Kind { None, Step, Jump } kind = None;
  double shift = 0; // beats to move Link's timeline forward by
};

// errorBeats: the interval's beat minus Link's phase (wrapped), at bpm. realign: line up
// at once (the sync just started, or the BPM or BPI changed). sinceStepMs: since the last step.
Correction phaseCorrection(double errorBeats, double bpm, bool realign, double sinceStepMs);

// When a block starts on Link's clock: its time by the sample count, plus how late the earliest
// calls of the last WINDOW_MS were after theirs. A call is often late (the thread woke up late,
// the host did other work first, the machine is busy) but never early, so the earliest calls
// follow the sound card's clock and the late ones don't move it; smoothed over SMOOTH_MS. When
// even the earliest calls of a whole window are REJOIN_MS off (the sample count lost time, as
// when the host dropped audio), it goes there at once.
class BlockClock
{
public:
  static constexpr double WINDOW_MS = 2000.0, SMOOTH_MS = 1000.0, REJOIN_MS = 50.0;
  static constexpr int BUCKETS = 8; // the window, in parts

  // the block whose first sample is at nominalUs by the sample count, of blockUs, called at
  // nowUs (Link's clock): returns the offset to add to nominalUs
  double update(double nominalUs, double nowUs, double blockUs);
  void reset() { m_have = false; }
  double offsetUs() const { return m_offset; }

private:
  long long m_bucket[BUCKETS] = {}, m_last = 0;
  double m_min[BUCKETS] = {};
  double m_offset = 0, m_since = 0;
  bool m_have = false;
};

// Counts the times Link's beat moved by more than JUMP_MS while the plug-in kept it in line
// and nothing on its side changed: another app moving it. add() is true once that happened
// MOVES times within WINDOW seconds.
class MoveCounter
{
public:
  static constexpr int MOVES = 3;
  static constexpr double WINDOW = 20.0;
  bool add(double seconds);
  void reset() { m_n = 0; }

private:
  double m_t[MOVES] = {};
  int m_n = 0;
};

// When the plug-in's blocks are heard, against when it thinks they are: the host's beat
// minus the interval's in blocks where the host follows Link (it plays at the session's
// tempo, which is the room's, Link is in line with the intervals, and nothing moved for
// SETTLE_MS), in rounds of WINDOW_MS. A round that holds still (within SPREAD_MS) and is
// MIN_MS or more off moves offsetMs() by what it measured (at most MAX_MS from 0). If the
// next round is still more than FOLLOWED as far off on the same side, the host's beat didn't
// follow Link when it moved, so its beat isn't on Link's phase: the offset goes back to 0
// until reset().
class Calibrator
{
public:
  static constexpr double WINDOW_MS = 1000.0, SETTLE_MS = 1000.0, SPREAD_MS = 1.0, MIN_MS = 0.3, MAX_MS = 80.0;
  static constexpr double FOLLOWED = 0.8;

  // a block of blockMs where the host's beat is residualMs after the interval's (sub-beat);
  // true when offsetMs() changed
  bool add(double residualMs, double blockMs);
  void skip();  // a block that can't be measured: the round starts again
  void reset(); // forgets all: offset 0
  double offsetMs() const { return m_offset; }
  bool gaveUp() const { return m_gaveUp; }
  int rounds() const { return m_rounds; } // that moved the offset

private:
  double m_offset = 0, m_sum = 0, m_min = 0, m_max = 0, m_ms = 0, m_last = 0;
  int m_n = 0, m_rounds = 0;
  bool m_gaveUp = false;
};

} // namespace linkmath

// Keeps the room's tempo on Link. Network thread, for the instance that leads Link.
class LinkTempoKeeper
{
public:
  struct Action
  {
    enum Kind { None, Restore } kind = None;
    // why Link gets the room's tempo back:
    //   Foreign: the host isn't at Link's tempo (another app set it, and the host doesn't follow Link)
    //   Earlier: the host was at Link's tempo before (the plug-in joined the host's Link session)
    //   NotTaken: the room didn't take the vote for it (within VOTE_WAIT)
    //   SameBpm: it rounds to the room's BPM, OutOfRange: no room can have it (no vote for either)
    enum Why { Foreign, Earlier, NotTaken, SameBpm, OutOfRange } why = Foreign;
  };

  // now: seconds; linkTempo: the session's; roomTempo: the interval's (what the plug-in puts on
  // Link); serverBpm: what the server set last (the next intervals'); hostTempo: 0 when the host
  // doesn't say; hostChangedAt: when the host's tempo last changed (TempoFollower); peers: Link's.
  // Nothing while the room's tempo is about to change, while the host's tempo moves (the
  // TempoFollower votes once it is still), or while a vote for it waits; else, once Link's tempo
  // has been off the room's for DELAY seconds, Restore. More than FOREIGN_RESTORES restores for
  // Foreign or Earlier within FOREIGN_WINDOW seconds: another app keeps setting Link's tempo,
  // contested() from then on.
  Action update(double now, double linkTempo, double roomTempo, int serverBpm, double hostTempo, double hostChangedAt,
                size_t peers);
  void voted(double now, int bpm); // the TempoFollower voted for the host's tempo (until the room has it)
  void reset();                    // the plug-in leads Link again: forgets all, contested() too
  bool contested() const { return m_contested; }
  int restores() const { return m_restores; }

  static constexpr double SAME = 0.005, DELAY = 0.5, HOLD = 1.25, VOTE_WAIT = 5.0, FOREIGN_WINDOW = 30.0;
  static constexpr int FOREIGN_RESTORES = 3;

private:
  double m_since = -1; // Link's tempo is off the room's since then
  int m_votedBpm = 0;
  double m_votedAt = -1e9;
  double m_foreign[FOREIGN_RESTORES + 1] = {};
  int m_numForeign = 0, m_restores = 0;
  bool m_contested = false;
};

// One plug-in instance's side of the program's Link
class LinkSync
{
public:
  LinkSync();
  ~LinkSync();

  // network thread
  void use(bool on); // Sync is on in a room: Link is on in this program while an instance uses it
  bool inUse() const;
  // Lead Link, unless another instance in this program does or Link is off here: false then.
  // release() (and use(false), which releases too) waits for a process() that runs at the
  // time to return, so the audio thread never uses Link after it.
  bool drive();
  void release();
  bool driving() const;
  static bool otherInstanceDrives(const LinkSync *self);
  static bool unavailable(); // Link couldn't start in this program
  static bool enabledInProgram();
  static double enabledFor(); // seconds since Link went on in this program, 0 when it is off
  size_t numPeers() const;     // other Link apps, 0 when not in use
  double sessionTempo() const; // 0 when not in use
  void restoreTempo();         // process() puts the room's tempo back on Link
  bool restorePending() const; // restoreTempo() waits for process()
  void setContested();         // another app leads Link: stop leading it until the next drive()
  bool contested() const;

  // audio thread
  struct Block
  {
    int64_t sampleTime = 0; // samples the plug-in processed before this block
    int frames = 0;
    double sampleRate = 48000;
  };
  struct Interval
  {
    bool running = false; // the engine plays intervals
    int pos = 0, len = 0, bpi = 0; // the interval playing at the start of the block
    double bpm = 0;
  };
  struct Host // what the host said for the block (JUCE's AudioPlayHead, read into a HostPosition)
  {
    bool playing = false, hasPosition = false;
    double ppq = 0, tempo = 0; // tempo 0: it doesn't say
  };
  // The tempo and the beat of the room on Link (a no-op unless driving). A block's first sample
  // is heard one block after process() is called (as the play head doesn't say more), on Link's clock
  // smoothed against the sample count so the callbacks' jitter doesn't move the beat, plus
  // what the Calibrator measured. Returns true while Link leads the host's bars: driving, with
  // peers, not contested, and the intervals play.
  bool process(const Block &block, const Interval &interval, const Host &host);
  void resetTiming(); // the sample rate changed (not during process())

  // for the status line and the tests
  double lastErrorMs() const; // the interval's beat minus Link's phase, before correcting it
  bool aligned() const;       // within 2 ms
  int jumps() const;
  int steps() const;
  double latencyMs() const;   // the Calibrator's offset
  bool calibrationGaveUp() const;

  struct Impl;

private:
  bool processOwned(const Block &block, const Interval &interval, const Host &host); // while driving
  std::unique_ptr<Impl> m;
};

} // namespace o11

#endif
