// o11proto - the scheduling of a thread, where the system lets a program
// choose it.
//
// macOS stretches the timed waits of an ordinary thread by its timer
// coalescing: on a loaded or background machine (a CI runner, say), a wait
// of 1 ms can take 10 and one of 5 ms 40. A thread with the time-constraint
// policy (what CoreAudio gives its own I/O threads) is not coalesced. A host
// of our own (a test, o11jam) that calls Engine::process() from a thread
// that sleeps between blocks makes that thread an audio thread here, as a
// DAW's already is. On Linux they give the thread the least timer slack
// (PR_SET_TIMERSLACK, 50 us by default). On Windows, whose timer ticks
// every 15.6 ms (a wait of 1 ms takes 15.6), the thread holds the 1 ms
// timer (timeBeginPeriod(1), for the whole process) until it ends; the
// network thread, then, holds it while Live runs. Elsewhere they do
// nothing.
#ifndef O11PROTO_REALTIME_H
#define O11PROTO_REALTIME_H

#include <string>

namespace o11proto {
namespace thread {

// The calling thread is a host's audio thread, woken every `period_seconds`
// (a block): on macOS the time-constraint policy, its computation half the
// period (at most 5 ms) and its constraint the period; on Linux the least
// timer slack; on Windows the 1 ms timer. True when it took.
bool makeAudioThread(double period_seconds);

// The calling thread does latency-critical work and blocks between (a
// server's thread, say): on macOS the user-interactive QoS class; on Linux
// the least timer slack; on Windows the 1 ms timer. True when it took.
bool makeLatencyCritical();

// The calling thread is Live's network thread (live::Transport): it blocks
// on its socket and wakes for each datagram and for each block the audio
// thread queues, and what it does then is on the audio's time. On macOS
// the time-constraint policy, as an audio thread of kNetworkPeriodSeconds
// (1 ms of computation within 2 ms; an ordinary thread's wake there came up
// to 24 ms late on a CI runner), else as makeLatencyCritical(); elsewhere as
// makeLatencyCritical().
enum class NetworkScheduling { Unchanged, LatencyCritical, Realtime };
constexpr double kNetworkPeriodSeconds = 0.002;
NetworkScheduling makeNetworkThread();

// For diagnostics, one line: the calling thread's scheduling (on macOS its
// QoS class, whether it has the time-constraint policy, the process's task
// role and background state; on Linux its timer slack; on Windows how many
// threads hold the 1 ms timer) and how long timed
// waits of 1, 2 and 5 ms
// take on it (`samples` of each: the median and the longest).
std::string describe(int samples = 8);

}  // namespace thread
}  // namespace o11proto

#endif  // O11PROTO_REALTIME_H
