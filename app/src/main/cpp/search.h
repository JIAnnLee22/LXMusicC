#ifndef LXM_SEARCH_H
#define LXM_SEARCH_H
#include "source.h"
#define SEARCH_QUERY_BYTES 256
#define SEARCH_PAGE_SIZE 8
#define SEARCH_RESULT_LIMIT 240
/* 自包含值类型，翻页/新查询/音源删除后仍可安全保留当前选择。 */
typedef struct {
    SourceSong song;
    char source_name[256];
    uint32_t source_id;
    int source_order, song_order, score;
} SearchItem;
typedef struct {
    SearchItem items[SEARCH_PAGE_SIZE];
    int count, total, page, sources, completed, failed;
    bool busy, cancelled;
    unsigned revision, paste_revision;
    char keyword[SEARCH_QUERY_BYTES], pasted[SEARCH_QUERY_BYTES];
    char message[512], error[256];
} SearchView;
typedef char *(*SearchHttp)(void *context,const char *request,uint64_t token,char error[256]);
bool search_start(SearchHttp http,SourceWake wake,void *context);
void search_stop(void);
bool search_query(const char *keyword);
void search_cancel(void);
void search_page(int delta);
bool search_view(SearchView *view);
int search_media(SearchItem *items,int offset,int limit,unsigned *revision);
/* Java 主线程只投递粘贴结果，GL 线程通过快照消费。 */
void search_paste(const char *text,size_t length,const char *error);
bool search_is_cancelled(uint64_t token);
#endif
