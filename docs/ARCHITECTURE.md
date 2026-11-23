# 架构与实现

本文描述各组件、接口、生命周期和关键设计决策。漏洞细节见
[EXPLOIT.md](EXPLOIT.md)，协议细节见 [API.md](API.md)。

## 1. 总览

```
┌──────────────┐   adb(shell)   ┌──────────────────────────┐
│ PC (adb)     │ ─────────────> │ glboot.sh 守护进程        │
└──────────────┘  setsid nohup  │ 心跳 / trigger 轮询       │
                                └───────────┬──────────────┘
                                            │ 收到 start
                                            v
┌──────────────┐   gltrigger    ┌──────────────────────────┐
│ 管理器 App    │ ─────────────> │ ghost1 (exploit)         │
│ com.ghostlock│   glstatus     │  KernelSnitch → 提权      │
│ .manager     │ <───────────── │  → broker(T, uid0)       │
└──────┬───────┘                └───────────┬──────────────┘
       │ exec /data/local/tmp/gl-su          │ serve: fork 子进程
       │                                    │ setcon adbd + abstract socket
       v                                    v
┌──────────────┐   connect @ghostlock  ┌──────────────────────────┐
│ gl-su 客户端  │ ───────────────────> │ glsrv (adbd 标签 socket)  │
│ shell_exec   │ <── 输出 + 状态尾包 ── │ 每连接 fork handler       │
└──────┬───────┘                       │ SO_PEERCRED + uid 白名单  │
       │                               │ fork sh -c 命令（root）   │
       │                               └──────────────────────────┘
       │  同一接口
┌──────┴───────┐
│ MT 管理器     │  自定义 su 命令 = /data/local/tmp/gl-su
│ 常驻交互 shell│  实测可读写 /data/adb、/data/data
└──────────────┘
```

## 2. 组件

| 组件 | 文件 | 凭据/域 | 作用 |
|---|---|---|---|
| 利用器 | `exploit/ghost1.c` | 启动时 shell，提权后 T 为 uid0 | 泄漏、写原语、提权、broker |
| broker main | `ghost1.c` 的 main 线程 | shell creds | 轮询邮箱、转发 BK 操作、输出 |
| broker T | `ghost1.c` 的触发线程 | fake `init_cred`（uid0） | 执行 BK 操作、服务 fork |
| 服务端 | `service/glsrv.c` | 由 broker 派生，uid0；socket 标签 adbd | 接受连接、授权、执行命令 |
| su 客户端 | `service/gl-su.c` | 调用者的 uid/域 | 连接服务、转发命令/交互 |
| 守护进程 | `scripts/glboot.sh` | adb shell（uid 2000） | 每次开机拉起 exploit + serve |
| 管理器 | `manager/` | untrusted_app（uid 10193） | 状态/终端/授权/日志/触发 |
| 诊断 | `service/keyprobe.c` | 调用者 | 对比 keyring 权限（Shizuku 结论） |

## 3. broker 与邮箱

- 邮箱路径固定为 `/data/local/tmp/ghost_cmd`（命令，一次一行）和
  `/data/local/tmp/ghost_out`（当前命令 stdout，0666）。
- main（shell creds）负责读写邮箱；T（uid0）通过共享内存原子变量
  `g_bk_req/g_bk_done` 执行 `bk_exec()`（`bk_call()` 15s 超时）。
- 命令集（详见 [API.md](API.md)）：`id / ls / stat / cat / hex / writehex /
  sysctl / chmod / mkdir / unlink / sync / probe / forktest / exectest /
  serve / setlabel`。
- `serve` 由 T fork 子进程：优先用预建 fd（当前未启用），否则 child 写
  `/proc/self/attr/current = u:r:adbd:s0`（8×100ms 重试）→ 建 abstract
  `@ghostlock` → `exec /system/bin/sh -c "exec /data/local/tmp/glsrv 3"`。
