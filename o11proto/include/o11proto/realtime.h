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
// (PR_SET_TIMERSLACK, 50 us by default); elsewhere they do nothing.
#ifndef O11PROTO_REALTIME_H
#define O11PROTO_REALTIME_H

#include <string>

namespace o11proto {
namespace thread {

// The calling thread is a host's audio thread, woken every `period_seconds`
// (a block): on macOS the time-constraint policy, its computation half the
// period (at most 5 ms) and its constraint the period; on Linux the least
// timer slack. True when it took.
bool makeAudioThread(double period_seconds);

// The calling thread does latency-critical work and blocks between (Live's
// network thread): on macOS the user-interactive QoS class; on Linux the
// least timer slack. True when it took.
bool makeLatencyCritical();

// For diagnostics, one line: the calling thread's scheduling (on macOS its
// QoS class, whether it has the time-constraint policy, the process's task
// role and background state; on Linux its timer slack) and how long timed
// waits of 1, 2 and 5 ms
// take on it (`samples` of each: the median and the longest).
std::string describe(int samples = 8);

}  // namespace thread
}  // namespace o11proto

#endif  // O11PROTO_REALTIME_H
