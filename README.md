# Android-NB

Émetteur-récepteur QO-100 narrowband piloté depuis un téléphone Android, avec
un Pluto ou une FishBall 7020 sous firmware Tezuka (F5OEO).

Le traitement tourne sur la carte : un démon (`nbd`) lit les IQ du transpondeur
NB fournis par le DDC de Maia, se verrouille en continu sur la balise PSK centrale,
démodule avec [WDSP](https://github.com/jimahlstrom/wdsp) et, à terme, envoie
l'audio en Opus au téléphone. Seuls l'audio et un waterfall allégé passent par
le réseau, ce qui permet le trafic à distance en 4G/5G (environ 150 kbit/s).

## Contenu

| Dossier | Rôle |
| --- | --- |
| `daemon/` | Démon C++ pour l'ARM du Zynq : verrou balise, WDSP, outils de test |
| `daemon/third_party/wdsp` | WDSP (sous-module, portage Linux de N2ADR) |
| `app/` | Application Android (Kotlin, Jetpack Compose) |

## Plan de fréquences retenu

- Transpondeur NB : 10489,500 à 10490,000 MHz en descente, 2400,000 à 2400,500 MHz en montée.
- Balises : CW basse 10489,500, PSK 10489,750, haute 10490,000 MHz.
- Flux IQ : 768 kS/s centré sur 10489,750 MHz, soit tout le transpondeur plus les deux balises extrêmes.
- Le verrou suit la balise PSK centrale (10489,750 MHz), comme SDR Console :
  tout le monde lit ainsi la même fréquence. Cette balise est en BPSK 400 Bd,
  sans porteuse : `nbd` l'élève au carré pour retrouver une raie. Sans
  `--beacon 10489750000` dans `nb.conf`, il se cale sur la balise CW basse
  (10489,500 MHz).

## Compiler le démon

```sh
git submodule update --init
sudo apt-get install libfftw3-dev libopus-dev
cmake -S daemon -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Pour la carte (binaire statique ARMv7, Cortex-A9) depuis Ubuntu 24.04 :

```sh
sudo dpkg --add-architecture armhf   # + dépôt ports.ubuntu.com pour armhf
sudo apt-get install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libfftw3-dev:armhf libopus-dev:armhf
cmake -S daemon -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-armhf.cmake -DNBD_BUILD_TESTS=OFF
cmake --build build-arm -j
```

La CI GitHub produit aussi ce binaire (artefact `nbd-zynq-armhf`) et l'APK de debug.

## Essai sur la carte

Copier `nbd` et `nbsynth` sur `/mnt/jffs2/` (persistant), puis :

```sh
# 1. Charge CPU sur signal synthétique : au-dessus de 1x, la carte suit.
/mnt/jffs2/nbsynth --seconds 30 > /tmp/test.cs16
/mnt/jffs2/nbd --input /tmp/test.cs16 --output /dev/null --wisdom /mnt/jffs2/

# 2. En direct : nbd règle l'AD9363 (LO 740,25 MHz, 3,072 MS/s, gain auto),
#    le DDC Maia (-500 kHz, décimation 4 -> 768 kS/s), route le DDC vers la
#    DMA et lit les IQ par iiod. Arrêter zynqrx et l'enregistreur Maia avant.
/mnt/jffs2/nbd --source pluto --freq 10489900000 --output /dev/null --wisdom /mnt/jffs2/

# Même chose depuis un PC du réseau (la carte en 192.168.2.1) :
nbd --source pluto:192.168.2.1 --freq 10489900000 --output - | aplay -f S16_LE -r 48000
```

Options utiles en direct : `--gain 40` (gain manuel en dB, sinon
`slow_attack`), `--lnb-lo` si le LNB n'est pas à 9750 MHz, `--duration 60`.
À l'arrêt (Ctrl-C ou fin), `nbd` ferme le buffer et rend la DMA à l'AD9363,
donc zynqrx et Maia retrouvent leur flux normal.
```

`nbd` affiche chaque seconde l'état de la balise (searching, locked,
holdover), la correction en Hz et la dérive en Hz/s. Le premier lancement
avec `--wisdom` calcule les plans FFTW de WDSP, ce qui peut prendre plusieurs
minutes sur le Cortex-A9 ; ils sont ensuite réutilisés.

## Téléphone : audio et commandes par WebSocket

Avec `--ws 8073`, `nbd` sert l'audio démodulé en Opus (48 kHz mono, paquets
de 20 ms, 16 kbit/s par défaut, réglable avec `--bitrate`), l'état de la
balise chaque seconde en JSON, et accepte les commandes de l'application :
fréquence, filtre, AGC, débit. Le protocole est décrit dans
`daemon/src/protocol.h`. Si la liaison cellulaire ralentit, `nbd` jette
l'audio en retard plutôt que d'accumuler du délai.

