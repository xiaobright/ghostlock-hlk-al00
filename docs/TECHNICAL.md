# 技术说明（HLK-AL00 / HarmonyOS 2.0.0.270）

> 本文只记录本项目在这台设备上的实现与实测结论，便于复现和理解。
> 所有偏移/地址都是**该固件专属**，换版本必须重新推导；不要照搬到其他机型。

## 1. 漏洞与目标

- 漏洞：Linux futex PI 路径的 `remove_waiter()` 误清了 `current->pi_blocked_on`（而非 waiter
  自己的任务），配合 `FUTEX_CMP_REQUEUE_PI` 的 EDEADLK 回滚，可让一个 waiter 的
  `pi_blocked_on` 悬空指向其栈上的 `rt_waiter`。本项目按公开研究 GhostLock
  （CVE-2026-43499）实现 4.14 ARM64 利用。
- 目标：HONOR 9X HLK-AL00，HarmonyOS 2.0.0.270（Android 10），kernel 4.14.116，KASLR，
  SELinux enforcing，`panic_on_oops=1`。
- 约束：运行期 root，不改 boot/vbmeta/super、不动分区；重启完全回滚。

## 2. 利用链概览

1. **mm_struct 泄露（KernelSnitch）**
   fork 子进程创建/释放 `mm_struct`，通过 futex 哈希桶的唤醒时延差异定位被释放的
   `mm_struct` slab；多候选做“pile 开/关/再开”三相确认，再用哈希约束爆破出完整
   `mm_struct` 地址。
2. **页回收与伪造对象**
   释放 order-3 的 mm slab（DISCARD）→ 用 AF_UNIX DGRAM 3776 字节 skb 载荷回收
   kmalloc-4k 对象（页 S）→ MSG_PEEK 读回验证。伪造 `rt_mutex`/`rt_waiter`/`task`
   结构与 fake 锁链都放在页 S 内。
3. **MCAST 盖章**
   `setsockopt(fd, SOL_IP=0, 43 MCAST_BLOCK_SOURCE, stamp, 0x108)` 把伪造 waiter
   写进内核栈旁路区（`W = E-0x1d0`），配合 `sched_setattr` 触发
   `rt_mutex_adjust_prio_chain` 的 walk。
4. **rb_erase_cached 写原语**
   walk 中对 fake 锁的 `rb_erase_cached`/重插产生有限写：
   - right 变体：`[pc+8] = right`，副作用 `[right] = pc`
   - leaf-zero：`[pc+8] = 0`
   - left 变体：见源码注释
   规则：**right 非 0 只能打从未插入过 waiter 的 fresh 锁**；复用锁必须 `right=0`
   （本场景 left 非 0 安全，遍历等 prio 走 right）。
5. **slide 与提权**
   用 fake `sched_class` 指针读回 `rt_sched_class` → slide；
   `init_cred = 0xffffff800ae39ce8 + slide`，`init_user_ns = 0xffffff800ae37ff0 + slide`。
   顺序：`pid=0`（+0x830）→ `real_cred`（+0x9F0）→ `cred`（+0x9F8）→ gidfix。
   `pid=0` 同时用于绕过厂商 HKIP 的 `prepare_creds` 检查（`hkip_check_xid_root` 对
   pid==0 放行）。
6. **SELinux permissive 窗口**
   将 `policydb.permissive_map` 的节点指针/高位置指向页 S 内的伪造 ebitmap：
   - 位索引 = **type value**（不是 SID、不是 index-1）：kernel=786、shell=1160
   - 本实现节点 `startbit=777`，`maps[0]=1<<9`（786），`maps[5]=1<<63`（1160）
   - `highbit` 写在 `policydb+0x1C8`（必须大于最高位）
   仅 root 会话期间有效；`enforce` 对外仍读 1，应用域不受影响。
7. **fscrypt 密钥交接**
   T（fake cred 后丢失 shell 的 session keyring）`KEYCTL_JOIN_SESSION_KEYRING(NULL)`
   建 K2 并 `SETPERM(0x3f3f3f3f)`；main（仍是 shell creds，session 里有 `fscrypt`
   keyring）用 `KEYCTL_SEARCH(KEY_SPEC_SESSION_KEYRING, "keyring", "fscrypt", dest=K2)`
   把密钥环链进 K2，随后 T 的 `request_key("logon","f2fs:<id>")` 才能命中。
   - 注意 `KEYCTL_LINK` 内核参数顺序是 **arg2=key、arg3=keyring**；按 serial LINK
     拿不到 possessor 位。
   - 密钥是 logon 类型，payload 用户态不可读，不能复制；只能交接 keyring。

## 3. Shizuku 启动路径与 fscrypt keyring

fscrypt keyring 不是挂在 uid 上，而是用户会话级共享的 keyring；只要进程从
adbd 的 shell 链上 fork/exec，它就继承 `_ses`（含 `keyring fscrypt`）。

实测（同一台机器，`service/keyprobe.c` + Shizuku `rish`）：

- 从 adb 启动的 Shizuku（USB 或 TCP）：`rish` 出来的 shell `_ses` serial 与
  adb shell 相同，`fscrypt` 子项相同；`adb tcpip 5555` 重启 adbd 后 serial
  都不变（keyring 是共享的用户会话级，不是 adbd 实例私有）。
