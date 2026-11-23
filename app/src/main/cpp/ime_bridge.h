#ifndef LXM_IME_BRIDGE_H
#define LXM_IME_BRIDGE_H
#include <android/native_activity.h>
#include <android/looper.h>
#include <stdbool.h>
#define IME_BYTES 2048
typedef struct {
    int session, start, end, composing_start, composing_end;
    bool changed, submit, hidden, failed;
    char text[IME_BYTES], error[256];
} ImeEvent;
/* 邮箱仅传值；UI/GL 与 Android 主线程不互相借用指针。 */
void ime_bridge_start(ALooper *looper);
void ime_bridge_stop(void);
bool ime_bridge_take(ImeEvent *event,int *bottom,int *reference_height);
bool ime_bridge_request(ANativeActivity *activity,int action,int session,const char *text,int start,int end,int kind);
#endif
