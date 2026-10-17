// o11proto - IPv4 UDP sockets for Live mode. Internal: included by the
// relay (server_live.cpp), the client transport (live_transport.cpp) and the
// Live tests only.
//
// The relay must answer every datagram from the local address it arrived at
// (a client's connect()ed socket drops anything else). Linux reports and sets
// that address with IP_PKTINFO, the BSDs and macOS report it with
// IP_RECVDSTADDR and set it with IP_SENDSRCADDR where that exists. Anywhere
// else (Windows) the routing table chooses, which is right on a machine with
// one address.
#ifndef O11PROTO_UDP_COMPAT_H
#define O11PROTO_UDP_COMPAT_H

#include <cstdint>
#include <cstring>

#include "socket_compat.h"

#ifndef _WIN32
#include <sys/uio.h>
#endif

#if defined(__linux__) && defined(IP_PKTINFO)
#define O11_UDP_PKTINFO 1
#elif defined(IP_RECVDSTADDR)
#define O11_UDP_RECVDSTADDR 1
#endif

namespace o11proto {
namespace udp {

// A datagram's addresses. IPv4 addresses and ports in network order.
struct Addr {
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
};

inline sockaddr_in toSockaddr(const Addr& a) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = a.ip;
    sa.sin_port = a.port;
    return sa;
}

inline bool isBroadcastOrMulticast(std::uint32_t ip_net) {
    std::uint32_t h = ntohl(ip_net);
    return h == 0xFFFFFFFFu || (h >> 28) == 0xE;
}

// Makes an IPv4 datagram socket, non-blocking, with both buffers at `buffer`
// bytes, without SIGPIPE where the system has SO_NOSIGPIPE, and (on Windows)
// without the ICMP "port unreachable" reset. With `exclusive`, a second bind
// of the same port fails on Windows too.
inline socket_t open(int buffer, bool exclusive) {
    if (!compat::startup()) return kInvalidSocket;
    socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) return fd;
#ifdef _WIN32
    if (exclusive) {
        int one = 1;
        compat::setOpt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, sizeof(one));
    }
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL off = FALSE;
    DWORD got = 0;
    ::WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
#else
    (void)exclusive;
#endif
    compat::setOpt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    compat::setOpt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    compat::noSigpipe(fd);                      // a send while another thread closes it must not end the process
    compat::setNonBlocking(fd);
    return fd;
}

// Asks for each datagram's local destination address. False where the
// system can't report it.
inline bool reportLocalAddress(socket_t fd) {
    int one = 1;
#if defined(O11_UDP_PKTINFO)
    return compat::setOpt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) == 0;
#elif defined(O11_UDP_RECVDSTADDR)
    return compat::setOpt(fd, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one)) == 0;
#else
    (void)fd;
    (void)one;
    return false;
#endif
}

enum class Recv { Datagram, Empty, Error };

// Reads one datagram into buf. `len` is its length; `truncated` is set when
// it was longer than `cap` (always pass a buffer longer than anything you
// accept, so truncation means "too big"). `local` is filled when the system
// reports the destination address.
inline Recv receive(socket_t fd, std::uint8_t* buf, std::size_t cap, std::size_t& len, bool& truncated,
                    Addr& from, Addr& local, bool& has_local) {
    truncated = false;
    has_local = false;
    sockaddr_in sa{};
#ifdef _WIN32
    (void)local;
    int salen = sizeof(sa);
    int n = ::recvfrom(fd, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                       reinterpret_cast<sockaddr*>(&sa), &salen);
    if (n < 0) {
        int err = compat::lastError();
        if (err == WSAEMSGSIZE) {
            truncated = true;
            len = cap;
        } else if (compat::isWouldBlock(err)) {
            return Recv::Empty;
        } else {
            return Recv::Error;                    // WSAECONNRESET and the like: skip it
        }
    } else {
        len = static_cast<std::size_t>(n);
    }
#else
    iovec iov;
    iov.iov_base = buf;
    iov.iov_len = cap;
    alignas(cmsghdr) char ctrl[64];
    msghdr mh{};
    mh.msg_name = &sa;
    mh.msg_namelen = sizeof(sa);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof(ctrl);
    ssize_t n = ::recvmsg(fd, &mh, MSG_DONTWAIT);
    if (n < 0) {
        int err = compat::lastError();
        if (compat::isWouldBlock(err) || compat::isInterrupted(err)) return Recv::Empty;
        return Recv::Error;
    }
    len = static_cast<std::size_t>(n);
    truncated = (mh.msg_flags & MSG_TRUNC) != 0;
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
#if defined(O11_UDP_PKTINFO)
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
            in_pktinfo pi;
            std::memcpy(&pi, CMSG_DATA(c), sizeof(pi));
            local.ip = pi.ipi_addr.s_addr;
            has_local = true;
        }
