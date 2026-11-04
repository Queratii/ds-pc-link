// Protocole partagé entre la DS (homebrew), l'ESP32 et le PC.
//
// Deux couches :
//   1) "Air"    : trames 802.11 brutes échangées entre la DS et l'ESP32.
//   2) "Série"  : messages entre l'ESP32 et le PC, encodés en COBS + CRC16,
//                 séparés par un octet 0x00.
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// 1) Couche AIR (homebrew uniquement)
// ---------------------------------------------------------------------------
// Trame de données 802.11 non-QoS, envoyée en BROADCAST : un broadcast n'attend
// pas d'ACK, ce qui évite toute contrainte de timing à la microseconde.
//
//  off  taille  champ
//   0     2     Frame Control = 0x0008 (type Data, sous-type 0, pas de DS)
//   2     2     Duration      = 0
//   4     6     Addr1 (dest)  = FF:FF:FF:FF:FF:FF
//  10     6     Addr2 (src)   = MAC de l'émetteur
//  16     6     Addr3 (BSSID) = LINK_BSSID (notre "réseau" privé)
//  22     2     Sequence ctrl
//  24     ...   LinkHeader + données de jeu
#define LINK_80211_HDR_LEN 24
#define LINK_DEFAULT_CHANNEL 7

// Adresse localement administrée (bit 0x02) : ne peut pas entrer en conflit
// avec une vraie carte réseau ni avec les adresses Nintendo (00:09:BF / 03:09:BF).
static const uint8_t LINK_BSSID[6] = {0x02, 'D', 'S', 'L', 'N', 'K'};

#define LINK_MAGIC    0x4B4E4C44u   // "DLNK" en little-endian
#define LINK_VERSION  1
#define LINK_MAX_DATA 400           // reste sous le MTU de la DS avec de la marge

typedef struct __attribute__((packed)) {
    uint32_t magic;    // LINK_MAGIC
    uint8_t  version;  // LINK_VERSION
    uint8_t  node;     // identifiant du joueur/nœud (0 = PC, 1..15 = DS)
    uint16_t seq;      // numéro de séquence de l'émetteur (détection de pertes)
    uint16_t len;      // nombre d'octets de données qui suivent
} LinkHeader;          // 10 octets

// Construit une trame air complète dans out. Retourne sa taille (sans FCS).
static inline size_t link_build_air_frame(uint8_t *out, const uint8_t src_mac[6],
                                          uint8_t node, uint16_t seq,
                                          const void *data, uint16_t len)
{
    if (len > LINK_MAX_DATA) len = LINK_MAX_DATA;
    uint8_t *p = out;
    p[0] = 0x08; p[1] = 0x00;            // Frame Control
    p[2] = 0x00; p[3] = 0x00;            // Duration
    for (int i = 0; i < 6; i++) p[4 + i]  = 0xFF;
    for (int i = 0; i < 6; i++) p[10 + i] = src_mac[i];
    for (int i = 0; i < 6; i++) p[16 + i] = LINK_BSSID[i];
    p[22] = (uint8_t)(seq << 4); p[23] = (uint8_t)(seq >> 4);
    LinkHeader h = { LINK_MAGIC, LINK_VERSION, node, seq, len };
    const uint8_t *hb = (const uint8_t *)&h;
    for (size_t i = 0; i < sizeof h; i++) p[LINK_80211_HDR_LEN + i] = hb[i];
    const uint8_t *d = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) p[LINK_80211_HDR_LEN + sizeof h + i] = d[i];
    return LINK_80211_HDR_LEN + sizeof h + len;
}

