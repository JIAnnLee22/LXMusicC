# 决策记录

## 01-ui-shell

- **最小边界**：保持 NativeActivity + EGL/GLES 3，四页只提供布局、滚动、选择状态和空状态。不做输入法、导入、真实搜索、播放、下载或库数据；相关入口明确显示尚未开放，避免把预览当成可用音乐功能。
- **渲染方案**：参考同级 `CUIDemo` 的 SDF/批次思路，重新实现小型 C 渲染器，不搬入演示 UI、Vulkan、图片后端、FBO 和 damage 历史。静止不刷帧，变化时整帧绘制，正确处理 Android 不保留交换缓冲的情况。
- **字体**：离线生成 Noto Sans SC Regular 的 UI 文案子集，避免启动时在主线程读完整字体、生成 SDF 或引入大字体包。实机发现 stb 的 SDF 对 CFF 三次曲线不完整，因此明确拒绝 CFF，使用 TrueType 二次曲线字体。当前未知字符回退 `?`，真实歌名的动态 CJK 字形缓存留待对应功能条目。
- **安全区**：实机 targetSdk 36 下 `contentRect` 仍覆盖系统导航栏，不能仅依赖它。新增纯 C 框架 JNI 只读查询系统栏/刘海，与 contentRect 取交集；不为查询安全区引入 Kotlin Activity 或 UI。异常与返回值契约见 `BRIDGE.md`。
- **返回兼容**：在 Activity manifest 关闭 `enableOnBackInvokedCallback`，否则新 Android 默认预测返回会直接关闭 NativeActivity、绕过 C 的 `AKEYCODE_BACK` 处理。保留系统传统返回路径，本轮不加入 Kotlin 回调。
- **状态与返回**：每页滚动/分类留在内存，旋转不重置；系统临时恢复用版本化 POD `UiState`，不提前实现磁盘持久化。返回依次关闭提示、收起设置说明、回歌曲页、退出。
- **验证条件**：连接的 Android 15 真机可安装/启动/截图，但系统拒绝 adb 注入触摸（`INJECT_EVENTS` 权限）；交互与旋转验证使用本机 `aosp_17` x86_64 模拟器（SwiftShader GLES 3）。不修改真机安全设置来绕过限制。

## 02-lx-source-import

- **输入方式**：采用 C 按钮「粘贴导入」统一接收链接、JSON、整个文件文本。无 Kotlin 编辑框/对话框，无文件选择器、存储权限或 ContentProvider 文件读取。剪贴板受 Android 前台权限约束，必须在 Java 主线程读取，因此增加最小 NativeActivity 子类；解析与管理逻辑仍在 C。文件导入是复制文件内容，不是复制文件路径。
- **JSON 范围**：LX 标准交付物是 JS 脚本，并没有统一的批量音源 JSON 导入标准。本轮选择单脚本 `script` / `code` / `content` 字段或 JSON 字符串，严格解码后复用同一元数据解析。暂不支持第三方 sources 订阅、MusicFree 插件清单、LX `gz_` 压缩存储或 `.lxmc` 备份；给出格式错误而不猜测执行。无需引入 JSON/JS/压缩 SDK。
- **真实注释兼容**：依据 LX 自定义源文档及真实六音脚本，允许开头 BOM/空白、`/*` / `/**` / `/*!` 和未知标签。name 必填，其他四项可缺省；版本为任意有界文本，不强制 semver。仅导入存储，不能因为已启用而执行代码；运行时严格留给 03。
- **安全与资源**：UTF-8、NUL、JSON 深度、字段/正文/总量/条数均有界；只按内容拒绝重复，不因同名替换既有源。允许显式 HTTP 导入并在 README 提醒不加密，默认建议 HTTPS，拒绝 HTTPS 降级重定向、URL 凭据以及非 HTTP(S) 协议。元数据首页只是显示，不自动打开。仅接受可信脚本，为未来运行时保留安全提示。
- **持久化取舍**：32 个小集合采用自定义版本化 C 二进制文件，不引入 sqlite；单文件保存正文、ID、启用、顺序，元数据重新派生。工作线程原子替换、失败回滚、损坏拒绝写入，避免解析失败破坏已有音源。不做断电级目录 fsync/备份恢复；损坏文件可先通过调试工具备份，再清除应用数据，后续如需求扩大再升级存储方案。
- **生命周期**：busy 时拒绝追加命令，避免无界积压；退出设置取消位并 join 工作线程，无 detached C 线程残留。缩短 socket 超时、每次读/重定向检查取消，但 Android 系统 DNS 不承诺硬截止，这是当前最小系统 HTTP 桥接的限制。
- **JVM native 装载**：NativeActivity dlopen 不能替代 System.loadLibrary 的 JVM 注册；实测剪贴板回调曾因此找不到 nativePaste，已通过 Kotlin companion 显式装载修复，后续模拟器回调成功。
- **中文元数据**：仅原 UI 文案子集无法显示真实音源描述。针对本轮扩充到 GB2312 常用 6763 汉字及文案，保持纯 C SDF 渲染，代价是 16 MiB atlas/CPU 与同量 GPU 空间；不提前开发完整动态 Unicode 字形缓存，罕见字/部分繁体/emoji 仍回退。来源与许可不变，度量压紧到 <800 行。
- **发布边界**：版本仍为 0.1.0；只更新 02 验收，不增 workflow push 分支触发，不打标签/建 Release。本地构建 APK 仅用于验收，不作为本轮发布产物。