- `setlabel` 用 `setxattr("security.selinux")` 改文件标签；用于把 `gl-su`
  改成 `u:object_r:shell_exec:s0`，让普通应用可以 exec。

### 3.1 为什么 T 不能退出

T 的 `real_cred/cred` 被写成静态 `init_cred`，`task->pid=0`。它是整个 root
能力的载体；退出/被 kill 可能触发 teardown panic。结束一律重启设备。
broker main 是普通 shell creds，可以随进程结束，但没必要。

## 4. glsrv（root 服务）

- 监听：abstract AF_UNIX `@ghostlock`，SOCK_STREAM，backlog 8。
- 服务进程由 `serve` 子进程 exec 而来；socket 标签在 `setcon adbd` 后创建，
  因此是 `adbd`。设备策略中应用域对 `adbd` 标签 socket 有 `connectto` 与
  read/write，所以普通应用可以连——这是整套方案不用给应用域开 permissive 的
  关键。
- 多连接：accept 后 fork 一个 handler 进程独占该连接，父进程继续 accept；
  handler 内再 fork 命令子进程并 waitpid，最后写尾包、关闭。
- 授权：`SO_PEERCRED` 取 uid/pid，逐连接读 `/data/adb/gl_allow`
  （每行 uid 或 `*`；文件缺失时只允许 2000）。未授权会回
  `gl-su: uid <n> is not authorized` + 状态 126。
- 执行：`fork` + `execl("/system/bin/sh","sh","-c",cmd)`，stdin/stdout/stderr
  都指向连接；子进程退出后由 handler 写 `{GLS1 响应 + GLD1 尾包}`。
- 继承：服务进程由 T 的 fork 链而来，继承 T 的会话 keyring（含 fscrypt keyring）
  与 root creds，所以命令可访问 CE 目录。

## 5. gl-su 客户端

- 静态 aarch64，放在 `/data/local/tmp/gl-su`，标签需为 `shell_exec`。
- `-c <cmd>`：一次请求，流式读输出，末尾 8 字节若为 GLD1 则作为退出码，不打印。
- 无 `-c`：交互模式。发送 `exec /system/bin/sh`，之后用 `poll` 双向转发
  stdin/stdout 与 socket；不剥尾包（尾包只在会话结束出现）。
- MT 的调用约定：先 `gl-su -h` 探测，然后 `Runtime.exec` 拉起常驻进程，
  stdin 发命令 + `echo 'MT - End of command'` sentinel，解析 `id` 的 uid。
  客户端对未知 flag 不报错，直接进交互模式。

## 6. 管理器 App

- 包名 `com.ghostlock.manager`，targetSdk 29（untrusted_app 域），uid 10193。
- 启动时/按钮操作在 IO 线程执行 `/data/local/tmp/gl-su -c ...`：
  - 状态：`id -u` 解析“root 可用/未授权/服务未运行”；
  - 终端：任意命令，显示输出和 `[rc=...]`；
  - 授权管理：`cat /data/adb/gl_allow` 读当前 uid，列出安装应用（label/pkg/uid），
    保存时用 `printf '%s' '<uids>' > /data/adb/gl_allow`（始终保留 2000，
    保留已卸载应用的 uid）；
  - 日志：经 root `tail` glsrv.log/rt.log，并直接读外部目录的
    `gldaemon.log`/`glstatus`。
- 启动 Root：优先检查 `glheartbeat`（≤30s）判守护进程是否在线；在线则写
  `gltrigger`，轮询 `glstatus`（最多 420s）；不在线则弹窗显示 adb 命令（可复制）。
- App assets 内嵌 `ghost1/glsrv/gl-su/start_root.sh/glboot.sh`；首次启动 Root
  或首次点击时解压到自己的外部目录，守护进程从这里同步到 `/data/local/tmp`。
- Shizuku 代码/Provider 仍保留，但启动不用它（原因见
  [TECHNICAL.md](TECHNICAL.md) 第 3 节）。

## 7. 守护进程 glboot.sh

