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
sudo apt-get install libfftw3-dev
cmake -S daemon -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Pour la carte (binaire statique ARMv7, Cortex-A9) depuis Ubuntu 24.04 :

```sh
sudo dpkg --add-architecture armhf   # + dépôt ports.ubuntu.com pour armhf
sudo apt-get install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libfftw3-dev:armhf
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

# 2. En direct, une fois le DDC Maia réglé à 768 kS/s autour de 10489,750 MHz
#    et routé vers la DMA (prochaine étape : nbd le fera lui-même) :
iio_readdev -u local: -b 4096 cf-ad9361-lpc voltage0 voltage1 |
  /mnt/jffs2/nbd --center 10489750000 --freq 10489900000 --output /dev/null
```

`nbd` affiche chaque seconde l'état de la balise (searching, locked,
holdover), la correction en Hz et la dérive en Hz/s. Le premier lancement
avec `--wisdom` calcule les plans FFTW de WDSP, ce qui peut prendre plusieurs
minutes sur le Cortex-A9 ; ils sont ensuite réutilisés.

## Licence

GPL-3.0 (WDSP est sous GPL-2.0 ou ultérieure).
