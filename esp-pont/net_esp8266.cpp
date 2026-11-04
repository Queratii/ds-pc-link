/* Couche radio pour ESP8266. Voir net_esp32.cpp pour la variante ESP32. */
#if defined(ARDUINO_ARCH_ESP8266)

#include <ESP8266WiFi.h>
#include <LwipDhcpServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <lwip/napt.h>
#include <lwip/dns.h>

extern "C" {
#include "user_interface.h"
#include "wpa2_enterprise.h"
}

#include "net.h"
#include "config.h"

/* Le routage NAT (ip_napt_*) n'existe que dans les variantes lwIP compilees
 * "avec features" et sans IPv6. Dans l'IDE Arduino :
 *   Outils > lwIP Variant > "v2 Higher Bandwidth"  (ou "v2 Lower Memory")
 * Les variantes "(no features)" et "IPv6" ne conviennent pas. */
#if !LWIP_FEATURES || LWIP_IPV6
#error "Choisir Outils > lwIP Variant > v2 Higher Bandwidth (variante AVEC features, sans IPv6)"
#endif

/* Taille des tables NAT. 256 entrees consomment environ 8 Ko de RAM,
 * largement suffisant pour quelques consoles et un ESP8266. */
#define NAPT_TABLE_SIZE   256
#define NAPT_PORTMAP_SIZE 8

#define RECOVERY_SSID "ESP-Pont-Secours"
#define UPLINK_RETRY_MS 20000UL

static DNSServer  dnsServer;
static bool       g_recovery   = false;
static bool       g_naptInit   = false;
static bool       g_naptActive = false;
static bool       g_portal     = false;
static LinkState  g_link       = LINK_IDLE;
static uint32_t   g_lastAttempt = 0;
static bool       g_mdnsUp     = false;
static bool       g_apOk       = false;

/* ------------------------------------------------------------------ */
/* Utilitaires                                                         */
/* ------------------------------------------------------------------ */

static WiFiPhyMode_t phyOfCfg() {
  switch (cfg.phy) {
    case PHY_11B: return WIFI_PHY_MODE_11B;
    case PHY_11G: return WIFI_PHY_MODE_11G;
    default:      return WIFI_PHY_MODE_11N;
  }
}

/* Le SDK type ce champ comme une enumeration AUTH_MODE, pas comme un entier. */
static AUTH_MODE authOfCfg() {
  switch (cfg.ap_sec) {
    case SEC_WPA:      return AUTH_WPA_PSK;
    case SEC_WPA2:     return AUTH_WPA2_PSK;
    case SEC_WPA_WPA2: return AUTH_WPA_WPA2_PSK;
    default:           return AUTH_OPEN;
  }
}

IPAddress netClientDns() {
  IPAddress apIp = WiFi.softAPIP();
  if (g_recovery || g_link != LINK_ONLINE) {
    /* Hors ligne : on annonce l'ESP lui-meme, le portail captif repond a tout. */
    return apIp;
  }
  if (cfg.dns_custom_on && cfg.dns_custom != 0) {
    return IPAddress(cfg.dns_custom);
  }
  IPAddress up = WiFi.dnsIP(0);
  if (up.isSet()) return up;
  return apIp;
}

/* Annonce le bon serveur DNS dans les baux DHCP du point d'acces.
 * Les clients deja connectes le recuperent a leur prochain renouvellement. */
static void applyClientDns() {
  WiFi.softAPDhcpServer().setDns(netClientDns());
}

/* ------------------------------------------------------------------ */
/* NAT (partage de la connexion montante vers les clients du point d'acces) */
/* ------------------------------------------------------------------ */

static void naptEnable(bool on) {
  if (on) {
    if (!g_naptInit) {
      err_t initRet = ip_napt_init(NAPT_TABLE_SIZE, NAPT_PORTMAP_SIZE);
      if (initRet != ERR_OK) return;  /* memoire insuffisante */
      g_naptInit = true;
    }
    err_t enRet = ip_napt_enable_no(SOFTAP_IF, 1);
    g_naptActive = (enRet == ERR_OK);
  } else if (g_naptInit) {
    ip_napt_enable_no(SOFTAP_IF, 0);
    g_naptActive = false;
  }
}

/* ------------------------------------------------------------------ */
/* Point d'acces                                                       */
/* ------------------------------------------------------------------ */

