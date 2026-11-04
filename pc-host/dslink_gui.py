#!/usr/bin/env python3
"""
dslink_gui.py — interface graphique de ds-pc-link.

Lancer :  python pc-host/dslink_gui.py

Chaque onglet active une fonction du projet et affiche son mode d'emploi :
  Accueil       vue d'ensemble, choix du parcours
  Liaison DS    démo DS <-> PC : pont radio ESP32, melonDS (UDP), ESP-Pont (relais)
  ESP-Pont      état du boîtier, accès à son interface web
  Capture       capture Wi-Fi vers un fichier .pcap (pont radio ESP32)
  Analyse       analyse d'une capture .pcap
  Rejeu         rejeu d'un beacon Nintendo capturé (pont radio ESP32)

Aucune dépendance en plus de Python (Tkinter est inclus dans l'installateur de
python.org), sauf pyserial pour les fonctions qui passent par le pont radio
ESP32 : pip install pyserial
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import queue
import re
import struct
import sys
import textwrap
import threading
import time
import tkinter as tk
import urllib.error
import urllib.request
import webbrowser
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dslink as dl  # noqa: E402

WINDOWS = sys.platform == "win32"
BASE_FONT = ("Segoe UI", 10) if WINDOWS else ("Helvetica", 10)
MONO_FONT = ("Consolas", 10) if WINDOWS else ("Courier", 10)

# Touches de la DS (masque keysHeld() de libnds)
KEY_NAMES = [(1, "A"), (2, "B"), (4, "SELECT"), (8, "START"), (16, "→"), (32, "←"),
             (64, "↑"), (128, "↓"), (256, "R"), (512, "L"), (1024, "X"), (2048, "Y"),
             (4096, "STYLET")]


def keys_text(k: int) -> str:
    return " ".join(n for b, n in KEY_NAMES if k & b) or "—"


def have_serial() -> bool:
    try:
        import serial  # noqa: F401
        return True
    except ImportError:
        return False


def serial_ports() -> list[str]:
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    return [p.device for p in list_ports.comports()]


def run_analyze(path: str) -> str:
    """Lance dslink.analyze() et rend sa sortie texte."""
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            dl.analyze(path)
    except SystemExit as e:
        buf.write(f"\n{e}\n")
    except OSError as e:
        buf.write(f"Lecture impossible : {e}\n")
    return buf.getvalue()


# ==========================================================================
# Textes d'aide
# ==========================================================================
GUIDE_HOME = """
# Bienvenue dans ds-pc-link
Ce logiciel relie une Nintendo DS, vraie ou émulée, à ce PC. Chaque onglet active une fonction du projet. Le panneau « Mode d'emploi » de chaque onglet explique comment s'en servir.

## Avant de commencer
- Compilez la démo DS `dslink_demo.nds` (voir `INSTALL.md`, étape 2). Copiez-la sur la carte de votre linker, ou ouvrez-la dans melonDS.
- Pour le pont radio ESP32 et les outils de capture : `pip install pyserial`, puis relancez ce logiciel.

## Choisir son parcours
- melonDS : aucun matériel. Onglet Liaison DS, mode « melonDS ». Sur la DS : touche B.
- Vraie DS + ESP-Pont (recommandé) : onglet ESP-Pont pour vérifier le boîtier, puis Liaison DS, mode « ESP-Pont ». Sur la DS : touche B.
- Vraie DS + pont radio ESP32 : onglet Liaison DS, mode « Pont radio ESP32 ». Sur la DS : touche A.
Les boutons en bas de cette page ouvrent directement le bon réglage.

## Les onglets
- Liaison DS : démarre la démo. Les DS connectées s'affichent avec leurs touches, et vous pouvez leur envoyer du texte.
- ESP-Pont : lit l'état du boîtier, signale les réglages incompatibles avec la DS, et ouvre son interface web.
- Capture, Analyse, Rejeu : outils de recherche sur le multijoueur des cartouches d'origine. Capture et Rejeu demandent le pont radio ESP32 ; Analyse fonctionne sans matériel.

## Dans la démo, sur la DS
- Au démarrage : A = radio directe (pont ESP32), B = réseau (melonDS, ESP-Pont).
- Ensuite : A mesure l'aller-retour avec le PC (ligne RTT), START quitte.
- Le texte envoyé depuis l'onglet Liaison DS s'affiche sur la ligne « PC: ».
- Plusieurs consoles : chacune a besoin d'une démo compilée avec un `NODE` différent.
"""

GUIDE_SERIAL = """
# Pont radio ESP32
Vraie DS, latence minimale. Un ESP32 flashé avec `esp32-bridge` est branché en USB à ce PC. La DS ne se connecte à aucun réseau : elle émet ses messages directement sur un canal Wi-Fi.

## Étapes
1. Branchez l'ESP32, cliquez « Actualiser » et choisissez son port.
2. Gardez le canal 7, sauf si vous l'avez changé dans la démo DS.
3. Cliquez « Démarrer ». Le journal doit afficher `ESP32 OK`.
4. Lancez `dslink_demo.nds` sur la DS et appuyez sur A.

## Ensuite
- La DS apparaît dans la liste dès son premier message, avec ses touches et le signal reçu.
- Tapez un texte en bas et cliquez « Envoyer » : il s'affiche sur la DS.
- La ligne d'état indique les compteurs de l'ESP32 (trames reçues, perdues, émises).

## En cas de souci
- « Pas de réponse de l'ESP32 » : mauvais port, ou firmware pas flashé. Fermez tout autre logiciel qui utilise le port (moniteur série de l'IDE Arduino, autre onglet de ce logiciel).
- Aucune DS : vérifiez que la DS a choisi A, et que le canal est le même des deux côtés.
"""

GUIDE_UDP = """
# melonDS
Aucun matériel. melonDS simule un point d'accès Wi-Fi (melonAP) relié à ce PC. La démo s'y connecte et échange en UDP.

