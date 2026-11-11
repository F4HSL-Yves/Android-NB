Récepteur et émetteur BLU QO-100 bande étroite pour carte Tezuka (FishBall 7020, Pluto) et téléphone Android.

## Ce qu'il faut

- Une carte avec le firmware **Tezuka** (broker MQTT inclus), reliée au LNB et, pour émettre, au PA.
- Un téléphone **Android 8 ou plus récent** sur le même réseau que la carte.
- Facultatif : l'Arduino de commutation PTT (le même qu'avec Android-DATV).

Le firmware de la carte n'est pas modifié : tout se trouve dans un dossier `nb/` sur la carte SD, et une ligne s'ajoute au démarrage automatique.

## Installer sur la carte

1. Télécharger `qo100-nb-carte-….zip` et le décompresser : on obtient un dossier `nb`.
2. Copier ce dossier sur la carte SD, dans la partition `boot`, soit avec la carte SD dans un PC, soit depuis le PC avec `scp -O -r nb root@ADRESSE_CARTE:/boot/`.
3. Se connecter à la carte (`ssh root@ADRESSE_CARTE`) et lancer `sh /boot/nb/install.sh`.

La carte démarre ensuite en bande étroite à chaque mise sous tension. Un démarrage DATV (Webtioune, Android-DATV) reprend la radio automatiquement, puis la rend à la bande étroite quand il s'arrête.

Les réglages sont dans `/boot/nb/nb.conf` :
- retirer `--tx` pour une carte en réception seule ;
- retirer `--beacon 10489750000` pour se caler sur la balise CW à 10489,500 au lieu de la balise centrale.

## Installer l'application

1. Télécharger `qo100-nb-….apk` sur le téléphone et l'ouvrir. Android demande d'autoriser l'installation depuis le navigateur ou le gestionnaire de fichiers.
2. Dans **Réglages réception**, saisir l'adresse de la carte, puis appuyer sur **Écouter**.
3. Pour émettre, dans **Réglages émission** :
   - indiquer si un Arduino commande le PTT ;
   - lancer **Calibrer** une fois pour régler la correction TX (émet une porteuse quelques secondes).
   Le gain TX se règle sur l'accueil.

## Mettre à jour

Remplacer `/boot/nb/nbd` par celui du nouveau zip, puis lancer `sh /boot/nb/nbctl.sh nb` ou redémarrer la carte. Installer le nouvel APK par-dessus l'ancien.
