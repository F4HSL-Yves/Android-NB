# Mode d'emploi de QO-100 NB

QO-100 NB transforme un téléphone Android en émetteur-récepteur BLU pour le
transpondeur bande étroite de QO-100. La radio est une carte sous firmware
Tezuka (FishBall 7020 ou Pluto). La carte fait tout le traitement : réception
du transpondeur, verrouillage sur la balise et démodulation. Le téléphone
reçoit l'audio et le waterfall par le réseau, et lui envoie le micro. Le trafic
marche donc aussi à distance, en 4G ou 5G (environ 150 kbit/s).

## Sommaire

1. [Ce qu'il faut](#1-ce-quil-faut)
2. [Installer l'appli](#2-installer-lappli)
3. [Premier démarrage](#3-premier-démarrage)
4. [L'écran d'accueil](#4-lécran-daccueil)
5. [Recevoir](#5-recevoir)
6. [Émettre](#6-émettre)
7. [Réglages réception](#7-réglages-réception)
8. [Réglages émission](#8-réglages-émission)
9. [Partager la carte avec le DATV et SDR Console](#9-partager-la-carte-avec-le-datv-et-sdr-console)
10. [Trafic à distance (4G, 5G)](#10-trafic-à-distance-4g-5g)
11. [Mettre à jour](#11-mettre-à-jour)
12. [Réglages de la carte (nb.conf)](#12-réglages-de-la-carte-nbconf)
13. [Sécurités d'émission](#13-sécurités-démission)
14. [Problèmes courants](#14-problèmes-courants)

---

## 1. Ce qu'il faut

- Une carte **FishBall 7020** ou un **Pluto** avec le firmware **Tezuka**
  (F5OEO). Le broker MQTT de Tezuka sert à piloter la carte. Elle est reliée
  au LNB pour recevoir et au PA pour émettre.
- Un téléphone **Android 8 ou plus récent**, sur le même réseau que la carte,
  ou relié à ce réseau par un VPN.
- Facultatif : l'**Arduino de commutation PTT**, le même qu'avec
  Android-DATV. L'appli fonctionne sans lui, et c'est le réglage par défaut.

Le firmware de la carte n'est pas modifié. L'appli dépose ses fichiers dans
`/boot/nb` sur la carte SD, et ajoute une ligne à `/mnt/jffs2/autorun.sh` pour
se lancer au démarrage.

**Pluto d'origine (sans lecteur de carte SD)** : l'appli met ses fichiers en
mémoire, dans `/tmp/nb`, et seulement `nb.conf` et quelques octets de
mémorisation dans la flash (`/mnt/jffs2/nb`). La mémoire est vidée à chaque
redémarrage du Pluto : l'appli s'en aperçoit au START suivant et recopie les
fichiers toute seule, en quelques secondes. Sur un Pluto d'origine, utilisez
Tezuka **v0.3.20** (fichier `pluto.frm` seul) : les versions 0.3.22 à 0.3.24
ne démarrent pas avec le bootloader d'ADI.
Si le son a des coupures sur un Pluto, vérifiez qu'il utilise ses deux cœurs
(`grep -c processor /proc/cpuinfo` doit répondre 2) : l'environnement U-Boot
d'ADI limite souvent à un seul cœur. Corrigez avec `maxcpus = 2` dans
`config.txt` (lecteur USB « PlutoSDR ») ou `fw_setenv maxcpus 2` en ssh, puis
redémarrez le Pluto.

## 2. Installer l'appli

Sur le téléphone, ouvrir ce lien. Il donne toujours la dernière version :

**https://github.com/F4HSL-Yves/Android-NB/releases/latest/download/qo100-nb.apk**

Ou scanner ce QR code :

![QR code : télécharger l'APK](img/qr-apk.png)

Ensuite :

1. Si Chrome prévient que le fichier peut être dangereux, choisir
   « Télécharger quand même ».
2. Ouvrir le fichier téléchargé.
3. La première fois, Android demande d'autoriser l'installation depuis Chrome.
   Toucher « Paramètres », activer « Autoriser cette source », puis revenir et
   toucher « Installer ».
4. Si Play Protect avertit d'une appli inconnue, choisir « Installer quand
   même ».

L'appli s'appelle **QO-100 NB**.

## 3. Premier démarrage

1. Ouvrir l'onglet **Réglages réception**. Dans « Connexion », saisir
   l'adresse de la carte, 192.168.1.125 par défaut. Laisser le port à 8073.
2. Revenir sur l'**Accueil** et appuyer sur **START**, en bas à gauche.
3. Android peut demander l'accès au **réseau local**. Il faut l'accepter, sinon
   la carte reste injoignable en Wi-Fi.

Au premier START, l'appli installe tout ce qu'il faut sur la carte, toute
seule. Le bouton affiche « Installation… » quelques secondes. L'appli se
connecte en ssh avec le mot de passe root par défaut de Tezuka (`analog`),
copie le programme `nbd` et ses scripts, puis ajoute le lancement au démarrage
de la carte.

Si le mot de passe root de la carte a été changé :

1. Dans **Réglages réception**, section « Installer sur la carte », saisir le
   mot de passe.
2. Appuyer une fois sur **Installer / mettre à jour**.

L'appli retient ce mot de passe pour les mises à jour suivantes.

Ensuite, le bouton passe à **« Ça démarre… »**, puis à **STOP** une fois la
réception établie. Le badge en haut passe à « connecté », et le badge balise
passe à « PSK ✓ » (ou le nom de la balise choisie) dès que la carte s'est calée sur la balise, en quelques
secondes en général.

À chaque mise sous tension, la carte démarre la radio libre : SDR Console ou
le DATV peuvent s'en servir sans l'appli. START reprend la bande étroite. Si
plus aucune appli n'est connectée pendant 2 minutes (téléphone éteint sans
STOP, par exemple), la carte libère la radio d'elle-même.

## 4. L'écran d'accueil

De haut en bas :

| Zone | Rôle |
| --- | --- |
| **?** | À côté du titre : ouvre ce mode d'emploi dans le navigateur. |
| **Cadenas** | En haut à droite. Un appui verrouille l'écran : seuls le PTT et le cadenas répondent, la barre d'onglets disparaît. Un appui long sur le cadenas déverrouille. Pratique en QSO, téléphone en main ou dans la poche. |
| **Badges** | Le badge balise donne la balise suivie (basse, PSK ou haute) et son état. « ✓ » : verrouillée (vert). « ~ » : perdue, la dernière correction est gardée (orange). « ? » : recherche en cours (rouge). Touchez-le pour choisir la balise de synchro : basse (CW, 10489,500), PSK (10489,750) ou haute (10490,000). Après un changement, la carte recherche la nouvelle balise pendant quelques secondes. Au START, la carte cherche d'abord la balise autour de la dernière correction LNB retenue : le verrouillage prend alors une ou deux secondes. À côté, l'état de la liaison : connecté, connexion… ou arrêté. |
| **Ligne de message** | Problème en cours, en rouge (MQTT injoignable, réception DATV en cours, erreur d'installation…). |
| **Spectre et waterfall** | Toute la bande ou un zoom. Le trait rouge marque la fréquence d'écoute, la zone claire la bande passante du filtre. |
| **Vue** | Bande entière, 100 k, 25 k ou 6 k. |
| **Fréquence** | Fréquence de réception en grand. En dessous, la fréquence d'émission correspondante (« TX … »), avec « (hors zone) » si l'émission n'y est pas permise. |
| **Pas** | Pas des boutons − et + : 10 Hz, 100 Hz, 1 kHz ou 10 kHz. |
| **RIT** | Décale l'écoute de ±3 kHz sans bouger l'émission. « 0 » le remet à zéro. |
| **Signal** | S-mètre : S1 à S9 en vert (6 dB par point S), puis S9+10 à S9+60 en rouge. À droite, le niveau brut de nbd en dB relatifs. Le niveau de S9 se règle dans Réglages réception. |
| **Muet TX, M1 à M3** | Coupure du son pendant l'émission, et trois mémoires. |
| **Gain TX** | Gain de sortie de la carte, de -50 à 0 dB. L'appli le retient et le remet sur la carte à chaque START et à chaque PTT. |
| **START / STOP et PTT** | En bas, sous le pouce. |

Les zones d'émission (Gain TX, PTT, Muet TX) n'apparaissent que si la carte est
configurée pour émettre (voir [section 12](#12-réglages-de-la-carte-nbconf)).

Tant que l'appli est au premier plan, l'écran reste allumé. Après un START,
la réception continue en arrière-plan ou écran éteint : une notification
« Réception en cours » le rappelle, STOP l'arrête. Seule l'émission est
coupée en arrière-plan.

## 5. Recevoir

### Changer de fréquence

- **Toucher un signal** dans le spectre ou le waterfall : le filtre se centre
  dessus.
- **Glisser à l'horizontale** sur le spectre : le curseur de fréquence suit le doigt.
- **Pincer** : zoome ou dézoome (jusqu'à 3 kHz de large).
- Boutons **−** et **+** : avancent du pas choisi.
- **Toucher la fréquence** affichée : ouvre « Aller à », pour saisir une
  fréquence en MHz (virgule ou point, par exemple `10489,650`).

Le zoom suit la fréquence d'écoute. La fréquence est toujours la vraie
fréquence de descente : la carte retire elle-même la dérive du LNB grâce à la
balise.

### Mémoires

- **Appui long** sur M1, M2 ou M3 : mémorise la fréquence affichée.
- **Appui court** : y retourne. La mémoire active est surlignée.

### La balise

La carte se cale en continu sur la **balise PSK centrale** (10489,750 MHz),
comme SDR Console. Toutes les fréquences affichées sont donc justes à quelques
hertz près, et identiques à celles des autres stations. L'onglet Réglages
réception, section « État », donne la correction LNB, la dérive et le rapport
signal/bruit de la balise.

Si la balise est perdue (pluie, pointage), la dernière correction est gardée
(« balise ~ »). La recherche reprend seule au bout de 15 s.

## 6. Émettre

> Avant la première émission, lancer une fois la **calibration** (voir
> [section 8](#8-réglages-émission)), sinon le signal peut tomber à côté de la
> fréquence affichée.

1. Se placer sur la fréquence à contacter. La ligne « TX … » sous la fréquence
   donne la fréquence de montée. L'émission est refusée hors du transpondeur
   (2400,005 à 2400,495 MHz) et autour de la balise PSK centrale.
2. Régler le **Gain TX** (curseur sur l'accueil). Commencer bas, puis monter
   jusqu'à un signal du même niveau que la balise PSK, sans la dépasser.
3. Appuyer sur **PTT**. Le bouton passe à « PTT… » pendant la mise en route,
   puis à **ÉMISSION** (rouge). La première fois, Android demande l'accès au
   micro : l'accepter, puis appuyer à nouveau.
4. Parler. Le waterfall montre son propre signal revenir du satellite, environ
   un quart de seconde plus tard. La réception continue pendant l'émission
   (full duplex).
5. Appuyer à nouveau sur le bouton pour repasser en réception.

Le PTT est un interrupteur : un appui pour émettre, un appui pour arrêter.

**Muet TX** coupe le son de la réception pendant l'émission. Il évite
l'effet Larsen avec le haut-parleur du téléphone. Sans casque, l'activer.

L'émission s'arrête d'elle-même si l'appli passe en arrière-plan (écran
éteint, autre appli). Android coupe d'ailleurs le micro dans ce cas.

## 7. Réglages réception

### Connexion

- **Carte** : adresse IP de la carte.
- **Port** : 8073, sauf réglage différent dans `nb.conf`.

### Installer sur la carte

- **Mot de passe root de la carte** : `analog` par défaut sur Tezuka.
- **Installer / mettre à jour** : copie `nbd` et ses scripts sur la carte. Ce
  n'est utile que si le mot de passe a changé ou pour forcer une
  réinstallation, puisque START le fait tout seul.
- **Oublier la clé** : à utiliser seulement si la carte a été réinstallée ou
  remplacée, et que l'appli refuse de s'y connecter parce que sa clé ssh a
  changé. La nouvelle clé est mémorisée à l'installation suivante.

### Réception

| Réglage | Choix | Conseil |
| --- | --- | --- |
| **Gain RX** | Auto, ou 0 à 70 dB | Auto laisse l'AGC de la carte régler le gain d'entrée. Décochez Auto pour régler le gain à la main, par exemple pour éviter la saturation si la bande est très forte. |
| **Filtre** | 1,8 k, 2,4 k, 2,7 k, 3,2 k | 2,4 k pour la BLU courante, 1,8 k si la bande est chargée. |
| **AGC** | Off, Lent, Moyen, Rapide | Moyen ou Lent en phonie. |
| **Bruit** | Off, NR1, NR2 | Réducteur de bruit. NR2 est plus fort, mais peut colorer la voix. |
| **Chute** | Off, 4/s, 8/s, 12/s | Vitesse du waterfall. Off coupe le spectre et économise les données. |
| **Spectre** | 30, 40, 50, 60 % | Part de la hauteur pour la courbe du spectre ; le waterfall prend le reste. |
| **Échelle** | 20, 30, 45, 65 dB | Hauteur de la courbe du spectre, en dB au-dessus du bruit. 65 dB : tout tient. 20 ou 30 dB : les raies faibles montent plus haut (les fortes sont écrêtées). Un trait léger marque chaque palier de 10 dB au-dessus du bruit. |
| **Débit** | 8k, 12k, 16k, 24k | Débit audio Opus. 16k convient partout, 8k en 4G faible. |
| **Tampon** | 80, 120, 200, 300 ms | Anti-coupures. Court en Wi-Fi, long en 4G. |
| **S-mètre S9 à** | -100 à +20 dB (-17 par défaut) | Le niveau de nbd est relatif : ce réglage fixe le niveau affiché S9. **Bruit = S2**, sur une fréquence sans signal, le place pour que le bruit s'affiche S2. |

### État

- Panneau balise : verrouillée ou non, correction LNB, dérive, SNR.
- Ligne de liaison : débit audio, remplissage du tampon, paquets perdus ou
  jetés, coupures, débit du waterfall.
- Ligne carte : source des IQ, source du spectre, plage de fréquences,
  nombre de clients connectés.

## 8. Réglages émission

Cet onglet n'apparaît que si la carte est configurée pour émettre.

### Émission

| Réglage | Rôle |
| --- | --- |
| **Micro** | Gain du micro, de -10 à +30 dB. |
| **Graves**, **Aigus** | Égaliseur, ±12 dB. Un peu moins de graves rend la BLU plus lisible. |
| **Compresseur** | Augmente la puissance moyenne. Utile en signal faible. |
| **Porteuse 1 kHz** | Remplace la voix par une porteuse à +1 kHz du cadran, au prochain PTT. Sert aux réglages. |
| **PTT par l'Arduino** | **Désactivé par défaut.** À activer seulement si un Arduino commande le PA (voir ci-dessous). |

Sans Arduino, le PTT ouvre et ferme simplement la sortie RF de la carte.

Avec l'Arduino activé, l'ordre est le même qu'avec Android-DATV : l'appli
demande `arduino/ptt on`, attend que l'Arduino confirme son PTT, puis
seulement ouvre la sortie RF. Sans confirmation en 3 secondes, l'émission est
annulée avec le message « PTT non confirmé par l'Arduino ». Si l'option est
activée alors qu'aucun Arduino n'est branché, le PTT ne passe donc jamais.

### Correction TX

L'oscillateur de la carte n'est pas parfait : le signal émis peut tomber
quelques centaines de hertz à côté. La calibration mesure cet écart et le
corrige.

1. Se placer sur une fréquence libre du transpondeur.
2. Régler un gain TX modéré.
3. Appuyer sur **Calibrer**. La carte émet une porteuse quelques secondes, la
   retrouve dans la réception, et calcule la correction.
4. Le résultat s'affiche : « Dernière mesure : porteuse reçue à … Hz,
   correction … Hz ».

La correction est enregistrée dans le téléphone. Elle peut aussi être saisie à
la main dans le champ « Correction (Hz) », puis **OK**. Refaire la calibration
si l'émission dérive, par exemple après un changement de saison.

### Informations

Sous la calibration : indicatif de la carte, état de la sortie RF, PTT de
l'Arduino, température du PA. Pendant l'émission s'y ajoutent le niveau micro,
l'ALC, le niveau de sortie et la durée d'émission.

## 9. Partager la carte avec le DATV et SDR Console

Un seul logiciel peut utiliser le récepteur de la carte à la fois.

- **DATV (Webtioune, Android-DATV)** : rien à faire. Un démarrage DATV prend
  la radio, et l'appli affiche « Réception DATV en cours ». Quand le DATV
  s'arrête, la carte repasse seule en bande étroite, et l'appli se reconnecte.
  Sans appli connectée, la carte libère de nouveau la radio au bout de
  2 minutes.
- **SDR Console** : il ne prévient pas la carte. Après une mise sous tension,
  la radio est libre : SDR Console marche directement. Si l'appli vient de
  servir, appuyer d'abord sur **STOP** (ou attendre 2 minutes sans appli
  connectée), puis lancer SDR Console. Pour reprendre la bande étroite,
  quitter SDR Console et appuyer sur **START**.
- Si SDR Console donne un son haché ou un spectre décalé, la bande étroite
  tient encore la radio : appuyer sur START puis STOP dans l'appli, ou publier
  `datv` sur le topic MQTT `cmd/nb/mode`, puis reconnecter SDR Console.

## 10. Trafic à distance (4G, 5G)

L'appli doit pouvoir joindre la carte : en pratique, par un VPN vers le réseau
de la maison (WireGuard, Tailscale, box…). Saisir alors l'adresse de la carte
vue à travers le VPN.

Pour limiter les données et les coupures :

- **Tampon** : 200 ou 300 ms ;
- **Débit** : 12k ou 8k si la liaison est faible ;
- **Chute** : 4/s, ou Off.

Compter environ 150 kbit/s avec le waterfall, 20 kbit/s sans.

## 11. Mettre à jour

**La carte** se met à jour toute seule : à chaque START, l'appli regarde sur
GitHub s'il existe une version plus récente du programme de la carte, la
télécharge et l'installe (quelques secondes). Sans internet, elle garde la
version déjà sur le téléphone.

**L'appli** elle-même : quand une version plus récente est publiée, un
bandeau en haut de l'écran le signale après un START ; le toucher ouvre la
page de téléchargement. Pour l'installer :

1. Ouvrir à nouveau le lien d'installation (ou le QR code).
2. Installer l'APK par-dessus l'ancien, sans désinstaller.

Les réglages de l'appli et de la carte sont conservés.

## 12. Réglages de la carte (nb.conf)

Les réglages de la carte sont dans `/boot/nb/nb.conf`, sur la carte SD
(`/mnt/jffs2/nb/nb.conf` sur un Pluto sans carte SD). Les
mises à jour ne remplacent pas ce fichier. Pour le modifier, se connecter en
ssh (`ssh root@192.168.1.125`) et l'éditer, puis redémarrer la carte.

- Retirer `--tx` pour une carte en **réception seule** : les commandes
  d'émission disparaissent de l'appli.
- Retirer `--beacon 10489750000` pour se caler sur la **balise CW basse**
  (10489,500 MHz) au lieu de la balise PSK centrale.

Le journal de `nbd` est dans `/tmp/nbd.log` sur la carte.

## 13. Sécurités d'émission

La carte coupe l'émission d'elle-même :

- si le téléphone se déconnecte ;
- si le flux micro s'arrête plus d'une seconde ;
- si la fréquence sort du transpondeur ;
- après 10 minutes d'émission continue.

L'appli coupe aussi l'émission quand elle passe en arrière-plan. Avec
l'Arduino, la RF ne s'ouvre qu'après la confirmation de son PTT, et se coupe
avant lui au relâchement.

## 14. Problèmes courants

| Symptôme | Cause et solution |
| --- | --- |
| « Android refuse l'accès au réseau local » | Autoriser « Réseau local » pour QO-100 NB dans les réglages Android de l'appli. |
| « MQTT de la carte injoignable (port 1883) » | Vérifier l'adresse de la carte et que le téléphone est sur le même réseau (ou le VPN). |
| L'installation échoue sur le mot de passe | Saisir le bon mot de passe root dans Réglages réception, puis Installer / mettre à jour. |
| L'installation refuse la clé ssh | La carte a été réinstallée : « Oublier la clé », puis installer. Si la carte n'a pas changé, ne pas le faire, et vérifier qu'on parle bien à la bonne carte. |
| « Réception DATV en cours » | Un logiciel DATV utilise la carte. La bande étroite reprend à son arrêt. |
| « balise ? » qui reste rouge | Vérifier le pointage et le LNB. Avec un LNB dont l'OL n'est pas à 9750 MHz, ajouter `--lnb-lo` dans `nb.conf`. |
| Balise « ? » ou stations décalées avec la balise basse ou haute | La carte cherche ces balises à ±25 kHz seulement. Si le LNB est décalé de plus de 25 kHz, choisir la balise PSK (cherchée à ±100 kHz) en touchant le badge balise. |
| PTT grisé | Pas connecté, MQTT injoignable, ou fréquence hors zone d'émission (voir la ligne « TX … »). |
| « PTT non confirmé par l'Arduino » | L'option « PTT par l'Arduino » est activée sans Arduino, ou il ne répond pas. La désactiver s'il n'y en a pas. |
| « Micro refusé » | Autoriser le micro pour QO-100 NB dans les réglages Android. |
| « Calibration échouée : porteuse non reçue » | Gain TX trop bas, balise non verrouillée, ou PA éteint. |
| Son haché | Augmenter le tampon, baisser le débit ou la vitesse du waterfall. |
| SDR Console ne reçoit rien, son haché ou spectre décalé | La bande étroite tient encore la radio. Appuyer sur STOP dans l'appli (ou publier `datv` sur `cmd/nb/mode`), puis reconnecter SDR Console. |
