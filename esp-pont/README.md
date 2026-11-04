# ESP-Pont

Passerelle Wi-Fi configurable par interface web, pour **ESP8266** (NodeMCU V3)
et pour **ESP32**. Le même code compile pour les deux puces.

L'ESP se connecte à un réseau Wi-Fi existant, puis rediffuse son propre point
d'accès dont on choisit librement le **nom**, le **type de sécurité**
(Ouvert / WPA / WPA2 / mixte), le **canal** et le **protocole 802.11 b/g/n**.
Le trafic des appareils connectés est routé vers le réseau existant par NAT. La liaison amont accepte aussi le WPA2-Entreprise 802.1X, pour les réseaux universitaires type eduroam.

C'est exactement ce qu'il faut pour une vieille console : la console parle au
pont avec ce qu'elle sait faire (802.11b, réseau ouvert), et le pont parle au
routeur de la maison en WPA2. Le pont fait la traduction de sécurité.

```
  Nintendo DS  ──(802.11b, ouvert)──>  ESP8266  ──(802.11b, WPA2)──>  Box / routeur ──> Internet
                                       le pont
```

---

## 1. Matériel

| Élément | Détail |
|---|---|
| Cartes testées | NodeMCU V3 LoLin (ESP-12E) et ESP32-WROOM-32 DevKit |
| Puces | ESP8266 et ESP32, Wi-Fi 2,4 GHz 802.11 b/g/n |
| USB | CH340 sur la NodeMCU, CP2102 sur la carte ESP32 : installer le pilote correspondant si aucun port COM n'apparaît |
| Alimentation | USB 5 V, 500 mA minimum. Une alimentation faible provoque des redémarrages aléatoires en émission |

Aucun composant externe n'est nécessaire. Le bouton **FLASH** et la LED bleue
de la carte servent d'interface de secours.

---

## 2. Installation dans l'IDE Arduino

