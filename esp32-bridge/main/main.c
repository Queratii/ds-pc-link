// Pont 802.11 brut <-> USB série pour Nintendo DS.
// ESP-IDF 5.x, testé en conception pour ESP32 / ESP32-S3 / ESP32-C3.
//
// Modes :
//   MODE_LINK     : homebrew — ne remonte que nos trames (BSSID 02:44:53:4C:4E:4B)
//   MODE_SNIFF    : capture brute de tout le canal (pour Wireshark)
//   MODE_NINTENDO : capture filtrée sur le multijoueur Nintendo (cartouches d'origine)
//
// Côté PC : pc-host/dslink.py (voir README).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "cobs.h"
#include "linkproto.h"

#define FW_VERSION      1
#define UART_PORT       UART_NUM_0     // port "UART" de la carte (pont USB-série)
#define UART_BAUD       921600
#define RX_RING_BYTES   (48 * 1024)

// --------------------------------------------------------------------------
// État global
// --------------------------------------------------------------------------
static volatile uint8_t g_mode    = MODE_LINK;
static volatile uint8_t g_channel = LINK_DEFAULT_CHANNEL;
static uint8_t  g_mac[6];
static uint16_t g_seq;

static RingbufHandle_t   g_rx_ring;
static SemaphoreHandle_t g_uart_lock;
static SemaphoreHandle_t g_tx_lock;

static volatile uint32_t st_rx, st_drop, st_tx, st_txerr;

// Hôte Nintendo détecté (BSSID appris depuis un beacon) — MODE_NINTENDO
static volatile bool g_host_known;
static uint8_t g_host_bssid[6];

// Beacon rejoué (expériences cartouches d'origine)
static uint8_t  g_beacon[512];
static volatile uint16_t g_beacon_len;
static volatile uint16_t g_beacon_ms;

typedef struct __attribute__((packed)) {
    int8_t   rssi;
    uint8_t  channel;
    uint32_t ts_us;
    uint16_t len;          // sans FCS
    uint8_t  frame[];
} rx_item_t;

// --------------------------------------------------------------------------
// Série : écriture d'une trame [type][payload][crc16] en COBS + 0x00
// --------------------------------------------------------------------------
static void ser_send(uint8_t type, const void *p1, size_t n1, const void *p2, size_t n2)
{
    static uint8_t raw[SER_MAX_FRAME + 8];
    static uint8_t enc[SER_MAX_FRAME + SER_MAX_FRAME / 254 + 16];

    xSemaphoreTake(g_uart_lock, portMAX_DELAY);
    size_t n = 0;
    raw[n++] = type;
    if (n1 && n + n1 + n2 + 2 <= sizeof raw) { memcpy(raw + n, p1, n1); n += n1; }
    if (n2 && n + n2 + 2 <= sizeof raw)      { memcpy(raw + n, p2, n2); n += n2; }
    uint16_t crc = crc16_ccitt(raw, n);
    raw[n++] = crc & 0xFF;
    raw[n++] = crc >> 8;
    size_t e = cobs_encode(raw, n, enc);
    enc[e++] = 0x00;
    uart_write_bytes(UART_PORT, enc, e);
    xSemaphoreGive(g_uart_lock);
}

static void ser_log(const char *msg)
{
    ser_send(SER_EVT_LOG, msg, strlen(msg), NULL, 0);
}

