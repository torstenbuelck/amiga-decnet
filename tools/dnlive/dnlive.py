#!/usr/bin/env python3
"""dnlive - live traffic display for an exhibition: "DECnet live".

Serves a full-screen page (index.html, amber VT terminal style) on
http://127.0.0.1:8023/ and its data on /api, once a second:

  - bytes per second in and out of two interfaces (default eth0, the
    LAN with the old machines, and wlan0, the uplink), from
    /sys/class/net/IF/statistics
  - on the LAN interface: frames by protocol (DECnet, LAT, MOP, IP,
    other), read with a packet socket (needs CAP_NET_RAW or root)
  - DECnet nodes heard (hellos), LAT services announced, and an event
    log: nodes coming and going, logical links opened and closed

Python 3 standard library only (Raspberry Pi OS Bookworm as it comes).

Usage: dnlive.py [--lan eth0] [--wan wlan0] [--nodes FILE] [--port 8023]
                 [--bind 127.0.0.1] [--no-promisc]

Copyright (C) 2026  Torsten Bülck (tuti)
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import os
import socket
import struct
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HISTORY = 180                   # seconds of graph history
AVERAGE = 10                    # seconds the protocol rates are averaged
EVENTS = 40                     # event log lines kept
ETH_P_ALL = 0x0003
SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_PROMISC = 1

PROTOS = ("DECnet", "LAT", "MOP", "IP", "other")
ETHERTYPES = {0x6003: "DECnet", 0x6004: "LAT", 0x6001: "MOP", 0x6002: "MOP",
              0x0800: "IP", 0x86DD: "IP", 0x0806: "IP"}
HERE = os.path.dirname(os.path.abspath(__file__))

lock = threading.Lock()
state = {}


def now():
    return time.time()


def addr_str(a):
    return "%d.%d" % (a >> 10, a & 1023)


def load_nodes(path):
    """NAME ADDRESS per line (more columns and ; comments ignored)"""
    names = {}
    try:
        with open(path) as f:
            for line in f:
                w = line.split(";")[0].split()
                if len(w) >= 2 and "." in w[1]:
                    try:
                        area, node = (int(x) for x in w[1].split("."))
                        names[(area << 10) | node] = w[0].upper()
                    except ValueError:
                        pass
    except OSError:
        pass
    return names


def dn_addr(mac):
    """DECnet address from an AA-00-04-00-lo-hi MAC, else None"""
    if mac[:4] == b"\xaa\x00\x04\x00":
        return mac[4] | (mac[5] << 8)
    return None


class Counters:
    """bytes per second of one interface, from its statistics"""

    def __init__(self, name):
        self.name = name
        self.last = None
        self.rx = deque([0] * HISTORY, HISTORY)
        self.tx = deque([0] * HISTORY, HISTORY)
        self.rx_total = self.tx_total = 0

    def read(self, what):
        try:
            with open("/sys/class/net/%s/statistics/%s" % (self.name, what)) as f:
                return int(f.read())
        except (OSError, ValueError):
            return None

    def up(self):
        try:
            with open("/sys/class/net/%s/operstate" % self.name) as f:
                return f.read().strip() in ("up", "unknown")
        except OSError:
            return False

    def sample(self):
        rx, tx = self.read("rx_bytes"), self.read("tx_bytes")
        if rx is None or tx is None:
            self.rx.append(0)
            self.tx.append(0)
            self.last = None
            return
        if self.last:
            self.rx.append(max(0, rx - self.last[0]))
            self.tx.append(max(0, tx - self.last[1]))
        self.last = (rx, tx)
        self.rx_total, self.tx_total = rx, tx


class Sniffer:
    """frames on the LAN interface: protocols, DECnet nodes, LAT, links"""

    def __init__(self, ifname, names, promisc):
        self.ifname = ifname
        self.names = names
        self.promisc = promisc
        self.error = None
        self.cur = {p: [0, 0] for p in PROTOS}     # this second: frames, bytes
        self.hist = {p: deque([0] * HISTORY, HISTORY) for p in PROTOS}
        self.recent = {p: deque([(0, 0)] * AVERAGE, AVERAGE) for p in PROTOS}
        self.nodes = {}         # addr -> dict(kind, last, hello)
        self.lat = {}           # (node, service) -> dict(last, ident, timer)
        self.latnode = {}       # LAT node name -> last
        self.links = {}         # (src, srclink, dst) -> opened
        self.events = deque(maxlen=EVENTS)

    def name(self, a):
        return self.names.get(a) or addr_str(a)

    def label(self, a):
        """NAME (23.208), or just the address without a name"""
        n = self.names.get(a)
        return "%s (%s)" % (n, addr_str(a)) if n else addr_str(a)

    def event(self, text):
        self.events.append((now(), text))

    # -- per frame
    def frame(self, data, outgoing):
        if len(data) < 14:
            return
        et = struct.unpack("!H", data[12:14])[0]
        if et == 0x8100 and len(data) >= 18:            # VLAN tag
            et = struct.unpack("!H", data[16:18])[0]
            payload = data[18:]
        else:
            payload = data[14:]
        proto = ETHERTYPES.get(et, "other")
        c = self.cur[proto]
        c[0] += 1
        c[1] += len(data) + 4
        src = data[6:12]
        if et == 0x6003:
            self.decnet(src, payload)
        elif et == 0x6004:
            self.lat_msg(payload)

    def decnet(self, src, p):
        if len(p) < 3:
            return
        n = p[0] | (p[1] << 8)              # length word
        m = p[2:2 + n]
        if not m:
            return
        flags = m[0]
        if flags & 0x80:                    # padding
            m = m[flags & 0x7F:]
            if not m:
                return
            flags = m[0]
        a = dn_addr(src)
        if flags & 1:                       # control message
            kind = (flags >> 1) & 7
            if kind == 5 and a is not None:     # Ethernet router hello
                self.hello(a, "Router", m[13] if len(m) > 13 else 64,
                           struct.unpack("<H", m[15:17])[0] if len(m) >= 17 else 15)
            elif kind == 6 and a is not None:   # Ethernet endnode hello
                self.hello(a, "Endnode", 0,
                           struct.unpack("<H", m[28:30])[0] if len(m) >= 30 else 15)
            return
        if (flags & 7) == 6 and len(m) >= 21:   # long data header
            dst = dn_addr(m[3:9])
            sa = dn_addr(m[11:17])
            nsp = m[21:]
            if dst is not None and sa is not None and nsp:
                self.nsp(sa, dst, nsp)

    def hello(self, a, kind, prio, timer):
        t = now()
        n = self.nodes.get(a)
        if n is None:
            self.event("%s meldet sich" % self.label(a))
            n = self.nodes[a] = {}
        n.update(kind=kind, last=t, timer=max(5, min(timer or 15, 120)))

    def nsp(self, sa, dst, m):
        f = m[0]
        if len(m) < 5:
            return
        dlink, slink = struct.unpack("<HH", m[1:5])
        if f in (0x18, 0x68):               # connect initiate (or again)
            key = (sa, slink, dst)
            if key not in self.links:
                self.links[key] = now()
                self.event("%s -> %s verbunden"
                           % (self.name(sa), self.name(dst)))
        elif f in (0x38, 0x48):             # disconnect initiate / confirm
            for key in [k for k in self.links
                        if (k[0], k[2]) in ((sa, dst), (dst, sa))
                        and k[1] in (dlink, slink)]:
                del self.links[key]
                self.event("%s -> %s getrennt"
                           % (self.name(key[0]), self.name(key[2])))

    def lat_msg(self, m):
        # service announcement: header, group mask, node, greeting, services
        if len(m) < 13 or (m[0] & 0xFC) != 0x28:
            return
        timer = m[10] or 30
        p = 13 + m[12]

        def ascic(p):
            if p >= len(m):
                return "", len(m)
            n = m[p]
            return m[p + 1:p + 1 + n].decode("latin-1", "replace"), p + 1 + n

        node, p = ascic(p)
        _greeting, p = ascic(p)
        if not node or p >= len(m):
            return
        t = now()
        if node not in self.latnode:
            self.event("LAT: %s kündigt Dienste an" % node)
        self.latnode[node] = t
        count = m[p]
        p += 1
        for _ in range(count):
            if p >= len(m):
                break
            p += 1                          # rating
            name, p = ascic(p)
            ident, p = ascic(p)
            if name:
                self.lat[(node, name)] = dict(last=t, ident=ident.strip(),
                                              timer=timer)

    # -- once a second
    def tick(self):
        t = now()
        for p in PROTOS:
            self.recent[p].append(tuple(self.cur[p]))
            self.hist[p].append(self.cur[p][1])
            self.cur[p] = [0, 0]
        for a, n in list(self.nodes.items()):
            if t - n["last"] > 3 * n["timer"] + 5:
                self.event("%s ist verstummt" % self.label(a))
                del self.nodes[a]
        for k, v in list(self.lat.items()):
            if t - v["last"] > 3 * v["timer"] + 5:
                del self.lat[k]
        for k, v in list(self.latnode.items()):
            if t - v > 300:
                del self.latnode[k]
        for k, v in list(self.links.items()):
            if t - v > 3600:                # never saw it end
                del self.links[k]

    def run(self):
        try:
            s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                              socket.htons(ETH_P_ALL))
            s.bind((self.ifname, 0))
            if self.promisc:
                idx = socket.if_nametoindex(self.ifname)
                mreq = struct.pack("iHH8s", idx, PACKET_MR_PROMISC, 0, b"")
                s.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
        except OSError as e:
            self.error = "kein Mitschnitt auf %s: %s" % (self.ifname, e.strerror)
            return
        while True:
            try:
                data, addr = s.recvfrom(2048)
            except OSError:
                time.sleep(1)
                continue
            with lock:
                self.frame(data, addr[2] == socket.PACKET_OUTGOING)


def snapshot(lan, wan, sn):
    t = now()
    nodes = sorted(({"name": sn.name(a), "addr": addr_str(a), "kind": n["kind"],
                     "age": round(t - n["last"])}
                    for a, n in sn.nodes.items()),
                   key=lambda n: (n["kind"] != "Router", n["name"]))
    lat = sorted(({"service": k[1], "node": k[0], "ident": v["ident"]}
                  for k, v in sn.lat.items()), key=lambda x: x["service"])
    return {
        "time": t,
        "ifaces": [
            {"name": c.name, "role": role, "up": c.up(),
             "rx": list(c.rx), "tx": list(c.tx)}
            for c, role in ((lan, "lan"), (wan, "wan"))],
        "protos": [{"name": p,
                    "fps": sum(r[0] for r in sn.recent[p]) / AVERAGE,
                    "bps": sum(r[1] for r in sn.recent[p]) / AVERAGE}
                   for p in PROTOS],
        "nodes": nodes,
        "lat": lat,
        "links": len(sn.links),
        "events": [{"t": e[0], "text": e[1]} for e in sn.events],
        "error": sn.error,
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api"):
            with lock:
                body = json.dumps(state.get("snap", {})).encode()
            ctype = "application/json"
        elif self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                body = f.read()
            ctype = "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description="DECnet live traffic display")
    ap.add_argument("--lan", default="eth0")
    ap.add_argument("--wan", default="wlan0")
    ap.add_argument("--nodes", default=os.path.join(HERE, "nodes"))
    ap.add_argument("--port", type=int, default=8023)
    ap.add_argument("--bind", default="127.0.0.1")
    # without it the card drops the DECnet multicasts (hellos) and all
    # unicast between other machines; a bridge port is promiscuous anyway
    ap.add_argument("--no-promisc", dest="promisc", action="store_false",
                    help="leave the LAN interface out of promiscuous mode")
    a = ap.parse_args()

    lan, wan = Counters(a.lan), Counters(a.wan)
    sn = Sniffer(a.lan, load_nodes(a.nodes), a.promisc)
    threading.Thread(target=sn.run, daemon=True).start()

    def ticker():
        nxt = time.time()
        while True:
            nxt += 1
            time.sleep(max(0, nxt - time.time()))
            with lock:
                lan.sample()
                wan.sample()
                sn.tick()
                state["snap"] = snapshot(lan, wan, sn)

    threading.Thread(target=ticker, daemon=True).start()
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
