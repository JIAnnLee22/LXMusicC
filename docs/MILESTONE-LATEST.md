# v1.1.0

- 生成时间：2026-11-23 10:42:19
- 类型：正式版

## 已完成的功能集

- `01-ui-shell` 纯 C 界面框架与页面骨架
- `02-lx-source-import` LX 音源导入与元数据解析
- `03-source-runtime` 音源请求执行（JS + HTTP）
- `04-search` 多源搜索界面
- `05-playback-service` 前台播放服务与通知
- `06-cache` 公共目录缓存与离线播放
- `07-player-ui` 播放页与进度
- `08-library` 收藏 / 历史 / 歌单
- `09-settings-polish` 设置与打磨
- `10-release` 发布 1.0.0
- `11-system-ime` 系统键盘输入（IME 桥接）
- `12-media-services` 系统媒体服务（其它 App / 系统可以“调用”播放）
- `13-app-ux` 常规音乐 App 的体验补齐
- `14-release-1.1` 发布 1.1.0

## 本次里程碑包含的提交

```
872de63 14-release-1.1: 升级1.1.0并完善发布说明与Release链路验收
efbd7bc 13-app-ux: 补齐预测返回与常规交互并修复界面实例交接
210be7f 12-media-services: 暴露系统媒体库控制并恢复播放队列
e06c7b8 11-system-ime: 系统键盘输入（IME 桥接）（自动提交）
2c27bcb 11-system-ime: 接入系统键盘与原生文本编辑
bd276a2 plan: 加入系统键盘(IME)、系统媒体服务、常规 App 体验与 1.1.0 发布四个功能集
```

## 安装

下载本 Release 的 `lxmusicc.apk`，允许安装未知应用后安装；Android 8.0+，arm64-v8a / x86_64，OpenGL ES 3.0。

用同一 Release 的 `SHA256SUMS` 核对完整性。未配置专用发布证书时使用调试证书签名，不保证跨构建覆盖更新；卸载会丢失应用内数据。

功能、用法和兼容限制见 [README](https://github.com/JIAnnLee22/LXMusicC/blob/main/README.md) 与 [CHANGELOG](https://github.com/JIAnnLee22/LXMusicC/blob/main/CHANGELOG.md)。

## 构建环境

SDK API 36 / Build Tools 36.0.0 / NDK 29.0.14206865 / CMake 4.1.2；JDK 21。
由 `scripts/setup-android-sdk.sh` 仅补缺失组件，`scripts/build.sh :app:assembleRelease` 构建，CI 检查签名后上传；详见 [发布说明](https://github.com/JIAnnLee22/LXMusicC/blob/main/docs/RELEASE.md)。
