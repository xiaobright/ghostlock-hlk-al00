# Android 10 免 PC 启动：一条未验证的思路

> **状态：未实现、未验证、不推荐。**
> 本文只记录“没有电脑时怎么把 adbd 拉起来”的可行性、设备侧阻塞点和风险，供研究参考。
> 本项目支持的启动方式仍是 [README](../README.md) 里的 adb 守护进程（每次开机一条 adb 命令）。
> 仅在自有设备/研究环境尝试；可能导致 adb 丢失、bootloop，甚至需要恢复出厂。

## 1. 问题

- Android 10（SDK 29）没有 Wireless Debugging 配对（Android 11+ 的 `adb pair`）。
  Shizuku 的无线启动在 SDK < 30 时只走 `adb tcpip`，需要 PC（或 root/system）先打开。
- 本项目已验证：**从 adb 启动的 Shizuku shell 带 fscrypt keyring**（USB/TCP 都是同一个
  用户会话级 `_ses`；`adb tcpip` 重启 adbd 后 serial 都不变）。所以“只要有一个 adbd shell，
  keyring 就不是问题”。
- 剩下的问题只有一个：**没有 PC 时，怎么让 adbd 监听 TCP，并让手机上的 adb 客户端连上它？**

## 2. 设备侧事实（HLK-AL00 / HarmonyOS 2.0.0.270）

### 2.1 SELinux（`plat_seapp_contexts` + `policy.cil` 实测）

CVE-2024-31317 注入出来的进程是 `uid=1000`、`seinfo=platform`，落到 `system_app` 域。
在这台华为的策略下：

| 操作 | `system_app` |
|---|---|
| exec `/system/bin/sh`（`shell_exec`） | ✅ |
| exec `/system/bin/toybox`（`toolbox_exec`） | ✅ |
| exec `/system/bin/settings`（`system_file`） | ❌ 只有 read/open |
| exec `/system/bin/app_process*`（`zygote_exec`） | ❌ 只有 lnk_file read |
| exec `/data/app/.../lib*/libshizuku.so`（`apk_data_file`） | ❌ |
| exec 自己 data 目录里的文件（`app_data_file`） | ❌ 无 execute |
| `setprop service.adb.tcp.port`（`shell_prop`） | ✅ `property_service set` |
| `setprop ctl.restart adbd`（`ctl_adbd_prop`） | ❌ 无规则 |
| `setprop ctl.start/stop`（`ctl_start_prop`/`ctl_stop_prop`） | ❌ 无规则 |
| 对 adbd 发信号（`toybox kill adbd`） | ❌ 无 `allow system_app adbd` |

结论：**system_app 只能跑 `sh` 和 `toybox`**；能设端口属性，但不能自己重启 adbd，
也不能用 `settings` 写 `adb_enabled`。

### 2.2 `adb_enabled` 的 system_server 路径

AOSP Android 10 的 `AdbService` 监听 `Settings.Global.ADB_ENABLED`，变化时通过
`IAdbTransport.onAdbEnabled` / `AdbDebuggingManager.setAdbEnabled` 启停 adbd
（`services/core/java/com/android/server/adb/`）。本机 `dumpsys adb` 能看到
`android.debug.IAdbManager` 服务。

这意味着：**一个有 `WRITE_SECURE_SETTINGS` 的普通 App** 可以通过 binder 写这个 setting，
触发 system_server 重启 adbd——不需要 exec `settings`。`WRITE_SECURE_SETTINGS` 在本机的
保护级别是 `signature|privileged|development`，可以用一次 `adb shell pm grant` 授予并持久。

### 2.3 fscrypt keyring

已实测：USB 与 TCP（`adb tcpip 5555` + Wi-Fi）启动的 Shizuku shell，`_ses` serial 和
`fscrypt` 子项完全一致；重启 adbd 也不变。所以只要链路的最后一步是 **adbd shell**，
keyring 就带。

## 3. 理论链条（未实现）

一次性 PC 准备：

1. 安装 App（需要声明 `WRITE_SECURE_SETTINGS`，或用现成 fork 改）；
2. `adb shell pm grant <pkg> android.permission.WRITE_SECURE_SETTINGS`；
3. （可选）安装 Shizuku/管理器，用于后续在 shell 里跑 `newProcess`。

每次开机（可自动化）：

1. App 触发 CVE-2024-31317（写 `hidden_api_blacklist_exemptions`），payload 改成
   `toybox setprop service.adb.tcp.port 5555`——system_app 可以 exec toybox，也可以
   set `shell_prop`；
