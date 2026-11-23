#ifndef LXM_LIBRARY_H
#define LXM_LIBRARY_H
#include "search.h"
#include <android/looper.h>
#define LIBRARY_PAGE_SIZE 6
#define LIBRARY_TRACK_LIMIT 1024
#define LIBRARY_LIST_LIMIT 32
#define LIBRARY_LIST_SONGS 240
/* list=0 收藏，1 最近播放，>=2 自建歌单；-1 为歌单目录。 */
enum { LIB_FAVORITE=1, LIB_HISTORY, LIB_CREATE, LIB_RENAME, LIB_DELETE, LIB_ADD, LIB_REMOVE };
typedef struct { uint32_t id; SearchItem item; } LibraryTrack;
typedef struct { uint32_t id; char name[256]; int count; } LibraryList;
typedef struct {
    unsigned revision,paste_revision,request;
    bool busy,healthy;
    int list,page,total,count;
    LibraryTrack songs[LIBRARY_PAGE_SIZE];
    LibraryList lists[LIBRARY_PAGE_SIZE];
    char query[256],title[256],message[256],pasted[256];
} LibraryView;
/* 进程级 worker 不持有 Activity；退出界面只解除 looper，后台播放仍能记录历史。 */
bool library_start(const char *directory,ALooper *looper);
void library_ui(ALooper *looper);
unsigned library_browse(int list,int page,const char *query);
bool library_view(LibraryView *view);
bool library_command(int action,uint32_t list,uint32_t track,const SearchItem *item,const char *name);
bool library_record(const SearchItem *item);
bool library_valid_item(const SearchItem *item);
/* 后台媒体调用；返回总条数或-1，输出数组容量为limit（0..240）。 */
int library_media(int list,LibraryTrack *tracks,LibraryList *lists,int offset,int limit);
unsigned library_revision(void);
void library_paste(const char *text,size_t length,const char *error);
#endif