static void startAp() {
  IPAddress ip, mask;
  if (g_recovery) {
    ip   = IPAddress(192, 168, 4, 1);
    mask = IPAddress(255, 255, 255, 0);
  } else {
    ip   = IPAddress(cfg.ap_ip);
    mask = IPAddress(cfg.ap_mask);
  }
  /* La passerelle annoncee aux clients est l'ESP lui-meme : c'est lui
   * qui route vers le reseau existant. */
  WiFi.softAPConfig(ip, ip, mask);

  const char *ssid    = g_recovery ? RECOVERY_SSID : cfg.ap_ssid;
  uint8_t     channel = g_recovery ? 1 : cfg.ap_channel;
  uint8_t     maxConn = g_recovery ? 4 : cfg.ap_max;
  int         hidden  = (!g_recovery && cfg.ap_hidden) ? 1 : 0;

  /* Un mot de passe WPA doit faire au moins 8 caracteres : en dessous,
   * le SDK refuse la configuration, on retombe donc en reseau ouvert. */
  bool useOpen = g_recovery || cfg.ap_sec == SEC_OPEN || strlen(cfg.ap_pass) < 8;
  const char *psk = useOpen ? (const char *)NULL : (const char *)cfg.ap_pass;

  g_apOk = WiFi.softAP(ssid, psk, (int)channel, hidden, (int)maxConn);

  /* WiFi.softAP() force WPA2 des qu'un mot de passe est fourni. Pour offrir
   * le choix WPA / WPA2 / mixte, on corrige le mode d'authentification
   * directement dans le SDK. */
  if (!useOpen) {
    struct softap_config sc;
    wifi_softap_get_config(&sc);
    AUTH_MODE want = authOfCfg();
    if (sc.authmode != want) {
      sc.authmode = want;
      wifi_softap_set_config_current(&sc);
    }
  }

  DhcpServer &dhcp = WiFi.softAPDhcpServer();
  dhcp.setRouter(true);       /* annonce l'ESP comme passerelle par defaut */
  dhcp.setLeaseTime(10);      /* minutes : renouvellement rapide des baux  */
  applyClientDns();
}

/* ------------------------------------------------------------------ */
/* Liaison montante                                                    */
/* ------------------------------------------------------------------ */

/* Efface toute trace d'une session 802.1X precedente. */
static void enterpriseClear() {
  wifi_station_set_wpa2_enterprise_auth(0);
  wifi_station_clear_enterprise_identity();
  wifi_station_clear_enterprise_username();
  wifi_station_clear_enterprise_password();
  wifi_station_clear_enterprise_ca_cert();
  wifi_station_clear_enterprise_cert_key();
}

/* Prepare l'authentification WPA2-Entreprise puis lance la connexion.
 * Utilise aussi bien au demarrage que dans la relance periodique. */
static void staBegin() {
  bool ent = (cfg.sta_mode == STA_ENTERPRISE &&
              cfg.eap_user[0] != '\0' && cfg.eap_pass[0] != '\0');

  enterpriseClear();

  if (ent) {
    /* L'ESP8266 n'a pas d'horloge temps reel : sans cette ligne, le
     * certificat du serveur RADIUS serait juge hors de sa periode de
     * validite et l'authentification echouerait systematiquement. */
    wifi_station_set_enterprise_disable_time_check(true);

    /* Identite exterieure, visible en clair sur l'air. A defaut
     * d'identite anonyme, on retombe sur l'identifiant reel. */
    const char *anon = cfg.eap_anon[0] ? cfg.eap_anon : cfg.eap_user;
    wifi_station_set_enterprise_identity((u8 *)anon, strlen(anon));
    wifi_station_set_enterprise_username((u8 *)cfg.eap_user, strlen(cfg.eap_user));
    wifi_station_set_enterprise_password((u8 *)cfg.eap_pass, strlen(cfg.eap_pass));
    wifi_station_set_wpa2_enterprise_auth(1);

    /* Aucune cle partagee en 802.1X : le SDK negocie EAP avec le serveur. */
    WiFi.begin(cfg.sta_ssid);
  } else {
    WiFi.begin(cfg.sta_ssid, cfg.sta_pass);
  }
}

static void startSta() {
  if (g_recovery || cfg.sta_ssid[0] == '\0') {
    enterpriseClear();
    WiFi.disconnect(false);
    g_link = LINK_IDLE;
    return;
  }

  WiFi.hostname(cfg.hostname);

  if (cfg.sta_dhcp) {
    /* 0.0.0.0 partout = retour au DHCP */
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  } else {
    WiFi.config(IPAddress(cfg.sta_ip), IPAddress(cfg.sta_gw),
                IPAddress(cfg.sta_mask), IPAddress(cfg.sta_dns));
  }

  staBegin();
  g_link = LINK_CONNECTING;
  g_lastAttempt = millis();
}