```sh
/mnt/jffs2/nbd --source pluto --ws 8073 --wisdom /mnt/jffs2/

# Vérification depuis un PC (Python standard, rien à installer) :
python3 daemon/tools/nbws_probe.py ws://192.168.2.1:8073 --freq 10489900000

# Sans carte, pour développer l'application : un fichier rejoué en temps réel.
nbsynth --seconds 60 > test.cs16
nbd --input test.cs16 --realtime --loop --ws 8073
```

### Waterfall

Les clients reçoivent aussi le waterfall de la bande, en lignes de 8 bits
par case (0,5 dB par pas, niveau de référence dans l'en-tête), avec la
dérive du LNB déjà retirée : l'axe est en vraies fréquences de descente.
L'application choisit la vue (toute la bande ou un zoom autour de la
fréquence), le nombre de cases (largeur de l'écran, 1024 au plus) et la
cadence (4 à 12 lignes/s, ou arrêt). Compter environ 50 kbit/s pour
768 cases à 8 lignes/s, 65 kbit/s pour 1024 cases.

Le spectre vient du spectromètre FPGA de Maia (WebSocket `/waterfall` de
maia-httpd, déjà branché sur la sortie du DDC), donc sans FFT sur le CPU de
la carte. Si Maia ne fournit rien dans les 5 premières secondes, ou pour un
fichier rejoué, `nbd` calcule le spectre lui-même (`--spectrum local`).
`--spectrum off` le coupe. Côté PC :

```sh
python3 daemon/tools/nbws_probe.py ws://192.168.2.1:8073 --spectrum 768000,768,8
```

## Installer (utilisateurs)

Sur le téléphone, ouvrir
**https://github.com/F4HSL-Yves/Android-NB/releases/latest/download/qo100-nb.apk**
(toujours la dernière version) ou scanner ce QR code :

![QR code : télécharger l'APK](docs/img/qr-apk.png)

Chaque version publiée (onglet « Releases » du dépôt) contient l'APK signé,
qui embarque `nbd` et ses scripts : le bouton « Installer sur la carte » de
l'appli les copie par ssh dans `/boot/nb` et ajoute le lancement à
`/mnt/jffs2/autorun.sh`. Le firmware Tezuka n'exécute rien depuis la SD, d'où
le passage par ssh. Le zip `nb/` reste fourni pour une installation à la main.
La marche à suivre est dans [docs/release-notes.md](docs/release-notes.md),
recopiée sur la page de chaque version.

## Distribuer une nouvelle version

Pousser un tag `v0.2` (par exemple) sur le commit à publier : le workflow
`release.yml` compile `nbd` pour la carte et l'APK release, puis crée la
release GitHub. L'APK est signé avec la clé fournie dans les secrets du
dépôt :
- `NB_KEYSTORE_B64` : le keystore, en base64 ;
- `NB_KEYSTORE_PASSWORD` ;
- `NB_KEY_ALIAS` ;
- `NB_KEY_PASSWORD`.

Sans ces secrets, il retombe sur la clé de debug. La clé de release ne
change jamais : un APK signé avec une autre clé ne peut pas mettre à jour
l'appli déjà installée.

## Installation durable sur la carte (Tezuka)

Le plus simple : `sh /boot/nb/install.sh` une fois le dossier `nb/` copié
(voir ci-dessus). À la main :

La racine de la carte est en RAM et la jffs2 est presque pleine : `nbd` et
ses réglages vont sur la carte SD (`/boot/nb/`), et seul un petit
`autorun.sh` va dans la jffs2 (lancé par `S98autostart`).

```sh
# Depuis le PC (scp -O : la carte n'a pas sftp-server)
ssh root@192.168.1.125 mkdir -p /boot/nb
scp -O nbd board/nbctl.sh board/nb.conf root@192.168.1.125:/boot/nb/
scp -O board/autorun.sh root@192.168.1.125:/mnt/jffs2/autorun.sh
```

Une seule application peut tenir le récepteur : `nbctl.sh` bascule entre
la réception DATV (`zynqrx`) et la bande étroite (`nbd`), relance `nbd`
s'il s'arrête. `zynqrx`
reste lancé comme sur le firmware d'origine : en `nb` il est mis en pause
(`cmd/zynqrx/process stop`). NB est le mode par défaut, y compris au
démarrage : un start DATV (Webtioune, Android-DATV…) arrête `nbd` et passe
en `datv`, et le stop correspondant revient en `nb`, même téléphone éteint.

```sh
mosquitto_pub -h 192.168.1.125 -t cmd/nb/mode -m nb     # bande étroite
mosquitto_pub -h 192.168.1.125 -t cmd/nb/mode -m datv   # retour au DATV
mosquitto_sub -h 192.168.1.125 -t state/nb/mode         # mode courant (retenu)
```

Sur la carte : `sh /boot/nb/nbctl.sh nb|datv|status`. Le journal de `nbd`
est dans `/tmp/nbd.log`, la wisdom FFTW dans `/boot/nb/`.

## Application Android

L'application se connecte à `nbd` (adresse de la carte, port 8073), joue
l'audio avec le décodeur Opus d'Android (MediaCodec, aucune bibliothèque
ajoutée) et affiche l'état de la balise, la correction LNB et le S-mètre.
On y règle la fréquence (pas de 10 Hz à 10 kHz, ou saisie directe), le
filtre, l'AGC, le débit et le tampon anti-gigue (80 à 300 ms : plus long en
4G, plus court en Wi-Fi). La reconnexion est automatique.

Au-dessus du cadran, le spectre et le waterfall montrent la bande avec les
balises, la fréquence d'écoute (trait rouge) et la bande passante du
filtre. Toucher un signal centre le filtre dessus ; glisser à
l'horizontale accorde comme un bouton. Vues : toute la bande, 100, 25 ou
6 kHz (le zoom suit la fréquence).

START met d'abord la carte en mode NB par son MQTT (port 1883 :
`cmd/nb/mode nb`, donc zynqrx en pause et nbd lancé), puis se connecte à
`nbd`. STOP publie `cmd/nb/mode datv` : nbd s'arrête et la radio est libre
pour SDR Console. Un start DATV (Webtioune, Android-DATV) prend la radio et
l'appli affiche « Réception DATV en cours » ; au stop, la carte revient
d'elle-même en NB et l'appli se reconnecte. L'APK de debug est
produit par la CI.