#elif defined(O11_UDP_RECVDSTADDR)
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVDSTADDR) {
            in_addr a;
            std::memcpy(&a, CMSG_DATA(c), sizeof(a));
            local.ip = a.s_addr;
            has_local = true;
        }
#endif
    }
#endif
    from.ip = sa.sin_addr.s_addr;
    from.port = sa.sin_port;
    return Recv::Datagram;
}

// Sends one datagram to `to`, from the local address `local` when
// `use_local` (and the system can choose it). Returns 0 or the error code.
inline int sendTo(socket_t fd, const std::uint8_t* buf, std::size_t len, const Addr& to, std::uint32_t local,
                  bool use_local) {
    sockaddr_in sa = toSockaddr(to);
#if !defined(_WIN32) && (defined(O11_UDP_PKTINFO) || (defined(O11_UDP_RECVDSTADDR) && defined(IP_SENDSRCADDR)))
    if (use_local && !isBroadcastOrMulticast(local)) {
        iovec iov;
        iov.iov_base = const_cast<std::uint8_t*>(buf);
        iov.iov_len = len;
        alignas(cmsghdr) char ctrl[64];
        std::memset(ctrl, 0, sizeof(ctrl));
        msghdr mh{};
        mh.msg_name = &sa;
        mh.msg_namelen = sizeof(sa);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctrl;
#if defined(O11_UDP_PKTINFO)
        mh.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_PKTINFO;
        c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        in_pktinfo pi{};
        pi.ipi_spec_dst.s_addr = local;
        std::memcpy(CMSG_DATA(c), &pi, sizeof(pi));
#else
        mh.msg_controllen = CMSG_SPACE(sizeof(in_addr));
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_SENDSRCADDR;
        c->cmsg_len = CMSG_LEN(sizeof(in_addr));
        in_addr a{};
        a.s_addr = local;
        std::memcpy(CMSG_DATA(c), &a, sizeof(a));
#endif
        if (::sendmsg(fd, &mh, MSG_NOSIGNAL) >= 0) return 0;
        return compat::lastError();
    }
#else
    (void)local;
    (void)use_local;
#endif
#ifdef _WIN32
    int n = ::sendto(fd, reinterpret_cast<const char*>(buf), static_cast<int>(len), 0,
                     reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#else
    ssize_t n = ::sendto(fd, buf, len, MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#endif
    return n >= 0 ? 0 : compat::lastError();
}

// The error a send from a local address gives when that address no longer
// belongs to the machine.
inline bool isAddressGone(int err) {
#ifdef _WIN32
    return err == WSAEINVAL || err == WSAEADDRNOTAVAIL;
#else
    return err == EINVAL || err == EADDRNOTAVAIL;
#endif
}

// Parses a dotted IPv4 address into network order. False if it isn't one.
inline bool parseIPv4(const std::string& s, std::uint32_t& out) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    out = a.s_addr;
    return true;
}

}  // namespace udp
}  // namespace o11proto

#endif  // O11PROTO_UDP_COMPAT_H
