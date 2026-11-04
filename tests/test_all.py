#!/usr/bin/env python3
"""Tests hors matériel : C <-> Python, pcap, analyse. Lancer : python tests/test_all.py"""
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "pc-host"))
import dslink  # noqa: E402

BIN = os.path.join(tempfile.mkdtemp(), "c_helper")
subprocess.check_call(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "common"),
                       os.path.join(HERE, "c_helper.c"), "-o", BIN])


def c(*args):
    return subprocess.check_output([BIN, *args], text=True).strip()


rng = random.Random(1)
fails = 0


def check(name, cond):
    global fails
    if not cond:
        fails += 1
        print("ÉCHEC", name)


# 1) COBS + CRC identiques C/Python, y compris cas limites (zéros, 254/255 octets)
cases = [b"", b"\x00", b"\x00\x00", b"\x11\x00\x22", bytes(range(1, 255)),
         bytes(range(1, 256)), bytes([1] * 600)]
cases += [bytes(rng.randrange(0, 4) and rng.randrange(256) for _ in range(rng.randrange(1, 900)))
          for _ in range(200)]
for data in cases:
    body = data + struct.pack("<H", dslink.crc16_ccitt(data))
    py = dslink.cobs_encode(body)
    out = c("cobs", data.hex())
    check(f"cobs len={len(data)}", out == py.hex())
    check("no zero in cobs", 0 not in py)
    check("py roundtrip", dslink.cobs_decode(py) == body)

# 2) CRC de référence CCITT-FALSE
check("crc ref", dslink.crc16_ccitt(b"123456789") == 0x29B1)

# 3) Trame air : construite en C, relue en C
payload = b"S" + bytes(range(8))
frame = bytes.fromhex(c("link", payload.hex()))
check("link fc", frame[:2] == b"\x08\x00")
check("link bssid", frame[16:22] == bytes([0x02]) + b"DSLNK")
check("link parse", c("parse", frame.hex()) == f"ok 3 42 {len(payload)} {payload.hex()}")
check("link parse + FCS", c("parse", (frame + b"\xde\xad\xbe\xef").hex()).startswith("ok"))
bad = bytearray(frame); bad[17] ^= 1
check("link reject bssid", c("parse", bytes(bad).hex()) == "ko")
check("link reject court", c("parse", frame[:30].hex()) == "ko")
# Longueur annoncée > LINK_MAX_DATA (400) : rejet, même si la trame est assez longue
big = bytearray(bytes.fromhex(c("link", (b"x" * 400).hex())) + b"\x00" * 200)
struct.pack_into("<H", big, 24 + 8, 500)
check("link reject len > max", c("parse", bytes(big).hex()) == "ko")

# 4) Décodage d'un événement RX_LINK côté Python (comme envoyé par l'ESP32)
hdr = dslink.LINK_HEADER.pack(dslink.LINK_MAGIC, 1, 3, 42, len(payload))
ev_raw = bytes([dslink.SER_EVT_RX_LINK]) + struct.pack("b", -51) + hdr + payload
wire = dslink.cobs_encode(ev_raw + struct.pack("<H", dslink.crc16_ccitt(ev_raw)))


class FakeBridge(dslink.DsLinkBridge):
    def __init__(self):  # pas de port série
        import queue
        self.events = queue.Queue()
        self.bad_frames = 0


fb = FakeBridge()
fb._dispatch(wire)
ev = fb.events.get_nowait()
check("evt rx_link", ev["node"] == 3 and ev["seq"] == 42 and ev["data"] == payload and ev["rssi"] == -51)
fb._dispatch(wire[:-1] + b"\x01")
check("crc rejette", fb.bad_frames == 1)

# 4 bis) Mode réseau (UDP, melonDS) : message C relu en Python, réponse Python relue en C
import socket  # noqa: E402
import time  # noqa: E402

udp_msg = frame[24:]                                  # même message, sans en-tête 802.11
check("udp parse C", c("parsep", udp_msg.hex()) == f"ok 3 42 {len(payload)} {payload.hex()}")
check("udp parse py", dslink.parse_link_payload(udp_msg) == (3, 42, payload))
check("udp reject len > max", dslink.parse_link_payload(bytes(big[24:])) is None)

srv = dslink.DsLinkUdp(port=0, bind="127.0.0.1")
ds = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ds.bind(("127.0.0.1", 0))
ds.settimeout(2)
ds.sendto(udp_msg, ("127.0.0.1", srv.port))          # la "DS" (nœud 3) parle en premier
evs = []
end = time.time() + 2
while time.time() < end and not any(e["type"] == dslink.SER_EVT_RX_LINK for e in evs):
    try:
        evs.append(srv.events.get(timeout=0.2))
    except Exception:
        pass
