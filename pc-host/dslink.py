#!/usr/bin/env python3
"""
dslink.py — côté PC du pont DS <-> ESP32 <-> PC.

Commandes :
  python dslink.py ports                         liste les ports série
  python dslink.py demo    --port COM5           homebrew : dialogue avec la démo DS
  python dslink.py demo    --udp                 idem sans ESP32 (melonDS)
  python dslink.py demo    --relay esp-pont.local  idem, vraie DS derrière ESP-Pont
  python dslink.py demo    --port COM5 --udp     hub : vraies DS + melonDS ensemble
  python dslink.py sniff   --port COM5 -o cap.pcap [--nintendo] [--channel 7]
  python dslink.py analyze cap.pcap              analyse une capture (sans ESP32)
  python dslink.py replay-beacon --port COM5 cap.pcap [--index 0] [--interval 100]

Dépendance : pyserial (pip install pyserial) — sauf pour "analyze".
La classe DsLinkBridge s'importe dans votre port PC :
    from dslink import DsLinkBridge     # pont ESP32
    from dslink import DsLinkUdp        # melonDS / réseau, même interface
"""
from __future__ import annotations

import argparse
import queue
import struct
import sys
import threading
import time
from collections import Counter, defaultdict

# --------------------------------------------------------------------------
# Constantes (miroir de common/linkproto.h)
# --------------------------------------------------------------------------
SER_CMD_PING, SER_CMD_SET_CHANNEL, SER_CMD_SET_MODE = 0x01, 0x02, 0x03
SER_CMD_SEND_LINK, SER_CMD_TX_RAW = 0x04, 0x05
SER_CMD_BEACON_START, SER_CMD_BEACON_STOP = 0x06, 0x07
SER_EVT_PONG, SER_EVT_RX_LINK, SER_EVT_RX_RAW = 0x80, 0x81, 0x82
SER_EVT_LOG, SER_EVT_STATS = 0x83, 0x84
SER_CMD_MP_TX, SER_CMD_MP_CONFIG = 0x08, 0x09       # mode MP_HOST (utilisés par melonDS)
SER_EVT_MP_REPLY, SER_EVT_MP_WINDOW = 0x85, 0x86
MODE_LINK, MODE_SNIFF, MODE_NINTENDO, MODE_MP_HOST = 0, 1, 2, 3
RATE_1M, RATE_2M = 0x0A, 0x14
LINK_HEADER = struct.Struct("<IBBHH")       # magic, version, node, seq, len
LINK_MAGIC = 0x4B4E4C44
LINK_VERSION = 1
LINK_MAX_DATA = 400
LINK_UDP_PORT = 47474                       # mode réseau (melonDS, ESP-Pont)

MP_CMD = bytes.fromhex("0309BF000000")      # hôte -> clients
MP_REPLY = bytes.fromhex("0309BF000010")    # clients -> hôte
MP_ACK = bytes.fromhex("0309BF000003")      # ACK de l'hôte


