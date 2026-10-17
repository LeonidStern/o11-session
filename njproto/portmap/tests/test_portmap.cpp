// Acceptance tests of PORTMAP.md section 8 against the fake router in
// fake_router.py (started per scenario; it prints "READY <http-port>" and
// reports what it holds at GET /mappings).
//
// The build passes FAKE_ROUTER_PY (the script) and PYTHON3_EXE.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "njproto/portmap.h"
#include "socket_compat.h"

#ifdef _WIN32
#define NJ_POPEN _popen
#define NJ_PCLOSE _pclose
#else
#define NJ_POPEN popen
#define NJ_PCLOSE pclose
#endif

#ifndef FAKE_ROUTER_PY
#error "FAKE_ROUTER_PY must name fake_router.py"
#endif
#ifndef PYTHON3_EXE
#define PYTHON3_EXE "python3"
#endif

using namespace njproto;
using Clock = std::chrono::steady_clock;

static const int kPort = 2049;

static void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// A blocking HTTP/1.0 GET on 127.0.0.1; returns the body (empty on failure).
static std::string httpGet(int port, const std::string& path) {
    compat::startup();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return "";
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        if (compat::send(s, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<compat::ssize_type>(req.size())) {
            char buf[4096];
            for (;;) {
                compat::ssize_type n = compat::recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                out.append(buf, static_cast<std::size_t>(n));
            }
        }
    }
    compat::closeSocket(s);
    std::size_t p = out.find("\r\n\r\n");
    return p == std::string::npos ? "" : out.substr(p + 4);
}

// A TCP port nothing listens on: bind an ephemeral one and release it.
static int closedPort() {
    compat::startup();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 1;   // tcpmux: nothing listens there either
    if (s != kInvalidSocket && ::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
        socklen_t len = sizeof(sa);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len) == 0) port = ntohs(sa.sin_port);
    }
    if (s != kInvalidSocket) compat::closeSocket(s);
    return port;
}

struct FakeRouter {
    FILE* pipe = nullptr;
    int httpPort = 0;

    explicit FakeRouter(const std::string& scenario) {
        std::string cmd = std::string("\"") + PYTHON3_EXE + "\" -I \"" + FAKE_ROUTER_PY + "\" " + scenario;
#ifdef _WIN32
        // _popen runs the line through cmd.exe /c, which strips the first and last quote of a
        // line that starts with one; a second pair of quotes around the whole line keeps both paths
        cmd = "\"" + cmd + "\"";
#endif
        pipe = NJ_POPEN(cmd.c_str(), "r");
        if (!pipe) {
            std::printf("  cannot start %s\n", cmd.c_str());
            return;
        }
        char line[128] = {0};
        if (std::fgets(line, sizeof(line), pipe) && std::strncmp(line, "READY ", 6) == 0)
            httpPort = std::atoi(line + 6);
        else
            std::printf("  fake router did not report READY\n");
    }
    ~FakeRouter() {
        if (!pipe) return;
        if (httpPort) httpGet(httpPort, "/quit");
        NJ_PCLOSE(pipe);
    }
    bool ok() const { return pipe && httpPort; }
    std::string rootdesc() const { return "http://127.0.0.1:" + std::to_string(httpPort) + "/rootDesc.xml"; }
    std::vector<std::string> mappings() const {
        std::vector<std::string> lines;
        std::stringstream ss(httpGet(httpPort, "/mappings"));
        std::string l;
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) lines.push_back(l);
        }
        return lines;
    }
    int countMappings(const std::string& prefix) const {
        int n = 0;
        for (const auto& l : mappings())
            if (l.compare(0, prefix.size(), prefix) == 0) ++n;
        return n;
    }
};

struct Log {
    std::mutex m;
    std::vector<std::string> lines;

    void attach(PortMapper& pm) {
        pm.log = [this](const std::string& s) {
            std::lock_guard<std::mutex> lock(m);
            lines.push_back(s);
            std::printf("    %s\n", s.c_str());
        };
    }
    int count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(m);
        int n = 0;
        for (const auto& l : lines)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
    bool has(const std::string& needle) { return count(needle) > 0; }
    bool allPrefixed() {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& l : lines)
            if (l.compare(0, 10, "[portmap] ") != 0 || (!l.empty() && l.back() == '\n')) return false;
        return true;
    }
    bool waitFor(const std::string& needle, int timeoutMs) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < deadline) {
            if (has(needle)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return has(needle);
    }
};

// The gateway is always 127.0.0.1: scenarios without a NAT-PMP responder
// leave UDP 5351 closed, so that probe fails without finding anything.
static void pointAt(const FakeRouter& fake) {
    setEnv("NINJAM_UPNP_ROOTDESC", fake.rootdesc());
    setEnv("NINJAM_NATPMP_GATEWAY", "127.0.0.1");
}

