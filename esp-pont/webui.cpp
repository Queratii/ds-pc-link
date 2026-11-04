#if defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  extern "C" {
  #include "user_interface.h"
  }
  typedef ESP8266WebServer PontWebServer;
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <esp_wifi.h>
  #include <esp_wifi_ap_get_sta_list.h>
  typedef WebServer PontWebServer;
#endif

#include "webui.h"
#include "net.h"
#include "config.h"
#include "page.h"
#include "dslink_relay.h"

static PontWebServer server(80);
static bool     g_reboot      = false;
static uint32_t g_rebootAt    = 0;
static bool     g_applyPending = false;
static uint32_t g_applyAt     = 0;
static bool     g_factory     = false;

/* ------------------------------------------------------------------ */
/* Petits utilitaires                                                  */
/* ------------------------------------------------------------------ */

static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static String ipStr(uint32_t v) {
  if (v == 0) return String();
  return IPAddress(v).toString();
}

static uint32_t parseIp(const String &s) {
  IPAddress a;
  if (s.length() && a.fromString(s)) return (uint32_t)a;
  return 0;
}

static String uptimeStr() {
  uint32_t s = millis() / 1000UL;
  uint32_t d = s / 86400UL; s %= 86400UL;
  uint32_t h = s / 3600UL;  s %= 3600UL;
  uint32_t m = s / 60UL;    s %= 60UL;
  char buf[32];
  if (d) snprintf(buf, sizeof(buf), "%lu j %02lu:%02lu:%02lu",
                  (unsigned long)d, (unsigned long)h, (unsigned long)m, (unsigned long)s);
  else   snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                  (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(buf);
}

/* Indice de securite utilise par l'interface :
 * 0 Ouvert, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 inconnu, 6 WPA3 */
#if defined(ARDUINO_ARCH_ESP8266)
static uint8_t encToIndex(uint8_t enc) {
  switch (enc) {
    case ENC_TYPE_NONE: return 0;
    case ENC_TYPE_WEP:  return 1;
    case ENC_TYPE_TKIP: return 2;
    case ENC_TYPE_CCMP: return 3;
    case ENC_TYPE_AUTO: return 4;
    default:            return 5;
  }
}
#else
static uint8_t encToIndex(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:          return 0;
    case WIFI_AUTH_WEP:           return 1;
    case WIFI_AUTH_WPA_PSK:       return 2;
    case WIFI_AUTH_WPA2_PSK:      return 3;
    case WIFI_AUTH_WPA_WPA2_PSK:  return 4;
    case WIFI_AUTH_WPA3_PSK:      return 6;
    case WIFI_AUTH_WPA2_WPA3_PSK: return 6;
    default:                      return 3;  /* entreprise et autres : classe WPA2 */
  }
}
#endif

/* eduroam est un SSID normalise mondialement : utile comme repli quand le
 * balayage ne sait pas annoncer le 802.1X, ce qui est le cas de l'ESP8266. */
static bool ssidLooksEnterprise(const String &ssid) {
  String t = ssid;
  t.toLowerCase();
  return t == "eduroam" || t.startsWith("eduroam-") || t.endsWith("-eduroam");
}

/* Reseau a authentification 802.1X ?
 * L'ESP32 le rapporte directement dans le resultat du balayage. L'ESP8266
 * renvoie AUTH_WPA2_PSK pour l'entreprise comme pour le personnel, il faut
 * donc se rabattre sur le nom du reseau. */
static bool netIsEnterprise(int i) {
#if defined(ARDUINO_ARCH_ESP8266)
  return ssidLooksEnterprise(WiFi.SSID(i));
#else
  wifi_auth_mode_t a = WiFi.encryptionType(i);
  if (a == WIFI_AUTH_ENTERPRISE || a == WIFI_AUTH_WPA_ENTERPRISE ||
      a == WIFI_AUTH_WPA3_ENTERPRISE || a == WIFI_AUTH_WPA3_ENT_192) {
    return true;
  }
  return ssidLooksEnterprise(WiFi.SSID(i));
#endif
}

/* Le reseau accepte-t-il les debits 802.11b, seuls compris par une DS ? */
static bool netSupports11b(int i) {
#if defined(ARDUINO_ARCH_ESP8266)
  const struct bss_info *bi = (const struct bss_info *)WiFi.getScanInfoByIndex(i);
  return bi ? (bi->phy_11b != 0) : true;
#else
  const wifi_ap_record_t *r = (const wifi_ap_record_t *)WiFi.getScanInfoByIndex(i);
  return r ? (r->phy_11b != 0) : true;
#endif
}

