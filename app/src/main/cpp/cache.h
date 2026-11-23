#ifndef LXM_CACHE_H
#define LXM_CACHE_H
#include "search.h"
#define CACHE_PAGE_SIZE 6
#define CACHE_LIMIT 512
#define CACHE_URI_BYTES 512
#define CACHE_MIB (1024ull*1024ull)
enum { CACHE_DOWNLOAD=1, CACHE_DELETE, CACHE_CLEAR, CACHE_SET_LIMIT, CACHE_REFRESH };
typedef struct {
    SearchItem item;
    char uri[CACHE_URI_BYTES];
    uint64_t bytes;
    uint32_t id;
} CacheEntry;
typedef struct {
    CacheEntry items[CACHE_PAGE_SIZE];
    int count,total,page;
    uint64_t bytes,limit;
    bool busy;
    unsigned revision;
    char message[256];
} CacheView;
/* 所有系统存储/HTTP调用仅在缓存工作线程，op=1下载/2探测/3删除/4清遗留pending。 */
typedef bool (*CacheIo)(void *context,int op,const SearchItem *item,const char *uri,
                        uint64_t maximum,char result[CACHE_URI_BYTES],uint64_t *bytes,char error[256]);
bool cache_start(const char *directory,CacheIo io,SourceWake wake,void *context);
void cache_stop(void);
bool cache_command(int action,const SearchItem *item,uint32_t id,uint64_t limit);
void cache_page(int delta);
bool cache_view(CacheView *view);
/* 纯内存查找，播放锁可调用，不做IO或回调播放。 */
void cache_media_load(const char *directory);
bool cache_lookup(const SearchItem *item,char uri[CACHE_URI_BYTES]);
#endif
