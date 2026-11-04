// dslink : implémentation avec DSWiFi (BlocksDS).
//
// API utilisée (doc BlocksDS DSWiFi) :
//   Radio  : Wifi_InitDefault(INIT_ONLY), Wifi_EnableWifi(), Wifi_SetPromiscuousMode(1),
//            Wifi_SetChannel(n), Wifi_RawSetPacketHandler(h), Wifi_RxRawReadPacket(),
//            Wifi_RawTxFrame(len, rate, src) — rate : 0x000A = 1 Mbit/s, 0x0014 = 2 Mbit/s.
//            Le tampon lu dans le handler commence par l'en-tête 802.11.
//   Réseau : Wifi_InitDefault(WFC_CONNECT), Wifi_GetIPInfo(), sockets BSD (UDP).

#include <nds.h>
#include <dswifi9.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "dslink.h"

#define RX_SLOTS 8   // file circulaire de messages reçus

enum { TR_NONE, TR_RADIO, TR_UDP };
static int s_transport = TR_NONE;

static uint8_t  s_node;
static uint8_t  s_mac[6];
static uint16_t s_tx_seq;

static DsLinkMsg s_rx[RX_SLOTS];
static volatile int s_rx_head, s_rx_tail;   // head : écrit par le récepteur

static uint16_t s_last_seq_from[16];
static bool     s_seen_from[16];

static DsLinkStats s_stats;

// Tampons alignés sur 16 bits (exigé par DSWiFi)
static uint16_t s_rx_buf[(1600 + 1) / 2];
static uint16_t s_tx_buf[(LINK_80211_HDR_LEN + sizeof(LinkHeader) + LINK_MAX_DATA + 1) / 2];

// --- état du mode réseau ---
static int                s_sock = -1;
static uint32_t           s_local_ip;
static struct sockaddr_in s_peer;            // PC, appris à la première réponse
static bool               s_peer_known;
static struct sockaddr_in s_cand[3];         // destinations tant que le PC est inconnu
static int                s_ncand;

// Met un message valide dans la file (commun aux deux transports).
static void rx_accept(const LinkHeader *h, const uint8_t *data)
{
    uint8_t n = h->node & 15;
    if (s_seen_from[n]) {
        uint16_t expected = s_last_seq_from[n] + 1;
        uint16_t gap = (uint16_t)(h->seq - expected);
        if (h->seq == s_last_seq_from[n]) return;         // doublon
        // Un saut vers l'arrière (message en retard, émetteur redémarré) n'est
        // pas une perte : on ne compte que les sauts vers l'avant.
        if (gap != 0 && gap < 0x8000) s_stats.rx_lost += gap;
    }
    s_seen_from[n] = true;
    s_last_seq_from[n] = h->seq;

    int next = (s_rx_head + 1) % RX_SLOTS;
    if (next == s_rx_tail) { s_stats.rx_overflow++; return; }

    DsLinkMsg *m = &s_rx[s_rx_head];
    m->node = h->node;
    m->seq  = h->seq;
    m->len  = h->len;
    memcpy(m->data, data, h->len);
    s_rx_head = next;
    s_stats.rx_ok++;
}

// ===========================================================================
// Transport radio directe
// ===========================================================================

// Appelé par DSWiFi pour chaque trame reçue en mode promiscuous.
// Peut s'exécuter en contexte d'interruption : rester bref.
static void raw_handler(int packet_id, int length)
{
    if (length <= 0 || length > (int)sizeof s_rx_buf) return;
    Wifi_RxRawReadPacket((u32)packet_id, (u32)length, s_rx_buf);

    const uint8_t *f = (const uint8_t *)s_rx_buf;
    LinkHeader h;
    const uint8_t *data;

    // Selon le chemin de réception, la longueur peut inclure le FCS :
    // le parseur se base sur h.len, donc 4 octets de trop ne gênent pas.
    if (!link_parse_air_frame(f, (size_t)length, &h, &data)) return;
    if (memcmp(f + 10, s_mac, 6) == 0) return;          // notre propre trame
    rx_accept(&h, data);
}

bool dslink_init(int channel, uint8_t node)
{
    s_node = node;
    if (!Wifi_InitDefault(INIT_ONLY)) return false;

    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, s_mac);
    Wifi_RawSetPacketHandler(raw_handler);
    Wifi_EnableWifi();
    Wifi_SetPromiscuousMode(1);
    Wifi_SetChannel(channel);
    s_transport = TR_RADIO;
    return true;
}

// ===========================================================================
// Transport réseau (UDP) — melonDS, ESP-Pont, point d'accès ouvert
// ===========================================================================