1. **Ajouter le support ESP8266.** Fichier > Préférences > « URL de gestionnaire
   de cartes supplémentaires », ajouter :

   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```

   Puis Outils > Type de carte > Gestionnaire de cartes, chercher `esp8266` et
   installer la version **3.1.2 ou plus récente**. Les versions 2.x ne
   compileront pas ce projet.

2. **Ouvrir le projet.** Ouvrir [esp-pont.ino](esp-pont.ino). Les autres
   fichiers du dossier apparaissent automatiquement en onglets.

3. **Régler les options** dans le menu Outils :

   | Option | Valeur |
   |---|---|
   | Type de carte | **Generic ESP8266 Module** |
   | **NONOS SDK Version** | **nonos-sdk 3.0.5 (experimental)** |
   | **lwIP Variant** | **v2 Higher Bandwidth** |
   | Builtin Led | 2 |
   | CPU Frequency | 160 MHz |
   | Crystal Frequency | 26 MHz |
   | Flash Size | 4MB (FS:2MB OTA:~1019KB) |
   | Flash Mode | DIO |
   | Flash Frequency | 40MHz |
   | Reset Method | dtr (aka nodemcu) |
   | Upload Speed | 115200 |
   | Port | le port COM de la carte |

   Deux réglages sont indispensables, les autres décrivent simplement votre
   carte.

   **lwIP Variant** conditionne le routage NAT, qui n'existe que dans les
   variantes compilées « avec features ». Une variante `(no features)` ou
   `IPv6` arrête la compilation avec un message explicite.

   **NONOS SDK Version** conditionne le WPA2-Entreprise. Les versions 2.2.x
   n'authentifient pas correctement sur eduroam, elles transmettent une
   identité figée. C'est aussi la raison du choix de la carte générique :
   l'entrée « NodeMCU 1.0 » n'expose pas ce menu et fige le SDK en version
   2.2.1. Si vous n'avez pas besoin d'eduroam, la carte NodeMCU 1.0 convient
   parfaitement avec le seul réglage lwIP.

4. **Téléverser.** Aucune manipulation de bouton n'est nécessaire, la NodeMCU
   gère seule la mise en mode programmation.

### Variante ESP32

Le même croquis compile pour l'ESP32, sans aucune modification. Ajouter cette
URL dans les préférences, à côté de celle de l'ESP8266 :

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

Installer le paquet `esp32` en **version 3.x** (les versions 2.x ne compileront
pas ce projet, qui utilise les objets `WiFi.AP` et `WiFi.STA`), puis choisir **ESP32 Dev Module** comme type de
carte. Les valeurs par défaut conviennent, il n'y a ni menu lwIP ni menu SDK :
le routage NAT et le WPA2-Entreprise sont intégrés au cœur de l'ESP32. Prévoir
le pilote **CP2102** de Silicon Labs si aucun port COM n'apparaît, c'est la
puce USB de ces cartes.

Avantage décisif de l'ESP32 : l'onglet Radio affiche **deux** sélecteurs de
protocole au lieu d'un, car la puce accepte un réglage par interface. Le point
d'accès peut donc rester en 802.11b pour une Nintendo DS pendant que la
liaison amont tourne en 802.11n sur votre box. L'ESP8266 n'a qu'un réglage
commun aux deux interfaces et impose de choisir.

---

## 3. Première mise en service

1. Alimenter la carte. Au bout de deux secondes, un réseau Wi-Fi ouvert nommé
   **`ESP-Pont-Setup`** apparaît.
2. S'y connecter avec un téléphone ou un PC. La page de configuration s'ouvre
   d'elle-même (portail captif). Sinon, aller sur **http://192.168.4.1**.
3. Onglet **Liaison amont** : au premier démarrage, la liste des réseaux à
   portée s'affiche d'elle-même. Cliquer sur votre box, puis saisir sa clé.
4. Onglet **Point d'accès** : choisir le nom, la sécurité et le canal du réseau
   que verront vos appareils.
5. Onglet **Radio** : le 802.11b est déjà actif, à conserver pour une DS.
6. Cliquer sur **Enregistrer et appliquer**.

La radio redémarre. Si vous avez changé le nom ou la sécurité du point d'accès,
reconnectez-vous au nouveau réseau, puis rechargez `http://192.168.4.1`.

Une fois la liaison montante établie, l'interface est aussi joignable depuis
votre réseau habituel, à l'adresse IP affichée sur la page d'état, ou via
`http://esp-pont.local` si votre système gère le mDNS.

**Le réseau amont est oublié à chaque démarrage.** Débrancher puis rebrancher la
carte la ramène systématiquement sur son point d'accès de configuration, sans
liaison montante. Il faut reprendre l'analyse et resélectionner le réseau. Les
clés et les identifiants, eux, restent enregistrés : reprendre le même réseau ne
demande pas de les ressaisir, un clic puis Enregistrer suffisent. Tous les autres
réglages, nom du point d'accès, chiffrement, protocole radio, sont conservés.

---

## 4. L'interface web

**Liaison amont.** Le réseau existant auquel l'ESP se raccorde. Les réseaux à
portée sont listés avec leur niveau de signal, leur chiffrement et leur canal ; il suffit de cliquer sur celui voulu. Le type d'authentification est déduit du balayage et les champs à renseigner s'adaptent seuls : rien pour un réseau ouvert, une clé pour un réseau à clé partagée, un identifiant et un mot de passe pour un réseau 802.1X. Ensuite, au choix, DHCP ou adresse IP fixe.

**Point d'accès.** Le réseau diffusé par l'ESP :

- *Sécurité* : Ouvert, WPA-PSK (TKIP), WPA2-PSK (AES), ou WPA/WPA2 mixte.
- *Canal* : 1 à 13, utilisé uniquement tant que la liaison montante est absente.
- *Clients maximum* : 1 à 8.
- *SSID masqué* et *sous-réseau IP* du pont.

