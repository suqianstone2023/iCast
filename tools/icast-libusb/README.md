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

### 交叉编译（ARM 盒子 / Enigma2 等）

代码只依赖 libusb 和 libc99，交叉编译没有任何额外步骤。静态链接 musl 后得到
一个不依赖目标机 libc 的单文件，直接 scp 到设备就能跑：

```bash
# 1. 交叉 libusb（静态）
CC=/path/to/aarch64-linux-musl-gcc /path/to/libusb-1.0.27/configure \
    --host=aarch64-linux-musl --disable-udev --enable-static --disable-shared \
    --prefix=/tmp/libusb-a64 && make -j && make install

# 2. 本工具（静态）
make clean
make CC=/path/to/aarch64-linux-musl-gcc \
     LIBUSB_CFLAGS="-I/tmp/libusb-a64/include/libusb-1.0" \
     LIBUSB_LIBS="/tmp/libusb-a64/lib/libusb-1.0.a -static"
```

`LIBUSB_LIBS` 末尾的 `-static` 生成 static-pie 单文件（aarch64 约 650 KB）。
armv7hf（`arm-linux-musleabihf`）同理。已验证两个架构均零 warning 编出。

Enigma2（OpenBH / OpenATV 等）机顶盒没有内核 headers、厂商内核也拿不到源码配置，
是这种用法最典型的场景：盒子本地跑 `stream -o udp://127.0.0.1:5000`，
在 E2 里以 4097（IPTV）服务类型播放 `udp://@127.0.0.1:5000` 即可，
不需要动固件、不需要装内核模块。

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
                     可重复，每个 -o 都拿到完整复用
-p, --program N=URL  只输出第 N 个节目（PAT 会重写成单节目），可重复
-M, --monitor        调谐后持续打印状态
-t, --seconds N      运行 N 秒后退出
-q, --quiet          不往 stderr 打状态行
```

`stream` 会把 TS 输出到 stdout、文件或 UDP，状态行一律走 stderr，所以 `-o -` 可以直接管道：

```bash
icast-libusb stream -f 474 -o - | ffmpeg -i - -c copy out.mp4
```

## 一个节目一路输出（机顶盒、单频道录制）

一个频点上的节目是打包在一起的，整路复用几千万 bit/s 里大部分是你不看的那几套。
`-p` 把某一路节目单独切出来，并且**重写 PAT**（只留这一个节目，CRC 重新算），
下游拿到的就是一条干净的单节目流：

```bash
# 先看有几个节目、各自的节目号
icast-libusb stream -C -f 251 -o /tmp/mux.ts -t 5
python3 -c "
import sys; b=open('/tmp/mux.ts','rb').read()
i=0
while i+188<=len(b):
    p=b[i:i+188]
    if p[0]==0x47 and (((p[1]&0x1f)<<8)|p[2])==0 and (p[1]&0x40) and p[5]==0:
        j=8
        while j+4<=3+(((p[6]&0xf)<<8)|p[7])-4:
            print('program', (p[j]<<8)|p[j+1], 'pmt 0x%04x' % (((p[j+2]&0x1f)<<8)|p[j+3])); j+=4
        break
    i+=188"

# 每个节目一路，各自一个端口
icast-libusb -C -f 251 stream \
    -p 220=udp://127.0.0.1:5001 \
    -p 221=udp://127.0.0.1:5002 \
    -p 222=udp://127.0.0.1:5003
```

为什么要切：整路复用交给机顶盒的解复用器时，它要按峰值码率留缓冲，很容易在
preroll 阶段就卡住；单节目流的码率只有五分之一，稳定得多。顺带也解决了
"一路复用里挑一个节目播"这件事——每个端口就是一台频道。

`tests/tsfilter_test.c` 用手工构造的 PAT/PMT 覆盖了这段逻辑（含 PAT 重写与 CRC 校验），
`make test` 一起跑。

## 电视棒解不出来的情况：加扰

有线/卫星上很多频点是**条件接收（CA）加密**的。TS 头里有两个 bit 标着
`transport_scrambling_control`，非 0 就表示这一段负载是密文。电视棒的固件不做解扰，
也没有 CI/CAM 通道，所以：

- 节目表、节目名、PID 全都正常读得出来（PAT/PMT 是明文）
- 播放器能拿到数据、能列出频道，但**永远黑屏**
- 收下来的 TS 里码流连续性也是好的，看不出毛病

先确认一下再怀疑代码：

```bash
ffprobe -hide_banner udp://127.0.0.1:5001     # 能看到节目 = 表正常
# 数一下加扰包：非 0 就是加密的
python3 - <<'EOF'
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",5001)); s.settimeout(5)
b=bytearray()
while len(b)<400000:
    try: b+=s.recvfrom(65536)[0]
    except socket.timeout: break
i=t=c=0
while i+188<=len(b):
    if b[i]==0x47:
        t+=1; c+=1 if (b[i+3]>>6)&3 else 0
    i+=188
print("scrambled %d/%d" % (c,t))
EOF
```

明文频点这个比例应该是 0。这条路上唯一能播的是运营商没加密的节目，
或者把卡插进带 CI 槽的机器用它的调谐器去解。

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
