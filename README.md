# GhostLock HLK-AL00

HONOR 9X（HLK-AL00）/ HarmonyOS 2.0.0.270 / Linux 4.14.116 上的**运行期临时 root** 研究项目：
基于 GhostLock（futex PI 逻辑漏洞，CVE-2026-43499）的零 slide 利用链，配套 KernelSU 风格的管理器 App、
root 服务、MT 管理器自定义 su 集成，以及 adb 启动守护进程。

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
- Android 10 上 Shizuku 的 shell 拿不到 fscrypt keyring（见 `docs/TECHNICAL.md`），因此启动改用
  每次开机一条 adb 命令拉起的小守护进程；之后 App 内一键起 root

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

## 为什么不用 Shizuku 启动

实测 Shizuku 的 shell 会话能看到 `fscrypt` keyring（无 dest 的 SEARCH 成功），但**没有 possessor
权限**：带 dest 的 SEARCH/LINK 全部 EACCES。exploit 因此拿不到密钥（ENOKEY），连
`/data/local/tmp` 的 glsrv 都无法 exec。同一 boot 的 adb shell 则完全正常。

Android 10 上 Shizuku 本身也要每次开机用 adb 启动，所以本项目改为同样依赖 PC 的 adb 守护进程，
但 keyring 路径是正确的。详见 [docs/TECHNICAL.md](docs/TECHNICAL.md)。

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
docs/TECHNICAL.md        技术说明与关键结论
```

## 已知限制

- 仅在 HLK-AL00 / 2.0.0.270 上验证；其他固件需重新推导符号与偏移。
- exploit 概率性 panic；没有跨重启自动重试（守护进程随重启消失，需要 PC 重新拉起）。
- 触发文件位于 App 外部目录，具备存储权限的其他应用理论上可重复触发 exploit（**不能获得 su**，
  su 仍受 uid 白名单约束）。
- 未实现 KernelSU 式的 per-app 授权弹窗，目前是 uid 白名单。
- Shizuku 启动路径不可用（原因见上）。

## 参考与致谢

- [r0rt1z2/GhostLock](https://github.com/r0rt1z2/GhostLock)：GhostLock 4.14 ARM64 参考实现
- KernelSnitch：`mm_struct` slab 泄露思路
- CVE-2026-43499
- Shizuku、MT 管理器仅作为互操作对象；本项目**不包含、不再分发**它们的 APK

## License

MIT，见 [LICENSE](LICENSE)。