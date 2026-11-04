/*
 * dslink_relay.h - relais UDP pour le mode reseau de ds-pc-link.
 *
 * Les DS connectees au point d'acces envoient leurs messages dslink a leur
 * passerelle, c'est-a-dire a l'ESP, sur le port DSLINK_UDP_PORT. Le PC
 * s'inscrit en envoyant regulierement un message vide (noeud 0) a l'ESP,
 * via son adresse sur le reseau amont ou esp-pont.local. L'ESP relaie
 * ensuite dans les deux sens :
 *
 *   DS (noeud 1..15)  --UDP-->  ESP  --UDP-->  PC inscrit
 *   PC (noeud 0)      --UDP-->  ESP  --UDP-->  toutes les DS connues
 *
 * Les DS n'ont ainsi pas besoin de connaitre l'adresse du PC, qui se trouve
 * de l'autre cote du NAT.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#define DSLINK_UDP_PORT 47474   /* = LINK_UDP_PORT de common/linkproto.h */

/* A appeler en permanence depuis loop(). Demarre ou arrete le relais selon
 * cfg.relay_off, et traite les datagrammes en attente. */
void relayLoop();

bool      relayActive();
bool      relayPcKnown();
IPAddress relayPcIp();
uint16_t  relayPcPort();
uint8_t   relayDsCount();
