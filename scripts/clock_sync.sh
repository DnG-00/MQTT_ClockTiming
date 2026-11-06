#!/usr/bin/env bash
# The two Pis' clocks, kept together by chrony, so that a leg that starts on one and ends on the other (the gateway Pi's
# frame sent -> the broker Pi's message in) can be measured; and the offset between them logged with every run, so a
# leg is only reported when it is well above the clocks' error. The broker Pi is the reference (every other leg is
# timed on its clock already); the gateway Pi follows it alone. Durations on one machine (seal, open, the Pico's round
# trip, the broker's own legs) need none of this.
#
#   scripts/clock_sync.sh server [<subnet>]     broker Pi: chrony serves time to <subnet> (default: this Pi's /24)
#   scripts/clock_sync.sh client <broker ip>    gateway Pi: chrony follows the broker only, polled every 1-4 s
#   scripts/clock_sync.sh status                both: the offset now (chronyc tracking) and the sources
#   scripts/clock_sync.sh log start | stop      both: CLOCK_LOG (default results/clock_offset.csv), a row every
#                                               CLOCK_EVERY s (5); broker.sh and app.sh start and stop it
#   scripts/clock_sync.sh undo                  chrony's own configuration back (the copy server / client kept)
#
# server and client install chrony if missing (apt; it replaces systemd-timesyncd) and rewrite /etc/chrony/chrony.conf
# (the original kept once, as chrony.conf.iot-pqc-orig): sudo, asked for once. Both: makestep 1 3, so the clock is
# stepped only in chrony's first three updates (at boot) and slewed after, never jumping in the middle of a run.
# UPSTREAM (server): the broker Pi's own source, default pool.ntp.org; UPSTREAM=none for a lab without internet (the
# broker Pi's clock then is the lab's time, "local stratum 8", which is all the legs need: the two Pis agreeing).
# Wired Ethernet between the two Pis: tens of us; Wi-Fi's jitter is milliseconds, the size of the legs themselves.
#
# The log, one row per CLOCK_EVERY s, from chronyc -c tracking:
#   unix_s,host,source,stratum,offset_s,rms_offset_s,root_delay_s,root_dispersion_s,err_bound_s,leap
# offset_s: this clock minus the source's, now (positive: this clock is ahead); err_bound_s: chrony's bound on this
# clock's error, |offset| + root dispersion + root delay / 2. The gateway Pi's file is the one for the legs: report a
# gateway <-> broker leg only when it is at least 10x the err_bound_s around it.
set -euo pipefail
cd "$(dirname "$0")/.."
CONF=${CHRONY_CONF:-/etc/chrony/chrony.conf} ORIG=${CHRONY_CONF:-/etc/chrony/chrony.conf}.iot-pqc-orig
LOG=${CLOCK_LOG:-results/clock_offset.csv} EVERY=${CLOCK_EVERY:-5}
PID=${LOG%.csv}.pid
SUDO=$([ "$(id -u)" = 0 ] && echo "" || echo sudo)
COLS=unix_s,host,source,stratum,offset_s,rms_offset_s,root_delay_s,root_dispersion_s,err_bound_s,leap

