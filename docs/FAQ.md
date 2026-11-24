# FAQ

**Q：这是 KernelSU/Magisk 的替代品吗？**
不是。它是研究原型 + 极简管理器：没有内核侧授权、没有 per-app 授权弹窗、没有
模块系统。root 只存在于一个驻留进程和它的子进程里。

**Q：为什么不用 Shizuku 启动？**
不是“Shizuku 必然没有 keyring”，而是取决于启动路径：从 adb/adbd 启动（USB、TCP、
Android 11+ 无线配对）的 Shizuku shell 会继承共享的用户会话 keyring，`_ses` 里有
`fscrypt`；从 App 内或 root 方式启动时可能在另一条 session，`_ses` 不含 keyring，
exploit 才会 `readn=0`/ENOKEY。`keyprobe` 的 `search_dest_session` EACCES 是目标
keyring 写权限问题，adb shell 也有，不能当 possessor 判据。
本机 Android 10 没有无线配对（11+），pre-11 的“无线”仍需 PC/root 开 `adb tcpip`，
所以 Shizuku 省不掉 PC；本项目用启动路径可控、依赖最少的 adb 守护进程。
详见 [TECHNICAL.md](TECHNICAL.md)。

**Q：有没有办法完全不用电脑？**
Android 10 没有 Wireless Debugging 配对；pre-11 的 `adb tcpip` 需要 PC/root 先打开。
仓库里记录了一条“一次性 PC 授权 + 用户态 CVE 设属性 + `adb_enabled` 重启 adbd + libadb 连本机”
的未验证思路，阻塞点和风险见 [PC-FREE-ADB.md](PC-FREE-ADB.md)。本项目不实现、不推荐。

**Q：为什么每次开机都要在电脑上跑一条命令？**
root 是内存态，重启回滚；守护进程必须由 adb 会话启动才能拿到正确的
session keyring。之后 App 内是一键。

**Q：会改分区/变砖吗？**
不会改 boot/vbmeta/super，也不写分区。但 exploit 概率性内核 panic 会自动重启；
反复失败时设备可能暂时不稳定。没有重要数据的备用机最合适。

**Q：为什么有时候显示 exploit_failed / 设备自动重启？**
KernelSnitch 的时序判据会被 CPU boost/高负载干扰（`t_empty=0`）；
另有一条 stamp-loss panic 路径。都是已知的：重试/重启后再试。

**Q：为什么 FINAL 了但 /data/data 还是 EACCES/ENOKEY？**
T 没拿到 fscrypt keyring。正常路径是 adb 守护进程；如果用 Shizuku 启动，确认它是
从 adb/adbd 启动的（App 内/root 启动的 session 可能不含 keyring）。看
`keys ... readn=`（0 就是没拿到）和 `keys sess=... search=...`。

**Q：为什么 glsrv 有时起不来？**
serve 里的 `setcon adbd` 可能瞬时 EACCES。守护进程会每 20s 重试，最多 12 次；
仍失败就重启再来。日志里看 `[SV] setcon=`。

**Q：MT 怎么用？**
主设置 → “获取 root 权限” → “自定义 su 命令” = `/data/local/tmp/gl-su`；
先在管理器里授权 MT 的 uid。

**Q：其他 App 怎么申请 root？**
目前没有弹窗式授权。把它的 uid 加进 `/data/adb/gl_allow`（App 的“授权管理”），
然后它 exec `gl-su -c ...` 或自己连 `@ghostlock`。第三方 App 若支持自定义 su
路径，可以填 `/data/local/tmp/gl-su`。

**Q：重启后白名单/App/文件还在吗？**
`/data/adb/gl_allow`、`/data/local/tmp` 文件、已装的 App 都在；root 服务不在。

**Q：能支持别的机型/固件吗？**
不能直接支持。所有内核地址/偏移、SELinux type value、标签策略都按
HLK-AL00 / 2.0.0.270 推导。

**Q：为什么不把 APK 传到应用商店？**
它需要内核漏洞利用才能工作，且每次开机要 adb，不具备普通 App 的分发条件。

**Q：会收集数据吗？**
不会。App 没有网络权限；所有操作都在本机。