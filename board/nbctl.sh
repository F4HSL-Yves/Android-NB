#!/bin/sh
# QO-100 NB on a Tezuka board (FishBall 7020 / Pluto): switches the radio
# between DATV reception (zynqrx) and the narrowband receiver (nbd).
# Only one of them can own the AD9363 at a time.
#
#   nbctl.sh nb | datv     switch now (and remember it for the next boot)
#   nbctl.sh status        print the current mode
#   nbctl.sh boot          at boot: restore the remembered mode, then listen
#                          to MQTT (run from /mnt/jffs2/autorun.sh)
#
# MQTT (broker on the board, 127.0.0.1:1883):
#   cmd/nb/mode   "nb" or "datv"        switch
#   state/nb/mode "nb" or "datv"        current mode (retained)
#
# Files live on the SD card (the root filesystem is in RAM and jffs2 is
# almost full): /boot/nb/nbd, nb.conf, this script, the FFTW wisdom and the
# remembered mode. nbd is copied to /tmp to run, so a vfat mount option
# cannot stop it from executing.

NB_DIR=${NB_DIR:-/boot/nb}
MODE_FILE=$NB_DIR/mode
RUN_FLAG=/tmp/nb.run
LOG=/tmp/nbd.log
NBD_ARGS="--source pluto --ws 8073"
DEFAULT_MODE=datv
[ -f "$NB_DIR/nb.conf" ] && . "$NB_DIR/nb.conf"

log() { echo "nbctl: $*"; logger -t nbctl "$*" 2>/dev/null; }
pub() { mosquitto_pub -h 127.0.0.1 -r -t "$1" -m "$2" 2>/dev/null; }

wait_gone() {  # name, seconds
    i=0
    while pidof "$1" >/dev/null 2>&1 && [ $i -lt "$2" ]; do sleep 1; i=$((i + 1)); done
    pidof "$1" >/dev/null 2>&1 && killall -9 "$1" 2>/dev/null
    return 0
}

stop_nbd() {
    rm -f "$RUN_FLAG"
    killall nbd 2>/dev/null  # SIGTERM: nbd hands the DMA back to the AD9363
    wait_gone nbd 10
}

stop_zynqrx() {
    killall zynqrx 2>/dev/null
    wait_gone zynqrx 10
}

start_nb() {
    stop_zynqrx
    stop_nbd
    cp "$NB_DIR/nbd" /tmp/nbd && chmod 755 /tmp/nbd || { log "cannot copy $NB_DIR/nbd"; return 1; }
    touch "$RUN_FLAG"
    # Restarted if it ever exits, until the mode changes. Status lines are
    # off (--quiet): the log lives in RAM and the app shows the status.
    (
        while [ -f "$RUN_FLAG" ]; do
            echo "--- nbd start $(date)" >>"$LOG"
            /tmp/nbd $NBD_ARGS --wisdom "$NB_DIR/" --quiet >>"$LOG" 2>&1
            [ -f "$RUN_FLAG" ] && sleep 3
            # Keep the log small.
            [ "$(wc -c <"$LOG")" -gt 200000 ] && tail -c 50000 "$LOG" >"$LOG.1" && mv "$LOG.1" "$LOG"
        done
    ) </dev/null >/dev/null 2>&1 &
    log "mode nb (nbd $NBD_ARGS)"
}

start_datv() {
    stop_nbd
    # Same as /etc/init.d/S97datv: zynqrx waits for its own MQTT start command.
    AUTOSTART=0 /usr/bin/start_zynqrx >/dev/null 2>&1
    log "mode datv (zynqrx)"
}

switch() {
    case "$1" in
    nb) start_nb ;;
    datv) start_datv ;;
    *) log "unknown mode '$1' (nb or datv)"; return 1 ;;
    esac
    echo "$1" >"$MODE_FILE"
    sync
    pub state/nb/mode "$1"
}

current() {
    if [ -f "$RUN_FLAG" ] && pidof nbd >/dev/null 2>&1; then echo nb
    elif pidof zynqrx >/dev/null 2>&1; then echo datv
    else echo none
    fi
}

case "$1" in
nb | datv) switch "$1" ;;
status) current ;;
boot)
    mode=$(cat "$MODE_FILE" 2>/dev/null)
    [ -n "$mode" ] || mode=$DEFAULT_MODE
    # S97datv has already started zynqrx: nothing to do for DATV.
    if [ "$mode" = nb ]; then switch nb; else pub state/nb/mode datv; fi
    # Listen for mode changes, and come back if mosquitto restarts.
    (
        while true; do
            mosquitto_sub -h 127.0.0.1 -i nbctl -t cmd/nb/mode 2>/dev/null | while read -r m; do
                [ "$m" = "$(current)" ] && { pub state/nb/mode "$m"; continue; }
                switch "$m"
            done
            sleep 5
        done
    ) </dev/null >/dev/null 2>&1 &
    ;;
*)
    echo "usage: $0 nb|datv|status|boot" >&2
    exit 2
    ;;
esac
