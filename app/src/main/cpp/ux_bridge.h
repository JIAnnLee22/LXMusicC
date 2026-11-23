#ifndef LXM_UX_BRIDGE_H
#define LXM_UX_BRIDGE_H
#include <android/native_activity.h>
#include <android/looper.h>
#include <stdbool.h>
typedef struct { float font_scale, back_progress; int notifications; bool back, changed; } UxEvent;
void ux_bridge_start(ALooper *looper);
void ux_bridge_stop(void);
UxEvent ux_bridge_take(void);
/* 0=是否拦截内部返回，1=请求通知授权，2=打开系统通知设置。只投递主线程。 */
bool ux_bridge_request(ANativeActivity *activity,int action,int value);
#endif
