# GhostLock HLK-AL00

HONOR 9X（HLK-AL00）/ HarmonyOS 2.0.0.270 / Linux 4.14.116 上的**运行期临时 root** 研究项目：
基于 GhostLock（futex PI 逻辑漏洞，CVE-2026-43499）的零 slide 利用链，配套一个极简 root 管理器 App
（状态 / root 终端 / uid 白名单 / 日志）、root 服务、MT 管理器自定义 su 集成，以及 adb 启动守护进程。

定位是**研究原型 + 极简管理器**，不是 KernelSU/Magisk 的替代品：没有内核侧授权、没有
per-app 授权弹窗、没有模块系统。

> **仅用于安全研究与自己的设备。**
> 本项目不修改 boot/vbmeta/super 等任何分区，root 只存在于内存，重启完全回滚。
> 利用失败可能触发内核 panic 并自动重启；不同固件版本的符号/偏移不同，**不要在其他机型或版本上尝试**。
> 使用造成的一切后果由使用者承担。

## 特性

- 运行期 uid0（`init_cred` + full caps），不落盘、不改分区
- SELinux 仅对 kernel/shell 类型临时 permissive（应用域仍 enforcing）
- 继承 fscrypt 密钥，root 命令可访问 `/data/data` 等 CE 目录
- 常驻 root 服务：abstract socket `@ghostlock`（adbd 标签，普通应用可连）、uid 白名单、fork/exec 命令
- su 客户端 `gl-su`（`shell_exec` 标签），支持 `-c` 与交互模式；MT 管理器可直接把它当自定义 su
- 管理器 App：root 状态、root 终端、uid 授权管理、日志
- 启动用每次开机一条 adb 命令拉起的小守护进程（继承 adbd 的 session keyring）；之后 App 内一键起 root。
  Shizuku 的 shell 只有在从 adb/adbd 启动时才带 fscrypt keyring，且 Android 10 没有免 PC 的无线启动
  （无线配对是 Android 11+），见 [docs/TECHNICAL.md](docs/TECHNICAL.md)

## 文档

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)：组件、broker、服务、App、守护进程、生命周期
- [docs/EXPLOIT.md](docs/EXPLOIT.md)：漏洞、KernelSnitch、写原语、SELinux/fscrypt、HKIP、失败模式
- [docs/API.md](docs/API.md)：邮箱命令、glsrv 线协议、gl-su CLI、守护进程文件
- [docs/OPERATIONS.md](docs/OPERATIONS.md)：安装、每次开机、故障处理、MT、白名单、日志
- [docs/BUILD.md](docs/BUILD.md)：NDK/AGP/JDK 版本、构建、校验和
- [docs/TECHNICAL.md](docs/TECHNICAL.md)：技术结论速览（含 Shizuku keyring 结论）
- [docs/FAQ.md](docs/FAQ.md)：常见问题
- [SECURITY.md](SECURITY.md)：使用范围、合规、风险、数据与备份

## 支持环境

- HONOR 9X **HLK-AL00**，HarmonyOS **2.0.0.270**（Android 10，kernel **4.14.116**）
- PC + adb（启动守护进程、安装 APK）
- 构建：Android NDK（`aarch64-linux-android29-clang`）、JDK 17、Android SDK 36

## 快速开始

1. 安装管理器：
   ```
   adb install -r prebuilt/GhostLockManager-debug.apk
   ```
2. 打开一次 App（把 assets 释放到
   `/sdcard/Android/data/com.ghostlock.manager/files/ghost`）。
3. **每次开机后**在电脑上执行一次（App 的启动弹窗里可一键复制）：
   ```
   adb shell setsid nohup sh /sdcard/Android/data/com.ghostlock.manager/files/ghost/glboot.sh /sdcard/Android/data/com.ghostlock.manager/files/ghost >/dev/null 2>&1 </dev/null &
   ```
4. 打开 App → “启动 Root 服务”，等待 1–2 分钟。
5. 在 App 终端执行 `id`、`ls /data/data` 验证。

## MT 管理器

- MT：主设置 → 勾选“获取 root 权限” → “自定义 su 命令”填：
  ```
  /data/local/tmp/gl-su
  ```
- 首次使用前在 App 的“授权管理”里勾选 MT（uid），否则服务会拒绝。
- 支持 MT 的常驻交互 shell（stdin + `MT - End of command` sentinel）。已验证可对 `/data/adb`、
  `/data/data` 做创建/删除/修改。

## 授权模型

- `/data/adb/gl_allow`：每行一个 uid；`2000`（shell）默认放行；管理器和 MT 需加入。
- 服务端用 `SO_PEERCRED` 取客户端 uid 校验；未授权应用会收到 `not authorized`。
- App 的“授权管理”直接重写该文件（经 root 服务执行）。

## 每次开机与故障处理

- root 是内存态：**重启即回滚**，需重新执行第 3 步并点启动。
- 利用是概率性的：遇到 stamp-loss 会内核 panic 自动重启；重启后重跑守护进程、再点启动，
  通常 1–3 次内成功。