# --------------------------------------------------------------------------
# COBS + CRC16-CCITT (identiques à la version C)
# --------------------------------------------------------------------------
def cobs_encode(data: bytes) -> bytes:
    out = bytearray(b"\x00")
    code_idx, code = 0, 1
    for b in data:
        if b == 0:
            out[code_idx] = code
            code_idx, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_idx] = code
                code_idx, code = len(out), 1
                out.append(0)
    out[code_idx] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes | None:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0:
            return None
        if i + code - 1 > len(data):
            return None
        out += data[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


LINK_BSSID = bytes([0x02]) + b"DSLNK"


def build_link_payload(node: int, seq: int, data: bytes) -> bytes:
    """[LinkHeader][données] (miroir de common/linkproto.h)."""
    data = data[:LINK_MAX_DATA]
    return LINK_HEADER.pack(LINK_MAGIC, LINK_VERSION, node, seq & 0xFFFF, len(data)) + data


def build_air_frame(src_mac: bytes, node: int, seq: int, data: bytes) -> bytes:
    """Trame 802.11 complète, identique à link_build_air_frame() en C."""
    s = seq & 0xFFFF
    hdr = (b"\x08\x00" + b"\x00\x00" + b"\xff" * 6 + bytes(src_mac[:6]) + LINK_BSSID
           + bytes([(s << 4) & 0xFF, (s >> 4) & 0xFF]))
    return hdr + build_link_payload(node, s, data)


def frame_message(msg_type: int, payload: bytes = b"") -> bytes:
    body = bytes([msg_type]) + payload
    return cobs_encode(body + struct.pack("<H", crc16_ccitt(body))) + b"\x00"


# --------------------------------------------------------------------------
# Pont série
# --------------------------------------------------------------------------
class DsLinkBridge:
    """Connexion à l'ESP32. Les événements arrivent dans self.events (Queue)."""

    kind = "radio"                     # transport : DS sur les ondes, via esp32-bridge

    def __init__(self, port: str, baud: int = 921600):
        import serial  # import tardif : "analyze" n'en a pas besoin
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.events: queue.Queue = queue.Queue(maxsize=10000)
        self.bad_frames = 0
        self.mac: bytes | None = None   # MAC de l'ESP32, apprise par ping()
        self._stop = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        self._thread.join(timeout=1)
        self.ser.close()

    # --- envoi -------------------------------------------------------------
    def _send(self, t: int, payload: bytes = b""):
        with self._lock:
            self.ser.write(frame_message(t, payload))

    def ping(self):                    self._send(SER_CMD_PING)
    def set_channel(self, ch: int):    self._send(SER_CMD_SET_CHANNEL, bytes([ch]))
    def set_mode(self, mode: int):     self._send(SER_CMD_SET_MODE, bytes([mode]))
    def beacon_stop(self):             self._send(SER_CMD_BEACON_STOP)

    def send_link(self, data: bytes, node: int = 0, seq: int | None = None):
        """Émet un message dslink sur les ondes, au nom du nœud `node`.
        Sans `seq`, l'ESP32 numérote lui-même. Avec `seq` (hub qui fait suivre
        le message d'une autre DS), la trame est construite ici avec le numéro
        d'origine, pour que les DS comptent correctement les pertes."""
        if seq is None:
            self._send(SER_CMD_SEND_LINK, bytes([node]) + data)
        else:
            src = self.mac or b"\x02\x00\x00\x00\x00\x00"
            self.tx_raw(build_air_frame(src, node, seq, data))

    def tx_raw(self, frame: bytes, rate: int = RATE_2M):
        self._send(SER_CMD_TX_RAW, bytes([rate]) + frame)

    def beacon_start(self, frame: bytes, interval_ms: int = 100):
        self._send(SER_CMD_BEACON_START, struct.pack("<H", interval_ms) + frame)

    # --- réception ---------------------------------------------------------
    def _reader(self):
        buf = bytearray()
        while not self._stop:
            chunk = self.ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while True:
                z = buf.find(0)
                if z < 0:
                    if len(buf) > 8192:
                        buf.clear()
                    break
                raw, buf = bytes(buf[:z]), buf[z + 1:]
                if raw:
                    self._dispatch(raw)

    def _dispatch(self, raw: bytes):
        d = cobs_decode(raw)
        if not d or len(d) < 3 or struct.unpack("<H", d[-2:])[0] != crc16_ccitt(d[:-2]):
            self.bad_frames += 1      # ex. : messages du bootloader au démarrage
            return
        t, p = d[0], d[1:-2]
        try:
            ev = self._parse_event(t, p)
        except (struct.error, IndexError):
            # Charge utile trop courte : on l'ignore plutôt que de tuer le
            # thread de lecture, qui s'arrêterait sans prévenir.
            self.bad_frames += 1
            return
        if t == SER_EVT_PONG:
            self.mac = p[1:7]
        try:
            self.events.put_nowait(ev)
        except queue.Full:
            pass

    @staticmethod
    def _parse_event(t: int, p: bytes) -> dict:
        ev: dict = {"type": t, "time": time.time()}
        if t == SER_EVT_PONG:
            ev.update(fw=p[0], mac=p[1:7].hex(":"))
        elif t == SER_EVT_RX_LINK:
            magic, ver, node, seq, ln = LINK_HEADER.unpack_from(p, 1)
            ev.update(rssi=struct.unpack("b", p[:1])[0], node=node, seq=seq,
                      data=p[1 + LINK_HEADER.size:1 + LINK_HEADER.size + ln])
        elif t == SER_EVT_RX_RAW:
            rssi, ch, ts = struct.unpack_from("<bBI", p)
            ev.update(rssi=rssi, channel=ch, ts_us=ts, frame=p[6:])
        elif t == SER_EVT_LOG:
            ev.update(text=p.decode("utf-8", "replace"))
        elif t == SER_EVT_STATS:
            ev.update(zip(("rx", "drop", "tx", "txerr"), struct.unpack("<4I", p[:16])))
        return ev


# --------------------------------------------------------------------------
# Mode réseau : serveur UDP (melonDS, ESP-Pont, point d'accès ouvert)
# --------------------------------------------------------------------------
def parse_link_payload(pkt: bytes):
    """[LinkHeader][données] -> (node, seq, data) ou None (miroir de link_parse_payload)."""
    if len(pkt) < LINK_HEADER.size:
        return None
    magic, ver, node, seq, ln = LINK_HEADER.unpack_from(pkt)
    if magic != LINK_MAGIC or ver != LINK_VERSION or ln > LINK_MAX_DATA:
        return None
    if LINK_HEADER.size + ln > len(pkt):
        return None
    return node, seq, pkt[LINK_HEADER.size:LINK_HEADER.size + ln]


class DsLinkUdp:
    """Même interface que DsLinkBridge, mais sans ESP32 : les DS se connectent
    à un point d'accès (melonAP sous melonDS) et parlent en UDP.

    Le PC écoute ; chaque DS est découverte à son premier message, et les
    réponses partent vers l'adresse source vue, ce qui traverse les NAT.
    Les événements ont la même forme que ceux du pont (SER_EVT_RX_LINK), avec
    en plus "addr" ; "rssi" vaut 0, l'UDP ne le transporte pas.

    relay : adresse d'un ESP-Pont ("esp-pont.local", "192.168.1.50" ou
    "hôte:port"). Le PC s'y inscrit par un message vide chaque seconde, et le
    pont relaie alors les DS connectées à son point d'accès, sans PC_IP.

    Ce message vide du nœud 0 part aussi vers chaque DS connue : il lui
    confirme que le PC est là (ligne « PC connecte » de la démo), même quand
    le PC n'a rien d'autre à lui dire.
    """

    kind = "udp"                       # transport : melonDS et/ou ESP-Pont
    HELLO_PERIOD = 1.0

    def __init__(self, port: int = LINK_UDP_PORT, bind: str = "0.0.0.0",
                 relay: str | None = None):
        import socket
        self.relay = self._resolve(relay) if relay else None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, port))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.events: queue.Queue = queue.Queue(maxsize=10000)
        self.bad_frames = 0
        self.peers: dict[int, tuple] = {}          # node -> (ip, port)
        self._seq = 0
        self._stop = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        self._hello = threading.Thread(target=self._hello_loop, daemon=True)
        self._hello.start()

    @staticmethod
    def _resolve(relay: str) -> tuple:
        import socket
        host, _, p = relay.partition(":")
        try:
            return socket.gethostbyname(host), int(p) if p else LINK_UDP_PORT
        except OSError:
            raise SystemExit(f"Relais introuvable : {host}. Indiquez l'adresse amont "
                             "affichée sur la page d'état d'ESP-Pont.")

    def close(self):
        self._stop = True
        self._thread.join(timeout=1)
        self._hello.join(timeout=2)
        self.sock.close()

    def _dests(self) -> set:
        d = set(self.peers.values())
        if self.relay:
            d.add(self.relay)
        return d

    def _hello_loop(self):
        """Message vide du nœud 0, chaque seconde : inscription auprès du
        relais, et signe de présence pour les DS déjà connues."""
        while not self._stop:
            dests = self._dests()
            if dests:
                self._send_to(dests, b"")
            time.sleep(self.HELLO_PERIOD)

    # Commandes propres à l'ESP32 : sans objet ici.
    def ping(self):                    pass
    def set_channel(self, ch: int):    pass
    def set_mode(self, mode: int):     pass
    def beacon_stop(self):             pass

    def send_link(self, data: bytes, node: int = 0, seq: int | None = None,
                  exclude: int | None = None):
        """Envoie à toutes les DS découvertes. node = émetteur : 0 pour le PC,
        ou le numéro d'une autre DS quand le hub fait suivre son message (avec
        son `seq` d'origine). Via un relais, un seul envoi suffit : ESP-Pont le
        transmet à chaque DS de son point d'accès.
        exclude : DS à ne pas servir (celle qui a émis le message), sauf si
        elle est derrière le relais, qui sert aussi les autres DS."""
        dests = self._dests()
        skip = self.peers.get(exclude) if exclude is not None else None
        if skip is not None and skip != self.relay:
            dests.discard(skip)
        if dests:
            self._send_to(dests, data, node, seq)

    def send_link_to(self, target: int, data: bytes):
        """Envoie au nom du PC à une seule DS (ex. réponse à son ping)."""
        addr = self.peers.get(target)
        if addr:
            self._send_to({addr}, data)

    def _send_to(self, dests: set, data: bytes, node: int = 0, seq: int | None = None):
        with self._lock:
            if seq is None:                       # numérotation propre au PC
                seq = self._seq
                self._seq = (self._seq + 1) & 0xFFFF
            pkt = build_link_payload(node, seq, data)
            for addr in dests:
                try:
                    self.sock.sendto(pkt, addr)
                except OSError:
                    pass

    def _reader(self):
        import socket
        while not self._stop:
            try:
                pkt, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                # Windows signale par ConnectionResetError un ancien envoi
                # vers un port fermé (DS éteinte) : sans gravité.
                if self._stop:
                    return
                continue
            self._dispatch(pkt, addr)

    def _dispatch(self, pkt: bytes, addr: tuple):
        r = parse_link_payload(pkt)
        if r is None:
            self.bad_frames += 1
            return
        node, seq, data = r
        if node == 0:                 # notre propre broadcast, ou un autre PC
            return
        if self.peers.get(node) != addr:
            self.peers[node] = addr
            via = " (relais ESP-Pont)" if addr == self.relay else ""
            self._put({"type": SER_EVT_LOG, "time": time.time(),
                       "text": f"DS{node} connectée via {addr[0]}:{addr[1]}{via}"})
        self._put({"type": SER_EVT_RX_LINK, "time": time.time(), "rssi": 0,
                   "node": node, "seq": seq, "data": data, "addr": addr})

    def _put(self, ev: dict):
        try:
            self.events.put_nowait(ev)
        except queue.Full:
            pass


