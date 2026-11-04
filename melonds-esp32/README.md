# melonds-esp32 — melonDS + vraies DS, en multijoueur natif

Ce dossier ajoute à l'émulateur [melonDS](https://github.com/melonDS-emu/melonDS) une interface
multijoueur qui passe par un **ESP32**. melonDS joue la **DS hôte** d'une partie multijoueur
native (cartouche, téléchargement DS, multijoueur local de DSWiFi), et de **vraies DS**
la rejoignent comme clientes, sans savoir que l'hôte est émulé.

```
 melonDS (DS hôte émulée) ==USB== ESP32 (esp32-bridge, MODE_MP_HOST) ~~802.11b~~ vraies DS clientes
```

> **État : expérimental, jamais testé sur du matériel.** Le code compile. Le protocole suit ce
> qu'émule melonDS et ce que décrit GBATEK, mais personne ne l'a encore essayé avec de vraies DS.
> Voir [ce qui peut échouer](#ce-qui-peut-échouer).

---

## Pourquoi melonDS hôte, et pas l'inverse

Le multijoueur natif de la DS est cadencé par l'hôte. Il émet une trame **CMD** plusieurs dizaines
de fois par seconde, et chaque cliente doit répondre dans un créneau de **quelques centaines de
microsecondes**. Sur une vraie DS, c'est le matériel radio qui répond, avec des données préparées
à l'avance.

- **melonDS hôte :** après une CMD, melonDS *attend* les réponses (jusqu'à 25 ms par défaut).
  L'émulateur absorbe le délai de l'USB, et les vraies DS reçoivent leurs CMD normalement.
  C'est ce qui est réalisé ici.
- **melonDS client :** l'ESP32 devrait répondre seul, dans le créneau, à la place de l'émulateur.
  Ce n'est pas réalisé : l'interface ne reçoit rien dans ce rôle.

## Comment ça marche

| melonDS appelle | l'ESP32 |
|---|---|
| `SendPacket` (beacon, réponse d'authentification ou d'association…) | émet la trame. À la première, il **prend l'adresse MAC de l'hôte émulé**, pour que sa radio acquitte d'elle-même, en quelques µs, les trames que les clientes adressent à l'hôte. Il suit aussi le canal de l'hôte. |
| `RecvPacket` | lui remonte les trames reçues des clientes (authentification, association, sondes). |
| `SendCmd` | émet la CMD et ouvre une **fenêtre de réponses** (3 ms par défaut). |
| `RecvReplies` | lui transmet chaque réponse reçue dans la fenêtre. melonDS repart dès que toutes les clientes ont répondu, ou quand l'ESP32 clôt la fenêtre. |
| `SendAck` | émet l'ACK de fin de cycle. |

Les réponses sont rangées par **AID** (numéro de cliente). L'interface l'apprend en lisant les
réponses d'association que melonDS envoie.

Le firmware d'Espressif refuse normalement d'émettre les trames d'authentification et
d'association. `esp32-bridge` remplace la fonction de contrôle concernée à l'édition de liens
(`-zmuldefs`). N'émettez ces trames que pour un jeu que vous émulez, sur votre propre matériel.

---

## Installation

### 1. Flasher l'ESP32

`esp32-bridge` gère maintenant le mode `MODE_MP_HOST` : flashez-le comme indiqué dans
[INSTALL.md](../INSTALL.md), parcours C. Cible conseillée : un **ESP32 classique**
(ESP32-WROOM-32, DevKit).

### 2. Compiler melonDS avec l'interface ESP32

Sous Windows, dans le terminal **« MSYS2 UCRT64 »** :

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,SDL2,libarchive,enet,zstd,faad2}
pacman -S --needed mingw-w64-ucrt-x86_64-{qt6-base,qt6-svg,qt6-multimedia,qt6-tools}
cd /c/Users/<vous>/Documents/ds-pc-link
./melonds-esp32/build.sh
```

Le script :
1. récupère melonDS à la version sur laquelle le patch a été écrit, dans `../melonDS-esp32` ;
2. y copie `src/net/ESP32MP.cpp` et `ESP32MP.h` ;
3. applique `melonds.patch`, qui ajoute trois choses à melonDS : le type `MPInterface_ESP32`,
   son choix quand `MELONDS_ESP32_PORT` est défini, et le fichier à la compilation ;
4. compile. L'exécutable est `../melonDS-esp32/build/melonDS.exe`.

Sous Linux, mêmes étapes avec les paquets de votre distribution (voir `BUILD.md` de melonDS).

### 3. Lancer

Toujours dans le terminal MSYS2 UCRT64, pour que melonDS trouve les DLL de Qt :

```bash
export MELONDS_ESP32_PORT=COM5           # port de l'ESP32 (Linux : /dev/ttyUSB0)
../melonDS-esp32/build/melonDS.exe
```

Facultatif : `MELONDS_ESP32_WINDOW_US=4000` allonge la fenêtre de réponses (en µs).

Le journal de melonDS s'affiche dans ce terminal. Vous devez y voir `ESP32MP: port COM5 ouvert`
puis `ESP32 prêt, firmware v1`.

### 4. Jouer

1. Dans melonDS, lancez le jeu et **créez la partie** (hôte).
2. Sur les vraies DS, cherchez une partie avec le même jeu, ou ouvrez le téléchargement DS.
3. À l'association d'une DS, le journal affiche `cliente XX:XX:… associée, AID n`.
4. Pendant la partie, `réponse(s) arrivée(s) hors fenêtre` signale des réponses trop tardives
   pour la fenêtre. Allongez alors `MELONDS_ESP32_WINDOW_US`.

---

## Ce qui peut échouer

Ces points ne se vérifient qu'avec le matériel. Ils sont rangés du plus probable au moins
probable.

1. **L'ACK arrive trop tard.** melonDS fabrique l'ACK un peu après les réponses (temps émulé),
   et il traverse ensuite l'USB : il part plusieurs millisecondes après les réponses, contre
   quelques centaines de µs pour une vraie DS hôte. Si les clientes se déconnectent en cours de
   partie, c'est la première piste. Le remède serait que l'ESP32 émette lui-même l'ACK à la fin
   de la fenêtre.
2. **La radio de l'ESP32 n'acquitte pas les trames des clientes.** L'ESP32 est en écoute libre,
   sans être associé à un réseau. S'il n'envoie pas l'accusé de réception 802.11 des trames
   adressées à sa MAC empruntée, les DS répètent leurs demandes d'authentification sans jamais
   avancer. Le journal montre alors les mêmes demandes en boucle.
3. **Le cadencement des beacons.** melonDS émet ses beacons selon son temps émulé. Si
   l'émulation ralentit (melonDS attend les réponses à chaque CMD), l'intervalle s'étire et les
   DS peuvent perdre la partie de vue.
4. **La latence du port série.** Elle dépend de la puce USB : les CP210x conviennent, les FTDI
   ont un délai de 16 ms par défaut, à réduire dans le gestionnaire de périphériques.

Pour diagnostiquer, capturez avec un **second** ESP32 (`dslink.py sniff --nintendo`). Comparez
ensuite avec une capture de deux vraies DS : l'onglet *Analyse* de `dslink_gui.py` affiche les
délais CMD → réponse.

## Licence

`ESP32MP.cpp`, `ESP32MP.h` et `melonds.patch` sont destinés à melonDS, distribué sous licence
GNU GPL v3 ou ultérieure. Ils sont placés sous la même licence.
