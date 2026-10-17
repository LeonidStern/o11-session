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
  8 users, and the router is asked to forward the port (UPnP-IGD or NAT-PMP,
  njproto's PortMapper).

  While it runs, a "what's my IP" service on the internet is asked for this
  computer's public address, for when the router doesn't say (no UPnP or
  NAT-PMP, or a port forwarded by hand): JAMSA_WHATSMYIP=host:port/path points
  it elsewhere (the tests), and JAMSA_WHATSMYIP=off turns it off. JAMSA_PORTMAP=off
  leaves the router alone (the tests).

  Ports 2049 to 2058 are tried in turn (JAMSA_HOST_PORT=N takes port N instead,
  0 for any free one: the tests). start() and stop() block (stop waits
  for the router to remove the forward), so call them off the UI and audio
  threads.

*/

#ifndef _JAMSA_HOSTSERVER_H_
#define _JAMSA_HOSTSERVER_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace njproto { class Server; class PortMapper; }

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
    std::string publicAddress;   // host:port others use, once the router forwards the port
    std::string portmapNote;     // why there is no public address (the last thing the port mapper said)
    bool portmapPending = false; // still asking the router
    std::string localAddress;    // this machine's address in the local network, with the port
    std::string lookedUpAddress; // the public address with the port, from a "what's my IP" service: where
                                 // others join if the port is forwarded (the router may not say it is)
    bool lookupPending = false;  // still asking that service
    bool joinedFromInternet = false; // a player joined from an address on the internet, so the port is open
    int users = 0;
  };
  State state();

  // the log lines worth showing (port mapping, the server's errors), after serial
  std::vector<std::string> messagesSince(unsigned long long *serial);

  // Parses a line from the port mapper ("[portmap] ...") or the server into st; *show is whether
  // to show it. connections remembers each connection's address by id ("connection 3 from
  // 203.0.113.9", then "login 3 as Ana"), for joinedFromInternet. Public for the tests.
  static void parseLogLine(const std::string &line, State &st, bool *show, std::map<std::string, std::string> *connections = nullptr);
  // the IPv4 address in a "what's my IP" service's HTTP reply ("HTTP/1.1 200 OK ... 89.114.98.123\n")
  static bool parseWhatsMyIpReply(const std::string &reply, std::string *ip);
  // an IPv4 address on the internet, not a private, loopback or provider NAT one
  static bool isPublicIPv4(const std::string &addr);
  // Asks the services for this computer's public address: service is "host:port/path" (the
  // tests), "off" for none, or "" for the usual ones; "" when none answers or *stop is set.
  // Blocks for up to a few seconds per service.
  static std::string whatsMyIp(const std::string &service, const std::atomic<bool> *stop);

private:
  HostServer();
  ~HostServer();
  void threadProc();
  void onLog(const std::string &line);
  void lookUpPublicAddress(int port);
  void stopLookup();

  std::mutex m_startStop; // held while starting or stopping
  std::mutex m_mutex;     // guards everything below but the threads, the server and the mapper
  State m_state;
  std::vector<std::string> m_messages;
  unsigned long long m_serial = 0;
  std::map<std::string, std::string> m_connections; // for parseLogLine
  std::unique_ptr<njproto::Server> m_server; // used by the thread only, once started
  std::thread m_thread;
  std::atomic<bool> m_quit { false };
  std::atomic<int> m_users { 0 };
  std::unique_ptr<njproto::PortMapper> m_mapper; // started and stopped under m_startStop
  std::thread m_lookup; // asks the "what's my IP" service; joined by stop()
  std::atomic<bool> m_lookupStop { false };
};

// the address of this machine in its local network ("192.168.1.20"), or "" if not known
std::string localNetworkAddress();

} // namespace jamsa

#endif