rx = [e for e in evs if e["type"] == dslink.SER_EVT_RX_LINK]
check("udp evt rx_link", bool(rx) and rx[0]["node"] == 3 and rx[0]["data"] == payload)
check("udp peer appris", srv.peers.get(3) == ds.getsockname())
srv.send_link(b"Tbonjour")                            # le PC répond à l'adresse apprise
try:
    reply, _ = ds.recvfrom(2048)
except socket.timeout:
    reply = b""
check("udp reponse relue en C", c("parsep", reply.hex()).startswith("ok 0 0 8 " + b"Tbonjour".hex()))
ds.close()
srv.close()

# 4 ter) Relais ESP-Pont simulé : le PC s'inscrit par un message vide du nœud 0,
# les messages des DS arrivent depuis l'adresse du relais, et les réponses y repartent.
relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
relay.bind(("127.0.0.1", 0))
relay.settimeout(3)
srv = dslink.DsLinkUdp(port=0, bind="127.0.0.1", relay="127.0.0.1:%d" % relay.getsockname()[1])
try:
    hello, pc_addr = relay.recvfrom(2048)
except socket.timeout:
    hello, pc_addr = b"", None
check("relais: inscription", dslink.parse_link_payload(hello) is not None
      and dslink.parse_link_payload(hello)[0] == 0 and dslink.parse_link_payload(hello)[2] == b"")
if pc_addr:
    relay.sendto(udp_msg, pc_addr)                    # message de la DS 3, relayé
    rx = None
    end = time.time() + 2
    while time.time() < end and rx is None:
        try:
            e = srv.events.get(timeout=0.2)
        except Exception:
            continue
        if e["type"] == dslink.SER_EVT_RX_LINK:
            rx = e
    check("relais: DS reçue", rx is not None and rx["node"] == 3 and rx["addr"] == srv.relay)
    srv.send_link(b"Tvia")
    got = False
    end = time.time() + 3
    while time.time() < end and not got:              # ignorer les messages vides de maintien
        try:
            pkt, _ = relay.recvfrom(2048)
        except socket.timeout:
            break
        r = dslink.parse_link_payload(pkt)
        got = r is not None and r[2] == b"Tvia"
    check("relais: réponse au relais", got)
srv.close()
relay.close()

# 5) PCAP + analyse sur une capture synthétique "Nintendo"
def mgmt_beacon(bssid):
    hdr = b"\x80\x00\x00\x00" + b"\xff" * 6 + bssid + bssid + b"\x00\x00"
    fixed = b"\x00" * 8 + b"\x64\x00" + b"\x21\x00"
    ies = b"\x00\x00" + b"\x01\x02\x82\x84" + b"\xdd\x08\x00\x09\xbf\x00\x0a\x00\x00\x00"
    return hdr + fixed + ies


host = bytes.fromhex("0009bf123456")
client = bytes.fromhex("001656abcdef")
path = os.path.join(tempfile.mkdtemp(), "t.pcap")
w = dslink.PcapWriter(path)
t = 1000.0
w.write(mgmt_beacon(host), 7, -40, t)
for i in range(50):
    t += 0.0167
    w.write(b"\x28\x02\x00\x00" + dslink.MP_CMD + host + host + b"\x00\x00" + b"\x01" * 20, 7, -40, t)
    w.write(b"\x18\x01\x00\x00" + dslink.MP_REPLY + client + host + b"\x00\x00" + b"\x02" * 10, 7, -55, t + 0.0004)
    w.write(b"\x18\x02\x00\x00" + dslink.MP_ACK + host + host + b"\x00\x00" + b"\x00" * 4, 7, -40, t + 0.0008)
w.close()

recs = list(dslink.read_pcap(path))
check("pcap count", len(recs) == 151)
check("pcap rssi", recs[0][1] == -40)
kinds = [dslink.classify(f) for _, _, f in recs]
check("classify", kinds.count("beacon-nintendo") == 1 and kinds.count("mp-cmd") == 50
      and kinds.count("mp-reply") == 50 and kinds.count("mp-ack") == 50)
print("--- sortie de l'analyseur sur la capture synthétique ---")
dslink.analyze(path)
print("--------------------------------------------------------")

print(f"\n{len(cases)} cas COBS testés. {'TOUT OK' if not fails else f'{fails} échec(s)'}")
sys.exit(1 if fails else 0)