// --------------------------------------------------------------------------
// Réception radio (callback promiscuous) : filtre puis pousse dans le ring
// Ce callback tourne dans la tâche Wi-Fi : il doit rester très court.
// --------------------------------------------------------------------------
static bool frame_wanted(const uint8_t *f, size_t len)
{
    switch (g_mode) {
    case MODE_SNIFF:
        return true;

    case MODE_LINK: {
        LinkHeader h; const uint8_t *d;
        return link_parse_air_frame(f, len, &h, &d);
    }

    case MODE_NINTENDO: {
        if (len < 24) return false;
        const uint8_t *a1 = f + 4, *a2 = f + 10, *a3 = f + 16;
        // Beacon (FC = 0x80) portant l'IE Nintendo : on apprend le BSSID de l'hôte
        if (f[0] == 0x80 && len > 36 && find_nintendo_ie(f + 36, len - 36)) {
            memcpy(g_host_bssid, a3, 6);
            g_host_known = true;
            return true;
        }
        if (is_nintendo_mp_multicast(a1)) return true;
        if (g_host_known && (!memcmp(a1, g_host_bssid, 6) || !memcmp(a2, g_host_bssid, 6)
                             || !memcmp(a3, g_host_bssid, 6)))
            return true;
        return false;
    }
    }
    return false;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = buf;
    int len = pkt->rx_ctrl.sig_len - 4;               // retire le FCS
    if (len < 10 || len > 1600) return;
    if (!frame_wanted(pkt->payload, len)) return;

    rx_item_t *it;
    size_t sz = sizeof(rx_item_t) + len;
    if (xRingbufferSendAcquire(g_rx_ring, (void **)&it, sz, 0) != pdTRUE) {
        st_drop++;
        return;
    }
    it->rssi    = pkt->rx_ctrl.rssi;
    it->channel = pkt->rx_ctrl.channel;
    it->ts_us   = (uint32_t)esp_timer_get_time();
    it->len     = len;
    memcpy(it->frame, pkt->payload, len);
    xRingbufferSendComplete(g_rx_ring, it);
    st_rx++;
}

// Tâche : vide le ring vers le PC
static void uplink_task(void *arg)
{
    for (;;) {
        size_t sz;
        rx_item_t *it = xRingbufferReceive(g_rx_ring, &sz, portMAX_DELAY);
        if (!it) continue;

        if (g_mode == MODE_LINK) {
            LinkHeader h; const uint8_t *d;
            if (link_parse_air_frame(it->frame, it->len, &h, &d)) {
                uint8_t pre[1 + sizeof h];
                pre[0] = (uint8_t)it->rssi;
                memcpy(pre + 1, &h, sizeof h);
                ser_send(SER_EVT_RX_LINK, pre, sizeof pre, d, h.len);
            }
        } else {
            uint8_t pre[6];
            pre[0] = (uint8_t)it->rssi;
            pre[1] = it->channel;
            memcpy(pre + 2, &it->ts_us, 4);
            ser_send(SER_EVT_RX_RAW, pre, sizeof pre, it->frame, it->len);
        }
        vRingbufferReturnItem(g_rx_ring, it);
    }
}

// --------------------------------------------------------------------------
// Émission radio
// --------------------------------------------------------------------------
static void radio_tx(const uint8_t *frame, size_t len, uint8_t rate)
{
    xSemaphoreTake(g_tx_lock, portMAX_DELAY);
    // Débit 802.11b : la radio DS ne fait que 1 ou 2 Mbit/s.
    // Note : à partir d'ESP-IDF 5.4, cette fonction est remplacée par
    // esp_wifi_config_80211_tx() ; adaptez si le compilateur le signale.
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA,
                                  rate == RATE_2M ? WIFI_PHY_RATE_2M_L : WIFI_PHY_RATE_1M_L);
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    xSemaphoreGive(g_tx_lock);
    if (err == ESP_OK) st_tx++; else st_txerr++;
}

static void send_link(uint8_t node, const uint8_t *data, uint16_t len)
{
    static uint8_t frame[LINK_80211_HDR_LEN + sizeof(LinkHeader) + LINK_MAX_DATA];
    size_t n = link_build_air_frame(frame, g_mac, node, g_seq++, data, len);
    radio_tx(frame, n, RATE_2M);
}