2. App 在触发后自己把 `hidden_api_blacklist_exemptions` 清回 `null`（它有
   `WRITE_SECURE_SETTINGS`，走 `Settings.Global.putString`）；**不要依赖注入子进程去清**，
   因为 system_app 不能 exec `settings`；
3. App 写 `Settings.Global.putInt(ADB_ENABLED, 0)` 再 `1`，system_server 重启 adbd；
   adbd 读到 `service.adb.tcp.port=5555`，监听 TCP；
4. App 的 adb 客户端（例如 Shizuku 的 `libadb`）连 `127.0.0.1:5555`；首次会弹出
   “允许 USB 调试吗？”（App 的 adb key），在手机上点允许一次即可；
5. 这个 shell 是 adbd 子进程，带 fscrypt keyring；在里面跑 `ghost1` → root + keyring →
   `glsrv`；
6. 重启后 `service.adb.tcp.port` 是易失的，重复 1–3 即可；一次 PC 授权后，后续 boot
   不再需要 PC。

## 4. 为什么现成的 mx0341/Shizuku 不能直接用

- 它的 `--invoke-with` payload 是 `settings put ... "null" ; <app lib>/libshizuku.so ;
  pkill -9 sh`：
  - `settings` = `system_file`，system_app 不能 exec；
  - `libshizuku.so` 在 `/data/app/...`，`apk_data_file`，不能 exec；
  - `pkill` = `system_file`，不能 exec。
  唯一能跑的是 `sh` 本身；payload 实际失败。
- 它的 README 给的 `setprop ctl.restart adbd` 是 `ctl_adbd_prop`，system_app 没有 set 权限；
  `settings put global adb_enabled` 又需要 exec `settings`。
- 它的 `ShizukuProtectReceiver` 条件写反了：`if (!Util.isSecurityPatchUpToDate()) return;`，
  即只在“补丁已更新（漏洞已修）”时清 setting；对真正有漏洞的设备不清，残留有 bootloop 风险。
- 它 release 里的 `system-shizuku.zip` 还带了 `0.0.0.0:6666` 匿名 FTP 和 `0.0.0.0:6688`
  shell 监听脚本，**不要运行**。

## 5. 未验证点与风险

- **CVE-2024-31317 在本机是否可注入**：framework.jar 里有 `hidden_api_blacklist_exemptions`、
  `--set-api-blacklist-exemptions`、`--invoke-with`，补丁是 2020-11；但华为的 zygote/AMS
  是否完全保留 AOSP 语义，没有实测。注入失败/残留可能导致 bootloop。
- **`adb_enabled` 是否真的重启 adbd**：AOSP 是，但华为的 `AdbService`/transport 可能不同；
  切 `0` 会把 adb 停掉，如果 `1` 没生效，需要到开发者选项手动恢复 USB 调试。
- **libadb 的 key/授权流程**：`127.0.0.1:5555` 首次连接会弹授权框；App 的 key 必须被允许一次；
  如果 App 的 adb 客户端不支持这条路径，就断了。
- **一次性 PC 依赖**：`WRITE_SECURE_SETTINGS` 的 `pm grant` 仍需要一次 adb；这不是“开箱即免 PC”。
- **安全**：adbd 在 TCP 上监听会扩大攻击面（虽然仍需 key 授权）；只在可信网络、研究环境中用，
  用完重启/关闭。
- 这是**第二个用户态漏洞**（CVE-2024-31317），不是 root 本身；root 仍然来自 GhostLock 内核利用。
  它只负责“把 adb 拉起来”。

## 6. 结论

- 本项目**不实现**这条链；支持的启动方式仍是 adb 守护进程（一条命令/次 boot，依赖最少）。
- 如果其他人要继续研究，建议分三步、每步单独验证，并准备好恢复手段：
  1. 用 adb shell 直接写 CVE 的 setting，payload 用 `toybox setprop`，确认注入和残留清理；
  2. 试 `adb_enabled` 0/1 是否让 adbd 重启并监听 5555；
  3. 试 libadb 连 `127.0.0.1:5555`、key 授权、以及 shell 内的 keyring（`keyprobe`）。
- 任何一步失败就停；不要在一次实验里把三步串起来。

## 参考

- AOSP Android 10：`services/core/java/com/android/server/adb/AdbService.java`、
  `AdbDebuggingManager.java`（`ADB_ENABLED` 观察与 adbd 启停）
- CVE-2024-31317（Zygote command injection）公开研究
- `mx0341/Shizuku`（非官方 fork，仅作参考；本文不分发其 APK）