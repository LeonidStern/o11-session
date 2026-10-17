# Router port mapping specification

Revision 1, 2026-10-17. Companion to `PROTOCOL.md`. Describes the small
library a hosted room uses to ask the home router to forward its TCP port, so
that other players can reach a server behind NAT without the host configuring
the router by hand. Written from the behaviour of the Jamsa server's port
mapper so that it can be implemented without reading it.

## 1. Scope and dependencies

The library maps **one TCP port** (the server's listening port, same number
inside and outside) using **UPnP-IGD first, then NAT-PMP**. It runs on its
own thread, renews the mapping before it expires, keeps trying when no router
answers, and removes the mapping when stopped.

Dependencies: miniupnpc (BSD) for UPnP-IGD and libnatpmp (BSD) for NAT-PMP,
both found with `pkg-config` or as CMake targets `miniupnpc` and `natpmp`
when those exist. The standard library's threads, mutex and condition
variable. POSIX or Winsock sockets.

## 2. Interface

```
class PortMapper {
public:
    PortMapper();
    ~PortMapper();                  // Stop()
    void start(int port);           // non-blocking; replaces any mapping for a previous port
    void stop();                    // blocks until the mapping is removed (bounded by router timeouts)
    std::function<void(const std::string&)> log;   // one line per event, no trailing newline
};
bool isPrivateIPv4(const std::string& dotted);
```

`isPrivateIPv4` is true for addresses that can't be reached from the
internet: 10/8, 172.16/12, 192.168/16, carrier-grade NAT 100.64/10, loopback
127/8, link-local 169.254/16, and anything that is not a dotted IPv4 address.

Every log line starts with `[portmap] `. The application shows the last line
to the user and parses nothing else, except that it treats a line containing
`people can connect to <addr>:<port>` as success with that address, and any
other line as "still trying" or a reason.

## 3. Constants

| Name | Value |
|---|---|
| lease asked for | 3600 s |
| renewal | at half of the lease the router granted, at least 30 s |
| recheck of a permanent mapping | every 1800 s (routers forget mappings when they reboot) |
| retry after a failure to find a router | 300 s |
| UPnP discovery wait | 2000 ms |
| NAT-PMP answer wait | 3000 ms (2000 ms when removing while stopping) |
| mapping description | `NINJAM server` |
| protocol | TCP |

## 4. The mapping thread

`start(port)` first calls `stop()`, then starts the thread, which logs
`[portmap] asking the router to forward TCP port <port> to this machine` and
loops until stopped:

1. **Map** (section 5). Returns the lease in seconds, 0 for permanent, or
   -1 on failure.
2. On failure while a mapping was active ("lost the forward"): log
   `[portmap] lost the forward for TCP port <port>, trying again`, remove the
   old state (section 6) and loop at once.
3. On failure with no router found: log once per run of failures
   `[portmap] could not find a router that supports UPnP or NAT-PMP. Forward
   TCP port <port> on your router to this machine by hand, or turn on UPnP in
   the router's settings. Will keep trying every 5 minutes.` and wait 300 s.
4. On success: wait lease/2 (at least 30 s), or 1800 s for a permanent lease.
5. Waiting is interruptible by `stop()`.

When the loop ends, the mapping is removed (section 6) and the thread exits.
`stop()` sets the flag, wakes the thread, joins it, and returns.

## 5. Mapping

With an active method, renewing means repeating that method's add request.
Without one:

**UPnP-IGD.** Discover with one multicast search for all these types at once
(waiting the discovery time once, not once per type):
`urn:schemas-upnp-org:device:InternetGatewayDevice:1` and `:2`,
`urn:schemas-upnp-org:service:WANIPConnection:1` and `:2`,
`urn:schemas-upnp-org:service:WANPPPConnection:1`. If the environment
variable `NINJAM_UPNP_ROOTDESC` is set, skip the search and use that root
description URL (this is how tests point at a fake router). A found gateway
is usable when it is a connected IGD; one that is connected but reports a
private WAN address is also usable (the address is reported in section 7).
Then AddPortMapping (external port = internal port = `port`, the machine's LAN
address as reported by the discovery, the description, TCP, lease 3600).
If the router answers error **725** (only permanent leases supported), retry
with lease 0 and remember that this router is permanent-only. Error **718**
means the port is forwarded to another machine: log `[portmap] UPnP: TCP port
<port> on the router is already forwarded to another machine`; any other
error logs `[portmap] UPnP: router refused to forward TCP port <port> (error
<n>)`. On success, ask GetExternalIPAddress and report (section 7), returning
the lease (0 when permanent).

