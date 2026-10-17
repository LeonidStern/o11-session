// njproto - the thin layer between the sockets code and the platform.
// Internal: included by the library sources and the loopback test only.
//
// On POSIX every helper forwards to the usual call, so the behaviour there
// is unchanged. On Windows (MSVC or MinGW) the same names map to Winsock:
// SOCKET handles, closesocket(), ioctlsocket(FIONBIO), WSAPoll(), and
// WSAGetLastError() in place of errno. Winsock is started once, the first
// time a socket is made, and stays up for the life of the process.
#ifndef NJPROTO_SOCKET_COMPAT_H
#define NJPROTO_SOCKET_COMPAT_H

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>

#include "njproto/connection.h"   // socket_t, kInvalidSocket

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// send() must not raise SIGPIPE; Windows (and macOS) have no such flag and
// never raise it, so the flag is simply absent there.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
// Every socket here is non-blocking already, so where the flag is missing
// (Windows) dropping it changes nothing.
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

namespace njproto {
namespace compat {

#ifdef _WIN32
static_assert(std::is_same<socket_t, SOCKET>::value, "socket_t must be SOCKET on Windows");
static_assert(kInvalidSocket == INVALID_SOCKET, "kInvalidSocket must be INVALID_SOCKET");
using ssize_type = int;
using poll_count = ULONG;
#else
static_assert(std::is_same<socket_t, int>::value, "socket_t must be int on POSIX");
using ssize_type = ssize_t;
using poll_count = nfds_t;
#endif

// One-time Winsock startup; a no-op elsewhere. Call before the first
// socket()/getaddrinfo(). WSACleanup is deliberately never called: the
// library has no shutdown point and the OS reclaims everything at exit.
inline bool startup() {
#ifdef _WIN32
    struct Once {
        bool ok;
        Once() {
            WSADATA wsa;
            ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }
    };
    static Once once;
    return once.ok;
#else
    return true;
#endif
}

inline void closeSocket(socket_t s) {
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

inline bool setNonBlocking(socket_t s) {
#ifdef _WIN32
    u_long one = 1;
    return ::ioctlsocket(s, FIONBIO, &one) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// setsockopt/getsockopt take char* on Windows and void* on POSIX; a char*
// converts to both.
inline int setOpt(socket_t s, int level, int name, const void* val, socklen_t len) {
    return ::setsockopt(s, level, name, static_cast<const char*>(val), len);
}
inline int getOpt(socket_t s, int level, int name, void* val, socklen_t* len) {
    return ::getsockopt(s, level, name, static_cast<char*>(val), len);
}

// Lets a listening socket reuse a port in TIME_WAIT (POSIX). On Windows
// SO_REUSEADDR would let a second server bind the very same port while the
// first is still listening, so SO_EXCLUSIVEADDRUSE is used there instead;
// Windows does not keep the port busy after close the way POSIX does.
inline void setListenReuse(socket_t s) {
    int one = 1;
#ifdef _WIN32
    setOpt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, sizeof(one));
#else
    setOpt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

// recv/send: Windows wants char* buffers and int lengths.
inline ssize_type recv(socket_t s, void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    if (len > static_cast<std::size_t>(INT_MAX)) len = INT_MAX;
    return ::recv(s, static_cast<char*>(buf), static_cast<int>(len), flags);
#else
    return ::recv(s, buf, len, flags);
#endif
}
inline ssize_type send(socket_t s, const void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    if (len > static_cast<std::size_t>(INT_MAX)) len = INT_MAX;
    return ::send(s, static_cast<const char*>(buf), static_cast<int>(len), flags);
#else
    return ::send(s, buf, len, flags);
#endif
}

// poll(): WSAPoll shares the pollfd layout and the POLLIN/POLLOUT/POLLERR/
// POLLHUP names.
inline int poll(pollfd* fds, std::size_t n, int timeout_ms) {
#ifdef _WIN32
    return ::WSAPoll(fds, static_cast<poll_count>(n), timeout_ms);
#else
    return ::poll(fds, static_cast<poll_count>(n), timeout_ms);
#endif
}

// Windows only: WSAPoll on Windows before 10 version 2004 never reports a
// failed non-blocking connect (no POLLERR/POLLHUP), so the client also asks
// select()'s exceptfds, where Winsock has always reported it. Returns true
// and the error when the connect has failed; always false on POSIX, where
// poll() reports it.
inline bool connectFailed(socket_t s, int& err) {
#ifdef _WIN32
    fd_set ex;
    FD_ZERO(&ex);
    FD_SET(s, &ex);
    timeval zero{};
    if (::select(0, nullptr, nullptr, &ex, &zero) <= 0 || !FD_ISSET(s, &ex)) return false;
    err = 0;
    socklen_t len = sizeof(err);
    if (getOpt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err == 0) err = WSAECONNREFUSED;
    return true;
#else
    (void)s;
    (void)err;
    return false;
#endif
}

// The error of the last socket call on this thread.
inline int lastError() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// A non-blocking read/write that would block.
inline bool isWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// A non-blocking connect() that is still in progress.
inline bool isInProgress(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EINPROGRESS;
#endif
}

// A call interrupted by a signal (never on Windows in practice).
inline bool isInterrupted(int err) {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

// Text for a socket error code (one from lastError() or SO_ERROR).
inline std::string errorString(int err) {
#ifdef _WIN32
    char buf[256] = {0};
    DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, static_cast<DWORD>(err),
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf) - 1, nullptr);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    if (s.empty()) s = "socket error " + std::to_string(err);
    return s;
#else
    return std::strerror(err);
#endif
}

}  // namespace compat
}  // namespace njproto

#endif  // NJPROTO_SOCKET_COMPAT_H
