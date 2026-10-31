# 更早的 IPTV 方案（已弃用）

在内核驱动还没跑通之前，E2 上先走的这条路：用 `tools/icast-libusb` 从用户态
驱动棒子，把整频点拆成单节目 UDP 推到 localhost，E2 当 4097 IPTV 服务播放。

- `icast-stream`：SysV init 脚本，一个节目一个端口，`/etc/init.d/icast-stream`
- `icast-e2.sh`：手动的 start/stop/status 版本，顺带能生成 bouquet 条目

没继续用的原因不是不能用，而是它本质上是个「假 tuner」：界面上的频道是 IPTV
服务，不能扫描，不能换频点（一个进程钉死一个频点），E2 的信号强度、锁定状态、
盲扫全部无从谈起。后来把驱动搬上 4.4.35 之后，这些都由 E2 自己管了。

留在这里只是因为「怎么把 TS 塞给 E2」这件事上它踩过的坑还有参考价值：整频点
38 Mbit/s 直接喂 UDP，盒子的 demux 在 preroll 阶段就缓冲不过来，必须每个节目
单独重写 PAT 分离出来。
