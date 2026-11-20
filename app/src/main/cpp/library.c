#include "library.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdatomic.h>

#define FAVORITES 512
#define HISTORY 200
#define COMMANDS 64
typedef struct { uint32_t id,count; char name[256]; uint32_t songs[LIBRARY_LIST_SONGS]; } List;
typedef struct {
    uint32_t tracks,next_track,lists,next_list,favorites,history;
    uint32_t favorite[FAVORITES],recent[HISTORY];
    List list[LIBRARY_LIST_LIMIT];
    LibraryTrack track[LIBRARY_TRACK_LIMIT];
} Store;
_Static_assert(sizeof(Store)==5579832,"library v1 ABI changed: add an explicit migration");
typedef struct { int action; uint32_t list,track; SearchItem item; char name[256]; } Command;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    atomic_bool started;
    bool healthy,refresh;
    int head,count,list,page;
    unsigned request;
    char path[1100],query[256];
    Command queue[COMMANDS];
    Store *store;
    LibraryView view;
    ALooper *looper;
} lib={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};

static void wake(void) { if(lib.looper) ALooper_wake(lib.looper); }
static bool field(const char *s,size_t cap) {
    size_t n=strnlen(s,cap); return n<cap && (!n || source_utf8(s,n));
}
static bool valid(const SearchItem *i) {
    const SourceSong *s=&i->song;
    return field(i->source_name,sizeof(i->source_name)) && field(s->id,sizeof(s->id)) &&
        field(s->name,sizeof(s->name)) && s->name[0] && field(s->artist,sizeof(s->artist)) &&
        field(s->album,sizeof(s->album)) && field(s->quality,sizeof(s->quality)) &&
        field(s->url,sizeof(s->url)) && (!strncmp(s->url,"https://",8) || !strncmp(s->url,"http://",7)) &&
        field(s->cover,sizeof(s->cover)) && isfinite(s->duration) && s->duration>=0 && s->duration<=86400;
}
static bool same(const SearchItem *a,const SearchItem *b) {
    if(a->source_id!=b->source_id || strcmp(a->source_name,b->source_name)) return false;
    if(a->song.id[0] && b->song.id[0]) return !strcmp(a->song.id,b->song.id);
    return !strcmp(a->song.name,b->song.name) && !strcmp(a->song.artist,b->song.artist) &&
        !strcmp(a->song.album,b->song.album) && fabs(a->song.duration-b->song.duration)<2;
}
static LibraryTrack *track(uint32_t id) {
    for(uint32_t i=0;i<lib.store->tracks;++i) if(lib.store->track[i].id==id) return &lib.store->track[i];
    return NULL;
}
static List *list(uint32_t id) {
    for(uint32_t i=0;i<lib.store->lists;++i) if(lib.store->list[i].id==id) return &lib.store->list[i];
    return NULL;
}
static int index_of(const uint32_t *ids,uint32_t count,uint32_t id) {
    for(uint32_t i=0;i<count;++i) if(ids[i]==id) return (int)i;
    return -1;
}
static void remove_id(uint32_t *ids,uint32_t *count,int at) {
    if(at<0) return;
    memmove(ids+at,ids+at+1,(*count-(uint32_t)at-1)*sizeof(*ids)); --*count;
}
static void prune(void) {
    Store *s=lib.store;
    for(uint32_t i=0;i<s->tracks;) {
        uint32_t id=s->track[i].id;
        bool used=index_of(s->favorite,s->favorites,id)>=0 || index_of(s->recent,s->history,id)>=0;
        for(uint32_t j=0;!used && j<s->lists;++j) used=index_of(s->list[j].songs,s->list[j].count,id)>=0;
        if(used) ++i;
        else { memmove(s->track+i,s->track+i+1,(s->tracks-i-1)*sizeof(*s->track)); --s->tracks; }
    }
}
static bool refs(const uint32_t *ids,uint32_t n) {
    for(uint32_t i=0;i<n;++i) if(!track(ids[i]) || index_of(ids,i,ids[i])>=0) return false;
    return true;
}
/* 固定两个64位Android ABI，记录长度和版本都校验；未来结构变化须显式迁移。 */
static bool load(char message[256]) {
    Store *s=lib.store; s->next_track=1; s->next_list=2;
    FILE *f=fopen(lib.path,"rb");
    if(!f) {
        if(errno==ENOENT) return true;
        snprintf(message,256,"无法读取音乐库，已停止写入"); return false;
    }
    char magic[8]; uint32_t size;
    bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"LXLB\1\0\0\0",8) &&
        fread(&size,4,1,f)==1 && size==sizeof(Store) && fread(s,sizeof(*s),1,f)==1;
    ok=ok && s->tracks<=LIBRARY_TRACK_LIMIT && s->lists<=LIBRARY_LIST_LIMIT &&
        s->favorites<=FAVORITES && s->history<=HISTORY && s->next_track>0 && s->next_list>=2;
    for(uint32_t i=0;ok && i<s->tracks;++i) {
        LibraryTrack *t=&s->track[i]; ok=t->id>0 && t->id<s->next_track && valid(&t->item);
        for(uint32_t j=0;ok && j<i;++j) ok=t->id!=s->track[j].id && !same(&t->item,&s->track[j].item);
    }
    if(ok) ok=refs(s->favorite,s->favorites) && refs(s->recent,s->history);
    for(uint32_t i=0;ok && i<s->lists;++i) {
        List *p=&s->list[i]; ok=p->id>=2 && p->id<s->next_list && p->count<=LIBRARY_LIST_SONGS &&
            field(p->name,sizeof(p->name)) && p->name[0] && refs(p->songs,p->count);
        for(uint32_t j=0;ok && j<i;++j) ok=p->id!=s->list[j].id && strcasecmp(p->name,s->list[j].name);
    }
    if(ok) ok=fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if(ok) return true;
    memset(s,0,sizeof(*s)); s->next_track=1; s->next_list=2;
    snprintf(message,256,"音乐库损坏，已停止写入以保护原文件"); return false;
}
static bool save(void) {
    char tmp[1150]; snprintf(tmp,sizeof(tmp),"%s.tmp",lib.path);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600); FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
    if(!f) { if(fd>=0) close(fd); return false; }
    uint32_t size=sizeof(Store);
    bool ok=fwrite("LXLB\1\0\0\0",1,8,f)==8 && fwrite(&size,4,1,f)==1 && fwrite(lib.store,size,1,f)==1;
    if(ok) ok=fflush(f)==0 && fsync(fd)==0;
    if(fclose(f)) ok=false;
    if(ok && !rename(tmp,lib.path)) return true;
    unlink(tmp); return false;
}
/* ASCII忽略大小写；UTF-8按字节子串匹配，不做Unicode折叠。 */
static bool contains(const char *text,const char *query) {
    size_t n=strlen(query);
    for(;*text;++text) if(!strncasecmp(text,query,n)) return true;
    return !n;
}
static void publish(const char *message) {
    pthread_mutex_lock(&lib.lock);
    LibraryView *v=&lib.view; Store *s=lib.store;
    v->request=lib.request;
    v->list=lib.list; v->page=lib.page; v->count=v->total=0;
    v->healthy=lib.healthy; v->busy=lib.count>0;
    snprintf(v->query,256,"%s",lib.query);
    if(message) snprintf(v->message,256,"%s",message);
    uint32_t *ids=NULL,n=0;
    const char *title=lib.list==-1?"我的歌单":lib.list==0?"收藏":"最近播放";
    if(lib.list==0) { ids=s->favorite; n=s->favorites; }
    else if(lib.list==1) { ids=s->recent; n=s->history; }
    else if(lib.list>=2) {
        List *p=list((uint32_t)lib.list);
        if(p) { title=p->name; ids=p->songs; n=p->count; }
    }
    snprintf(v->title,256,"%s",title);
    /* 只在后台扫描；两遍计数/拷贝，UI始终最多六条。 */
    for(int pass=0;pass<2;++pass) {
        int matched=0;
        uint32_t limit=lib.list==-1?s->lists:n;
        for(uint32_t i=0;i<limit;++i) {
            LibraryTrack *t=ids?track(ids[i]):NULL;
            bool match=lib.list==-1?contains(s->list[i].name,lib.query):t &&
                (contains(t->item.song.name,lib.query) || contains(t->item.song.artist,lib.query) || contains(t->item.song.album,lib.query));
            if(!match) continue;
            if(pass && matched>=v->page*LIBRARY_PAGE_SIZE && v->count<LIBRARY_PAGE_SIZE) {
                if(lib.list==-1) {
                    List *p=&s->list[i]; LibraryList *out=&v->lists[v->count];
                    out->id=p->id; out->count=(int)p->count; snprintf(out->name,256,"%s",p->name);
                } else v->songs[v->count]=*t;
                ++v->count;
            }
            ++matched;
        }
        if(!pass) {
            v->total=matched; int last=matched?(matched-1)/LIBRARY_PAGE_SIZE:0;
            if(v->page>last) v->page=last;
            if(v->page<0) v->page=0;
            lib.page=v->page;
        }
    }
    ++v->revision; wake(); pthread_mutex_unlock(&lib.lock);
}
static bool name_copy(char out[256],const char *input) {
    if(!input || !field(input,256)) return false;
    while(*input && isspace((unsigned char)*input)) ++input;
    snprintf(out,256,"%s",input); size_t n=strlen(out);
    while(n && isspace((unsigned char)out[n-1])) out[--n]=0;
    if(!n) return false;
    for(size_t i=0;i<n;++i) if((unsigned char)out[i]<32 || out[i]==127) return false;
    return true;
}
static const char *change(const Command *c) {
    Store *s=lib.store; List *p=list(c->list);
    if(c->action==LIB_CREATE || c->action==LIB_RENAME) {
        char name[256];
        if(!name_copy(name,c->name)) return "歌单名称不能为空或含控制字符";
        for(uint32_t i=0;i<s->lists;++i) if((!p || p->id!=s->list[i].id) && !strcasecmp(name,s->list[i].name)) return "已有同名歌单";
        if(c->action==LIB_CREATE) {
            if(s->lists>=LIBRARY_LIST_LIMIT || s->next_list>=INT32_MAX) return "歌单已满（最多32张）";
            p=&s->list[s->lists++]; memset(p,0,sizeof(*p)); p->id=s->next_list++;
        } else if(!p) return "歌单不存在，请返回列表";
        snprintf(p->name,256,"%s",name); return NULL;
    }
    if(c->action==LIB_DELETE) {
        if(!p) return "歌单不存在";
        size_t i=(size_t)(p-s->list); memmove(p,p+1,(s->lists-i-1)*sizeof(*p)); --s->lists;
        return NULL;
    }
    if(c->action==LIB_REMOVE) {
        uint32_t *ids=c->list==0?s->favorite:c->list==1?s->recent:p?p->songs:NULL;
        uint32_t *n=c->list==0?&s->favorites:c->list==1?&s->history:p?&p->count:NULL;
        if(!ids) return "歌单不存在";
        remove_id(ids,n,index_of(ids,*n,c->track)); return NULL;
    }
    if(!valid(&c->item)) return "歌曲信息不合法";
    if(c->action==LIB_ADD && !p) return "歌单不存在";
    uint32_t id=0;
    for(uint32_t i=0;i<s->tracks;++i) if(same(&s->track[i].item,&c->item)) { id=s->track[i].id; break; }
    if(c->action==LIB_FAVORITE && id && index_of(s->favorite,s->favorites,id)>=0) {
        remove_id(s->favorite,&s->favorites,index_of(s->favorite,s->favorites,id)); return NULL;
    }
    if(c->action==LIB_FAVORITE && s->favorites>=FAVORITES) return "收藏已满（最多512首）";
    if(c->action==LIB_ADD && index_of(p->songs,p->count,id)>=0) return "歌曲已在歌单中，不会重复添加";
    if(c->action==LIB_ADD && p->count>=LIBRARY_LIST_SONGS) return "歌单已满（最多240首）";
    if(!id) {
        if(s->tracks>=LIBRARY_TRACK_LIMIT || s->next_track==UINT32_MAX) return "音乐库已满，请先移除歌曲";
        LibraryTrack *t=&s->track[s->tracks++]; id=t->id=s->next_track++; t->item=c->item;
    } else track(id)->item=c->item; // 更新易过期的URL及封面，不改变身份/其它歌单。
    if(c->action==LIB_FAVORITE) s->favorite[s->favorites++]=id;
    else if(c->action==LIB_ADD) p->songs[p->count++]=id;
    else if(c->action==LIB_HISTORY) {
        remove_id(s->recent,&s->history,index_of(s->recent,s->history,id));
        if(s->history==HISTORY) --s->history;
        memmove(s->recent+1,s->recent,s->history*sizeof(*s->recent)); s->recent[0]=id; ++s->history;
    }
    return NULL;
}
static void *worker(void *unused) {
    char message[256]="音乐库已就绪";
    lib.healthy=load(message); publish(message);
    for(;;) {
        pthread_mutex_lock(&lib.lock);
        while(!lib.count && !lib.refresh) pthread_cond_wait(&lib.cond,&lib.lock);
        bool command=lib.count>0; Command c={0};
        if(command) { c=lib.queue[lib.head]; lib.head=(lib.head+1)%COMMANDS; --lib.count; }
        lib.refresh=false;
        pthread_mutex_unlock(&lib.lock);
        const char *result=NULL;
        if(command) {
            if(!lib.healthy) result="音乐库不可写，请先备份检查原文件";
            else {
                Store *before=malloc(sizeof(*before));
                if(!before) result="内存不足，请重试";
                else {
                    memcpy(before,lib.store,sizeof(*before)); result=change(&c);
                    if(!result) { prune(); if(!save()) result="音乐库保存失败，修改已撤回，请重试"; }
                    if(result) memcpy(lib.store,before,sizeof(*before));
                    free(before);
                }
            }
            if(!result) result=c.action==LIB_HISTORY?"已更新最近播放":"音乐库修改已保存";
        }
        publish(result);
    }
    return NULL;
}
void library_ui(ALooper *looper) {
    pthread_mutex_lock(&lib.lock);
    if(lib.looper) ALooper_release(lib.looper);
    lib.looper=looper; if(looper) ALooper_acquire(looper);
    pthread_mutex_unlock(&lib.lock);
}
bool library_start(const char *directory,ALooper *looper) {
    if(!lib.started) {
        if(!directory || strlen(directory)+32>=sizeof(lib.path)) return false;
        lib.store=calloc(1,sizeof(Store)); if(!lib.store) return false;
        snprintf(lib.path,sizeof(lib.path),"%s/lx-library-v1.bin",directory);
        lib.view.busy=true; lib.view.revision=1;
        pthread_t thread;
        if(pthread_create(&thread,NULL,worker,NULL)) {
            free(lib.store); lib.store=NULL; lib.view.busy=false;
            snprintf(lib.view.message,256,"音乐库线程启动失败，请退出后重试");
            ++lib.view.revision; return false;
        }
        pthread_detach(thread); lib.started=true;
    }
    library_ui(looper); return true;
}
unsigned library_browse(int id,int page,const char *query) {
    pthread_mutex_lock(&lib.lock);
    lib.list=id; lib.page=page<0?0:page;
    snprintf(lib.query,sizeof(lib.query),"%s",query?query:"");
    unsigned request=++lib.request;
    lib.refresh=true; pthread_cond_signal(&lib.cond);
    pthread_mutex_unlock(&lib.lock); return request;
}
bool library_view(LibraryView *view) {
    pthread_mutex_lock(&lib.lock);
    bool changed=view->revision!=lib.view.revision;
    if(changed) *view=lib.view;
    pthread_mutex_unlock(&lib.lock); return changed;
}
bool library_command(int action,uint32_t id,uint32_t song,const SearchItem *item,const char *name) {
    if(!lib.started || action<LIB_FAVORITE || action>LIB_REMOVE) return false;
    pthread_mutex_lock(&lib.lock);
    bool ok=lib.count<COMMANDS;
    if(ok) {
        Command *c=&lib.queue[(lib.head+lib.count)%COMMANDS]; memset(c,0,sizeof(*c));
        c->action=action; c->list=id; c->track=song;
        if(item) c->item=*item;
        if(name) snprintf(c->name,sizeof(c->name),"%s",name);
        ++lib.count; lib.view.busy=true; ++lib.view.revision; wake(); pthread_cond_signal(&lib.cond);
    }
    pthread_mutex_unlock(&lib.lock); return ok;
}
bool library_record(const SearchItem *item) { return library_command(LIB_HISTORY,1,0,item,NULL); }
void library_paste(const char *text,size_t n,const char *error) {
    pthread_mutex_lock(&lib.lock);
    if(!error && text && n>0 && n<256 && source_utf8(text,n)) {
        memcpy(lib.view.pasted,text,n); lib.view.pasted[n]=0; ++lib.view.paste_revision;
        snprintf(lib.view.message,256,"已粘贴文字");
    } else snprintf(lib.view.message,256,"%s",error?error:"文字为空、过长或不是UTF-8");
    ++lib.view.revision; wake(); pthread_mutex_unlock(&lib.lock);
}
