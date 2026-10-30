# Linux DVB driver for SMIT iCast DTMB/DVBC dual mode demodulator USB dongle

支持 "iCast 融合电视伴侣" 双模 (DTMB/DVBC)、单模 (DVBC) 电视棒，USB ID `29df:0001`。
驱动加载后生成标准 `/dev/dvb/adapterN/`，可供 tvheadend、VLC、`dvbv5-tools` 等任何使用
Linux DVB API 的软件。

本仓库在上游基础上增加了**发行版内核的原生构建方式**（DKMS 安装，随内核升级自动重编），
同时保留上游的 OpenWrt SDK 交叉编译流程。

---

## 方式一：发行版原生构建（推荐）

### 为什么不能直接用上游的构建流程

上游的构建方式会把内核里的 `dvb-core`、`dvb-usb`、`dvb-usb-v2` 一并当外部模块重新编译
（路由器固件里没有 DVB 支持，只能自带）。在发行版内核上，这几个模块系统里已经有了，
再编一份会**撞模块名**，`dvb_core` 根本无法二次加载。

所以这里只编译两个业务模块，直接链接系统自带的 DVB-USB 框架：

| 模块 | 作用 |
|---|---|
| `dvb-usb-icast-fe.ko` | 前端（demodulator）实现，导出 `icast_fe_attach` |
| `dvb-usb-icast-smit.ko` | USB 设备驱动，握手 / 初始化 / 调谐命令 |

`dvb_usb_device_init()` 与 `dvb_usb_device_exit()` 都是 `EXPORT_SYMBOL`，因此外部模块可以
正常使用框架，驱动逻辑本身未做任何改动。

### 依赖

```bash
# Debian / Ubuntu
sudo apt install build-essential dkms linux-headers-$(uname -r)
# Fedora
sudo dnf install kernel-devel dkms gcc make
```

### 安装

```bash
git clone https://github.com/suqianstone2023/iCast.git
cd iCast
sudo ./install.sh
```

脚本会通过 DKMS 编译安装，并写入：

| 路径 | 内容 |
|---|---|
| `/usr/src/icast-1.0/` | DKMS 源码副本 |
| `/lib/modules/$(uname -r)/extra/icast/` | 安装后的 `.ko` |
| `/etc/modules-load.d/icast.conf` | 开机自动加载 |
| `/etc/modprobe.d/icast.conf` | 加载顺序（见下） |

卸载：`sudo ./uninstall.sh`

### 加载顺序是硬性要求

`dvb-usb-icast-smit` 通过 `dvb_attach(icast_fe_attach, ...)` 挂载前端，底层是
`symbol_request()` **运行时动态查找**符号，`modpost` 记录不到模块间依赖。

因此 `dvb-usb-icast-fe` 必须先加载，否则 `icast_fe_attach` 返回 `NULL`，probe 失败并打印：

```
dvb-usb-icast-smit: not attached iCast usb device
```

`install.sh` 写入的 `softdep` 就是为此：

```
softdep dvb_usb_icast_smit pre: dvb_usb_icast_fe
```

### 手动编译（不装 DKMS）

```bash
make            # 针对当前运行内核 /lib/modules/$(uname -r)/build
make KDIR=/lib/modules/<version>/build   # 指定其它内核
sudo make install
```

### 关于 `kapi/`

`dvb-usb.h` 是 `drivers/media/usb/dvb-usb/` 下的**私有头文件**，`linux-headers-$(uname -r)`
不提供，所以 `kapi/` 存放了与目标内核版本对应的原样副本：

| 文件 | 来源 |
|---|---|
| `kapi/dvb-usb.h` | `drivers/media/usb/dvb-usb/dvb-usb.h` |
| `kapi/dvb-pll.h` | `drivers/media/dvb-frontends/dvb-pll.h` |

当前副本取自 **v6.8**。注意 `dvb-usb.h` 里仍然 `#include "dvb-pll.h"`，而该文件在 6.x 已经
从 `usb/dvb-usb/` 挪到了 `dvb-frontends/`，取的时候别找错目录。

> **跨大版本升级内核（如 6.8 → 6.11）时必须同步替换这两个头文件**，否则
> `struct dvb_usb_adapter` / `struct dvb_usb_device` 布局可能与内核不一致。

---

## 方式二：OpenWrt SDK 交叉编译（上游原方式）

