#ifndef LXM_PLAYBACK_H
#define LXM_PLAYBACK_H
#include "search.h"
#include <android/looper.h>
#include <android/native_activity.h>
/* 数值也是 JNI 协议；LOAD 仅内部使用，END 为解码器自然结束回调。
 * SELECT/MODE 只由 C 侧消费（position 分别是队列绝对索引、播放模式），不转发给服务。 */
enum { PLAYBACK_LOAD=1, PLAYBACK_PLAY, PLAYBACK_PAUSE, PLAYBACK_SEEK,
    PLAYBACK_NEXT, PLAYBACK_PREVIOUS, PLAYBACK_END, PLAYBACK_STOP,
    PLAYBACK_SELECT, PLAYBACK_MODE };
/* PLAYBACK_MODE 的 position 取值：0 顺序、1 列表循环、2 单曲循环、3 随机。 */
enum { PLAYBACK_MODE_SEQUENCE=0, PLAYBACK_MODE_LIST, PLAYBACK_MODE_SINGLE,
    PLAYBACK_MODE_SHUFFLE };
/* 播放队列快照分页大小；播放页每帧可重新拉取当前页。 */
#define PLAYBACK_QUEUE_PAGE 6
typedef struct {
    SearchItem current;
    int count, index, state, mode;
    bool playing, requested, connected;
    int64_t position, duration;
    uint64_t generation;
    unsigned revision;
    char error[256];
    char local_uri[512];
} PlaybackView;
/* 队列分页快照：index 为当前播放曲目的绝对索引，page 为夹取后的页码。 */
typedef struct {
    SearchItem items[PLAYBACK_QUEUE_PAGE];
    int count, total, page, index;
    unsigned revision;
} PlaybackQueue;
/* 进程级队列不借用 Activity；仅 looper 在 UI 生存期间 acquire/release。 */
void playback_ui(ALooper *looper);
bool playback_view(PlaybackView *view);
bool playback_replace(const SearchItem *items,int count,int index);
bool playback_replace_at(const SearchItem *items,int count,int index,int64_t position,bool autoplay);
void playback_seek_index(unsigned serial,int index,int64_t position);
void playback_open_player(void);
bool playback_take_open(void);
void playback_command(int action,int64_t position);
bool playback_can_skip(int delta);
void playback_queue(PlaybackQueue *queue,int page);
void playback_ready(void);
void playback_gone(void);
void playback_failed(void);
void playback_state(uint64_t generation,int state,bool playing,bool requested,
                    int64_t position,int64_t duration,const char *error);
void playback_ensure(ANativeActivity *activity);
#endif
