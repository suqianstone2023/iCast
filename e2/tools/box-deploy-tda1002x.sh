#!/bin/sh
# Install the tda1002x replacement that Enigma2's cable "automatic scan" calls,
# then run a single-frequency sanity check against the known 251 MHz multiplex.
#
# Run on the box (root).  Exit 0 = ready for the GUI automatic scan.

set -e

SRC=${1:-/tmp/tda1002x}
DST=/usr/bin/tda1002x

echo "== 1. install =="
[ -f "$SRC" ] || { echo "FAIL: $SRC missing (scp it over first)"; exit 1; }
cp -f "$SRC" "$DST"
chmod 755 "$DST"
ls -l "$DST"

echo "== 2. driver state =="
cat /etc/icast.mode 2>/dev/null | sed 's/^/configured: /'
/usr/bin/icast-mode 2>/dev/null | head -2

echo "== 3. frontend free? =="
busy=0
for p in /proc/[0-9]*; do
    for f in $p/fd/*; do
        t=$(readlink "$f" 2>/dev/null) || continue
        case "$t" in
            *adapter1/frontend0*) echo "held by ${p#/proc/} $(cat $p/comm 2>/dev/null): $t"; busy=1;;
        esac
    done
done
[ $busy -eq 1 ] && echo "note: frontend busy -- the GUI scan closes it itself, a manual run will not work now"

echo "== 4. single point 251 MHz =="
if "$DST" --from 251000 --to 251000 2>/dev/null | head -1 | grep -q '^OK'; then
    echo "SANITY OK"
else
    echo "SANITY FAIL (no lock at 251000)"
fi

echo "== 5. done =="
