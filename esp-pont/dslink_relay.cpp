#include "dslink_relay.h"
#include "config.h"

#if defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <WiFiUdp.h>

/* Miroir de common/linkproto.h : l'IDE Arduino ne compile que les fichiers
 * du dossier du croquis, il ne peut pas inclure ../common. A garder
 * synchronise avec LinkHeader (10 octets, little-endian) :
 *   [magic u32][version u8][node u8][seq u16][len u16][donnees]          */
#define LINK_MAGIC     0x4B4E4C44UL   /* "DLNK" */
#define LINK_VERSION   1
#define LINK_MAX_DATA  400
#define LINK_HDR_LEN   10

#define MAX_NODES      16             /* 0 = PC, 1..15 = DS */
#define PC_TIMEOUT_MS  10000UL        /* le PC renvoie un message vide chaque seconde */
#define DS_TIMEOUT_MS  30000UL
#define MAX_PER_LOOP   8              /* datagrammes traites par tour de loop() */

struct Peer {
  IPAddress ip;
  uint16_t  port;
  uint32_t  seen;
  uint16_t  lastSeq;
  bool      on;
};

static WiFiUDP  udp;
static bool     g_started = false;
static Peer     g_pc;
static Peer     g_ds[MAX_NODES];
static uint8_t  g_buf[LINK_HDR_LEN + LINK_MAX_DATA + 4];

static bool parseHeader(const uint8_t *p, int n, uint8_t *node, uint16_t *seq) {
  if (n < LINK_HDR_LEN) return false;
  uint32_t magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  uint16_t len   = (uint16_t)(p[8] | (p[9] << 8));
  if (magic != LINK_MAGIC || p[4] != LINK_VERSION) return false;
  if (len > LINK_MAX_DATA || LINK_HDR_LEN + len > n) return false;
  *node = p[5];
  *seq  = (uint16_t)(p[6] | (p[7] << 8));
  return true;
}

static void sendTo(const Peer &d, const uint8_t *p, int n) {
  udp.beginPacket(d.ip, d.port);
  udp.write(p, n);
  udp.endPacket();
}

static void forgetAll() {
  g_pc.on = false;
  for (int i = 0; i < MAX_NODES; i++) g_ds[i].on = false;
}

void relayLoop() {
  bool want = (cfg.relay_off == 0);
  if (want && !g_started) {
    g_started = udp.begin(DSLINK_UDP_PORT);
    if (!g_started) return;
  } else if (!want && g_started) {
    udp.stop();
    g_started = false;
    forgetAll();
  }
  if (!g_started) return;

  uint32_t now = millis();
  if (g_pc.on && now - g_pc.seen > PC_TIMEOUT_MS) g_pc.on = false;
  for (int i = 1; i < MAX_NODES; i++)
    if (g_ds[i].on && now - g_ds[i].seen > DS_TIMEOUT_MS) g_ds[i].on = false;

  for (int k = 0; k < MAX_PER_LOOP; k++) {
    int size = udp.parsePacket();
    if (size <= 0) break;
    int n = udp.read(g_buf, sizeof g_buf);
    if (n <= 0 || n != size) continue;          /* trop grand pour dslink */

    uint8_t node; uint16_t seq;
    if (!parseHeader(g_buf, n, &node, &seq) || node >= MAX_NODES) continue;

    IPAddress from = udp.remoteIP();
    uint16_t  port = udp.remotePort();
    bool fromPc = g_pc.on && from == g_pc.ip && port == g_pc.port;

    if (node == 0 || fromPc) {
      /* Le PC. Un message du noeud 0 l'inscrit (ou le reinscrit), y compris
       * les messages vides de maintien envoyes chaque seconde. Il peut aussi
       * faire suivre, sous leur propre numero, les messages d'autres DS
       * (hub : vraies DS du pont radio, DS emulees dans melonDS) : on les
       * reconnait a l'adresse du PC. Tout est transmis aux DS du point
       * d'acces, sauf a celle dont le message est l'echo. */
      if (node == 0) {
        g_pc.ip = from; g_pc.port = port; g_pc.seen = now; g_pc.on = true;
      }
      for (int i = 1; i < MAX_NODES; i++)
        if (g_ds[i].on && i != node) sendTo(g_ds[i], g_buf, n);
    } else {
      /* Une DS. Sans adresse de PC, elle envoie chaque message a la fois a
       * sa passerelle et en broadcast : l'ESP peut recevoir deux copies. */
      Peer &d = g_ds[node];
      if (d.on && d.lastSeq == seq && d.ip == from) continue;
      d.ip = from; d.port = port; d.seen = now; d.lastSeq = seq; d.on = true;
      if (g_pc.on) sendTo(g_pc, g_buf, n);
    }
  }
}

bool      relayActive()  { return g_started; }
bool      relayPcKnown() { return g_started && g_pc.on; }
IPAddress relayPcIp()    { return g_pc.ip; }
uint16_t  relayPcPort()  { return g_pc.port; }

uint8_t relayDsCount() {
  uint8_t c = 0;
  for (int i = 1; i < MAX_NODES; i++) if (g_ds[i].on) c++;
  return c;
}
