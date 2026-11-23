# 接口与协议

## 1. broker 邮箱

路径固定：

- 命令：`/data/local/tmp/ghost_cmd`（一行文本，shell 侧写入）
- 输出：`/data/local/tmp/ghost_out`（当前命令 stdout，0666）

main（shell creds）每 200ms 轮询命令文件；执行前把 stdout 重定向到
`ghost_out`，执行后文件保留。T（uid0）实际执行 BK 操作。

### 命令表

| 命令 | 参数 | 说明 |
|---|---|---|
| `help` | – | 列出命令 |
| `id` | – | 打印 T 的 uid/gid/context 与 /proc/self 属性 |
| `ls <path>` | 路径 | 目录列表 |
| `stat <path>` | 路径 | mode/uid/gid/size |
| `cat <path>` | 路径 | 流式读（262144 分块） |
| `hex <path> [off] [len]` | 可选偏移/长度 | hexdump |
| `writehex <path> <off> <hex>` | – | `off=0` 用 write，否则 pwrite；**不 O_CREAT** |
| `sysctl <path> <value>` | – | 写 sysctl 字符串（末尾换行） |
| `chmod <path> <octal>` | – | chmod |
| `mkdir <path> [octal]` | 默认 0755 | mkdir |
| `unlink <path>` | – | 删除文件 |
| `sync` | – | sync() |
| `probe` | – | 探测一组系统文件可读性 |
| `forktest` | – | fork 子进程（继承 root creds）并 park |
| `exectest` | – | fork + exec `/system/bin/sh`，验证 HKIP |
| `serve` | – | 启动 root 服务（fork → setcon adbd → glsrv） |
| `setlabel <path> <context>` | – | `setxattr("security.selinux")` |

BK 操作枚举（内部）：`READ/WRITE/LIST/STAT/UNLINK/MKDIR/SYNC/PROBE/CHMOD/
FORK/EXEC/SERVE/SETLABEL`。`bk_call()` 通过 `g_bk_req/g_bk_done` 原子握手，
15s 超时。

## 2. glsrv 线协议

传输：AF_UNIX `SOCK_STREAM`，abstract 名称 `@ghostlock`（sun_path[0]=0，
后续 "ghostlock"）。socket 标签为 `adbd`（serve 子进程 setcon 后创建）。

请求（客户端 → 服务）：

```
struct { uint32_t magic; uint32_t len; }   // magic = 0x314c5347 ('GLS1', LE)
uint8_t cmd[len];                          // len <= 65536，不含 NUL
```

响应（服务 → 客户端）：

```
子进程 stdout/stderr 原样字节流
struct { uint32_t magic; int32_t status; } // magic = 0x314c4447 ('GLD1', LE)
close
```

- `status` 是 `WIFEXITED ? exit : 128+signal`；fork 失败为 127。
- 未授权：服务写文本 `gl-su: uid <n> is not authorized\n` + GLD1/126。
- 客户端读到 EOF；一次模式会剥离最后 8 字节尾包并以其 status 作为退出码。
- 交互模式不剥（尾包只在 shell 退出时出现）。

授权：`SO_PEERCRED`；逐连接读 `/data/adb/gl_allow`（每行 uid 或 `*`；
缺失时只允许 2000）。服务端每连接 fork 一个 handler，支持并发。

## 3. gl-su CLI

```
gl-su -c <command>     # 一次执行，输出到 stdout，退出码=远端 status
gl-su                  # 交互：远端 sh，stdin/stdout 双向转发
gl-su <user> -c <cmd>  # 忽略 user，兼容 su 语法
gl-su -v/--version     # 版本
gl-su -h/--help        # 简短帮助（MT 探测用，避免出现 --interactive 等字样）
```

退出码：

- `125`：参数不支持（历史；当前无参数也进交互）
- `126`：连接失败或服务拒绝（not authorized）
- 其他：远端命令退出码

## 4. 守护进程文件

App 外部目录（默认
`/sdcard/Android/data/com.ghostlock.manager/files/ghost`）：

| 文件 | 方向 | 格式 |
|---|---|---|
| `gltrigger` | App → 守护 | 文本，`start` 或 `stop` |
| `glstatus` | 守护 → App | `idle/settling/serving/ready/exploit_failed/serve_failed` |
| `glheartbeat` | 守护 → App | epoch 秒（每 10s 更新） |
| `gldaemon.log` | 守护 → 人 | 时间戳 + 事件 |
| `rt_attN.log` | 守护 → 人 | 第 N 次 exploit 的 rt.log 副本 |
| `ghost1/glsrv/gl-su/glboot.sh` | App 解压 | 二进制/脚本 |

启动命令：

```
adb shell setsid nohup sh <dir>/glboot.sh <dir> >/dev/null 2>&1 </dev/null &
```

## 5. App ↔ root

App 不直接持有 root：它 `ProcessBuilder("/data/local/tmp/gl-su","-c",cmd)`
执行命令，读 stdout+stderr，`waitFor()` 取退出码。状态检测就是
`id -u` 的解析；授权管理通过 root 执行 `printf '%s' '<uids>' >
/data/adb/gl_allow`。

## 6. 标签约定

- `gl-su` 必须是 `u:object_r:shell_exec:s0`（应用域可 exec）。
- `glsrv`/`ghost1` 保持 `shell_data_file` 即可（shell 域可 exec）。
- 服务 socket 必须是 `adbd` 标签（应用域有 connectto）。
- `setlabel` 只用于 `gl-su`；改完 shell 无写权限，更新前需改回
  `shell_data_file`。