// 8.1
TEST(upnp_public_address) {
    FakeRouter fake("public");
    CHECK(fake.ok());
    pointAt(fake);
    Log log;
    PortMapper pm;
    log.attach(pm);
    auto t0 = Clock::now();
    pm.start(kPort);
    CHECK(log.waitFor("people can connect to 93.184.216.34:" + std::to_string(kPort), 5000));
    CHECK(Clock::now() - t0 < std::chrono::seconds(5));
    CHECK(log.has("asking the router to forward TCP port " + std::to_string(kPort)));
    bool held = false;
    for (const auto& l : fake.mappings())
        if (l == "upnp TCP 2049 2049 127.0.0.1 3600 NINJAM server") held = true;
    CHECK(held);
    pm.stop();
    CHECK(log.has("UPnP: removed the forward for TCP port " + std::to_string(kPort)));
    CHECK_EQ(fake.countMappings("upnp "), 0);
    CHECK(log.allPrefixed());
}

// 8.2
TEST(upnp_private_address) {
    FakeRouter fake("private");
    CHECK(fake.ok());
    pointAt(fake);
    Log log;
    PortMapper pm;
    log.attach(pm);
    pm.start(kPort);
    CHECK(log.waitFor("which is not public", 5000));
    CHECK(log.has("router's own internet address is 192.168.1.1"));
    CHECK_EQ(fake.countMappings("upnp TCP 2049 2049 127.0.0.1 3600 NINJAM server"), 1);
    pm.stop();
    CHECK_EQ(fake.countMappings("upnp "), 0);
}

// 8.3
TEST(upnp_permanent_only) {
    FakeRouter fake("permanent");
    CHECK(fake.ok());
    pointAt(fake);
    Log log;
    PortMapper pm;
    log.attach(pm);
    pm.start(kPort);
    CHECK(log.waitFor("people can connect to 93.184.216.34:" + std::to_string(kPort), 5000));
    CHECK_EQ(fake.countMappings("upnp TCP 2049 2049 127.0.0.1 0 NINJAM server"), 1);
    CHECK(!log.has("refused"));
    pm.stop();
    CHECK_EQ(fake.countMappings("upnp "), 0);
}

// 8.4
TEST(upnp_718_then_natpmp) {
    FakeRouter fake("refuse718");
    CHECK(fake.ok());
    pointAt(fake);
    Log log;
    PortMapper pm;
    log.attach(pm);
    pm.start(kPort);
    CHECK(log.waitFor("NAT-PMP: people can connect to 93.184.216.34:" + std::to_string(kPort), 8000));
    CHECK(log.has("UPnP: TCP port " + std::to_string(kPort) + " on the router is already forwarded to another machine"));
    CHECK_EQ(fake.countMappings("upnp "), 0);
    CHECK_EQ(fake.countMappings("natpmp TCP 2049 2049 3600"), 1);
    pm.stop();
    CHECK(log.has("NAT-PMP: removed the forward for TCP port " + std::to_string(kPort)));
    CHECK_EQ(fake.countMappings("natpmp TCP"), 0);
}

// 8.5
TEST(no_router) {
    setEnv("NINJAM_UPNP_ROOTDESC", "http://127.0.0.1:" + std::to_string(closedPort()) + "/rootDesc.xml");
    setEnv("NINJAM_NATPMP_GATEWAY", "127.0.0.1");
    Log log;
    PortMapper pm;
    log.attach(pm);
    pm.start(kPort);
    CHECK(log.waitFor("could not find a router that supports UPnP or NAT-PMP", 10000));
    CHECK(log.has("Will keep trying every 5 minutes"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK_EQ(log.count("could not find a router"), 1);
    auto t0 = Clock::now();
    pm.stop();
    CHECK(Clock::now() - t0 < std::chrono::seconds(1));
    CHECK(!log.has("removed the forward"));
}

// 8.6
TEST(natpmp_renewal) {
    FakeRouter fake("natpmp10");
    CHECK(fake.ok());
    pointAt(fake);
    Log log;
    PortMapper pm;
    log.attach(pm);
    pm.start(kPort);
    CHECK(log.waitFor("NAT-PMP: people can connect to 93.184.216.34:" + std::to_string(kPort), 8000));
    // Wait for the second mapping request (lifetime != 0).
    std::vector<double> times;
    auto deadline = Clock::now() + std::chrono::seconds(14);
    while (Clock::now() < deadline) {
        times.clear();
        for (const auto& l : fake.mappings()) {
            double t;
            int priv, pub, life;
            if (std::sscanf(l.c_str(), "natpmp_request %lf %d %d %d", &t, &priv, &pub, &life) == 4 && life != 0)
                times.push_back(t);
        }
        if (times.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(times.size() >= 2);
    if (times.size() >= 2) {
        double gap = times[1] - times[0];
        std::printf("    renewal after %.2f s\n", gap);
        CHECK(gap >= 5.0 && gap <= 10.0);
    }
    pm.stop();
    CHECK_EQ(fake.countMappings("natpmp TCP"), 0);
}

// 8.7
TEST(private_ipv4) {
    for (const char* a : {"10.0.0.1", "172.16.0.1", "172.31.255.255", "192.168.1.1", "100.64.0.1",
                          "127.0.0.1", "169.254.1.1", "", "example", "1.2.3", "1.2.3.4.5", "256.1.1.1", "8.8.8.8 "})
        CHECK(isPrivateIPv4(a));
    for (const char* a : {"172.32.0.1", "100.128.0.1", "93.184.216.34", "8.8.8.8"})
        CHECK(!isPrivateIPv4(a));
}

TEST_MAIN()
