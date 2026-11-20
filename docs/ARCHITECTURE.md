# 架构说明

## 01-ui-shell：原生界面骨架

01 阶段入口为 Android 自带 `NativeActivity`，所有界面、命中测试和状态均为 C；该阶段没有 Java/Kotlin 源码或 Android UI SDK。02 阶段在此基础上添加无 UI 的最小 Kotlin 系统桥接，见下文。

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

01 阶段页面没有示例歌曲或虚假播放状态，音源/搜索/播放/缓存/库入口均为占位，不访问网络或新增业务持久化；02 阶段仅开放音源导入与管理。

### 本轮验证记录

- `./scripts/build.sh` 成功，生成 arm64-v8a / x86_64 两个 ABI；没有新增 Java/Kotlin 源码或测试用例。
- Android 15 真机安装、启动和截图确认中文图集、卡片、四页签及系统导航安全区；触摸注入被系统安全策略拒绝，未修改该设备安全设置。
- `aosp_17` 模拟器（x86_64、SwiftShader GLES 3）验证四页点击、歌曲分类保留、设置展开、滚动后命中、横竖屏、返回关闭说明/回歌曲/退出。最终版本连续四页切换 40 次，进程保持正常，LXMusicC/AndroidRuntime 日志无崩溃。
- 模拟器上 HOME → `am kill` → 启动，设置滚动与展开状态恢复；恢复前后截图 SHA-256 均为 `150d8d311fea4c27b92f65ff96cd02fd99fb3078992efcedcaa3772e6e57b3fc`。
- `git diff --check` 通过；未修改工作流、未打标签或创建 Release。

## 02-lx-source-import：音源导入与管理

| 模块 | 职责 |
| --- | --- |
| `source_parse.c` / `source.h` | 纯 C UTF-8 校验、严格 JSON 解码、LX 首块注释元数据提取，不执行 JS |
| `source.c` / `source.h` | 单工作线程、单槽命令队列、脚本去重、启停/排序/删除、版本化私有存储与元数据快照 |
| `source_bridge.[ch]` / `SourceActivity.kt` | 系统剪贴板、HTTP(S)、取消与字节传递；JNI 契约见 `BRIDGE.md` |
| `ui.[ch]` | 设置内音源子页、每页三项、UTF-8 换行、加载/空/错误状态、删除二次确认 |

### 数据与边界
- 原始粘贴内容先识别 HTTP(S) 直链并下载，否则直接解析。UTF-8（可有 BOM）的 JS 文件内容需以块注释 `/* ... */` 开头，兼容 `/**` 与 `/*!`。扫描最多 64 KiB 的头部，提取 `name/description/version/author/homepage`；name 必填，其他可缺省、未知标签忽略。只校验格式和元数据，不做 JS 语法验证。
- JSON 接受单个 JSON 字符串，或恰有一个字符串类型 `script`/`code`/`content` 字段的对象/包装结构；解码标准转义与 UTF-16 代理对，拒绝 NUL、不合法 UTF-8、尾部垃圾、超过 24 层嵌套及多个候选脚本。不是批量订阅/压缩备份导入器。
- 单次输入/正文 ≤2 MiB、总脚本 ≤8 MiB、音源 ≤32 项；name/author ≤255、description ≤1023、version ≤127、homepage ≤511 UTF-8 字节。超长字段拒绝而不是截断。正文按内容去重，不按名字覆盖。
- 工作线程独占脚本和 Store，C 的 UI/Java 回调只入队，锁保护 busy/message/revision 与元数据快照；网络、解析、加载、保存都不持锁运行。UI 仅在 revision 改变时复制有界快照，绘制每页三条，不在渲染线程读盘或下载。下载正文和解析临时缓冲处理后即释放；持久脚本以 8 MiB 上限留存。

### 私有持久化
- 文件：`ANativeActivity.internalDataPath/lx-sources-v1.bin`（应用私有 files 目录，不需要公共目录权限）。8 字节 magic/version `LXSC 01 00 00 00`，随后小端 u32 的 count、next_id；每条为 id、enabled（0/1）、正文长度三个 u32 和 UTF-8 脚本。文件顺序就是音源顺序；元数据重启时由 C 重新解析，避免与正文不同步。
- 保存先对 Store 做浅拷贝回滚点，写同目录 `.tmp`（0600）、fflush/fsync/close 后 rename；成功才发布新列表并释放已删除脚本，失败回滚内存。此机制针对正常退出/进程中断，不承诺断电级目录事务。
- 冷启动验证 magic、计数、ID 唯一性、启用值、长度、总量、脚本及文件尾；整体成功后才发布。损坏/读取失败不自动覆盖原文件，进入只读错误状态以保护数据；未完成的 tmp 不参与恢复。
- Activity 销毁置取消位、停止接收命令、join 工作线程，再释放上下文；系统 DNS 的不可硬取消限制见 `BRIDGE.md`。

### 元数据文字
离线 SDF 扩为 4096×4096 R8（16 MiB CPU 副本 + 16 MiB GPU 图集），加入 GB2312 的 6763 汉字与 C/Kotlin 文案，共 6870 字形。仅首次在字体线程读取；保留 CPU 副本用于 EGL 重建。这样常见音源名称/描述可以完整显示，罕见字、部分繁体、emoji 仍为 `?`；不是完整 Unicode 字体。图集和度量生成方式见 `assets/fonts/README.md`。

### 本轮验证记录
- `./scripts/build.sh` 成功（arm64-v8a / x86_64、AGP 内置 Kotlin）；没有新增 Java 源码或测试用例。原有 NDK 路径 CXX5304 提示仍在，不影响构建。
- `aosp_17` x86_64 模拟器：通过系统剪贴板粘贴 `https://raw.githubusercontent.com/pdone/lx-music-source/main/sixyin/latest.js`，真实下载 333015 字节，显示「六音音源」、描述、v1.2.1、作者六音和 homepage；兼容真实 `/*!` 头。
- 真实样本 SHA-256：`a407ffe0ab23c29cd32ce249b83e61c8e2b70719895f16034566ad7557282785`（第三方 latest 会变化，未将其脚本提交进仓库）。
- 另从 `https://gist.githubusercontent.com/LetMeFly666/0d1b6885f7eb42f563ce5964a03f7edb/raw` 获取 Tisfy 最小音源，先作为 JSON 的 `script` 字段粘贴导入（包含 `\u` 解码），删除后再粘贴完整文件文本导入成功。
- 切换六音为禁用并下移，force-stop 后启动：Tisfy 在前、六音在后且禁用状态保留；二次确认删除使计数 2→1，重新导入文件文本 1→2。最终版本覆盖安装/冷启动再次显示恢复数量及状态。
- 非法 JSON、普通非音源文本、无法连接的 HTTP 地址均显示可读错误，仍可再次粘贴；失败前后私有存储 SHA-256 同为 `578a63c4df0afa3d9574ca9f786f749b39e1da1238f5a6a980932fffeacbc556`。未触发最终版本 AndroidRuntime 崩溃。
- 横竖屏、滚动布局、返回管理上级正常；`git diff --check` 通过。未改 workflow、未创建标签/Release；仅勾选 02，未实现 JS 运行时或后续搜索。
