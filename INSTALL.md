# Tutoriel d'installation

Ce guide part de zéro et va jusqu'à la démo qui fonctionne : la DS envoie ses touches au PC, le PC
renvoie du texte à la DS, et la touche **A** mesure la latence. Pour le fonctionnement détaillé,
voir le [README](README.md).

---

## 0. Choisir son parcours

| Vous avez… | Parcours | Matériel en plus | Difficulté |
|---|---|---|---|
| seulement un PC | **A — melonDS** | aucun | facile |
| une vraie DS et un ESP8266 ou un ESP32 | **B — ESP-Pont** (recommandé) | carte ESP, câble USB | moyenne |
| une vraie DS et un ESP32, latence minimale | **C — pont radio** | ESP32 branché en USB au PC | avancée |
| des vraies DS **et** melonDS qui jouent ensemble | **D — hub** (A + B ou C) | celui de B ou C | moyenne |

Les étapes 1 et 2 sont communes à tous les parcours. Faites-les, puis passez directement à la
section de votre parcours.

Pour une vraie DS (B et C), il faut aussi un moyen de lancer des homebrews : linker (carte flash
type R4 ou DSTT) ou TWiLight Menu++ sur DSi ou 3DS.

---

## 1. Installer Python et le client PC

1. Installez **Python 3.9 ou plus récent** depuis https://www.python.org/downloads/.
   Sous Windows, cochez **« Add python.exe to PATH »** dans le premier écran de l'installateur.
2. Ouvrez un terminal dans le dossier du projet et vérifiez :
   ```bash
   python --version
   ```
3. **Parcours C seulement**, installez pyserial (les parcours A et B n'en ont pas besoin) :
   ```bash
   pip install pyserial
   ```

Facultatif : `python tests/test_all.py` lance les tests sans matériel (il faut aussi `gcc`).

> **Interface graphique.** Toutes les commandes `python pc-host/dslink.py …` de ce guide ont leur
> équivalent dans `python pc-host/dslink_gui.py`. L'onglet *Accueil* aide à choisir le parcours,
> l'onglet *Liaison DS* remplace `demo`, et l'onglet *ESP-Pont* vérifie le boîtier et signale les
> réglages incompatibles avec la DS.

---

## 2. Compiler la démo DS (`dslink_demo.nds`)

Le dépôt ne contient pas la démo compilée : il faut la construire avec **BlocksDS**, la chaîne de
compilation homebrew pour DS. La documentation officielle est ici :
https://blocksds.skylyrac.net/docs/setup/

**Sous Windows, sans Docker ni WSL (méthode testée) :**

1. Installez MSYS2, dans un PowerShell :
   ```powershell
   winget install --id MSYS2.MSYS2 -e
   ```
2. Ouvrez le terminal **« MSYS2 MSYS »** (menu Démarrer) et mettez-le à jour. Lancez la commande
   deux fois : la première met à jour le cœur de MSYS2.
   ```bash
   pacman -Syu
   pacman -S --needed make tar wget
   ```
3. Installez Wonderful Toolchain, puis BlocksDS :
   ```bash
   mkdir -p /opt/wonderful && cd /tmp
   wget https://wonderful.asie.pl/bootstrap/wf-bootstrap-windows-x86_64.tar.gz
   tar xzf wf-bootstrap-windows-x86_64.tar.gz -C /opt/wonderful
   export PATH=/opt/wonderful/bin:$PATH
   wf-pacman -Syu            # deux fois si on vous demande de relancer wf-pacman
   wf-pacman -S wf-tools
   wf-config repo enable blocksds
   wf-pacman -Syu
   wf-pacman -S blocksds-toolchain
   ```
4. Compilez, toujours dans le terminal MSYS2 :
   ```bash
   cd /c/Users/<vous>/Documents/ds-pc-link/ds-homebrew
   make
   ```

**Sous Linux ou WSL**, mêmes étapes 3 et 4 avec l'archive de Wonderful pour Linux (voir la doc
officielle).