- 从 App 内/root 方式启动的 Shizuku：服务端可能在另一条 session；失败日志
  `sess=319751070 search=-1/126 mlink=-1/13 ... join=0/0 mysess=958474601
  readn=0`，`kproc` 显示 fscrypt logon key 在 `_uid_ses.0`，T 侧 `nd0=982838717/0`
  能找到，但 main 的 `_ses` 不含 fscrypt、link 失败（`linkring=-1/13`）。
- `keyprobe` 的 `search_dest_session` 在 adb shell 同样 EACCES（目标 session
  keyring 写权限），不是 possessor 判据；要看 main 的 `_ses` 是否含 `fscrypt`
  以及能否 SEARCH/LINK 到 T 的 K2。

结论：Shizuku 的 shell **可以**带 fscrypt keyring，条件是服务端由 adbd shell
启动；Android 10 没有免 PC 的无线启动（Wireless Debugging 配对是 11+，pre-11 的
“无线”只是 `adb tcpip`，需要 PC/root 先开），所以本项目仍用 adb 守护进程：
启动路径可控、依赖最少。不是“Shizuku 必然没有 keyring”。

## 4. root 服务（glsrv）与客户端（gl-su）

- 服务端 socket 必须让普通应用可 `connectto`。设备策略里应用域只对 **adbd** 等少数标签
  放行；因此 serve 子进程先 `setcon u:r:adbd:s0`，创建 abstract `@ghostlock`，再
  exec `/system/bin/sh`（`typetransition adbd shell_exec process shell`）。
- 旧版服务是单连接；MT 会保持一个常驻 shell，会把服务占满。现版本每个 accept
  fork 一个 handler（多连接）。
- handler 用 `SO_PEERCRED` 取客户端 uid，读 `/data/adb/gl_allow`（每行 uid 或 `*`；
  文件缺失时只放行 2000）。未授权连接直接回 `not authorized` 并关闭。
- 命令由 handler fork + `sh -c` 执行，stdout/stderr 直接写回 socket，末尾跟
  `{magic 'GLD1', i32 status}` 尾包；客户端读到 EOF 后取退出码。
- `gl-su` 是静态 aarch64 客户端：`-c <cmd>` 一次性，或无参数交互（把 stdin/stdout
  与远端 `sh` 双向转发）。MT 用的是交互模式（stdin 发命令，按
  `MT - End of command` sentinel 分段，并解析 `id` 的 uid）。
- 普通应用只能 exec 标签为 `shell_exec` 的文件；所以部署后要把
  `/data/local/tmp/gl-su` 用 `setlabel` 改成 `u:object_r:shell_exec:s0`。
  注意：改完后 shell/adbd 没有该文件的写权限，**更新 gl-su 前**要先用 broker
  改回 `shell_data_file`。

## 5. 启动链（为什么需要 adb 守护进程）

- App（`com.ghostlock.manager`）不能自己获得 shell 域，因此在 App 外部目录放
  `glboot.sh`；用户每次开机在 PC 上执行弹窗里的 adb 命令启动它。
- 守护进程继承 adb shell 的 `_ses` keyring（fscrypt possessor），轮询
  `gltrigger`，触发后：
  `sync_bins` → 8s settle（等 UI/触摸 boost 衰减，KernelSnitch 的空 futex 计时会被
  高频压到时钟量化以下，`t_empty=0` 会 REJECT）→ `ghost1 root`（≤6 次）→
  `serve`（失败每 20s 重试，≤12 次）→ `setlabel gl-su shell_exec` → `ready`。
- 状态/心跳写入 App 外部目录，App 轮询显示 `settling/serving/ready/...`。
- `serve` 里 `setcon adbd` 偶发瞬时 EACCES（实测隔几秒~几分钟重发会成功），所以有
  重试；失败的子进程直接 `_exit(1)`，不泄漏 fd。
- exploit 有概率性 stamp-loss panic（`panic_on_oops=1` 自动重启）。重启后守护进程
  消失，需要重新执行 adb 命令并再点一次启动；通常 1–3 次内成功。

## 6. 实测证据（摘要）

- 守护进程路径：`trigger → FINAL(tmp=.../0) → [SV] setcon=11/0 → glsrv →
  id -u=0 → ls /data/data/... 正常`。
- App（uid 10193）经 `gl-su` 连服务，界面显示 “root 可用（uid 0）”；终端 `id`
  输出 `uid=0 ... context=u:r:shell:s0`。
- MT 自定义 su（`/data/local/tmp/gl-su`）可对 `/data/adb`、`/data/data`
  建/删/改；并发（MT 常驻 shell + 第二客户端）正常。
- Shizuku 启动路径的对照与更正后的结论见第 3 节。

## 7. 风险与边界

- 仅适用于 HLK-AL00 / 2.0.0.270；所有地址、偏移、符号都硬编码。
- root 是内存态；重启回滚。不要手动 kill 持有 fake creds/pid=0 的进程（可能导致
  teardown panic）；结束一律重启。
- 触发文件在 App 外部目录，具备存储权限的应用可重复触发 exploit（但不能绕过
  su 的 uid 白名单）。后续可加 nonce。
- 这些技术只应在自己的设备上用于研究与学习。