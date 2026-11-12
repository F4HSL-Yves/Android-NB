Récepteur et émetteur BLU QO-100 bande étroite pour carte Tezuka (FishBall 7020, Pluto) et téléphone Android.

Mode d'emploi complet : https://github.com/F4HSL-Yves/Android-NB/blob/main/docs/mode-emploi.md

## Nouveautés de la v1.11

- **Le curseur suit le doigt** : en glissant sur le spectre vers la droite, le curseur de fréquence part maintenant vers la droite (il partait à l'envers).

## Nouveautés de la v1.10

- **Émission possible sous Android 8 et 9** : ces versions n'ont pas d'encodeur Opus, et le PTT affichait « Micro : Failed to initialize audio/opus ». L'appli embarque maintenant son propre encodeur pour ces téléphones. À partir d'Android 10, rien ne change.

## Nouveautés de la v1.9

- **Installation sur la carte réparée pour Android 8 à 12** : le bouton « Installer / mettre à jour » échouait avec l'erreur « No virtual method toString(Ljava/nio/charset/Charset;) ». L'appli utilisait une fonction qui n'existe que depuis Android 13.

## Nouveautés de la v1.8

- **Émission normale dès le premier START après SDR Console** : SDR Console laisse ses filtres FIR et des horloges calculées pour eux. nbd coupait le filtre d'émission au PTT sans recalculer les horloges, et le signal sortait très faible jusqu'au START suivant. nbd coupe maintenant les filtres avant de régler les horloges, dès son démarrage.

## Nouveautés de la v1.7

- **Le gain TX choisi dans l'appli est retenu** et remis sur la carte à chaque START et à chaque PTT. SDR Console laissait la carte à -40 dB, et il fallait plusieurs START/STOP pour retrouver le niveau.

## Nouveautés de la v1.6

- **L'émission marche après SDR Console** : SDR Console laisse une bande d'émission étroite (1,15 MHz) et un filtre FIR sur la carte, qui atténuaient fortement le signal. nbd remet la bande à 18 MHz et coupe ce filtre à chaque émission. Il n'est plus nécessaire de redémarrer la carte.

## Nouveautés de la v1.5

- **STOP libère toujours la radio**, même pendant « Ça démarre… » ou quand la liaison MQTT se reconnecte. Avant, SDR Console pouvait se retrouver à partager la radio avec la bande étroite (son haché, spectre décalé).
- **La carte se souvient du mode** : après un STOP, elle reste libre pour SDR Console même si elle redémarre. START reprend la bande étroite.

## Nouveautés de la v1.4

- **Sans Arduino par défaut** : le PTT commande directement la sortie RF de la carte. Avec un Arduino de commutation, activer « PTT par l'Arduino » dans **Réglages émission** (à refaire une fois après la mise à jour si on s'en sert).
- **Mode d'emploi** complet de l'appli, ouvert par le bouton **?** à côté du titre de l'accueil.

## Nouveautés de la v1.3

- **PTT et calibration dès la première utilisation** : juste après l'installation, une fois le micro autorisé, le PTT ne réagissait pas et « Calibrer » restait grisé pendant une trentaine de secondes.

## Nouveautés de la v1.2

- **Installation de la carte automatique** : au premier START, l'appli installe QO-100 NB sur la carte toute seule (mot de passe root par défaut de Tezuka), et la met à jour à chaque nouvelle version de l'appli.
- **Démarrage immédiat** : les optimisations FFT sont fournies, nbd ne passe plus plusieurs minutes à les calculer à la première installation.

## Nouveautés de la v1.1

- **Installation de la carte depuis l'appli** : plus rien à copier à la main.
- **START / STOP** en bas de l'accueil, à côté du PTT. STOP rend la radio de la carte : SDR Console (ou un autre logiciel) peut alors s'en servir. START la reprend.
- **Verrouillage balise plus fiable** au démarrage : il ne se cale plus sur le reflet de la balise, et reprend sa recherche après 15 s s'il la perd.

## Ce qu'il faut

- Une carte avec le firmware **Tezuka** (broker MQTT inclus), reliée au LNB et, pour émettre, au PA.
- Un téléphone **Android 8 ou plus récent** sur le même réseau que la carte.
- Facultatif : l'Arduino de commutation PTT (le même qu'avec Android-DATV).

Le firmware de la carte n'est pas modifié. L'application installe elle-même ce qu'il faut sur la carte SD : il n'y a rien à copier depuis un PC.

## Installer

1. Sur le téléphone, ouvrir ce lien, qui donne toujours la dernière version :
   **https://github.com/F4HSL-Yves/Android-NB/releases/latest/download/qo100-nb.apk**
   ou scanner ce QR code :

   ![QR code : télécharger l'APK](https://raw.githubusercontent.com/F4HSL-Yves/Android-NB/main/docs/img/qr-apk.png)

   Si Chrome prévient que le fichier peut être dangereux, choisir « Télécharger quand même ». Ouvrir ensuite le fichier téléchargé. La première fois, Android demande d'autoriser l'installation depuis Chrome : « Paramètres », activer « Autoriser cette source », revenir et toucher « Installer ». Si Play Protect avertit d'une appli inconnue, choisir « Installer quand même ».
2. Dans l'onglet **Réglages réception**, saisir l'adresse de la carte (192.168.1.125 par défaut).
3. Appuyer sur **START** en bas de l'accueil.

Au premier START, l'appli installe QO-100 NB sur la carte toute seule (le bouton affiche « Installation… » quelques secondes) : elle copie le programme et ses scripts dans `/boot/nb` sur la carte SD par ssh, avec le mot de passe root par défaut de Tezuka (`analog`), puis ajoute le lancement au démarrage. Si ce mot de passe a été changé, le saisir dans **Réglages réception**, « Installer sur la carte », et appuyer une fois sur **Installer / mettre à jour**.

La bande étroite démarre aussitôt, puis à chaque mise sous tension (sauf après un STOP, voir plus bas). Un démarrage DATV (Webtioune, Android-DATV) reprend la radio automatiquement, puis la rend à la bande étroite quand il s'arrête. Pour SDR Console, qui ne prévient pas la carte, appuyer d'abord sur **STOP**.

4. Pour émettre, dans **Réglages émission** :
   - activer « PTT par l'Arduino » seulement si un Arduino commande le PA (désactivé par défaut) ;
   - lancer **Calibrer** une fois pour régler la correction TX (émet une porteuse quelques secondes).
   Le gain TX se règle sur l'accueil.

Les réglages de la carte sont dans `/boot/nb/nb.conf`, que les mises à jour ne remplacent pas :
- retirer `--tx` pour une carte en réception seule ;
- retirer `--beacon 10489750000` pour se caler sur la balise CW à 10489,500 au lieu de la balise PSK centrale (celle qu'utilise SDR Console).

Sans l'appli, le zip `qo100-nb-carte-….zip` contient les mêmes fichiers : copier son dossier `nb` dans la partition `boot` de la carte SD, puis lancer une fois `sh /boot/nb/install.sh` par ssh.

## Mettre à jour

Installer le nouvel APK par-dessus l'ancien (même lien), puis appuyer sur **START** : l'appli met la carte à jour toute seule, et ses réglages sont conservés.