## Étapes
1. Dans melonDS, gardez le firmware intégré (par défaut). Son profil Wi-Fi connaît déjà melonAP.
2. Dans melonDS, Config > Wi-Fi settings : mode indirect.
3. Cliquez « Démarrer ». Si Windows demande d'autoriser Python sur le réseau, acceptez pour les réseaux privés.
4. Ouvrez `dslink_demo.nds` dans melonDS et appuyez sur B.
5. Après quelques secondes de connexion, la DS affiche `PC connecte` et apparaît dans la liste.

## En cas de souci
- La DS reste « en attente... » : la recherche automatique n'atteint pas ce PC. Mettez l'IP de ce PC dans `PC_IP` (`ds-homebrew/source/main.c`), recompilez, relancez.
- « Echec : aucun point d'acces » : vérifiez le firmware intégré et le Wi-Fi de melonDS.
- Le port UDP doit être le même que celui de la démo (47474 par défaut).
"""

GUIDE_RELAY = """
# ESP-Pont (relais)
Vraie DS, sans câble. ESP-Pont fournit à la DS un réseau ouvert en 802.11b et relaie ses messages jusqu'à ce PC, qui reste sur le réseau de votre box.

## Étapes
1. Dans l'onglet ESP-Pont, vérifiez le boîtier : liaison amont établie, point d'accès Ouvert en 802.11b, relais actif.
2. Indiquez son adresse : `esp-pont.local`, ou l'adresse amont affichée dans l'onglet ESP-Pont.
3. Cliquez « Démarrer ». Ce PC s'inscrit auprès du relais chaque seconde.
4. Sur la DS, le réseau d'ESP-Pont doit être enregistré dans les réglages Wi-Fi Nintendo (depuis un jeu compatible WFC). Lancez la démo et appuyez sur B.
5. La DS affiche `PC connecte` et apparaît dans la liste, « via relais ».

## En cas de souci
- « Relais introuvable » : ce PC ne résout pas `esp-pont.local`. Utilisez l'adresse amont.
- La DS reste « en attente... » : l'onglet ESP-Pont doit montrer ce PC inscrit. Sinon, vérifiez l'adresse et le pare-feu de Windows.
- La DS ne trouve pas le réseau : sécurité Ouvert, protocole 802.11b, SSID non masqué.
"""

GUIDE_ESPONT = """
# ESP-Pont
Le boîtier qui donne à une vraie DS un réseau qu'elle comprend (ouvert, 802.11b), le route vers votre box, et relaie la liaison dslink.

