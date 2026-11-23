# 构建与版本

## 1. 工具链

### 目标二进制

| 项 | 值 |
|---|---|
| NDK | Android NDK（r27+ 均可；作者用 r29） |
| 编译器 | `aarch64-linux-android29-clang` |
| 链接 | `-static` |
| 目标设备 | Android 10 / kernel 4.14.116 (arm64) |

```
export ANDROID_NDK_HOME=/path/to/android-ndk
bash scripts/build_targets.sh
# 产物：prebuilt/ghost1 glsrv gl-su keyprobe
```

Windows 上建议在 WSL 里执行（NDK 的 linux-x86_64 工具链）；
Git Bash 直接跑也可以，只要 `ANDROID_NDK_HOME` 指向 Windows 版 NDK 的
`prebuilt/windows-x86_64` 对应编译器（脚本默认 linux 路径，按需改）。

### 管理器 App

| 项 | 值 |
|---|---|
| AGP | 8.13.0 |
| Kotlin | 2.1.0 |
| Gradle | 8.13（仓库自带 wrapper；distribution 用华为云镜像） |
| JDK | 17 |
| compileSdk | 36 |
| minSdk / targetSdk | 29 / 29 |
| 依赖 | androidx core-ktx 1.13.1、appcompat 1.7.0、material 1.12.0、coroutines 1.8.1、Shizuku api/provider 13.1.5 |
| Maven | 阿里云镜像（仓库已配置） |

```
cd manager
echo "sdk.dir=/path/to/Android/Sdk" > local.properties
./gradlew assembleDebug
# 输出 manager/app/build/outputs/apk/debug/app-debug.apk
```

- `local.properties` 不入库（`.gitignore`）。
- App assets 内含 `ghost1/glsrv/gl-su/start_root.sh/glboot.sh`；改二进制后执行
  `bash scripts/sync_assets.sh` 再打包。
- APK 用 debug 签名；正式分发请自行配置 release 签名。
- Shizuku 依赖/Provider 仍保留（历史启动路径），但当前启动不使用。

## 2. 当前发布版本

| 文件 | 大小 | md5 |
|---|---|---|
| `prebuilt/ghost1` | 2,270,696 | `33636f85f79ad89e6057c85794dfe1bb` |
| `prebuilt/glsrv` | 2,149,776 | `20614b208120f1a5ea51a17174a5f2c1` |
| `prebuilt/gl-su` | 2,140,504 | `80efdffdc8794fa25df3a0a10d54973c` |
| `prebuilt/keyprobe` | 2,140,856 | `8bdf94afe0a18fe0b00b90f31bf38ffc` |
| `prebuilt/GhostLockManager-debug.apk` | 15,739,478 | `403619281257913a977c207274bc148c` |

哈希也写在 `prebuilt/MD5SUMS`。重新编译出的二进制 md5 可能因 NDK 版本不同而
变化，属正常。

## 3. 架构相关

- 所有内核符号/偏移都在 `exploit/ghost1.c` 顶部和 `root_phases` 中硬编码，
  只对目标固件有效。换固件需要重新从内核镜像/符号推导：
  `rt_sched_class`、`init_cred`、`init_user_ns`、`policydb`、task/cred 偏移、
  SELinux type value、permissive_map 布局。
- 服务/客户端（glsrv/gl-su）与固件无关，主要依赖 Android 的 AF_UNIX 与
  SELinux 标签；换设备时仍需确认应用域能否 connectto 服务 socket 的标签。

## 4. 重建检查清单

1. `scripts/build_targets.sh` 成功，四个二进制静态链接；
2. `keyprobe` 在目标设备能跑（验证 keyring 环境）；
3. `scripts/sync_assets.sh` 后 App assets 的 md5 与 prebuilt 一致；
4. `./gradlew assembleDebug` 成功；
5. 安装后用 `id` 验证服务；`ls /data/data` 验证 fscrypt；
6. MT 自定义 su 路径正确、uid 已授权。