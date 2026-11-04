# ds-pc-link — Liaison sans fil directe Nintendo DS ↔ PC via ESP32

```
 Nintendo DS  ~~~ 802.11b brut (1-2 Mbit/s) ~~~  ESP32  ==== USB série 921600 ====  PC
 (homebrew ou                                   (pont radio)    COBS + CRC16      (dslink.py /
  cartouche)                                                                       votre port PC)
```

Le kit couvre deux situations très différentes :

| | **Homebrews** (vous contrôlez le code DS) | **Cartouches d'origine** |
|---|---|---|
| Protocole radio | le nôtre, simple, en broadcast | celui de Nintendo, imposé |
| Contrainte de timing | aucune (pas d'ACK) | réponses en quelques centaines de µs |
| État du kit | **fonctionnel de bout en bout** | **outils de phase 1** : capture, analyse, rejeu de beacon |
| Difficulté | quelques soirées | projet de recherche |
| Émulateur melonDS | oui, en **mode réseau** (UDP via melonAP, sans ESP32) | non |

---

## Contenu

```
common/linkproto.h        format des trames air + protocole série (partagé C)
common/cobs.h             encodage COBS
esp32-bridge/             firmware ESP-IDF 5.x (pont + capture)
ds-homebrew/              bibliothèque dslink.c/.h + démo (BlocksDS)
pc-host/dslink.py         client PC : démo, capture pcap, analyse, rejeu de beacon
tests/                    tests hors matériel (C <-> Python, pcap, analyseur)
esp-pont/                 ESP-Pont : point d'accès ouvert 802.11b routé + relais dslink (Arduino)
homebrews/VoidCrew.nds    jeu homebrew Wi-Fi (BlocksDS), prêt à lancer sur la DS
```

Le kit propose deux ESP différents : `esp32-bridge/` pour la **radio directe** (trames brutes,
relié au PC en USB), et `esp-pont/` pour le **mode réseau** (point d'accès + relais dslink, relié
au PC par le réseau de la box). ESP-Pont a sa propre documentation,
[esp-pont/README.md](esp-pont/README.md). Le nom du dossier doit rester identique à celui du
croquis `esp-pont.ino` pour que l'IDE Arduino l'ouvre.

## Matériel

- Un ESP32 classique, S3 ou C3 avec un port **UART** (pont USB-série CP210x/CH340). Les cartes S3/C3
  qui n'ont que l'USB natif marchent aussi, mais il faut remplacer `driver/uart` par `usb_serial_jtag`
  dans `main.c`.
- Pour les homebrews : un linker (carte flash, TWiLight Menu++, etc.) pour lancer le `.nds` sur la DS.
- Python 3.9+ et `pip install pyserial`.
- Pour ESP-Pont (facultatif) : un ESP8266 (NodeMCU V3) ou un ESP32, et l'IDE Arduino.

## Compilation

**ESP32**
```bash
cd esp32-bridge
idf.py set-target esp32        # ou esp32s3 / esp32c3
idf.py build flash             # (pas de "monitor" : le port parle en binaire)
```
Avec ESP-IDF < 5.3, remplacez `esp_driver_uart` par `driver` dans `main/CMakeLists.txt`.
À partir d'ESP-IDF 5.4, si `esp_wifi_config_80211_tx_rate` est signalée comme obsolète,
remplacez-la par `esp_wifi_config_80211_tx` (un seul appel, dans `radio_tx`).

**DS (BlocksDS)**
```bash
cd ds-homebrew
make                           # -> dslink_demo.nds
```

**Tests sans matériel**
```bash
python tests/test_all.py       # nécessite gcc
```

---

## Partie 1 — Homebrews

### Comment ça marche

1. La DS passe sa radio en mode *promiscuous* sur un canal fixe (7 par défaut) via DSWiFi
   (`Wifi_EnableWifi`, `Wifi_SetPromiscuousMode`, `Wifi_SetChannel`).
2. Chaque message est une **trame de données 802.11 en broadcast** avec un BSSID privé
   `02:44:53:4C:4E:4B` ("DSLNK") et un en-tête `LinkHeader` (magic, nœud, séquence, longueur).
   Le broadcast n'attend pas d'ACK : aucune contrainte de timing.
3. L'ESP32 capte ces trames, les filtre et les remonte au PC ; dans l'autre sens il émet ce que le
   PC lui demande, en 802.11b à 2 Mbit/s.
4. Les numéros de séquence permettent de compter les pertes. Le broadcast n'étant pas acquitté,
   **votre jeu doit tolérer des pertes** : envoyez l'état complet régulièrement plutôt que des
   deltas, ou ajoutez vos propres acquittements pour les événements importants.

### Lancer la démo

```bash
python pc-host/dslink.py ports                   # trouver le port de l'ESP32
python pc-host/dslink.py demo --port COM5        # ou /dev/ttyUSB0
```
Puis lancez `dslink_demo.nds` sur la DS et choisissez **A** (radio directe). Sans ESP32, ou sous
l'émulateur melonDS, voir plus bas le [mode réseau](#mode-réseau-udp--émulateur-melonds-ou-vraie-ds-sans-pont-radio).
Le PC affiche les touches pressées ; **A** mesure
l'aller-retour (RTT) ; ce que vous tapez dans le terminal s'affiche sur la DS.
Plusieurs DS : changez `NODE` (1 à 15) dans `ds-homebrew/source/main.c` pour chacune.

### Intégrer dans vos jeux

**Côté DS** — copiez `dslink.c`, `dslink.h` et `common/linkproto.h` :
```c
dslink_init(7, MON_NUMERO_DE_JOUEUR);
// à chaque image :
Wifi_Update();
dslink_send(&mon_etat, sizeof mon_etat);
DsLinkMsg m;
while (dslink_poll(&m)) traiter(m.node, m.data, m.len);
```

**Côté port PC** :
```python
from dslink import DsLinkBridge, SER_EVT_RX_LINK
br = DsLinkBridge("COM5")
br.set_channel(7)
br.send_link(b"...")                 # vers toutes les DS
ev = br.events.get()                 # ev["node"], ev["data"], ev["rssi"]
```
Si votre port PC n'est pas en Python, réimplémentez les ~60 lignes du protocole série
(COBS + CRC16-CCITT, voir `common/linkproto.h`) : c'est volontairement minimal.

### Mode réseau (UDP) : émulateur melonDS, ou vraie DS sans pont radio

`dslink` a un second transport, choisi au démarrage de la démo (**B**). La DS se connecte au
point d'accès enregistré dans ses réglages Wi-Fi (WFC), puis échange avec le PC des datagrammes
UDP (port `47474`) qui contiennent le même `LinkHeader` que les trames radio. L'API est la même ;
seul l'appel d'initialisation change :
```c
dslink_init_udp(NULL, LINK_UDP_PORT, MON_NUMERO_DE_JOUEUR);   // au lieu de dslink_init(7, ...)
```
```python
from dslink import DsLinkUdp
br = DsLinkUdp()                     # même interface que DsLinkBridge
```
Le PC n'a rien à configurer : il découvre chaque DS à son premier message et lui répond à
l'adresse vue, ce qui traverse les NAT. Côté DS, sans adresse explicite, les messages partent vers
la passerelle et vers le broadcast du sous-réseau jusqu'à la première réponse du PC.

C'est **le seul mode qui fonctionne sous émulateur** : melonDS ne relaie pas les trames 802.11
brutes vers le PC (il les échange seulement entre plusieurs instances de melonDS).

#### Sous melonDS

1. Utilisez le **firmware intégré** de melonDS (réglage par défaut, sans fichier `firmware.bin`).
   Son profil Wi-Fi pointe déjà vers le point d'accès émulé **melonAP**. Avec un firmware de
   vraie console, il faut d'abord enregistrer melonAP depuis les réglages Wi-Fi (WFC) d'un jeu.
2. Réglages Wi-Fi de melonDS : le mode **indirect** marche sans installation. Le mode **direct**
   (libpcap) place la DS émulée sur votre réseau local.
3. Lancez le PC, puis la démo :
   ```bash
   python pc-host/dslink.py demo --udp
   ```
   Ouvrez `dslink_demo.nds` dans melonDS et appuyez sur **B**. La ligne `PC connecte` s'affiche
   dès la première réponse.
4. Si elle reste sur `en attente...`, la recherche automatique n'atteint pas le PC dans votre
   configuration. Mettez l'IP locale du PC dans `PC_IP` (`ds-homebrew/source/main.c`), recompilez,
   et autorisez Python dans le pare-feu de Windows (UDP entrant, port 47474).

Plusieurs instances de melonDS : donnez à chacune une démo compilée avec un `NODE` différent.

#### Sur une vraie DS, avec ESP-Pont (relais dslink)

```
 DS ~~(802.11b, ouvert)~~ ESP-Pont ~~(WPA2)~~ box ---- PC
      UDP 47474 -->  relais dslink  -->  UDP 47474
```

Une vraie DS ne connaît que les réseaux **ouverts ou WEP** en 802.11b. **ESP-Pont** (`esp-pont/`)
lui en fournit un : un ESP8266 ou un ESP32 diffuse ce réseau ouvert et le route en NAT vers votre
box en WPA2. Il intègre aussi un **relais dslink**, actif par défaut. La DS envoie ses messages à
sa passerelle, c'est-à-dire à ESP-Pont, qui les transmet au PC inscrit, et inversement. Ni la DS
ni le PC n'ont d'adresse à configurer : `PC_IP` reste vide.

1. Flashez et configurez ESP-Pont : [esp-pont/README.md](esp-pont/README.md), sections 2, 3 et 6
   (point d'accès **Ouvert**, protocole **802.11b**).
2. Enregistrez le réseau d'ESP-Pont dans les réglages Wi-Fi de la DS (depuis un jeu compatible WFC).
3. Sur le PC, branché au même réseau que la box :
   ```bash
   python pc-host/dslink.py demo --relay esp-pont.local    # ou l'IP amont d'ESP-Pont
   ```
4. Lancez `dslink_demo.nds` et appuyez sur **B**. La page d'état d'ESP-Pont affiche le PC inscrit
   et le nombre de DS relayées.

Dans vos jeux : `DsLinkUdp(relay="esp-pont.local")` côté PC, `dslink_init_udp(NULL, ...)` côté DS.

Par rapport à la radio directe, ce mode ajoute quelques secondes de connexion (association, DHCP)
et un peu de latence, mais n'impose aucune contrainte de canal, et il n'utilise que des API Wi-Fi
standard de la DS.

### Jeu fourni : Void Crew

`homebrews/VoidCrew.nds` est un jeu homebrew compilé avec BlocksDS, qui propose des salons
multijoueurs Wi-Fi (« Créer un salon Wi-Fi », « Créer un salon Wi-Fi local », analyse des canaux).
Copiez-le sur la carte de votre linker et lancez-le comme `dslink_demo.nds`. Il est fourni en
binaire seul : ses sources ne font pas partie de ce dépôt. Il n'utilise pas `dslink`. Sous melonDS,
ses salons Wi-Fi dépendent donc de l'émulation Wi-Fi de melonDS (plusieurs instances pour le jeu
local, melonAP pour le réseau), pas de ce kit.

### À vérifier sur le matériel

Ces points dépendent de votre version de BlocksDS / ESP-IDF et n'ont pas pu être testés ici :
- que la DS reçoit bien les broadcasts dans ce mode (sinon : voir la démo NiFi `dsnifi`) ;
- le code de retour exact de `Wifi_RawTxFrame` (la doc est ambiguë : 0 = succès) ;
- le taux de pertes réel à 2 Mbit/s ; repassez à 1 Mbit/s (`0x000A` / `RATE_1M`) si besoin ;
- en mode réseau : que `ioctl(FIONBIO)` et `SO_BROADCAST` sont acceptés par votre version de
  DSWiFi, et que la passerelle de melonDS en mode indirect mène bien au PC (sinon, `PC_IP`).

---

## Partie 2 — Cartouches d'origine

Ici, c'est la cartouche qui impose le protocole multijoueur de Nintendo. D'après GBATEK :

- l'**hôte** émet des **beacons** avec un élément propriétaire (OUI `00:09:BF`) qui décrit la partie ;
- il envoie régulièrement une trame **MP « CMD »** à `03:09:BF:00:00:00` ;
- chaque **client** doit répondre à `03:09:BF:00:00:10` dans un créneau très court ;
- l'hôte acquitte avec une trame à `03:09:BF:00:00:03`.

Le PC, derrière une liaison USB, est trop lent pour tenir ces créneaux. La couche MP devra donc
tourner **sur l'ESP32** ; le PC ne fournit que les données de jeu, en avance. C'est faisable mais
incertain, d'où la feuille de route par phases.

### Phase 1 — Observer (outils fournis)

Faites jouer **deux vraies DS** ensemble, et capturez :
```bash
python pc-host/dslink.py sniff --port COM5 --channel 7 --nintendo -o partie.pcap
```
Le canal dépend du jeu : capturez d'abord sans `--nintendo` sur les canaux 1, 7 et 13 pour le
trouver. À l'arrêt, l'analyse s'affiche (et `python pc-host/dslink.py analyze partie.pcap` la
relance) :
- hôtes détectés et contenu de leur élément Nintendo ;
- **période des trames MP** et **délai CMD → réponse client** : ce sont les chiffres qui disent si
  l'ESP32 pourra suivre ;
