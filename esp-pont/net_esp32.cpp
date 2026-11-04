/* Couche radio pour ESP32. Voir net_esp8266.cpp pour la variante ESP8266.
 *
 * Avantage decisif de l'ESP32 ici : esp_wifi_set_protocol() prend une
 * interface en argument. Le point d'acces peut donc rester en 802.11b pour
 * une console ancienne pendant que la liaison montante tourne en g ou en n.
 */
#if defined(ARDUINO_ARCH_ESP32)

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_eap_client.h>
#include <esp_idf_version.h>
#include <soc/soc_caps.h>

#include "net.h"
#include "config.h"

#define RECOVERY_SSID   "ESP-Pont-Secours"
#define UPLINK_RETRY_MS 20000UL

static DNSServer dnsServer;
static bool      g_recovery    = false;
static bool      g_portal      = false;
static bool      g_naptActive  = false;
static bool      g_apOk        = false;
static bool      g_mdnsUp      = false;
static LinkState g_link        = LINK_IDLE;
static uint32_t  g_lastAttempt = 0;

/* ------------------------------------------------------------------ */
/* Conversions                                                         */
/* ------------------------------------------------------------------ */

/* Le protocole est un masque de bits cumulatif : le mode g inclut le b,
 * le mode n inclut les deux, exactement comme le veut la norme en 2,4 GHz.
 * Le mode b seul n'annonce que les debits DSSS, ceux de la DS. */
static uint8_t protoBitmap(uint8_t phy) {
  switch (phy) {
    case PHY_11B: return WIFI_PROTOCOL_11B;
    case PHY_11G: return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
    default:      return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  }
}

static uint8_t phyOfBitmap(uint8_t bm) {
  if (bm & WIFI_PROTOCOL_11N) return PHY_11N;
  if (bm & WIFI_PROTOCOL_11G) return PHY_11G;
  return PHY_11B;
}

static wifi_auth_mode_t authOfCfg() {
  switch (cfg.ap_sec) {
    case SEC_WPA:      return WIFI_AUTH_WPA_PSK;
    case SEC_WPA2:     return WIFI_AUTH_WPA2_PSK;
    case SEC_WPA_WPA2: return WIFI_AUTH_WPA_WPA2_PSK;
    default:           return WIFI_AUTH_OPEN;
  }
}

static wifi_cipher_type_t cipherOfCfg() {
  /* Le WPA d'origine impose TKIP, le WPA2 utilise CCMP, le mode mixte
   * propose les deux pour que les vieux clients WPA puissent s'associer. */
  switch (cfg.ap_sec) {
    case SEC_WPA:      return WIFI_CIPHER_TYPE_TKIP;
    case SEC_WPA_WPA2: return WIFI_CIPHER_TYPE_TKIP_CCMP;
    default:           return WIFI_CIPHER_TYPE_CCMP;
  }
}

/* L'ESP32 ne prend pas une puissance en dBm mais un palier predefini.
 * On choisit le palier immediatement inferieur a la valeur demandee. */
static wifi_power_t txPowerOfCfg() {
  float dbm = (float)cfg.tx_qdbm * 0.25f;
  if (dbm >= 19.5f) return WIFI_POWER_19_5dBm;
  if (dbm >= 19.0f) return WIFI_POWER_19dBm;
  if (dbm >= 18.5f) return WIFI_POWER_18_5dBm;
  if (dbm >= 17.0f) return WIFI_POWER_17dBm;
  if (dbm >= 15.0f) return WIFI_POWER_15dBm;
  if (dbm >= 13.0f) return WIFI_POWER_13dBm;
  if (dbm >= 11.0f) return WIFI_POWER_11dBm;
  if (dbm >= 8.5f)  return WIFI_POWER_8_5dBm;
  if (dbm >= 7.0f)  return WIFI_POWER_7dBm;
  if (dbm >= 5.0f)  return WIFI_POWER_5dBm;
  if (dbm >= 2.0f)  return WIFI_POWER_2dBm;
  return WIFI_POWER_MINUS_1dBm;
}

IPAddress netClientDns() {
  IPAddress apIp = g_recovery ? IPAddress(192, 168, 4, 1) : IPAddress(cfg.ap_ip);
  if (g_recovery || g_link != LINK_ONLINE) {
    /* Hors ligne : on s'annonce soi-meme, le portail captif repond a tout. */
    return apIp;
  }
  if (cfg.dns_custom_on && cfg.dns_custom != 0) return IPAddress(cfg.dns_custom);
  IPAddress up = WiFi.STA.dnsIP(0);
  if (up != IPAddress((uint32_t)0)) return up;
  return apIp;
}

