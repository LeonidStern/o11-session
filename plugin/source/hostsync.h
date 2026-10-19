/*
    O11 Session - hostsync.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The host leads ("Sync", on by default), from what it tells the plug-in in
  each block (JUCE's AudioPlayHead::PositionInfo, read into a HostPosition by
  the processor). A plug-in can't set its host's tempo, so:

  - tempo: when the host's tempo changes and stays for a second at a value
    that rounds to another BPM than the room's, the plug-in votes the room to
    it (!vote bpm N), once; and once when it joins a room. It doesn't vote
    again when someone else changes the room, so two players don't fight; the
    editor says when the host's tempo and the room's differ.

  - bars: while the host plays, at the room's tempo, the plug-in lines its
    intervals up with the host's bars: each interval starts where the host is
    at a multiple of BPI beats from bar 1. NINJAM's interval phase is each
    client's own, so the plug-in moves it: the interval playing then ends a
    little sooner or later, and the next ones start in line. Nothing jumps
    inside an interval. Loops in the host don't move the intervals (they keep
    the line they had before the loop), a move of the playhead lines them up
    again, and a stopped host leaves them alone.

    The interval that ends sooner or later is also the one the plug-in sends,
    so the others hear one interval of another length each time: once when the
    host starts, moves or changes tempo, not at each time round a loop. A loop
    the host doesn't declare (no loop points) looks like a move the first time
    round, and like a loop from then on (the same jump back). A drift (the
    engine's intervals are a whole number of samples) is fixed only once it has
    lasted a whole interval: about a millisecond, now and then.

  BarSync runs on the audio thread (no locks, no allocations), the
  TempoFollower on the network thread.

  With Ableton Link (linksync.h), the room leads the other way: Live, with
  its Link on, takes the room's tempo, and its bars start with the
  intervals, so BarSync leaves the intervals alone; a tempo change in Live
  still votes the room (the TempoFollower).

*/

#ifndef _O11_HOSTSYNC_H_
#define _O11_HOSTSYNC_H_

#include <string>

namespace o11 {

// what the host said about its transport for a block
struct HostPosition
{
  bool playing = false;
  bool hasPosition = false; // ppq is valid
  double ppq = 0;           // quarter notes from the project's start (bar 1), at the block's first sample
  double tempo = 0;         // quarter notes per minute, 0 when the host doesn't say
  bool cycleActive = false; // the host loops from cycleStart to cycleEnd (quarter notes)
  double cycleStart = 0, cycleEnd = 0;
  int timeSigNum = 0, timeSigDen = 0; // the time signature, 0 when the host doesn't say
};

namespace barmath {

// where the host is in an interval of len samples and bpi beats, at ppq: samples from its start
double hostIntervalPos(double ppq, int bpi, double len);

// a difference of positions in an interval of len samples, wrapped into [-len/2, len/2)
double wrapSamples(double samples, double len);

// The interval playing now is at pos, and the host at target (both in an interval of the usual
// length len): when should it end so the next one starts with the host's? In samples from now,
// at least 1: the host's next interval start, or the one after, whichever makes the interval
// playing now closer to the usual length.
int intervalEnd(int pos, int len, double target);

} // namespace barmath

class BarSync
{
public:
  enum State
  {
    Off,         // not following, or no intervals play
    NoPosition,  // the host doesn't say where it is
    Stopped,     // the host doesn't play
    OtherTempo,  // the host's tempo isn't the room's: bars can't line up
    Lining,      // the interval playing now ends early or late, so the next one is in line
    Aligned,     // the intervals start on the host's bars
  };
  struct Interval
  {
    bool running = false; // the engine plays intervals
    int pos = 0;          // in the interval playing at the start of the block
    int len = 0;          // the usual length (Engine::usualIntervalLength())
    int bpi = 0;
    double bpm = 0;
    int count = 0;        // Engine::loopCount(): which interval plays
  };

  // One block: returns how many samples from now the interval playing now should end
  // (Engine::setIntervalEnd()), or 0 to leave it.
  int process(const HostPosition &host, int frames, double sampleRate, const Interval &iv);
  void reset(); // forget the host's position (follow turned off or on, a new sample rate)

  State state() const { return m_state; }
  double errorMs() const { return m_errorMs; } // where the host's interval is, minus ours
  int corrections() const { return m_corrections; }

