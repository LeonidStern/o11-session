// o11proto - the socket layer: standalone Asio, and the few platform bits
// Asio leaves to us. Internal: included by the library sources and the tests
// that play raw peers.
//
// The library's sockets, waits and timers are Asio's (one io_context for each
// client, server and Live transport, run on the library's own threads or the
// caller's). Asio starts Winsock, makes every socket it opens or accepts
// SIGPIPE-free (MSG_NOSIGNAL on its sends, SO_NOSIGPIPE where that exists)
// and maps the platform's error codes. What stays here: the Asio headers in
// one place, Connection's socket, SIGPIPE for a socket adopted by its handle,
// the listening socket's address reuse, and the raw calls the tests' peers use.
//
// On POSIX every raw helper forwards to the usual call. On Windows (MSVC or
// MinGW) the same names map to Winsock: SOCKET handles, closesocket(),
// WSAPoll(), and WSAGetLastError() in place of errno.
#ifndef O11PROTO_SOCKET_COMPAT_H
#define O11PROTO_SOCKET_COMPAT_H

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

// Asio (ASIO_STANDALONE and the rest come from the o11proto_asio target). Its
// headers come first: on Windows they include winsock2.h before windows.h.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wcomma"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#if !defined(_WIN32)
#include <asio/posix/stream_descriptor.hpp>
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "o11proto/connection.h"   // socket_t, kInvalidSocket

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

// The raw sends below never raise SIGPIPE either: MSG_NOSIGNAL where the flag
// exists (Linux, FreeBSD, newer macOS SDKs), SO_NOSIGPIPE on the socket where
// that exists (noSigpipe()). Windows has no SIGPIPE, and there the flag is 0.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
// Every socket here is non-blocking already, so where the flag is missing
// (Windows) dropping it changes nothing.
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

namespace o11proto {

// Connection's socket (connection.h): an Asio socket on its owner's
// io_context, or, for a socket adopted by its handle, on one of its own.
struct Connection::Net {
    std::unique_ptr<asio::io_context> own;     // declared first: it outlives the socket
    asio::ip::tcp::socket socket;
    explicit Net(asio::ip::tcp::socket&& s) : socket(std::move(s)) {}
    Net(std::unique_ptr<asio::io_context> io, asio::ip::tcp::socket&& s) : own(std::move(io)), socket(std::move(s)) {}
};

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

// A wait or read that would block, in Asio's terms.
inline bool wouldBlock(const asio::error_code& ec) {
    return ec == asio::error::would_block || ec == asio::error::try_again;
}

// The address of an endpoint as text: "1.2.3.4", an IPv4-mapped IPv6
// address (::ffff:1.2.3.4) as plain IPv4, other IPv6 addresses as such.
inline std::string addressText(const asio::ip::address& a) {
    if (a.is_v6() && a.to_v6().is_v4_mapped())
        return asio::ip::make_address_v4(asio::ip::v4_mapped, a.to_v6()).to_string();
    return a.to_string();
}

// Winsock startup for the raw calls (Asio starts it for its own sockets); a
// no-op elsewhere. WSACleanup is deliberately never called: the library has
// no shutdown point and the OS reclaims everything at exit.
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
// Windows does not keep the port busy after close the way POSIX does. (Asio's
// reuse_address is SO_REUSEADDR everywhere, which is why this is ours.)
inline void setListenReuse(socket_t s) {
    int one = 1;
#ifdef _WIN32
    setOpt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, sizeof(one));
#else
    setOpt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

// Sets SO_NOSIGPIPE where it exists, for systems whose send() takes no
// MSG_NOSIGNAL; harmless where both exist, a no-op where neither does. Asio
// does this for the sockets it makes; Connection::adopt() for one adopted by
// its handle.
inline void noSigpipe(socket_t s) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setOpt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)s;
#endif
}

// ------------------------------------------------------------- raw calls
// For the native calls Asio has no equivalent of (the relay's recvmsg and
// sendmsg with their local address) and for the tests' raw peers.

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

// recv/send: Windows wants char* buffers and int lengths.
inline ssize_type recv(socket_t s, void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    if (len > static_cast<std::size_t>(INT_MAX)) len = INT_MAX;
    return ::recv(s, static_cast<char*>(buf), static_cast<int>(len), flags);
#else
    return ::recv(s, buf, len, flags);
#endif
}
// MSG_NOSIGNAL is always added: no send through here can raise SIGPIPE.
inline ssize_type send(socket_t s, const void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    if (len > static_cast<std::size_t>(INT_MAX)) len = INT_MAX;
    return ::send(s, static_cast<const char*>(buf), static_cast<int>(len), flags);
#else
    return ::send(s, buf, len, flags | MSG_NOSIGNAL);
#endif
}

// poll(): WSAPoll shares the pollfd layout and the POLLIN/POLLOUT/POLLERR/
// POLLHUP names. The library waits with Asio; this is for the tests.
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

// The error of the last raw socket call on this thread.
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
}  // namespace o11proto

#endif  // O11PROTO_SOCKET_COMPAT_H
