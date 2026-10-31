# 为 Enigma2 盒子交叉编译 iCast 内核驱动

目标机：GigaBlue gbmv200 / Hi3798MV200，OpenBH 5.6
内核：4.4.35（厂商树，`COMPATIBLE_MACHINE = "gbmv200"`）
架构：32 位 armv7l，`cortexa15hf-neon-vfpv4`，hard-float

结果：**编译通过，产物与盒子内核 ABI 完全匹配**，运行时行为待上机验证。

## 两个仓库怎么"合作"

上游 `nxdong520/iCast` 是完整的内核驱动：

| 文件 | 角色 |
|---|---|
| `icast-fe.c` | `dvb_frontend` 驱动，注册真前端，`set_frontend` / `read_status` / `read_snr` |
| `icast-smit.c` | `struct usb_driver`，probe / id_table / `dvb_usb_device_properties` |
| `smit.c` | SMIT 私协议（0xFE 握手、`smit_set`、`smit_tuner_stat`） |
| `linuxdvb/` | 自带一份 dvb-core / dvb-usb 源码 |
| `Makefile` | 全量重编 dvb-core + dvb-usb + dvb-usb_v2 + iCast |

fork（`suqianstone2023/iCast`）的驱动本体与上游**逐字节相同**，只加了构建层：
`kapi/`（内核 API 兼容头）、`dkms.conf`、`tools/icast-libusb`（用户态版）。

所以"合作"的实质是：**上游出驱动本体，fork 出构建策略**。但两边都不能直接用在盒子上：

- 上游 `Makefile` 会把 `dvb-core` / `dvb-usb` 当外部模块重编。盒子内核里这两个是**内置**的
  （`CONFIG_DVB_CORE=y`、`CONFIG_DVB_USB=y`），再加载一份会符号冲突。
- fork 的 `Makefile` 思路正确（只编两个 iCast 模块、链内核自带的框架），但它指向 `kapi/dvb-usb.h`，
  那是 **v6.16 的头文件**，`kapi-guard.h` 自己写着"只验证过 v5.15~v6.16"。4.4.35 在警告范围内之外，
  而结构体布局不匹配**不会编译失败**，只会运行期踩内存。

解法：fork 的 native 策略 + **4.4.35 内核源码自带的头文件**。新增 `Makefile.e2` 做这件事。

## 构建步骤（在 Linux x86_64 上）

### 1. 内核源码

```
https://source.mynonpublic.com/gigablue/mv200/gigablue-linux-4.4.35-20181224.tar.gz
sha256 0654d5aa21c51eaea46f7203014afe60052ec0990a92b9e289e1ca8a2793907c
```

来源是 oe-alliance-core 的 `meta-brands/meta-gigablue/recipes-linux/linux-gigablue_4.4.35.bb`。

### 2. 工具链

Bootlin `armv7-eabihf--glibc--stable-2022.08-1`（gcc 11）。解压即用，不需要 root。

不要用 gcc 13+：4.4 的 kconfig 和 dtc 会因 `yylloc` 重复定义链接失败（见下）。

### 3. 配置内核

```sh
export PATH=$HOME/e2build/armv7-eabihf--glibc--stable-2022.08-1/bin:$PATH
export ARCH=arm CROSS_COMPILE=arm-linux-
cd linux-4.4.35
cp <oe 仓库的 gbmv200 defconfig> .config
make olddefconfig
```

defconfig 取自
`meta-brands/meta-gigablue/recipes-linux/linux-gigablue-4.4.35/gbmv200/defconfig`。

### 4. modules_prepare

```sh
make HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 \
     -fomit-frame-pointer -std=gnu89 -fcommon" modules_prepare -j16
```

`-fcommon` 是必需的：GCC 10 起默认 `-fno-common`，而 4.4 的
`scripts/dtc/dtc-parser.tab.c` 与 `dtc-lexer.lex.c` 都定义 `yylloc`，链接会失败。
OE 自己也要打 `fix-multiple-defs-yylloc.patch` 解决同一问题。

确认 `make kernelrelease` 输出 `4.4.35`（决定 vermagic）。

### 5. 编译模块

```sh
cd icast
ln -sf Makefile.e2 Makefile     # kbuild 只认固定文件名
make KDIR=$HOME/e2build/linux-4.4.35 CROSS_COMPILE=arm-linux-
```

`Makefile.e2` 相对 fork 的 `Makefile` 只有一处实质差别：头文件来自内核树

```
-I$(KDIR)/drivers/media/usb/dvb-usb
-I$(KDIR)/drivers/media/dvb-core
-I$(KDIR)/drivers/media/dvb-frontends
```

第三个是 `dvb-pll.h` 的位置。4.4 里它在 `dvb-frontends/`，后来才移到 `tuners/`。
`kapi/dvb-pll.h` 那个手写存根因此不再需要。

## 为跑起来必须改的三处

编译通过不等于能工作。三处都是 4.4.35 特有的坑，前两处会直接导致崩溃或驱动完全不生效。

### ① `icast-fe.c`：`get_frontend` 签名

`get_frontend` 在 v4.7 从 `(fe)` 改成 `(fe, props)`。上游按新签名写，在 4.4 上只有一条
`incompatible pointer type` 警告，但运行期内核用一个参数去调用双参数函数，回调里
`props->frequency = ...` 写的是 r1 里的残留值——**野指针写**。

已加 `#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)` 分支。

### ② `icast-smit.c`：端点判断用 `usb_endpoint_num()`（我自己引入的，差点没发现）

`usb_endpoint_num()` 会掩掉方向位，只返回 0..15。拿它跟 `0x82`（130）比较永远为假，
于是 `smit_has_endpoints()` 恒返回 false，**整个 `smit_probe` 被编译器优化成
"打印一句 + return -ENODEV"**——模块能加载，但设备永远不会被接管。