/* Protege les pages si un mot de passe d'administration est defini. */
static bool guard() {
  if (cfg.admin_pass[0] == '\0') return true;
  if (!server.authenticate(cfg.admin_user, cfg.admin_pass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static void handleRoot() {
  if (!guard()) return;
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send_P(200, "text/html", PAGE_INDEX);
}

static void handleStatus() {
  if (!guard()) return;

  IPAddress apIp = WiFi.softAPIP();
  String j = "{";
  j += "\"link\":\"";       j += netLinkName();  j += "\",";
  j += "\"link_label\":\""; j += netLinkName();  j += "\",";
  j += "\"nat\":";          j += netNaptActive() ? "true" : "false"; j += ",";
  j += "\"portal\":";       j += netPortalActive() ? "true" : "false"; j += ",";
  j += "\"recovery\":";     j += netRecovery() ? "true" : "false"; j += ",";
  j += "\"sta_ssid\":\"";   j += jsonEscape(String(cfg.sta_ssid)); j += "\",";
  j += "\"sta_auth\":\"";   j += (cfg.sta_mode == STA_ENTERPRISE ? "WPA2-Entreprise (802.1X)" : "WPA/WPA2 personnel"); j += "\",";
  j += "\"sta_ip\":\"";     j += (netLink() == LINK_ONLINE ? WiFi.localIP().toString() : String("-")); j += "\",";
  j += "\"rssi\":";         j += (netLink() == LINK_ONLINE ? String(WiFi.RSSI()) : String("0")); j += ",";
  j += "\"ap_ssid\":\"";    j += jsonEscape(WiFi.softAPSSID()); j += "\",";
  j += "\"ap_sec\":\"";     j += (netRecovery() ? "Ouvert (secours)" : cfgSecurityName(cfg.ap_sec)); j += "\",";
  j += "\"ap_ip\":\"";      j += apIp.toString(); j += "\",";
  j += "\"channel\":";      j += String(WiFi.channel()); j += ",";
  j += "\"phy\":\"";        j += cfgPhyName(netActualPhy()); j += "\",";
  j += "\"phy_sta\":\"";    j += cfgPhyName(netActualPhySta()); j += "\",";
  j += "\"per_iface\":";    j += NET_PER_IFACE_PHY ? "true" : "false"; j += ",";
  j += "\"clients\":";      j += String(WiFi.softAPgetStationNum()); j += ",";
  j += "\"client_dns\":\""; j += netClientDns().toString(); j += "\",";
  j += "\"heap\":";         j += String(ESP.getFreeHeap()); j += ",";
  j += "\"uptime\":\"";     j += uptimeStr(); j += "\",";
  j += "\"relay\":";        j += relayActive() ? "true" : "false"; j += ",";
  j += "\"relay_port\":";   j += String(DSLINK_UDP_PORT); j += ",";
  j += "\"relay_pc\":\"";
  if (relayPcKnown()) {
    j += relayPcIp().toString(); j += ':'; j += String(relayPcPort());
  }
  j += "\",";
  j += "\"relay_ds\":";     j += String(relayDsCount()); j += ",";

  j += "\"stations\":[";
  bool first = true;
  char mac[18];

#if defined(ARDUINO_ARCH_ESP8266)
  struct station_info *si = wifi_softap_get_station_info();
  while (si != NULL) {
    if (!first) j += ',';
    first = false;
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             si->bssid[0], si->bssid[1], si->bssid[2],
             si->bssid[3], si->bssid[4], si->bssid[5]);
    j += "{\"ip\":\"";
    j += IPAddress(si->ip.addr).toString();
    j += "\",\"mac\":\"";
    j += mac;
    j += "\"}";
    si = si->next.stqe_next;
  }
  wifi_softap_free_station_info();
#else
  wifi_sta_list_t        stas;
  wifi_sta_mac_ip_list_t pairs;
  if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK &&
      esp_wifi_ap_get_sta_list_with_ip(&stas, &pairs) == ESP_OK) {
    for (int k = 0; k < pairs.num; k++) {
      if (!first) j += ',';
      first = false;
      const uint8_t *m = pairs.sta[k].mac;
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               m[0], m[1], m[2], m[3], m[4], m[5]);
      j += "{\"ip\":\"";
      j += IPAddress(pairs.sta[k].ip.addr).toString();
      j += "\",\"mac\":\"";
      j += mac;
      j += "\"}";
    }
  }
#endif
  j += "]}";

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), j);
}

