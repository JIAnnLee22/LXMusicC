#ifndef LXM_COVER_BRIDGE_H
#define LXM_COVER_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
/* SourceActivity.requestCover 的 C 包装；context 是 ANativeActivity*。仅封面工作线程调用。
 * 成功返回 malloc 的 JPEG/PNG 原始字节（不追加 0），长度写入 *size，调用者 free；
 * 失败返回 NULL（*size=0），原因写 LXMusicC 日志，由调用方显示占位并可再次请求。 */
unsigned char *cover_bridge_fetch(void *context,const char *url,uint64_t token,size_t *size);
#endif
