#!/bin/sh
# Full-band cable scan wrapper for the iCast DVB-C dongle.
#
# Deliberate departures from E2's own arguments:
#
#  --sr is stripped.  E2 defaults to 6900 kS/s for cable; this headend
#  runs 6875 and the demodulator has no AFC to absorb the 25 kS/s
#  offset, so anything tuned at 6900 comes back empty.
#
#  --scan-stepsize is stripped.  E2 defaults to 1000 kHz steps, but the
#  dongle's lock window is +/-0: 251 MHz will NOT lock at 250 or 253.
#  8000 kHz is the CCIR cable grid and both known muxes (251/259) sit
#  on it.
#
#  --from/--to cover the whole DVB-C downlink band (115-862 MHz).
#  E2 never passes these, so they survive.
kept=
skip=0
for a in "$@"; do
	if [ "$skip" = 1 ]; then
		skip=0
		continue
	fi
	if [ "$a" = "--sr" ] || [ "$a" = "--scan-stepsize" ]; then
		skip=1
		continue
	fi
	kept="$kept $a"
done

# shellcheck disable=SC2086
exec /usr/bin/tda1002x.v6 \
	--from 115000 --to 862000 --scan-stepsize 8000 --sr 6875000 \
	--verbose --open-timeout 15000 \
	--log /tmp/tda1002x-ui.log \
	$kept
