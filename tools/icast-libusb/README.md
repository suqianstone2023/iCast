# icast-libusb

SMIT iCast DTMB/DVBC 电视棒（`29df:0001`）的**用户态驱动**：直接用 libusb 驱动设备，
不加载任何内核模块，不产生 `/dev/dvb`，**与内核版本完全无关**。

换内核、换发行版、换到 ARM 盒子（树莓派、香橙派），只要 libusb 能枚举到设备就能跑。

## 什么时候用它，什么时候别用

| | 内核驱动（`/dev/dvb`）| 本工具（libusb）|
|---|---|---|
| 内核依赖 | 要，每个内核重编 | 无 |
| tvheadend 完整功能 | 能扫描、能自动切频、多路复用器调度 | **只能固定输出一个频点** |
| VLC / `w_scan` / dvbv5-tools | 能 | 不能（没有 `/dev/dvb`）|
| 同时多个程序读同一设备 | 能（DVB demux 共享）| 不能，独占 |
| 硬件 PID 过滤 | 驱动也没实现 | 同样没有，输出整路复用 |

**结论**：要拿电视棒当长期电视后端（扫描、多频点、自动切台），用内核驱动。
本工具适合：单频点录制 / 监测、不能加载内核模块的环境、跨平台临时使用、
以及把某一路复用当 IPTV 源喂给 tvheadend。

## 构建

```bash
# Debian / Ubuntu
sudo apt install build-essential libusb-1.0-0-dev
# Fedora
sudo dnf install gcc make libusb1-devel
# macOS
brew install libusb

make
sudo make install     # 装到 /usr/local/bin，并（以 root 运行时）安装 udev 规则
```

## 使用

```bash
# 看看插了哪些
icast-libusb list

# 设备信息：双模还是单模、识别字符串
icast-libusb info

# 调 474 MHz，持续打印锁定状态 / 信号 / 信噪比（Ctrl-C 退出）
icast-libusb tune -f 474 -M

# 有线（DVBC）474 MHz、6875 kSym/s、64QAM
icast-libusb tune -f 474 --dvbc -r 6875 -m 64

# 录 30 秒复用到文件
icast-libusb stream -f 474 -o /tmp/mux.ts -t 30

# 把这一路复用推成 UDP，给 tvheadend 当 IPTV 输入
icast-libusb stream -f 474 -o udp://239.1.1.1:5000
```

参数说明：

```
-d, --device N       第 N 个设备（默认 0）
-f, --freq MHZ       中心频率，单位 MHz，可带小数
-C, --dvbc           走 DVBC 分支（默认走 DTMB/DVBT 分支）
-r, --symbol-rate K  DVBC 符号率，单位 kSym/s（默认 6875）
-m, --qam N          DVBC 星座：16/32/64/128/256（默认 64）
-b, --bandwidth MHZ  DTMB 带宽（默认 8）
-o, --out TARGET     '-' 表示 stdout，或文件路径，或 udp://host:port
-M, --monitor        调谐后持续打印状态
-t, --seconds N      运行 N 秒后退出
-q, --quiet          不往 stderr 打状态行
```

`stream` 会把 TS 输出到 stdout、文件或 UDP，状态行一律走 stderr，所以 `-o -` 可以直接管道：

```bash
icast-libusb stream -f 474 -o - | ffmpeg -i - -c copy out.mp4
```

## 接 tvheadend

1. 先起推流：`icast-libusb stream -f 474 -o udp://239.1.1.1:5000`
2. tvheadend → 配置 → DVB 输入 → 网络 → 添加 → 类型选 **IPTV Automatic Network**
3. URL 填 `udp://@239.1.1.1:5000`，保存
4. 在服务页里给它加复用器（频率随便填，实际由本工具决定）

**注意**：IPTV 输入下 tvheadend **不能反过来控制调谐**——工具推哪个频点，它就只收到哪个频点。
想换台只能杀掉进程换个 `-f` 重来。要扫描全部频点请用内核驱动。

## 权限

非 root 运行时需要 udev 规则（`make install` 以 root 身份会装好）：

```
SUBSYSTEM=="usb", ATTR{idVendor}=="29df", ATTR{idProduct}=="0001", MODE="0660", GROUP="plugdev", TAG+="uaccess"
```

装完 `sudo udevadm control --reload && sudo udevadm trigger`，把用户加进 `plugdev` 组，
重新登录生效。

也可以直接 `sudo icast-libusb ...`，但那样写出来的 TS 文件属主是 root。

## 与内核驱动共存

工具打开设备时会自动把内核驱动 detach 掉（`libusb_set_auto_detach_kernel_driver`），
进程退出后设备回到内核驱动的控制。但更干净的做法是用之前先卸掉：

```bash
sudo modprobe -r dvb_usb_icast_smit dvb_usb_icast_fe
```

否则两边抢同一个设备容易出奇怪的现象（比如 tvheadend 以为 adapter 还在，实际读不到流）。

## 测试

sink（TS 对齐 + UDP 分包）有独立测试，不需要硬件：

```bash
make test
```

覆盖两个容易在无声无息中搞坏下游的场景：开头有无效字节、一个 TS 包被两次 USB 读切断；
以及 UDP 报文必须凑满 7 个包（1316 字节），否则接收端会丢掉每个报文的尾巴。

## 协议来源

命令表、时序、握手全部照搬内核驱动 `smit.c` / `icast-fe.c`，**一个字节都没有改**：
协议是 USB 抓包逆出来的，没有文档可以对照"更干净"的实现。工具内的唯一差别都是防御性的
（收包前清零、声明的长度做上限钳制、轮询循环加上限）。

信号强度 / 信噪比沿用了内核驱动里那段定点数换算，所以读数和 `/dev/dvb` 下的表现一致。

## 许可

GPL-2.0-or-later。
