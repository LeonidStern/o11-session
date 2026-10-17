# Port mapping notes

Clean-room implementation of `PORTMAP.md` revision 1 (2026-10-17). The only
inputs were that document, the other njproto sources and documents, and the
public headers of miniupnpc (API version 17) and libnatpmp. No NINJAM, Jamsa
or WDL source was read. Wherever the specification leaves room, the choice is
listed here.

## Layout

- `include/njproto/portmap.h`, `portmap/src/portmap.cpp`: `PortMapper` and
  `isPrivateIPv4` (sections 2-7).
- `portmap/tests/fake_router.py`: the fake router of section 8 (IGD root
  description and SOAP control over HTTP on an ephemeral 127.0.0.1 port,
  NAT-PMP on UDP 127.0.0.1:5351, `GET /mappings` reports what it holds).
- `portmap/tests/test_portmap.cpp`: starts the script per scenario, points the
  mapper at it with `NINJAM_UPNP_ROOTDESC` / `NINJAM_NATPMP_GATEWAY`, and
  checks 8.1-8.7.
- `tools/njserver.cpp --port-map` (default off) runs a `PortMapper` on the
  listening port and forwards its lines to the server log.

## Choices

- **Renewal of short leases (3 and 8.6).** "Renewal at half of the lease the
  router granted, at least 30 s" and test 8.6 ("a NAT-PMP router that grants a
  lease of 10 s: a renewal request arrives between 5 and 10 s after the
  first") contradict each other for leases under 60 s, where a 30 s floor
  would let the mapping expire before the renewal. The floor is applied only
  when the lease is at least 60 s (where it changes nothing), so a 10 s lease
  is renewed after 5 s as the test wants; a lease of 1 s waits 1 s rather
  than 0.
- **"Connected" for a root description URL (5).** `UPNP_GetIGDFromUrl` does
  not say whether it checks the connection status, so `UPNPIGD_IsConnected`
  (GetStatusInfo = `Connected`) is asked explicitly after it. With the
  multicast search, `UPNP_GetValidIGD`'s answer is used: with API 17, 1
  (connected) is usable; with API 18 or later, 1 (public address) and 2
  (connected, private address) are usable, which is the "private WAN address
  is also usable" sentence.
- **UPnP lease returned (5).** AddPortMapping has no field for the lease the
  router granted, so the lease asked for (3600, or 0 when the router is
  permanent-only) is returned.
- **External address `0.0.0.0` (7).** A router that answers
  GetExternalIPAddress with `0.0.0.0` (or an empty string) is treated as "did
  not say its public address" rather than as a public address.
- **NAT-PMP error number (5, 6).** `(error <n>)` carries the NAT-PMP result
  code of RFC 6886 (1-5) when the router answered; a libnatpmp failure that is
  not a router answer (socket error, wrong source) keeps the library's
  negative code. "Did not answer" is logged for anything that is not a router
  answer, including a timeout and an ICMP port unreachable.
- **NAT-PMP retransmission (5).** libnatpmp retransmits on its own schedule
  (250 ms, 500 ms, ...) inside `readnatpmpresponseorretry`, which is called
  every 250 ms while the stop flag is checked; the 3000 ms budget is the
  mapper's, independent of the library's retry count.
- **NAT-PMP gateway when not forced (5).** libnatpmp reads the default route
  itself; when it cannot, NAT-PMP counts as "no router".
- **Stop during a request (4, 5).** When `stop()` arrives while a NAT-PMP
  answer is awaited, the request is abandoned without a log line, the loop
  ends and the removal of section 6 runs (for a mapping that may or may not
  have been made; the router's answer to the removal tells). The UPnP
  discovery (2000 ms) and the HTTP calls are not interruptible.
- **"Lost the forward" (4).** A failed renewal logs the UPnP/NAT-PMP reason
  line first (section 5), then the "lost the forward" line, then removes the
  old state, which asks the router that just failed to delete the mapping
  (bounded by miniupnpc's HTTP timeout or 2000 ms for NAT-PMP).
- **Test 8.5 "no router at all".** The root description URL points at a
  closed loopback port and the NAT-PMP gateway at 127.0.0.1 with nothing on
  UDP 5351, so the test does not depend on the multicast search finding
  nothing on the build machine. miniupnpc prints `connect: Connection
  refused` to stderr in that case; it is the library's own message.
- **Test port.** The tests map port 2049 and use 127.0.0.1 as the LAN
  address (what miniupnpc reports for a loopback root description).
- **NAT-PMP lifetime of 0 granted.** Not expected from a router; it would be
  treated as a permanent lease (rechecked every 1800 s), as section 4 reads.
- `log` is copied when `start()` is called; setting it afterwards has no
  effect on the running thread.