static void handleGetConfig() {
  if (!guard()) return;

  String j = "{";
  j += "\"sta_ssid\":\"";  j += jsonEscape(String(cfg.sta_ssid)); j += "\",";
  j += "\"sta_dhcp\":";    j += cfg.sta_dhcp ? "1" : "0"; j += ",";
  j += "\"sta_ip\":\"";    j += ipStr(cfg.sta_ip);   j += "\",";
  j += "\"sta_gw\":\"";    j += ipStr(cfg.sta_gw);   j += "\",";
  j += "\"sta_mask\":\"";  j += ipStr(cfg.sta_mask); j += "\",";
  j += "\"sta_dns\":\"";   j += ipStr(cfg.sta_dns);  j += "\",";
  j += "\"sta_mode\":";    j += String(cfg.sta_mode); j += ",";
  j += "\"eap_anon\":\"";  j += jsonEscape(String(cfg.eap_anon)); j += "\",";
  j += "\"eap_user\":\"";  j += jsonEscape(String(cfg.eap_user)); j += "\",";
  j += "\"ap_ssid\":\"";   j += jsonEscape(String(cfg.ap_ssid)); j += "\",";
  j += "\"ap_sec\":";      j += String(cfg.ap_sec);     j += ",";
  j += "\"ap_channel\":";  j += String(cfg.ap_channel); j += ",";
  j += "\"ap_hidden\":";   j += cfg.ap_hidden ? "1" : "0"; j += ",";
  j += "\"ap_max\":";      j += String(cfg.ap_max);     j += ",";
  j += "\"ap_ip\":\"";     j += ipStr(cfg.ap_ip);   j += "\",";
  j += "\"ap_mask\":\"";   j += ipStr(cfg.ap_mask); j += "\",";
  j += "\"phy\":";         j += String(cfg.phy);     j += ",";
  j += "\"phy_sta\":";     j += String(cfg.phy_sta); j += ",";
  j += "\"tx_qdbm\":";     j += String(cfg.tx_qdbm); j += ",";
  j += "\"nat_on\":";      j += cfg.nat_on ? "1" : "0"; j += ",";
  j += "\"dns_custom_on\":"; j += cfg.dns_custom_on ? "1" : "0"; j += ",";
  j += "\"dns_custom\":\""; j += ipStr(cfg.dns_custom); j += "\",";
  j += "\"relay_on\":";    j += cfg.relay_off ? "0" : "1"; j += ",";
  j += "\"hostname\":\"";  j += jsonEscape(String(cfg.hostname));   j += "\",";
  j += "\"admin_user\":\""; j += jsonEscape(String(cfg.admin_user)); j += "\"";
  j += "}";

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), j);
}

/* Copie une valeur texte du formulaire si elle est presente. */
static void takeStr(const char *name, char *dest, size_t size) {
  if (!server.hasArg(name)) return;
  String v = server.arg(name);
  strncpy(dest, v.c_str(), size - 1);
  dest[size - 1] = '\0';
}

static uint8_t takeU8(const char *name, uint8_t cur, uint8_t lo, uint8_t hi) {
  if (!server.hasArg(name)) return cur;
  long v = server.arg(name).toInt();
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return (uint8_t)v;
}

static uint8_t takeBool(const char *name, uint8_t cur) {
  if (!server.hasArg(name)) return cur;
  return server.arg(name) == "1" ? 1 : 0;
}

static uint32_t takeIp(const char *name, uint32_t cur) {
  if (!server.hasArg(name)) return cur;
  return parseIp(server.arg(name));
}

static void fail(const char *msg) {
  String j = "{\"ok\":false,\"error\":\"";
  j += msg;
  j += "\"}";
  server.send(200, F("application/json"), j);
}