```shell
#下载SDK并解压(相应固件的SDK包或者交叉编译链)
wget https://downloads.immortalwrt.org/releases/23.05.2/targets/x86/64/immortalwrt-sdk-23.05.2-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz
tar xvf immortalwrt-sdk-23.05.2-x86-64_gcc-12.3.0_musl.Linux-x86_64.tar.xz

# 下载源码
git clone https://github.com/nxdong520/iCast.git
cd iCast

# 设置环境变量(X86_64架构)
export ARCH=x86
export STAGING_DIR=../immortalwrt-sdk-23.05.2-x86-64_gcc-12.3.0_musl.Linux-x86_64/staging_dir/toolchain-x86_64_gcc-12.3.0_musl/bin/
export KERNEL_DIR=../immortalwrt-sdk-23.05.2-x86-64_gcc-12.3.0_musl.Linux-x86_64/build_dir/target-x86_64_musl/linux-x86_64/linux-5.15.150
export TOOLCHAIN="../immortalwrt-sdk-23.05.2-x86-64_gcc-12.3.0_musl.Linux-x86_64/staging_dir/toolchain-x86_64_gcc-12.3.0_musl/bin/x86_64-openwrt-linux-"
export SRC_DIR=$(pwd)

# 编译准备（下载并解包对应版本内核源码）
make -f Makefile.openwrt prepare

# 开始编译
make -f Makefile.openwrt
```

### 将生成的内核驱动程序ko文件复制到目标机器

```shell
#加载驱动程序
insmod dvb-core.ko
insmod dvb-usb.ko
insmod dvb-usb-icast-fe.ko
insmod dvb-usb-icast-smit.ko

root@ImmortalWrt:~# dmesg
[  188.519518] usb 1-1: new high-speed USB device number 2 using ehci-pci
[  237.136519] dvb-usb: found a 'iCast DVB USB DONGLE adapter' in warm state.
[  237.146160] dvb-usb: will pass the complete MPEG2 transport stream to the software demuxer.
[  237.157533] dvbdev: DVB: registering new adapter (iCast DVB USB DONGLE adapter)
[  237.174939] usb 1-1: DVB: registering adapter 0 frontend 0 (iCast DTMB/DVBC USB demodulator)...
[  237.191475] dvb-usb: iCast DVB USB DONGLE adapter successfully initialized and connected.
[  237.215414] usbcore: registered new interface driver icast

root@ImmortalWrt:~# ls /dev/dvb/adapter0/ -al
drwxr-xr-x    2 root     root           120 May 28 14:42 .
drwxr-xr-x    3 root     root            60 May 28 11:26 ..
crw-------    1 root     root      212,   4 May 28 14:42 demux0
crw-------    1 root     root      212,   5 May 28 14:42 dvr0
crw-------    1 root     root      212,   3 May 28 14:42 frontend0
crw-------    1 root     root      212,   7 May 28 14:42 net0
```

---

## 验证

```bash
lsusb | grep 29df          # 应看到 29df:0001
ls /dev/dvb/               # 应多出一个 adapterN
dmesg | grep -i icast      # 应看到 successfully initialized and connected
```

## 目录说明

| 路径 | 说明 |
|---|---|
| `icast-fe.c` | 前端实现：调谐、锁定状态、信号强度 / 信噪比 |
| `icast-smit.c` | USB 驱动：ID 表、probe 握手、`dvb_usb_device_init` |
| `smit.c` / `smit.h` | 设备通信协议（bulk 命令封装） |
| `Makefile` | 发行版原生构建（本仓库新增） |
| `Makefile.openwrt` | OpenWrt SDK 交叉编译（上游原文件） |
| `env.*` | 各架构交叉编译环境变量样例 |
| `kapi/` | 内核私有头文件副本（本仓库新增） |

## 已知问题

- **USB ID 固定为 `29df:0001`**。若设备使用其它 PID（部分批次为 `0002`），需修改
  `icast-smit.c` 中的 `USB_PID_SMIT_ICAST` 后重新编译。
- 编译时提示 `the frame size of 1184 bytes is larger than 1024 bytes`：`smit.c` 在栈上
  放了一个 1004 字节的数据包，属上游写法，不影响功能。
- 设备通信协议通过 USB 抓包取得，并参考了网友对闭源驱动的逆向分析，未做完整覆盖，
  不同固件版本的兼容性取决于设备实际行为。

## 许可与致谢

GPL-2.0（见 `LICENSE`）。驱动原始作者：Xiaodong Ni <nxiaodong520@gmail.com>，
上游仓库：<https://github.com/nxdong520/iCast>。
本仓库仅新增发行版内核的构建与安装方式，驱动实现未做修改。