**Avec Docker**, dans le dossier du projet :
```bash
docker run --rm -v "%cd%":/work -w /work/ds-homebrew skylyrac/blocksds:slim-latest make
```
(sous PowerShell, remplacez `%cd%` par `${PWD}`).

Le Makefile trouve seul BlocksDS dans `/opt/wonderful/thirdparty/blocksds/core` (Wonderful) ou
`/opt/blocksds/core`. S'il est ailleurs : `make BLOCKSDS=/chemin/vers/blocksds/core`.

> Avec une version récente de BlocksDS, `Wifi_Update()` n'existe plus côté DS : la bibliothèque
> Wi-Fi se met à jour seule. Ne l'appelez pas dans vos jeux.

Résultat : `ds-homebrew/dslink_demo.nds`.

> **Plusieurs consoles ?** Chaque DS (ou chaque fenêtre melonDS) doit avoir un numéro différent.
> Changez `#define NODE 1` dans `ds-homebrew/source/main.c` (1 à 15) et recompilez une démo par
> console.

---

## A. Parcours melonDS (aucun matériel)

### A1. Installer melonDS

Téléchargez melonDS depuis https://melonds.kuribo64.net et décompressez-le.

### A2. Régler le Wi-Fi

1. **Firmware :** laissez melonDS utiliser son **firmware intégré** (réglage par défaut, aucun
   fichier `firmware.bin` indiqué dans *Config > Emu settings*). Il connaît déjà le point d'accès
   simulé **melonAP**, sans rien configurer dans la console.
2. **Wi-Fi :** dans *Config > Wi-Fi settings*, choisissez le **mode indirect**. Il fonctionne sans
   rien installer.

### A3. Lancer

1. Sur le PC :
   ```bash
   python pc-host/dslink.py demo --udp
   ```
   Si Windows demande d'autoriser Python sur le réseau, acceptez pour les **réseaux privés**.
2. Dans melonDS : *File > Open ROM…* puis `ds-homebrew/dslink_demo.nds`.
3. Sur la DS émulée, appuyez sur **B** (mode réseau). Après quelques secondes de connexion, l'écran
   affiche `PC connecte`, et le terminal affiche `DS1 connectée via …`.