- émetteurs et répartition des trames.

Le fichier `.pcap` s'ouvre dans **Wireshark** (filtre utile : `wlan.addr[0:3] == 03:09:bf`).

### Phase 2 — Se faire voir (outil fourni)

Rejouez un beacon capturé, la DS hôte d'origine éteinte :
```bash
python pc-host/dslink.py replay-beacon --port COM5 --channel 7 partie.pcap
```
Si la DS cliente liste la partie et tente de rejoindre, l'outil affiche ses trames de réaction
(authentification, association). **C'est le premier vrai jalon** : il prouve que la DS accepte
l'ESP32 comme hôte au niveau radio.

### Phase 3 — Accepter un client (à écrire)

Répondre à l'authentification et à l'association de la DS, en s'appuyant sur les échanges capturés
entre deux vraies DS. Pas de contrainte de timing serrée à ce stade.

### Phase 4 — Boucle MP sur l'ESP32 (à écrire, le cœur du problème)

Implémenter sur l'ESP32 la boucle hôte : émettre la trame CMD à la cadence observée, recevoir les
réponses, émettre l'ACK. Le PC envoie les données de jeu **avant** qu'elles soient nécessaires,
l'ESP32 les tient dans un tampon. Les mesures de la phase 1 diront si la latence d'émission de
`esp_wifi_80211_tx()` est acceptable. Être **hôte** est plus favorable que client, car c'est l'hôte
qui fixe le rythme.