# --------------------------------------------------------------------------
# Hub : faire jouer ensemble des DS de transports différents
# --------------------------------------------------------------------------
class DsLinkHub:
    """Relie plusieurs transports pour que toutes les DS se parlent :

        vraie DS ~~ondes~~ ESP32 (DsLinkBridge) ─┐
        vraie DS ~~Wi-Fi~~ ESP-Pont (relais)  ───┤
        melonDS (DS émulée) ─────────────────────┴─ DsLinkUdp

    Chaque message reçu d'une DS repart vers toutes les autres, avec son
    numéro de nœud et de séquence d'origine : pour le jeu, c'est comme si les
    consoles étaient à côté. Seule exception, un message venu des ondes n'y
    est pas réémis : les DS à portée de l'ESP32 l'ont déjà entendu.

    self.events reçoit les événements de tous les transports, chacun marqué
    par la clé "bridge". Chaque DS doit avoir un numéro de nœud différent.
    """

    def __init__(self, bridges: list):
        self.bridges = bridges
        self.events: queue.Queue = queue.Queue(maxsize=20000)
        self.forwarded = 0
        self._stop = False
        self._threads = [threading.Thread(target=self._pump, args=(br,), daemon=True)
                         for br in bridges]
        for t in self._threads:
            t.start()

    def _pump(self, br):
        while not self._stop:
            try:
                ev = br.events.get(timeout=0.2)
            except queue.Empty:
                continue
            ev["bridge"] = br
            if ev["type"] == SER_EVT_RX_LINK:
                self._forward(br, ev)
            try:
                self.events.put_nowait(ev)
            except queue.Full:
                pass

    def _forward(self, src, ev):
        for br in self.bridges:
            if br is src and br.kind == "radio":
                continue
            try:
                if br.kind == "udp":
                    br.send_link(ev["data"], node=ev["node"], seq=ev["seq"],
                                 exclude=ev["node"] if br is src else None)
                else:
                    br.send_link(ev["data"], node=ev["node"], seq=ev["seq"])
                self.forwarded += 1
            except OSError:
                pass

    def send_link(self, data: bytes):
        """Message du PC (nœud 0) vers toutes les DS de tous les transports."""
        for br in self.bridges:
            br.send_link(data)

    def reply(self, ev: dict, data: bytes):
        """Répond, au nom du PC, à la DS qui a émis l'événement ev."""
        br = ev["bridge"]
        if br.kind == "udp":
            br.send_link_to(ev["node"], data)
        else:
            br.send_link(data)

    def close(self):
        self._stop = True
        for t in self._threads:
            t.join(timeout=1)
        for br in self.bridges:
            br.close()


