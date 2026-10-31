# iCast DVB-C 电视棒 · Enigma2 部署包

把「融合电视伴侣」这根 USB 棒（SMIT iCast，`29df:0001`）变成 Enigma2 里的一个
真 tuner：界面里能看到、能手动调台、能走 E2 自己的自动扫描存频道。
不串流、没有中间进程，TS 直接走 E2 的 demux 进解码器。

**适用范围**：Enigma2 固件（OpenBH / OpenPLi / ...），内核 **4.4.35**，**armv7**
（GigaBlue UHD Trio 4K / Hi3798MV200 实测通过；内核版本不同的话模块会加载失败，
需要按 `e2/BUILD-KERNEL-MODULE.md` 重新交叉编译）。

## 三步装好

```sh
# 1. 包传到盒子（scp 或 U 盘都行），解包
tar xzf icast-e2-deploy-*.tar.gz -C /tmp
cd /tmp/icast-e2-deploy

# 2. 安装（会自动备份已有文件、加载驱动、自检）
sh install.sh
#    不想开机自动加载的话：sh install.sh --no-autostart

# 3. 重启 E2（菜单 -> 关机/重启 -> 重启 GUI；或者整机重启更省事）
```

装完不用改任何 E2 设置，默认参数就是给这根棒子调好的。

## 验证

- `install.sh` 结尾会打印 `/dev/dvb/adapter*/frontend0` 和 dmesg 的相关行；
- E2 界面：设置 → 搜台/调谐器配置，应多出一个 **DVB-C** 调谐器（Tuner C）；
  如果显示成 DVB-T，说明驱动没带 `icast_mode=dvbc` 参数加载，看下面「常见问题」。

## 扫台

菜单 → 设置 → 频道搜索 → **自动扫描**，选这个 tuner，默认参数直接开扫：

- 全段 115–862 MHz，8 MHz 栅格，约 4 分钟；
- E2 默认给扫描器的参数（SR 6900、步进 1000kHz）这根棒子锁不上，装好的
  `/usr/bin/tda1002x` wrapper 会自动改写成 SR 6875、步进 8000，不用手动调；
- 扫描器日志：`/tmp/tda1002x-ui.log`；
- 扫完 **ScanFilter** 插件自动剔掉广播电台和 ad/UI 数据业务（在 E2 进程内做，
  不装常驻进程），日志 `/tmp/scanfilter.log`。想留电台就停用这个插件：
  `mv /usr/lib/enigma2/python/Plugins/Extensions/ScanFilter{,.off}` 再重启 GUI。

实测（有线网全段）：29 个转发器、258 个频道名，清理后剩 141 个电视频道。

## 常见问题

**界面里没出现 tuner**
`lsmod | grep icast` 确认两个模块都进内核了；`cat /tmp/icast-load.log` 看自启
脚本的输出；`dmesg | tail -30` 看 USB 枚举和 probe。都没问题的话重启整机。
E2 对 USB tuner 有硬门槛：`/dev/dvb/adapterN/frontend0` 和 `/dev/misc/vtunerN`
两个都有它才认，少一个 adapter 整个被丢弃。

**tuner 显示成 DVB-T**
模块加载时没带 `icast_mode=dvbc`。正确顺序（fe 必须在前）：

```sh
modprobe dvb_usb_icast_fe && modprobe dvb_usb_icast_smit
```

装了本包的话 `/etc/modprobe.d/icast.conf` 已配好，别手工 `insmod` 抢在 modprobe 前。

**扫描结果 0 个或频道认错**
- 确认用的还是 wrapper（`head -1 /usr/bin/tda1002x` 应该是 `#!/bin/sh`，
  真身是 `.v6`）：SR 必须是 6875，频率必须落在 8 MHz 栅格上，这棒子没有 AFC；
- 频道认错（PAT 是别的频点的）是老版本驱动的 FIFO 残留问题，重装本包里的
  .ko 即可（`set_frontend` 末尾会排掉残留）。

**卸载**：`sh uninstall.sh`（E2 占着前端时 rmmod 会失败，重启整机后再跑一次）。

## 包里都有什么

| 文件 | 去处 | 说明 |
|---|---|---|
| `files/ko/*.ko` | `/lib/modules/$(uname -r)/extra/icast/` | 驱动，fe 在前 smit 在后加载 |
| `files/etc/modprobe.d/icast.conf` | 同路径 | 加载顺序 softdep + `icast_mode=dvbc` |
| `files/etc/rcS.d/S99icast-load` | 同路径 | 开机后台加载，绝不阻塞 rcS |
| `files/bin/tda1002x.v6` | `/usr/bin/tda1002x.v6` | 扫描器本体（ARM 静态链接） |
| `files/bin/tda1002x` | `/usr/bin/tda1002x` | wrapper：改写 E2 传的扫描参数 |
| `files/ScanFilter-plugin.py` | `.../Plugins/Extensions/ScanFilter/plugin.py` | 扫描存盘自动过滤 |
| `install.sh` / `uninstall.sh` | — | 装 / 卸 |

驱动和扫描器的源码、踩坑记录在仓库 `e2/` 目录：
<https://github.com/suqianstone2023/iCast>

打好的成品 tar.gz 不放 main（main 只进源码），在仓库的 **`e2` 分支**
`e2/deploy/releases/` 下，直接下载即可：

<https://github.com/suqianstone2023/iCast/tree/e2/e2/deploy/releases>