Passez à la [vérification](#5-vérifier-que-tout-marche).

### Si la DS reste sur « en attente... »

La recherche automatique n'atteint pas le PC dans votre configuration. Indiquez l'adresse du PC
à la main :
1. Trouvez l'IP locale du PC (`ipconfig` sous Windows, ligne « Adresse IPv4 »).
2. Dans `ds-homebrew/source/main.c`, mettez-la dans `PC_IP`, par exemple `#define PC_IP "192.168.1.20"`.
3. Recompilez (étape 2) et relancez.

---

## B. Parcours vraie DS + ESP-Pont (recommandé)

```
 DS ~~(Wi-Fi ouvert)~~ ESP-Pont ~~(Wi-Fi WPA2)~~ box ---- PC
```

### B1. Flasher ESP-Pont

Le détail est dans [esp-pont/README.md](esp-pont/README.md), section 2. En bref :

1. Installez l'**IDE Arduino 2**.
2. Dans *Fichier > Préférences*, ajoutez l'URL du gestionnaire de cartes :
   - ESP8266 : `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   - ESP32 : `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Dans le gestionnaire de cartes, installez `esp8266` (3.1.2 ou plus) ou `esp32` (3.x).
4. Ouvrez `esp-pont/esp-pont.ino`.
5. Choisissez la carte :
   - ESP8266 : **Generic ESP8266 Module**, avec *lwIP Variant* = **v2 Higher Bandwidth**
     (les autres réglages sont dans le README d'ESP-Pont) ;
   - ESP32 : **ESP32 Dev Module**, réglages par défaut.
6. Branchez la carte, choisissez son port COM, puis **Téléverser**. Si aucun port n'apparaît,
   installez le pilote USB : CH340 pour la NodeMCU, CP2102 pour la plupart des ESP32.

### B2. Configurer ESP-Pont

1. Alimentez la carte. Un réseau Wi-Fi ouvert **`ESP-Pont-Setup`** apparaît.
2. Connectez-y un téléphone ou le PC. La page de configuration s'ouvre ; sinon, allez sur
   http://192.168.4.1.
3. Onglet **Liaison amont** : cliquez sur votre box et saisissez sa clé.
4. Onglet **Point d'accès** : donnez un nom au réseau (par exemple `DS-Pont`) et gardez la sécurité
   sur **Ouvert**. La DS ne sait pas se connecter en WPA.
5. Onglet **Radio** : gardez le **802.11b**.
6. Onglet **Système** : vérifiez que **Relayer dslink** est coché (c'est le cas par défaut).
7. **Enregistrer et appliquer**.
8. Reconnectez le PC à votre box. La page d'état d'ESP-Pont, joignable sur http://esp-pont.local
   ou sur l'IP affichée, doit montrer une liaison amont **établie**. Notez cette **adresse amont**.

> ESP-Pont oublie le réseau de la box à chaque redémarrage (choix volontaire, voir son README) :
> après avoir débranché la carte, refaites l'étape 3. La clé reste enregistrée, il suffit de
> recliquer sur la box puis d'enregistrer.

### B3. Enregistrer ESP-Pont dans la DS

La DS se connecte au point d'accès de ses **réglages Wi-Fi Nintendo (WFC)**. Ils s'atteignent :
- **DS / DS Lite :** depuis un jeu compatible Wi-Fi Nintendo (Mario Kart DS, par exemple), menu
  *Nintendo WFC > Paramètres Nintendo WFC > Paramètres de connexion* ;
- **DSi / 3DS :** depuis un jeu DS compatible, de la même façon. Les réglages Internet du menu de
  la console ne servent pas pour les jeux DS.

Choisissez une connexion libre, puis *Rechercher un point d'accès*, sélectionnez `DS-Pont` et
enregistrez. Le test de connexion à Nintendo peut échouer (les serveurs ont fermé) : ce n'est pas
un problème, l'enregistrement est conservé.

### B4. Lancer

1. Sur le PC, branché sur la box :
   ```bash
   python pc-host/dslink.py demo --relay esp-pont.local
   ```
   Si le nom n'est pas trouvé, utilisez l'adresse amont notée en B2, par exemple
   `--relay 192.168.1.50`. Acceptez Python dans le pare-feu de Windows (réseaux privés).
2. Copiez `dslink_demo.nds` sur la carte du linker et lancez-le sur la DS.
3. Appuyez sur **B**. Après la connexion, l'écran affiche `PC connecte`. La page d'état d'ESP-Pont
   affiche le PC inscrit et `1 DS`.

---

## C. Parcours vraie DS + pont radio (esp32-bridge)

```
 DS ~~(trames Wi-Fi brutes)~~ ESP32 ==== USB ==== PC
```

### C1. Installer ESP-IDF

Installez **ESP-IDF 5.3** avec l'installateur officiel d'Espressif :
https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
Il ajoute un raccourci **« ESP-IDF Command Prompt »** : utilisez-le pour les commandes suivantes.

Avec une autre version d'ESP-IDF, voir les deux ajustements notés dans le README, section
*Compilation*.

### C2. Compiler et flasher

```bash
cd esp32-bridge
idf.py set-target esp32        # ou esp32s3 / esp32c3 selon votre carte
idf.py build
idf.py -p COM5 flash           # remplacez COM5 par le port de la carte
```
N'utilisez pas `idf.py monitor` : le port série transporte un protocole binaire, pas du texte.

### C3. Lancer

1. Trouvez le port de l'ESP32 :
   ```bash
   python pc-host/dslink.py ports
   ```
2. Démarrez la démo côté PC :
   ```bash
   python pc-host/dslink.py demo --port COM5
   ```
   Le terminal doit afficher `ESP32 OK — firmware v1`.
3. Lancez `dslink_demo.nds` sur la DS et appuyez sur **A** (radio directe).

---

## D. Vraies DS et melonDS ensemble (hub)

C'est la combinaison des parcours précédents : des vraies DS (parcours B ou C) et des DS dans
melonDS (parcours A) jouent ensemble, le PC faisant suivre les messages des unes aux autres.

1. Préparez chaque partie comme dans son parcours (ESP32 flashé, ESP-Pont configuré, melonDS
   réglé).
2. Compilez **une démo par console**, chacune avec un `NODE` différent dans
   `ds-homebrew/source/main.c` (1, 2, 3…). Gardez par exemple `NODE 1` pour la vraie DS et
   `NODE 2` pour melonDS.
3. Lancez le PC avec tous les transports utilisés :
   ```bash
   python pc-host/dslink.py demo --port COM5 --udp               # pont radio + melonDS
   python pc-host/dslink.py demo --relay esp-pont.local          # ESP-Pont + melonDS
   ```
   ou, dans `dslink_gui.py`, onglet *Liaison DS* : cochez les deux transports.
4. Lancez la démo sur chaque console : **A** sur les vraies DS du pont radio, **B** sur celles
   d'ESP-Pont et dans melonDS.
5. Sur chaque écran, la section « autres DS » montre les touches tenues par les autres consoles.

Le hub ne sert qu'aux jeux qui utilisent `dslink`. Il ne fait pas jouer une cartouche d'origine
ni le « salon Wi-Fi local » de Void Crew avec melonDS (voir le README).

---

## 5. Vérifier que tout marche

Une fois la DS connectée :

| Action | Résultat attendu |
|---|---|
| appuyer sur des touches de la DS | le terminal affiche `DS1 touches=0x…` à chaque changement |
| appuyer sur **A** sur la DS | la ligne `RTT` de la DS affiche le temps d'aller-retour en µs |
| taper un texte dans le terminal + Entrée | il s'affiche sur la DS, ligne `PC:` |
| **START** sur la DS | quitte la démo |
| **Ctrl+C** dans le terminal | arrête le PC |

---

## 6. Dépannage

**Le terminal dit « Relais introuvable ».** Le PC ne résout pas `esp-pont.local`. Utilisez l'IP
amont affichée sur la page d'état d'ESP-Pont.

**La DS dit « Echec : aucun point d'acces joignable ».** Elle n'a aucun réseau enregistré, ou il
n'est pas à portée. Refaites B3. Sous melonDS, vérifiez le firmware intégré et le mode Wi-Fi (A2).

**La DS se connecte mais reste sur « en attente... ».**
- Parcours B : le PC est-il lancé avec `--relay` ? La page d'état d'ESP-Pont indique-t-elle
  « aucun PC inscrit » ? Si oui, vérifiez l'adresse du relais et le pare-feu de Windows.
- Parcours A : voir la fin de la section A.

**La DS ne voit pas le réseau d'ESP-Pont.** Vérifiez : sécurité **Ouvert**, protocole **802.11b**,
SSID **non masqué**.

**Parcours C : « Pas de réponse de l'ESP32 ».** Mauvais port, ou firmware pas flashé. Fermez tout
autre logiciel qui utiliserait le port (moniteur série de l'IDE Arduino, par exemple).

**Plusieurs DS, une seule apparaît.** Elles ont le même `NODE` : recompilez la démo avec un numéro
différent pour chacune (étape 2).

---

## 7. Et ensuite ?

Pour utiliser la liaison dans vos propres jeux, voir *Intégrer dans vos jeux* et *Mode réseau*
dans le [README](README.md). Côté DS, copiez `dslink.c`, `dslink.h` et `common/linkproto.h`. Côté
PC, importez `DsLinkUdp` ou `DsLinkBridge` depuis `pc-host/dslink.py`.
