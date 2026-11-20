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

## 03-source-runtime：JS 请求执行

| 模块 | 职责 |
| --- | --- |
| `vendor/quickjs/` | 固定 MIT 纯 C 引擎、正则、Unicode、浮点转换，不链接 OS/CLI 标准库 |
| `source_runtime.[ch]` | 独立上下文、任务队列、Promise pump、中断/资源限制、歌曲数组校验与有界 C 结构 |
| `lx_compat.js` | 最小 LX mobile 2.0.0 全局对象、事件能力表、HTTP 参数/回调、buffer 编解码、timer；CMake 转成编译期字符串，无运行时磁盘加载 |
| `RuntimeHttp.kt` / `source_bridge.[ch]` | 系统 HTTP(S) 和 JSON 字节传输，无歌曲/音源业务，JNI 详见 BRIDGE.md |
| `source.c` / `ui.c` | 单槽命令 `SOURCE_CHECK`，仅对启用源显式自检并显示可重试摘要；不改搜索页 |
| `docs/examples/itunes-preview.js` | 可直接粘贴导入的真实公共预览音源，明确标记扩展协议 |

### 调用与数据
`runtime_call(script,provider,action,info_json,http,cancelled,context,result,error)` 只能在后台线程调用。script 必须通过既有 LX 注释/UTF-8 解析；传入原始元数据和 rawScript。provider=NULL 时选择 inited.sources 中首个宣告该 action 的 provider；显式 provider 则校验其 actions。初始化可通过异步 HTTP/timer 发送 inited；没有 handler/能力、初始化/handler 抛错、Promise 没有可执行后续任务、HTTP 失败、JSON/歌曲字段不合法均返回可读错误。没有静默模板回退。

每 action 创建新的 QuickJS Runtime/Context，注入兼容层后执行脚本，等待 inited，再调用 `handler({source,action,info})`。先 pump Promise jobs，后执行最早到期的 HTTP/timer；网络同步走系统桥接，不占 GL/Java 主线程，也不持 service.lock。`lx.request` 回调在同一 JS 工作线程执行，Promise 可继续链式处理；取消函数删除尚未开始的任务。所有异步任务/返回值由 QuickJS 引用计数管理，退出释放 JSValue、请求缓冲、上下文和 Runtime。Activity 取消与 running 状态由 volatile/互斥锁检查，退出 join 后才释放 Activity 上下文。

`RuntimeResult` 由调用者置于 heap，结束调用 `runtime_result_free`。search 必须返回数组，元素契约为：

```js
{ id: '可选 ID', name: '歌名', artist: '歌手', album: '可空专辑',
  duration: 255.333, quality: 'preview-aac', url: 'https://…' }
```

C 规范化前 30 项（has_more 标记截断），逐字段检查类型、UTF-8/NUL/长度。id 最多 127 字节，name/artist/album 各 255，quality 127，url 2047；name/artist/quality/url 必填非空，album 可空/缺省，duration 为有限的 0..86400 秒。下载地址限定 HTTP(S)。`result.json` 保留 handler JSON（≤2 MiB），`info_json` 为首曲 `{type:quality,musicInfo:原始歌曲对象}`，后续可保留脚本扩展字段。musicUrl 校验 HTTP(S) 字符串；lyric 校验 `{lyric:string,…}`，空歌词合法。不是完整 LX 平台 musicInfo 转换层；标准源需要调用方提供平台自己的 id/hash/meta 等字段。

资源上限：32 MiB JS 分配、512 KiB JS 栈；每 action 40 秒截止中断（在 JS/调度点检查）、32 个排队任务、256 个累计任务、16 次 HTTP；单请求与原始响应 ≤2 MiB。HTTP 总时限/取消不保证打断系统 DNS 或写 socket，且同步 HTTP 期间 JS timer 不运行，不能当成强沙箱或硬实时截止。HTTP/歌曲解析失败销毁本次上下文，下次自检重新开始，不改私有音源文件。

### 兼容范围与验收入口
兼容层支持 `on(request)`、`send(inited/updateAlert)`、request 回调与 cancel、setTimeout/clearTimeout、buffer.from/bufToString 的 utf8/hex/base64、Promise/async/await。body 对象转 JSON，form 转 URL 编码，qs 百分号编码；默认尝试 JSON 正文，不能解析时保留文本，json=true 则失败。crypto/zlib/Node/DOM/完整 Buffer/multipart/二进制等未实现，依赖它们的第三方源不能运行，见 DECISIONS.md。updateAlert 接受但不打开网页或展示升级弹窗。

「搜索自检（love）」依次真实 search（page=1,limit=5）、首曲 musicUrl、lyric，每步独立上下文；成功显示首曲及数量，日志有首曲的歌名/歌手/专辑/时长/音质与地址存在性，不记录可能带密钥的完整 URL。失败可重复点击。官方 LX 源不宣告 search 时报告不支持，不调用未宣告 action；示例的 search/itunes/preview-aac 明确是本项目扩展。没有自由输入、多源聚合/结果列表或播放/缓存功能。