static void add_cand(uint32_t ip_be, uint16_t port)
{
    if (ip_be == 0 || s_ncand >= 3) return;
    for (int i = 0; i < s_ncand; i++)
        if (s_cand[i].sin_addr.s_addr == ip_be) return;
    struct sockaddr_in *a = &s_cand[s_ncand++];
    memset(a, 0, sizeof *a);
    a->sin_family      = AF_INET;
    a->sin_port        = htons(port);
    a->sin_addr.s_addr = ip_be;
}

bool dslink_init_udp(const char *pc_ip, uint16_t port, uint8_t node)
{
    s_node = node;

    // Connexion au point d'accès des réglages Wi-Fi de la console (WFC).
    // Sous melonDS avec le firmware intégré, melonAP y est déjà enregistré.
    if (!Wifi_InitDefault(WFC_CONNECT)) return false;
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, s_mac);

    struct in_addr gw, mask, dns1, dns2;
    struct in_addr me = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    s_local_ip = me.s_addr;

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) return false;

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;
    bind(s_sock, (struct sockaddr *)&local, sizeof local);

    int one = 1;
    ioctl(s_sock, FIONBIO, &one);                                  // non bloquant
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); // si supporté

    // Destinations essayées tant que le PC n'a pas répondu :
    //  - l'adresse donnée, si elle l'est ;
    //  - sinon la passerelle (sous melonDS en mode indirect, c'est le PC hôte)
    //    et le broadcast du sous-réseau (melonDS en mode direct, PC sur le
    //    même réseau local).
    s_ncand = 0;
    s_peer_known = false;
    if (pc_ip && pc_ip[0]) {
        add_cand(inet_addr(pc_ip), port);
    } else {
        add_cand(gw.s_addr, port);
        add_cand(me.s_addr | ~mask.s_addr, port);
    }
    if (s_ncand == 0) return false;

    s_transport = TR_UDP;
    return true;
}

// Vide la file de réception du socket (appelé par dslink_poll).
static void udp_pump(void)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s_sock, s_rx_buf, sizeof s_rx_buf, 0,
                         (struct sockaddr *)&from, &fl);
        if (n <= 0) return;

        LinkHeader h;
        const uint8_t *data;
        if (!link_parse_payload((const uint8_t *)s_rx_buf, (size_t)n, &h, &data)) continue;
        if (h.node == s_node) continue;                // notre propre broadcast

        // On se verrouille sur l'adresse du PC (nœud 0) qui répond : elle
        // traverse les NAT. Les broadcasts d'autres DS ne la remplacent pas.
        if (h.node == 0) {
            s_peer = from;
            s_peer_known = true;
        }
        rx_accept(&h, data);
    }
}

static bool udp_send(const uint8_t *pkt, size_t n)
{
    if (s_peer_known)
        return sendto(s_sock, pkt, n, 0, (struct sockaddr *)&s_peer, sizeof s_peer) == (int)n;

    bool ok = false;
    for (int i = 0; i < s_ncand; i++)
        if (sendto(s_sock, pkt, n, 0, (struct sockaddr *)&s_cand[i], sizeof s_cand[i]) == (int)n)
            ok = true;
    return ok;
}

// ===========================================================================
// API commune
// ===========================================================================

bool dslink_send(const void *data, uint16_t len)
{
    size_t n = link_build_air_frame((uint8_t *)s_tx_buf, s_mac, s_node, s_tx_seq++,
                                    data, len);
    bool ok = false;
    if (s_transport == TR_RADIO) {
        ok = Wifi_RawTxFrame((u16)n, 0x0014, s_tx_buf) == 0;     // 2 Mbit/s
    } else if (s_transport == TR_UDP) {
        // Même message, sans l'en-tête 802.11
        ok = udp_send((const uint8_t *)s_tx_buf + LINK_80211_HDR_LEN,
                      n - LINK_80211_HDR_LEN);
    }
    if (ok) s_stats.tx_ok++; else s_stats.tx_fail++;
    return ok;
}

bool dslink_poll(DsLinkMsg *out)
{
    if (s_transport == TR_UDP) udp_pump();
    if (s_rx_tail == s_rx_head) return false;
    int old = enterCriticalSection();
    memcpy(out, &s_rx[s_rx_tail], sizeof *out);
    s_rx_tail = (s_rx_tail + 1) % RX_SLOTS;
    leaveCriticalSection(old);
    return true;
}

bool dslink_connected(void) { return s_transport == TR_RADIO || s_peer_known; }
void dslink_get_stats(DsLinkStats *out) { *out = s_stats; }
const uint8_t *dslink_mac(void) { return s_mac; }
uint32_t dslink_local_ip(void) { return s_transport == TR_UDP ? s_local_ip : 0; }