## Émission BLU (full duplex)

Avec `--tx`, `nbd` module aussi la BLU : l'appli envoie son micro en Opus
(48 kHz mono, paquets de 20 ms, type binaire 0x03), `nbd` le décode, le
module en USB avec WDSP (leveler et ALC, porteuse de réglage possible),
monte à 3,072 MS/s (CIC d'ordre 4 puis NCO) et l'écrit dans la DMA TX de
l'AD9363 par iiod. La réception continue pendant ce temps : on entend son
propre signal revenir du satellite.

- **Plan de fréquences** : montée = descente - 8089,5 MHz, plus une
  correction réglable dans l'appli (`txcal`). L'OL TX de l'AD9363 reste à
  2399,5 MHz (`--tx-lo`), sous le transpondeur, pour que sa fuite et
  l'image IQ tombent hors bande ; le signal est placé à +0,5 à +1 MHz par
  le NCO.
- **Même logique qu'Android-DATV** pour la RF : l'appli passe
  `cmd/pluto/<indicatif>/tx/stream/mode` à `pass` (pluto_stream libère la
  DMA TX ; l'ancien mode est remis quand on arrête l'appli), demande
  l'émission à `nbd` (RF encore muette), active `arduino/ptt on`, attend
  `arduino/status/ptt on`, puis seulement `tx/mute 0`. Au relâchement :
  `tx/mute 1`, `arduino/ptt off`, puis `nbd`. Le curseur de gain règle
  `cmd/pluto/<indicatif>/tx/gain` (-50 à 0 dB). Sans Arduino (réglage de
  l'appli), le PTT ne commande que le mute.
- **Sécurités dans `nbd`** : il coupe l'OL TX (`TX_LO_powerdown`) et publie
  `arduino/ptt off` si le téléphone se déconnecte, si le flux micro
  s'arrête plus d'une seconde, si la fréquence sort du transpondeur ou
  après 10 minutes d'émission (`--tx-max`). L'appli coupe aussi
  l'émission quand elle passe en arrière-plan.
- **Essai sans l'appli** : `tools/nbws_probe.py ws://CARTE:8073 --tx-tune
  --seconds 10` ouvre l'émetteur avec une porteuse à +1 kHz du cadran ; la
  RF reste muette tant que `tx/mute` n'est pas passé à 0. Hors carte,
  `--tx-file tx.cs16` écrit les IQ dans un fichier au lieu de la DMA.

## Licence

GPL-3.0 (WDSP est sous GPL-2.0 ou ultérieure).
