# /mnt/jffs2/autorun.sh, sourced by /etc/init.d/S98autostart at boot.
# QO-100 NB: restore the DATV/NB mode and listen on MQTT (files on the SD).
grep -q " /boot " /proc/mounts || for p in /dev/mmcblk0p1 /dev/mmcblk0 /dev/mmcblk1p1; do [ -b $p ] && mkdir -p /boot && mount -t vfat $p /boot && break; done  # QO-100 NB: SD on /boot
[ -f /boot/nb/nbctl.sh ] && sh /boot/nb/nbctl.sh boot
