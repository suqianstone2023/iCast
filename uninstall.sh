#!/bin/bash
# Remove the SMIT iCast driver installed by install.sh.
# Usage: sudo ./uninstall.sh
set -u

PKG_VER="1.0"

if [ "$(id -u)" -ne 0 ]; then
    echo "!! root required: sudo $0"
    exit 1
fi

echo "==> unload modules"
modprobe -r dvb_usb_icast_smit 2>/dev/null
modprobe -r dvb_usb_icast_fe 2>/dev/null

echo "==> remove DKMS registration"
dkms remove icast/${PKG_VER} --all 2>/dev/null || true
rm -rf "/usr/src/icast-${PKG_VER}"

echo "==> drop load-at-boot configuration"
rm -f /etc/modules-load.d/icast.conf /etc/modprobe.d/icast.conf

depmod -a
echo "==> done"
