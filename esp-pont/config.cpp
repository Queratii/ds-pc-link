#include "config.h"
#include <EEPROM.h>
#include <string.h>

Config cfg;

/* Garde-fou : la structure doit tenir dans la zone EEPROM reservee. */
static_assert(sizeof(Config) <= CFG_EEPROM_SIZE,
              "Config trop grande pour la zone EEPROM");

/* CRC32 (polynome IEEE, version sans table pour economiser la flash) */
static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

static uint32_t cfgCrc(const Config &c) {
  /* Tout sauf le champ crc lui-meme, qui est en dernier. */
  return crc32((const uint8_t *)&c, sizeof(Config) - sizeof(uint32_t));
}

void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic   = CFG_MAGIC;
  cfg.version = CFG_VERSION;

  /* Liaison montante : rien de configure au premier demarrage. */
  cfg.sta_dhcp = 1;
  cfg.sta_mode = STA_PERSONAL;

  /* Point d'acces : ouvert au depart, sinon impossible de s'y connecter
   * pour faire la premiere configuration. */
  strcpy(cfg.ap_ssid, "ESP-Pont-Setup");
  cfg.ap_pass[0]  = '\0';
  cfg.ap_sec      = SEC_OPEN;
  cfg.ap_channel  = 6;
  cfg.ap_hidden   = 0;
  cfg.ap_max      = 4;
  cfg.ap_ip       = (uint32_t)IPAddress(192, 168, 4, 1);
  cfg.ap_mask     = (uint32_t)IPAddress(255, 255, 255, 0);

  /* Radio : 802.11b par defaut, pour que les consoles anciennes (DS) voient
   * le reseau sans reglage. Passer en g ou n pour du debit. */
  cfg.phy     = PHY_11B;
  cfg.tx_qdbm = 82;           /* 20.5 dBm */
  cfg.phy_sta = PHY_11N;      /* la liaison montante profite du debit max */

  cfg.nat_on        = 1;
  cfg.dns_custom_on = 0;
  cfg.dns_custom    = 0;

  strcpy(cfg.admin_user, "admin");
  cfg.admin_pass[0] = '\0';   /* pas d'authentification tant que vide */
  strcpy(cfg.hostname, "esp-pont");
}

bool cfgLoad() {
  EEPROM.begin(CFG_EEPROM_SIZE);
  Config tmp;
  EEPROM.get(0, tmp);
  EEPROM.end();

  if (tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION &&
      tmp.crc == cfgCrc(tmp)) {
    memcpy(&cfg, &tmp, sizeof(Config));

    /* Garde-fous : une valeur aberrante ne doit pas empecher le demarrage. */
    if (cfg.ap_channel < 1 || cfg.ap_channel > 13) cfg.ap_channel = 6;
    if (cfg.ap_max < 1 || cfg.ap_max > 8)          cfg.ap_max = 4;
    if (cfg.phy < PHY_11B || cfg.phy > PHY_11N)    cfg.phy = PHY_11B;
    if (cfg.phy_sta < PHY_11B || cfg.phy_sta > PHY_11N) cfg.phy_sta = PHY_11N;
    if (cfg.tx_qdbm > 82)                          cfg.tx_qdbm = 82;
    if (cfg.ap_sec > SEC_WPA_WPA2)                 cfg.ap_sec = SEC_OPEN;
    if (cfg.sta_mode > STA_ENTERPRISE)             cfg.sta_mode = STA_PERSONAL;
    if (cfg.relay_off > 1)                         cfg.relay_off = 0;
    if (cfg.ap_ip == 0)   cfg.ap_ip   = (uint32_t)IPAddress(192, 168, 4, 1);
    if (cfg.ap_mask == 0) cfg.ap_mask = (uint32_t)IPAddress(255, 255, 255, 0);
    if (cfg.ap_ssid[0] == '\0')  strcpy(cfg.ap_ssid, "ESP-Pont");
    if (cfg.hostname[0] == '\0') strcpy(cfg.hostname, "esp-pont");

    /* Terminaisons de chaines garanties. */
    cfg.sta_ssid[sizeof(cfg.sta_ssid) - 1]     = '\0';
    cfg.sta_pass[sizeof(cfg.sta_pass) - 1]     = '\0';
    cfg.ap_ssid[sizeof(cfg.ap_ssid) - 1]       = '\0';
    cfg.ap_pass[sizeof(cfg.ap_pass) - 1]       = '\0';
    cfg.admin_user[sizeof(cfg.admin_user) - 1] = '\0';
    cfg.admin_pass[sizeof(cfg.admin_pass) - 1] = '\0';
    cfg.hostname[sizeof(cfg.hostname) - 1]     = '\0';
    cfg.eap_anon[sizeof(cfg.eap_anon) - 1]     = '\0';
    cfg.eap_user[sizeof(cfg.eap_user) - 1]     = '\0';
    cfg.eap_pass[sizeof(cfg.eap_pass) - 1]     = '\0';
    return true;
  }

  cfgDefaults();
  return false;
}

bool cfgSave() {
  cfg.magic   = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.crc     = cfgCrc(cfg);

  EEPROM.begin(CFG_EEPROM_SIZE);
  EEPROM.put(0, cfg);
  bool ok = EEPROM.commit();
  EEPROM.end();
  return ok;
}

void cfgErase() {
  EEPROM.begin(CFG_EEPROM_SIZE);
  for (size_t i = 0; i < sizeof(Config); i++) EEPROM.write(i, 0xFF);
  EEPROM.commit();
  EEPROM.end();
}

const char *cfgSecurityName(uint8_t sec) {
  switch (sec) {
    case SEC_WPA:      return "WPA-PSK (TKIP)";
    case SEC_WPA2:     return "WPA2-PSK (AES)";
    case SEC_WPA_WPA2: return "WPA/WPA2 mixte";
    default:           return "Ouvert";
  }
}

const char *cfgPhyName(uint8_t phy) {
  switch (phy) {
    case PHY_11B: return "802.11b";
    case PHY_11G: return "802.11g";
    default:      return "802.11n";
  }
}
