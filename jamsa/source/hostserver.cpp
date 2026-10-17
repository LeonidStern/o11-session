/*
    Jamsa - hostserver.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "hostserver.h"

#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "njproto/server.h"

namespace jamsa {

HostServer &HostServer::instance()
{
  static HostServer *s = new HostServer; // lives as long as the process (the plug-in may be unloaded)
  return *s;
}

HostServer::HostServer()
{
}

HostServer::~HostServer()
{
}

bool HostServer::start(const void *owner, int opusKbps, std::string *error)
{
  std::lock_guard<std::mutex> busy(m_startStop);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.owner == owner) return true;
    if (m_state.owner)
    {
      if (error)
        *error = "Another Jamsa in this program already hosts a server (port " + std::to_string(m_state.port) +
                 "). Connect to localhost:" + std::to_string(m_state.port) + " to join it.";
      return false;
    }
  }

  njproto::ServerConfig cfg;
  cfg.allow_anonymous = true;
  cfg.anonymous_privs = njproto::kPrivChat | njproto::kPrivVote;
  cfg.bpm = DEFAULT_BPM;
  cfg.bpi = DEFAULT_BPI;
  cfg.max_users = MAX_USERS;
  cfg.allow_codecs = njproto::kCapOpus | njproto::kCapFlac;
  cfg.chosen_codec = njproto::kCapOpus;
  cfg.opus_kbps = (std::uint32_t)(opusKbps >= 32 && opusKbps <= 510 ? opusKbps : 128);
  cfg.vote_threshold_percent = 1; // a single vote changes BPM or BPI
  cfg.log = [this](const std::string &line) {
    // what is worth showing: the server's complaints
    if (line.compare(0, 6, "error:") == 0 || line.compare(0, 8, "warning:") == 0)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_messages.push_back(line);
      if (m_messages.size() > 100) m_messages.erase(m_messages.begin());
      m_serial ++;
    }
  };

  std::unique_ptr<njproto::Server> server(new njproto::Server);
  int port = 0;
  for (int i = 0; i < PORTS_TO_TRY; i ++)
  {
    cfg.port = (std::uint16_t)(FIRST_PORT + i);
    if (server->start(cfg))
    {
      port = server->port();
      break;
    }
  }
  if (!port)
  {
    if (error) *error = "Ports " + std::to_string(FIRST_PORT) + " to " + std::to_string(FIRST_PORT + PORTS_TO_TRY - 1) + " are in use by other programs.";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = State();
    m_state.owner = owner;
    m_state.port = port;
    const std::string local = localNetworkAddress();
    if (!local.empty()) m_state.localAddress = local + ":" + std::to_string(port);
    m_messages.clear();
  }
  m_users = 0;
  m_quit = false;
  m_server = std::move(server);
  m_thread = std::thread([this]() { threadProc(); });
  return true;
}

void HostServer::threadProc()
{
  while (!m_quit)
  {
    m_server->poll(20);
    m_users = (int)m_server->userCount();
  }
  m_server->stop();
}

void HostServer::stop(const void *owner)
{
  std::lock_guard<std::mutex> busy(m_startStop);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_state.owner || m_state.owner != owner) return;
  }
  m_quit = true;
  if (m_thread.joinable()) m_thread.join();
  m_server.reset();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = State();
  m_users = 0;
}

HostServer::State HostServer::state()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  State s = m_state;
  s.users = m_users;
  return s;
}

std::vector<std::string> HostServer::messagesSince(unsigned long long *serial)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<std::string> out;
  if (*serial < m_serial)
  {
    const size_t missed = (size_t)(m_serial - *serial);
    const size_t from = missed < m_messages.size() ? m_messages.size() - missed : 0;
    out.assign(m_messages.begin() + (long)from, m_messages.end());
    *serial = m_serial;
  }
  return out;
}

std::string localNetworkAddress()
{
  // the address a packet to the internet would leave from; connecting a UDP socket sends nothing
  std::string out;
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return out;
#else
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return out;
#endif
  sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = htonl(0x08080808u); // 8.8.8.8: any address on the internet does
  sockaddr_in me;
  socklen_t len = sizeof(me);
  if (::connect(s, (sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (sockaddr *)&me, &len) == 0)
  {
    char buf[64];
    if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf))) out = buf;
  }
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
  if (out == "0.0.0.0" || out.compare(0, 4, "127.") == 0) out.clear();
  return out;
}

} // namespace jamsa