static void handlePostConfig() {
  if (!guard()) return;

  /* Sauvegarde : la configuration est restauree telle quelle si une
   * valeur envoyee par le formulaire est refusee. */
  Config n = cfg;

  /* --- liaison montante --- */
  takeStr("sta_ssid", cfg.sta_ssid, sizeof(cfg.sta_ssid));
  takeStr("sta_pass", cfg.sta_pass, sizeof(cfg.sta_pass));
  cfg.sta_dhcp = takeBool("sta_dhcp", cfg.sta_dhcp);
  cfg.sta_ip   = takeIp("sta_ip",   cfg.sta_ip);
  cfg.sta_gw   = takeIp("sta_gw",   cfg.sta_gw);
  cfg.sta_mask = takeIp("sta_mask", cfg.sta_mask);
  cfg.sta_dns  = takeIp("sta_dns",  cfg.sta_dns);
  cfg.sta_mode = takeU8("sta_mode", cfg.sta_mode, STA_PERSONAL, STA_ENTERPRISE);
  takeStr("eap_anon", cfg.eap_anon, sizeof(cfg.eap_anon));
  takeStr("eap_user", cfg.eap_user, sizeof(cfg.eap_user));
  takeStr("eap_pass", cfg.eap_pass, sizeof(cfg.eap_pass));

  /* --- point d'acces --- */
  takeStr("ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
  takeStr("ap_pass", cfg.ap_pass, sizeof(cfg.ap_pass));
  cfg.ap_sec     = takeU8("ap_sec", cfg.ap_sec, SEC_OPEN, SEC_WPA_WPA2);
  cfg.ap_channel = takeU8("ap_channel", cfg.ap_channel, 1, 13);
  cfg.ap_max     = takeU8("ap_max", cfg.ap_max, 1, 8);
  cfg.ap_hidden  = takeBool("ap_hidden", cfg.ap_hidden);
  cfg.ap_ip      = takeIp("ap_ip",   cfg.ap_ip);
  cfg.ap_mask    = takeIp("ap_mask", cfg.ap_mask);

  /* --- radio --- */
  cfg.phy     = takeU8("phy", cfg.phy, PHY_11B, PHY_11N);
  cfg.phy_sta = takeU8("phy_sta", cfg.phy_sta, PHY_11B, PHY_11N);
  cfg.tx_qdbm = takeU8("tx_qdbm", cfg.tx_qdbm, 0, 82);

  /* --- pont et administration --- */
  cfg.nat_on        = takeBool("nat_on", cfg.nat_on);
  cfg.dns_custom_on = takeBool("dns_custom_on", cfg.dns_custom_on);
  cfg.dns_custom    = takeIp("dns_custom", cfg.dns_custom);
  cfg.relay_off     = takeBool("relay_on", cfg.relay_off ? 0 : 1) ? 0 : 1;
  takeStr("hostname",   cfg.hostname,   sizeof(cfg.hostname));
  takeStr("admin_user", cfg.admin_user, sizeof(cfg.admin_user));

  /* Un espace seul efface le mot de passe de la page. */
  if (server.hasArg("admin_pass")) {
    String v = server.arg("admin_pass");
    v.trim();
    strncpy(cfg.admin_pass, v.c_str(), sizeof(cfg.admin_pass) - 1);
    cfg.admin_pass[sizeof(cfg.admin_pass) - 1] = '\0';
  }

  /* --- validations --- */
  if (cfg.ap_ssid[0] == '\0') { cfg = n; return fail("Le SSID du point d'acces est vide"); }
  if (cfg.ap_sec != SEC_OPEN && strlen(cfg.ap_pass) < 8) {
    cfg = n;
    return fail("Mot de passe du point d'acces : 8 caracteres minimum en WPA/WPA2");
  }
  if (cfg.sta_ssid[0] != '\0' && cfg.sta_mode == STA_ENTERPRISE) {
    if (cfg.eap_user[0] == '\0') {
      cfg = n;
      return fail("WPA2-Entreprise : l'identifiant est obligatoire");
    }
    if (cfg.eap_pass[0] == '\0') {
      cfg = n;
      return fail("WPA2-Entreprise : le mot de passe est obligatoire");
    }
  }

  if (cfg.ap_ip == 0)   cfg.ap_ip   = (uint32_t)IPAddress(192, 168, 4, 1);
  if (cfg.ap_mask == 0) cfg.ap_mask = (uint32_t)IPAddress(255, 255, 255, 0);
  if (cfg.hostname[0] == '\0') strcpy(cfg.hostname, "esp-pont");
  if (cfg.admin_user[0] == '\0') strcpy(cfg.admin_user, "admin");

  /* Meme sous-reseau des deux cotes : le routage serait impossible. */
  if (netLink() == LINK_ONLINE) {
    uint32_t lan  = (uint32_t)WiFi.localIP() & (uint32_t)WiFi.subnetMask();
    uint32_t mine = cfg.ap_ip & cfg.ap_mask;
    if (lan == mine) {
      cfg = n;
      return fail("Le sous-reseau du pont doit differer de celui du routeur");
    }
  }

  bool ok = cfgSave();
  if (!ok) { cfg = n; return fail("Ecriture en flash impossible"); }

  server.send(200, F("application/json"), F("{\"ok\":true}"));

  /* La radio est reconfiguree juste apres, pour que la reponse parte d'abord. */
  g_applyPending = true;
  g_applyAt = millis() + 400;
}

static void handleScan() {
  if (!guard()) return;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, true);   /* asynchrone, reseaux masques inclus */
    server.send(200, F("application/json"), F("{\"scanning\":true}"));
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, F("application/json"), F("{\"scanning\":true}"));
    return;
  }

  String j = "{\"scanning\":false,\"nets\":[";
  for (int i = 0; i < n && i < 25; i++) {
    if (i) j += ',';
    j += "{\"ssid\":\"";
    j += jsonEscape(WiFi.SSID(i));
    j += "\",\"rssi\":";
    j += String(WiFi.RSSI(i));
    j += ",\"ch\":";
    j += String(WiFi.channel(i));
    j += ",\"sec\":";
    j += String(encToIndex(WiFi.encryptionType(i)));
    j += ",\"ent\":";
    j += netIsEnterprise(i) ? "1" : "0";
    j += ",\"b\":";
    j += netSupports11b(i) ? "1" : "0";
    j += '}';
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, F("application/json"), j);
}

