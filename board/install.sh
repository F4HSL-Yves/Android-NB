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
set -e
NB_DIR=/boot/nb
AUTORUN=/mnt/jffs2/autorun.sh
LINE="[ -f $NB_DIR/nbctl.sh ] && sh $NB_DIR/nbctl.sh boot"

[ -x /usr/bin/mosquitto_sub ] || [ -x /usr/sbin/mosquitto ] || command -v mosquitto_sub >/dev/null ||
    { echo "install.sh: no MQTT broker on this board: a Tezuka firmware is needed"; exit 1; }
[ -f "$NB_DIR/nbd" ] && [ -f "$NB_DIR/nbctl.sh" ] && [ -f "$NB_DIR/nb.conf" ] ||
    { echo "install.sh: copy the nb/ folder to $NB_DIR first"; exit 1; }

if [ -f "$AUTORUN" ] && grep -qF "$NB_DIR/nbctl.sh boot" "$AUTORUN"; then
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
