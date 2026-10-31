#!/bin/sh
# iCast 电视棒 · 卸载（配合 install.sh）
#
# 会删掉：内核模块、modprobe 配置、rcS 自启脚本、扫描器、ScanFilter 插件。
# 不会动：/etc/enigma2 下的频道库（lamedb5 / bouquet）—— 那是扫出来的数据。

MODDIR="/lib/modules/$(uname -r)/extra/icast"
PLUGDIR=/usr/lib/enigma2/python/Plugins/Extensions/ScanFilter

[ "$(id -u)" = "0" ] || { echo "请用 root 跑"; exit 1; }

echo "[icast] 先卸驱动（E2 正占着前端的话会失败，重启后直接跑本脚本即可）"
rmmod dvb_usb_icast_smit 2>/dev/null && echo "[icast] smit 已卸" || echo "[icast] smit 卸载失败（多半被 E2 占用），忽略"
rmmod dvb_usb_icast_fe 2>/dev/null && echo "[icast] fe 已卸" || echo "[icast] fe 卸载失败，忽略"

rm -f /etc/rcS.d/S99icast-load
rm -f /etc/modprobe.d/icast.conf
rm -f /usr/bin/tda1002x /usr/bin/tda1002x.v6
rm -rf "$PLUGDIR"
rm -rf "$MODDIR"
depmod -a >/dev/null 2>&1 || depmod >/dev/null 2>&1

echo "[icast] 卸完了。如有备份想恢复旧文件，找 *.bak-*："
ls /usr/bin/tda1002x.bak-* "$PLUGDIR"/plugin.py.bak-* 2>/dev/null || true
echo "[icast] 重启 E2 后界面里就不会再有这个 tuner 了。"
exit 0
