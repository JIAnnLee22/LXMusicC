# 自动化开发规则（Pi 循环）

本仓库由 `tools/pi-loop.sh` 反复驱动 Pi 编码代理推进，进度以 `PLAN.md` 为准。

## 目标

做成能日常使用的 Android 音乐播放器：LX 音源 → 搜索 → 播放 → 缓存 → 后台播放 → 库/歌单。

## 技术边界

- 界面、渲染、交互、音源解析、缓存索引、播放状态机：**C**（NDK，NativeActivity + EGL/GLES）。
  纯 C UI 的写法可以参考同级仓库 `../CUIDemo`（EGL 封装、SDF 文字、批次渲染、damage 局部重绘）。
- **Kotlin 只允许**出现在 Android 框架绕不开的地方：前台服务/MediaSession/通知、MediaStore 公共目录写入、
  系统 HTTP(S) 客户端、必须的 JS 引擎封装。每个 Kotlin 文件 ≤ 400 行、不写 UI、业务逻辑留在 C。
- 所有 JNI 接口集中登记到 `docs/BRIDGE.md`（函数名、签名、线程、返回值）。C 侧调用要检查异常与返回值。
- 不允许新增 Java 源码。
- 第三方库优先 vendor 单文件/小体量 C 库（stb、miniz、sqlite3、QuickJS/duktape 等），
  附 LICENSE 与来源说明；不要引入需要 Gradle 原生依赖的大型 SDK。

## 每轮交付（硬性）

1. `./scripts/build.sh` 必须成功（CI 用同一份脚本）。
2. 完成 `PLAN.md` 当前条目的验收项，并把该条的 `- [ ]` 改成 `- [x]`（只勾这一条）。
3. `README.md` 只更新功能/使用层面的内容；架构与实现细节写进 `docs/ARCHITECTURE.md`。
4. 一个功能集一个提交：`git add -A && git commit -m "<条目ID>: <一句话>"`，然后 `git push origin HEAD`。
5. 不要给 workflow 增加 `push: branches` 触发，也不要自己打 tag 或建 Release：
   只有 `tools/pi-loop.sh` 会在每 6 小时的里程碑和项目完成时打 `snapshot-*` / `v1.0.0` 标签，
   由标签触发 CI 构建发布。平时提交要保持“没有安装包产出”。

## 工程质量

- 单文件 ≤ 800 行；公共接口写在头文件；复杂逻辑写注释（中文即可）。
- 不在渲染/主线程做网络或磁盘 IO；用后台线程 + 队列，回调时加锁。
- 列表用分页/游标，避免一次性加载全部数据；大缓冲区及时释放。
- 网络、解析、播放失败都要可重试且不崩溃；日志统一 `adb logcat -s LXMusicC`。
- 无网络、无音源、无缓存、空搜索结果都要有空状态。

## 交互

- 不向用户提问、不等待确认；不确定就选最简单能用的方案，并记进 `docs/DECISIONS.md`。
- 不做与本轮条目无关的重构；不要删除之前轮次已实现的功能。

## 提交与发布

- 默认分支 `main`；禁止 force-push、禁止改写历史。
- 提交信息：`<条目ID>: <一句话>`，例如 `04-search: 多源并发搜索与结果列表`。
- `versionName` 在 `10-release` 之前保持 `0.x`；正式发布由 `pi-loop.sh` 打 `v1.0.0` 标签完成。