# --------------------------------------------------------------------------
# PCAP (radiotap) — lisible par Wireshark
# --------------------------------------------------------------------------
LINKTYPE_IEEE802_11 = 105
LINKTYPE_RADIOTAP = 127


def radiotap_header(channel: int, rssi: int) -> bytes:
    # présent : bit 3 (CHANNEL) + bit 5 (DBM_ANTSIGNAL)
    freq = 2407 + 5 * channel if channel < 14 else 2484
    return struct.pack("<BBHI", 0, 0, 13, (1 << 3) | (1 << 5)) + \
        struct.pack("<HHb", freq, 0x00A0, rssi)


class PcapWriter:
    def __init__(self, path: str):
        self.f = open(path, "wb")
        self.f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535,
                                 LINKTYPE_RADIOTAP))

    def write(self, frame: bytes, channel: int, rssi: int, t: float):
        pkt = radiotap_header(channel, rssi) + frame
        sec = int(t)
        self.f.write(struct.pack("<IIII", sec, int((t - sec) * 1e6), len(pkt), len(pkt)))
        self.f.write(pkt)
        self.f.flush()

    def close(self):
        self.f.close()


def read_pcap(path: str):
    """Rend des tuples (temps, rssi|None, trame 802.11)."""
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        nano = magic in (0xA1B23C4D, 0x4D3CB2A1)
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            sec, frac, incl, _ = struct.unpack(endian + "IIII", rec)
            pkt = f.read(incl)
            t = sec + frac / (1e9 if nano else 1e6)
            rssi = None
            if linktype == LINKTYPE_RADIOTAP:
                rt_len = struct.unpack_from("<H", pkt, 2)[0]
                if rt_len == 13:
                    rssi = struct.unpack_from("b", pkt, 12)[0]
                pkt = pkt[rt_len:]
            elif linktype != LINKTYPE_IEEE802_11:
                raise SystemExit(f"Type de lien pcap non géré : {linktype}")
            yield t, rssi, pkt


