# v1.0.0 发布准备

此摘要随 10-release 提交更新，尚不表示正式标签或远端 Release 已创建；最终摘要由 `tools/pi-loop.sh` 在完成后生成。

## 功能与使用

- 音源导入与管理、多源搜索、在线播放与后台媒体控制。
- 公共 Music 目录缓存与离线播放、封面、拖动进度、播放队列与循环/随机模式。
- 收藏、最近播放、自建歌单、本地搜索、仅 Wi-Fi 下载与主题设置。
- 不内置音源或音乐资源，只兼容部分 LX 音源；仓库示例仅提供短预览。

完整功能、用法与限制见 [README](../README.md) 和 [CHANGELOG](../CHANGELOG.md)。

## 安装

正式发布后下载同一 Release 的 `lxmusicc.apk` 与 `SHA256SUMS`，核对 SHA-256 后允许安装未知应用并安装。需要 Android 8.0+、arm64-v8a / x86_64、OpenGL ES 3.0。

未配置专用发布证书时使用调试证书签名（应用不可调试），跨构建可能签名不同、不能覆盖更新；不要直接卸载旧版，卸载会丢失音源、音乐库、设置和缓存列表。签名与验证边界见 [发布说明](RELEASE.md)。

## 构建环境

JDK 21；SDK API 36；Build Tools 36.0.0；NDK 29.0.14206865；CMake 4.1.2。与 `scripts/setup-android-sdk.sh`、`app/build.gradle.kts` 和 `tools/pi-loop.sh` 一致；安装脚本只补缺失组件。

CI 使用 `scripts/build.sh :app:assembleRelease`，签名验证成功才上传安装包，不上传 unsigned APK。仅里程碑标签发布附件，普通分支 push 不产出安装包。远端下载与安装验收须在外层脚本发布后复核。