- 由用户在 PC 上 `adb shell setsid nohup sh .../glboot.sh <dir>` 启动；继承 adb
  shell 的 `_ses` keyring（fscrypt possessor）。
- 启动时 `sync_bins`（md5 比对，复制 ghost1/glsrv/gl-su 到 /data/local/tmp 并
  chmod），写 `glstatus=idle`，每 10s 更新 `glheartbeat`（epoch 秒）。
- 主循环每 1s 看 `gltrigger`：
  - `start`：若 glsrv 已在跑 → ready；否则 sync、清空 prog/pls 日志、写
    `settling`、sleep 8s（触摸/UI boost 衰减）、最多 6 次跑 `ghost1 root`
    （每次清 rt.log，最多等 150s，FINAL/失败即停，保存 `rt_attN.log`）、
    写 `serving`、每 20s 发一次 `serve` 最多 12 次、成功则 `setlabel`
    `gl-su` 为 shell_exec 并写 `ready`；失败写 `exploit_failed`/`serve_failed`。
  - `stop`：退出（不影响已起的 broker/服务；它们要继续服务只能重启清理）。

## 8. 生命周期

```
开机(干净) → adb 启动 glboot.sh → App 检测心跳
   → 写 trigger → settling(8s) → exploit(≤6 次)
   → serving(setcon adbd, serve 20s×12) → ready
   → App/MT 经 gl-su 使用 root
   → 重启：broker/服务/守护进程全部消失，内核内存回滚
```

- 概率性失败：exploit 的 stamp-loss 会内核 panic（`panic_on_oops=1`）自动重启；
  重启后重跑 adb 命令、再点启动。
- `serve` 的 setcon 偶发瞬时 EACCES：由 20s 间隔重试兜底；失败子进程直接退出，
  不泄漏 fd。
- 已知失败日志：`rt_mutex_adjust_prio_chain+0x140/+0x30c` panic；f2fs 回滚
  可能留下旧日志，判断成功以 boot_id/uptime 和 O_SYNC 的 prog.log 为准。

## 9. 文件与路径

| 路径 | 说明 |
|---|---|
| `/data/local/tmp/ghost1` | 利用器（shell_data_file） |
| `/data/local/tmp/glsrv` | 服务端 |
| `/data/local/tmp/gl-su` | 客户端（需改 shell_exec） |
| `/data/local/tmp/keyprobe` | keyring 诊断（可选） |
| `/data/local/tmp/ghost_cmd|ghost_out` | broker 邮箱 |
| `/data/local/tmp/rt.log|prog.log|pls.log` | exploit 日志 |
| `/data/local/tmp/rt_attN.log` | 守护进程按次保存的日志 |
| `/data/local/tmp/glsrv.log` | 服务日志（O_APPEND，0666） |
| `/data/adb/gl_allow` | uid 白名单（持久） |
| `<app外部目录>/ghost/` | App 解压的二进制、glboot.sh、gltrigger、glstatus、glheartbeat、gldaemon.log |
| `@ghostlock` | abstract socket（内核命名空间，无文件） |

## 10. 安全模型与边界

- `gl-su` 被普通应用 exec：策略允许应用域执行 `shell_exec` 标签文件；
  因此客户端本身可以放在 `/data/local/tmp`（无需给应用域 permissive）。
- socket 标签是 `adbd`：应用域有 `connectto`；`SO_PEERCRED` + uid 白名单
  是授权边界。任何未授权应用都能连上但会被拒绝。
- SELinux permissive 窗口只覆盖 kernel/shell 两个 type，应用域仍 enforcing；
  `enforce` 对外仍读 1。
- fscrypt 密钥通过 session keyring 交接给 T/服务，命令可以访问 CE 目录；
  密钥不可导出（logon 类型）。
- 触发文件在 App 外部目录：具备存储权限的应用理论上可重复触发 exploit，
  但不能获得 su（白名单仍生效）。后续可加 nonce。
- 不改分区、不改 boot/vbmeta/super；重启回滚。