### 本轮验证记录
- `./scripts/build.sh` 成功，两个 ABI（arm64-v8a/x86_64）及 Kotlin 编译通过；保留原有非阻塞 NDK 路径 CXX5304 提示。未新增 Java 源码/测试用例。
- 在 x86_64 `aosp_17` 模拟器覆盖安装；将仓库示例按既有私有存储格式加入验收设备（先备份原数据），冷启动经 C 解析恢复后，从音源管理按钮执行。没有使用静态伪造 HTTP 响应。
- 真实查询 `https://itunes.apple.com/search` 的 love 返回 5 曲；首曲 `Love / Keyshia Cole / The Way It Is`，255.333 秒，`preview-aac`，存在 HTTPS previewUrl；musicUrl 的真实 HEAD 成功，lyric 返回 `{"lyric":""}`（12 JSON 字节）。buffer 的 UTF-8/base64 往返、初始化 setTimeout 和 Promise/async 同时经过此链路。
- 关闭模拟器 Wi-Fi/移动数据后显示「无法连接音源服务器，请检查网络后重试」；重新联网并再次点击，同样返回 5 曲和两个 handler 成功，无 AndroidRuntime 崩溃。
- 对已导入的真实 Tisfy 最小源执行自检，报告「音源未完成初始化或没有 request handler」，进程保持正常；没有假装所有 LX 源可搜索。只读审查后补充 base64 无 padding/URL-safe 兼容、QuickJS 数组异常三态与公共请求参数检查。
- 最终构建覆盖安装后再次返回 5 曲和两个 handler 成功；自检前后私有音源文件 SHA-256 同为 `86fce0441f2674f562458e140caeb6e60b89b3a67efc55ee6cd6eb9ad251add9`。自写改动 `git diff --cached --check`（排除 vendor）通过；完整检查仅报告上游原有行尾空白，保留原始 vendor 便于校验。未修改 workflow、未创建标签/Release。

## 04-search：多源搜索界面

| 模块 | 职责 |
| --- | --- |
| `search.[ch]` | 单槽最新查询、后台协调器、最多四路音源任务、取消代次、聚合去重排序、八项分页快照 |
| `source.c:source_snapshot` | 后台复制已启用脚本与元数据，独立 store_lock 保护快照与修改/删除/保存，搜索不借用存储指针 |
| `ui.[ch]` | C 输入框/字母数字软键盘、UTF-8 退格、加载/空/错误/取消/重试、分页列表、选曲进入播放页 |
| `main.c` | 硬件按键转发、快照同步、系统剪贴板请求、启动与退出顺序 |
| `source_bridge.[ch]` / `SourceActivity.kt` | 复用剪贴板传输，按查询代次检查 HTTP 取消；无 Kotlin UI/搜索业务 |

### 调度、锁与生命周期
- GL 线程只写查询、翻页/取消及读取快照；协调器等待最新单槽查询，后台调用 source_snapshot 深拷贝全部启用脚本（总量仍 ≤8 MiB），不在 GL 线程复制脚本、执行 JS、网络或磁盘 IO。音源尚未加载可读失败并允许重试。
- 每轮最多四个 joinable pthread，各自领取源索引，使用独立 QuickJS 上下文调用既有 runtime_call。每源 search 参数为 `{keyword,page:1,limit:30}`；provider=NULL 沿用首个支持 search 的 provider。未声明 search/初始化失败单源报错，不中止其余音源。源完成就发布结果和进度，不等待所有源。
- 每源完成释放 RuntimeResult/JS/脚本快照，整轮 join 后释放 Job。新搜索递增原子 token，覆盖尚未开始的查询；协调器等待上一轮线程释放后才开始新一轮，因此持续重搜也不无限创建线程。每 action 的 32 MiB JS heap/40 秒调度截止等限制保持不变。
- search.lock 保护查询/结果/页码/revision；排序在后台执行，只向 UI 拷贝当前八项（每项为自包含的有界数组）。wake 仅调用 ALooper_wake，不递归取锁。store_lock 保护 Store 深拷贝与修改；原有 service.lock 继续保护管理状态，搜索不持 store_lock 执行网络/JS。搜索中启停/删除音源不破坏已开始的快照，下一次搜索才采用新配置。
- 取消立即置 UI 为已取消并停止接收该代次结果，保留已完成结果；JS 中断/调度与 HTTP 检查点都检查 token。DNS/底层 socket 仍非硬取消，等待期间 UI 可操作。每次合并与完成发布前在锁内再校验 token，旧查询不能覆盖新结果。退出先通知 Kotlin closing，再取消/join 搜索协调器及其全部任务，最后停止音源存储线程。

### 聚合、分页与输入
- 每源规范化前 30 首，后台最多保留相关度最靠前的 240 首；只做有界首批结果，不自动拉取无限远程分页。UI 每页八首，上一页/下一页只切本地窗口，不重新联网；不足一页或越界操作不改变页码。
- 同源非空 id 相同，或歌名/歌手/专辑按 ASCII 大小写与空白归一化后相同且时长差 ≤2 秒，判为重复。跨源重复优先保留设置中靠前的源，再保留该源靠前的歌曲，避免网络完成顺序改变所选来源；不同专辑/时长的版本保留。不是 Unicode 全量 case-fold 或音频指纹匹配。
- 排序依次为歌名精确匹配、歌手精确、歌名前缀、歌名包含、歌手包含、专辑包含、其他；同分按音源设置顺序、源内返回顺序确定。不根据网络快慢排序；进度到达时结果可能重新排序。
- 关键词 ≤255 UTF-8 字节，C 拼 JSON 时转义引号/反斜线/控制字符；软键盘和硬件按键编辑字母/数字，中文及其他字符从系统剪贴板传入，GL 经 revision 消费。UTF-8 删除一次删一个码点；单次粘贴替换输入，不自动触发查询。
- 列表展示歌名/歌手/时长/音质/来源，长文本有界裁切或换行。查询、列表、页码、选曲在 Activity 内切页/旋转时保留，冷启动不持久化搜索历史。既有字体对罕见字/部分繁体/带音符拉丁字母仍可能回退为 `?`。
- 点击任意结果按值复制 SearchItem（含来源、元数据及 search 返回的 URL）后进入播放页；此轮只完成导航与选曲，不调用播放服务、不发声、不缓存，也不保留所有歌曲的原始 JS 扩展字段。05 再接入音频状态机和前台服务，不能把本轮选曲展示当成播放成功。

