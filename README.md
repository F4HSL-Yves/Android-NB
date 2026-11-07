# Android-NB

Émetteur-récepteur QO-100 narrowband piloté depuis un téléphone Android, avec
un Pluto ou une FishBall 7020 sous firmware Tezuka (F5OEO).

Le traitement tourne sur la carte : un démon (`nbd`) lit les IQ du transpondeur
NB fournis par le DDC de Maia, se verrouille en continu sur la balise CW,
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
- Le verrou suit la balise CW basse (10489,500 MHz).

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

## Installation durable sur la carte (Tezuka)

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
s'il s'arrête, et retient le mode pour le prochain démarrage. `zynqrx`
reste lancé comme sur le firmware d'origine : en `nb` il est mis en pause
(`cmd/zynqrx/process stop`), et un start DATV depuis le Dashboard pendant
que `nbd` tourne arrête `nbd` et repasse en `datv`.

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
6 kHz (le zoom suit la fréquence). L'APK de debug est
produit par la CI.

## Licence

GPL-3.0 (WDSP est sous GPL-2.0 ou ultérieure).
