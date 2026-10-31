#!/bin/sh
# icast-boot.sh - delayed bring-up of the SMIT iCast USB tuner (DVB-C).
#
# Started DETACHED by /etc/rcS.d/S99icast-boot, which only does
# `setsid ... & exit 0`, so this can NEVER block the boot sequence
# (/etc/init.d/rc runs rcS.d scripts in the foreground in order).
#
# Two hardware facts drive the design:
#   1. The dongle accepts only ONE init handshake per power-up.  A reboot
#      of this box does NOT cut USB power, so the firmware keeps its
#      "already initialised" state and the first insmod after a reboot
#      times out (-110).  Only a physical unplug/replug clears that.
#   2. enigma2 enumerates tuners only at startup, so it must be restarted
#      once the frontend actually exists.
#
# So: load -> verify with a real tune+stream test -> restart enigma2.
# If the self-test fails, park in a monitor loop and recover the moment
# the stick is replugged.

LOG=/var/volatile/log/icast-boot.log
GUARD=/var/run/icast-boot.lock
MODDIR=/lib/modules/$(uname -r)/extra
PROBE=/usr/bin/dvbprobe
TPROBE=/usr/bin/dvbtprobe
DVID=29df
DPID=0001

# Mode comes from /etc/icast.mode so `icast-mode dtmb` survives a reboot.
MODE=$(cat /etc/icast.mode 2>/dev/null)
case "$MODE" in
	dvbc|dtmb) ;;
	*) MODE=dvbc ;;
esac

TV_WAIT=300       # seconds to wait for enigma2
SETTLE=20         # extra seconds after enigma2 appears
DONGLE_WAIT=120   # seconds to wait for the stick at boot
MONITOR=900       # seconds to watch for a replug afterwards

log() {
	echo "$(date '+%Y-%m-%d %H:%M:%S') [$$] $*" >> "$LOG"
}

e2_running() {
	ps 2>/dev/null | grep -q "[e]nigma2"
}

