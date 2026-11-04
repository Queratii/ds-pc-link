// Démo dslink : la DS envoie son état (boutons) et mesure la latence vers le PC.
//
// Messages applicatifs (1er octet = type) :
//   'S' état   : [keys u32][frame u32]                 DS -> PC, et vers les autres DS
//   'P' ping   : [ticks u32]                           DS -> PC (le PC renvoie 'p')
//   'p' pong   : [ticks u32][nœud visé u8]             PC -> DS
//   'T' texte  : [chaîne]                              PC -> DS
//
// Le hub du PC (dslink.py demo avec plusieurs transports) fait suivre l'état
// de chaque DS aux autres : vraies DS et DS émulées dans melonDS voient leurs
// touches respectives, comme dans un jeu multijoueur.
//
// Au démarrage, choix du transport :
//   A = radio directe (vraie DS + pont ESP32)
//   B = réseau Wi-Fi + UDP (émulateur melonDS, ou vraie DS + relais ESP-Pont)
// Ensuite : A = ping, START = quitter.

#include <nds.h>
#include <dswifi9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dslink.h"

#define CHANNEL 7
#define NODE    1      // changez-le pour chaque console (1..15)

// Adresse IP du PC pour le mode réseau. Vide : recherche automatique (passerelle
// + broadcast), qui suffit sous melonDS et derrière ESP-Pont avec son relais
// dslink actif. Ne la renseignez que pour joindre le PC sans relais, ex. "192.168.1.20".
#define PC_IP   ""

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void halt(const char *msg)
{
    printf("%s\nSTART = quitter\n", msg);
    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) exit(0);
    }
}

int main(void)
{
    consoleDemoInit();
    printf("dslink demo - noeud %d\n\n", NODE);
    printf("A : radio directe\n    (vraie DS + pont ESP32)\n");
    printf("B : reseau Wi-Fi + UDP\n    (melonDS, ou DS + ESP-Pont)\n\n");

    bool udp;
    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        uint32_t k = keysDown();
        if (k & KEY_A) { udp = false; break; }
        if (k & KEY_B) { udp = true;  break; }
        if (k & KEY_START) return 0;
    }

    if (udp) {
        printf("Connexion au point d'acces\n(reglages Wi-Fi de la console)...\n");
        if (!dslink_init_udp(PC_IP, LINK_UDP_PORT, NODE))
            halt("Echec : aucun point d'acces\njoignable. Sous melonDS,\nactivez le Wi-Fi (melonAP).");
        uint32_t ip = dslink_local_ip();
        printf("IP %lu.%lu.%lu.%lu  port %d\n",
               (unsigned long)(ip & 0xFF), (unsigned long)((ip >> 8) & 0xFF),
               (unsigned long)((ip >> 16) & 0xFF), (unsigned long)(ip >> 24), LINK_UDP_PORT);
    } else {
        printf("Init Wi-Fi, canal %d...\n", CHANNEL);
        if (!dslink_init(CHANNEL, NODE)) halt("Echec init Wi-Fi");
        const uint8_t *mac = dslink_mac();
        printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    printf("A = ping, START = quitter\n");

    cpuStartTiming(0);              // timers 0+1 en cascade, ~33,5 MHz
    uint32_t frame = 0;
    char last_text[33] = "";
    uint32_t last_rtt_us = 0;

    // État des autres consoles, reçu via le hub du PC
    uint32_t peer_keys[16] = { 0 };
    uint32_t peer_seen[16] = { 0 };   // image de la dernière réception (0 = jamais)

    for (;;) {
        swiWaitForVBlank();
        // Pas de Wifi_Update() : les versions récentes de DSWiFi (BlocksDS)
        // l'ont retirée côté ARM9, la bibliothèque se met à jour seule.
        scanKeys();
        uint32_t held = keysHeld(), down = keysDown();
        if (down & KEY_START) break;
        frame++;

        // État toutes les 2 images (30 Hz) : suffisant pour un jeu au tour/action légère.
        // En mode réseau, c'est aussi ce qui permet au PC de découvrir la DS.
        if ((frame & 1) == 0) {
            uint8_t m[9] = { 'S' };
            put_u32(m + 1, held);
            put_u32(m + 5, frame);
            dslink_send(m, sizeof m);
        }

        if (down & KEY_A) {
            uint8_t m[5] = { 'P' };
            put_u32(m + 1, cpuGetTiming());
            dslink_send(m, sizeof m);
        }

        DsLinkMsg msg;
        while (dslink_poll(&msg)) {
            if (msg.len == 0) continue;
            switch (msg.data[0]) {
            case 'p':
                // Le pong vise une DS : ignorer celui d'une autre console
                if (msg.len >= 5 && (msg.len < 6 || msg.data[5] == NODE)) {
                    uint32_t dt = cpuGetTiming() - get_u32(msg.data + 1);
                    last_rtt_us = (uint32_t)((uint64_t)dt * 1000 / 33514);
                }
                break;
            case 'S':
                if (msg.len >= 9 && msg.node >= 1 && msg.node <= 15) {
                    peer_keys[msg.node] = get_u32(msg.data + 1);
                    peer_seen[msg.node] = frame;
                }
                break;
            case 'T': {
                int n = msg.len - 1;
                if (n > 32) n = 32;
                memcpy(last_text, msg.data + 1, n);
                last_text[n] = 0;
                break;
            }
            }
        }

        if ((frame % 30) == 0) {
            DsLinkStats st;
            dslink_get_stats(&st);
            printf("\x1b[12;0H");
            printf("PC   %-20s\n", dslink_connected() ? "connecte" : "en attente...");
            printf("TX ok %-6lu  echec %-4lu\n", (unsigned long)st.tx_ok, (unsigned long)st.tx_fail);
            printf("RX ok %-6lu  perdus %-4lu\n", (unsigned long)st.rx_ok, (unsigned long)st.rx_lost);
            printf("RTT  %-6lu us          \n", (unsigned long)last_rtt_us);
            printf("PC: %-28s\n", last_text);

            // Autres consoles (au plus 5 lignes) : leurs touches tenues
            printf("-- autres DS --------------------\n");
            int shown = 0;
            for (int n = 1; n <= 15 && shown < 5; n++) {
                if (n == NODE || peer_seen[n] == 0) continue;
                bool live = frame - peer_seen[n] < 120;          // < 2 s
                uint32_t k = peer_keys[n];
                printf("DS%-2d %s %c%c%c%c %c%c%c%c %c%c   \n", n,
                       live ? "ok  " : "muet",
                       (k & KEY_A) ? 'A' : '.', (k & KEY_B) ? 'B' : '.',
                       (k & KEY_X) ? 'X' : '.', (k & KEY_Y) ? 'Y' : '.',
                       (k & KEY_UP) ? '^' : '.', (k & KEY_DOWN) ? 'v' : '.',
                       (k & KEY_LEFT) ? '<' : '.', (k & KEY_RIGHT) ? '>' : '.',
                       (k & KEY_L) ? 'L' : '.', (k & KEY_R) ? 'R' : '.');
                shown++;
            }
            if (shown == 0) printf("aucune pour l'instant           \n");
        }
    }
    return 0;
}
