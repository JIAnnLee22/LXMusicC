# JNI 桥接契约

当前没有 Kotlin/Java 源码或自定义 native 方法。C 通过 Android 框架 JNI 只读查询安全区，以兼容 targetSdk 36 的 edge-to-edge；UI 和业务逻辑仍全部在 C。

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

其余系统操作仅使用 NDK API：`ANativeActivity_finish` 关闭 Activity；`APP_CMD_SAVE_STATE` 由 native_app_glue 管理临时界面状态，无新增 JNI 接口。