发现方式：`nm -u icast-smit.o` 里没有 `dvb_usb_device_init`，反汇编 `smit_probe`
只剩 12 条指令。改成比较 `bEndpointAddress` 后变成 119 条。

### ③ 健壮性

- `smit_probe`：`kmalloc(4)` 没判 NULL 就 `buf[0] = 0xFE`
- `smit_init`：`u8 buf[1000]` 未初始化，末尾 `strstr(buf + 18, "DTMB:Y")` 可能越界读
- `smit_probe`：id_table 只按 VID/PID 匹配，usbcore 会为每个接口各调一次 probe，
  第二遍 `smit_init()` 会把 MCU 打进忙状态（`01 00 80 02 01 00`）。现在只让带
  SMIT 命令端点（bulk OUT 0x01 + bulk IN 0x82）的接口通过，遍历所有 altsetting 判断。

## 产物

```
dvb-usb-icast-fe.ko     12924  sha256 301dce01891efee9c8f4a38a2b3779d1d303c97f3eb93b578b079212615dd56d
dvb-usb-icast-smit.ko   15476  sha256 6d2d406a154406da25205f086943ef07f5ea1c5b2f3d1caaebe782e8c809b224
```

核对结果：

```
vermagic=4.4.35 SMP mod_unload ARMv7 p2v8      两个模块一致
ELF32 / ARM / REL                              32 位 armv7 正确
license=GPL                                    能取 GPL-only 符号（usb_bulk_msg）
alias=usb:v29DFp0001d*dc*dsc*dp*ic*isc*ip*in*  modprobe 可按 USB ID 自动加载
__ksymtab_icast_fe_attach                      fe 模块导出，smit 模块的 dvb_attach 能取到
```

未定义符号全部是内核导出项：`dvb_usb_device_init` / `dvb_usb_device_exit` / `usb_bulk_msg` /
`usb_control_msg` / `usb_register_driver` / `__symbol_get` / `__kmalloc` / `printk` /
`__aeabi_unwind_cpp_pr1`（`CONFIG_ARM_UNWIND=y`）/ `__gnu_mcount_nc`（`CONFIG_FUNCTION_TRACER=y`）。

内核没开 `CONFIG_MODVERSIONS`、没开模块签名，所以不需要 `Module.symvers`，
也不需要签任何东西。

## 上机加载

**加载顺序有意义**：`dvb-usb-icast-smit.ko` 通过 `dvb_attach` 找 `icast_fe_attach`，
而 `dvb_attach` 只能在**已加载**模块里找符号，所以 fe 必须先上。没有 `Module.symvers`，
`depmod` 生成不出这个依赖，`modprobe` 不会自动排序。

```sh
insmod /lib/modules/4.4.35/extra/icast/dvb-usb-icast-fe.ko
insmod /lib/modules/4.4.35/extra/icast/dvb-usb-icast-smit.ko
```

或者用现成的 `load-icast-dvb.sh`（见同目录），它按顺序加载并打印 dmesg。

成功的话应该看到 `/dev/dvb/adapterN/frontend0`、`demux0`、`dvr0`，
`cat /sys/class/dvb/dvbN.frontend0/name` 应该回
"iCast DVBC USB demodulator"。

## 第二轮加固（commit 2c14794）

第一轮只做到「编得过、vermagic 对」。对着 `BlackHole/enigma2` 的
`lib/dvb/dvb.cpp` 和 4.4.35 的 dvb-core 逐条核对之后，又改了十处，
其中两处是「编译完全正常、跑起来必然不对」的类型：

1. **`.info.type` 从来没设**。dvb-core 的 `FE_GET_INFO` 是原样
   `memcpy` `fe->ops.info`，而 `fe_type` 枚举从 0 开始，0 就是 `FE_QPSK`。
   E2 的 `eDVBUsbAdapter` 拿这个字段决定 tuner 类型（FE_QAM → DVB-C，
   FE_QPSK → DVB-S2），于是有线棒子会被标成卫星头。现在单模式 FE_QAM、
   双模式 FE_OFDM，并且已经在 .ko 里按结构体偏移反查确认：
   `type=1`、`caps=0xb0200`。
2. **`read_status` 读未初始化栈**：`smit_tuner_stat` 返回不足 32 字节时
   仍然取 `buf[29]` 当锁标志；失败路径还不写 `*status`。

其余：probe 改为 workqueue（`smit_async=1`）、`smit_check` 加 3 秒上限、
`smit_init` 首包失败即报错、锁状态位补全、频率 kHz→Hz、`read_ber` /
`read_ucblocks`（4.4 就叫这个名字，4.11 才改名）、`set_frontend` 传播错误。

E2 侧的完整链路（为什么必须有 `/dev/misc/vtunerN`、调谐怎么落到真前端）
写在 `E2-INTEGRATION.md`。

## 尚未验证

编译、ABI、结构体字段都已核对，**运行时行为一条都没验证过**——
盒子当时开不了机。
上机时按这个顺序，每步看 dmesg：

1. `insmod` 两个模块，看 probe 有没有打印 "not ours"（说明端点判断误判）
2. `dmesg | tail` 看 `smit_init` 的握手有没有报错
3. `ls /dev/dvb/adapter*/`
4. 有前端就 `dvb-fe-tool --freq=251000000 --delivery-system=DVB-C/A`（或 E2 里直接扫频）
5. 出锁 → E2 里能扫到台，这是真 tuner 路线成了
6. 卡在 `01 00 80 02 01 00` → 棒子又进忙状态，必须重新上电

风险：`insmod` 可能 Oops。内核 panic 会再次把盒子搞挂，且这次同样得靠断电或刷机恢复。
