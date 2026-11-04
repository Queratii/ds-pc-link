/*
 * net.h - gestion radio : point d'acces, liaison montante, NAT, portail captif.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

enum LinkState : uint8_t {
  LINK_IDLE = 0,      /* aucune liaison montante configuree */
  LINK_CONNECTING,    /* tentative de connexion en cours    */
  LINK_ONLINE         /* connecte au reseau existant        */
};

/* Initialise la radio et applique la configuration courante.
 * recovery = true : point d'acces de secours ouvert, liaison montante coupee. */
void netBegin(bool recovery);

/* A appeler en permanence depuis loop(). */
void netLoop();

/* Reapplique cfg a la radio (apres enregistrement depuis l'interface web). */
void netApply();

/* Bascule a chaud en mode secours (point d'acces ouvert, sans mot de passe). */
void netEnterRecovery();

bool        netRecovery();

/* true si le point d'acces a bien demarre. */
bool        netApOk();

/* Mode PHY reellement applique au point d'acces (valeurs de PhyChoice). */
uint8_t     netActualPhy();

/* Mode PHY reellement applique a la liaison montante. Sur ESP8266, la radio
 * n'a qu'un reglage : cette fonction renvoie alors la meme valeur. */
uint8_t     netActualPhySta();

LinkState   netLink();
bool        netPortalActive();
bool        netNaptActive();
const char *netLinkName();

/* Adresse IP proposee comme serveur DNS aux clients du point d'acces. */
IPAddress   netClientDns();

/* Relance une tentative de connexion immediate a la liaison montante. */
void netRetryUplink();
