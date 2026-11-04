// dslink : liaison sans fil DS <-> PC.
// À copier dans vos homebrews (avec ../common/linkproto.h).
//
// Deux transports, même API d'envoi et de réception :
//   - radio directe : trames 802.11 brutes vers le pont ESP32 (vraie DS seulement) ;
//   - réseau (UDP)  : via un point d'accès, compatible avec l'émulateur melonDS.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "linkproto.h"

typedef struct {
    uint8_t  node;              // émetteur (0 = PC)
    uint16_t seq;
    uint16_t len;
    uint8_t  data[LINK_MAX_DATA];
} DsLinkMsg;

typedef struct {
    uint32_t rx_ok, rx_lost, rx_overflow, tx_ok, tx_fail;
} DsLinkStats;

// Transport radio directe : écoute libre sur le canal donné, sans point d'accès.
// node = identifiant de cette console (1..15).
// Ne fonctionne pas sous émulateur : melonDS ne relaie pas ces trames au PC.
bool dslink_init(int channel, uint8_t node);

// Transport réseau : connexion au point d'accès enregistré dans les réglages
// Wi-Fi de la console (WFC), puis UDP vers le PC. Sous melonDS, c'est le point
// d'accès émulé « melonAP ». Sur une vraie DS : ESP-Pont ou un réseau ouvert.
//   pc_ip : adresse du PC, ex. "192.168.1.20". NULL ou "" : recherche
//           automatique (passerelle + broadcast du sous-réseau), qui suffit
//           sous melonDS ; il faut une adresse explicite derrière ESP-Pont.
//   port  : LINK_UDP_PORT, sauf si vous l'avez changé côté PC.
// Bloque pendant la connexion (quelques secondes). Retourne false en cas
// d'échec (aucun point d'accès configuré ou joignable).
bool dslink_init_udp(const char *pc_ip, uint16_t port, uint8_t node);

// Mode réseau : vrai dès que le PC a répondu une fois. Toujours vrai en radio.
bool dslink_connected(void);

// Envoie un message (len <= LINK_MAX_DATA). Retourne false en cas d'échec.
bool dslink_send(const void *data, uint16_t len);

// Récupère le prochain message reçu. Retourne false s'il n'y en a pas.
bool dslink_poll(DsLinkMsg *out);

void dslink_get_stats(DsLinkStats *out);
const uint8_t *dslink_mac(void);

// Mode réseau : adresse IPv4 obtenue (ordre réseau), 0 en mode radio.
uint32_t dslink_local_ip(void);
