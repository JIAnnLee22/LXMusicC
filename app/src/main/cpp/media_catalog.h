#ifndef LXM_MEDIA_CATALOG_H
#define LXM_MEDIA_CATALOG_H
#include "playback.h"
/* 调用者free；仅序列化系统播放器镜像，队列/模式仍由C持有。 */
char *media_queue_json(const SearchItem *items,const PlaybackView *view,unsigned serial);
#endif