### 本轮验证记录
- `./scripts/build.sh` 成功（arm64-v8a / x86_64、Kotlin）；原有 NDK 路径 CXX5304 警告不阻塞构建。自写 C 文件最多 428 行、Kotlin 最多 134 行，未新增测试用例、Java 或依赖 SDK。
- 最终 APK 覆盖安装于 `aosp_17` x86_64 模拟器，C 软键盘逐字输入 love；三个启用源在同一毫秒以独立线程开始（token=2，source=4/3/5）。两个公开预览源各返回 30 条，聚合为 26 首；无 handler 的 Tisfy 源单独失败。较后配置的源先返回，较前源到达后仍保留其来源，确认结果不按网络先后选源。
- 首曲 Love / Keyshia Cole，4:15、preview-aac；列表切到第二页显示另一批歌曲，点击首曲进入播放页显示对应元数据。切页保留搜索结果，横竖屏布局和滚动可用；未冒充实际音频播放。
- 关闭 Wi-Fi/数据后查询显示无法连接服务器，恢复后重试成功；关闭 Wi-Fi、移动网络配置 `network delay 3000` + `network speed gsm` 后真实请求报告「音源 HTTP 超时，请重试」，恢复 `delay none` / `speed full` / Wi-Fi 后同词重试返回 30 条。
- 弱网下取消 jazzpiano 立即显示已取消；等待四秒后的截图 SHA-256 与取消时相同（`2db11df4d0a3bf68ea6a1efcb78df51cbd2c68df5e8029d23029810f11a3f40f`），没有迟到结果覆盖；重试代次由 3→5，新轮正常返回。无匹配关键词返回空数组，全部禁用源后显示无启用音源并提供管理入口。
- 系统剪贴板的关键词粘贴可替换输入，超过 255 字节拒绝并提示；沿用 nativePaste 的导入/搜索区分，无 AndroidRuntime 崩溃。验收结束恢复模拟器原音源文件，SHA-256 与备份一致：`86fce0441f2674f562458e140caeb6e60b89b3a67efc55ee6cd6eb9ad251add9`；网络恢复正常。
- `git diff --check` 通过；workflow、Manifest、版本号未改，不创建 tag/Release，仅勾选 04。

## 05-playback-service：前台播放与系统媒体控制

| 模块 | 职责 |
| --- | --- |
| `playback.[ch]` | 进程级 C 播放队列、加载代次、播放/暂停/前后曲/seek/停止、自然结束策略、状态快照与 UI 订阅 |
| `playback_bridge.[ch]` | 服务 GlobalRef、JNI 检查/字节传输、异步服务命令、状态与系统控制回调，完整签名见 BRIDGE.md |
| `PlaybackService.kt` | Media3 ExoPlayer、MediaSessionService/系统通知、焦点/noisy/网络唤醒锁；ForwardingPlayer 将控制交给 C，不保存业务队列 |
| `SourceActivity.kt` | 用户前台操作时投递服务启动，无绑定和 Activity 播放状态 |
| `ui.[ch]` / `main.c` | 当前页替换队列、最小控制按钮与实际进度、前后台订阅/恢复，不涉及缓存/封面/拖动条 |

### 状态与线程
- C 持有有界 SearchItem 深拷贝队列和 PlaybackView，mutex 保护；队列不指向搜索结果/音源存储。选曲、下一首及替换递增加载代次；进度/状态和自然结束均按代次丢弃旧回调，网络地址失败不删除队列，播放可重试。
- GL 只处理内存快照与 JNI 排队，无网络/磁盘 IO。C→Kotlin applyPlayback 复制最多三个有界 UTF-8 字节数组，再 Handler.post 到服务主线程；主线程修改 ExoPlayer/Session，音频加载、解码和 IO 由 Media3 内部线程运行。系统按钮经 ForwardingPlayer 回 C，C 决策后调用原始 ExoPlayer，不通过 wrapper 递归执行。
- Media3 只持有当前曲目的单项 timeline；通知的上一首/下一首命令由 wrapper 显式提供，C 决定边界。有下一首的自然结束自动切换；末曲结束关闭播放请求，不循环。seek 按实际音频时长夹取，示例不再把完整歌曲元数据时长当预览长度。
- Player.Listener 在状态变化时上报，播放中每秒采样进度；C 只发布变化快照。Activity onPause 解绑并 release looper，onResume 重新 acquire/取快照；无界面时不依赖 GL 线程驱动播放。onDestroy 只关闭导入/搜索，服务独立存活；服务销毁清 JNI 引用和输出状态，保留同进程队列供用户前台继续。
- JNI 锁序为 playback.lock → bridge_lock；nativeReady/nativeGone 先更新/释放 bridge_lock，再调用 C 状态发布。Kotlin apply 只排队不重入 C；回调不借用 Activity 上下文。库在 Activity/Service 各自的类初始化显式 System.loadLibrary，可只由服务装载。
- Manifest 声明媒体播放前台类型/权限、WAKE_LOCK；服务主动 addSession 以启用自动媒体通知/前台提升，避免没有 MediaController 时只解码而不显示通知。音频焦点与耳机拔出由 Media3 管理，前台提升被拒、播放网络/解码错误均转为可重试文案。

### 本轮验证记录
- `./scripts/build.sh` 成功，arm64-v8a/x86_64 与 Kotlin 均通过，保留原有非阻塞 CXX5304 提示；自写单文件均满足 C ≤800/Kotlin ≤400 行。没有新增 Java 或测试用例。
- `aosp_17` x86_64 模拟器通过真实 Apple 搜索与音频请求，选 Love / Keyshia Cole 后 AAC 解码并播放约 30 秒预览；C 页显示实际 0:30 而非搜索元数据 4:15，队列 1/8。通知服务 dumpsys 显示 isForeground=true、mediaPlayback 类型 0x2、foregroundId=1001、三个控制按钮。
- HOME、息屏后系统会话保持 PLAYING、进度继续；GSM 来电及 accept 后变为 PAUSED，position=18163 ms 保持，cancel 后恢复 PLAYING、position=18179 ms。通知卡片实际点击暂停/下一首有效，元数据从 Lana Del Rey 变为 Destiny's Child；系统媒体键 play/pause/next/headsethook 同样有效。
- 返回退出 Activity 后日志出现 native activity finished，服务/会话仍在；耳机键恢复后重启 Activity，C 页恢复 Destiny's Child、4/8 及实时进度。暂停时 C seek 将 28383→30023 ms（时长夹取），后退变为 20023 ms；切至末曲及末尾 seek 后按钮可恢复播放，不回到队列头。
- 关闭 Wi-Fi/移动数据后切新曲显示 ERROR_CODE_IO_NETWORK_CONNECTION_FAILED 与可读重试提示；恢复网络后 play 重新 prepare 并播放同曲。未修改模拟器音源文件，未以本地假响应冒充真实网络；未修改工作流或创建发布标签。