static void handleReboot() {
  if (!guard()) return;
  server.send(200, F("application/json"), F("{\"ok\":true}"));
  g_reboot = true;
  g_rebootAt = millis() + 400;
}

static void handleFactory() {
  if (!guard()) return;
  server.send(200, F("application/json"), F("{\"ok\":true}"));
  g_factory = true;
  g_reboot = true;
  g_rebootAt = millis() + 400;
}

/* Portail captif : tant que la liaison montante est absente, toute requete
 * vers un autre hote est renvoyee vers la page de configuration. */
static void handleNotFound() {
  if (netPortalActive()) {
    String host = server.hostHeader();
    String ap   = WiFi.softAPIP().toString();
    if (host != ap) {
      String url = "http://" + ap + "/";
      server.sendHeader(F("Location"), url, true);
      server.sendHeader(F("Cache-Control"), F("no-store"));
      server.send(302, F("text/plain"), F(""));
      return;
    }
  }
  server.send(404, F("text/plain"), F("404"));
}

/* ------------------------------------------------------------------ */

void webBegin() {
  server.on("/",              HTTP_GET,  handleRoot);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/config",    HTTP_GET,  handleGetConfig);
  server.on("/api/config",    HTTP_POST, handlePostConfig);
  server.on("/api/scan",      HTTP_GET,  handleScan);
  server.on("/api/reboot",    HTTP_POST, handleReboot);
  server.on("/api/factory",   HTTP_POST, handleFactory);

  /* Sondes de detection de portail captif des systemes courants. */
  server.on("/generate_204",       HTTP_GET, handleNotFound);
  server.on("/gen_204",            HTTP_GET, handleNotFound);
  server.on("/hotspot-detect.html",HTTP_GET, handleNotFound);
  server.on("/ncsi.txt",           HTTP_GET, handleNotFound);
  server.on("/connecttest.txt",    HTTP_GET, handleNotFound);
  server.on("/success.txt",        HTTP_GET, handleNotFound);
  server.on("/fwlink",             HTTP_GET, handleNotFound);

  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();

  if (g_applyPending && (int32_t)(millis() - g_applyAt) >= 0) {
    g_applyPending = false;
    netApply();
  }
  if (g_reboot && (int32_t)(millis() - g_rebootAt) >= 0) {
    if (g_factory) cfgErase();
    delay(50);
    ESP.restart();
  }
}

bool webRebootPending() { return g_reboot; }
