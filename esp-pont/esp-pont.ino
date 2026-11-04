/*
 * ESP-Pont - passerelle Wi-Fi configurable par interface web
 * Cibles : NodeMCU V3 (LoLin) / ESP-12E / ESP-12F (ESP8266), ESP32 DevKit
 *
 * L'ESP se connecte a un reseau Wi-Fi existant (liaison montante) et
 * rediffuse un point d'acces dont on choisit le SSID, la securite
 * (Ouvert / WPA / WPA2 / mixte), le canal et le protocole 802.11 b/g/n.
 * Le trafic des clients est route vers le reseau existant par NAT.
 *
 * Toute la configuration se fait depuis la page web embarquee, joignable
 * en permanence sur l'adresse IP du point d'acces (192.168.4.1 par defaut)
 * et, une fois la liaison montante etablie, sur l'IP obtenue du routeur.
 *
 * Reglages Arduino IDE pour l'ESP8266 (detail dans README.md, section 2) :
 *   Carte           : Generic ESP8266 Module (core esp8266 3.1.2 ou plus)
 *   NONOS SDK       : nonos-sdk 3.0.5   (indispensable pour eduroam)
 *   lwIP Variant    : "v2 Higher Bandwidth"   (variante AVEC features)
 *   Flash Size      : 4MB (FS:2MB OTA:~1019KB) ou equivalent
 *   CPU Frequency   : 160 MHz recommande
 *   Vitesse serie   : 115200
 * Pour l'ESP32 : carte "ESP32 Dev Module", core arduino-esp32 3.x.
 */

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
  /* LED du module ESP-12 sur GPIO2 : elle s'allume a l'etat bas. */
  #define LED_ON_LEVEL  LOW
  #define LED_OFF_LEVEL HIGH
#else
  #include <WiFi.h>
  /* LED des cartes ESP32 DevKit sur GPIO2 : elle s'allume a l'etat haut.
   * La definition de carte generique ne declare pas LED_BUILTIN. */
  #define LED_ON_LEVEL  HIGH
  #define LED_OFF_LEVEL LOW
  #ifndef LED_BUILTIN
    #define LED_BUILTIN 2
  #endif
#endif

#include "config.h"
#include "net.h"
#include "webui.h"
#include "dslink_relay.h"

#define FW_VERSION "1.1"

/* Bouton FLASH de la carte NodeMCU (GPIO0, actif a l'etat bas).
 * Ne jamais le maintenir pendant la mise sous tension : l'ESP8266
 * demarrerait en mode programmation. Il faut l'appuyer carte demarree. */
#define BTN_PIN            0
#define HOLD_RECOVERY_MS   3000UL
#define HOLD_FACTORY_MS    10000UL

#define LED_PIN            LED_BUILTIN   /* GPIO2, actif a l'etat bas */

static bool     btnDown        = false;
static uint32_t btnSince       = 0;
static bool     recoveryArmed  = false;

static bool     ledOn   = false;

/* ------------------------------------------------------------------ */

static void ledLoop() {
  uint32_t period, onTime;

  if (netRecovery()) {            /* secours : clignotement rapide      */
    period = 200;  onTime = 100;
  } else if (netLink() == LINK_ONLINE) {  /* en ligne : battement lent  */
    period = 3000; onTime = 80;
  } else {                        /* hors ligne / portail : moyen       */
    period = 1000; onTime = 500;
  }

  uint32_t now = millis();
  uint32_t phase = now % period;
  bool want = (phase < onTime);
  if (want != ledOn) {
    ledOn = want;
    digitalWrite(LED_PIN, ledOn ? LED_ON_LEVEL : LED_OFF_LEVEL);
  }
}

static void buttonLoop() {
  bool down = (digitalRead(BTN_PIN) == LOW);
  uint32_t now = millis();

  if (down && !btnDown) {
    btnDown = true;
    btnSince = now;
    recoveryArmed = false;
    return;
  }

  if (down && btnDown) {
    uint32_t held = now - btnSince;

    if (held >= HOLD_FACTORY_MS) {
      Serial.println(F("[BTN] Reinitialisation complete demandee."));
      cfgErase();
      delay(100);
      ESP.restart();
    } else if (held >= HOLD_RECOVERY_MS && !recoveryArmed) {
      recoveryArmed = true;
      Serial.println(F("[BTN] Passage en mode secours : point d'acces ouvert."));
      netEnterRecovery();
    }
    return;
  }

  if (!down && btnDown) btnDown = false;
}

static void banner(bool loaded) {
  Serial.println();
  Serial.println(F("=================================="));
  Serial.print(F("  ESP-Pont v"));
  Serial.println(F(FW_VERSION));
  Serial.println(F("=================================="));
  Serial.print(F("Configuration : "));
  Serial.println(loaded ? F("chargee depuis la flash") : F("valeurs par defaut"));
  Serial.print(F("Point d'acces : "));
  Serial.print(WiFi.softAPSSID());
  Serial.print(F("  ("));
  Serial.print(cfgSecurityName(cfg.ap_sec));
  Serial.println(F(")"));
  Serial.print(F("Etat du point : "));
  Serial.println(netApOk() ? F("diffuse") : F("ECHEC DE DEMARRAGE"));
  Serial.print(F("Interface web : http://"));
  Serial.println(WiFi.softAPIP());
  Serial.print(F("Protocole     : "));
  Serial.print(cfgPhyName(netActualPhy()));
  if (netActualPhy() != cfg.phy) {
    Serial.print(F("  (demande : "));
    Serial.print(cfgPhyName(cfg.phy));
    Serial.print(F(")"));
  }
  Serial.println();
#if NET_PER_IFACE_PHY
  Serial.print(F("Proto. amont  : "));
  Serial.println(cfgPhyName(netActualPhySta()));
#endif
  Serial.print(F("Canal radio   : "));
  Serial.println(WiFi.channel());
  if (cfg.sta_ssid[0]) {
    Serial.print(F("Liaison amont : "));
    Serial.println(cfg.sta_ssid);
    Serial.print(F("Authentif.    : "));
    if (cfg.sta_mode == STA_ENTERPRISE) {
      Serial.print(F("WPA2-Entreprise 802.1X, identifiant "));
      Serial.println(cfg.eap_user[0] ? cfg.eap_user : "(vide)");
    } else {
      Serial.println(F("WPA/WPA2 personnel"));
    }
  } else {
    Serial.println(F("Liaison amont : non configuree"));
  }
  Serial.print(F("Relais dslink : "));
  if (cfg.relay_off) {
    Serial.println(F("coupe"));
  } else {
    Serial.print(F("UDP "));
    Serial.println(DSLINK_UDP_PORT);
  }
  Serial.println(F("----------------------------------"));
}

/* ------------------------------------------------------------------ */

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF_LEVEL);

  bool loaded = cfgLoad();

  /* La liaison amont est volontairement oubliee a chaque demarrage : il faut
   * reselectionner le reseau depuis l'interface. Seule la copie en memoire est
   * videe, la cle et les identifiants restent enregistres, donc reprendre le
   * meme reseau ne demande pas de les ressaisir. Aucune ecriture en flash ici,
   * l'oubli devient definitif seulement au prochain enregistrement. */
  cfg.sta_ssid[0] = '\0';

  netBegin(false);
  webBegin();

  banner(loaded);
}

void loop() {
  netLoop();
  webLoop();
  relayLoop();
  buttonLoop();
  ledLoop();
  yield();
}