## 06-cache：公共目录缓存与离线播放

| 模块 | 职责 |
| --- | --- |
| `cache.[ch]` | C 单工作线程/单槽命令、去重索引、原子持久化、容量限制、六项分页快照、内存播放查找 |
| `cache_bridge.[ch]` | 安全路径段、扩展名选择、JNI 有界 UTF-8 字节协议与异常检查 |
| `CacheStorage.kt` / `SourceActivity.kt` | 系统 HTTP 流式传输、MediaStore 公共目录 IO、旧系统存储授权；无 UI 或缓存策略 |
| `playback.[ch]` / `playback_bridge.c` | 保留原始远程地址，另存本地 URI；加载与继续播放时优先缓存，不改服务 JNI 协议 |
| `ui.[ch]` / `main.c` | 显式缓存当前曲目、缓存管理、占用/上限、二次确认删除/清空、启动/取消/join |

### 数据、线程与播放
- C worker 独占 Store；GL/系统回调仅提交内存命令、分页和读快照。下载、探测、删除、启动加载、fsync/rename 都在 worker，不持 cache.lock 做 IO。忙碌时拒绝新命令，不排无界下载队列；无缓存时明确显示引导。wake 回调只允许 ALooper_wake，不得重入 C 锁。
- 查找按来源 ID/来源名/音质 + 非空歌曲 ID；无 ID 时按歌名/歌手/专辑/相近时长。URL 不作身份键，签名 URL 变化不重复下载；不同来源、不同音质分开保留。成功条目包括完整 SearchItem、URI、实际大小、单调 ID，最多512条；下载前探测同曲已有 URI，存在即返回，不发 HTTP。
- 默认1024 MiB；提供256/512/1024/2048 MiB，低于已用空间的设置拒绝。按实际下载字节和剩余配额强制中断，不自动驱逐旧曲；单曲另限2 GiB。检查文件/启动探测剔除已丢失文件，更新外部改写后的大小；如果外部增大导致超额则暂停新增下载，仍可播放/删除。
- `cache.lock` 保护独立查找表与当前六项快照，内存上限约2 MiB/表；Store 加查找表合计约4 MiB，UI只复制一页。worker 发布时才更新表，播放加载在 playback.lock 内查询 cache.lock；缓存模块不回调播放，不产生反向锁序。下载缓冲32 KiB，不将整曲放进堆或JNI数组。
- 播放队列保留远程 SearchItem，PlaybackView.local_uri 单独记录本地输出；load 优先本地 URI，暂停后继续若查找结果变化则以原位置重新LOAD。缓存页可冷启动直接选曲，无需网络搜索或启用原音源。Media3 内部线程读取 content URI，可 seek/后台播放；不存在、权限/解码失败沿用可读重试状态。
- Activity 结束先设置 Kotlin closing、join缓存线程，释放Store，但保留有界内存查找表供独立服务后台切歌。重新进入后worker从磁盘刷新；服务无 Activity 引用。删除正在播放的文件不主动停止音频，已打开的FD可能继续播放，也可能在后续seek失败；用户可重新选曲。

### 持久化和公共文件
- 私有文件 `internalDataPath/lx-cache-v1.bin`：magic `LXCC 01 00 00 00`、u32记录长度/u32数量/u32 next_id/u64上限，然后按顺序存记录。当前arm64-v8a/x86_64都是小端64位ABI，记录3880字节；校验记录长度、计数、字符串边界/UTF-8、URI、大小、ID/身份唯一性。未来更改结构须迁移版本，不能默默重解释。
- 保存到0600同目录tmp，fflush/fsync/close后rename；损坏索引只读保护，不覆盖公共文件。下载完成后保存索引，失败删除新文件并回滚；单条删除先删公共文件再存索引，清空每16条和结尾保存。若删除后进程中断，下一次启动探测可修复过期索引。
- 公共路径 `Music/LXMusicC/<音源>/<歌手> - <歌名>.<ext>`。C按UTF-8边界缩短路径段，替换控制符、分隔符和非法字符，扩展名优先URL后缀，再按音质判断，未知默认m4a；不信任外部元数据作为路径。系统MediaStore/旧系统数字后缀避让同名文件，不覆盖其它歌曲。
- MediaStore与C索引不是跨进程原子事务：在公共文件发布到索引rename的极短窗口被强杀，可能留下未索引的完整公共音频；不猜测并删除已发布文件。pending/旧part启动清理，普通失败即时回收。清除数据/卸载后不扫描重建旧公共文件，不申请额外读取权限，用户可用文件管理器管理残留文件。完整JNI和旧系统授权路径见 BRIDGE.md。

### 本轮验证记录
- `./scripts/build.sh` 成功（arm64-v8a/x86_64、Kotlin）；无新增测试用例、Java源码或依赖SDK，保留既有非阻塞CXX5304提示。
- `aosp_17` x86_64模拟器实际API37，真实Apple预览音频下载：`Music/LXMusicC/iTunes 公开预览示例/Keyshia Cole - Love.m4a`，995863字节。系统Files应用可浏览Music/LXMusicC/音源目录并显示音频；MediaStore查询显示`is_pending=0`、`_size=995863`。
- 关闭Wi-Fi/移动数据，force-stop冷启动，从缓存列表选择Love/Keyshia Cole；`dumpsys connectivity`显示`Active default network: none`，系统媒体会话为`PLAYING(3)`，界面显示离线播放、实际0:30时长及进度。不是依靠在线播放器内存缓冲验收。
- 断网重复缓存仍只有一个公共文件，提示不会重复下载。单条二次确认删除、重新下载后二次确认清空均令公共文件数归零并显示空状态；上限从1024改512 MiB，强制停止后C索引和界面仍为512 MiB。验收结束恢复网络，保留一个可用缓存，不改原音源数据。
- API26–28权限/legacy目录分支未实机验证；不宣称所有ROM均已验证。没有开始07及以后条目；工作流/版本保持不变，不创建标签或Release。

