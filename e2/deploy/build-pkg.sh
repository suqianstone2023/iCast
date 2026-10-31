#!/bin/bash
# 组装 Enigma2 部署包（在开发机上跑，不在盒子上跑）。
#
#   ./build-pkg.sh                # 输出 icast-e2-deploy-<日期>.tar.gz 到本目录
#   ./build-pkg.sh -o /some/dir   # 指定输出目录
#   ./build-pkg.sh --tda <path>   # 指定 ARM 版扫描器二进制（缺省从常用位置找）
#   ./build-pkg.sh --ko <dir>     # 指定 .ko 所在目录（缺省从常用位置找）
#
# 缺省查找位置（相对仓库）：
#   .ko        ../../../icast-e2/ko/          （工作区 icast-e2/ko）
#   tda1002x.v6  ../../../icast-e2/tools/tda1002x.v6.armv7
# 找不到时先用 buildroot 工具链编一个再打包：
#   scp e2/tools/tda1002x.c 到 219.219.0.41，然后用
#   ~/e2build/armv7-eabihf--glibc--stable-2022.08-1/bin/arm-buildroot-linux-gnueabihf-gcc
#   -O2 -static -o tda1002x.v6.armv7 tda1002x.c
set -euo pipefail

DEPLOY=$(cd "$(dirname "$0")" && pwd)          # .../icast-fork/e2/deploy
WORK=$(cd "$DEPLOY/../../.." && pwd)           # 工作区根（icast-e2 所在）
REPO=$(cd "$DEPLOY/../.." && pwd)              # 仓库根（icast-fork）
OUT=""
TDA=""
KODIR=""

while [ $# -gt 0 ]; do
	case "$1" in
	-o) OUT="$2"; shift 2 ;;
	--tda) TDA="$2"; shift 2 ;;
	--ko) KODIR="$2"; shift 2 ;;
	*) echo "未知参数 $1"; exit 1 ;;
	esac
done

KODIR=${KODIR:-$WORK/icast-e2/ko}
TDA=${TDA:-$WORK/icast-e2/tools/tda1002x.v6.armv7}

[ -f "$KODIR/dvb-usb-icast-fe.ko" ] || { echo "找不到 $KODIR/dvb-usb-icast-fe.ko（用 --ko 指定）"; exit 1; }
[ -f "$KODIR/dvb-usb-icast-smit.ko" ] || { echo "找不到 smit.ko"; exit 1; }
[ -f "$TDA" ] || { echo "找不到扫描器二进制 $TDA（用 --tda 指定，编译方法见脚本头部注释）"; exit 1; }

STAGE=$(mktemp -d)/icast-e2-deploy
PKG=$DEPLOY/pkg

mkdir -p "$STAGE/files/ko" "$STAGE/files/bin" "$STAGE/files/etc/modprobe.d" "$STAGE/files/etc/rcS.d"

cp "$PKG/install.sh" "$PKG/uninstall.sh" "$PKG/README.md" "$STAGE/"
cp "$KODIR/dvb-usb-icast-fe.ko" "$KODIR/dvb-usb-icast-smit.ko" "$STAGE/files/ko/"
cp "$TDA" "$STAGE/files/bin/tda1002x.v6"
cp "$REPO/e2/tools/tda1002x-wrapper.sh" "$STAGE/files/bin/tda1002x"
cp "$REPO/e2/tools/ScanFilter-plugin.py" "$STAGE/files/ScanFilter-plugin.py"
cp "$PKG/overlay/etc/modprobe.d/icast.conf" "$STAGE/files/etc/modprobe.d/"
cp "$PKG/overlay/etc/rcS.d/S99icast-load" "$STAGE/files/etc/rcS.d/"

chmod 0755 "$STAGE/install.sh" "$STAGE/uninstall.sh" \
	"$STAGE/files/bin/tda1002x" "$STAGE/files/bin/tda1002x.v6" \
	"$STAGE/files/etc/rcS.d/S99icast-load"

# 扫描器 strip 一下，562KB 的 debug_info 没必要带进包
if command -v arm-buildroot-linux-gnueabihf-strip >/dev/null 2>&1; then
	arm-buildroot-linux-gnueabihf-strip "$STAGE/files/bin/tda1002x.v6"
elif [ "$(uname)" = "Darwin" ] && command -v llvm-strip >/dev/null 2>&1; then
	llvm-strip "$STAGE/files/bin/tda1002x.v6" 2>/dev/null || true   # macOS 上的 llvm-strip 不认 ARM 也会留原样
fi

NAME="icast-e2-deploy-$(date +%Y%m%d).tar.gz"
OUT=${OUT:-$DEPLOY}
mkdir -p "$OUT"
tar -C "$(dirname "$STAGE")" -czf "$OUT/$NAME" "$(basename "$STAGE")"
rm -rf "$(dirname "$STAGE")"

echo "打包完成: $OUT/$NAME ($(du -h "$OUT/$NAME" | cut -f1))"
echo "上盒子:   scp $OUT/$NAME root@<盒子IP>:/tmp/ && ssh 进去 tar xzf /tmp/$NAME -C /tmp && sh /tmp/icast-e2-deploy/install.sh"
