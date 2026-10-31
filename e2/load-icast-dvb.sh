#!/bin/sh
# 手工加载 iCast 内核驱动（Enigma2 / gbmv200 / 4.4.35）
#
# 故意不做成开机自启：insmod 有 Oops 风险，panic 会把盒子搞挂。
# 先在 SSH 里手工跑一次，确认 dmesg 干净，再决定要不要固化。
#
# 用法：
#   sh load-icast-dvb.sh            加载并打印结果
#   sh load-icast-dvb.sh unload     卸载
#   SMIT_ASYNC=0 sh load-icast-dvb.sh
#       同步 probe（初始化在 khubd 里跑）。默认是异步，初始化丢给
#       workqueue，避免长时间占住枚举线程。
#
# 加载成功与否只看一件事：/dev/dvb/adapterN/frontend0 有没有出现。

DIR=$(dirname "$0")
MODDIR=${MODDIR:-/lib/modules/$(uname -r)/extra/icast}
SMIT_ASYNC=${SMIT_ASYNC:-1}

FE=dvb-usb-icast-fe.ko
SMIT=dvb-usb-icast-smit.ko

log() { echo "[icast-dvb] $*"; }

find_mod() {
	for d in "$MODDIR" "$DIR" "$DIR/ko"; do
		[ -f "$d/$1" ] && { echo "$d/$1"; return 0; }
	done
	return 1
}

case "$1" in
unload)
	for m in dvb-usb-icast-smit dvb-usb-icast-fe; do
		awk -v m="$m" '$1 == m { found = 1 } END { exit !found }' /proc/modules 2>/dev/null \
			&& { rmmod "$m" && log "removed $m"; }
	done
	exit 0
	;;
esac

# 设备在不在
if ! lsusb 2>/dev/null | awk '/29df:0001/ { found = 1 } END { exit !found }'; then
	log "警告：没看到 29df:0001 设备"
fi

FE_PATH=$(find_mod "$FE") || { log "找不到 $FE（找过 $MODDIR $DIR $DIR/ko）"; exit 1; }
SMIT_PATH=$(find_mod "$SMIT") || { log "找不到 $SMIT"; exit 1; }

before=$(ls -d /dev/dvb/adapter* 2>/dev/null | tr '\n' ' ')

# fe 必须先上：smit 模块用 dvb_attach 找 icast_fe_attach，
# 而 dvb_attach 只能在已加载的模块里找符号。
OUT=$(insmod "$FE_PATH" 2>&1) || { log "insmod $FE 失败: $OUT"; dmesg | tail -15; exit 1; }
log "loaded $FE"
sleep 1

if ! OUT=$(insmod "$SMIT_PATH" smit_async="$SMIT_ASYNC" 2>&1); then
	log "insmod $SMIT 失败: $OUT"
	dmesg | tail -20
	rmmod dvb-usb-icast-fe 2>/dev/null
	exit 1
fi
log "loaded $SMIT (smit_async=$SMIT_ASYNC)"

# 异步初始化要几秒：smit_init 有一百多次 bulk 往返，
# 卡住时 smit_check 还会各等最多 3 秒。
log "等待适配器出现 ..."
new=""
i=0
while [ $i -lt 60 ]; do
	for d in /dev/dvb/adapter*; do
		[ -d "$d" ] || continue
		case " $before " in *" $d "*) continue ;; esac
		[ -e "$d/frontend0" ] && { new=$d; break; }
	done
	[ -n "$new" ] && break
	i=$((i + 1))
	sleep 1
done

echo "--- dmesg ---"
dmesg | tail -25
echo "--- /proc/modules ---"
awk '/icast/ { print }' /proc/modules
echo "--- /dev/dvb ---"
ls -l /dev/dvb/adapter*/ 2>/dev/null || log "没有 /dev/dvb/adapter*，说明 probe 没成功"

if [ -z "$new" ]; then
	log "适配器没有出现。看上面的 dmesg，然后 unload 重试或换 SMIT_ASYNC=0"
	exit 1
fi

nr=$(echo "$new" | sed 's|.*adapter||')
log "新适配器：$new"
echo "--- frontend 信息 ---"
for f in /sys/class/dvb/dvb"$nr".frontend0/name /sys/class/dvb/dvb"$nr".frontend0/type; do
	[ -f "$f" ] && echo "$f = $(cat "$f")"
done

# E2 侧的前提：任何 USB 适配器都走 eDVBUsbAdapter，它必须能打开
# /dev/misc/vtunerN，否则整个 adapter 被丢弃。
echo "--- E2 需要的 vtuner 设备 ---"
if ls /dev/misc/vtuner* >/dev/null 2>&1; then
	ls -l /dev/misc/vtuner*
elif ls /dev/vtuner* >/dev/null 2>&1; then
	log "只有 /dev/vtuner*，缺 /dev/misc/vtuner*（E2 只认后者）"
	ls -l /dev/vtuner*
else
	log "没有任何 vtuner 设备：E2 会丢弃这个 adapter，需要 vtuner 模块"
fi