## 07-player-ui：封面、进度与队列

| 模块 | 职责 |
| --- | --- |
| `player_ui.[ch]` / `ui.[ch]` | C播放页、真实时长与拖动预览、六项队列分页/选曲、模式入口、上滑封面/下滑队列顶部、过渡动画 |
| `playback.[ch]` | 锁内队列快照、绝对索引选曲、四模式与有界随机历史、切歌清状态/递增代次；后台服务独立运行 |
| `cover.[ch]` / `vendor/stb_image.h` | C最新请求单槽线程、取消代次、私有64槽缓存、JPEG/PNG解码/双线性缩放、RGBA所有权转交 |
| `cover_bridge.[ch]` / `CoverHttp.kt` | 有界二进制HTTP传输、系统TLS、取消检查；完整JNI契约见BRIDGE.md |
| `render.[ch]` / `main.c` | RGBA纹理上传、居中裁切/圆角/淡入、CPU当前图保留与EGL重建、前后台帧调度 |
| `source_runtime.[ch]` / `cache.c` | 可选cover字段规范化、原有音频索引v1兼容读取/v2原子写入、重复缓存补元数据 |

### 输入与状态
- 进度条命中区28dp，DOWN/MOVE仅改变UI预览，UP一次下发毫秒seek；轻点同样跳转。目标按实际时长夹取，未知时长只显示不可拖动的轨道。CANCEL/失焦/旋转不提交；当前generation变化则取消旧手势、清预览/旧命中区域并重置封面入场，防止旧曲时长控制新曲。服务状态每秒校准，前台播放页在校准间插值绘制，实际播放状态仍以服务为准。
- 队列仍来自搜索当前页（最多8首）、C API仍上限240；每次只拷六项，不把整队列塞UI。点击绝对索引经load清position/duration/error/local_uri并换代，队列展开时切歌跟随新索引所在页。上滑只从封面开始，避免阻断普通内容滚动和进度拖动；顶部向下滑/返回/按钮收起，左右切歌不实现。
- 模式0顺序到末曲停止；1列表首尾环绕；2仅自然结束重播本曲，手动切歌仍环绕；3每次从当前曲之外抽一首，上一首回随机历史（至多队列长度）。单曲队列随机/循环可重播。模式与队列均只进程内保留，不新增08库/历史持久化。系统通知上下首经同一C策略，结束事件按generation丢弃，并拒绝暂停/停止后的迟到自动续播。
- Kotlin只在Activity创建时指定系统媒体音量流；C不吞音量按键（即使搜索键盘展开）。Media3继续采用系统音频路由/焦点，没有独立音量或输出设备业务。

### 封面线程与资源
- `SourceSong.cover[2048]`位于旧song前缀之后，search可选字符串缺失/无效则留空；示例返回真实artworkUrl100。只下载当前曲，不预取全队列、不执行额外pic handler。地址变化先释放CPU旧图/清GL纹理，再提交最新单槽；同地址可复用已显示的同专辑图。仅有显式重试，不自动无限重试；加载时显示加载文案、失败为空图/音符占位。
- worker同步HTTP后先检查JPEG/PNG魔数及头尺寸，再stb解码，原图≤2048边/4 Mi像素，正文≤2 MiB；最长边缩到512 RGBA，当前CPU图≤1 MiB，原始正文/完整解码临时缓冲处理后释放。GL线程只取内存图片/上传纹理，不做网络/磁盘/解码。UI收到NULL图同样清旧纹理。
- 每次request递增原子代次、清待发布图片；网络取消检查和发布前锁内校验拒绝旧图。停止先取消再join，随后清Activity context，不留下detached线程。当前CPU图由Engine持有，窗口销毁只删GPU，重建时重传；退出join后free。
- 私有`cover-00.bin`..`cover-63.bin`：标准FNV-1a64(URL)%64选槽；magic `LXCV 01 00 00 00`，小端u32宽/高/URL字节数，原URL及RGBA。宽/高各≤512，完整URL精确比较，读完检查EOF；碰撞/截断/坏头删除该槽重新下载。写0600临时文件、fflush/fsync/rename，无独立索引；64槽合计上限约64.13 MiB，另有单工作线程临时文件，不计公共音频配额。
- shader沿用顶点格式，mode2为RGBA并复用圆角距离场；封面前后flush、临时绑定封面再恢复字体atlas，避免批次串纹理；按宽高居中裁切，不拉伸。200ms级封面淡入/入场和队列滑入只在前台播放页刷新，暂停静止恢复阻塞等事件。

### 缓存格式迁移
- 原文件名`lx-cache-v1.bin`保留，新写magic为`LXCC 02 00 00 00`、记录5928字节；v1/3880字节由LegacySong/LegacyItem/LegacyEntry精确定义并静态校验，转换字段到新结构，cover初始化为空。v1仍可直接读取播放，下一次成功写入才原子升级；损坏文件仍拒绝覆盖，不能把sizeof变化误判为正常旧数据损坏。
- 明确缓存已有歌曲时只补齐新cover URL，公共音频不重下；因此升级后已缓存曲可从最新搜索补封面，之后冷启动断网仍能按URL命中私有封面槽。未提供地址的旧曲保留占位，不伪造封面。旧APK不支持读取v2，降级需备份/恢复原索引。

