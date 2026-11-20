#ifndef LXM_COVER_H
#define LXM_COVER_H
#include "source.h"
/* 含结尾 0 的地址缓冲上限；有效 URL 最多 COVER_URL_BYTES-1 字节。 */
#define COVER_URL_BYTES 2048
/* 解码缩放后的输出：最长边 ≤512 的 RGBA8，≤1 MiB。pixels 所有权随 take 转交。 */
typedef struct { unsigned char *pixels; int width,height; uint64_t token; } CoverImage;
/* 单实例。directory 为应用私有目录；context 交给 cover_bridge_fetch（ANativeActivity）。 */
bool cover_start(const char *directory,SourceWake wake,void *context);
void cover_stop(void);
/* 最新请求覆盖尚未处理的旧请求；空/NULL/超长 URL 视为取消并发布空封面。 */
void cover_request(const char *url);
/* GL 线程取走当前待发布图片；成功时调用者负责 free(out->pixels)。空图 width=height=0。 */
bool cover_take(CoverImage *out);
/* 代次比较，无锁无 IO；供 Kotlin 下载线程的取消回调使用。 */
bool cover_cancelled(uint64_t token);
#endif
