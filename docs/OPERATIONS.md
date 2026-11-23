# 使用与运维

## 1. 前置条件

- HONOR 9X **HLK-AL00**，HarmonyOS **2.0.0.270**（Android 10，kernel 4.14.116）。
  其他版本/机型不要用（地址与偏移不同）。
- PC + adb；设备开启 USB 调试。
- 安装管理器 APK（`adb install -r`）。**每次安装都会触发鸿蒙的安装器/应用商店
  校验**，即使 root/pm install 也不能绕过，需要手动点允许。

## 2. 首次安装

1. `adb install -r prebuilt/GhostLockManager-debug.apk`
2. 打开一次 App。App 会把 `ghost1/glsrv/gl-su/glboot.sh` 解压到
   `/sdcard/Android/data/com.ghostlock.manager/files/ghost`。
3. （可选）把 MT 管理器加入白名单：见第 5 节。

## 3. 每次开机

1. 在电脑上执行（App 的启动弹窗可直接复制）：
   ```
   adb shell setsid nohup sh /sdcard/Android/data/com.ghostlock.manager/files/ghost/glboot.sh /sdcard/Android/data/com.ghostlock.manager/files/ghost >/dev/null 2>&1 </dev/null &
   ```
2. 打开管理器 → “启动 Root 服务”。
3. 状态会经历 `settling`（等系统空闲）→ `serving`（起服务）→ `ready`
   （“root 可用（uid 0）”），通常 1–2 分钟。
4. 验证：终端执行 `id`、`ls /data/data`；或直接使用 MT。

> root 只存在于内存。**重启后必须重新执行第 1 步并点启动**。

## 4. 故障与重试

### 4.1 内核 panic / 自动重启

exploit 有概率遇到 stamp-loss panic，`panic_on_oops=1` 会自动重启，守护进程和
服务随之消失。重启后：

1. 重新执行第 3 步的 adb 命令；
2. 再点一次“启动 Root 服务”。

通常 1–3 次内成功。若连续失败，先重启一次设备再试（清掉残留状态）。

### 4.2 `serve_failed`

`setcon adbd` 可能瞬时 EACCES。守护进程已按 20s 间隔重试最多 12 次
（约 4 分钟）；仍失败时重启设备重来。`gldaemon.log` 会记录每次 serve 尝试。

### 4.3 状态“未授权”

用 App 的“授权管理”勾选对应应用；shell（uid 2000）默认放行。MT 需要它的
uid 在 `/data/adb/gl_allow` 中。

### 4.4 `gl-su 未部署 / cannot connect`

- 没跑守护进程/没启动服务：按第 3 步操作；
- 服务已挂但 socket 不是 adbd（旧一次 serve 的瞬时失败）：重新点启动/重启。
- 更新过 gl-su 但启动文件无写权限：先经 broker `setlabel` 回
  `shell_data_file` 再更新（守护进程通常已处理）。

### 4.5 安装被拦截

鸿蒙应用商店/安装器校验：在弹窗点允许；这是系统级 UI，root 不能绕过。

## 5. MT 管理器

1. 主设置 → 勾选“获取 root 权限”；
2. “自定义 su 命令” = `/data/local/tmp/gl-su`；
3. 在管理器 App 的“授权管理”里勾选 MT（uid）；
4. 打开 `/data/adb`、`/data/data` 做建/删/改验证。

MT 会先执行 `gl-su -h` 探测，然后拉起常驻 shell，通过 stdin 发命令并用
`MT - End of command` sentinel 分段；`gl-su` 已兼容这种调用。

## 6. 授权白名单

- 文件：`/data/adb/gl_allow`，每行一个 uid；`2000` 默认保留。
- 管理器“授权管理”列出已安装应用并重写文件（保留 2000 与已卸载 uid）。
- 未授权 uid 连接会收到 `not authorized`（退出码 126）。

## 7. 日志与诊断

- `glsrv.log`：服务端连接/命令/拒绝记录（O_APPEND）。
- `rt.log`：当前 exploit 输出；`rt_attN.log`：守护进程按次保存。
- `prog.log`：exploit 的 O_SYNC 进度（A..R/N/FINAL 等）。
- `pls.log`：预建 socket 诊断（当前未启用该路径）。
- `gldaemon.log`/`glstatus`/`glheartbeat`：守护进程状态。
- App 的“查看日志”会一起展示。

判断成功：以 `boot_id`/uptime 未变、`[rt] FINAL uid=0`、`glsrv` 存在为准；
f2fs 回滚可能留下旧日志，不要只看旧 FINAL。

## 8. 结束与回滚

- 直接重启设备：broker、服务、守护进程消失，内核内存回滚。
- 不要手动 kill 持有 fake creds/pid=0 的进程（可能 teardown panic）。
- `/data/local/tmp` 下的二进制/日志、`/data/adb/gl_allow`、App 和 MT 会保留；
  需要时自行删除/卸载。

## 9. 速查表

| 症状 | 处理 |
|---|---|
| App 弹“需要先启动 adb 守护进程” | 执行弹窗命令，点“重新检测” |
| 一直 settling | 正常等待/重试；检查是否刚开机、是否有人重复点启动 |
| exploit_failed | 再点启动；多次失败则重启设备 |
| serve_failed | 等守护进程重试；仍失败重启 |
| 设备自动重启 | stamp-loss panic，属预期；重跑守护进程+启动 |
| MT 报 su 不存在 | 自定义 su 路径是否为 `/data/local/tmp/gl-su`；服务是否 ready |
| MT 被拒绝 | 把 MT uid 加入白名单 |