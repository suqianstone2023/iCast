#!/bin/sh
# icast-e2.sh - run icast-libusb as a background IPTV source on an Enigma2 box
#
# The dongle provides exactly one transport stream at a time, so this script
# pins one frequency and publishes it as a local UDP stream that Enigma2 can
# play as a 4097 (IPTV) service. Changing frequency means restarting it.
#
#   ./icast-e2.sh start -f 474 -p 5000 [--dvbc -r 6875 -m 64]
#   ./icast-e2.sh stop
#   ./icast-e2.sh status
#   ./icast-e2.sh bouquet -p 5000 -n "DTMB 474"
#
# Exit codes: 0 ok, 1 usage, 2 already running, 3 not running, 4 start failed.

set -eu

BIN=${BIN:-/usr/bin/icast-libusb}
PIDFILE=${PIDFILE:-/var/run/icast-libusb.pid}
LOGFILE=${LOGFILE:-/tmp/icast-libusb.log}

FREQ=474
PORT=5000
EXTRA=""

die() { echo "icast-e2: $1" >&2; exit "${2:-1}"; }

running() {
	[ -f "$PIDFILE" ] || return 1
	pid=$(cat "$PIDFILE" 2>/dev/null) || return 1
	[ -n "$pid" ] || return 1
	kill -0 "$pid" 2>/dev/null
}

cmd_start() {
	running && die "already running (pid $(cat "$PIDFILE")), use stop first" 2
	[ -x "$BIN" ] || die "$BIN not found or not executable" 4

	# Detach from the shell so the stream survives logout. busybox on E2 has
	# no setsid, so plain background + disown-free nohup is what we rely on.
	nohup "$BIN" stream -f "$FREQ" -o "udp://127.0.0.1:$PORT" $EXTRA \
		> "$LOGFILE" 2>&1 &
	pid=$!
	echo "$pid" > "$PIDFILE"

	# Give the init sequence a moment, then check it did not die on probe.
	sleep 3
	if ! kill -0 "$pid" 2>/dev/null; then
		rm -f "$PIDFILE"
		echo "--- $LOGFILE ---" >&2
		tail -n 20 "$LOGFILE" >&2 || true
		die "process exited during startup" 4
	fi

	echo "streaming ${FREQ}MHz -> udp://127.0.0.1:$PORT (pid $pid)"
	echo "log: $LOGFILE"
}

cmd_stop() {
	running || die "not running" 3
	pid=$(cat "$PIDFILE")
	kill "$pid" 2>/dev/null || true
	i=0
	while kill -0 "$pid" 2>/dev/null && [ $i -lt 10 ]; do
		sleep 1
		i=$((i + 1))
	done
	kill -9 "$pid" 2>/dev/null || true
	rm -f "$PIDFILE"
	echo "stopped"
}

cmd_status() {
	if running; then
		pid=$(cat "$PIDFILE")
		echo "running (pid $pid)"
		# The state line the tool prints sits at the end of the log once tuned.
		grep -aE "lock|signal|snr" "$LOGFILE" 2>/dev/null | tail -n 3 || true
		return 0
	fi
	echo "not running"
	return 3
}

# Enigma2 stores IPTV services in a userbouquet file; ':' inside the URL has
# to be percent-encoded or the parser splits the wrong field.
cmd_bouquet() {
	url="udp%3a//127.0.0.1%3a$PORT"
	printf '#SERVICE 4097:0:1:0:0:0:0:0:0:0:%s:%s\n' "$url" "$NAME"
	printf '#DESCRIPTION %s\n' "$NAME"
}

NAME="iCast"

while [ $# -gt 0 ]; do
	case $1 in
	start|stop|status|bouquet) CMD=$1; shift ;;
	-f) FREQ=$2; shift 2 ;;
	-p) PORT=$2; shift 2 ;;
	-n) NAME=$2; shift 2 ;;
	-C|--dvbc) EXTRA="$EXTRA --dvbc"; shift ;;
	-r) EXTRA="$EXTRA -r $2"; shift 2 ;;
	-m) EXTRA="$EXTRA -m $2"; shift 2 ;;
	-b) EXTRA="$EXTRA -b $2"; shift 2 ;;
	-h|--help) sed -n '2,15p' "$0"; exit 0 ;;
	*) die "unknown argument: $1" ;;
	esac
done

[ -n "${CMD:-}" ] || die "need one of: start | stop | status | bouquet"
"cmd_$CMD"