**Radio.** Protocole 802.11b, 802.11g ou 802.11n, et puissance d'émission
réglable de 0 à 20,5 dBm. Le **802.11b est actif par défaut sur le point
d'accès**, pour que les consoles anciennes voient le réseau sans aucun réglage.

Sur **ESP32**, un second sélecteur règle la liaison amont indépendamment : point
d'accès en b pour la console, amont en n pour le débit. Sur **ESP8266**, le
réglage est unique et vaut pour les deux interfaces, le b plafonne alors tout
le trafic à 11 Mbit/s. L'interface n'affiche le second sélecteur que si la puce
en est capable.

**Système.** Activation du routage NAT, serveur DNS imposé aux clients, relais
dslink, nom d'hôte mDNS, mot de passe de l'interface web, liste des appareils
connectés, redémarrage et réinitialisation.

**Relais dslink.** Actif par défaut, il sert le mode réseau de ds-pc-link
(liaison DS ↔ PC, voir le [README principal](../README.md)). L'ESP écoute le
port UDP **47474**. Les DS connectées au point d'accès lui envoient leurs
messages, puisqu'il est leur passerelle. Le PC s'inscrit en lui envoyant un
message vide chaque seconde (`dslink.py demo --relay esp-pont.local`). L'ESP
transmet alors les messages des DS au PC, et ceux du PC à toutes les DS. La
page d'état indique le PC inscrit et le nombre de DS relayées. Un PC qui se tait
plus de 10 s est oublié, une DS au bout de 30 s. Le relais ne vérifie pas qui
s'inscrit : n'importe quel appareil du réseau amont peut se déclarer PC.

Les champs de mot de passe restent vides à l'affichage : les laisser vides
conserve la valeur enregistrée. Pour supprimer le mot de passe de l'interface
web, saisir un simple espace.

---

## 5. eduroam et les réseaux WPA2-Entreprise

Un réseau universitaire comme eduroam n'utilise pas de clé partagée mais une
authentification personnelle 802.1X, avec identifiant et mot de passe. Le pont
sait s'y raccorder côté liaison amont.

Dans l'onglet **Liaison amont**, sélectionner eduroam dans la liste. Le pont le
reconnaît et affiche directement les champs 802.1X.

| Champ | Contenu | Exemple |
|---|---|---|
| Identité anonyme | identité extérieure, transmise en clair, facultative | `anonymous@univ-smb.fr` |
| Identifiant | le compte complet, avec son domaine | `prenom.nom@univ-smb.fr` |
| Mot de passe | le mot de passe de ce compte | |

La méthode EAP, PEAP ou TTLS, est négociée automatiquement avec le serveur, il
n'y a rien à choisir. L'association prend quelques secondes de plus qu'un réseau
à clé partagée, jusqu'à six.

Le point d'accès diffusé par le pont, lui, reste en Ouvert ou en WPA2 personnel.
Le 802.1X ne concerne que la liaison vers le réseau de l'établissement.

Quatre points à connaître avant de s'en servir.

**Le certificat du serveur n'est pas vérifié.** L'ESP8266 n'ayant pas d'horloge,
il jugerait tout certificat hors de sa période de validité ; la vérification est
donc désactivée. Face à un faux point d'accès diffusant le même nom, vos
identifiants seraient exposés. Ne laissez pas le pont allumé sans surveillance
dans un lieu public.

**Le SDK 3.0.5 est obligatoire**, voir les réglages de compilation en section 2.
Les versions 2.2.x transmettent une identité figée et l'authentification échoue.

**La détection repose sur le nom du réseau.** Le balayage de l'ESP ne distingue
pas un réseau d'entreprise d'un réseau personnel, le SDK annonce « WPA2 » dans
les deux cas. Le pont reconnaît donc eduroam à son SSID, normalisé mondialement.
Pour tout autre réseau 802.1X, un lien sous le champ de clé bascule en saisie d'identifiant, et un lien symétrique fait l'inverse.