- `serve` 的 `setcon adbd` 偶发瞬时 EACCES；守护进程每 20 秒自动重试，最多 12 次（约 4 分钟）。
- 若 App 显示“需先启动 adb 守护进程”，按弹窗命令执行，再点“重新检测”。

## 关于 Shizuku 启动

Shizuku 的 shell 是否有 fscrypt keyring，取决于**服务端的启动路径**，不是 uid：

- 从 adb/adbd 启动（USB、TCP、Android 11+ 无线配对）时，shell 继承共享的用户会话 keyring，
  `_ses` 里有 `fscrypt`，exploit 的两阶段交接可用。本机实测 USB 与 TCP 两种传输的 `_ses`
  serial 和 fscrypt 子项完全一致（`adb tcpip` 重启 adbd 后 serial 都不变）。
- 从 App 内或 root 方式启动时，服务端可能在另一条 session 里；此时 main 的 `_ses` 不含
  `fscrypt`（日志 `search=-1/126`），T 只能在 `_uid_ses.0` 里看到 keyring 但 main 无 possessor
  （`linkring=-1/13`），exploit 最终 `readn=0`/ENOKEY。

另外，`keyprobe` 里 `search_dest_session` 的 EACCES 是“目标 keyring 写权限”问题，adb shell
也会出现，不能当作 possessor 的判据。

本机是 Android 10：Shizuku 的无线配对（Wireless Debugging）是 Android 11+；pre-11 的“无线”
只是 `adb tcpip`，仍需要 PC（或 root/system）先打开，且 `service.adb.tcp.port` 重启即失。
所以对本机，Shizuku 并不能省掉 PC；本项目选择 adb 守护进程，理由是**启动路径可控、依赖最少**，
而不是“Shizuku 必然没有 keyring”。详见 [docs/TECHNICAL.md](docs/TECHNICAL.md) 第 3 节。

## 构建

### 目标二进制（ghost1 / glsrv / gl-su / keyprobe）

```
export ANDROID_NDK_HOME=/path/to/android-ndk
bash scripts/build_targets.sh
```

产物写入 `prebuilt/`。若要把新二进制同步进 App 的 assets：

```
bash scripts/sync_assets.sh
```

### 管理器 App

```
cd manager
echo "sdk.dir=/path/to/Android/Sdk" > local.properties
./gradlew assembleDebug
# 产物：manager/app/build/outputs/apk/debug/app-debug.apk
```

工程已配置阿里云 Maven 镜像，便于大陆网络构建。

## 目录

```
exploit/ghost1.c         利用链 + broker + root 服务启动（单文件）
service/glsrv.c          root 服务端（@ghostlock，adbd 标签，多连接）
service/gl-su.c          su 客户端（-c / 交互模式）
service/keyprobe.c       keyring 权限诊断工具
scripts/build_targets.sh 用 NDK 交叉编译以上二进制
scripts/glboot.sh        adb 启动守护进程（心跳/触发/状态）
scripts/sync_assets.sh   把二进制同步进 App assets
manager/                 Android 管理器工程（Kotlin，AGP 8.13）
prebuilt/                gl-su / glsrv / ghost1 / keyprobe / 管理器 APK
docs/ARCHITECTURE.md     架构、组件、生命周期
docs/EXPLOIT.md          利用链与内核细节
docs/API.md              邮箱/线协议/CLI/守护进程文件
docs/OPERATIONS.md       使用、故障处理、MT、白名单
docs/BUILD.md            构建与版本
docs/TECHNICAL.md        技术结论速览
docs/FAQ.md              常见问题
SECURITY.md              安全/合规/风险/备份
```

## 已知限制

- 仅在 HLK-AL00 / 2.0.0.270 上验证；其他固件需重新推导符号与偏移。
- exploit 概率性 panic；没有跨重启自动重试（守护进程随重启消失，需要 PC 重新拉起）。
- 触发文件位于 App 外部目录，具备存储权限的其他应用理论上可重复触发 exploit（**不能获得 su**，
  su 仍受 uid 白名单约束）。
- 与 KernelSU/Magisk 不同，未实现 per-app 授权弹窗；目前是 uid 白名单。
- Android 10 没有免 PC 的 Shizuku 无线启动；从 adb 启动时 keyring 正常，但本项目仍用 adb 守护进程。

## 参考与致谢

- [r0rt1z2/GhostLock](https://github.com/r0rt1z2/GhostLock)：GhostLock 4.14 ARM64 参考实现
- KernelSnitch：`mm_struct` slab 泄露思路
- CVE-2026-43499
- Shizuku、MT 管理器仅作为互操作对象；本项目**不包含、不再分发**它们的 APK

## 商标与非官方声明

HONOR、HUAWEI、HarmonyOS 等名称是其各自权利人的商标。本项目是非官方、独立的
安全研究项目，与 HONOR/Huawei 无关联，也未获其认可。仓库不包含任何厂商固件、
内核镜像、SELinux 策略二进制或第三方 APK。

## License

MIT，见 [LICENSE](LICENSE)。