### Phase 5 — Protocole du jeu

Décoder ce que le jeu met dans les trames MP. Si votre port PC provient d'une décompilation du jeu,
le code réseau d'origine vous le donne directement ; sinon, il faut le rétro-ingénierer à partir des
captures.

### Si l'ESP32 ne suffit pas

Plan B pour la phase 4 : un adaptateur Wi-Fi USB à firmware ouvert, comme ceux à puce Atheros
AR9271 (pilote `ath9k_htc`, firmware open source modifiable), pour exécuter la boucle MP au plus
près de la radio.

---

## Légal

Les fichiers BIOS/firmware et les jeux doivent provenir de vos propres consoles et cartouches.
Restez à faible puissance et sur les canaux autorisés en France (1 à 13).

## Références

- BlocksDS DSWiFi — fonctions raw TX/RX : https://blocksds.skylyrac.net/docs/dswifi/group__dswifi9__raw__tx__rx.html
- BlocksDS DSWiFi — en-tête `dswifi9.h` : https://blocksds.skylyrac.net/docs/dswifi/dswifi9_8h.html
- GBATEK (documentation matérielle DS, section Wi-Fi) : https://problemkaputt.de/gbatek.htm
- ESP-IDF — envoi de trames 802.11 brutes : https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/wifi.html