**Le conflit avec la Nintendo DS ne concerne que l'ESP8266.** Les réseaux de
campus désactivent souvent les débits 802.11b, que la DS est seule à
comprendre. Sur ESP8266, la radio n'a qu'un réglage pour ses deux interfaces,
il faut donc choisir entre la console et le campus. **Sur ESP32, le problème
disparaît** : chaque interface a son protocole, le point d'accès reste en b
pendant que la liaison amont s'associe en g ou en n.

---

## 6. Le cas de la Nintendo DS

Ce qu'acceptent les consoles :

| Console | Protocole | Sécurité acceptée |
|---|---|---|
| DS et DS Lite | 802.11b uniquement | Ouvert ou WEP |
| DSi, DSi XL, 3DS, en mode jeu DS | 802.11b uniquement | Ouvert ou WEP |
| DSi, DSi XL, 3DS, fonctions système | 802.11b/g | Ouvert, WEP, WPA, WPA2 |

**L'ESP8266 ne sait pas créer un point d'accès WEP.** Le SDK d'Espressif refuse
ce mode d'authentification en point d'accès, ce n'est pas un choix de ce projet
et aucun réglage ne le contourne. Le WEP n'est donc pas proposé dans
l'interface.

Conséquence pratique, réglage conseillé pour lancer un jeu DS :

- Sécurité du point d'accès : **Ouvert**
- Protocole : **802.11b seul**, c'est la valeur par défaut
- Canal : 1, 6 ou 11 quand la liaison montante est absente

Pour une DSi ou une 3DS utilisée en mode système, **WPA2-PSK (AES)** fonctionne
et reste préférable.

Dans les deux cas, la liaison entre l'ESP et votre box reste en WPA2 : votre
réseau principal n'est jamais exposé en clair.

---

## 7. Bouton FLASH et LED

| Action | Effet |
|---|---|
| Appui maintenu 3 s | Ouvre le point d'accès de secours `ESP-Pont-Secours`, sans mot de passe, sur 192.168.4.1 |
| Appui maintenu 10 s | Efface toute la configuration et redémarre |

Le mode secours coupe la liaison montante et rouvre l'interface, ce qui permet
de récupérer la carte après une mauvaise configuration ou un mot de passe de
point d'accès oublié. Il force aussi le **802.11g**, afin que le réseau de
secours reste visible même si le protocole configuré rendait le point d'accès
invisible pour votre appareil. Un simple redémarrage restaure la configuration
normale.

**Ne maintenez jamais FLASH pendant la mise sous tension** : l'ESP8266
démarrerait en mode programmation au lieu de démarrer le firmware. Appuyez sur
le bouton une fois la carte démarrée.

LED bleue :

| Rythme | État |
|---|---|
| Clignotement rapide | Mode secours |
| Clignotement lent régulier | Liaison montante absente, portail captif actif |
| Bref éclair toutes les 3 s | Connecté, pont opérationnel |

---

## 8. Limites à connaître

Ces limites viennent du matériel, pas du code.

**Un seul SSID à la fois.** L'ESP8266 n'a qu'une radio et une seule interface
de point d'accès. Il ne peut pas diffuser en même temps un réseau de
configuration et un réseau pour vos appareils. La page de configuration est
donc servie **sur le point d'accès lui-même**, et reste accessible en
permanence depuis n'importe quel appareil qui y est connecté, ainsi que depuis
votre réseau habituel une fois la liaison montante établie. Le bouton FLASH
fournit le filet de sécurité si ce point d'accès devient inaccessible.

**Un seul canal.** Le point d'accès et la liaison montante partagent la même
radio, donc le même canal. Dès que l'ESP se connecte à votre box, son point
d'accès bascule sur le canal de la box, ce qui déconnecte brièvement les
appareils connectés au pont. Le canal choisi dans l'interface ne s'applique
donc que tant que la liaison montante est absente.

