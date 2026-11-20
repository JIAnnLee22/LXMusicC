#ifndef LXM_SOURCE_H
#define LXM_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SOURCE_LIMIT 32
#define SOURCE_BYTES (2u*1024u*1024u)
#define SOURCE_TOTAL (8u*1024u*1024u)
typedef struct {
    char name[256], description[1024], version[128], author[256], homepage[512];
    uint32_t id;
    bool enabled;
} SourceMeta;
typedef struct {
    SourceMeta items[SOURCE_LIMIT];
    int count;
    bool busy;
    unsigned revision;
    char message[256];
} SourceView;
/* 仅解析，不执行。成功时调用者负责 free(*script)。 */
bool source_parse(const char *input,size_t length,SourceMeta *meta,char **script,char error[256]);
bool source_utf8(const char *s,size_t n);
typedef char *(*SourceFetch)(void *context,const char *url,char error[256]);
typedef void (*SourceWake)(void *context);
enum { SOURCE_IMPORT=1, SOURCE_TOGGLE, SOURCE_DELETE, SOURCE_UP, SOURCE_DOWN };
/* 单实例。启动/停止由 NativeActivity 线程调用，IO 与解析在内部工作线程。 */
bool source_start(const char *directory,SourceFetch fetch,SourceWake wake,void *context);
void source_stop(void);
bool source_command(int action,uint32_t id,const char *input,size_t length);
void source_message(const char *message);
bool source_view(SourceView *view);
#endif
