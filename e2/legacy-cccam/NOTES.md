# iCast USB 棒 → Enigma2（GigaBlue UHD Trio 4K / OpenBH 5.6）

## 结论先说

整条链路已经跑通并截图验证过：棒子出 TS → CCcam 取 CW → libdvbcsa 解扰 →
HTTP 推给 Enigma2，屏幕上出的是干净的 CCTV-1 1080 画面。

**但 2026-10-31 02:00 左右盒子被我的一条开机脚本锁死在 rcS 阶段，起不来了。**
原因见下面「事故」。

## 架构

```
USB 棒 (29df:0001)
  └─ icast-cli  (libusb, 直接驱动棒子, 输出整频点 TS 到 stdout)
       └─ icast_cw.py  (单进程)
            ├─ 抓 ESM/PID 2205 的 ECM
            ├─ CCcam 客户端 → 读卡服务器取 CW（地址/端口/账号走环境变量）
            ├─ libdvbcsa 解扰 (按包 ctl 位选 odd/even key)
            └─ HTTP 1.1 chunked :8110/stream  (单节目 2202视频 + 2203音频)
                 └─ Enigma2 4097 服务 (bouquet 里两条: /stream 与 /stream2)
```

E2 侧不做任何 vtuner/demux 绑定 —— 那条路已证明走不通（vendor vtuner 的
demux 永远收不到 PIDLIST）。界面里的「调谐器 C — iCast DVB-C USB」来自
`/etc/enigma2/nimfix/nim_sockets` 覆盖 NIM Socket 2 的类型，纯展示 + 配置用。

## 文件清单

| 路径 | 作用 |
|---|---|
| `/home/root/icast/icast-cli` | ARM 版 libusb CLI（**只存在盒子上，本地无副本**） |
| `/home/root/icast/icast_cw.py` | 解扰 + HTTP 主程序（本目录同名文件） |
| `/etc/rcS.d/S51icast-nimfix` | bind mount nim_sockets，让 Socket2 显示为 DVB-C |
| `/etc/rcS.d/S52icast-cw` | 开机拉起 icast_cw.py（等端口 8110 就绪后再返回） |
| `/etc/rcS.d/S53icast-watch` | 看门狗：E2 停在 iCast 但没连流 → 切到另一条 ref 强制重调 |
| `/etc/enigma2/userbouquet.icast.tv` | 两条 4097 服务 iCast Live / iCast Live B |
| `/etc/enigma2/nimfix/nim_sockets` | 本目录 nim_sockets 覆盖到 /proc/bus/nim_sockets |

`/etc/rcS.d/` 里**不要**保留 `S50icast-vtunerd`、`S98icast-tuner`、`S99icast-stream`：
`icast-vtunerd2` 会独占 USB 设备，icast-cli 就永远 "Resource busy"。

## 事故：为什么起不来

`/etc/init.d/rc` 的循环是 `for i in /etc/rc$runlevel.d/S*` —— **改名成 `.off`
并不能禁用**，`S50icast-vtunerd.off` 照样执行。所以每次开机 vtunerd2 都在抢棒子。
这条已经修掉（直接删除这三个软链）。

真正致命的是我自己写的 S53：为了让看门狗等 E2 的 webif 起来，我把它改成了
**无限循环等待**。而 rcS 是**串行执行** S* 脚本的，enigma2 由 inittab 在 rcS
返回之后才启动 —— 于是变成：

```
rcS → S53 死等 E2 webif → E2 永远不启动 → 死锁
```

表现：能 ping 通（网络在更早的脚本里已起），但 2201(ssh) 和 888(webif) 都不通。
修复版 S53 已经改成 `start` 只 fork worker 然后立刻返回，绝不在前台等待。

**教训：rcS.d 里的脚本一律不允许在前台 blocking。**

## 恢复（盒子已死，只能物理操作）

1. **先试救援/引导菜单**（不丢数据）：断电，按住前面板电源键再上电。
   若出现引导菜单，选 Rescue / Recovery；只要网络起来且 sshd 在，
   我就能从 2201 登进去删掉 `/etc/rcS.d/S53icast-watch` 再重启。
2. **不行就 U 盘重刷**：OpenBH 5.6 for `gbtrio4k`，解压到 FAT32 U 盘根目录
   （会生成 `gbtrio4k/` 目录，里面含 bootargs.bin / apploader.bin / fastboot.bin
   和镜像），断电插盘、按住电源键上电，按屏幕提示刷。
   **代价：/etc 和 /home/root 全清**，本目录文件要重新推一遍；
   而且 `icast-cli` 这个 ARM 二进制没有本地副本，需要重新交叉编译
   （上游 nxdong520/iCast 源码，armv7 工具链）。

## 未完成

- [ ] 恢复盒子后：推 S53 修复版，删掉任何 `S5*...off` 残留
- [ ] 重启验证一次「开机自启 → E2 自动出画面」（此前只验证到手动 zap 出画面）
- [ ] 备份 `icast-cli` ARM 二进制到本地 / 重新交叉编译并归档
- [ ] 把胶水代码整理进 fork 仓库（不改上游驱动）
