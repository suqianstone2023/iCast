# E2 侧到底怎么认出这根棒子

这份文档回答的是「驱动编出来了，插上盒子为什么可能还是看不到 tuner」。
答案是 E2 的 USB 通路比想象中绕，而且它有一道硬门槛。

依据全部来自 `BlackHole/enigma2` 分支 `Python3.14` 的 `lib/dvb/dvb.cpp`
（OpenBH 5.6 的用户层源码），以及 4.4.35 内核里 `drivers/media/usb/dvb-usb/`。

## 1. 枚举链路

`eDVBResourceManager::eDVBResourceManager()` 里：

```c
int num_adapter = 1;
while (eDVBAdapterLinux::exist(num_adapter)) {
    if (eDVBAdapterLinux::isusb(num_adapter)) {
        eDVBAdapterLinux *adapter = new eDVBUsbAdapter(num_adapter);
        addAdapter(adapter);
    }
    num_adapter++;
}
```

三个关键点：

- **只从 adapter1 开始扫**（adapter0 留给内置调谐器，单独处理）。
- `isusb(nr)` 先看 `/sys/class/dvb/dvb<nr>.frontend0/device/ep_00` 存不存在，
  再读 `.../device/subsystem` 的符号链接里有没有 `/usb`。我们走 dvb-usb 框架，
  父设备就是 `usb_device`，所以**必然是 true**。
- 进了这个分支，用的就是 `eDVBUsbAdapter`，不是普通 adapter。

## 2. eDVBUsbAdapter 的构造函数

它对我们的驱动提了四个硬要求，逐条对上：

| E2 做的事 | 对驱动的要求 |
|---|---|
| `open("/dev/dvb/adapterN/frontend0")` + `FE_GET_INFO` | 前端必须能打开、能回 info |
| 按 `fe_info.type` 判类型，未知类型直接 `goto error` | **`.info.type` 必须设成 FE_QAM** |
| `open("/dev/dvb/adapterN/demux0")` | demux 必须可用（dvb-usb 自动建） |
| `open("/dev/misc/vtuner{0,1,...}")`，打不开就 `goto error` | **必须有 /dev/misc/vtunerN** |

第三条之后它把 fe_info、类型名、DELSYS 都灌给 vtuner：

```c
ioctl(vtunerFd, VTUNER_SET_NAME, name);
ioctl(vtunerFd, VTUNER_SET_TYPE, type);      // "DVB-C" / "DVB-S2" / ...
ioctl(vtunerFd, VTUNER_SET_FE_INFO, &fe_info);
ioctl(vtunerFd, VTUNER_SET_HAS_OUTPUTS, "no");
ioctl(vtunerFd, VTUNER_SET_ADAPTER, nr);
```

然后 `mappedFrontendName[虚拟前端] = 真实前端`，起一个 pump 线程。
那个映射在 `eDVBAdapterLinux` 打开前端时生效：

```c
std::map<std::string, std::string>::iterator it = mappedFrontendName.find(name);
if (it != mappedFrontendName.end()) name = it->second;
```

**所以调谐是直接打到我们的 `/dev/dvb/adapterN/frontend0` 上的**，
vtuner 只负责让 E2 知道「有这么个 tuner」并且把 PID 列表传达下来。
pump 线程收到 `MSG_PIDLIST` 后，是在**我们真实的 demux0** 上做
`DMX_SET_PES_FILTER`（`DMX_OUT_TSDEMUX_TAP`）+ `DMX_START` / `DMX_ADD_PID`，
再把 demux 读出来的 TS `write()` 进 vtuner 设备。

### 结论

驱动不需要懂 vtuner，它只要是一块**正常工作的 dvb-usb 设备**：
真前端 + 真 demux + 能吐全复用。剩下的 E2 自己桥。

## 3. 这次针对这条链路改了什么

1. **`.info.type = FE_QAM`**
   dvb-core 不填这个字段（`FE_GET_INFO` 直接 `memcpy` `fe->ops.info`），
   而枚举从 0 开始，0 就是 `FE_QPSK`。上游驱动没设，E2 会把有线棒子
   标成 **DVB-S2** 卫星头，用户层根本选不到台。单模式设 `FE_QAM`，
   DTMB 双模式设 `FE_OFDM`。
2. **初始化移出 probe（`smit_async=1`，默认）**
   `smit_init()` 是一百多次 bulk 往返，`smit_check()` 的重试还在
   `msleep(cnt/2+3)` 上递增。放在 `->probe` 里等于把 khubd 按住几十秒，
   那是 4.4 内核里最容易被枚举路径踩出 UAF 的姿势。现在 probe 只登记、
   立刻返回，`smit_init` + `dvb_usb_device_init` 在 workqueue 里跑。
   想回到老行为：`insmod ... smit_async=0`。
