/*
    Jamsa - hostserver.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "hostserver.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>

#include "njproto/portmap.h"
#include "njproto/server.h"

namespace jamsa {

static const size_t MAX_MESSAGES = 200;

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

static bool env_is_off(const char *name)
{
  const char *v = getenv(name);
  return v && strcmp(v, "off") == 0;
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
  stopLookup(); // (stop() has joined the last one already)

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
  cfg.log = [this](const std::string &line) { onLog(line); };

  std::unique_ptr<njproto::Server> server(new njproto::Server);
  int port = 0;
  const char *fixedPort = getenv("JAMSA_HOST_PORT"); // the tests: 0 for any free port
  for (int i = 0; i < PORTS_TO_TRY; i ++)
  {
    cfg.port = (std::uint16_t)(fixedPort ? atoi(fixedPort) : FIRST_PORT + i);
    if (server->start(cfg))
    {
      port = server->port();
      break;
    }
    if (fixedPort) break;
  }
  if (!port)
  {
    if (error) *error = "Ports " + std::to_string(FIRST_PORT) + " to " + std::to_string(FIRST_PORT + PORTS_TO_TRY - 1) + " are in use by other programs.";
    return false;
  }

  const bool portmap = !env_is_off("JAMSA_PORTMAP");
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = State();
    m_state.owner = owner;
    m_state.port = port;
    m_state.portmapPending = portmap;
    const std::string local = localNetworkAddress();
    if (!local.empty()) m_state.localAddress = local + ":" + std::to_string(port);
    m_state.lookupPending = true;
    m_messages.clear();
    m_connections.clear();
  }
  m_users = 0;
  m_quit = false;
  m_server = std::move(server);
  m_thread = std::thread([this]() { threadProc(); });

  if (portmap)
  {
    m_mapper.reset(new njproto::PortMapper);
    m_mapper->log = [this](const std::string &line) { onLog(line); };
    m_mapper->start(port);
  }
  m_lookupStop = false;
  m_lookup = std::thread([this, port]() { lookUpPublicAddress(port); });
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
  m_mapper.reset(); // removes the router's forward; no more log calls after this
  m_quit = true;
  if (m_thread.joinable()) m_thread.join();
  m_server.reset();
  stopLookup();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = State();
  m_users = 0;
}

void HostServer::stopLookup()
{
  m_lookupStop = true;
  if (m_lookup.joinable()) m_lookup.join();
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
  const unsigned long long first = m_serial - m_messages.size();
  if (*serial < first) *serial = first;
  for (unsigned long long i = *serial; i < m_serial; i ++) out.push_back(m_messages[(size_t)(i - first)]);
  *serial = m_serial;
  return out;
}

static bool starts_with(const std::string &s, const char *prefix)
{
  return s.compare(0, strlen(prefix), prefix) == 0;
}

static bool starts_with_nocase(const std::string &s, const char *prefix)
{
  const size_t n = strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i ++)
  {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

// the first word after prefix ("connection 3 from 1.2.3.4" with "connection " is "3"), and what follows it
static bool word_after(const std::string &line, const char *prefix, std::string *word, std::string *rest)
{
  if (!starts_with(line, prefix)) return false;
  const size_t from = strlen(prefix);
  const size_t end = line.find(' ', from);
  *word = line.substr(from, end == std::string::npos ? std::string::npos : end - from);
  *rest = end == std::string::npos ? std::string() : line.substr(end + 1);
  return !word->empty();
}

void HostServer::parseLogLine(const std::string &line, State &st, bool *show, std::map<std::string, std::string> *connections)
{
  *show = false;
  std::string id, rest;
  if (starts_with(line, "[portmap]"))
  {
    std::string text = line.substr(9);
    while (!text.empty() && text[0] == ' ') text.erase(0, 1);
    *show = true;

    static const char *reachable = "people can connect to ";
    const size_t at = line.find(reachable);
    if (at != std::string::npos)
    {
      std::string addr = line.substr(at + strlen(reachable));
      while (!addr.empty() && (addr.back() == ' ' || addr.back() == '.')) addr.pop_back();
      st.publicAddress = addr;
      st.portmapNote.clear();
      st.portmapPending = false;
    }
    else if (line.find("asking the router") != std::string::npos)
    {
      st.portmapPending = true;
    }
    else if (line.find("removed the forward") != std::string::npos)
    {
      st.publicAddress.clear();
      st.portmapPending = false;
    }
    else if (line.find("lost the forward") != std::string::npos)
    {
      st.publicAddress.clear();
      st.portmapNote = text;
      st.portmapPending = true;
    }
    else
    {
      st.publicAddress.clear();
      st.portmapNote = text;
      st.portmapPending = false;
    }
  }
  else if (word_after(line, "connection ", &id, &rest))
  {
    // "connection 3 from 203.0.113.9": the address, for when it logs in
    if (connections && starts_with(rest, "from ")) (*connections)[id] = rest.substr(5);
  }
  else if (word_after(line, "login ", &id, &rest))
  {
    // "login 3 as Ana privs=cv": someone joined. From an address on the internet, the port is
    // open, whatever the router said.
    if (connections)
    {
      auto it = connections->find(id);
      if (it != connections->end())
      {
        if (isPublicIPv4(it->second)) st.joinedFromInternet = true;
        connections->erase(it);
      }
    }
  }
  else if (starts_with_nocase(line, "error") || starts_with_nocase(line, "warning"))
  {
    *show = true;
  }
}

void HostServer::onLog(const std::string &text)
{
  std::string line = text;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return;

  std::lock_guard<std::mutex> lock(m_mutex);
  bool show = false;
  parseLogLine(line, m_state, &show, &m_connections);
  if (m_connections.size() > 64) m_connections.clear(); // connections that never logged in
  if (show)
  {
    m_messages.push_back(line);
    if (m_messages.size() > MAX_MESSAGES) m_messages.erase(m_messages.begin());
    m_serial ++;
  }
}

// ---------------------------------------------------------------------------
// The public address, from a "what's my IP" service
// ---------------------------------------------------------------------------

// four numbers from 0 to 255 with dots between, and nothing else
static bool parse_ipv4(const std::string &t, int b[4])
{
  int parts = 0, value = -1;
  for (char c : t)
  {
    if (c >= '0' && c <= '9')
    {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) return false;
    }
    else if (c == '.' && value >= 0 && parts < 3)
    {
      b[parts ++] = value;
      value = -1;
    }
    else return false;
  }
  if (parts != 3 || value < 0) return false;
  b[3] = value;
  return true;
}

bool HostServer::parseWhatsMyIpReply(const std::string &reply, std::string *ip)
{
  if (reply.compare(0, 5, "HTTP/") != 0) return false;
  const size_t code = reply.find(' ');
  if (code == std::string::npos || reply.compare(code + 1, 4, "200 ") != 0) return false;
  const size_t body = reply.find("\r\n\r\n");
  if (body == std::string::npos) return false;
  std::string t = reply.substr(body + 4);
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  int b[4];
  if (!parse_ipv4(t, b)) return false;
  *ip = t;
  return true;
}

bool HostServer::isPublicIPv4(const std::string &addr)
{
  int b[4];
  if (!parse_ipv4(addr, b)) return false;
  return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||   // this network, private, loopback, multicast
           (b[0] == 100 && b[1] >= 64 && b[1] < 128) ||               // the provider's NAT (100.64/10)
           (b[0] == 169 && b[1] == 254) ||                            // link-local
           (b[0] == 172 && b[1] >= 16 && b[1] < 32) ||                // private
           (b[0] == 192 && b[1] == 168));                             // private
}

#ifdef _WIN32
typedef SOCKET socket_t;
static const socket_t NO_SOCKET = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool connect_in_progress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
typedef int socket_t;
static const socket_t NO_SOCKET = -1;
static void close_socket(socket_t s) { close(s); }
static bool connect_in_progress() { return errno == EINPROGRESS; }
#endif

// waits until s can be written to (or read from), at most until deadline or until *stop
static bool wait_socket(socket_t s, bool write, std::chrono::steady_clock::time_point deadline, const std::atomic<bool> *stop)
{
  for (;;)
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0 || *stop) return false;
    const int slice = (int)std::min<long long>(left, 100); // so stop() never waits long
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = slice * 1000;
    const int r = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
#else
    // poll, not select: a host can have more files open than an fd_set holds
    pollfd p;
    p.fd = s;
    p.events = write ? POLLOUT : POLLIN;
    p.revents = 0;
    const int r = poll(&p, 1, slice);
#endif
    if (r != 0) return r > 0;
  }
}

// A plain HTTP GET (the services answer in a few hundred bytes): the reply, or "" within
// timeoutMs. Non-blocking, so a stalled service holds nothing up for longer.
static std::string http_get(const std::string &host, int port, const std::string &path, int timeoutMs, const std::atomic<bool> *stop)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  addrinfo hints, *res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return "";
  std::string reply;
  const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s != NO_SOCKET)
  {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
#ifdef SO_NOSIGPIPE
    int one = 1; // macOS: a write to a closed connection mustn't kill the host
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef MSG_NOSIGNAL
    const int sendFlags = MSG_NOSIGNAL; // Linux: the same
#else
    const int sendFlags = 0;
#endif
    bool ok = ::connect(s, res->ai_addr, (int)res->ai_addrlen) == 0 || (connect_in_progress() && wait_socket(s, true, deadline, stop));
    if (ok)
    {
      int err = 0;
      socklen_t len = sizeof(err);
      ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0;
    }
    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: Jamsa\r\nAccept: text/plain\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (ok && sent < request.size())
    {
      if (!wait_socket(s, true, deadline, stop)) ok = false;
      else
      {
        const int n = (int)send(s, request.data() + sent, (int)(request.size() - sent), sendFlags);
        if (n <= 0) ok = false;
        else sent += (size_t)n;
      }
    }
    while (ok && reply.size() < 8192 && wait_socket(s, false, deadline, stop))
    {
      char buf[1024];
      const int n = (int)recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break; // closed: the whole reply is in
      reply.append(buf, (size_t)n);
    }
    close_socket(s);
  }
  freeaddrinfo(res);
  return reply;
}

std::string HostServer::whatsMyIp(const std::string &service, const std::atomic<bool> *stop)
{
  struct Service { std::string host; int port; std::string path; };
  std::vector<Service> services;
  if (!service.empty())
  {
    // "host:port/path", for the tests; "off" asks nobody
    if (service != "off")
    {
      const size_t slash = service.find('/'), colon = service.find(':');
      Service sv;
      sv.host = service.substr(0, (std::min)(colon, slash));
      sv.port = colon < slash ? atoi(service.substr(colon + 1, slash - colon - 1).c_str()) : 80;
      sv.path = slash != std::string::npos ? service.substr(slash) : "/";
      services.push_back(sv);
    }
  }
  else services = { { "api.ipify.org", 80, "/" }, { "checkip.amazonaws.com", 80, "/" } };

  std::string ip;
  for (const Service &sv : services)
  {
    if (*stop) break;
    if (parseWhatsMyIpReply(http_get(sv.host, sv.port, sv.path, 4000, stop), &ip)) break;
    ip.clear();
  }
  return ip;
}

void HostServer::lookUpPublicAddress(int port)
{
#ifdef _WIN32
  WSADATA wsa;
  const bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
  const char *env = getenv("JAMSA_WHATSMYIP");
  const std::string ip = whatsMyIp(env ? env : "", &m_lookupStop);
#ifdef _WIN32
  if (wsaOk) WSACleanup();
#endif
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state.port != port) return; // stopped meanwhile
  if (!ip.empty()) m_state.lookedUpAddress = ip + ":" + std::to_string(port);
  m_state.lookupPending = false;
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