## Première configuration
1. Alimentez le boîtier. Un réseau ouvert `ESP-Pont-Setup` apparaît.
2. Connectez-y ce PC, puis cliquez « Configuration initiale » (http://192.168.4.1).
3. Liaison amont : choisissez votre box et saisissez sa clé.
4. Point d'accès : un nom, sécurité Ouvert. Radio : 802.11b. Système : relais dslink coché.
5. Enregistrez, puis reconnectez ce PC à votre box.

## Vérifier le boîtier
1. Indiquez son adresse (`esp-pont.local` ou son adresse amont) et cliquez « Lire l'état ».
2. La section Diagnostic signale ce qui empêcherait la DS de fonctionner.
3. « Utiliser ce relais » prépare l'onglet Liaison DS avec cette adresse.

## Bon à savoir
- Le boîtier oublie le réseau de la box à chaque redémarrage : resélectionnez-le dans l'interface web (la clé reste enregistrée).
- Si un mot de passe protège l'interface web, l'état ne peut pas être lu ici : ouvrez l'interface web.
- Bouton FLASH : 3 s pour le réseau de secours `ESP-Pont-Secours`, 10 s pour tout effacer.
"""

GUIDE_CAPTURE = """
# Capture Wi-Fi
Enregistre tout ce qui passe sur un canal dans un fichier .pcap lisible par Wireshark. C'est la phase 1 du travail sur les cartouches d'origine : observer deux vraies DS qui jouent ensemble. Nécessite le pont radio ESP32.

## Étapes
1. Choisissez le port de l'ESP32 et le canal.
2. Cochez « Seulement le multijoueur Nintendo » pour ne garder que les trames utiles.
3. Choisissez le fichier de sortie, puis cliquez « Démarrer ».
4. Lancez une partie multijoueur entre deux vraies DS.
5. Cliquez « Arrêter » : l'analyse s'affiche (hôtes, période des trames, délai de réponse).

## Trouver le canal du jeu
Capturez d'abord sans filtre quelques secondes sur les canaux 1, 7 et 13 : celui où le compteur grimpe pendant la partie est le bon.

## Dans Wireshark
Filtre utile : `wlan.addr[0:3] == 03:09:bf`
"""

GUIDE_ANALYSE = """
# Analyse d'une capture
Relit un fichier .pcap (capturé ici ou ailleurs) et résume le trafic multijoueur Nintendo. Aucun matériel nécessaire.

## Étapes
1. Choisissez le fichier .pcap.
2. Cliquez « Analyser ».

## Ce qu'il faut regarder
- Hôtes Nintendo détectés : les DS qui ont créé une partie, et le contenu de leur élément Nintendo.
- Période entre trames MP-CMD : le rythme imposé par l'hôte.
- Délai MP-CMD -> réponse client : le temps dont dispose un client pour répondre. C'est ce chiffre qui dit si l'ESP32 pourra tenir le rôle.
- Émetteurs : qui parle, et combien.
"""

GUIDE_REPLAY = """
# Rejeu de beacon
Phase 2 du travail sur les cartouches d'origine : l'ESP32 réémet l'annonce de partie (beacon) d'une DS hôte capturée, pour voir si une DS cliente la prend pour une vraie partie. Nécessite le pont radio ESP32.

## Étapes
1. Choisissez une capture contenant un beacon Nintendo (onglet Capture, avec deux vraies DS).
2. Choisissez le port de l'ESP32 et le canal du jeu.
3. Éteignez la DS hôte d'origine.
4. Cliquez « Démarrer ».
5. Sur la DS cliente, cherchez une partie : si elle liste la partie et tente de la rejoindre, ses trames de réaction (authentification, association) s'affichent dans le journal.

## Réglages
- Index : quel beacon rejouer quand la capture en contient plusieurs.
- Intervalle : période d'émission en millisecondes (100 ms, comme une vraie DS).
"""

MODE_GUIDES = {"serial": GUIDE_SERIAL, "udp": GUIDE_UDP, "relay": GUIDE_RELAY}


# ==========================================================================
# Widgets utilitaires
# ==========================================================================
def make_text(parent, **kw) -> tk.Text:
    w = tk.Text(parent, wrap="word", relief="flat", borderwidth=0, padx=8, pady=6,
                font=BASE_FONT, cursor="arrow", **kw)
    w.tag_configure("h1", font=(BASE_FONT[0], 13, "bold"), spacing1=4, spacing3=6)
    w.tag_configure("h2", font=(BASE_FONT[0], 10, "bold"), spacing1=10, spacing3=3)
    w.tag_configure("code", font=MONO_FONT, foreground="#8a4b08")
    w.tag_configure("item", lmargin1=8, lmargin2=24, spacing3=2)
    return w


def set_rich_text(w: tk.Text, text: str):
    """Mise en forme minimale : '# ' titre, '## ' sous-titre, '- ' ou '1. '
    liste, `code` en chasse fixe."""
    w.configure(state="normal")
    w.delete("1.0", "end")
    for line in textwrap.dedent(text).strip("\n").splitlines():
        line = line.rstrip()
        if line.startswith("## "):
            w.insert("end", line[3:] + "\n", ("h2",))
            continue
        if line.startswith("# "):
            w.insert("end", line[2:] + "\n", ("h1",))
            continue
        tags: tuple = ()
        if line.startswith("- "):
            line, tags = "•  " + line[2:], ("item",)
        elif re.match(r"\d+\. ", line):
            tags = ("item",)
        for i, part in enumerate(line.split("`")):
            if part:
                w.insert("end", part, tags + (("code",) if i % 2 else ()))
        w.insert("end", "\n")
    w.configure(state="disabled")


class LogBox(ScrolledText):
    def __init__(self, parent, height=8):
        super().__init__(parent, height=height, wrap="word", font=MONO_FONT, state="disabled")

    def add(self, line: str):
        self.configure(state="normal")
        self.insert("end", time.strftime("%H:%M:%S  ") + line + "\n")
        self.see("end")
        self.configure(state="disabled")

    def set(self, text: str):
        self.configure(state="normal")
        self.delete("1.0", "end")
        self.insert("end", text)
        self.configure(state="disabled")


class PortPicker(ttk.Frame):
    """Choix du port série de l'ESP32."""

    def __init__(self, parent):
        super().__init__(parent)
        self.var = tk.StringVar()
        self.cb = ttk.Combobox(self, textvariable=self.var, width=18)
        self.cb.pack(side="left")
        ttk.Button(self, text="Actualiser", command=self.refresh).pack(side="left", padx=(6, 0))
        self.note = ttk.Label(self, foreground="#a03030")
        self.note.pack(side="left", padx=(8, 0))
        self.refresh()

    def refresh(self):
        if not have_serial():
            self.note.configure(text="pyserial absent : pip install pyserial")
            self.cb["values"] = []
            return
        ports = serial_ports()
        self.cb["values"] = ports
        self.note.configure(text="" if ports else "aucun port détecté")
        if ports and self.var.get() not in ports:
            self.var.set(ports[0])

    def get(self) -> str:
        return self.var.get().strip()


def labeled(parent, row, text, widget, **grid):
    ttk.Label(parent, text=text).grid(row=row, column=0, sticky="w", padx=(0, 10), pady=3)
    widget.grid(row=row, column=1, sticky="w", pady=3, **grid)
    return widget


def int_of(var, default: int) -> int:
    try:
        return int(var.get())
    except (ValueError, tk.TclError):
        return default


# ==========================================================================
# Sessions en arrière-plan
# ==========================================================================
class Worker:
    """Une session (liaison, capture, rejeu) dans un thread. Elle ne touche
    jamais l'interface : elle lui poste des messages (type, contenu)."""

    def __init__(self, post):
        self.post = post
        self.stop_evt = threading.Event()

    def start(self):
        threading.Thread(target=self._safe_run, daemon=True).start()

    def stop(self):
        self.stop_evt.set()

    def _safe_run(self):
        try:
            self.run()
        except SystemExit as e:
            self.post("log", str(e))
        except Exception as e:  # noqa: BLE001 — tout doit remonter à l'utilisateur
            self.post("log", f"Erreur : {e}")
        finally:
            self.post("ended")

    def run(self):
        raise NotImplementedError

    def events(self, br):
        """Itère sur les événements du pont jusqu'à l'arrêt demandé."""
        while not self.stop_evt.is_set():
            try:
                yield br.events.get(timeout=0.2)
            except queue.Empty:
                continue


def open_bridge(port: str, channel: int, mode: int, post, stop_evt) -> dl.DsLinkBridge:
    """Ouvre le pont radio ESP32, vérifie qu'il répond, règle canal et mode."""
    if not have_serial():
        raise RuntimeError("pyserial n'est pas installé : pip install pyserial, puis relancez.")
    if not port:
        raise RuntimeError("Choisissez le port série de l'ESP32.")
    br = dl.DsLinkBridge(port)
    br.ping()
    end = time.time() + 3
    while time.time() < end and not stop_evt.is_set():
        try:
            ev = br.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if ev["type"] == dl.SER_EVT_PONG:
            post("log", f"ESP32 OK — firmware v{ev['fw']}, MAC {ev['mac']}")
            br.set_channel(channel)
            br.set_mode(mode)
            return br
    br.close()
    raise RuntimeError("Pas de réponse de l'ESP32 : vérifiez le port et le firmware esp32-bridge.")


class LinkWorker(Worker):
    def __init__(self, post, mode, port="", channel=7, udp_port=dl.LINK_UDP_PORT, relay=None):
        super().__init__(post)
        self.mode, self.port, self.channel = mode, port, channel
        self.udp_port, self.relay = udp_port, relay
        self.br = None

    def run(self):
        if self.mode == "serial":
            br = open_bridge(self.port, self.channel, dl.MODE_LINK, self.post, self.stop_evt)
        else:
            br = dl.DsLinkUdp(self.udp_port, relay=self.relay if self.mode == "relay" else None)
            self.post("log", f"Écoute UDP sur le port {br.port}.")
            if br.relay:
                self.post("log", f"Inscription auprès d'ESP-Pont {br.relay[0]}:{br.relay[1]}.")
        self.br = br
        self.post("started")
        try:
            for ev in self.events(br):
                t = ev["type"]
                if t == dl.SER_EVT_RX_LINK:
                    d = ev["data"]
                    if d[:1] == b"P":                    # ping -> pong immédiat
                        br.send_link(b"p" + d[1:5])
                        self.post("ping", ev["node"])
                    elif d[:1] == b"S" and len(d) >= 9:
                        keys, frame = struct.unpack_from("<II", d, 1)
                        addr = ev.get("addr")
                        if addr is None:
                            where = f"{ev['rssi']} dBm"
                        elif getattr(br, "relay", None) == addr:
                            where = "via relais ESP-Pont"
                        else:
                            where = f"{addr[0]}:{addr[1]}"
                        self.post("ds", (ev["node"], keys, frame, where))
                elif t == dl.SER_EVT_LOG:
                    self.post("log", ev["text"])
                elif t == dl.SER_EVT_STATS:
                    self.post("stats", ev)
        finally:
            self.br = None
            br.close()

    def send_text(self, text: str) -> bool:
        br = self.br
        if br is None:
            return False
        br.send_link(b"T" + text.encode("utf-8", "replace")[:63])
        return True


class SniffWorker(Worker):
    def __init__(self, post, port, channel, nintendo, out):
        super().__init__(post)
        self.port, self.channel, self.nintendo, self.out = port, channel, nintendo, out

    def run(self):
        mode = dl.MODE_NINTENDO if self.nintendo else dl.MODE_SNIFF
        br = open_bridge(self.port, self.channel, mode, self.post, self.stop_evt)
        w = dl.PcapWriter(self.out)
        n = 0
        # Horodatage : horloge µs de l'ESP32 (précise), comme dslink.py sniff
        base_pc = base_ts = last_ts = None
        wraps = 0
        self.post("started")
        self.post("log", f"Capture canal {self.channel} -> {self.out}")
        try:
            for ev in self.events(br):
                if ev["type"] == dl.SER_EVT_RX_RAW:
                    ts = ev["ts_us"]
                    if base_ts is None:
                        base_pc, base_ts = ev["time"], ts
                    elif ts < last_ts:
                        wraps += 1
                    last_ts = ts
                    t = base_pc + ((ts + (wraps << 32)) - base_ts) / 1e6
                    w.write(ev["frame"], ev["channel"], ev["rssi"], t)
                    n += 1
                elif ev["type"] == dl.SER_EVT_STATS:
                    self.post("stats", (n, ev["rx"], ev["drop"]))
        finally:
            w.close()
            br.close()
        self.post("stats", (n, None, None))
        self.post("log", f"Capture terminée : {n} trames.")
        self.post("analysis", run_analyze(self.out))


class ReplayWorker(Worker):
    def __init__(self, post, port, channel, capture, index, interval):
        super().__init__(post)
        self.port, self.channel, self.capture = port, channel, capture
        self.index, self.interval = index, interval

    def run(self):
        beacons = [f for _, _, f in dl.read_pcap(self.capture) if dl.classify(f) == "beacon-nintendo"]
        if not beacons:
            raise RuntimeError("Aucun beacon Nintendo dans cette capture.")
        frame = beacons[min(self.index, len(beacons) - 1)]
        br = open_bridge(self.port, self.channel, dl.MODE_NINTENDO, self.post, self.stop_evt)
        try:
            br.beacon_start(frame, self.interval)
            self.post("started")
            self.post("log", f"Rejeu du beacon BSSID {frame[16:22].hex(':')} "
                             f"({len(beacons)} dans la capture) toutes les {self.interval} ms.")
            for ev in self.events(br):
                if ev["type"] == dl.SER_EVT_RX_RAW:
                    k = dl.classify(ev["frame"])
                    if k != "beacon-nintendo":
                        self.post("log", f"réaction : {k:<12} de {ev['frame'][10:16].hex(':')}")
        finally:
            br.beacon_stop()
            time.sleep(0.2)
            br.close()


# ==========================================================================
# Onglets
# ==========================================================================
class Tab(ttk.Frame):
    """Onglet : contenu à gauche, mode d'emploi à droite, file de messages."""

    def __init__(self, nb, app, title, guide):
        super().__init__(nb, padding=10)
        nb.add(self, text=title)
        self.app = app
        self.worker: Worker | None = None
        self.q: queue.Queue = queue.Queue()
        self.columnconfigure(0, weight=3)
        self.columnconfigure(1, weight=2)
        self.rowconfigure(0, weight=1)
        self.body = ttk.Frame(self)
        self.body.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        side = ttk.LabelFrame(self, text="Mode d'emploi", padding=4)
        side.grid(row=0, column=1, sticky="nsew")
        self.guide = make_text(side, width=40)
        sb = ttk.Scrollbar(side, command=self.guide.yview)
        self.guide.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        self.guide.pack(fill="both", expand=True)
        set_rich_text(self.guide, guide)
        self.after(100, self._pump)

    def post(self, kind, payload=None):
        self.q.put((kind, payload))

    def _pump(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                self.on_message(kind, payload)
        except queue.Empty:
            pass
        self.after(100, self._pump)

    def on_message(self, kind, payload):
        pass

    def stop_worker(self):
        if self.worker:
            self.worker.stop()


class StartStop(ttk.Frame):
    """Boutons Démarrer / Arrêter + ligne d'état."""

    def __init__(self, parent, on_start, on_stop):
        super().__init__(parent)
        self.b_start = ttk.Button(self, text="Démarrer", command=on_start)
        self.b_stop = ttk.Button(self, text="Arrêter", command=on_stop, state="disabled")
        self.b_start.pack(side="left")
        self.b_stop.pack(side="left", padx=(6, 0))
        self.status = ttk.Label(self, text="arrêté")
        self.status.pack(side="left", padx=(12, 0))

    def running(self, on: bool, text: str | None = None):
        self.b_start.configure(state="disabled" if on else "normal")
        self.b_stop.configure(state="normal" if on else "disabled")
        if text is not None:
            self.status.configure(text=text)


# --------------------------------------------------------------------------
class HomeTab(ttk.Frame):
    def __init__(self, nb, app):
        super().__init__(nb, padding=10)
        nb.add(self, text="Accueil")
        txt = make_text(self)
        sb = ttk.Scrollbar(self, command=txt.yview)
        txt.configure(yscrollcommand=sb.set)
        bar = ttk.Frame(self)
        bar.pack(side="bottom", fill="x", pady=(10, 0))
        sb.pack(side="right", fill="y")
        txt.pack(fill="both", expand=True)
        set_rich_text(txt, GUIDE_HOME)
        ttk.Label(bar, text="Démarrer avec :").pack(side="left", padx=(0, 8))
        ttk.Button(bar, text="melonDS", command=lambda: app.goto_link("udp")).pack(side="left")
        ttk.Button(bar, text="Vraie DS + ESP-Pont",
                   command=lambda: app.goto_link("relay")).pack(side="left", padx=6)
        ttk.Button(bar, text="Vraie DS + pont radio ESP32",
                   command=lambda: app.goto_link("serial")).pack(side="left")


# --------------------------------------------------------------------------
class LinkTab(Tab):
    def __init__(self, nb, app):
        super().__init__(nb, app, "Liaison DS", GUIDE_SERIAL)
        b = self.body
        b.columnconfigure(0, weight=1)
        b.rowconfigure(3, weight=1)

        # 1. Mode
        fm = ttk.LabelFrame(b, text="1. Mode de liaison", padding=8)
        fm.grid(row=0, column=0, sticky="ew")
        self.mode = tk.StringVar(value="serial")
        self.radios = []
        for val, label in (("serial", "Pont radio ESP32 (USB) — vraie DS, touche A sur la DS"),
                           ("udp", "melonDS (UDP) — émulateur, touche B sur la DS"),
                           ("relay", "ESP-Pont (relais) — vraie DS sans câble, touche B")):
            r = ttk.Radiobutton(fm, text=label, value=val, variable=self.mode,
                                command=self._mode_changed)
            r.pack(anchor="w")
            self.radios.append(r)

        # 2. Réglages (un cadre par mode)
        self.fs = ttk.LabelFrame(b, text="2. Réglages", padding=8)
        self.fs.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        self.f_serial = ttk.Frame(self.fs)
        self.port = labeled(self.f_serial, 0, "Port série", PortPicker(self.f_serial))
        self.channel = tk.IntVar(value=7)       # = LINK_DEFAULT_CHANNEL de linkproto.h
        labeled(self.f_serial, 1, "Canal Wi-Fi",
                ttk.Spinbox(self.f_serial, from_=1, to=13, width=5, textvariable=self.channel))
        self.f_udp = ttk.Frame(self.fs)
        self.udp_port = tk.IntVar(value=dl.LINK_UDP_PORT)
        labeled(self.f_udp, 0, "Port UDP",
                ttk.Entry(self.f_udp, width=8, textvariable=self.udp_port))
        self.f_relay = ttk.Frame(self.fs)
        self.relay = tk.StringVar(value="esp-pont.local")
        labeled(self.f_relay, 0, "Adresse d'ESP-Pont",
                ttk.Entry(self.f_relay, width=24, textvariable=self.relay))
        labeled(self.f_relay, 1, "Port UDP",
                ttk.Entry(self.f_relay, width=8, textvariable=self.udp_port))

        # 3. Démarrage + liste des DS
        fr = ttk.LabelFrame(b, text="3. Liaison", padding=8)
        fr.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        self.ss = StartStop(fr, self.start, self.stop)
        self.ss.pack(anchor="w")
        cols = ("ds", "touches", "image", "origine", "vu")
        self.tree = ttk.Treeview(fr, columns=cols, show="headings", height=4)
        for c, t, wd in zip(cols, ("DS", "Touches tenues", "Image", "Signal / origine", "Activité"),
                            (50, 190, 80, 170, 110)):
            self.tree.heading(c, text=t)
            self.tree.column(c, width=wd, anchor="w", stretch=(c == "touches"))
        self.tree.pack(fill="x", pady=(8, 0))
        self.seen: dict[int, float] = {}

        send = ttk.Frame(fr)
        send.pack(fill="x", pady=(8, 0))
        ttk.Label(send, text="Texte vers les DS").pack(side="left")
        self.text = tk.StringVar()
        e = ttk.Entry(send, textvariable=self.text)
        e.pack(side="left", fill="x", expand=True, padx=6)
        e.bind("<Return>", lambda _e: self.send())
        ttk.Button(send, text="Envoyer", command=self.send).pack(side="left")

        # Journal
        fl = ttk.LabelFrame(b, text="Journal", padding=4)
        fl.grid(row=3, column=0, sticky="nsew", pady=(8, 0))
        self.log = LogBox(fl)
        self.log.pack(fill="both", expand=True)

        self._mode_changed()
        self.after(500, self._tick)

    def _mode_changed(self):
        m = self.mode.get()
        for f in (self.f_serial, self.f_udp, self.f_relay):
            f.pack_forget()
        {"serial": self.f_serial, "udp": self.f_udp, "relay": self.f_relay}[m].pack(anchor="w")
        set_rich_text(self.guide, MODE_GUIDES[m])

    def set_mode(self, mode: str, relay: str | None = None):
        if self.worker:
            return
        self.mode.set(mode)
        if relay:
            self.relay.set(relay)
        self._mode_changed()

    def start(self):
        m = self.mode.get()
        self.tree.delete(*self.tree.get_children())
        self.seen.clear()
        self.worker = LinkWorker(self.post, m, port=self.port.get(),
                                 channel=int_of(self.channel, 7),
                                 udp_port=int_of(self.udp_port, dl.LINK_UDP_PORT),
                                 relay=self.relay.get().strip() or None)
        for r in self.radios:
            r.configure(state="disabled")
        self.ss.running(True, "démarrage…")
        self.worker.start()

    def stop(self):
        self.stop_worker()
        self.ss.b_stop.configure(state="disabled")

    def send(self):
        t = self.text.get()
        if not t:
            return
        if self.worker and self.worker.send_text(t):
            self.log.add(f"→ DS : {t}")
            self.text.set("")
        else:
            self.log.add("Démarrez d'abord la liaison.")

    def on_message(self, kind, p):
        if kind == "log":
            self.log.add(p)
        elif kind == "started":
            self.ss.running(True, "en marche — en attente des DS")
        elif kind == "ended":
            self.worker = None
            for r in self.radios:
                r.configure(state="normal")
            self.ss.running(False, "arrêté")
        elif kind == "ds":
            node, keys, frame, where = p
            iid = str(node)
            new = iid not in self.seen
            self.seen[node] = time.time()
            vals = (f"DS{node}", keys_text(keys), frame, where, "active")
            if self.tree.exists(iid):
                self.tree.item(iid, values=vals)
            else:
                self.tree.insert("", "end", iid=iid, values=vals)
            if new:
                self.log.add(f"DS{node} connectée ({where}).")
                self.ss.status.configure(text=f"en marche — {len(self.seen)} DS")
        elif kind == "ping":
            self.log.add(f"ping de DS{p}, réponse envoyée (RTT affiché sur la DS)")
        elif kind == "stats":
            self.ss.status.configure(
                text=f"en marche — {len(self.seen)} DS | ESP32 : reçues {p['rx']}, "
                     f"perdues {p['drop']}, émises {p['tx']}, erreurs {p['txerr']}")

    def _tick(self):
        now = time.time()
        for node, t in self.seen.items():
            iid = str(node)
            if self.tree.exists(iid):
                age = now - t
                self.tree.set(iid, "vu", "active" if age < 1.5 else f"muette depuis {age:.0f} s")
        self.after(500, self._tick)


# --------------------------------------------------------------------------
class EspPontTab(Tab):
    def __init__(self, nb, app):
        super().__init__(nb, app, "ESP-Pont", GUIDE_ESPONT)
        b = self.body
        b.columnconfigure(0, weight=1)
        b.rowconfigure(1, weight=1)

        f = ttk.LabelFrame(b, text="Boîtier", padding=8)
        f.grid(row=0, column=0, sticky="ew")
        self.host = tk.StringVar(value="esp-pont.local")
        labeled(f, 0, "Adresse", ttk.Entry(f, width=26, textvariable=self.host))
        bar = ttk.Frame(f)
        bar.grid(row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Button(bar, text="Lire l'état", command=self.fetch).pack(side="left")
        ttk.Button(bar, text="Ouvrir l'interface web",
                   command=lambda: webbrowser.open(f"http://{self.host.get().strip()}/")).pack(
            side="left", padx=6)
        ttk.Button(bar, text="Configuration initiale (192.168.4.1)",
                   command=lambda: webbrowser.open("http://192.168.4.1/")).pack(side="left")
        ttk.Button(bar, text="Utiliser ce relais",
                   command=lambda: app.goto_link("relay", self.host.get().strip())).pack(
            side="left", padx=6)

        fo = ttk.LabelFrame(b, text="État et diagnostic", padding=4)
        fo.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.out = LogBox(fo, height=16)
        self.out.pack(fill="both", expand=True)
        self.out.set("Cliquez « Lire l'état ».")

    def fetch(self):
        host = self.host.get().strip()
        self.out.set(f"Lecture de http://{host}/api/status …")

        def job():
            try:
                with urllib.request.urlopen(f"http://{host}/api/status", timeout=4) as r:
                    self.post("status", json.loads(r.read().decode("utf-8")))
            except urllib.error.HTTPError as e:
                if e.code == 401:
                    self.post("error", "L'interface est protégée par un mot de passe : "
                                       "ouvrez l'interface web pour consulter l'état.")
                else:
                    self.post("error", f"Réponse HTTP {e.code}.")
            except (urllib.error.URLError, OSError, ValueError) as e:
                self.post("error", f"Boîtier injoignable ({e}).\n\nVérifiez qu'il est allumé, "
                                   "que sa liaison amont est établie, et essayez son adresse IP "
                                   "à la place de esp-pont.local. Pour une première configuration, "
                                   "connectez-vous au réseau ESP-Pont-Setup.")

        threading.Thread(target=job, daemon=True).start()

    def on_message(self, kind, p):
        if kind == "error":
            self.out.set(p)
        elif kind == "status":
            self.out.set(self._format(p))

    @staticmethod
    def _format(s: dict) -> str:
        link = {"connecte": "établie", "connexion": "connexion en cours",
                "secours": "mode secours"}.get(s.get("link"), "non configurée")
        if "relay" not in s:
            relay = "absent (firmware antérieur à la v1.1, reflashez esp-pont)"
        elif not s["relay"]:
            relay = "coupé"
        else:
            relay = (f"actif, PC inscrit {s['relay_pc']}" if s.get("relay_pc")
                     else "actif, aucun PC inscrit") + f", {s.get('relay_ds', 0)} DS relayée(s)"
        rows = [
            ("Liaison amont", link),
            ("Réseau amont", s.get("sta_ssid") or "—"),
            ("Adresse amont", s.get("sta_ip", "—")),
            ("Point d'accès", f"{s.get('ap_ssid')}  ({s.get('ap_sec')})"),
            ("Protocole", s.get("phy")),
            ("Canal", s.get("channel")),
            ("Clients", s.get("clients")),
            ("Relais dslink", relay),
            ("Durée de marche", s.get("uptime")),
        ]
        out = "\n".join(f"{k:<17}{v}" for k, v in rows)

        warn = []
        if s.get("link") != "connecte":
            warn.append("Liaison amont non établie : le PC et le relais ne peuvent pas se "
                        "joindre. Resélectionnez votre box dans l'interface web.")
        if not str(s.get("ap_sec", "")).startswith("Ouvert"):
            warn.append("Point d'accès chiffré : une DS ou DS Lite ne peut pas s'y connecter. "
                        "Passez la sécurité sur Ouvert.")
        if s.get("phy") != "802.11b":
            warn.append("Protocole du point d'accès autre que 802.11b : la DS ne verra pas le réseau.")
        if s.get("relay") is False:
            warn.append("Relais dslink coupé : cochez-le dans l'onglet Système de l'interface web.")
        out += "\n\nDiagnostic\n"
        out += "\n".join(f"  ! {w}" for w in warn) if warn else "  Tout est prêt pour la DS."
        return out


# --------------------------------------------------------------------------
class CaptureTab(Tab):
    def __init__(self, nb, app):
        super().__init__(nb, app, "Capture", GUIDE_CAPTURE)
        b = self.body
        b.columnconfigure(0, weight=1)
        b.rowconfigure(1, weight=1)

        f = ttk.LabelFrame(b, text="Réglages", padding=8)
        f.grid(row=0, column=0, sticky="ew")
        self.port = labeled(f, 0, "Port série", PortPicker(f))
        self.channel = tk.IntVar(value=7)
        labeled(f, 1, "Canal Wi-Fi", ttk.Spinbox(f, from_=1, to=13, width=5, textvariable=self.channel))
        self.nintendo = tk.BooleanVar(value=True)
        labeled(f, 2, "Filtre", ttk.Checkbutton(f, text="Seulement le multijoueur Nintendo",
                                                variable=self.nintendo))
        self.out = tk.StringVar(value=os.path.abspath("capture.pcap"))
        fp = ttk.Frame(f)
        ttk.Entry(fp, width=46, textvariable=self.out).pack(side="left")
        ttk.Button(fp, text="Parcourir…", command=self.browse).pack(side="left", padx=(6, 0))
        labeled(f, 3, "Fichier", fp)
        self.ss = StartStop(f, self.start, self.stop)
        self.ss.grid(row=4, column=0, columnspan=2, sticky="w", pady=(8, 0))

        fl = ttk.LabelFrame(b, text="Journal et analyse", padding=4)
        fl.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.log = LogBox(fl, height=14)
        self.log.pack(fill="both", expand=True)

    def browse(self):
        p = filedialog.asksaveasfilename(defaultextension=".pcap",
                                         filetypes=[("Capture pcap", "*.pcap")],
                                         initialfile=os.path.basename(self.out.get()))
        if p:
            self.out.set(p)

    def start(self):
        self.log.set("")
        self.worker = SniffWorker(self.post, self.port.get(), int_of(self.channel, 7),
                                  self.nintendo.get(), self.out.get())
        self.ss.running(True, "démarrage…")
        self.worker.start()

    def stop(self):
        self.stop_worker()
        self.ss.b_stop.configure(state="disabled")
        self.ss.status.configure(text="arrêt, analyse en cours…")

    def on_message(self, kind, p):
        if kind == "log":
            self.log.add(p)
        elif kind == "started":
            self.ss.running(True, "capture en cours")
        elif kind == "stats":
            n, rx, drop = p
            extra = f" | ESP32 : reçues {rx}, perdues {drop}" if rx is not None else ""
            self.ss.status.configure(text=f"{n} trames écrites{extra}")
        elif kind == "analysis":
            self.log.add("Analyse :\n" + p)
        elif kind == "ended":
            self.worker = None
            self.ss.running(False)


# --------------------------------------------------------------------------
class AnalyseTab(Tab):
    def __init__(self, nb, app):
        super().__init__(nb, app, "Analyse", GUIDE_ANALYSE)
        b = self.body
        b.columnconfigure(0, weight=1)
        b.rowconfigure(1, weight=1)
        f = ttk.LabelFrame(b, text="Capture", padding=8)
        f.grid(row=0, column=0, sticky="ew")
        self.path = tk.StringVar(value=os.path.abspath("capture.pcap"))
        ttk.Entry(f, width=50, textvariable=self.path).pack(side="left")
        ttk.Button(f, text="Parcourir…", command=self.browse).pack(side="left", padx=6)
        self.b_go = ttk.Button(f, text="Analyser", command=self.go)
        self.b_go.pack(side="left")
        fo = ttk.LabelFrame(b, text="Résultat", padding=4)
        fo.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.out = LogBox(fo, height=18)
        self.out.pack(fill="both", expand=True)

    def browse(self):
        p = filedialog.askopenfilename(filetypes=[("Capture pcap", "*.pcap"), ("Tous", "*.*")])
        if p:
            self.path.set(p)

    def go(self):
        path = self.path.get()
        self.out.set("Analyse en cours…")
        self.b_go.configure(state="disabled")
        threading.Thread(target=lambda: self.post("result", run_analyze(path)), daemon=True).start()

    def on_message(self, kind, p):
        if kind == "result":
            self.out.set(p)
            self.b_go.configure(state="normal")


# --------------------------------------------------------------------------
class ReplayTab(Tab):
    def __init__(self, nb, app):
        super().__init__(nb, app, "Rejeu de beacon", GUIDE_REPLAY)
        b = self.body
        b.columnconfigure(0, weight=1)
        b.rowconfigure(1, weight=1)
        f = ttk.LabelFrame(b, text="Réglages", padding=8)
        f.grid(row=0, column=0, sticky="ew")
        self.cap = tk.StringVar(value=os.path.abspath("capture.pcap"))
        fp = ttk.Frame(f)
        ttk.Entry(fp, width=46, textvariable=self.cap).pack(side="left")
        ttk.Button(fp, text="Parcourir…", command=self.browse).pack(side="left", padx=(6, 0))
        labeled(f, 0, "Capture", fp)
        self.port = labeled(f, 1, "Port série", PortPicker(f))
        self.channel = tk.IntVar(value=7)
        labeled(f, 2, "Canal Wi-Fi", ttk.Spinbox(f, from_=1, to=13, width=5, textvariable=self.channel))
        self.index = tk.IntVar(value=0)
        labeled(f, 3, "Index du beacon", ttk.Spinbox(f, from_=0, to=99, width=5, textvariable=self.index))
        self.interval = tk.IntVar(value=100)
        labeled(f, 4, "Intervalle (ms)",
                ttk.Spinbox(f, from_=20, to=1000, increment=10, width=6, textvariable=self.interval))
        self.ss = StartStop(f, self.start, self.stop)
        self.ss.grid(row=5, column=0, columnspan=2, sticky="w", pady=(8, 0))
        fl = ttk.LabelFrame(b, text="Réactions des DS", padding=4)
        fl.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.log = LogBox(fl, height=14)
        self.log.pack(fill="both", expand=True)

    def browse(self):
        p = filedialog.askopenfilename(filetypes=[("Capture pcap", "*.pcap"), ("Tous", "*.*")])
        if p:
            self.cap.set(p)

    def start(self):
        self.log.set("")
        self.worker = ReplayWorker(self.post, self.port.get(), int_of(self.channel, 7),
                                   self.cap.get(), int_of(self.index, 0), int_of(self.interval, 100))
        self.ss.running(True, "démarrage…")
        self.worker.start()

    def stop(self):
        self.stop_worker()
        self.ss.b_stop.configure(state="disabled")

    def on_message(self, kind, p):
        if kind == "log":
            self.log.add(p)
        elif kind == "started":
            self.ss.running(True, "rejeu en cours")
        elif kind == "ended":
            self.worker = None
            self.ss.running(False, "arrêté")


# ==========================================================================
# Application
# ==========================================================================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ds-pc-link")
        self.geometry("1150x740")
        self.minsize(950, 620)
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        elif "clam" in style.theme_names():
            style.theme_use("clam")
        self.option_add("*Font", BASE_FONT)

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=8, pady=8)
        HomeTab(self.nb, self)
        self.link = LinkTab(self.nb, self)
        self.espont = EspPontTab(self.nb, self)
        self.tabs = [self.link, self.espont, CaptureTab(self.nb, self),
                     AnalyseTab(self.nb, self), ReplayTab(self.nb, self)]
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    def goto_link(self, mode: str, relay: str | None = None):
        self.link.set_mode(mode, relay)
        self.nb.select(self.link)

    def on_close(self):
        for t in self.tabs:
            t.stop_worker()
        self.after(400, self.destroy)        # laisse les sessions fermer leur port


def main():
    if WINDOWS:
        try:  # texte net sur les écrans à haute résolution
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:  # noqa: BLE001
            pass
    App().mainloop()


if __name__ == "__main__":
    main()
