#!/usr/bin/env python3
"""Fake home router for the njproto_portmap acceptance tests (PORTMAP.md 8).

    fake_router.py <scenario>

Scenarios:
  public      UPnP IGD, AddPortMapping succeeds, external address 93.184.216.34
  private     like public, but the external address is 192.168.1.1
  permanent   AddPortMapping answers error 725 unless the lease is 0
  refuse718   AddPortMapping answers error 718; NAT-PMP responder up (3600 s)
  natpmp10    no IGD (root description is a 404); NAT-PMP grants 10 s leases

Serves the IGD root description at GET /rootDesc.xml and SOAP control at
POST /ctl/IPConn on an ephemeral 127.0.0.1 port, and NAT-PMP (RFC 6886) on
UDP 127.0.0.1:5351 when the scenario has a responder. Prints
"READY <http-port>" on stdout once everything listens. Bookkeeping endpoints:

  GET /mappings   one line per held mapping and per NAT-PMP mapping request:
                    upnp TCP <ext> <int> <client> <lease> <desc>
                    natpmp TCP <private> <public> <lifetime>
                    natpmp_request <seconds-since-start> <private> <public> <lifetime>
  GET /quit       stops the fake router

Standard library only.
"""
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCENARIOS = {
    "public": dict(upnp=True, external="93.184.216.34", add_error=0, natpmp=False, natpmp_lease=3600),
    "private": dict(upnp=True, external="192.168.1.1", add_error=0, natpmp=False, natpmp_lease=3600),
    "permanent": dict(upnp=True, external="93.184.216.34", add_error=725, natpmp=False, natpmp_lease=3600),
    "refuse718": dict(upnp=True, external="93.184.216.34", add_error=718, natpmp=True, natpmp_lease=3600),
    "natpmp10": dict(upnp=False, external="93.184.216.34", add_error=0, natpmp=True, natpmp_lease=10),
}

NATPMP_PUBLIC = "93.184.216.34"
START = time.monotonic()

state_lock = threading.Lock()
upnp_mappings = {}      # (proto, ext_port) -> (int_port, client, lease, desc)
natpmp_mappings = {}    # (proto, private_port) -> (public_port, lifetime)
natpmp_requests = []    # (t, private_port, public_port, lifetime)


def report_lines():
    with state_lock:
        out = []
        for (proto, ext), (iport, client, lease, desc) in upnp_mappings.items():
            out.append("upnp %s %d %d %s %d %s" % (proto, ext, iport, client, lease, desc))
        for (proto, priv), (pub, life) in natpmp_mappings.items():
            out.append("natpmp %s %d %d %d" % (proto, priv, pub, life))
        for (t, priv, pub, life) in natpmp_requests:
            out.append("natpmp_request %.3f %d %d %d" % (t, priv, pub, life))
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ UPnP

ROOTDESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>fake router</friendlyName>
    <manufacturer>njproto tests</manufacturer>
    <modelName>fake</modelName>
    <UDN>uuid:00000000-0000-0000-0000-000000000001</UDN>
    <deviceList>
      <device>
        <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
        <friendlyName>WANDevice</friendlyName>
        <manufacturer>njproto tests</manufacturer>
        <modelName>fake</modelName>
        <UDN>uuid:00000000-0000-0000-0000-000000000002</UDN>
        <serviceList>
          <service>
            <serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
            <serviceId>urn:upnp-org:serviceId:WANCommonIFC1</serviceId>
            <controlURL>/ctl/CommonIfCfg</controlURL>
            <eventSubURL>/evt/CommonIfCfg</eventSubURL>
            <SCPDURL>/WANCfg.xml</SCPDURL>
          </service>
        </serviceList>
        <deviceList>
          <device>
            <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
            <friendlyName>WANConnectionDevice</friendlyName>
            <manufacturer>njproto tests</manufacturer>
            <modelName>fake</modelName>
            <UDN>uuid:00000000-0000-0000-0000-000000000003</UDN>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
                <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
                <controlURL>/ctl/IPConn</controlURL>
                <eventSubURL>/evt/IPConn</eventSubURL>
                <SCPDURL>/WANIPCn.xml</SCPDURL>
              </service>
            </serviceList>
          </device>
        </deviceList>
      </device>
    </deviceList>
  </device>
