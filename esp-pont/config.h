/*
 * config.h - structure de configuration persistante (EEPROM emulee en flash)
 *
 * Projet ESP-Pont : passerelle Wi-Fi / Wi-Fi pour NodeMCU V3 (ESP8266).
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

/* ------------------------------------------------------------------ */
/* Versionnage de la structure stockee en flash                        */
/* ------------------------------------------------------------------ */
#define CFG_MAGIC    0x314E4F50UL   /* "PON1" */
#define CFG_VERSION  3
#define CFG_EEPROM_SIZE 1024

/* Le projet cible deux puces. L'ESP8266 n'a qu'un reglage de protocole radio
 * pour toute la puce ; l'ESP32 en accepte un par interface, ce qui permet un
 * point d'acces en 802.11b pour une console et une liaison montante en g ou n.
 * NET_PER_IFACE_PHY vaut 1 quand ce reglage separe est disponible. */
#if defined(ARDUINO_ARCH_ESP32)
  #define NET_PER_IFACE_PHY 1
#else
  #define NET_PER_IFACE_PHY 0
#endif

/* Mode d'authentification de la liaison montante.
 *
 * STA_ENTERPRISE couvre le WPA2-Entreprise (802.1X), celui des reseaux
 * universitaires type eduroam : pas de cle partagee, mais un identifiant
 * et un mot de passe personnels, avec une identite anonyme facultative.
 * Le SDK negocie seul la methode EAP (PEAP ou TTLS) avec le serveur. */
enum StaSecurity : uint8_t {
  STA_PERSONAL   = 0,   /* WPA / WPA2-PSK, cle partagee */
  STA_ENTERPRISE = 1    /* WPA2-Entreprise, identifiant + mot de passe */
};

/* Type de securite du point d'acces cree par l'ESP.
 *
 * ATTENTION : le WEP n'existe pas ici. Le SDK Espressif refuse AUTH_WEP
 * en mode point d'acces ; seuls Ouvert / WPA / WPA2 sont possibles.
 * Pour une DS/DS Lite en mode jeu DS (qui ne connait que WEP et Ouvert),
 * il faut donc utiliser SEC_OPEN.
 */
enum ApSecurity : uint8_t {
  SEC_OPEN     = 0,   /* aucun chiffrement            */
  SEC_WPA      = 1,   /* WPA-PSK (TKIP)               */
  SEC_WPA2     = 2,   /* WPA2-PSK (AES/CCMP)          */
  SEC_WPA_WPA2 = 3    /* mode mixte WPA/WPA2          */
};

/* Mode PHY radio. Les valeurs correspondent a WiFiPhyMode_t du core. */
enum PhyChoice : uint8_t {
  PHY_11B = 1,
  PHY_11G = 2,
  PHY_11N = 3
};

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;

  /* --- liaison montante (mode station : le reseau existant) --- */
  char     sta_ssid[33];
  char     sta_pass[65];
  uint8_t  sta_dhcp;          /* 1 = DHCP, 0 = IP fixe */
  uint8_t  pad1[3];
  uint32_t sta_ip;
  uint32_t sta_gw;
  uint32_t sta_mask;
  uint32_t sta_dns;

  /* --- point d'acces diffuse par l'ESP (la DS s'y connecte) --- */
  char     ap_ssid[33];
  char     ap_pass[65];
  uint8_t  ap_sec;            /* ApSecurity */
  uint8_t  ap_channel;        /* 1..13, ignore si la liaison montante est active */
  uint8_t  ap_hidden;         /* 1 = SSID masque */
  uint8_t  ap_max;            /* 1..8 clients */
  uint32_t ap_ip;
  uint32_t ap_mask;

  /* --- radio --- */
  uint8_t  phy;               /* PhyChoice : protocole du point d'acces.
                               * Sur ESP8266 il s'applique a toute la puce. */
  uint8_t  tx_qdbm;           /* puissance en quarts de dBm : 0..82 */
  uint8_t  nat_on;            /* 1 = routage NAT actif (partage de connexion) */
  uint8_t  dns_custom_on;     /* 1 = imposer un DNS aux clients du point d'acces */
  uint32_t dns_custom;

  /* --- administration --- */
  char     admin_user[17];
  char     admin_pass[33];    /* vide = interface web sans mot de passe */
  char     hostname[25];

  /* --- WPA2-Entreprise sur la liaison montante (eduroam) --- */
  uint8_t  sta_mode;          /* StaSecurity */
  uint8_t  phy_sta;           /* PhyChoice : protocole de la liaison montante.
                               * Ignore sur ESP8266 (voir NET_PER_IFACE_PHY). */
  uint8_t  relay_off;         /* 0 = relais dslink actif (defaut), 1 = coupe.
                               * Logique inversee pour que les configurations
                               * deja enregistrees, ou cet octet valait 0,
                               * activent le relais sans changer de version. */
  uint8_t  pad2;
  char     eap_anon[65];      /* identite anonyme, ex. anonymous@univ-smb.fr */
  char     eap_user[65];      /* identifiant complet, ex. prenom.nom@univ-smb.fr */
  char     eap_pass[65];

  uint32_t crc;
};

extern Config cfg;

/* Charge la config depuis la flash. Retourne false et applique les
 * valeurs par defaut si la zone est vide ou corrompue. */
bool cfgLoad();

/* Ecrit la config courante en flash (recalcule le CRC). */
bool cfgSave();

/* Remet toutes les valeurs par defaut en memoire (sans ecrire en flash). */
void cfgDefaults();

/* Efface la zone de configuration : le prochain demarrage repart a zero. */
void cfgErase();

/* Libelles lisibles */
const char *cfgSecurityName(uint8_t sec);
const char *cfgPhyName(uint8_t phy);