/* ------------------------------------------------------------------ */
/* Point d'acces                                                       */
/* ------------------------------------------------------------------ */

static void startAp() {
  IPAddress ip   = g_recovery ? IPAddress(192, 168, 4, 1)   : IPAddress(cfg.ap_ip);
  IPAddress mask = g_recovery ? IPAddress(255, 255, 255, 0) : IPAddress(cfg.ap_mask);

  WiFi.AP.begin();

  /* Sur une interface serveur, config() interprete ses deux derniers
   * arguments comme le debut de la plage DHCP puis le DNS a annoncer aux
   * clients. Zero laisse la plage par defaut. */
  WiFi.AP.config(ip, ip, mask, IPAddress((uint32_t)0), netClientDns());

  const char *ssid    = g_recovery ? RECOVERY_SSID : cfg.ap_ssid;
  int         channel = g_recovery ? 1 : (int)cfg.ap_channel;
  int         maxConn = g_recovery ? 4 : (int)cfg.ap_max;
  int         hidden  = (!g_recovery && cfg.ap_hidden) ? 1 : 0;

  /* Une cle WPA doit faire au moins huit caracteres, sinon le pilote
   * refuse la configuration : on retombe alors en reseau ouvert. */
  bool useOpen = g_recovery || cfg.ap_sec == SEC_OPEN || strlen(cfg.ap_pass) < 8;
  const char *psk = useOpen ? (const char *)NULL : (const char *)cfg.ap_pass;

  wifi_auth_mode_t auth = useOpen ? WIFI_AUTH_OPEN : authOfCfg();

  g_apOk = WiFi.AP.create(ssid, psk, channel, hidden, maxConn, false,
                          auth, cipherOfCfg());

  /* Le reglage qui justifie le passage a l'ESP32 : protocole propre au
   * point d'acces, independant de celui de la liaison montante.
   * Le secours force le 802.11g : si le reglage courant rendait le point
   * d'acces invisible pour un appareil, il reste joignable. */
  esp_wifi_set_protocol(WIFI_IF_AP, protoBitmap(g_recovery ? PHY_11G : cfg.phy));
}

/* ------------------------------------------------------------------ */
/* Liaison montante                                                    */
/* ------------------------------------------------------------------ */

static void enterpriseClear() {
  esp_wifi_sta_enterprise_disable();
}

static void staBegin() {
  bool ent = (cfg.sta_mode == STA_ENTERPRISE &&
              cfg.eap_user[0] != '\0' && cfg.eap_pass[0] != '\0');

  enterpriseClear();

  /* Le protocole doit etre fixe avant l'association, pas pendant. */
  esp_wifi_set_protocol(WIFI_IF_STA, protoBitmap(cfg.phy_sta));

  if (ent) {
    /* Pas d'horloge temps reel : sans cela le certificat du serveur RADIUS
     * serait juge hors de sa periode de validite. */
    esp_eap_client_set_disable_time_check(true);

    const char *anon = cfg.eap_anon[0] ? cfg.eap_anon : cfg.eap_user;
    esp_eap_client_set_identity((const unsigned char *)anon, strlen(anon));
    esp_eap_client_set_username((const unsigned char *)cfg.eap_user, strlen(cfg.eap_user));
    esp_eap_client_set_password((const unsigned char *)cfg.eap_pass, strlen(cfg.eap_pass));
    esp_wifi_sta_enterprise_enable();

    WiFi.begin(cfg.sta_ssid);   /* aucune cle partagee en 802.1X */
  } else {
    WiFi.begin(cfg.sta_ssid, cfg.sta_pass);
  }

  g_link = LINK_CONNECTING;
  g_lastAttempt = millis();
}

static void startSta() {
  if (g_recovery || cfg.sta_ssid[0] == '\0') {
    enterpriseClear();
    WiFi.disconnect(false);
    g_link = LINK_IDLE;
    return;
  }

  WiFi.STA.setHostname(cfg.hostname);

  if (cfg.sta_dhcp) {
    WiFi.STA.config();   /* sans argument : retour au DHCP */
  } else {
    WiFi.STA.config(IPAddress(cfg.sta_ip), IPAddress(cfg.sta_gw),
                    IPAddress(cfg.sta_mask), IPAddress(cfg.sta_dns));
  }

  staBegin();
}