### 本轮验证记录
- 最终`./scripts/build.sh`成功，arm64-v8a/x86_64与Kotlin编译通过；保留既有非阻塞CXX5304提示。自写文件C≤800/Kotlin≤400，无新增Java或仓库测试文件；stb原样vendor例外。
- `aosp_17` x86_64模拟器：旧v1索引恢复1首/0.9 MiB/512 MiB上限，选择原公共音频正常播放；缺封面为音符占位。暂停时从轨道拖到约78.5%，系统MediaSession位置23571ms/实际时长约30024ms，截图显示0:23/0:30；不是按搜索4:15时长seek。
- 真实Apple搜索love返回30条/去重26首；当前页8首入队，Keyshia Cole真实JPEG封面显示，磁盘生成cover槽。上滑封面展开队列、翻页后选曲有效；列表循环在首曲上一首切到第8首God's Property & Kirk Franklin，队列第2/2页高亮第8首。
- 单曲循环：首曲seek近末尾后自然结束，仍是Keyshia Cole且系统位置回到5ms继续PLAYING；随机下一首选到Jhené Aiko，上一首回Keyshia Cole。音量键令STREAM_MUSIC speaker从5到6，恢复后调回；横屏双栏封面/控制、竖屏与滚动正常，系统日志没有AndroidRuntime崩溃。
- 最终APK再次安装，重复缓存原曲只补元数据，索引升级v2/5928字节/1条/总5956字节。关闭Wi-Fi和数据、force-stop冷启动从缓存选曲，connectivity显示Active default network:none，音频实际播放且封面从私有槽显示；随后暂停。公共目录仍仅原995863字节Love.m4a，没有重复音频。
- 结束恢复网络/竖屏和原音源文件，SHA-256仍为`86fce0441f2674f562458e140caeb6e60b89b3a67efc55ee6cd6eb9ad251add9`；保留已验证升级的v2缓存索引。未改workflow/版本，不打标签或创建Release，仅勾选07。自写改动diff检查通过；完整检查仅报告新vendor原始头文件的两处上游行尾空白，保留原样校验。蓝牙设备切换未连接实测，沿用系统路由不宣称覆盖所有ROM。

## 08-library：收藏、历史与歌单

| 模块 | 职责 |
| --- | --- |
| `library.[ch]` | 进程级 C 库工作线程、64槽命令队列、歌曲身份去重、收藏/历史/歌单关系、事务回滚与私有持久化、六项分页与本地搜索 |
| `library_ui.[ch]` / `ui.[ch]` | 库分类/浏览/搜索、名称输入、创建/重命名/删除、歌曲移除二次确认、播放页收藏与添加歌单入口 |
| `playback.c:playback_state` | 当前 generation 的 STATE_READY 且 playing 首次成立才投递历史；不把加载/失败/每秒进度当新播放 |
| `main.c` / `source_bridge.[ch]` | worker首次启动、looper订阅/解绑、库快照、复用短文本剪贴板；无新增Kotlin/Java/JNI签名 |

### 线程、身份和关系
- worker独占Store；GL/Java主线程/播放回调只复制内存命令，IO与搜索/分页扫描均由worker执行。64个FIFO槽有界，满时用户操作拒绝并可重试；历史投递失败会在后续播放状态回调重试。库锁只保护队列、分页请求、快照和looper，不持锁做加载/fsync/rename；锁序为playback.lock → library.lock，库不回调播放模块。
- worker为有意的进程级常驻线程（首次启动后detach），不是Activity任务：只保留私有目录路径和自己的Store，不持有Activity、JNI对象或Engine。Activity结束只释放所acquire的looper；后台服务切歌仍能记录历史。无任务时cond等待，重开界面重新订阅；进程终止由系统一起回收，不在Activity退出时取消未完成库写入。
- 全库最多1024个LibraryTrack，共享稳定单调track ID；收藏最多512个ID，历史最多200个ID，最多32张歌单，每张最多240个ID。不按歌单重复存整曲，移除关系后prune没有任何引用的歌曲；删除歌单不动收藏、历史、其它歌单或公共缓存。内存约5.32MiB Store，加单次修改的同量回滚副本、约0.35MiB命令队列及一页快照。
- 库身份为来源ID/来源名 + 非空歌曲ID；ID缺失时用歌名/歌手/专辑/相近时长（差<2秒）。与音频缓存不同，库不按音质/URL区分同曲；再次添加歌单返回已存在，收藏按钮切换关系，历史再次播放只置顶，超过200首淘汰最旧关系。成功新增/历史更新会刷新共享歌曲的URL/音质/封面，不影响其它列表身份。不同来源不合并，来源删除不会删除已保存的库信息。
- 歌单名称UTF-8≤255字节，去ASCII首尾空白、拒绝空名/控制字符，ASCII忽略大小写拒绝重名；不是全Unicode归一化。本地搜索按歌名/歌手/专辑子串，歌单目录按名称；ASCII不区分大小写，中文按UTF-8子串。查询/翻页用请求代次，只发布/消费匹配快照，每页六项；点击歌曲携带稳定track ID而非页内下标。歌单删除和歌曲移除的确认同时绑定action+ID，避免两种ID碰撞引起误删。

### 持久化与失败边界
- 文件`internalDataPath/lx-library-v1.bin`：8字节magic `LXLB 01 00 00 00`、u32 Store长度、5579832字节Store；固定当前arm64-v8a/x86_64小端64位ABI并static_assert，完整文件5579844字节。头含tracks/next_track/lists/next_list/favorites/history，后为收藏/历史ID数组、32个歌单记录和1024个歌曲记录；未来布局变化必须显式迁移，不能直接改sizeof覆盖旧数据。
- 每个修改先复制Store回滚点，验证/修改/清理无引用记录，写0600同目录tmp，fflush/fsync/close后rename；成功才发布新页和已保存消息，失败回滚内存。加载校验版本/长度/数量/递增ID/UTF-8/地址/有限时长/引用存在性/身份与名称唯一性/EOF；损坏只读保护，不覆盖原文件，不自动恢复tmp。
- 仅保证已报告保存成功的提交；动作刚入队或写盘中强杀可能丢最后未确认操作，未fsync目录，不承诺断电级事务。收藏/歌单不下载音频；库保留搜索返回的远程地址，过期需重新搜索选曲更新，播放仍按原缓存身份优先离线命中。卸载/清除应用数据会删除库；不做导出/同步/扫描公共媒体库，也不持久化播放队列。

