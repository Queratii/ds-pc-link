/*
 * webui.h - serveur HTTP de configuration + portail captif.
 */
#pragma once

#include <Arduino.h>

void webBegin();
void webLoop();

/* true si un redemarrage a ete demande par l'interface web. */
bool webRebootPending();