// Vérifie un message [LinkHeader][données], sans en-tête 802.11 : c'est le
// contenu d'un datagramme UDP en mode réseau. Retourne 1 si valide, 0 sinon.
static inline int link_parse_payload(const uint8_t *p, size_t n,
                                     LinkHeader *hdr, const uint8_t **data)
{
    if (n < sizeof(LinkHeader)) return 0;
    uint8_t *dst = (uint8_t *)hdr;
    for (size_t i = 0; i < sizeof *hdr; i++) dst[i] = p[i];
    if (hdr->magic != LINK_MAGIC || hdr->version != LINK_VERSION) return 0;
    // Les récepteurs copient les données dans des tampons de LINK_MAX_DATA
    // octets : un message qui annonce plus est rejeté (sinon débordement).
    if (hdr->len > LINK_MAX_DATA) return 0;
    if (sizeof(LinkHeader) + hdr->len > n) return 0;
    *data = p + sizeof(LinkHeader);
    return 1;
}

// Vérifie qu'une trame reçue est bien à nous. Remplit *hdr et *data.
// Retourne 1 si valide, 0 sinon. frame_len = longueur SANS le FCS.
static inline int link_parse_air_frame(const uint8_t *f, size_t frame_len,
                                       LinkHeader *hdr, const uint8_t **data)
{
    if (frame_len < LINK_80211_HDR_LEN + sizeof(LinkHeader)) return 0;
    if ((f[0] & 0x0C) != 0x08) return 0;                 // pas une trame Data
    for (int i = 0; i < 6; i++) if (f[16 + i] != LINK_BSSID[i]) return 0;
    return link_parse_payload(f + LINK_80211_HDR_LEN, frame_len - LINK_80211_HDR_LEN,
                              hdr, data);
}

// ---------------------------------------------------------------------------
// 1 bis) Mode RÉSEAU (UDP) — melonDS, ESP-Pont, tout point d'accès ouvert
// ---------------------------------------------------------------------------
// La DS se connecte à un point d'accès (melonAP sous melonDS) et envoie au PC
// des datagrammes UDP contenant [LinkHeader][données], sans en-tête 802.11.
// Le PC écoute sur ce port et répond à l'adresse source des datagrammes reçus,
// ce qui traverse les NAT (celui de melonDS comme celui d'ESP-Pont).
#define LINK_UDP_PORT 47474

// ---------------------------------------------------------------------------
// 2) Couche SÉRIE (ESP32 <-> PC)
// ---------------------------------------------------------------------------
// Trame décodée : [type:1][payload:n][crc16:2 little-endian]
// Le CRC (CCITT-FALSE) couvre type + payload. Sur le fil : COBS(...) puis 0x00.

// PC -> ESP32
#define SER_CMD_PING          0x01  // payload : rien            -> SER_EVT_PONG
#define SER_CMD_SET_CHANNEL   0x02  // payload : [canal 1..13]
#define SER_CMD_SET_MODE      0x03  // payload : [MODE_*]
#define SER_CMD_SEND_LINK     0x04  // payload : [node][données] (homebrew)
#define SER_CMD_TX_RAW        0x05  // payload : [rate][trame 802.11 sans FCS]
#define SER_CMD_BEACON_START  0x06  // payload : [intervalle_ms u16 LE][trame]
#define SER_CMD_BEACON_STOP   0x07  // payload : rien
#define SER_CMD_MP_TX         0x08  // payload : [flags MP_TX_*][n° de CMD][trame DS] (mode MP, voir plus bas)
#define SER_CMD_MP_CONFIG     0x09  // payload : [fenêtre de réponses en µs, u16 LE]

// ESP32 -> PC
#define SER_EVT_PONG          0x80  // payload : [version firmware][mac x6]
#define SER_EVT_RX_LINK       0x81  // payload : [rssi i8][LinkHeader][données]
#define SER_EVT_RX_RAW        0x82  // payload : [rssi i8][canal][ts_us u32 LE][trame]
#define SER_EVT_LOG           0x83  // payload : texte ASCII
#define SER_EVT_STATS         0x84  // payload : [rx u32][drop u32][tx u32][txerr u32]
#define SER_EVT_MP_REPLY      0x85  // payload : [n° de CMD][rssi i8][rate][ts_us u32 LE][trame sans FCS]
#define SER_EVT_MP_WINDOW     0x86  // payload : [n° de CMD][nb de réponses u8][réponses tardives u32 LE]

