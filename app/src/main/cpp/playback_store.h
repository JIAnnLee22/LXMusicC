#ifndef LXM_PLAYBACK_STORE_H
#define LXM_PLAYBACK_STORE_H
#include "playback.h"
typedef struct {
    int count,index,mode;
    int64_t position;
    unsigned serial;
    SearchItem items[SEARCH_RESULT_LIMIT];
} PlaybackSnapshot;
bool playback_snapshot(PlaybackSnapshot *snapshot);
void playback_restore(const PlaybackSnapshot *snapshot);
bool playback_store_start(const char *directory);
void playback_store_wait(void); /* 仅媒体后台worker可等待首次读取 */
void playback_store_dirty(void); /* 无IO，合并信号 */
#endif