# sysfs path of our dongle (empty when unplugged).  lsusb is avoided: it
# talks to the device and this box's USB stack is fragile.
dongle_dir() {
	for d in /sys/bus/usb/devices/*/idVendor; do
		[ -r "$d" ] || continue
		if [ "$(cat "$d" 2>/dev/null)" = "$DVID" ]; then
			if [ "$(cat "$(dirname "$d")/idProduct" 2>/dev/null)" = "$DPID" ]; then
				dirname "$d"
			fi
		fi
	done | head -1
}

frontend_present() {
	[ -e /dev/dvb/adapter1/frontend0 ]
}

# 0 = usable, 1 = broken, 2 = cannot tell
#
# In dvbc mode the cable mux on 251 MHz is a real signal, so "locked and
# streaming" is the pass criterion.  In dtmb mode there is usually no
# aerial attached, so nothing can ever lock; all we can prove is that the
# frontend exists and that the driver accepts a DVB-T tune instead of
# returning EINVAL.  Treating "no lock" as failure there would park the
# helper in its monitor loop forever and enigma2 would never be restarted.
tuner_ok() {
	frontend_present || return 1
	if [ "$MODE" = "dtmb" ]; then
		[ -x "$TPROBE" ] || return 2
		out=$(timeout 30 "$TPROBE" /dev/dvb/adapter1 474000000 0 2>&1)
		echo "$out" >> "$LOG"
		case "$out" in
			*busy*) return 2 ;;
			*"FE_SET_PROPERTY failed"*) return 1 ;;
			*"tune ioctl"*) return 0 ;;
			*) return 1 ;;
		esac
	fi
	[ -x "$PROBE" ] || return 2
	out=$(timeout 30 "$PROBE" /dev/dvb/adapter1 251000000 6875000 3 0 2>&1)
	echo "$out" >> "$LOG"
	case "$out" in
		*busy*) return 2 ;;
		*"0.00 Mbit/s"*) return 1 ;;
		*"Mbit/s"*) return 0 ;;
		*) return 1 ;;
	esac
}

load_driver() {
	if lsmod 2>/dev/null | grep -q dvb_usb_icast_smit; then
		log "driver already loaded"
		return 0
	fi
	for f in dvb-usb-icast-fe.ko dvb-usb-icast-smit.ko; do
		[ -f "$MODDIR/$f" ] || { log "missing $MODDIR/$f"; return 1; }
	done
	log "insmod dvb-usb-icast-fe.ko"
	insmod "$MODDIR/dvb-usb-icast-fe.ko" >> "$LOG" 2>&1 || log "WARN: fe insmod error"
	sleep 1
	log "insmod dvb-usb-icast-smit.ko icast_mode=$MODE"
	insmod "$MODDIR/dvb-usb-icast-smit.ko" icast_mode="$MODE" >> "$LOG" 2>&1 \
		|| log "WARN: smit insmod error"
	i=0
	while [ $i -lt 45 ]; do
		frontend_present && return 0
		i=$((i + 1))
		sleep 1
	done
	return 1
}

restart_e2() {
	log "restarting enigma2 so it re-scans the frontends"
	killall enigma2 2>/dev/null
	# wait until the old instance is really gone, otherwise the respawn
	# check below would succeed against the dying process
	i=0
	while [ $i -lt 30 ]; do
		e2_running || break
		sleep 1
		i=$((i + 1))
	done
	i=0
	while [ $i -lt 90 ]; do
		e2_running && break
		sleep 2
		i=$((i + 2))
	done
	if e2_running; then
		log "enigma2 back up after ~${i}s"
	else
		log "WARN: enigma2 did not respawn - check /etc/inittab"
	fi
}

# ---------------------------------------------------------------- main --
[ -d /var/run ] || mkdir -p /var/run
if [ -e "$GUARD" ]; then
	exit 0
fi
: > "$GUARD" 2>/dev/null || true

log "=== bring-up start (mode=$MODE, kernel=$(uname -r)) ==="

# 1. wait for enigma2 and let it finish its one-shot tuner scan
i=0
while [ $i -lt $TV_WAIT ]; do
	e2_running && break
	i=$((i + 2))
	sleep 2
done
if ! e2_running; then
	log "enigma2 never came up - giving up (use /usr/bin/icast-up.sh by hand)"
	exit 0
fi
log "enigma2 up (waited ${i}s), settling ${SETTLE}s"
sleep $SETTLE

# 2. wait for the stick to be plugged in at all
i=0
while [ $i -lt $DONGLE_WAIT ]; do
	[ -n "$(dongle_dir)" ] && break
	i=$((i + 5))
	sleep 5
done

WORKING=0
if [ -z "$(dongle_dir)" ]; then
	log "dongle not plugged in - will watch for it for ${MONITOR}s"
else
	# 3. load the driver and prove it works
	load_driver
	sleep 12            # let any -110 induced re-enumeration settle
	tuner_ok
	case $? in
		0) if [ "$MODE" = "dtmb" ]; then
			log "self-test PASSED (frontend up, DVB-T tune accepted)"
		   else
			log "self-test PASSED (locked, stream flowing)"
		   fi
		   WORKING=1 ;;
		2) log "self-test inconclusive (frontend busy or probe missing)"
		   frontend_present && WORKING=1 ;;
		*) log "self-test FAILED - no lock, no stream."
		   log "the stick did not lose USB power during the reboot, so its"
		   log "firmware refuses a new handshake. UNPLUG and REPLUG it." ;;
	esac
fi

# 4. monitor for a replug (also covers "was not plugged in at boot")
if [ "$WORKING" = "0" ]; then
	BASE=$(cat "$(dongle_dir)/devnum" 2>/dev/null)
	log "monitoring for replug (baseline devnum=${BASE:-none})"
	GONE=0
	i=0
	while [ $i -lt $MONITOR ]; do
		sleep 5
		i=$((i + 5))
		d=$(dongle_dir)
		if [ -z "$d" ]; then
			if [ "$GONE" = "0" ]; then
				log "dongle went away (unplugged)"
				GONE=1
			fi
			continue
		fi
		CUR=$(cat "$d/devnum" 2>/dev/null)
		if [ "$GONE" = "1" ] || { [ -n "$BASE" ] && [ "$CUR" != "$BASE" ]; }; then
			log "dongle (re)appeared: devnum $BASE -> $CUR"
			GONE=0
			BASE="$CUR"
			sleep 8        # firmware boot
			if ! frontend_present; then
				rmmod dvb_usb_icast_smit 2>/dev/null
				rmmod dvb_usb_icast_fe 2>/dev/null
				sleep 2
				load_driver
			fi
			tuner_ok
			case $? in
				0) log "self-test PASSED after replug"
				   WORKING=1; break ;;
				2) log "frontend present but busy - assuming usable"
				   WORKING=1; break ;;
				*) log "still no lock after replug - waiting for another one" ;;
			esac
		fi
	done
	if [ "$WORKING" = "0" ]; then
		log "no working tuner after ${MONITOR}s - giving up this boot"
		exit 0
	fi
fi

# 5. enigma2 only sees frontends that exist at its own startup
restart_e2
log "=== bring-up done ==="
exit 0
