#!/bin/sh
# iCast DVB-C 电视棒 · Enigma2 一键安装
#
# 适用：Enigma2 固件（OpenBH / OpenPLi / ...），内核 4.4.35，armv7。
#       GigaBlue UHD Trio 4K（gbmv200 / Hi3798MV200）实测通过。
#
# 用法（在盒子上，解包目录里）：
#   sh install.sh                  # 装文件 + 尝试加载驱动 + 装开机自启
#   sh install.sh --no-autostart   # 不装开机自动加载
#
# 全部改动前都会先备份（文件名后缀 .bak-时间戳），卸载见 uninstall.sh。

TS=$(date +%Y%m%d-%H%M%S)
KVER=$(uname -r)
ARCH=$(uname -m)
MODDIR="/lib/modules/$KVER/extra/icast"
PLUGDIR=/usr/lib/enigma2/python/Plugins/Extensions/ScanFilter
AUTOSTART=1

for a in "$@"; do
	case "$a" in
	--no-autostart) AUTOSTART=0 ;;
	*) echo "未知参数: $a"; exit 1 ;;
	esac
done

log()  { echo "[icast] $*"; }
die()  { echo "[icast] 错误: $*" >&2; exit 1; }
backup() { [ -f "$1" ] && { cp -a "$1" "$1.bak-$TS"; log "已备份 $1 -> $1.bak-$TS"; } || true; }

# ---- 0. 环境检查 --------------------------------------------------------
[ "$(id -u)" = "0" ] || die "请用 root 跑（ssh 进盒子默认就是 root）"
[ -f files/ko/dvb-usb-icast-fe.ko ] || die "找不到 files/ko/，包没解完整？"

if [ "$ARCH" != "armv7l" ]; then
	log "警告: 当前架构 $ARCH 不是 armv7l，内核模块可能根本加载不上"
fi
case "$KVER" in
4.4.35*) ;;
*) log "警告: 驱动是按 4.4.35 编译的，当前内核 $KVER，vermagic 对不上 modprobe 会拒绝加载" ;;
esac
[ -d /usr/lib/enigma2 ] || log "警告: 没看到 /usr/lib/enigma2，这真是台 E2 机器吗？（继续装）"

# ---- 1. 内核模块 --------------------------------------------------------
mkdir -p "$MODDIR"
cp files/ko/dvb-usb-icast-fe.ko files/ko/dvb-usb-icast-smit.ko "$MODDIR/"
depmod -a >/dev/null 2>&1 || depmod >/dev/null 2>&1 || log "警告: depmod 失败，modprobe 可能找不到模块"
log "内核模块 -> $MODDIR"

# ---- 2. modprobe 配置：加载顺序 + 默认 DVB-C ---------------------------
# 顺序是硬性的：fe 必须先于 smit，否则 icast_fe_attach 拿不到符号。
# icast_mode=dvbc 必须显式给：热重启后固件缓冲里的 DTMB:Y 会把前端翻成 DVB-T。
mkdir -p /etc/modprobe.d
cp files/etc/modprobe.d/icast.conf /etc/modprobe.d/icast.conf.new &&
	mv /etc/modprobe.d/icast.conf.new /etc/modprobe.d/icast.conf
log "modprobe 配置 -> /etc/modprobe.d/icast.conf"

# ---- 3. 扫描器（E2 扫台时调用的是 /usr/bin/tda1002x）-------------------
install -m 0755 files/bin/tda1002x.v6 /usr/bin/tda1002x.v6 2>/dev/null ||
	{ cp files/bin/tda1002x.v6 /usr/bin/tda1002x.v6; chmod 0755 /usr/bin/tda1002x.v6; }
backup /usr/bin/tda1002x
install -m 0755 files/bin/tda1002x /usr/bin/tda1002x 2>/dev/null ||
	{ cp files/bin/tda1002x /usr/bin/tda1002x; chmod 0755 /usr/bin/tda1002x; }
log "扫描器 -> /usr/bin/tda1002x（真身在 /usr/bin/tda1002x.v6，wrapper 改写参数）"

# ---- 4. ScanFilter 插件：扫完自动剔掉广播/数据业务 ---------------------
mkdir -p "$PLUGDIR"
backup "$PLUGDIR/plugin.py"
cp files/ScanFilter-plugin.py "$PLUGDIR/plugin.py"
chmod 0644 "$PLUGDIR/plugin.py"
[ -f "$PLUGDIR/__init__.py" ] || : > "$PLUGDIR/__init__.py"
rm -f "$PLUGDIR"/plugin.pyc 2>/dev/null   # 让 E2 重启后重新编译
log "ScanFilter 插件 -> $PLUGDIR（运行日志 /tmp/scanfilter.log）"

# ---- 5. 开机自动加载 ----------------------------------------------------
if [ "$AUTOSTART" = "1" ]; then
	cp files/etc/rcS.d/S99icast-load /etc/rcS.d/S99icast-load
	chmod 0755 /etc/rcS.d/S99icast-load
	log "开机自动加载 -> /etc/rcS.d/S99icast-load（后台执行，不会阻塞 rcS）"
else
	log "按 --no-autostart 跳过开机自启；要加载驱动请手动跑: modprobe dvb_usb_icast_fe && modprobe dvb_usb_icast_smit"
fi

# ---- 6. 尝试加载驱动（已经加载就跳过）----------------------------------
if [ -d /sys/module/dvb_usb_icast_smit ]; then
	log "驱动已在内核里，跳过加载"
else
	log "加载驱动..."
	if modprobe dvb_usb_icast_fe 2>/dev/null && modprobe dvb_usb_icast_smit 2>/dev/null; then
		:
	else
		log "modprobe 不行，退回 insmod"
		insmod "$MODDIR/dvb-usb-icast-fe.ko" &&
		insmod "$MODDIR/dvb-usb-icast-smit.ko" icast_mode=dvbc ||
			die "驱动加载失败，dmesg 里看原因: dmesg | tail -30"
	fi
	# 棒子枚举是 USB hotplug 异步的，等 adapter 节点出来（最多 10 秒）
	i=0
	while [ $i -lt 20 ]; do
		ls /dev/dvb/adapter*/frontend0 >/dev/null 2>&1 && break
		i=$((i + 1)); sleep 0.5
	done
fi

# ---- 7. 自检 ------------------------------------------------------------
echo
log "==== 自检 ===="
if ls /dev/dvb/adapter*/frontend0 >/dev/null 2>&1; then
	ls -l /dev/dvb/adapter*/frontend0
else
	log "!! 还没看到 /dev/dvb/adapter*/frontend0，看 dmesg: dmesg | tail -30"
fi
dmesg 2>/dev/null | grep -i icast | tail -5
echo
log "装完了。接下来："
log "  1. 重启 E2（菜单 -> 关机/重启 -> 重启 GUI，或整机重启）"
log "  2. 界面里应多出一个 DVB-C 调谐器（Tuner C）"
log "  3. 菜单 -> 设置 -> 频道搜索 -> 自动扫描，选这个 tuner，直接开扫"
log "     （扫 115-862 MHz 全段，约 4 分钟；SR/步进 wrapper 会自动改，不用管）"
log "  4. 扫完广播电台和 ad 数据业务会被 ScanFilter 自动清掉"
exit 0