### 本轮验证记录
- `./scripts/build.sh`成功（arm64-v8a/x86_64），保留既有非阻塞CXX5304提示；自写单文件≤800行，无新增Kotlin/Java、依赖或仓库测试用例。
- `aosp_17` x86_64模拟器实际操作：创建commute→添加Love/Keyshia Cole→重复添加显示不会重复→重命名commute2；磁盘检查1个track、收藏1、历史1、歌单1且只有1个成员。KEYSHIA本地搜索匹配Keyshia Cole；force-stop/重新启动后歌曲与名称保留。
- 最终APK验证移除歌单歌曲使成员1→0但收藏/历史各1；删除歌单使歌单1→0，收藏/历史不变；取消收藏使收藏1→0，历史仍1。创建7张歌单可翻到第2/2页list7，删除后正确回到6项/第1/1页，无卡在空尾页。
- 损坏文件冷启动显示停止写入保护，原7字节文件SHA-256保持`f526795c95399cea27c055c842c3d6ab018ed0fa4f66f701c28ab22dec28237b`；恢复此前实际UI生成的库快照，SHA-256与备份一致。没有修改音源文件，其SHA-256仍为`86fce0441f2674f562458e140caeb6e60b89b3a67efc55ee6cd6eb9ad251add9`。
- 真实Apple搜索返回26首，当前页8首入队；返回退出Activity后日志确认native activity finished，系统媒体下一首切到Musiq Soulchild并实际PLAYING，磁盘历史从1→2、共享曲目从1→2。重新进入界面后库文件SHA-256仍为`afc3df39322537b3b7e791be39bf1e982529346c110d2f2fef395cd364007e42`，验证记录不依赖Activity存活。最终暂停播放，无AndroidRuntime/Fatal signal崩溃。
- 仅完成08，09/10保持未勾选；版本/workflow不变，不打tag或创建Release。本地APK只用于验收，不作为发布产物。

## 09-settings-polish：设置与界面打磨

| 模块 | 职责 |
| --- | --- |
| `settings.[ch]` | C 单槽工作线程、设置原子保存/失败回滚、快照与原子下载策略；GL 线程调色板 |
| `ui.[ch]` | 音源/缓存入口、直接配额设置、Wi-Fi 开关、三色主题、关于/隐私；窄屏容量按钮两列 |
| `SystemNetwork.kt` / `SourceActivity.kt` / `cache_bridge.c` | 系统默认网络能力转位掩码，回 C 读取策略；完整 JNI 契约见 BRIDGE.md |
| `render.[ch]` / `library_ui.c` / `player_ui.c` | 线性 UTF-8 段落换行、动态状态高度、加载骨架、按钮文字缩放、离屏图元剔除 |

### 设置与生命周期
- 文件 `internalDataPath/lx-settings-v1.bin` 固定8字节：`4C 58 53 50`（LXSP）、版本1、wifi_only（0/1）、theme（0薄荷绿/1天空蓝/2淡紫色）、保留字节0。逐字节编码，不依赖结构体布局/ABI；检查精确长度/EOF/字段范围。缺文件使用开启Wi-Fi限制和薄荷绿；读取失败/损坏提示并保留原件，显式保存允许重建。
- 独立worker加载、保存；GL只提交单槽命令并读取revision快照，不做文件IO。保存0600同目录tmp、fsync/close/rename，成功后才发布新值，失败保持原开关/主题并显示可重试错误。队列忙/未就绪时拒绝点击，不累计无限任务；退出停止接收，完成已接收的保存后join，之后才释放Activity。
- 主题变量仅GL线程经 `settings_palette` 更新，后台只发布整数主题快照，避免颜色数据竞争。缓存上限仍由 `cache.c` 维护和保存，设置与缓存管理页共用按钮/实时快照；容量拒绝原因在设置页可见。
- 下载策略使用一个C原子整数发布“就绪+仅Wi-Fi”状态，冷启动未加载时保守拒绝。Kotlin只查询系统事实并回C判断，网络不符合条件在既有取消检查点抛错误，走MediaStore pending/legacy part清理。已缓存曲目在探测命中后返回，不依赖网络；设置修改、检查/删除缓存均不产生网络请求。不新增下载队列、Android UI或系统权限。

### 布局与资源
- 共用 `render_paragraph` 按字形advance累加、UTF-8边界换行，一次线性扫描取代原逐字重测整行；状态/错误/隐私说明按返回高度推进，不截断关键错误。搜索和库等待期间使用静态骨架，空状态保留管理/前往搜索/重试引导；播放错误按钮明确显示“重试”。
- GLES批次在提交quad前检查当前逻辑裁剪矩形，跳过完全离屏的卡片/字形，部分可见仍由scissor精确裁剪。render_begin/unclip重置逻辑裁剪，封面纹理flush/恢复行为不变；分页上限维持原值，不按全库数量增长绘制/命中数组。
- 窄屏（区域<360dp）缩至12dp侧边距，容量按钮在可用宽<360dp时变为2×2；按钮文字按宽度缩小以保留完整操作名，外部歌名仍有界省略。长播放器错误与缓存说明动态推进后续控件，横屏双栏/短屏滚动继续可达。安全区沿用系统栏/刘海与contentRect交集，不重复扣边距；旋转后的滚动在提交帧前测量并夹取。