  static constexpr double TOLERANCE_MS = 1.0;  // closer than this is in line
  static constexpr double SAME_TEMPO = 0.01;   // BPM: the host's tempo is the room's
  static constexpr double SAME_PLACE = 0.01;   // quarter notes: the host plays on from where it was

private:
  State m_state = Off;
  double m_errorMs = 0;
  int m_corrections = 0;
  bool m_wasPlaying = false;
  double m_expectedPpq = 0; // where the host should be at the next block, if it plays on
  double m_blockBeats = 0;  // how far the last block went (quarter notes)
  double m_loopOffset = 0;  // quarter notes the host's loops went back, since it last started or moved
  int m_fixedInterval = -1; // the interval whose end was moved (the next one starts in line)
  bool m_jumped = false;    // the host jumped since it started: from m_jumpFrom to m_jumpTo (quarter notes)
  double m_jumpFrom = 0, m_jumpTo = 0;
  bool m_lineUp = true;     // line up at once: the host started or moved, or the tempo or interval changed
  int m_outOfLine = 0;      // samples out of line since the intervals were in line (a drift)
  int m_len = 0, m_bpi = 0; // the interval's usual length and beats in the last block
};

// Votes the room to the host's tempo. Network thread.
class TempoFollower
{
public:
  // now: seconds; hostTempo: 0 when the host doesn't say; roomBpm: the room's BPM, 0 when
  // not in a room. Returns the BPM to vote for (!vote bpm N), or 0.
  int update(double now, double hostTempo, int roomBpm);
  void arm(); // once more, when the host's tempo is known and still: on joining a room, on following again
  void disarm(); // no vote for the tempo the host has now (Live takes the room's through Link)
  bool waiting() const { return m_armed; } // a vote may come when the host's tempo is still
  double changedAt() const { return m_since; } // when the host's tempo last changed (seconds)
  int votes() const { return m_votes; }

  static constexpr double QUIET = 1.0;   // seconds the host's tempo stays before a vote
  static constexpr double SAME = 0.005;  // BPM: the host's tempo didn't change
  static constexpr int MIN_BPM = 40, MAX_BPM = 400; // what NINJAM servers take

private:
  double m_tempo = 0, m_since = 0;
  bool m_armed = false;
  int m_votes = 0;
};

// a tempo as the editor writes it: "120", "109.6", "120.25"
std::string bpmText(double bpm);

// What to call the host: "Ableton" for Ableton Live (not "Live", the room's mode), else its name (or "The DAW" when it doesn't say)
std::string shortHostName(const std::string &hostName);

// The host's name where there is little room (the transport bar): shortHostName() without a
// maker or version in it, or its first word, if that fits in ten characters ("Ableton", "Bitwig",
// "Cubase", "REAPER"), else "DAW"
std::string hostChipName(const std::string &hostName);

// The line the editor shows when the host's tempo isn't the room's, such as
// "Ableton is at 120 BPM, the room at 95: set Ableton to 95", or "" when they agree. suggestLink:
// Link would lead the host if it were on there ("turn on Link in Ableton, or set it to 95").
std::string hostTempoNote(const std::string &hostName, double hostTempo, int roomBpm, bool following, bool suggestLink = false);

// Ableton Link, while Sync is on (linksync.h). Off: Sync is off, so the plug-in isn't in Link.
// Waiting: Sync is on, out of a room: in Link once in one. Leading: this instance leads Link.
// OtherInstance: in a room, while another instance in this program leads Link. Contested:
// another app on the network keeps leading Link, so this instance stopped. Unavailable: Link
// couldn't start.
enum class LinkState { Off, Waiting, Leading, OtherInstance, Contested, Unavailable };

// "Turn on Link in Ableton: ...", when Link sees no other app (the chat says it once, the Sync
// button's tooltip while it lasts)
std::string linkHint(const std::string &hostName);

// the Sync button's tooltip; peers: the other Link apps
std::string syncTooltip(const std::string &hostName, LinkState link, int peers);

// Where the next interval starts in the host, at quarter note ppq of a time signature num/den
// (4/4 when not known): "Next interval at Live bar 9", or "... bar 9, beat 3" when it doesn't
// start on a bar. bar and beat count from 1.
std::string nextIntervalText(const std::string &chipName, double ppq, int num, int den);
void barAndBeat(double ppq, int num, int den, int *bar, int *beat, bool *onBeat);

} // namespace o11

#endif
