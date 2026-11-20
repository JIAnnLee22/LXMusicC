#ifndef LXM_PLAYBACK_H
#define LXM_PLAYBACK_H
#include "search.h"
#include <android/looper.h>
#include <android/native_activity.h>
/* 数值也是 JNI 协议；LOAD 仅内部使用，END 为解码器自然结束回调。 */
enum { PLAYBACK_LOAD=1, PLAYBACK_PLAY, PLAYBACK_PAUSE, PLAYBACK_SEEK,
    PLAYBACK_NEXT, PLAYBACK_PREVIOUS, PLAYBACK_END, PLAYBACK_STOP };
typedef struct {
    SearchItem current;
    int count, index, state;
    bool playing, requested, connected;
    int64_t position, duration;
    uint64_t generation;
    unsigned revision;
    char error[256];
} PlaybackView;
/* 进程级队列不借用 Activity；仅 looper 在 UI 生存期间 acquire/release。 */
void playback_ui(ALooper *looper);
bool playback_view(PlaybackView *view);
bool playback_replace(const SearchItem *items,int count,int index);
void playback_command(int action,int64_t position);
bool playback_can_skip(int delta);
void playback_ready(void);
void playback_gone(void);
void playback_failed(void);
void playback_state(uint64_t generation,int state,bool playing,bool requested,
                    int64_t position,int64_t duration,const char *error);
void playback_ensure(ANativeActivity *activity);
#endif