# --------------------------------------------------------------------------
# Analyse des trames Nintendo
# --------------------------------------------------------------------------
def find_nintendo_ie(body: bytes) -> bytes | None:
    i = 0
    while i + 2 <= len(body):
        eid, ln = body[i], body[i + 1]
        if i + 2 + ln > len(body):
            return None
        if eid == 0xDD and ln >= 3 and body[i + 2:i + 5] == b"\x00\x09\xbf":
            return body[i + 2:i + 2 + ln]
        i += 2 + ln
    return None


def classify(f: bytes) -> str:
    if len(f) < 24:
        return "court"
    a1 = f[4:10]
    if f[0] == 0x80:
        return "beacon-nintendo" if find_nintendo_ie(f[36:]) else "beacon"
    if a1 == MP_CMD:
        return "mp-cmd"
    if a1 == MP_REPLY:
        return "mp-reply"
    if a1 == MP_ACK:
        return "mp-ack"
    if f[0] == 0xD4:
        return "ack-80211"
    ftype = (f[0] >> 2) & 3
    return {0: "gestion", 1: "controle", 2: "donnees"}.get(ftype, "autre")


def analyze(path: str):
    counts: Counter = Counter()
    beacons: dict[str, bytes] = {}
    last_cmd_t = None
    cmd_periods, reply_delays = [], []
    first_t = None
    by_src: dict[str, Counter] = defaultdict(Counter)

    for t, rssi, f in read_pcap(path):
        first_t = first_t if first_t is not None else t
        kind = classify(f)
        counts[kind] += 1
        if len(f) >= 16:
            by_src[f[10:16].hex(":")][kind] += 1
        if kind == "beacon-nintendo":
            bssid = f[16:22].hex(":")
            beacons.setdefault(bssid, find_nintendo_ie(f[36:]))
        elif kind == "mp-cmd":
            if last_cmd_t is not None:
                cmd_periods.append(t - last_cmd_t)
            last_cmd_t = t
        elif kind == "mp-reply" and last_cmd_t is not None:
            reply_delays.append(t - last_cmd_t)

    print(f"Capture : {path}")
    print("\nTrames par catégorie :")
    for k, v in counts.most_common():
        print(f"  {k:<16} {v}")

    print(f"\nHôtes Nintendo détectés (beacons avec IE 00:09:BF) : {len(beacons)}")
    for bssid, ie in beacons.items():
        print(f"  BSSID {bssid}  IE ({len(ie)} o) : {ie.hex(' ')}")

    def stats(name, xs):
        if not xs:
            return
        xs = sorted(xs)
        med = xs[len(xs) // 2]
        print(f"  {name:<34} n={len(xs):<6} médiane={med * 1e3:8.3f} ms  "
              f"min={xs[0] * 1e3:8.3f}  max={xs[-1] * 1e3:8.3f}")

    print("\nTiming (horodatage ESP32, précision ~quelques dizaines de µs) :")
    stats("Période entre trames MP-CMD", cmd_periods)
    stats("Délai MP-CMD -> réponse client", reply_delays)

    print("\nÉmetteurs :")
    for src, c in sorted(by_src.items(), key=lambda kv: -sum(kv[1].values()))[:10]:
        print(f"  {src}  " + ", ".join(f"{k}={v}" for k, v in c.most_common()))


# --------------------------------------------------------------------------
# Commandes CLI
# --------------------------------------------------------------------------
def wait_pong(br: DsLinkBridge, timeout: float = 3.0):
    br.ping()
    end = time.time() + timeout
    while time.time() < end:
        try:
            ev = br.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if ev["type"] == SER_EVT_PONG:
            print(f"ESP32 OK — firmware v{ev['fw']}, MAC {ev['mac']}")
            return True
    print("Pas de réponse de l'ESP32 (port, débit, firmware ?)")
    return False


def demo_pong(ev: dict) -> bytes | None:
    """Réponse du PC à un message de la démo DS, ou None.
    'P'[ticks] -> 'p'[ticks][nœud visé] : le nœud visé évite qu'une autre DS,
    qui reçoit aussi le pong, en tire un faux temps d'aller-retour."""
    d = ev["data"]
    if d[:1] == b"P" and len(d) >= 5:
        return b"p" + d[1:5] + bytes([ev["node"]])
    return None


def open_bridges(a) -> list:
    """Ouvre les transports demandés : radio (--port) et/ou UDP (--udp, --relay)."""
    bridges = []
    try:
        if a.port:
            br = DsLinkBridge(a.port, a.baud)
            bridges.append(br)
            if not wait_pong(br):
                raise SystemExit(1)
            br.set_channel(a.channel)
            br.set_mode(MODE_LINK)
            print(f"Radio : canal {a.channel}. Sur les vraies DS de l'ESP32, choisissez A.")
        if a.udp or a.relay:
            br = DsLinkUdp(a.udp_port, relay=a.relay)
            bridges.append(br)
            print(f"Réseau : écoute UDP sur le port {br.port}. Dans melonDS, choisissez B.")
            if br.relay:
                print(f"Inscription auprès d'ESP-Pont {br.relay[0]}:{br.relay[1]}. "
                      "Sur les vraies DS d'ESP-Pont, choisissez B.")
    except BaseException:
        for br in bridges:
            br.close()
        raise
    if not bridges:
        raise SystemExit("Indiquez --port COMx (pont radio ESP32), --udp (melonDS), "
                         "--relay esp-pont.local (ESP-Pont), ou plusieurs à la fois.")
    return bridges


def cmd_demo(a):
    hub = DsLinkHub(open_bridges(a))
    if len(hub.bridges) > 1:
        print("Hub actif : les DS des différents transports jouent ensemble.")
    print("Tapez un texte + Entrée pour l'envoyer aux DS. Ctrl+C pour quitter.\n")

    def stdin_loop():
        for line in sys.stdin:
            hub.send_link(b"T" + line.strip().encode()[:63])
    threading.Thread(target=stdin_loop, daemon=True).start()

    last_state: dict[int, tuple] = {}
    try:
        while True:
            ev = hub.events.get()
            if ev["type"] == SER_EVT_RX_LINK:
                d = ev["data"]
                pong = demo_pong(ev)
                if pong:
                    hub.reply(ev, pong)
                elif d[:1] == b"S" and len(d) >= 9:
                    keys, frame = struct.unpack_from("<II", d, 1)
                    if last_state.get(ev["node"], (None,))[0] != keys:
                        where = (f"rssi={ev['rssi']} dBm" if ev["bridge"].kind == "radio"
                                 else f"via {ev['addr'][0]}:{ev['addr'][1]}")
                        print(f"DS{ev['node']}  touches=0x{keys:04X}  image={frame}  {where}")
                    last_state[ev["node"]] = (keys, frame)
            elif ev["type"] == SER_EVT_LOG:
                print(f"[{ev['bridge'].kind}] {ev['text']}")
    except KeyboardInterrupt:
        pass
    finally:
        hub.close()


def cmd_sniff(a):
    br = DsLinkBridge(a.port, a.baud)
    if not wait_pong(br):
        return
    br.set_channel(a.channel)
    br.set_mode(MODE_NINTENDO if a.nintendo else MODE_SNIFF)
    w = PcapWriter(a.out)
    n = 0
    # Horodatage : on utilise l'horloge µs de l'ESP32 (précise), pas l'heure
    # d'arrivée sur le PC (le port série regroupe les trames par paquets).
    base_pc = base_ts = last_ts = None
    wraps = 0
    print(f"Capture canal {a.channel} -> {a.out}  (Ctrl+C pour arrêter)")
    try:
        while True:
            ev = br.events.get()
            if ev["type"] == SER_EVT_RX_RAW:
                ts = ev["ts_us"]
                if base_ts is None:
                    base_pc, base_ts = ev["time"], ts
                elif ts < last_ts:
                    wraps += 1                       # compteur 32 bits : ~71 min
                last_ts = ts
                t = base_pc + ((ts + (wraps << 32)) - base_ts) / 1e6
                w.write(ev["frame"], ev["channel"], ev["rssi"], t)
                n += 1
                if a.verbose:
                    print(f"{classify(ev['frame']):<16} {len(ev['frame']):5d} o  "
                          f"{ev['rssi']} dBm")
            elif ev["type"] == SER_EVT_STATS:
                print(f"\r{n} trames écrites | ESP32 rx={ev['rx']} perdues={ev['drop']}",
                      end="", flush=True)
    except KeyboardInterrupt:
        print()
    finally:
        w.close()
        br.close()
    analyze(a.out)


def cmd_replay_beacon(a):
    beacons = [f for _, _, f in read_pcap(a.capture) if classify(f) == "beacon-nintendo"]
    if not beacons:
        raise SystemExit("Aucun beacon Nintendo dans cette capture.")
    frame = beacons[min(a.index, len(beacons) - 1)]
    br = DsLinkBridge(a.port, a.baud)
    if not wait_pong(br):
        return
    br.set_channel(a.channel)
    br.set_mode(MODE_NINTENDO)
    br.beacon_start(frame, a.interval)
    print(f"Rejeu du beacon BSSID {frame[16:22].hex(':')} toutes les {a.interval} ms.")
    print("Éteignez la DS hôte d'origine et regardez si la DS cliente liste la partie.")
    print("Ctrl+C pour arrêter.")
    try:
        while True:
            ev = br.events.get()
            if ev["type"] == SER_EVT_RX_RAW:
                k = classify(ev["frame"])
                if k != "beacon-nintendo":
                    print(f"  réaction : {k:<12} de {ev['frame'][10:16].hex(':')}")
    except KeyboardInterrupt:
        pass
    finally:
        br.beacon_stop()
        time.sleep(0.2)
        br.close()


def cmd_ports(_a):
    from serial.tools import list_ports
    for p in list_ports.comports():
        print(f"{p.device:<20} {p.description}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_port(p, required=True):
        p.add_argument("--port", required=required, help="ex. COM5 ou /dev/ttyUSB0")
        p.add_argument("--baud", type=int, default=921600)
        p.add_argument("--channel", type=int, default=7)

    sub.add_parser("ports").set_defaults(fn=cmd_ports)

    p = sub.add_parser("demo"); add_port(p, required=False)
    p.add_argument("--udp", action="store_true",
                   help="mode réseau, sans ESP32 (melonDS, ESP-Pont)")
    p.add_argument("--udp-port", type=int, default=LINK_UDP_PORT)
    p.add_argument("--relay", metavar="HOTE",
                   help="ESP-Pont à utiliser comme relais (ex. esp-pont.local) ; implique --udp")
    p.set_defaults(fn=cmd_demo)

    p = sub.add_parser("sniff"); add_port(p)
    p.add_argument("-o", "--out", default="capture.pcap")
    p.add_argument("--nintendo", action="store_true", help="filtre multijoueur Nintendo")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_sniff)

    p = sub.add_parser("analyze"); p.add_argument("capture")
    p.set_defaults(fn=lambda a: analyze(a.capture))

    p = sub.add_parser("replay-beacon"); add_port(p); p.add_argument("capture")
    p.add_argument("--index", type=int, default=0)
    p.add_argument("--interval", type=int, default=100, help="ms")
    p.set_defaults(fn=cmd_replay_beacon)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