/* ------------------------------------------------------------------ */
/* Portail captif                                                      */
/* ------------------------------------------------------------------ */

/* Il detourne toutes les requetes DNS, il ne doit donc tourner que tant
 * que la liaison montante est absente, sans quoi les clients du pont ne
 * resoudraient plus aucun nom de domaine. */
static void updatePortal() {
  bool want = g_recovery || g_link != LINK_ONLINE;
  if (want && !g_portal) {
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(0);
    if (dnsServer.start(53, "*", WiFi.AP.localIP())) g_portal = true;
  } else if (!want && g_portal) {
    dnsServer.stop();
    g_portal = false;
  }
}

/* ------------------------------------------------------------------ */
/* Transitions                                                         */
/* ------------------------------------------------------------------ */

static void onOnline() {
  g_link = LINK_ONLINE;

  /* Reannonce du bon DNS aux clients du point d'acces. */
  WiFi.AP.dnsIP(0, netClientDns());

  if (cfg.nat_on) g_naptActive = WiFi.AP.enableNAPT(true);

  if (!g_mdnsUp) {
    g_mdnsUp = MDNS.begin(cfg.hostname);
    if (g_mdnsUp) MDNS.addService("http", "tcp", 80);
  }
}

static void onOffline() {
  g_link = LINK_CONNECTING;
  g_naptActive = false;
  WiFi.AP.dnsIP(0, netClientDns());
  g_lastAttempt = millis();
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */

void netBegin(bool recovery) {
  g_recovery = recovery;

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(WIFI_PS_NONE);   /* un routeur ne dort pas */

  netApply();
}

void netApply() {
  WiFi.mode(g_recovery ? WIFI_AP : WIFI_AP_STA);

  /* Sur les puces bibande, type ESP32-C5, on verrouille la bande 2,4 GHz :
   * la radio est unique, et une liaison montante partie en 5 GHz y
   * entrainerait le point d'acces, laissant la console sans reseau.
   * Cette fonction n'existe qu'a partir d'ESP-IDF 5.4 (arduino-esp32 3.2),
   * et seulement sur les puces qui ont une radio 5 GHz. */
#if defined(SOC_WIFI_SUPPORT_5G) && SOC_WIFI_SUPPORT_5G && \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
#endif

  WiFi.setTxPower(txPowerOfCfg());

  startAp();
  startSta();

  if (g_link != LINK_ONLINE) {
    WiFi.AP.enableNAPT(false);
    g_naptActive = false;
  }
  updatePortal();
}

void netEnterRecovery() {
  g_recovery = true;
  netApply();   /* startAp() force le 802.11g en secours */
}

void netRetryUplink() {
  if (g_recovery) return;
  WiFi.disconnect(false);
  startSta();   /* gere aussi le cas d'une liaison amont non configuree */
}

void netLoop() {
  if (!g_recovery && cfg.sta_ssid[0] != '\0') {
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && g_link != LINK_ONLINE) {
      onOnline();
    } else if (!connected && g_link == LINK_ONLINE) {
      onOffline();
    } else if (!connected && millis() - g_lastAttempt > UPLINK_RETRY_MS) {
      g_lastAttempt = millis();
      staBegin();
    }
  }

  updatePortal();
  if (g_portal) dnsServer.processNextRequest();
}

bool      netRecovery()     { return g_recovery; }
LinkState netLink()         { return g_link; }
bool      netPortalActive() { return g_portal; }
bool      netNaptActive()   { return g_naptActive; }
bool      netApOk()         { return g_apOk; }

uint8_t netActualPhy() {
  uint8_t bm = 0;
  if (esp_wifi_get_protocol(WIFI_IF_AP, &bm) != ESP_OK) return cfg.phy;
  return phyOfBitmap(bm);
}

uint8_t netActualPhySta() {
  uint8_t bm = 0;
  if (esp_wifi_get_protocol(WIFI_IF_STA, &bm) != ESP_OK) return cfg.phy_sta;
  return phyOfBitmap(bm);
}

const char *netLinkName() {
  if (g_recovery) return "secours";
  switch (g_link) {
    case LINK_ONLINE:     return "connecte";
    case LINK_CONNECTING: return "connexion";
    default:              return "inactif";
  }
}

#endif /* ARDUINO_ARCH_ESP32 */