### 本轮验证记录
- 最终 `./scripts/build.sh` 成功，arm64-v8a/x86_64与Kotlin编译通过；保留既有非阻塞CXX5304提示。自写C≤800行、Kotlin≤400行，未新增Java、SDK或仓库测试文件。
- `aosp_17` x86_64模拟器：三色即时覆盖全页，8字节设置实际从`wifi=1/theme=1`切到`wifi=0/theme=2`再恢复`wifi=1/theme=2`；force-stop/覆盖安装后仍为淡紫色且Wi-Fi限制开启，原缓存上限512MiB不丢。
- 真实Apple搜索love返回26首；打开未缓存曲目，Wi-Fi关闭而蜂窝默认网络117仍联网时缓存被拒绝并显示可读原因，未增加缓存。关闭限制后同一入口完成真实预览下载，缓存数1→2、占用约1.9MiB；结束恢复Wi-Fi与限制。已有缓存与库内容继续可用。
- 截图检查1080×2424竖屏、2424×1080横屏、840×1600（320dp宽）窄屏；设置/关于说明可换行和滚动，容量窄屏两列、底栏在系统手势区之上。主题与布局检查使用SwiftShader，不宣称替代全部真机/ROM性能验收。
- 最终运行时连续操作305秒：164轮四页切换与双向滚动，共1968次tap/swipe，PID始终16398，命令退出0，LXMusicC/AndroidRuntime/DEBUG无崩溃日志。54次内存采样总PSS为67674..68616KiB，末次68390KiB；Native Heap PSS为34018..34224KiB，末次34220KiB（约+202KiB后平台稳定），未见持续增长。此项是模拟器空状态/已有库与设置页的持续操作，真实搜索长列表另以上述26首分页/滚动验证，不冒充长时间联网压力测试。
- 额外运行 `./scripts/build.sh :app:lintDebug`：首次发现既有服务启动点缺少Media3 UnstableApi opt-in，仅在既有 `requestPlaybackStart` 增加显式注解后重跑，构建和lint均成功（0错误，原有SDK/依赖版本等警告保留）。注解不改变运行时行为；未新增测试用例。音源文件SHA-256仍为`86fce0441f2674f562458e140caeb6e60b89b3a67efc55ee6cd6eb9ad251add9`。
- 只勾选09，10仍未开始；README仅更新功能/使用，JNI细节留在docs。版本仍0.1.0，workflow不变，不创建标签或Release，本地APK仅验收。

## 10-release：1.0.0 发布准备

### 发布链路
- Gradle 版本为 1.0.0 / versionCode 2，纯 C 设置与关于同步；功能/安装说明定稿在 README、CHANGELOG，发布工程说明在 `docs/RELEASE.md`。没有新 JNI、Kotlin/Java 文件或业务功能改动。
- Release 变体配置签名，支持四个 `LX_RELEASE_*` 环境变量；全无时采用现有调试证书，全有时使用外部 keystore，缺项拒绝配置。无专用发行证书导致的跨构建覆盖风险已明确记录，不把调试签名称为稳定发行身份。
- CI 共用 `scripts/build.sh`；Release 构建或签名验证失败立即终止，只收集 `lxmusicc.apk` 与 SHA256SUMS。外层 `write_summary` 同步附件名、签名警告和固定 SDK 版本；既有 bootstrap 骨架模板是初始 0.x 工程模板，本轮不将其重构为 1.0 发布模板，现有仓库不会重建覆盖它。

### 本轮验证记录
- `./scripts/setup-android-sdk.sh` 成功：现有 SDK 齐全，无需安装；API36 / Build Tools36.0.0 / NDK29.0.14206865 / CMake4.1.2 与 Gradle、外层脚本常量和里程碑文档逐项核对一致。`bash -n` 检查构建、SDK、外层脚本通过。
- `./scripts/build.sh` 与 `./scripts/build.sh :app:assembleRelease` 均成功（最终复跑分别约 1 秒）；Release 的 lintVital 通过，保留本机 Nix SDK 既有 CXX5304 路径警告，无错误。未新增测试用例。
- `apksigner verify --verbose --print-certs` 通过 APK Signature Scheme v2；`aapt dump badging` 确认 package `jiannlee22.lxmusicc`、1.0.0 / 2、minSdk26、targetSdk36、arm64-v8a / x86_64，未设 application-debuggable。本次验收包 SHA-256 为 `3e9aa2b416ecf6d5b834e5b8c439434a915803ce6bcae8db2febd5ace3c5ea15`（仅对应本地包，不承诺 CI 重构建逐字节相同）。
- 无签名参数的默认路径成功；四变量显式指向已有本机调试 keystore 的路径构建并验签成功，未生成新密钥；仅设置 KEY_ALIAS 的不完整配置按预期失败并给出中文错误，之后恢复无参数复跑成功。带参数的 build.sh 已实际多次成功执行，不存在只验证无参数构建的遗漏。
- Android 17 x86_64 模拟器与 Android 15 arm64 真机均由 0.1.0 / 1 用 `adb install -r` 覆盖安装成功，未卸载或清数据。冷启动 `am start -W` 返回 ok，模拟器约183ms、真机约315ms；真机/模拟器截图均显示完整歌曲页。模拟器继续切换搜索、正在播放、设置，设置显示1.0.0，原收藏、3个音源、2首缓存与淡紫色/Wi-Fi设置仍在；检查进程存活，模拟器当前进程 LXMusicC/AndroidRuntime 日志没有崩溃。
- 两份 workflow 经 YAML 解析及所有 run 段的 bash 语法检查通过；自动断言无分支 push 触发，PLAN 的 diff 只有10复选框变化，`git diff --check` 通过。签名包与 SHA256SUMS 附件收集在被忽略的 logs 目录本地复核，校验通过。
- 本轮不手动创建标签、Release 或上传 APK；仅勾选10，保持标签触发和普通分支 push 无安装包产出。远端正式附件下载/安装需外层 pi-loop.sh 发布后复核，本地验证不冒充正式 Release 已发布成功。