**NAT-PMP** (only when UPnP found nothing or refused). Gateway: the default
route, or the environment variable `NINJAM_NATPMP_GATEWAY` when set. Send a
TCP mapping request (private port = public port = `port`, lifetime 3600),
waiting up to 3000 ms for the answer and checking the stop flag every 250 ms.
A refusal while probing is silent; while renewing it logs `[portmap] NAT-PMP:
router did not answer` or `[portmap] NAT-PMP: router refused to forward TCP
port <port> (error <n>)`. On success remember the public port the router
granted (it may differ) and reuse it for renewals, ask the public address and
report it, return the granted lifetime.

If neither works, return -1.

## 6. Removing

UPnP: DeletePortMapping for the port; log `[portmap] UPnP: removed the forward
for TCP port <port>` or `[portmap] UPnP: could not remove the forward for TCP
port <port> (error <n>)`. NAT-PMP: a mapping request with lifetime 0 for the
granted public port, waiting at most 2000 ms and not giving up on the stop
flag; log the same two forms with `NAT-PMP`. Then the method is none.

## 7. Reporting the address

After a successful mapping, with `<how>` = `UPnP` or `NAT-PMP` and the
external port actually granted:

- no address known: `[portmap] <how>: router forwards TCP port <port> to this
  machine, but did not say its public address`;
- a private address (section 2): `[portmap] <how>: router forwards TCP port
  <port> to this machine, but the router's own internet address is <addr>,
  which is not public. Your internet provider probably uses carrier-grade
  NAT, or another router sits in front of this one, so people outside your
  network still can't connect. Ask your provider for a public IPv4 address, or
  run the server on a hosted machine.`;
- otherwise: `[portmap] <how>: people can connect to <addr>:<port>`.

## 8. Acceptance tests

Against a fake router in the test (an HTTP server answering the IGD root
description and SOAP AddPortMapping / DeletePortMapping /
GetExternalIPAddress, pointed at with `NINJAM_UPNP_ROOTDESC`; and a UDP
responder on 127.0.0.1:5351 speaking NAT-PMP, pointed at with
`NINJAM_NATPMP_GATEWAY=127.0.0.1`):

1. UPnP router with public address 93.184.216.34: within 5 s of `start`, the
   router holds a TCP mapping for the port with description `NINJAM server`
   and the log has `people can connect to 93.184.216.34:<port>`; `stop()`
   removes it and logs `removed the forward`.
2. UPnP router with address 192.168.1.1: the mapping is made and the log has
   `which is not public`.
3. UPnP router answering 725: a permanent mapping (lease 0) is made.
4. UPnP router answering 718: the log says the port is forwarded to another
   machine and NAT-PMP is tried next; with the NAT-PMP responder up, the
   mapping is made through it and reported with `NAT-PMP`.
5. No router at all: the "could not find a router" line appears once, the
   thread keeps running, and `stop()` returns within 1 s.
6. A NAT-PMP router that grants a lease of 10 s: a renewal request arrives
   between 5 and 10 s after the first.
7. `isPrivateIPv4`: true for 10.0.0.1, 172.16.0.1, 172.31.255.255,
   192.168.1.1, 100.64.0.1, 127.0.0.1, 169.254.1.1, "" and "example"; false
   for 172.32.0.1, 100.128.0.1, 93.184.216.34, 8.8.8.8.

## 9. Deliverables

A static library target `njproto_portmap` (C++17, `include/njproto/portmap.h`,
`portmap/src/portmap.cpp`), CMake option `NJPROTO_BUILD_PORTMAP` (default ON
when miniupnpc and libnatpmp are found), a ctest test with the fake router
written in C++ or as a Python script started by the test, and a `NOTES.md`
line for anything this document left open.

## 10. Revision history

- **1** (2026-10-17): first version.
