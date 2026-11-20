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
| SourceActivity → C | `nativePaste` | `([BLjava/lang/String;)V` | Java 主线程 | 成功参数是标准 UTF-8 字节数组；失败 bytes=null、error 为可读错误。C 检查长度、UTF-8 后复制进工作队列，不持有 JNI 引用 |
| C → SourceActivity | `fetchSource` | `(Ljava/lang/String;)[B` | C 音源工作线程，同步等待系统 HTTP(S) | 非 null 字节数组，首字节 1=正文字节、0=错误文本；剩余部分为标准 UTF-8。C 检查异常/null/长度/状态/UTF-8，成功返回 heap 字符串，调用者 free |
| C → SourceActivity | `cancelSourceFetch` | `()V` | NativeActivity 胶水线程退出前 | 置 volatile 取消位，无网络 IO；下载在下一阻塞点结束后检查取消 |

C 包装入口：`bool source_bridge_paste(ANativeActivity*)`、`char *source_bridge_fetch(void*,const char*,char error[256])`、`void source_bridge_cancel(ANativeActivity*)`。下载回调的 context 为当前 ANativeActivity；工作线程退出并 join 后才释放 Activity 相关上下文。取消位也在 Kotlin `onDestroy` 的 `super` 调用前设置；已关闭/正在销毁的 Activity 不执行排队的剪贴板回调。C 停止服务后拒绝入队。

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

**JVM 装载要求**：Kotlin companion 显式 `System.loadLibrary("lxmusicc")`；NativeActivity 自己的 dlopen 不会把库登记到 JVM ClassLoader 的 native 方法查找表，单靠 manifest 的 `android.app.lib_name` 会导致 nativePaste 出现 UnsatisfiedLinkError。AGP 9 使用内置 Kotlin 支持，无新增 Kotlin 插件/第三方运行时 SDK。
