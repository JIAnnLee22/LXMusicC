#ifndef LXM_PLAYBACK_BRIDGE_H
#define LXM_PLAYBACK_BRIDGE_H
#include "playback.h"
/* 仅投递主线程；不得同步回调 playback_*，由调用者持有播放锁。 */
bool playback_bridge_apply(int action,const PlaybackView *view,const SearchItem *queue,unsigned serial);
bool playback_bridge_start(ANativeActivity *activity);
#endif