</root>
"""

SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"


def soap_ok(action, args):
    body = "".join("<%s>%s</%s>" % (k, v, k) for k, v in args)
    return ('<?xml version="1.0"?>\n'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            '<s:Body><u:%sResponse xmlns:u="%s">%s</u:%sResponse></s:Body></s:Envelope>'
            % (action, SERVICE, body, action))


def soap_fault(code, text):
    return ('<?xml version="1.0"?>\n'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
            '<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>'
            '<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
            '<errorCode>%d</errorCode><errorDescription>%s</errorDescription>'
            '</UPnPError></detail></s:Fault></s:Body></s:Envelope>' % (code, text))


def arg(body, name):
    m = re.search(r"<%s>([^<]*)</%s>" % (name, name), body)
    return m.group(1).strip() if m else ""


class Handler(BaseHTTPRequestHandler):
    scenario = None
    server_ref = None

    def log_message(self, fmt, *args):   # quiet
        pass

    def _send(self, status, text, ctype="text/xml; charset=\"utf-8\""):
        data = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/mappings":
            self._send(200, report_lines(), "text/plain")
        elif self.path == "/quit":
            self._send(200, "bye\n", "text/plain")
            threading.Thread(target=self.server_ref.shutdown, daemon=True).start()
        elif self.path == "/rootDesc.xml" and self.scenario["upnp"]:
            self._send(200, ROOTDESC)
        else:
            self._send(404, "not found\n", "text/plain")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        action = self.headers.get("SOAPAction", "").strip('"')
        action = action.split("#")[-1]
        if self.path != "/ctl/IPConn" or not self.scenario["upnp"]:
            self._send(404, "not found\n", "text/plain")
            return
        sc = self.scenario
        if action == "GetStatusInfo":
            self._send(200, soap_ok(action, [("NewConnectionStatus", "Connected"),
                                             ("NewLastConnectionError", "ERROR_NONE"),
                                             ("NewUptime", "1000")]))
        elif action == "GetExternalIPAddress":
            self._send(200, soap_ok(action, [("NewExternalIPAddress", sc["external"])]))
        elif action == "AddPortMapping":
            proto = arg(body, "NewProtocol")
            ext = int(arg(body, "NewExternalPort") or 0)
            iport = int(arg(body, "NewInternalPort") or 0)
            client = arg(body, "NewInternalClient")
            lease = int(arg(body, "NewLeaseDuration") or 0)
            desc = arg(body, "NewPortMappingDescription")
            err = sc["add_error"]
            if err == 725 and lease != 0:
                self._send(500, soap_fault(725, "OnlyPermanentLeasesSupported"))
            elif err == 718:
                self._send(500, soap_fault(718, "ConflictInMappingEntry"))
            else:
                with state_lock:
                    upnp_mappings[(proto, ext)] = (iport, client, lease, desc)
                self._send(200, soap_ok(action, []))
        elif action == "DeletePortMapping":
            proto = arg(body, "NewProtocol")
            ext = int(arg(body, "NewExternalPort") or 0)
            with state_lock:
                had = upnp_mappings.pop((proto, ext), None)
            if had is None:
                self._send(500, soap_fault(714, "NoSuchEntryInArray"))
            else:
                self._send(200, soap_ok(action, []))
        else:
            self._send(500, soap_fault(401, "Invalid Action"))


# --------------------------------------------------------------- NAT-PMP

def natpmp_loop(sock, scenario, stop):
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            data, peer = sock.recvfrom(64)
        except socket.timeout:
            continue
        except OSError:
            break
        if len(data) < 2 or data[0] != 0:
            continue
        epoch = int(time.monotonic() - START)
        op = data[1]
        if op == 0:
            packed = socket.inet_aton(NATPMP_PUBLIC)
            resp = struct.pack("!BBHI", 0, 128, 0, epoch) + packed
        elif op in (1, 2) and len(data) >= 12:
            _, _, _, priv, pub, life = struct.unpack("!BBHHHI", data[:12])
            proto = "UDP" if op == 1 else "TCP"
            with state_lock:
                natpmp_requests.append((time.monotonic() - START, priv, pub, life))
                if life == 0:
                    natpmp_mappings.pop((proto, priv), None)
                    granted_pub, granted_life = pub, 0
                else:
                    granted_pub = pub if pub else priv
                    granted_life = min(life, scenario["natpmp_lease"])
                    natpmp_mappings[(proto, priv)] = (granted_pub, granted_life)
            resp = struct.pack("!BBHIHHI", 0, 128 + op, 0, epoch, priv, granted_pub, granted_life)
        else:
            resp = struct.pack("!BBHI", 0, 128 + (op & 127), 5, epoch)
        try:
            sock.sendto(resp, peer)
        except OSError:
            pass


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in SCENARIOS:
        sys.stderr.write("usage: fake_router.py <%s>\n" % "|".join(SCENARIOS))
        return 2
    scenario = SCENARIOS[sys.argv[1]]

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    httpd.daemon_threads = True
    Handler.scenario = scenario
    Handler.server_ref = httpd

    stop = threading.Event()
    udp = None
    if scenario["natpmp"]:
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.bind(("127.0.0.1", 5351))
        threading.Thread(target=natpmp_loop, args=(udp, scenario, stop), daemon=True).start()

    sys.stdout.write("READY %d\n" % httpd.server_address[1])
    sys.stdout.flush()
    try:
        httpd.serve_forever(poll_interval=0.2)
    finally:
        stop.set()
        if udp is not None:
            udp.close()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
