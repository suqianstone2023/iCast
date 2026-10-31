# Enigma2 侧的东西

驱动本身在仓库根目录，`e2/` 放的是把棒子变成「界面上能看见、能调台、能扫描」的那个
tuner 所需要的一切：扫描器源码、给 E2 打的补丁、部署脚本，以及几份踩坑记录。

跑通的机器：GigaBlue UHD Trio 4K（Hi3798MV200，ARMv7，内核 4.4.35）+ OpenBH 5.6。

## 链路

不串流，也没有中间进程。棒子出流后直接交给 E2 自己的 demux：

```
adapter1/frontend0 (iCast, 我们的驱动)
  └─ adapter1/demux0 ──► /dev/misc/vtuner0 ──► adapter0/demux0 ──► 解码器
```

`eDVBUsbAdapter` 认 adapter 的两个硬条件（`E2-INTEGRATION.md` 里有源码依据）：

1. `/dev/dvb/adapterN/frontend0` 能开、`FE_GET_INFO` 回的类型是 `FE_QAM`；
2. `/dev/misc/vtunerN` 存在。

少一个整个 adapter 就被丢掉，界面上连「调谐器 C」都不会出现。

## 目录

| 路径 | 说明 |
|---|---|
| `E2-INTEGRATION.md` | E2 到底怎么枚举和处理 USB tuner，逐条对应到驱动要实现什么 |
| `BUILD-KERNEL-MODULE.md` | 用 4.4.35 内核源码交叉编译两个 .ko 的完整步骤 |
| `load-icast-dvb.sh` | 手工加载驱动（故意不做自启，`insmod` 有 Oops 风险） |
| `tools/tda1002x.c` | 扫描器。E2 调它扫频，它把结果按 E2 期望的格式打到 stdout |
| `tools/tda1002x-wrapper.sh` | 部署成 `/usr/bin/tda1002x` 的壳，负责改写 E2 传下来的参数 |
| `tools/ScanFilter-plugin.py` | E2 插件，扫描存盘时自动剔掉广播和数据业务 |
| `tools/lamedb-filter.py` | 不装插件时的手工清理版，直接改 lamedb/lamedb5 |
| `tools/retunetest.c` | 模拟 E2 的调台时序（同 fd / 重开、section / PES 四种组合），排查用 |
| `tools/dvbprobe.c`、`dvbtprobe.c`、`nitprobe.c` | 锁频 / 读 NIT 的小工具，验证信号用 |
| `tools/v3tune.c` | 最早的调谐试验品 |
| `tools/ICastMode/` | 早期插件，只负责在界面里把 tuner 显示成 DVB-C |
| `legacy-cccam/` | 抛弃的老方案：icast-cli 出流 + CCcam 解扰 + HTTP 推给 E2，留作参考 |
| `legacy-iptv/` | 更早的方案：用户态驱动 + 单节目 UDP，E2 当 IPTV 播，已弃用 |

## 部署

```sh
# 1. 驱动（先按 BUILD-KERNEL-MODULE.md 编出两个 .ko，推到盒子 /lib/modules/*/extra/icast/）
sh load-icast-dvb.sh
dmesg | tail -20          # 只看一件事：/dev/dvb/adapter1/frontend0 有没有出来

# 2. 扫描器
#    把 tda1002x.c 交叉编译成 /usr/bin/tda1002x.v6，
#    tda1002x-wrapper.sh 放到 /usr/bin/tda1002x 并 chmod +x
#    E2 调的是 /usr/bin/tda1002x，壳会转给 .v6

# 3. 扫描过滤插件
mkdir -p /usr/lib/enigma2/python/Plugins/Extensions/ScanFilter
cp tools/ScanFilter-plugin.py /usr/lib/enigma2/python/Plugins/Extensions/ScanFilter/plugin.py
# 重启 E2 后看 /tmp/scanfilter.log，第一行应该有 "scan filter v5 active"
```

## 两个必须改参数的地方

`tda1002x-wrapper.sh` 里那两行不是随便写的：

- **`--sr` 被丢掉**。E2 对有线默认 6900 kS/s，这个头端是 6875，而棒子没有 AFC
  去吃掉这 25 kS/s 的偏差，按 6900 调上去就是空的。壳里固定给 6875000。
- **`--scan-stepsize` 也丢掉**。E2 默认 1000 kHz 步进，但棒子的锁频窗口是 ±0——
  251 MHz 在 250 和 253 上都锁不住。改成 8000 kHz 走标准有线栅格，251 和 259 正好都在线上。

## 扫描过滤是怎么做的

不装常驻进程。`ScanFilter-plugin.py` 是 E2 进程内的插件，挂三个钩子：

- `scanStatusChanged`（扫完）：先 `saveServicelist()` 把内存库强行落盘，再解析
  `lamedb5`，把所有 `type != 1` 的服务（2 = 广播、12 = 数据业务，`ad1`–`ad28`、
  `UI-*`、loader 升级那一堆）`removeService` 掉，最后再存一次。
- `ok()`（扫描结果页关闭）：E2 是在这一刻才真正把 lamedb5 写盘的，所以清理要排在
  它后面；同时 `userbouquet.LastScanned.tv` 是拿屏幕上的列表拼的，会带着非电视条目，
  这里一并重写再 `reloadBouquets()`。
- `newService`：只做探测，某些固件 `getLastServiceRef()` 返回空，拿不到就跳过。

实测一轮全段扫描（115–862 MHz）：29 个转发器、258 个频道名入库，最后
`lamedb5` 剩 141 个服务、全部 `type=1`，`LastScanned` bouquet 里非电视 0 条。

## 踩过的坑（都写在代码注释里了）

- **rcS.d 里的脚本不许在前台阻塞**。`/etc/init.d/rc` 是拿 `S*` 通配串行跑 rcS.d 的，
  而 enigma2 由 inittab 在 rcS 返回之后才拉起来。把脚本改名成 `.off` 没用，照样执行。
  因为一条死等 webif 的看门狗脚本，盒子砖过两次。
- **棒子固件停流后不清 FIFO**。上一个频点的 TS 会残留在 bulk-in 里，E2 拿到的第一张
  PAT 是旧 mux，频道就认错了。驱动在 `set_frontend` 末尾排掉大约 390 KB 才算干净。
- **前端是独占的**。E2 一跑就 hold 住 `adapter1/frontend0`，外面 open 拿 `EBUSY`。
  但 E2 自己发起扫描时会先 `stopService` 放开，所以扫描器要耐心等，不能直接报错。
- **不要偷偷退到 adapter0**。adapter0 是盒子自带的 Si 有线前端，看着「能调」，其实跟
  棒子一点关系没有，之前被这个假象骗过一次，现在所有 fallback 都删了。
- **`icast_mode` 默认必须是 dvbc**。热重启后固件缓冲里可能还留着上一轮的 `DTMB:Y`，
  读到了就会把前端翻成 `FE_OFDM`，E2 当 DVB-T 处理，扫完列表是空的、画面是黑的。

## 恢复

`lamedb5` 被改坏或者想找回广播/电台：

```sh
# 扫描前留的备份（盒子上）
ls /etc/enigma2/lamedb5.bak-*
# 或者干脆停用插件
mv /usr/lib/enigma2/python/Plugins/Extensions/ScanFilter /usr/lib/enigma2/python/Plugins/Extensions/ScanFilter.off
```
