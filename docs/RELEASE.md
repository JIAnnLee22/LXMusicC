# 1.0.0 构建与发布

## 固定环境

| 组件 | 版本 / 范围 |
| --- | --- |
| JDK | 21 |
| Gradle / Android Gradle Plugin | 9.6.0 / 9.4.1 |
| compileSdk / targetSdk | 36 / 36 |
| minSdk | 26（Android 8.0） |
| Build Tools | 36.0.0 |
| NDK | 29.0.14206865 |
| CMake | 4.1.2 |
| ABI / 图形 | arm64-v8a、x86_64 / OpenGL ES 3.0 |
| versionName / versionCode | 1.0.0 / 2（高于旧快照的 1） |

上述 SDK 组件与 `app/build.gradle.kts`、`scripts/setup-android-sdk.sh` 默认值、`tools/pi-loop.sh` 常量及生成的 `docs/MILESTONE-LATEST.md` 一致。SDK 脚本只补缺失组件，包含 platform-tools；在本机或 GitHub runner 已安装完整时不重复安装。环境变量可覆盖安装版本，但须与 Gradle 固定值一致，否则不构成本项目的受支持构建环境。

```sh
export ANDROID_HOME="$HOME/Android/Sdk" # 或 runner 预装 SDK
./scripts/setup-android-sdk.sh
./scripts/build.sh
./scripts/build.sh :app:assembleRelease
"$ANDROID_HOME/build-tools/36.0.0/apksigner" verify --verbose app/build/outputs/apk/release/app-release.apk
adb -s <设备序列号> install -r app/build/outputs/apk/release/app-release.apk
adb -s <设备序列号> shell am start -W -n jiannlee22.lxmusicc/.SourceActivity
adb -s <设备序列号> logcat -d -s LXMusicC AndroidRuntime
```

## 签名与更新边界

Release 变体不可调试。签名支持以下四个本地环境变量，必须全部提供或全部省略：

- `LX_RELEASE_STORE_FILE`：已有专用 keystore 的绝对路径。
- `LX_RELEASE_STORE_PASSWORD`、`LX_RELEASE_KEY_ALIAS`、`LX_RELEASE_KEY_PASSWORD`：签名凭据，不写入仓库。

CI 可配置仓库 secrets：`ANDROID_KEYSTORE_BASE64`（keystore 的 base64）、`ANDROID_STORE_PASSWORD`、`ANDROID_KEY_ALIAS`、`ANDROID_KEY_PASSWORD`。仅在 runner 临时目录解码，构建后删除；不上传密钥或 unsigned APK。配置不完整或签名验证失败会停止发布，不降级为 unsigned。

本轮没有已有的专用发布证书，也不擅自创建、提交私钥或配置远端 secrets。全部签名参数省略时，使用 Android 调试证书签署 Release 变体，保证当前包可安装，但不是稳定的发行者身份；GitHub runner 重新生成的调试证书可能变化，不能保证跨构建覆盖更新。以后启用专用证书也可能无法覆盖旧包。调试证书不适合应用商店；1.0.0 是 GitHub 侧载版本，不代表已具备长期签名保障。

同签名、较高 versionCode 可覆盖旧版本而保留数据；不同签名不能覆盖安装。不要自动卸载、清数据或绕过签名检查；README 已提醒数据丢失风险。

## 自动发布边界

- 本轮只提交并推送分支，不创建标签或 Release，不上传本地 APK。
- `tools/pi-loop.sh` 在所有条目完成后生成最终摘要并创建 `v1.0.0`，每约 6 小时可生成 `snapshot-*`；摘要仍包含安装、签名限制和固定环境，不退回旧 `lxmusicc-debug.apk` 名称。
- `release.yml` 只监听 `snapshot-*` / `v*` 标签及手动触发；只有标签 ref 才发布。`ci.yml` 只有 PR / 手动构建，不上传产物；没有 push 分支触发。
- CI 通过共用 `scripts/build.sh` 构建 Release，必须通过 `apksigner verify`；发布附件只有已签名的 `lxmusicc.apk` 和 `SHA256SUMS`，不再容忍 Release 构建失败或上传 unsigned APK。
- `docs/MILESTONE-LATEST.md` 是下一次标签的说明来源，不是已在 GitHub 发布成功的证明。

## 验收边界

本地构建、签名检查、安装及启动结果记录在 `docs/ARCHITECTURE.md` 的 10-release 节。本地安装使用与 CI 相同的 Release 变体，但不等于已验证远端附件下载。

正式标签必须由外层脚本在本轮提交之后创建，因此「从正式 Release 下载 APK 并启动」只能在脚本发布后复核：确认 Actions 成功、附件齐全，下载 `lxmusicc.apk` 并用 `SHA256SUMS` 核对，在支持设备安装、启动并查看四页/版本。未执行前不得声称远端下载验收通过。Android 8/9 的旧存储权限分支及所有厂商 ROM 不因本轮启动检查而视为全面覆盖。
