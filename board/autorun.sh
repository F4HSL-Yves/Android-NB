# /mnt/jffs2/autorun.sh, sourced by /etc/init.d/S98autostart at boot.
# QO-100 NB: restore the DATV/NB mode and listen on MQTT (files on the SD).
[ -f /boot/nb/nbctl.sh ] && sh /boot/nb/nbctl.sh boot
