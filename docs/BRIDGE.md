# JNI 桥接契约

UI 和业务逻辑全部在 C。C 通过 Android 框架 JNI 只读查询安全区；02 阶段新增 `SourceActivity.kt`（NativeActivity 子类），仅封装系统剪贴板与 HTTP(S)，没有 Kotlin UI 或 Java 源码。

## 系统安全区（01-ui-shell）

C 入口：`bool platform_insets(ANativeActivity *activity, int insets[4])`，位于 `app/src/main/cpp/platform.c`。调用线程为 NativeActivity 胶水/GL 线程（非 Java 主线程），只读取已存在的 Window/View 状态，不修改 View；按需 attach，退出时 detach。返回 `true` 时写入左/上/右/下物理像素；`false` 时保持调用者原有值，等待下一帧重试。

| 框架接收对象 | 函数名 | JNI 签名 | 返回值 / 处理 |
| --- | --- | --- | --- |
| `Activity` | `getWindow` | `()Landroid/view/Window;` | Window；null 为失败 |
| `Window` | `getDecorView` | `()Landroid/view/View;` | Decor View；null 为失败 |
| `View` | `getRootWindowInsets` | `()Landroid/view/WindowInsets;` | 附着前可能 null，稍后重试 |
| `WindowInsets` | `getSystemWindowInsetLeft/Top/Right/Bottom` | 各为 `()I` | API 23+，系统栏像素；负值为失败 |
| `WindowInsets` | `getDisplayCutout` | `()Landroid/view/DisplayCutout;` | 仅 API 28+；null 表示无刘海 |
| `DisplayCutout` | `getSafeInsetLeft/Top/Right/Bottom` | 各为 `()I` | 仅 API 28+，刘海安全边距；与系统栏逐边取最大 |

每次类/方法查找和调用都检查异常，异常清除并记录 `LXMusicC` 日志；对象返回值检查 null，整数返回值检查非负。局部引用限定在 `PushLocalFrame/PopLocalFrame` 内，帧分配失败也清除异常。系统栏使用 API 23 的兼容查询（API 30 起废弃但仍可用）；若后续 Android 改变兼容行为，应替换为 `getInsets(Type.systemBars() | Type.displayCutout())`。

其余生命周期操作使用 NDK API：`ANativeActivity_finish` 关闭 Activity；`APP_CMD_SAVE_STATE` 由 native_app_glue 管理临时界面状态。

## 音源输入和下载（02-lx-source-import）