3. **`smit_check()` 加了 3 秒上限**
   厂商传进来的 `checkcount` 是 300，最坏情况能睡到一分钟。
   超时就按「命令没完成」返回，而不是继续等。
4. **`smit_init()` 首包失败就返回错误**
   握手第一个命令失败说明 MCU 已经废了（或者被初始化过第二次），
   继续跑完剩下二十几条只是浪费时间，还会注册一个永远锁不上的 tuner。
5. **`read_status` 不再读未初始化栈**
   原来 `smit_tuner_stat` 返回不足 32 字节时仍然读 `buf[29]`。
   现在短回复一律当「还没锁」，并且 `*status` 永远先清 0
   （原来失败路径是不写，调用方拿到的是上次的值）。
6. **锁状态位补全**
   只有 `FE_HAS_SIGNAL | FE_HAS_LOCK` 的话，E2 的信号界面会认为
   carrier/viterbi 都没过。现在给了完整的一套。
7. **`get_frontend` 的频率单位**
   `read_status` 把回读的 kHz 直接写进了以 Hz 为单位的缓存，
   `get_frontend()` 会返回 251000 而不是 251000000。
8. **`read_ucblocks` 的名字**
   4.4 叫 `read_ucblocks`，v4.11 才改名 `read_uncorrected_blocks`。
   按版本条件编译，两边都能编。
9. **端点判断用 `bEndpointAddress`**
   `usb_endpoint_num()` 会掩掉方向位只回 0..15，跟 `0x82` 比恒假，
   结果整个 `smit_probe` 被优化成 12 条指令直接 `-ENODEV`。

## 4. 上机顺序与判据

```sh
# 0. 先确认内核有 vtuner（E2 的硬门槛）
ls -l /dev/misc/vtuner*

# 1. 加载（脚本会等适配器出现并打印 frontend 类型）
sh load-icast-dvb.sh

# 2. driver 侧判据
dmesg | tail -30                   # 应有 "dongle initialised, DVB-C mode"
                                   # 和 "iCast DVBT USB ... successfully initialized"
cat /sys/class/dvb/dvb1.frontend0/name
cat /sys/class/dvb/dvb1.frontend0/type    # 应为 FE_QAM

# 3. E2 视角判据
grep -a "eDVBUsbAdapter" /home/root/logs/enigma2.log | tail
#   应有 "linking adapter1/frontend0 to vtuner0"
#   以及 [eDVBResourceManager] found N adapter ... 里多出一个

# 4. 播台
wget -qO- "http://127.0.0.1/web/zap?sRef=1:0:1:DC:..."
```

## 5. 失败模式对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| 没有 `/dev/dvb/adapter1` | probe 没过 | 看 dmesg 里 `init handshake rejected` / `no answer to the hello exchange` |
| 有 adapter 但 `type` 是 FE_QPSK | `.info.type` 没生效 | 检查是否加载了旧 .ko |
| dmesg 说 `init handshake rejected (-1004)` | MCU 已处于 busy 态（被初始化过第二次，或上次进程没释放） | 拔插/断电重启，让 MCU 复位 |
| adapter 有、E2 列表里没有 | 缺 `/dev/misc/vtunerN` | 装/加载 vtuner 模块，或把 /dev/vtunerN 软链到 /dev/misc/ |
| E2 认了但一调就 fail | demux 没起流 | 检查 `dvb_usb_ctrl_feed` 是否收到 feed（`dmesg` debug） |
| insmod 后立刻 panic | 结构体布局/回调签名不匹配 | 确认 vermagic 与内核一致，确认用的是本仓库的 Makefile.e2 |

## 6. 和之前那条「用户态 vtuner」路线的关系

之前写 `vtunerd` 是想凭空造一个 tuner：用户态进程往 `/dev/misc/vtunerN`
灌 `SET_ADAPTER` / `SET_HAS_OUTPUTS`，让 E2 以为有硬件。
从这份源码看，那条路注定走不通：`eDVBUsbAdapter` 第一步就去打开
`/dev/dvb/adapterN/frontend0` 做 `FE_GET_INFO`，没有真设备就直接 `error`，
vtuner 只是它**桥接真硬件**时用的辅助节点，不是硬件的替代品。

现在这条路的顺序反过来：驱动提供真硬件，E2 自己去桥。
