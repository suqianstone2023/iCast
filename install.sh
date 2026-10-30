#!/bin/bash
# Install the SMIT iCast (29df:0001) DTMB/DVBC USB driver through DKMS.
# Usage: sudo ./install.sh
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
KVER="$(uname -r)"
PKG_VER="1.0"
DKMS_SRC="/usr/src/icast-${PKG_VER}"

if [ "$(id -u)" -ne 0 ]; then
    echo "!! root required: sudo $0"
    exit 1
fi

if ! command -v dkms >/dev/null 2>&1; then
    echo "!! dkms not found. Install it first:"
    echo "     Debian/Ubuntu : sudo apt install dkms build-essential"
    echo "     Fedora        : sudo dnf install dkms"
    exit 1
fi

if [ ! -d "/lib/modules/${KVER}/build" ]; then
    echo "!! kernel headers for ${KVER} are missing. Install them first:"
    echo "     Debian/Ubuntu : sudo apt install linux-headers-${KVER}"
    exit 1
fi

echo "==> 1/5 copy sources to ${DKMS_SRC} and register with DKMS"
rm -rf "${DKMS_SRC}"
mkdir -p "${DKMS_SRC}"
cp -a "${SRC}/Makefile" "${SRC}/dkms.conf" "${SRC}"/*.c "${SRC}"/*.h "${SRC}/kapi" "${DKMS_SRC}/"

# Repeat installs: drop any previous registration first.
if dkms status icast 2>/dev/null | grep -q .; then
    dkms remove icast/${PKG_VER} --all >/dev/null 2>&1 || true
fi

dkms add icast/${PKG_VER} || { echo "!! dkms add failed"; exit 1; }

echo "==> 2/5 build and install the modules"
dkms build   icast/${PKG_VER} -k "${KVER}" || { echo "!! build failed, see /var/lib/dkms/icast/${PKG_VER}/build/make.log"; exit 1; }
dkms install icast/${PKG_VER} -k "${KVER}" || { echo "!! install failed"; exit 1; }

echo "==> 3/5 write load-at-boot and load-order configuration"
cat > /etc/modules-load.d/icast.conf <<'EOF'
# SMIT iCast DTMB/DVBC USB dongle
dvb_usb_icast_fe
dvb_usb_icast_smit
EOF

cat > /etc/modprobe.d/icast.conf <<'EOF'
# dvb-usb-icast-smit resolves icast_fe_attach at runtime through
# symbol_request(), which modpost cannot see.  The frontend module must be
# loaded first or frontend_attach() fails and the device never attaches.
softdep dvb_usb_icast_smit pre: dvb_usb_icast_fe
EOF

echo "==> 4/5 load the modules"
modprobe dvb_usb_icast_fe   && echo "    [ok] dvb_usb_icast_fe"
modprobe dvb_usb_icast_smit && echo "    [ok] dvb_usb_icast_smit"

echo "==> 5/5 verify"
echo "--- lsmod ---"
lsmod | grep -E "icast|^dvb_usb " || true
echo "--- dmesg (last 25 lines) ---"
dmesg | tail -25
echo "--- /dev/dvb ---"
ls -l /dev/dvb/ 2>/dev/null || echo "    (no /dev/dvb: device not plugged in or not recognised)"

if lsusb | grep -qi "29df:0001"; then
    echo "    [ok] USB device 29df:0001 present"
else
    echo "    [!!] 29df:0001 not found - plug the dongle into a USB port"
fi