install() {  # chrony, and its configuration: the original kept once
    command -v chronyd >/dev/null || { $SUDO apt-get update -qq && $SUDO apt-get install -y -qq chrony; }
    [ -e "$ORIG" ] || [ ! -e "$CONF" ] || $SUDO cp -p "$CONF" "$ORIG"
}
write_conf() {  # stdin -> $CONF, then chrony restarted on it
    $SUDO tee "$CONF" > /dev/null
    [ -n "${CHRONY_CONF:-}" ] && return 0  # a test configuration: nothing to restart
    $SUDO systemctl restart chrony 2>/dev/null || $SUDO systemctl restart chronyd
}
common() {  # both roles
    printf '%s\n' "driftfile /var/lib/chrony/chrony.drift" "rtcsync" "makestep 1 3" "leapsectz right/UTC"
}
running() { [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; }
my_subnet() {  # this Pi's /24, from the address it reaches the network with
    ip -4 -o route get 1.1.1.1 2>/dev/null | tr ' ' '\n' | grep -A1 -x src | tail -1 |
        awk -F. 'NF == 4 {print $1 "." $2 "." $3 ".0/24"}'
}
row() {  # one log row: chronyc -c tracking's fields 2 (the source), 3, 5 (system time: the source minus this clock),
    # 7, 11, 12, 14 -> this clock minus the source's, and chrony's error bound
    chronyc -c tracking 2>/dev/null | awk -F, -v t="$(date +%s.%N)" -v h="$(hostname)" '
        NF >= 14 {o = -$5; a = o < 0 ? -o : o
                  printf "%s,%s,%s,%s,%.9f,%.9f,%.9f,%.9f,%.9f,%s\n", t, h, $2, $3, o, $7, $11, $12, a + $12 + $11 / 2, $14}'
}

case "${1:-}" in
    server)
        NET=${2:-$(my_subnet)}
        [ -n "$NET" ] || { echo "no subnet: scripts/clock_sync.sh server <subnet, e.g. 192.168.0.0/24>"; exit 1; }
        install
        { echo "# scripts/clock_sync.sh server: this Pi is the lab's reference (the original: $ORIG)"
          [ "${UPSTREAM:-pool.ntp.org}" = none ] || echo "pool ${UPSTREAM:-pool.ntp.org} iburst"
          echo "local stratum 8"   # serves even with no upstream reachable
          echo "allow $NET"
          common; } | write_conf
        echo "chrony serving $NET$([ "${UPSTREAM:-}" = none ] && echo " (no upstream: this clock is the lab's time)")"
        echo "next, on the gateway Pi: scripts/clock_sync.sh client <this Pi's ip>" ;;
    client)
        S=${2:?"scripts/clock_sync.sh client <the broker Pi ip>"}
        install
        { echo "# scripts/clock_sync.sh client: the broker Pi only (the original: $ORIG)"
          echo "server $S iburst minpoll 0 maxpoll 2"   # every 1-4 s: tight on a LAN, a few packets a second at most
          common; } | write_conf
        [ -n "${CHRONY_CONF:-}" ] && exit 0
        # synchronised within 60 s, the correction under 1 ms: else say so (the broker's chrony, its allow line, UDP 123)
        if chronyc waitsync 12 0.001 0 5 > /dev/null; then echo "following $S:"; chronyc tracking | sed -n '1p;4,6p'
        else echo "[!] not synchronised to $S after 60 s: chronyc sources -v; the broker Pi's 'allow', UDP 123"; exit 1; fi ;;
    status)
        chronyc tracking; echo; chronyc sources -v ;;
    log)
        case "${2:-}" in
            start)
                command -v chronyc >/dev/null || { echo "clock log: no chronyc (scripts/clock_sync.sh server | client)"; exit 0; }
                running && exit 0
                mkdir -p "$(dirname "$LOG")"
                [ -s "$LOG" ] || echo "$COLS" > "$LOG"
                # setsid: its own session, as broker.sh's broker (closing the SSH terminal doesn't stop it)
                $(command -v setsid || true) nohup bash -c 'while :; do bash "$0" log row >> "$1"; sleep "$2"; done' \
                    "$0" "$LOG" "$EVERY" > /dev/null 2>&1 < /dev/null &
                echo $! > "$PID"
                echo "clock log: $LOG every ${EVERY} s (pid $!)" ;;
            stop) if running; then kill "$(cat "$PID")"; rm -f "$PID"; echo "clock log stopped"; fi ;;
            row) row ;;
            *) echo "usage: $0 log start | stop"; exit 1 ;;
        esac ;;
    undo)
        [ -e "$ORIG" ] || { echo "nothing to undo: no $ORIG"; exit 1; }
        $SUDO cp -p "$ORIG" "$CONF" && $SUDO rm -f "$ORIG"
        [ -n "${CHRONY_CONF:-}" ] || $SUDO systemctl restart chrony 2>/dev/null || $SUDO systemctl restart chronyd
        echo "chrony's own configuration is back" ;;
    *) echo "usage: $0 server [<subnet>] | client <broker ip> | status | log start | stop | undo"; exit 1 ;;
esac
