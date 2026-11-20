# 决策记录

## 01-ui-shell

- **最小边界**：保持 NativeActivity + EGL/GLES 3，四页只提供布局、滚动、选择状态和空状态。不做输入法、导入、真实搜索、播放、下载或库数据；相关入口明确显示尚未开放，避免把预览当成可用音乐功能。
- **渲染方案**：参考同级 `CUIDemo` 的 SDF/批次思路，重新实现小型 C 渲染器，不搬入演示 UI、Vulkan、图片后端、FBO 和 damage 历史。静止不刷帧，变化时整帧绘制，正确处理 Android 不保留交换缓冲的情况。
- **字体**：离线生成 Noto Sans SC Regular 的 UI 文案子集，避免启动时在主线程读完整字体、生成 SDF 或引入大字体包。实机发现 stb 的 SDF 对 CFF 三次曲线不完整，因此明确拒绝 CFF，使用 TrueType 二次曲线字体。当前未知字符回退 `?`，真实歌名的动态 CJK 字形缓存留待对应功能条目。
- **安全区**：实机 targetSdk 36 下 `contentRect` 仍覆盖系统导航栏，不能仅依赖它。新增纯 C 框架 JNI 只读查询系统栏/刘海，与 contentRect 取交集；不为查询安全区引入 Kotlin Activity 或 UI。异常与返回值契约见 `BRIDGE.md`。
- **返回兼容**：在 Activity manifest 关闭 `enableOnBackInvokedCallback`，否则新 Android 默认预测返回会直接关闭 NativeActivity、绕过 C 的 `AKEYCODE_BACK` 处理。保留系统传统返回路径，本轮不加入 Kotlin 回调。
- **状态与返回**：每页滚动/分类留在内存，旋转不重置；系统临时恢复用版本化 POD `UiState`，不提前实现磁盘持久化。返回依次关闭提示、收起设置说明、回歌曲页、退出。
- **验证条件**：连接的 Android 15 真机可安装/启动/截图，但系统拒绝 adb 注入触摸（`INJECT_EVENTS` 权限）；交互与旋转验证使用本机 `aosp_17` x86_64 模拟器（SwiftShader GLES 3）。不修改真机安全设置来绕过限制。