#define MODE_LINK   0   // homebrew : ne remonte que nos trames LINK_BSSID
#define MODE_SNIFF  1   // capture : remonte toutes les trames du canal
#define MODE_NINTENDO 2 // capture filtrée : seulement les adresses Nintendo
#define MODE_MP_HOST  3 // multijoueur natif : l'ESP32 joue la radio d'une DS hôte émulée

// ---------------------------------------------------------------------------
// 3) Mode MP_HOST — multijoueur natif Nintendo, émulateur hôte (melonDS)
// ---------------------------------------------------------------------------
// melonDS émule la DS hôte ; l'ESP32 lui prête sa radio, de vraies DS sont
// les clientes. Une "trame DS" est le tampon matériel de la DS émulée :
//   [0..7]  réservé   [8] débit (RATE_1M / RATE_2M)   [9] canal
//   [10..11] longueur u16 LE, FCS de 4 octets COMPRIS
//   [12..]  trame 802.11, puis 4 octets de FCS (non significatifs)
// L'ESP32 :
//  - prend comme adresse MAC l'émetteur des trames de l'hôte (addr2), pour
//    que sa radio acquitte d'elle-même les trames que les clientes lui
//    adressent (authentification, association) ;
//  - suit le canal indiqué à l'octet 9 ;
//  - après une trame marquée MP_TX_CMD, ouvre une fenêtre de réponses :
//    chaque réponse de cliente (vers 03:09:BF:00:00:10) part aussitôt en
//    SER_EVT_MP_REPLY, puis SER_EVT_MP_WINDOW clôt la fenêtre. Les deux
//    portent le n° de CMD donné par le PC : une fin de fenêtre en retard ne
//    peut pas être prise pour celle de la CMD suivante. Une nouvelle CMD clôt
//    d'abord la fenêtre précédente si elle est encore ouverte ;
//  - remonte les autres trames du réseau de l'hôte en SER_EVT_RX_RAW.
#define MP_TX_CMD             0x01  // la trame est une CMD : ouvrir la fenêtre de réponses
#define MP_DEFAULT_WINDOW_US  3000  // fenêtre par défaut (les clientes répondent en < 1 ms)
#define DS_HW_HDR_LEN         12

#define RATE_1M 0x0A
#define RATE_2M 0x14

#define SER_MAX_FRAME 1700

static inline uint16_t crc16_ccitt(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// Adresses multicast du protocole multijoueur Nintendo (d'après GBATEK) :
//   03:09:BF:00:00:00  hôte -> clients (trame MP "CMD")
//   03:09:BF:00:00:10  clients -> hôte (réponses)
//   03:09:BF:00:00:03  hôte -> clients (ACK MP)
// Attention : les vraies DS ont des MAC de plusieurs OUI Nintendo, on ne peut
// donc pas filtrer sur l'adresse source. Le firmware ESP32 combine ce test avec
// la détection des beacons portant l'élément propriétaire OUI 00:09:BF.
static inline int is_nintendo_mp_multicast(const uint8_t a[6])
{
    return a[0] == 0x03 && a[1] == 0x09 && a[2] == 0xBF;
}

// Adresse de destination des réponses des clientes (03:09:BF:00:00:10)
static inline int is_nintendo_mp_reply(const uint8_t a[6])
{
    return is_nintendo_mp_multicast(a) && a[3] == 0x00 && a[4] == 0x00 && a[5] == 0x10;
}

// Cherche un Information Element "vendor specific" (0xDD) avec l'OUI 00:09:BF
// dans le corps d'un beacon. body = début des IE, n = taille.
// Retourne un pointeur sur l'IE (octet 0xDD) ou NULL.
static inline const uint8_t *find_nintendo_ie(const uint8_t *body, size_t n)
{
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t id = body[i], len = body[i + 1];
        if (i + 2 + len > n) break;
        if (id == 0xDD && len >= 3 && body[i + 2] == 0x00 && body[i + 3] == 0x09
            && body[i + 4] == 0xBF)
            return body + i;
        i += 2 + len;
    }
    return NULL;
}
