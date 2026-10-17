/*
    Jamsa - hostserver.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The built-in server: a njproto::Server run on a thread of its own, one per
  process, so one plug-in instance at a time owns it. It is set up like
  JamTaba's private server: anyone can join without an account, a single vote
  changes BPM or BPI, any player can switch the room's codec with !codec (Opus
  or FLAC) and its Opus bitrate with !bitrate (at first the host's own), up to
  8 users.

  Ports 2049 to 2058 are tried in turn. The server only listens: it doesn't ask
  the router to forward the port (UPnP or NAT-PMP), nor a service on the
  internet for this computer's public address; both are for a later version.
  start() and stop() block for the thread, so call them off the UI and audio
  threads.

*/

#ifndef _JAMSA_HOSTSERVER_H_
#define _JAMSA_HOSTSERVER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace njproto { class Server; }

namespace jamsa {

class HostServer
{
public:
  static HostServer &instance();

  enum { FIRST_PORT = 2049, PORTS_TO_TRY = 10, MAX_USERS = 8, DEFAULT_BPM = 120, DEFAULT_BPI = 4 };

  // true when started (or already running for this owner); the room's Opus bitrate starts at opusKbps
  bool start(const void *owner, int opusKbps, std::string *error);
  void stop(const void *owner);

  struct State
  {
    const void *owner = nullptr; // null when no server runs
    int port = 0;
    std::string localAddress;    // this machine's address in the local network, with the port
    int users = 0;
  };
  State state();

  // the server's log lines worth showing (errors), after serial
  std::vector<std::string> messagesSince(unsigned long long *serial);

private:
  HostServer();
  ~HostServer();
  void threadProc();

  std::mutex m_startStop; // held while starting or stopping
  std::mutex m_mutex;     // guards everything below but the thread and the server
  State m_state;
  std::vector<std::string> m_messages;
  unsigned long long m_serial = 0;
  std::unique_ptr<njproto::Server> m_server; // used by the thread only, once started
  std::thread m_thread;
  std::atomic<bool> m_quit { false };
  std::atomic<int> m_users { 0 };
};

// the address of this machine in its local network ("192.168.1.20"), or "" if not known
std::string localNetworkAddress();

} // namespace jamsa

#endif
