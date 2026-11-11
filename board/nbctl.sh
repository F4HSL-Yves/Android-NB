#!/bin/sh
# QO-100 NB on a Tezuka board (FishBall 7020 / Pluto): switches the radio
# between DATV reception (zynqrx) and the narrowband receiver (nbd).
# Only one of them can own the AD9363 at a time. zynqrx stays running, as
# the firmware starts it; it only holds the radio between its own MQTT
# "process start" and "process stop", so the switch pauses it instead of
# killing it (tested: an idle zynqrx leaves nbd alone).
#
#   nbctl.sh nb | datv     switch now (and remember it for the next boot)
#   nbctl.sh status        print the current mode
#   nbctl.sh boot          at boot: start in nb mode (zynqrx idle), then
#                          listen to MQTT (run from /mnt/jffs2/autorun.sh)
#
# MQTT (broker on the board, 127.0.0.1:1883):
#   cmd/nb/mode   "nb" or "datv"        switch
#   state/nb/mode "nb" or "datv"        current mode (retained)
# NB is the default mode. A DATV start from any client (Webtioune,
# Android-DATV...: dt/zynqrx/process = start) stops nbd and switches to
# datv, so the two never fight; the matching stop switches back to nb.
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
BOOT_MODE=nb
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

ensure_zynqrx() {  # running but idle, as /etc/init.d/S97datv leaves it
    pidof zynqrx >/dev/null 2>&1 || AUTOSTART=0 /usr/bin/start_zynqrx >/dev/null 2>&1
}

pause_zynqrx() {  # zynqrx gives the radio back on "process stop"
    pidof zynqrx >/dev/null 2>&1 || return 0
    mosquitto_pub -h 127.0.0.1 -t cmd/zynqrx/process -m stop 2>/dev/null
    sleep 2
}

start_nb() {
    # Read nb.conf again: the boot listener runs for days and must follow
    # changes made since (NBD_ARGS gaining --tx, for instance).
    [ -f "$NB_DIR/nb.conf" ] && . "$NB_DIR/nb.conf"
    pause_zynqrx
    stop_nbd
    cp "$NB_DIR/nbd" /tmp/nbd && chmod 755 /tmp/nbd || { log "cannot copy $NB_DIR/nbd"; return 1; }
    # The flag holds this start's token: a restart loop left over from an
    # earlier start (asleep between two runs while stop_nbd removed the flag)
    # sees another token and ends instead of starting a second nbd.
    token="$$.$(date +%s)"
    echo "$token" >"$RUN_FLAG"
    # Restarted if it ever exits, until the mode changes. Status lines are
    # off (--quiet): the log lives in RAM and the app shows the status.
    (
        mine() { [ "$(cat "$RUN_FLAG" 2>/dev/null)" = "$token" ]; }
        while mine; do
            echo "--- nbd start $(date)" >>"$LOG"
            /tmp/nbd $NBD_ARGS --wisdom "$NB_DIR/" --quiet >>"$LOG" 2>&1
            mine && sleep 3
            # Keep the log small.
            [ "$(wc -c <"$LOG")" -gt 200000 ] && tail -c 50000 "$LOG" >"$LOG.1" && mv "$LOG.1" "$LOG"
        done
    ) </dev/null >/dev/null 2>&1 &
    log "mode nb (nbd $NBD_ARGS)"
}

start_datv() {
    stop_nbd
    # zynqrx waits for its own start (Dashboard, Webtioune page).
    ensure_zynqrx
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
    if [ -f "$RUN_FLAG" ]; then echo nb
    elif pidof zynqrx >/dev/null 2>&1; then echo datv
    else echo none
    fi
}

case "$1" in
nb | datv) switch "$1" ;;
status) current ;;
boot)
    # S97datv has already started zynqrx, idle: NB takes the radio.
    if [ "$BOOT_MODE" = datv ]; then switch datv; else switch nb; fi
    # Listen for mode changes and for DATV starts, and come back if
    # mosquitto restarts.
    (
        while true; do
            # zynqrx's state now, so its next repeat is not taken for a change.
            last_proc=$(mosquitto_sub -h 127.0.0.1 -t dt/zynqrx/process -C 1 -W 3 2>/dev/null)
            mosquitto_sub -h 127.0.0.1 -i nbctl -v -t cmd/nb/mode -t dt/zynqrx/process 2>/dev/null |
                while read -r topic m; do
                    case "$topic" in
                    cmd/nb/mode)
                        if [ "$m" = "$(current)" ]; then pub state/nb/mode "$m"; else switch "$m"; fi
                        ;;
                    dt/zynqrx/process)
                        # zynqrx repeats its state every second: act only on
                        # a change, so a manual cmd/nb/mode datv holds. Our own
                        # pause_zynqrx "stop" arrives once the mode is already
                        # nb, so it never switches twice.
                        [ "$m" = "$last_proc" ] && continue
                        last_proc=$m
                        if [ "$m" = start ] && [ "$(current)" = nb ]; then
                            log "DATV reception started: leaving nb"
                            switch datv
                        elif [ "$m" = stop ] && [ "$(current)" = datv ]; then
                            log "DATV reception stopped: back to nb"
                            switch nb
                        fi
                        ;;
                    esac
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
