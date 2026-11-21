#!/bin/sh
# QO-100 NB: one-time install on a Tezuka board (FishBall 7020 / Pluto).
#
# 1. Copy the nb/ folder of the release to the SD card, as /boot/nb
#    (card in a PC: the "boot" FAT partition; or scp -O -r nb root@CARD:/boot/).
# 2. On the board (ssh root@CARD):   sh /boot/nb/install.sh
#
# The Tezuka firmware itself is not modified: this only adds the start of
# nbctl.sh to /mnt/jffs2/autorun.sh (run at boot by S98autostart), keeping
# whatever that file already does, then starts NB right away.
#
# Board without an SD card (Pluto): the app copies the files to /tmp/nb
# instead, and runs this script from there. /tmp is in RAM, so nothing is
# added to autorun.sh: after a reboot, the app's next START copies them
# again. nb.conf and the remembered mode and LNB correction stay in
# /mnt/jffs2/nb (a few hundred bytes).
set -e
NB_DIR=${NB_DIR:-$(cd "$(dirname "$0")" && pwd)}
case "$NB_DIR" in /tmp/*) NB_STATE=/mnt/jffs2/nb ;; *) NB_STATE=$NB_DIR ;; esac
export NB_DIR NB_STATE
AUTORUN=/mnt/jffs2/autorun.sh
LINE="[ -f $NB_DIR/nbctl.sh ] && sh $NB_DIR/nbctl.sh boot"

[ -x /usr/bin/mosquitto_sub ] || [ -x /usr/sbin/mosquitto ] || command -v mosquitto_sub >/dev/null ||
    { echo "install.sh: no MQTT broker on this board: a Tezuka firmware is needed"; exit 1; }
mkdir -p "$NB_STATE"
# Manual install on a Pluto (nb/ copied to /tmp/nb by scp): keep the settings in the jffs2.
[ -f "$NB_STATE/nb.conf" ] || { [ -f "$NB_DIR/nb.conf" ] && cp "$NB_DIR/nb.conf" "$NB_STATE/nb.conf"; } || true
[ -f "$NB_DIR/nbd" ] && [ -f "$NB_DIR/nbctl.sh" ] && [ -f "$NB_STATE/nb.conf" ] ||
    { echo "install.sh: copy the nb/ folder to $NB_DIR first"; exit 1; }

if [ "$NB_STATE" != "$NB_DIR" ]; then
    echo "no SD card: QO-100 NB runs from $NB_DIR (RAM), settings in $NB_STATE; copied again by the app after a reboot"
elif [ -f "$AUTORUN" ] && grep -qF "$NB_DIR/nbctl.sh boot" "$AUTORUN"; then
    echo "autorun.sh already starts QO-100 NB"
else
    [ -f "$AUTORUN" ] && cp "$AUTORUN" "$NB_DIR/autorun.sh.bak"
    {
        echo ""
        echo "# QO-100 NB: restore the DATV/NB mode and listen on MQTT (files on the SD)."
        echo "$LINE"
    } >>"$AUTORUN"
    chmod +x "$AUTORUN"
    echo "added QO-100 NB to $AUTORUN (previous copy: $NB_DIR/autorun.sh.bak)"
fi

# Start now instead of waiting for the next boot, unless it already runs
# (a second boot would add a second MQTT listener).
if ps | grep -v grep | grep -q "nbctl.sh boot"; then
    sh "$NB_DIR/nbctl.sh" nb
else
    sh "$NB_DIR/nbctl.sh" boot
fi
sleep 3
sh "$NB_DIR/nbctl.sh" status
echo "QO-100 NB installed. App: board address $(ip -4 addr show 2>/dev/null | awk '/inet / && $2 !~ /^127/ {sub("/.*","",$2); print $2; exit}'), port 8073."
