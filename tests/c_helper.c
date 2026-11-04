// Outil de test : vérifie que les implémentations C et Python sont identiques.
//   ./c_helper cobs <hex>   -> COBS(hex + crc16) en hex
//   ./c_helper link <hex>   -> trame air construite (MAC 02:00:00:00:00:01, node 3, seq 42)
//   ./c_helper parse <hex>  -> "ok node seq len datahex" ou "ko"
//   ./c_helper parsep <hex> -> idem pour un message UDP (sans en-tête 802.11)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cobs.h"
#include "linkproto.h"

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    while (s[0] && s[1]) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; s += 2; }
    return n;
}

static void hex(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", d[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    static uint8_t in[4096], tmp[4096], out[8192];
    if (argc < 3) return 1;
    size_t n = unhex(argv[2], in);

    if (!strcmp(argv[1], "cobs")) {
        uint16_t crc = crc16_ccitt(in, n);
        memcpy(tmp, in, n); tmp[n] = crc & 0xFF; tmp[n + 1] = crc >> 8;
        size_t e = cobs_encode(tmp, n + 2, out);
        hex(out, e);
        // aller-retour
        size_t d = cobs_decode(out, e, tmp);
        if (d != n + 2 || memcmp(tmp, in, n)) { printf("ROUNDTRIP_FAIL\n"); return 2; }
    } else if (!strcmp(argv[1], "link")) {
        uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
        size_t l = link_build_air_frame(out, mac, 3, 42, in, (uint16_t)n);
        hex(out, l);
    } else if (!strcmp(argv[1], "parse")) {
        LinkHeader h; const uint8_t *d;
        if (link_parse_air_frame(in, n, &h, &d)) {
            printf("ok %u %u %u ", h.node, h.seq, h.len);
            hex(d, h.len);
        } else printf("ko\n");
    } else if (!strcmp(argv[1], "parsep")) {   // message UDP, sans en-tête 802.11
        LinkHeader h; const uint8_t *d;
        if (link_parse_payload(in, n, &h, &d)) {
            printf("ok %u %u %u ", h.node, h.seq, h.len);
            hex(d, h.len);
        } else printf("ko\n");
    }
    return 0;
}