**Débit modeste.** Le routage NAT sur ESP8266 plafonne autour de 2 à 6 Mbit/s,
et nettement moins en 802.11b. C'est largement suffisant pour une console, pas
pour de la vidéo.

**NAT sortant seulement.** Les appareils du pont joignent le réseau et
Internet, mais rien ne peut les joindre depuis l'extérieur. Il n'y a pas de
redirection de port.

**Sécurité.** Un point d'accès ouvert routé vers votre réseau domestique est
une porte ouverte sur ce réseau pour qui se trouve à portée. Si vous utilisez
le mode Ouvert pour une console, pensez à masquer le SSID, à définir un mot de
passe sur l'interface web, et à couper le pont quand vous ne jouez pas.

**Portail captif.** Il ne tourne que tant que la liaison montante est absente.
C'est volontaire : un portail captif détourne toutes les requêtes DNS, il
empêcherait donc les appareils du pont de résoudre le moindre nom de domaine.

---

## 9. Dépannage

**La compilation s'arrête sur le message `lwIP Variant`.** Outils > lwIP
Variant > « v2 Higher Bandwidth ». Ni `(no features)`, ni `IPv6`.

**Le réseau `ESP-Pont-Setup` n'apparaît pas.** Ouvrir le moniteur série à
115200 bauds et regarder la bannière de démarrage. Si rien ne s'affiche,
vérifier le pilote CH340 et le câble USB, certains câbles ne transportent que
l'alimentation.

**La console ne voit pas le réseau.** Passer le protocole en 802.11b seul dans
l'onglet Radio, et désactiver le SSID masqué.

**La console se connecte mais n'a pas Internet.** Vérifier que le NAT est
activé dans l'onglet Système, que la liaison montante est bien connectée
(pastille verte en haut de page), et que le sous-réseau du pont diffère de
celui de la box. Si votre box est en 192.168.4.x, changez l'adresse IP du pont
en 192.168.9.1.

**La liaison montante ne s'établit plus après un changement de protocole.**
Certains routeurs refusent une association en 802.11b. Revenir en 802.11g ou
802.11n, ou passer le point d'accès en 802.11b seulement au moment de jouer.

**Plus aucun accès à l'interface.** Appui de 3 s sur FLASH pour le point
d'accès de secours, ou 10 s pour repartir de zéro.

---

## 10. Organisation du code

| Fichier | Rôle |
|---|---|
| [esp-pont.ino](esp-pont.ino) | Démarrage, boucle principale, bouton FLASH, LED d'état |
| [config.h](config.h) / [config.cpp](config.cpp) | Structure de configuration, lecture et écriture en flash avec contrôle CRC |
| [net.h](net.h) | Interface commune de la couche radio |
| [net_esp8266.cpp](net_esp8266.cpp) / [net_esp32.cpp](net_esp32.cpp) | Couche radio propre à chaque puce : point d'accès, liaison amont, protocole, NAT, portail captif |
| [webui.h](webui.h) / [webui.cpp](webui.cpp) | Serveur HTTP, API JSON, validation des réglages |
| [page.h](page.h) | Interface web complète, stockée en flash |
| [dslink_relay.h](dslink_relay.h) / [dslink_relay.cpp](dslink_relay.cpp) | Relais UDP dslink entre les DS du point d'accès et le PC inscrit |

`dslink_relay.cpp` recopie l'en-tête `LinkHeader` de `../common/linkproto.h`,
car l'IDE Arduino ne compile que les fichiers du dossier du croquis : modifiez
les deux ensemble.

La configuration occupe environ 320 octets au début de la zone EEPROM émulée,
protégés par un CRC32 : une zone vierge ou corrompue fait repartir la carte sur
les valeurs par défaut au lieu de la bloquer.
