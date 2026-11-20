#ifndef LXM_SOURCE_RUNTIME_H
#define LXM_SOURCE_RUNTIME_H
#include <stdbool.h>
#include <stddef.h>
#define RUNTIME_SONG_LIMIT 30
#define RUNTIME_HTTP_BYTES (2u*1024u*1024u)
typedef struct {
    char id[128], name[256], artist[256], album[256], quality[128], url[2048];
    double duration; /* 秒；预览源可返回完整歌曲时长，但 URL 只是预览。 */
} SourceSong;
typedef struct {
    SourceSong songs[RUNTIME_SONG_LIMIT];
    int count;
    bool has_more;
    char *json; /* handler 原始 JSON 返回值；由 runtime_result_free 释放。 */
    char *info_json; /* 搜索首曲对应的 {type,musicInfo}，供直链/歌词请求使用。 */
} RuntimeResult;
/* request/response 都为标准 UTF-8 JSON。返回 heap 字符串，运行时负责 free。
 * 网络错误返回 NULL 并填 error；HTTP 非 2xx 仍返回 statusCode/body，交给脚本处理。
 */
typedef char *(*RuntimeHttp)(void *context,const char *request,char error[256]);
typedef bool (*RuntimeCancelled)(void *context);
/* 仅后台线程调用；每次新建隔离 JS 上下文，不开放文件、进程、模块或 JNI。
 * provider=NULL 时从 inited.sources 中选择支持 action 的首项。
 * search 规范输出见 docs/ARCHITECTURE.md；失败 result 为空，可直接重试。
 */
bool runtime_call(const char *script,const char *provider,const char *action,const char *info_json,
                  RuntimeHttp http,RuntimeCancelled cancelled,void *context,
                  RuntimeResult *result,char error[256]);
void runtime_result_free(RuntimeResult *result);
#endif
