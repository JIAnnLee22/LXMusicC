# 架构说明

## 01-ui-shell：原生界面骨架

应用入口保持 Android 自带 `NativeActivity`，所有界面、命中测试和状态均为 C；没有 Java/Kotlin 源码，也没有引入 Android UI SDK。

| 文件 | 职责 |
| --- | --- |
| `app/src/main/cpp/main.c` | NativeActivity 生命周期、事件循环、EGL 上下文/窗口、字体加载线程、输入转发与临时状态保存 |
| `render.[ch]` | GLES 3.0 批次渲染：圆角矩形距离场、SDF 图集文字、裁剪与像素/dp 转换 |
| `ui.[ch]` | 四页布局、每页滚动/选择状态、触摸命中、惯性滚动、返回键和空状态 |
| `platform.[ch]` | 仅查询 Android 系统导航栏/状态栏/刘海安全区，契约见 `BRIDGE.md` |
| `font_data.h`、`assets/fonts/ui.sdf` | 离线生成的字形度量与 1024×1024 R8 SDF 图集 |
| `scripts/font-atlas.c`、`scripts/generate-font.sh` | 开发期图集生成，不参与正常 Gradle 构建 |

### 线程和帧循环

- NativeActivity 胶水线程拥有 EGL/GL、UI 状态与输入；不执行网络/磁盘 IO。
- 启动时单独的 pthread 读取字体 asset（1 MiB）；用 C11 atomic release/acquire 发布完成状态，以 `ALooper_wake` 唤醒事件线程，随后在 GL 线程上传纹理。退出先 join，再释放缓冲。
- 纹理 CPU 副本保留至 Activity 结束，窗口/上下文销毁后可直接重传，不再读取磁盘。TERM_WINDOW 销毁全部 GL/EGL 资源，下一次 INIT_WINDOW 重建；失败清理并延迟重试。
- 静止界面阻塞等待事件；触摸、滚动、提示条或生命周期变化请求整帧重画；动画期间非阻塞轮询，`eglSwapInterval(1)` 交给 vsync 限速。失焦/暂停停止绘制并取消手势。
- 图元使用同一顶点格式、同一 shader 与预乘 alpha 混合，顶点按批次上传；内容区与固定页签区之间 flush + scissor，避免滚动内容覆盖页签。不假设 swapchain 保留上一帧，不引入 damage 历史。

### 布局、输入和状态

- 坐标统一为 dp，系统 density 决定像素比例。每次画帧读取 surface 尺寸，内容区与系统安全区取交集，宽屏限制内容最大宽 900dp；播放页宽度超过 600dp 时使用封面/文字双栏，其余页面可滚动。
- 四页共享固定底部导航，各页保留独立滚动位置；歌曲分类、搜索分类、设置展开项保存在 `UiState`。旋转只重排，不重置状态；超出新视口的滚动位置在呈现前夹取。
- 单一活动 pointer 跟踪点击/拖动，多指不会意外触发点击；8dp 位移后转为滚动，取消/失焦/旋转清空速度。惯性速度有上限和指数衰减，内容与命中区域同坐标、同裁剪。
- Manifest 显式关闭预测返回回调，将系统返回送入 NativeActivity 的传统按键路径；返回先关闭提示、再收起设置说明、再回歌曲页；歌曲页返回由 `ANativeActivity_finish` 退出。
- `APP_CMD_SAVE_STATE` 仅保存版本化的 POD 界面状态给系统临时恢复；不是歌曲库持久化，强制停止/主动退出后的冷启动不承诺恢复。

### 文字与范围

本轮采用离线 Noto Sans SC Regular 子集，含 ASCII 及当前 UI 文案，UTF-8 解码后按码点二分查字形，未知字符显示 `?`。图集生成时检测缺字/容量/曲线类型，应用检查 asset 长度、分配和读取结果、shader 编译/link 与 EGL 返回值。后续真实歌名需要扩展为动态字形缓存或完整 CJK 方案。

当前页面没有示例歌曲或虚假播放状态，所有音源/搜索/播放/缓存/库入口均明确标注尚未实现。不访问网络、不新增缓存索引或业务持久化。

### 本轮验证记录

- `./scripts/build.sh` 成功，生成 arm64-v8a / x86_64 两个 ABI；没有新增 Java/Kotlin 源码或测试用例。
- Android 15 真机安装、启动和截图确认中文图集、卡片、四页签及系统导航安全区；触摸注入被系统安全策略拒绝，未修改该设备安全设置。
- `aosp_17` 模拟器（x86_64、SwiftShader GLES 3）验证四页点击、歌曲分类保留、设置展开、滚动后命中、横竖屏、返回关闭说明/回歌曲/退出。最终版本连续四页切换 40 次，进程保持正常，LXMusicC/AndroidRuntime 日志无崩溃。
- 模拟器上 HOME → `am kill` → 启动，设置滚动与展开状态恢复；恢复前后截图 SHA-256 均为 `150d8d311fea4c27b92f65ff96cd02fd99fb3078992efcedcaa3772e6e57b3fc`。
- `git diff --check` 通过；未修改工作流、未打标签或创建 Release。
