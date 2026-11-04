// COBS (Consistent Overhead Byte Stuffing) - framing série ESP32 <-> PC.
// Chaque trame encodée ne contient aucun octet 0x00 ; on la termine par 0x00.
#pragma once
#include <stddef.h>
#include <stdint.h>

// out doit faire au moins len + len/254 + 2 octets.
static inline size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t r = 0, w = 1, code_idx = 0;
    uint8_t code = 1;
    while (r < len) {
        if (in[r] == 0) {
            out[code_idx] = code; code = 1; code_idx = w++; r++;
        } else {
            out[w++] = in[r++]; code++;
            if (code == 0xFF) { out[code_idx] = code; code = 1; code_idx = w++; }
        }
    }
    out[code_idx] = code;
    return w;
}

// Retourne la taille décodée, ou 0 si la trame est invalide.
static inline size_t cobs_decode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t r = 0, w = 0;
    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) return 0;
        for (uint8_t i = 1; i < code; i++) {
            if (r >= len) return 0;
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) out[w++] = 0;
    }
    return w;
}
