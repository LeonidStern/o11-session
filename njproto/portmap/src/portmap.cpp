// njproto - router port mapping, implemented from PORTMAP.md revision 1.
//
// One thread (section 4) maps the port (section 5), waits for the renewal
// time or the retry time, and removes the mapping (section 6) when told to
// stop. UPnP-IGD goes through miniupnpc, NAT-PMP through libnatpmp.
#include "njproto/portmap.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <natpmp.h>

#if MINIUPNPC_API_VERSION < 17
#error "njproto_portmap needs miniupnpc API version 17 or later"
#endif

namespace njproto {

namespace {

// Section 3.
constexpr unsigned kLeaseSeconds = 3600;
constexpr int kMinRenewalSeconds = 30;
constexpr int kPermanentRecheckSeconds = 1800;
constexpr int kRetrySeconds = 300;
constexpr int kDiscoveryMs = 2000;
constexpr int kNatpmpWaitMs = 3000;
constexpr int kNatpmpRemoveWaitMs = 2000;
constexpr int kPollStepMs = 250;
constexpr const char* kDescription = "NINJAM server";
constexpr const char* kProtocol = "TCP";

constexpr const char* kUpnpTypes[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    nullptr,
};

enum class Method { None, Upnp, Natpmp };

// Winsock must be up before libnatpmp or miniupnpc open a socket; a no-op
// elsewhere. Never torn down: the process end reclaims it.
void socketsStartup() {
#ifdef _WIN32
    struct Once {
        Once() {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
        }
    };
    static Once once;
#endif
}

const char* envString(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// The NAT-PMP result code of RFC 6886 behind a libnatpmp error, or the
// library's own (negative) code when it is not a router answer.
int natpmpErrorCode(int r) {
    switch (r) {
        case NATPMP_ERR_UNSUPPORTEDVERSION: return 1;
        case NATPMP_ERR_NOTAUTHORIZED: return 2;
        case NATPMP_ERR_NETWORKFAILURE: return 3;
        case NATPMP_ERR_OUTOFRESOURCES: return 4;
        case NATPMP_ERR_UNSUPPORTEDOPCODE: return 5;
        default: return r;
    }
}

// A router answer that says no, as opposed to no answer at all.
bool natpmpIsRefusal(int r) {
    return r == NATPMP_ERR_UNSUPPORTEDVERSION || r == NATPMP_ERR_UNSUPPORTEDOPCODE ||
           r == NATPMP_ERR_UNDEFINEDERROR || r == NATPMP_ERR_NOTAUTHORIZED ||
           r == NATPMP_ERR_NETWORKFAILURE || r == NATPMP_ERR_OUTOFRESOURCES;
}

}  // namespace

bool isPrivateIPv4(const std::string& dotted) {
    unsigned octet[4] = {0, 0, 0, 0};
    int n = 0;
    std::size_t i = 0;
    while (n < 4) {
        if (i >= dotted.size() || dotted[i] < '0' || dotted[i] > '9') return true;
        unsigned v = 0;
        std::size_t digits = 0;
        while (i < dotted.size() && dotted[i] >= '0' && dotted[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(dotted[i] - '0');
            if (v > 255 || ++digits > 3) return true;
            ++i;
        }
        octet[n++] = v;
        if (n < 4) {
            if (i >= dotted.size() || dotted[i] != '.') return true;
            ++i;
        }
    }
    if (i != dotted.size()) return true;
    const unsigned a = octet[0], b = octet[1];
    if (a == 10) return true;                       // 10/8
    if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16/12
    if (a == 192 && b == 168) return true;          // 192.168/16
    if (a == 100 && b >= 64 && b <= 127) return true;  // 100.64/10
    if (a == 127) return true;                      // 127/8
    if (a == 169 && b == 254) return true;          // 169.254/16
    return false;
}

struct PortMapper::Impl {
    std::function<void(const std::string&)> log;
    int port = 0;

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;

    // State of the active method.
    Method method = Method::None;
    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {0};
    bool upnpPermanentOnly = false;
    in_addr_t natpmpGateway = 0;
    bool natpmpForceGateway = false;
    unsigned natpmpPublicPort = 0;

    void emit(const std::string& line) {
        if (log) log("[portmap] " + line);
    }

    bool stopRequested() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping;
    }

    // Sleeps for the given time unless stop() wakes the thread first.
    void waitFor(int seconds) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping; });
    }

    // ---------------------------------------------------------------- UPnP

    // Finds a usable gateway and fills urls/data/lanaddr. Section 5.
    bool upnpFind() {
        if (const char* rootdesc = envString("NINJAM_UPNP_ROOTDESC")) {
            if (UPNP_GetIGDFromUrl(rootdesc, &urls, &data, lanaddr, sizeof(lanaddr)) != 1) return false;
            if (!UPNPIGD_IsConnected(&urls, &data)) {
                FreeUPNPUrls(&urls);
                return false;
            }
            return true;
        }
        int error = 0;
        UPNPDev* devlist = upnpDiscoverDevices(kUpnpTypes, kDiscoveryMs, nullptr, nullptr,
                                               UPNP_LOCAL_PORT_ANY, 0, 2, &error, 1);
        if (!devlist) return false;
#if MINIUPNPC_API_VERSION >= 18
        char wanaddr[64] = {0};
        int r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
        // 1: connected, public address; 2: connected, private address (still usable).
        bool usable = (r == 1 || r == 2);
#else
        int r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
        bool usable = (r == 1);   // 1: connected; 2: not connected; 3: not an IGD
#endif
        freeUPNPDevlist(devlist);
        if (r != 0 && !usable) FreeUPNPUrls(&urls);
        return usable;
    }

    // AddPortMapping on the gateway held in urls/data. Returns the lease or -1.
    int upnpAdd() {
        const std::string portStr = std::to_string(port);
        int r = UPNPCOMMAND_UNKNOWN_ERROR;
        if (!upnpPermanentOnly) {
            r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portStr.c_str(), portStr.c_str(),
                                    lanaddr, kDescription, kProtocol, nullptr,
                                    std::to_string(kLeaseSeconds).c_str());
            if (r == 725) upnpPermanentOnly = true;   // OnlyPermanentLeasesSupported
        }
        if (upnpPermanentOnly)
            r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portStr.c_str(), portStr.c_str(),
                                    lanaddr, kDescription, kProtocol, nullptr, "0");
        if (r != UPNPCOMMAND_SUCCESS) {
            if (r == 718)
                emit("UPnP: TCP port " + portStr + " on the router is already forwarded to another machine");
            else
                emit("UPnP: router refused to forward TCP port " + portStr + " (error " + std::to_string(r) + ")");
            return -1;
        }
        char external[64] = {0};
        std::string addr;
        if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external) == UPNPCOMMAND_SUCCESS)
            addr = external;
        report("UPnP", addr, port);
        return upnpPermanentOnly ? 0 : static_cast<int>(kLeaseSeconds);
    }

    void upnpRemove() {
        const std::string portStr = std::to_string(port);
        int r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, portStr.c_str(), kProtocol, nullptr);
        if (r == UPNPCOMMAND_SUCCESS)
            emit("UPnP: removed the forward for TCP port " + portStr);
        else
            emit("UPnP: could not remove the forward for TCP port " + portStr + " (error " + std::to_string(r) + ")");
        FreeUPNPUrls(&urls);
        std::memset(&urls, 0, sizeof(urls));
    }

    // ------------------------------------------------------------- NAT-PMP

    bool natpmpGatewayFromEnv() {
        natpmpForceGateway = false;
        natpmpGateway = 0;
        if (const char* gw = envString("NINJAM_NATPMP_GATEWAY")) {
            in_addr a{};
            if (inet_pton(AF_INET, gw, &a) != 1) return false;
            natpmpGateway = a.s_addr;
            natpmpForceGateway = true;
        }
        return true;
    }

    // Waits for the answer to the pending request for up to waitMs, calling
    // readnatpmpresponseorretry every 250 ms (it retransmits on its own
    // schedule). With checkStop, gives up when stop() was called. Returns the
    // library result, or NATPMP_TRYAGAIN when the time ran out.
    int natpmpWait(natpmp_t& np, natpmpresp_t& resp, int waitMs, bool checkStop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        for (;;) {
            if (checkStop && stopRequested()) return NATPMP_TRYAGAIN;
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return NATPMP_TRYAGAIN;
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            int step = static_cast<int>(left < kPollStepMs ? left : kPollStepMs);
            fd_set fds;
            FD_ZERO(&fds);
#ifdef _WIN32
            FD_SET(static_cast<SOCKET>(np.s), &fds);
#else
            FD_SET(np.s, &fds);
#endif
            timeval tv{};
            tv.tv_sec = step / 1000;
            tv.tv_usec = (step % 1000) * 1000;
            select(np.s + 1, &fds, nullptr, nullptr, &tv);
            int r = readnatpmpresponseorretry(&np, &resp);
            if (r != NATPMP_TRYAGAIN) return r;
        }
    }

    // Sends a TCP mapping request and waits for the answer. Returns the
    // granted lifetime, or -1 (silently while probing).
    int natpmpAdd(bool renewing) {
        const std::string portStr = std::to_string(port);
        if (!natpmpGatewayFromEnv()) return -1;
        natpmp_t np;
        if (initnatpmp(&np, natpmpForceGateway ? 1 : 0, natpmpGateway) != 0) return -1;
        unsigned publicPort = renewing ? natpmpPublicPort : static_cast<unsigned>(port);
        int r = sendnewportmappingrequest(&np, NATPMP_PROTOCOL_TCP, static_cast<uint16_t>(port),
                                          static_cast<uint16_t>(publicPort), kLeaseSeconds);
        natpmpresp_t resp{};
        if (r >= 0) r = natpmpWait(np, resp, kNatpmpWaitMs, true);
        if (r < 0 || resp.type != NATPMP_RESPTYPE_TCPPORTMAPPING) {
            closenatpmp(&np);
            if (renewing && !stopRequested()) {
                if (natpmpIsRefusal(r))
                    emit("NAT-PMP: router refused to forward TCP port " + portStr + " (error " +
                         std::to_string(natpmpErrorCode(r)) + ")");
                else
                    emit("NAT-PMP: router did not answer");
            }
            return -1;
        }
        natpmpPublicPort = resp.pnu.newportmapping.mappedpublicport;
        const int lifetime = static_cast<int>(resp.pnu.newportmapping.lifetime);

        std::string addr;
        natpmpresp_t addrResp{};
        if (sendpublicaddressrequest(&np) >= 0 &&
            natpmpWait(np, addrResp, kNatpmpWaitMs, true) >= 0 &&
            addrResp.type == NATPMP_RESPTYPE_PUBLICADDRESS) {
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &addrResp.pnu.publicaddress.addr, buf, sizeof(buf))) addr = buf;
        }
        closenatpmp(&np);
        report("NAT-PMP", addr, static_cast<int>(natpmpPublicPort));
        return lifetime;
    }

    void natpmpRemove() {
        const std::string portStr = std::to_string(natpmpPublicPort);
        natpmp_t np;
        int r = NATPMP_ERR_SOCKETERROR;
        if (natpmpGatewayFromEnv() && initnatpmp(&np, natpmpForceGateway ? 1 : 0, natpmpGateway) == 0) {
            r = sendnewportmappingrequest(&np, NATPMP_PROTOCOL_TCP, static_cast<uint16_t>(port),
                                          static_cast<uint16_t>(natpmpPublicPort), 0);
            natpmpresp_t resp{};
            if (r >= 0) r = natpmpWait(np, resp, kNatpmpRemoveWaitMs, false);
            closenatpmp(&np);
        }
        if (r >= 0)
            emit("NAT-PMP: removed the forward for TCP port " + portStr);
        else
            emit("NAT-PMP: could not remove the forward for TCP port " + portStr + " (error " +
                 std::to_string(natpmpErrorCode(r)) + ")");
    }

    // ------------------------------------------------------------ sections 5-7

    void report(const std::string& how, const std::string& addr, int externalPort) {
        const std::string p = std::to_string(externalPort);
        if (addr.empty() || addr == "0.0.0.0")
            emit(how + ": router forwards TCP port " + p + " to this machine, but did not say its public address");
        else if (isPrivateIPv4(addr))
            emit(how + ": router forwards TCP port " + p + " to this machine, but the router's own internet address is " +
                 addr + ", which is not public. Your internet provider probably uses carrier-grade NAT, or another "
                 "router sits in front of this one, so people outside your network still can't connect. Ask your "
                 "provider for a public IPv4 address, or run the server on a hosted machine.");
        else
            emit(how + ": people can connect to " + addr + ":" + p);
    }

    // Section 5: lease in seconds, 0 for permanent, -1 on failure.
    int map() {
        if (method == Method::Upnp) return upnpAdd();
        if (method == Method::Natpmp) return natpmpAdd(true);

        upnpPermanentOnly = false;
        if (upnpFind()) {
            int lease = upnpAdd();
            if (lease >= 0) {
                method = Method::Upnp;
                return lease;
            }
            FreeUPNPUrls(&urls);
            std::memset(&urls, 0, sizeof(urls));
        }
        if (stopRequested()) return -1;
        int lease = natpmpAdd(false);
        if (lease >= 0) {
            method = Method::Natpmp;
            return lease;
        }
        return -1;
    }

    // Section 6.
    void remove() {
        if (method == Method::Upnp) upnpRemove();
        else if (method == Method::Natpmp) natpmpRemove();
        method = Method::None;
    }

    // Section 4.
    void run() {
        const std::string portStr = std::to_string(port);
        emit("asking the router to forward TCP port " + portStr + " to this machine");
        bool warned = false;
        while (!stopRequested()) {
            const int lease = map();
            if (stopRequested()) break;
            if (lease < 0) {
                if (method != Method::None) {
                    emit("lost the forward for TCP port " + portStr + ", trying again");
                    remove();
                    continue;
                }
                if (!warned) {
                    emit("could not find a router that supports UPnP or NAT-PMP. Forward TCP port " + portStr +
                         " on your router to this machine by hand, or turn on UPnP in the router's settings. "
                         "Will keep trying every 5 minutes.");
                    warned = true;
                }
                waitFor(kRetrySeconds);
                continue;
            }
            warned = false;
            waitFor(renewalSeconds(lease));
        }
        remove();
    }

    // Half the lease, at least 30 s, or the recheck time for a permanent
    // lease. The 30 s floor is not applied to a lease shorter than 60 s,
    // where it would let the mapping expire before the renewal (NOTES.md).
    static int renewalSeconds(int lease) {
        if (lease == 0) return kPermanentRecheckSeconds;
        int half = lease / 2;
        if (lease >= 2 * kMinRenewalSeconds && half < kMinRenewalSeconds) half = kMinRenewalSeconds;
        return half < 1 ? 1 : half;
    }
};

PortMapper::PortMapper() : impl_(new Impl) {}

PortMapper::~PortMapper() { stop(); }

void PortMapper::start(int port) {
    stop();
    socketsStartup();
    impl_->log = log;
    impl_->port = port;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = false;
    }
    impl_->thread = std::thread([this] { impl_->run(); });
}

void PortMapper::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
}

}  // namespace njproto