桥接实现在 `app/src/main/cpp/source_bridge.[ch]` 与 `app/src/main/kotlin/jiannlee22/lxmusicc/SourceActivity.kt`。

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestSourcePaste` | `()Z` | NativeActivity 胶水线程调用，投递 Java 主线程读取剪贴板 | true 仅表示已投递；false/异常为失败；不创建 View，不读取剪贴板 URI 对应文件 |
| SourceActivity → C | `nativePaste` | `([BLjava/lang/String;Z)V` | Java 主线程 | 04 新增 search 标记：false=音源导入、true=搜索关键词；标准 UTF-8 字节数组，失败 bytes=null、error 为可读错误。C 检查长度/UTF-8 后投递，不持有 JNI 引用 |
| C → SourceActivity | `fetchSource` | `(Ljava/lang/String;)[B` | C 音源工作线程，同步等待系统 HTTP(S) | 非 null 字节数组，首字节 1=正文字节、0=错误文本；剩余部分为标准 UTF-8。C 检查异常/null/长度/状态/UTF-8，成功返回 heap 字符串，调用者 free |
| C → SourceActivity | `cancelSourceFetch` | `()V` | NativeActivity 胶水线程退出前 | 置 volatile 取消位，无网络 IO；下载在下一阻塞点结束后检查取消 |

C 包装入口：`bool source_bridge_paste(ANativeActivity*,bool search)`、`char *source_bridge_fetch(void*,const char*,char error[256])`、`void source_bridge_cancel(ANativeActivity*)`。下载回调的 context 为当前 ANativeActivity；工作线程退出并 join 后才释放 Activity 相关上下文。取消位也在 Kotlin `onDestroy` 的 `super` 调用前设置；已关闭/正在销毁的 Activity 不执行排队的剪贴板回调。C 停止服务后拒绝入队。

所有 C 端 GetObjectClass/GetMethodID、字符串/数组分配读取及 Call*Method 都检查异常与返回值，异常清除并记录 `LXMusicC`；PushLocalFrame/PopLocalFrame 限定局部引用，按需 attach/detach。URL 仅接受 ASCII（非 ASCII 路径需百分号编码），避免 NewStringUTF 的 modified UTF-8 歧义；正文使用 ByteArray，不用 GetStringUTFChars。后者仅用于桥接内固定 BMP 中文错误消息，取出后复制、立即 Release。

下载最多 5 次重定向，校验每跳 HTTP(S)、禁止 URL 凭据及 HTTPS 降级；单个正文最多 2 MiB，连接超时 2 秒、读超时 1 秒，循环检查 30 秒总时限和取消位。系统 DNS/底层调用不保证遵守硬截止，取消为尽力而为，不在主线程关闭 socket。失败不修改存储，可重新粘贴重试。02 阶段 HTTP 明文仅用于用户显式粘贴的地址；03 阶段显式自检还允许可信脚本发起 HTTP 请求，均建议使用 HTTPS。导入/启用不自动执行，元数据 homepage 不自动访问。

## 音源运行时 HTTP（03-source-runtime）

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestRuntime` | `([B)[B` | 音源 C 工作线程，同步调用 `RuntimeHttp.execute` | 非 null 标准 UTF-8 JSON 字节；成功 `{statusCode,headers,body}`，传输失败 `{error}`；HTTP 非 2xx 仍是成功传输，由脚本判断 |

C 包装：`char *source_bridge_http(void *context,const char *request,char error[256])`；context 为当前 `ANativeActivity*`。请求 `{url,method,headers,body,timeout}`，body 为字符串或 null；正文 JSON/form 编码、查询参数与 LX 回调由 C 所运行的 JS 兼容层处理，Kotlin 不处理 LX 事件或歌曲。返回 JSON 中 headers 名小写、body 为原始 UTF-8 字符串（即便 Content-Type 是 JSON）；兼容层再尝试解析并向 `(err,response,body)` 传递结果。无新增 Java→C 回调。

两向都是 ByteArray：C 检查 GetObjectClass/GetMethodID、NewByteArray、SetByteArrayRegion、CallObjectMethod、GetArrayLength、GetByteArrayRegion 的异常和返回值，Push/PopLocalFrame、attach/detach，读取后不保留 JNI 引用。请求上限 2 MiB；原始响应上限 2 MiB，gzip 解压后也检查；JSON 封装可能转义膨胀，C 另限 `2 MiB × 6 + 64 KiB` 并验证 UTF-8。失败返回 heap=NULL + 可读错误；正常返回 heap JSON，由运行时 free。

HTTP 支持 GET/POST/PUT/DELETE/PATCH/HEAD、请求头和 UTF-8 正文；仅无凭据 HTTP(S)、ASCII/百分号地址，禁止 HTTPS 降级。最多 5 跳重定向；303 和 POST 的 301/302 改 GET（HEAD 保持），跨站去除所有自定义头，携带正文的跨站跳转拒绝。Host/长度/连接/压缩等传输头由系统控制。连接超时最多 2 秒、读超时最多 1 秒，单请求总时限由 timeout 夹在 100 ms..30 s（默认 15 s）；读写分块检查取消/时限。状态 ≥400 读取 errorStream；HEAD/空响应合法。仅 UTF-8 文本，非 UTF-8/multipart/二进制响应明确不支持。TLS 采用系统验证，不绕过证书。

销毁仍通过 `cancelSourceFetch()V` 的 volatile closing 位中止导入和运行时 HTTP；NativeActivity 退出时先取消再 join。无网络 IO 在主/GL 线程；不在主线程关闭 socket。系统 DNS、建连/写 socket 等底层调用不承诺硬总时限；JS cancel 函数可删除尚未发出的队列任务，已开始的同步系统请求只能在返回/读写检查点看到 Activity 取消和超时，JS timer 在此期间不会并行执行。

## 搜索输入与按次取消（04-search）

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestSearchPaste` | `()Z` | GL/胶水线程调用，投递 Java 主线程 | 与 requestSourcePaste 共用系统剪贴板封装；nativePaste 的 search=true，仅接受最多 255 个 UTF-8 字节，C 通过搜索快照交给 UI，不解析或导入为音源 |
| C → SourceActivity | `requestRuntimeSearch` | `([BJ)[B` | 最多四个搜索 C 工作线程 | data 与 requestRuntime 相同；token 为本轮非零代次，响应仍为标准 UTF-8 JSON，错误返回 `{error}` |
| SourceActivity → C | `nativeSearchCancelled` | `(J)Z` | 同一搜索 HTTP 工作线程 | 以原子读比较代次；true 表示已取消、被新查询替代或正在退出，无锁、无 IO、无 UI 访问 |

`source_bridge_search_http(context,request,token,error)` 复用原有 attach/local-frame/字节检查路径；额外 jlong 参数对应 Kotlin Long。调用 nativeSearchCancelled 的异常会被 RuntimeHttp 的异常路径转换为错误，C 同样校验返回值；C 调用 Kotlin 方法仍逐次检查 JNI 异常/null/长度/UTF-8。搜索取消只递增 C 代次，不调用会永久关闭桥接的 cancelSourceFetch，不影响导入和自检。Kotlin 在 HTTP 原有的开始、重定向、读写块和响应完成检查点调用取消判断；已阻塞的 DNS、建连、socket 操作仍不能硬中断。即使底层晚返回，C 二次检查代次后丢弃旧结果。

输入框、字母/数字软键盘、UTF-8 退格和硬件按键编辑均为纯 C；中文通过系统剪贴板输入。不新增 Kotlin View、InputConnection 或输入法 UI。Activity 退出先置 closing，再 search_stop（取消/join 全部任务），最后 source_stop，确保所有工作线程退出后才释放 Activity。

**JVM 装载要求**：Kotlin companion 显式 `System.loadLibrary("lxmusicc")`；NativeActivity 自己的 dlopen 不会把库登记到 JVM ClassLoader 的 native 方法查找表，单靠 manifest 的 `android.app.lib_name` 会导致 nativePaste 出现 UnsatisfiedLinkError。AGP 9 使用内置 Kotlin 支持，无新增 Kotlin 插件/第三方运行时 SDK。

## 前台播放服务（05-playback-service）

实现：`playback.[ch]`、`playback_bridge.[ch]`、`PlaybackService.kt`；服务也显式装载同一个 native 库。所有队列内容为 C 的自包含 SearchItem 值，不携带 Activity 或音源脚本引用。

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestPlaybackStart` | `()Z` | GL/胶水线程调用，投递 Android 主线程 | true=已投递前台用户操作的 startService；false/异常失败，异步失败走 nativeFailure |
| PlaybackService → C | `nativeReady` | `()Z` | Android 主线程 onCreate | 登记服务 GlobalRef 和 applyPlayback 方法，true=登记成功，C 尝试排入当前曲目；false=登记失败，服务停止，允许前台重试 |
| PlaybackService → C | `nativeGone` | `()V` | Android 主线程 onDestroy | 删除服务 GlobalRef，断开输出，保留 C 队列/位置，不借用已销毁 Activity |
| PlaybackStart → C | `nativeFailure` | `()V` | Android 主线程 | 清启动中标记，发布可重试启动错误 |
| C → PlaybackService | `applyPlayback` | `(I[B[B[BJJZ)Z` | GL 或服务回调线程调用，仅 Handler.post；执行在 Android 主线程 | 参数 action、URL/title/artist 标准 UTF-8 字节数组、positionMs、generation、autoplay；true=已入队，false/异常失败。只有 LOAD 使用三个数组，其余为 null |
| PlaybackService → C | `nativeCommand` | `(IJ)V` | Android 主线程，媒体会话/耳机按键/解码结束 | action=2 播放、3 暂停、4 seek、5 下一首、6 上一首、7 自然结束、8 停止；Long 为 seek 毫秒，action=7 时为结束曲目的 generation，其余忽略 |
| PlaybackService → C | `nativeCanSkip` | `(I)Z` | Android 主线程，Media3 查询 | delta=±1，true=目标存在；无 IO、不暴露队列指针 |
| PlaybackService → C | `nativeState` | `(JIZZJJ[B)V` | Android 主线程，Player.Listener 及播放中每秒 | generation、Media3 state(1..4)、isPlaying、playWhenReady、positionMs、durationMs、可空错误 UTF-8 字节数组；未知时长为 0。C 校验长度≤255/UTF-8/非负值/状态，丢弃旧代次 |

C 的 `playback_bridge_start/apply` 检查 attach/GetObjectClass/GetMethodID/GlobalRef/局部帧/字节数组分配写入/CallBooleanMethod 的结果与异常；清异常并用 LXMusicC 记录，不使用 modified UTF-8 传歌曲元数据。service 全局引用只由 bridge_lock 保护；nativeReady/nativeGone **先释放 bridge_lock，再进入播放锁**。C 调用 apply 时持播放锁，Kotlin 仅 post，禁止同步回调 C，避免重入与锁反转。

播放快照/队列用独立 playback.lock；进度回调只有内容变化才发布。UI 订阅的 ALooper 在锁内 acquire/release，Activity 暂停或退出解除订阅，不让息屏回调唤醒 GL 线程；恢复重新订阅并读取最新快照。服务不持有 Activity，Activity 的 closing、HTTP 取消和 source/search_stop 均不停止播放。进程死亡不恢复队列，也不从后台擅自重启播放。

Media3 负责系统音频焦点与 noisy（拔耳机）处理；瞬态焦点丢失暂停、恢复后继续，永久丢失后手动播放。前台提升失败由 MediaSessionService.Listener 转为暂停和可读错误。媒体会话通知豁免 Android 13 POST_NOTIFICATIONS 运行时授权，因此不请求无关通知权限；服务声明 mediaPlayback 类型及对应前台权限、WAKE_LOCK。外部媒体控制可控制播放/seek/切歌，但不能替换或修改 C 队列。

## 公共目录缓存（06-cache）

实现：`cache_bridge.[ch]` → `SourceActivity.kt` → `CacheStorage.kt`。没有新增 Java→C 回调；权限提示仅投递主线程，授权后用户再次点击缓存。

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestCache` | `(I[B[B[B[BJ)[B` | 仅缓存 C 工作线程，同步系统 IO | action、url、folder、filename、uri、maxBytes；四个可空 ByteArray 都是标准 UTF-8。返回非 null ByteArray，首字节 1 成功、0 失败，余下为 UTF-8；失败为可读错误 |

- **action=1 下载**：url/folder/filename 非空，uri=null；maxBytes 是 C 计算的剩余配额，Kotlin 在声明长度检查和每次写块前强制限制，并另限单曲 2 GiB。成功正文为十进制实际字节数 + `\n` + MediaStore 返回的 `content://media/...` URI，无尾换行；不把音频内容带回 JNI。
- **action=2 探测**：只需 uri，实际打开只读文件描述符查询 statSize，而不是信任过期的 SIZE 列；成功正文同下载。协议中的固定错误 **`缓存文件不存在`** 表示丢失/零长，C 可移除索引并重新下载；权限不足或临时访问失败用不同错误，C 保留索引，禁止盲目重下。修改此固定文案必须两端同步。
- **action=3 删除**：只需 uri；成功正文任意，不存在也成功；失败不移除 C 索引。API26–28 查询 DATA 并兜底删除真实文件；权限不足不弹越权删除授权 UI。
- **action=4 清理**：不需数组参数；删除本包在 `Music/LXMusicC/` 下的 pending 条目，并尽力删除超过十分钟的 `.part`；成功正文任意。仅后台启动时调用，不扫描导入其他音乐。

C 包装 `cache_bridge_io` 检查 GetEnv/attach、局部帧、类/方法查找、数组创建/写入/调用/长度/读取的异常与返回值；清异常并记录 LXMusicC。响应最多 1023 字节，检查 UTF-8、大小范围、数字分隔与 URI 前缀/长度；数组局部帧结束释放，线程退出 detach。歌名/路径不经 modified UTF-8；路径段清理与扩展名推断由 C 执行。

API29+ 使用 Audio MediaStore、RELATIVE_PATH 与 IS_PENDING：先创建、流式写、关闭流，再解除 pending。失败删除半成品。API26–28 仅按需申请 `WRITE_EXTERNAL_STORAGE`（manifest maxSdkVersion=28），先写 `.part`、改名、登记 DATA，返回的依然是 content URI；没有 READ_MEDIA_AUDIO、所有文件权限或 Kotlin UI。

下载采用系统 HttpURLConnection，32 KiB 分块；仅 HTTP(S)、禁止凭据和 HTTPS 降级、最多五跳、必须 HTTP200、拒绝常见 HTML/XML/JSON 错误页、空文件及 Content-Length 不完整响应。连接15秒/单次读10秒、复制总时限10分钟（检查点生效）；DNS/阻塞底层调用仍非硬截止。Activity closing 在读块/重定向等检查点取消；退出先 `cancelSourceFetch` 置位，再 `cache_stop` join，之后才释放 Activity。未完成下载不跨 Activity 继续，不影响独立播放服务。

既有 `applyPlayback(I[B[B[BJJZ)Z` 签名不变：C 命中索引时 URL 字节数组改为 content URI，服务和 Media3 负责后台读取/解码，不在 JNI 调用线程打开文件。

## 封面与播放页（07-player-ui）

实现：`cover_bridge.[ch]` → `SourceActivity.kt` → `CoverHttp.kt`；C 后台线程管理封面下载调度、磁盘缓存、解码与缩放，Kotlin 仅传输二进制 HTTP 正文。

| 方向 / 接收对象 | 函数名 | JNI 签名 | 线程 | 返回值 / 契约 |
| --- | --- | --- | --- | --- |
| C → SourceActivity | `requestCover` | `([BJ)[B` | 单个封面 C 工作线程，同步 HTTP | url 为 1..2047 字节 ASCII/百分号 HTTP(S) 地址，token 为非零代次；返回非 null ByteArray，1+原始图片字节（正文 1..2 MiB），或 0+可读 UTF-8 错误 |
| SourceActivity → C | `nativeCoverCancelled` | `(J)Z` | 同一 HTTP 工作线程，开始/重定向/读块/完成检查点 | 原子比较代次，true=已切换封面、取消或退出；无锁、无 IO |

C 包装 `cover_bridge_fetch(context,url,token,size)` 返回 malloc 图片字节及长度，失败为 NULL/0；检查 GetEnv/attach、局部帧、类/方法查找、数组分配/写入/调用/长度/读取的异常及返回值，错误状态/UTF-8 文本有界校验并记录 LXMusicC。二进制不使用字符串转换，局部帧释放引用，按需 detach。

HTTP 仅无凭据 HTTP(S)、系统证书校验、最多五次重定向，禁止 HTTPS 降级，仅接受 HTTP 200；连接 2 秒/读 1 秒/总 20 秒检查点时限，正文最多 2 MiB。DNS/底层阻塞仍不承诺硬截止。Activity 退出先置既有 closing，再 cover_stop 取消并 join，最后才释放 Activity；没有主/GL 线程网络或磁盘 IO。

播放服务 JNI 签名不变。C 内部命令新增 `PLAYBACK_SELECT=9`（绝对队列索引）和 `PLAYBACK_MODE=10`（0顺序/1列表循环/2单曲循环/3随机），只在 C 消费、不转发 Kotlin；系统 nativeCommand 仍为原有 2..8，nativeCanSkip 采用当前模式边界。`SourceActivity.onCreate` 仅用框架 `volumeControlStream=STREAM_MUSIC`；C 对音量键返回未处理，由系统调节媒体流，Media3 沿用系统输出路由，不新增音量/路由 JNI 或 UI。
