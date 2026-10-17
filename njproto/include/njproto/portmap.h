// njproto - router port mapping (PORTMAP.md revision 1).
//
// A PortMapper asks the home router to forward one TCP port to this machine,
// UPnP-IGD first and NAT-PMP second, on its own thread. It renews the mapping
// before it expires, keeps trying when no router answers and removes the
// mapping when stopped. Everything it has to say goes through `log`, one line
// per event, each starting with "[portmap] ".
#ifndef NJPROTO_PORTMAP_H
#define NJPROTO_PORTMAP_H

#include <functional>
#include <memory>
#include <string>

namespace njproto {

class PortMapper {
public:
    PortMapper();
    ~PortMapper();                  // stop()
    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    // Non-blocking. Replaces any mapping made for a previous port (stop() first).
    void start(int port);
    // Blocks until the mapping is removed; bounded by the router timeouts.
    void stop();

    // One line per event, no trailing newline. Called from the mapping thread.
    // Set it before start(). The last line is what the user should see; a
    // line containing "people can connect to <addr>:<port>" means success.
    std::function<void(const std::string&)> log;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// True for addresses that cannot be reached from the internet: 10/8,
// 172.16/12, 192.168/16, 100.64/10, 127/8, 169.254/16, and anything that is
// not a dotted IPv4 address.
bool isPrivateIPv4(const std::string& dotted);

}  // namespace njproto

#endif  // NJPROTO_PORTMAP_H