// Rejoue périodiquement un beacon capturé (expérience "la DS voit-elle l'hôte ?")
static void beacon_task(void *arg)
{
    for (;;) {
        uint16_t ms = g_beacon_ms, len = g_beacon_len;
        if (ms == 0 || len < 36) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        uint64_t ts = esp_timer_get_time();          // horodatage 802.11 (µs)
        memcpy(g_beacon + 24, &ts, 8);
        radio_tx(g_beacon, len, RATE_2M);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

// --------------------------------------------------------------------------
// Commandes du PC
// --------------------------------------------------------------------------
static void handle_cmd(const uint8_t *m, size_t n)
{
    uint8_t type = m[0];
    const uint8_t *p = m + 1;
    size_t pl = n - 1;
    char buf[64];

    switch (type) {
    case SER_CMD_PING: {
        uint8_t v[7] = { FW_VERSION };
        memcpy(v + 1, g_mac, 6);
        ser_send(SER_EVT_PONG, v, sizeof v, NULL, 0);
        break;
    }
    case SER_CMD_SET_CHANNEL:
        if (pl >= 1 && p[0] >= 1 && p[0] <= 13) {
            g_channel = p[0];
            esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
            snprintf(buf, sizeof buf, "canal %u", g_channel);
            ser_log(buf);
        }
        break;
    case SER_CMD_SET_MODE:
        if (pl >= 1 && p[0] <= MODE_NINTENDO) {
            g_mode = p[0];
            g_host_known = false;
            snprintf(buf, sizeof buf, "mode %u", g_mode);
            ser_log(buf);
        }
        break;
    case SER_CMD_SEND_LINK:
        if (pl >= 1) send_link(p[0], p + 1, (uint16_t)(pl - 1));
        break;
    case SER_CMD_TX_RAW:
        if (pl >= 1 + 24) radio_tx(p + 1, pl - 1, p[0]);
        break;
    case SER_CMD_BEACON_START:
        if (pl >= 2 + 36 && pl - 2 <= sizeof g_beacon) {
            g_beacon_ms = 0;
            memcpy(g_beacon, p + 2, pl - 2);
            g_beacon_len = (uint16_t)(pl - 2);
            g_beacon_ms = p[0] | (p[1] << 8);
            ser_log("beacon: rejeu actif");
        }
        break;
    case SER_CMD_BEACON_STOP:
        g_beacon_ms = 0;
        ser_log("beacon: arrêt");
        break;
    }
}

static void downlink_task(void *arg)
{
    static uint8_t acc[SER_MAX_FRAME * 2];
    static uint8_t dec[SER_MAX_FRAME * 2];
    size_t n = 0;
    uint8_t chunk[256];

    for (;;) {
        int r = uart_read_bytes(UART_PORT, chunk, sizeof chunk, pdMS_TO_TICKS(20));
        for (int i = 0; i < r; i++) {
            if (chunk[i] != 0x00) {
                if (n < sizeof acc) acc[n++] = chunk[i];
                else n = sizeof acc + 1;              // trop long : on jette
                continue;
            }
            if (n > 0 && n <= sizeof acc) {
                size_t d = cobs_decode(acc, n, dec);
                if (d >= 3) {
                    uint16_t crc = dec[d - 2] | (dec[d - 1] << 8);
                    if (crc == crc16_ccitt(dec, d - 2)) handle_cmd(dec, d - 2);
                }
            }
            n = 0;
        }
    }
}

static void stats_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t s[4] = { st_rx, st_drop, st_tx, st_txerr };
        ser_send(SER_EVT_STATS, s, sizeof s, NULL, 0);
    }
}

// --------------------------------------------------------------------------
// Initialisation
// --------------------------------------------------------------------------
void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, 8192, 16384, 0, NULL, 0);
    uart_param_config(UART_PORT, &uc);

    g_uart_lock = xSemaphoreCreateMutex();
    g_tx_lock   = xSemaphoreCreateMutex();
    g_rx_ring   = xRingbufferCreate(RX_RING_BYTES, RINGBUF_TYPE_NOSPLIT);

    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);   // la DS est en 802.11b
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, g_mac);

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(uplink_task,   "uplink",   4096, NULL, 10, NULL);
    xTaskCreate(downlink_task, "downlink", 4096, NULL, 10, NULL);
    xTaskCreate(beacon_task,   "beacon",   3072, NULL, 9,  NULL);
    xTaskCreate(stats_task,    "stats",    2048, NULL, 2,  NULL);

    ser_log("dslink pret");
}