/* ------------------------------------------------------------------ */
/* Portail captif                                                      */
/* ------------------------------------------------------------------ */

/* Le portail captif detourne TOUTES les requetes DNS. Il ne doit donc
 * tourner que tant que la liaison montante est absente, sinon les clients
 * du pont ne pourraient plus resoudre le moindre nom de domaine. */
static void updatePortal() {
  bool want = g_recovery || g_link != LINK_ONLINE;
  if (want && !g_portal) {
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(0);
    if (dnsServer.start(53, "*", WiFi.softAPIP())) g_portal = true;
  } else if (!want && g_portal) {
    dnsServer.stop();
    g_portal = false;
  }
}

/* ------------------------------------------------------------------ */
/* Transitions d'etat                                                  */
/* ------------------------------------------------------------------ */

static void onOnline() {
  g_link = LINK_ONLINE;
  applyClientDns();
  naptEnable(cfg.nat_on != 0);

  /* WiFi.begin() peut renegocier le mode PHY : on le reimpose si besoin. */
  if (WiFi.getPhyMode() != phyOfCfg()) WiFi.setPhyMode(phyOfCfg());

  if (!g_mdnsUp) {
    g_mdnsUp = MDNS.begin(cfg.hostname);
    if (g_mdnsUp) MDNS.addService("http", "tcp", 80);
  }
}

static void onOffline() {
  g_link = LINK_CONNECTING;
  g_naptActive = false;
  applyClientDns();
  g_lastAttempt = millis();
}

/* ------------------------------------------------------------------ */
/* API publique                                                        */
/* ------------------------------------------------------------------ */

void netBegin(bool recovery) {
  g_recovery = recovery;

  WiFi.persistent(false);       /* la config est geree par nous, pas par le SDK */
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   /* un routeur ne dort pas */

  netApply();
}

void netApply() {
  WiFi.mode(g_recovery ? WIFI_AP : WIFI_AP_STA);

  /* En mode secours on force le 802.11g : si le reglage courant rendait le
   * point d'acces invisible pour un appareil donne, le secours reste joignable. */
  WiFiPhyMode_t want = g_recovery ? WIFI_PHY_MODE_11G : phyOfCfg();
  WiFi.setPhyMode(want);
  WiFi.setOutputPower((float)cfg.tx_qdbm * 0.25f);

  startAp();

  /* softAP() peut renegocier le mode PHY : on le reimpose si besoin. */
  if (WiFi.getPhyMode() != want) WiFi.setPhyMode(want);

  startSta();

  if (g_link != LINK_ONLINE) naptEnable(false);
  updatePortal();
}

void netEnterRecovery() {
  g_recovery = true;
  netApply();
}

void netRetryUplink() {
  if (g_recovery) return;
  WiFi.disconnect(false);
  startSta();
}

void netLoop() {
  if (!g_recovery && cfg.sta_ssid[0] != '\0') {
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && g_link != LINK_ONLINE) {
      onOnline();
    } else if (!connected && g_link == LINK_ONLINE) {
      onOffline();
    } else if (!connected && millis() - g_lastAttempt > UPLINK_RETRY_MS) {
      /* Relance periodique : le point d'acces du pont, lui, reste en place. */
      g_lastAttempt = millis();
      staBegin();
    }
  }

  updatePortal();
  if (g_portal) dnsServer.processNextRequest();
  if (g_mdnsUp) MDNS.update();
}

bool      netRecovery()     { return g_recovery; }
bool      netApOk()         { return g_apOk; }
uint8_t   netActualPhy()    { return (uint8_t)WiFi.getPhyMode(); }
/* Une seule radio, un seul reglage : les deux interfaces partagent le mode. */
uint8_t   netActualPhySta() { return (uint8_t)WiFi.getPhyMode(); }
LinkState netLink()         { return g_link; }
bool      netPortalActive() { return g_portal; }
bool      netNaptActive()   { return g_naptActive; }

const char *netLinkName() {
  if (g_recovery) return "secours";
  switch (g_link) {
    case LINK_ONLINE:     return "connecte";
    case LINK_CONNECTING: return "connexion";
    default:              return "inactif";
  }
}

#endif /* ARDUINO_ARCH_ESP8266 */
