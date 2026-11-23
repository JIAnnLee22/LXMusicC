#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stddef.h>

/* 06 的 v1 ABI 原样定义，升级封面字段时逐条转换，绝不丢弃已有公共音频。 */
typedef struct {
    char id[128],name[256],artist[256],album[256],quality[128],url[2048];
    double duration;
} LegacySong;
typedef struct {
    LegacySong song;
    char source_name[256];
    uint32_t source_id;
    int source_order,song_order,score;
} LegacyItem;
typedef struct {
    LegacyItem item;
    char uri[512];
    uint64_t bytes;
    uint32_t id;
} LegacyEntry;
_Static_assert(sizeof(LegacyEntry)==3880,"v1 cache ABI changed");
_Static_assert(sizeof(LegacySong)==offsetof(SourceSong,cover),"song prefix changed");

/* 固定64位Android ABI上的版本化记录；头和记录长度均校验，损坏时不覆盖。 */
typedef struct {
    uint32_t count,next_id;
    uint64_t limit;
    CacheEntry entries[CACHE_LIMIT];
} Store;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool running,started,pending,healthy;
    int action,page;
    uint32_t id;
    uint64_t requested_limit;
    SearchItem input;
    Store *store;
    CacheEntry *lookup;
    int lookup_count;
    char path[1100];
    CacheView view;
    CacheIo io;
    SourceWake wake;
    void *context;
} cache={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};

static bool same(const SearchItem *a,const SearchItem *b) {
    if(a->source_id!=b->source_id || strcmp(a->source_name,b->source_name) || strcmp(a->song.quality,b->song.quality)) return false;
    if(a->song.id[0] && b->song.id[0]) return !strcmp(a->song.id,b->song.id);
    return !strcmp(a->song.name,b->song.name) && !strcmp(a->song.artist,b->song.artist) &&
        !strcmp(a->song.album,b->song.album) && fabs(a->song.duration-b->song.duration)<2;
}
static uint64_t usage(const Store *s) {
    uint64_t n=0; for(uint32_t i=0;i<s->count;++i) n+=s->entries[i].bytes; return n;
}
static void page_locked(void) {
    CacheView *v=&cache.view;
    cache.page=cache.page<0?0:cache.page;
    int last=cache.lookup_count?(cache.lookup_count-1)/CACHE_PAGE_SIZE:0;
    if(cache.page>last) cache.page=last;
    v->page=cache.page; v->count=0;
    for(int i=cache.page*CACHE_PAGE_SIZE;i<cache.lookup_count && v->count<CACHE_PAGE_SIZE;++i)
        v->items[v->count++]=cache.lookup[i];
    ++v->revision;
    if(cache.wake) cache.wake(cache.context);
}
static void publish(const char *message) {
    pthread_mutex_lock(&cache.lock);
    cache.lookup_count=cache.store->count;
    memcpy(cache.lookup,cache.store->entries,cache.store->count*sizeof(CacheEntry));
    cache.view.total=cache.store->count; cache.view.bytes=usage(cache.store); cache.view.limit=cache.store->limit;
    cache.view.busy=false;
    snprintf(cache.view.message,256,"%s",message);
    page_locked();
    pthread_mutex_unlock(&cache.lock);
}
static bool save(char error[256]) {
    char temp[1150]; snprintf(temp,sizeof(temp),"%s.tmp",cache.path);
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);
    FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
    if(!f) { if(fd>=0) close(fd); goto fail; }
    uint32_t size=sizeof(CacheEntry); Store *s=cache.store;
    bool ok=fwrite("LXCC\2\0\0\0",1,8,f)==8 && fwrite(&size,4,1,f)==1 &&
        fwrite(&s->count,4,1,f)==1 && fwrite(&s->next_id,4,1,f)==1 && fwrite(&s->limit,8,1,f)==1 &&
        fwrite(s->entries,sizeof(CacheEntry),s->count,f)==s->count;
    if(ok) ok=fflush(f)==0 && fsync(fd)==0;
    if(fclose(f)!=0) ok=false;
    if(ok && rename(temp,cache.path)==0) return true;
    unlink(temp);
fail:
    snprintf(error,256,"缓存索引保存失败，请检查可用空间后重试"); return false;
}
static bool field(const char *s,size_t capacity) {
    size_t n=strnlen(s,capacity); return n<capacity && (!n || source_utf8(s,n));
}
static bool load(Store *s,const char *path,char error[256]) {
    s->limit=1024*CACHE_MIB; s->next_id=1;
    FILE *f=fopen(path,"rb");
    if(!f) {
        if(errno==ENOENT) return true;
        snprintf(error,256,"无法读取缓存索引，已停止写入"); return false;
    }
    char magic[8]; uint32_t size;
    bool ok=fread(magic,1,8,f)==8;
    bool legacy=ok && !memcmp(magic,"LXCC\1\0\0\0",8);
    ok=ok && (legacy || !memcmp(magic,"LXCC\2\0\0\0",8)) && fread(&size,4,1,f)==1 &&
        size==(legacy?sizeof(LegacyEntry):sizeof(CacheEntry)) && fread(&s->count,4,1,f)==1 && s->count<=CACHE_LIMIT &&
        fread(&s->next_id,4,1,f)==1 && s->next_id>0 && fread(&s->limit,8,1,f)==1 &&
        s->limit>=64*CACHE_MIB && s->limit<=16*1024*CACHE_MIB;
    for(uint32_t i=0;ok && i<s->count;++i) {
        CacheEntry *e=&s->entries[i];
        if(!legacy) { ok=fread(e,sizeof(*e),1,f)==1; continue; }
        LegacyEntry old;
        ok=fread(&old,sizeof(old),1,f)==1;
        if(!ok) break;
        memcpy(&e->item.song,&old.item.song,sizeof(old.item.song));
        memcpy(e->item.source_name,old.item.source_name,sizeof(old.item.source_name));
        e->item.source_id=old.item.source_id; e->item.source_order=old.item.source_order;
        e->item.song_order=old.item.song_order; e->item.score=old.item.score;
        memcpy(e->uri,old.uri,sizeof(old.uri)); e->bytes=old.bytes; e->id=old.id;
    }
    for(uint32_t i=0;ok && i<s->count;++i) {
        CacheEntry *e=&s->entries[i]; SourceSong *song=&e->item.song;
        ok=e->id>0 && e->id<s->next_id && e->bytes>0 && e->bytes<=2*1024*CACHE_MIB &&
            field(e->uri,sizeof(e->uri)) && !strncmp(e->uri,"content://media/",16) &&
            field(e->item.source_name,sizeof(e->item.source_name)) && field(song->id,sizeof(song->id)) &&
            field(song->name,sizeof(song->name)) && field(song->artist,sizeof(song->artist)) &&
            field(song->album,sizeof(song->album)) && field(song->quality,sizeof(song->quality)) &&
            field(song->url,sizeof(song->url)) && field(song->cover,sizeof(song->cover)) && isfinite(song->duration) && song->duration>=0;
        for(uint32_t j=0;ok && j<i;++j) if(e->id==s->entries[j].id || same(&e->item,&s->entries[j].item)) ok=false;
    }
    if(ok) ok=fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if(ok) return true;
    memset(s,0,sizeof(*s)); s->limit=1024*CACHE_MIB; s->next_id=1;
    snprintf(error,256,"缓存索引损坏，已停止写入以保护公共文件"); return false;
}
static void remove_at(uint32_t i) {
    Store *s=cache.store;
    memmove(s->entries+i,s->entries+i+1,(s->count-i-1)*sizeof(CacheEntry)); --s->count;
}
static bool io(int op,const SearchItem *item,const char *uri,uint64_t max,char *result,uint64_t *size,char error[256]) {
    return cache.io && cache.io(cache.context,op,item,uri,max,result,size,error);
}
static void refresh(char error[256]) {
    bool changed=false; Store *s=cache.store;
    for(uint32_t i=0;i<s->count;) {
        char reason[256]=""; uint64_t bytes=0;
        bool exists=io(2,NULL,s->entries[i].uri,0,NULL,&bytes,reason);
        if(!exists && !strcmp(reason,"缓存文件不存在")) {
            remove_at(i); changed=true;
        } else {
            if(exists && bytes!=s->entries[i].bytes && bytes<=2*1024*CACHE_MIB) {
                s->entries[i].bytes=bytes; changed=true;
            }
            if(reason[0] && !error[0]) snprintf(error,256,"%s",reason);
            ++i;
        }
    }
    if(changed && !save(error)) cache.healthy=false;
}
static void change(int action,const SearchItem *item,uint32_t id,uint64_t limit,char error[256]) {
    Store *s=cache.store;
    if(action==CACHE_REFRESH) { refresh(error); return; }
    if(action==CACHE_SET_LIMIT) {
        if(limit<64*CACHE_MIB || limit>16*1024*CACHE_MIB || limit<usage(s)) {
            snprintf(error,256,"上限不能低于已用空间，请先删除缓存"); return;
        }
        uint64_t before=s->limit; s->limit=limit;
        if(!save(error)) s->limit=before;
        return;
    }
    if(action==CACHE_DELETE || action==CACHE_CLEAR) {
        unsigned removed=0;
        for(uint32_t i=0;i<s->count;) {
            if(action==CACHE_CLEAR || s->entries[i].id==id) {
                if(!io(3,NULL,s->entries[i].uri,0,NULL,NULL,error)) break;
                remove_at(i);
                // 清空每16项保存，避免512次全量fsync；崩溃后的旧条目由启动探测修复。
                if(++removed%16==0 && !save(error)) { cache.healthy=false; break; }
                if(action==CACHE_DELETE) break;
            } else ++i;
        }
        if(removed && cache.healthy && !save(error)) cache.healthy=false;
        return;
    }
    if(action!=CACHE_DOWNLOAD || !item) return;
    for(uint32_t i=0;i<s->count;++i) if(same(item,&s->entries[i].item)) {
        if(io(2,NULL,s->entries[i].uri,0,NULL,NULL,error)) {
            // 旧缓存可补齐新搜索提供的封面地址，但绝不重新下载音频。
            if(item->song.cover[0] && strcmp(item->song.cover,s->entries[i].item.song.cover)) {
                char previous[2048]; memcpy(previous,s->entries[i].item.song.cover,sizeof(previous));
                memcpy(s->entries[i].item.song.cover,item->song.cover,sizeof(previous));
                if(!save(error)) {
                    memcpy(s->entries[i].item.song.cover,previous,sizeof(previous)); return;
                }
            }
            snprintf(error,256,"该歌曲已缓存，不会重复下载"); return;
        }
        if(strcmp(error,"缓存文件不存在")) return;
        remove_at(i); if(!save(error)) { cache.healthy=false; return; } error[0]=0; break;
    }
    if(s->count>=CACHE_LIMIT || s->next_id==UINT32_MAX) { snprintf(error,256,"缓存条目已满，请先删除"); return; }
    uint64_t used=usage(s),available=used<s->limit?s->limit-used:0;
    if(!available) { snprintf(error,256,"缓存空间已满，请删除缓存或提高上限"); return; }
    CacheEntry entry={.item=*item,.id=s->next_id};
    if(!io(1,item,NULL,available,entry.uri,&entry.bytes,error)) return;
    if(entry.bytes>available) {
        io(3,NULL,entry.uri,0,NULL,NULL,error); snprintf(error,256,"文件超过缓存剩余上限"); return;
    }
    s->entries[s->count++]=entry; ++s->next_id;
    if(!save(error)) {
        --s->count; --s->next_id;
        char ignored[256]; io(3,NULL,entry.uri,0,NULL,NULL,ignored);
    }
}
static void *worker(void *unused) {
    char error[256]="";
    cache.healthy=load(cache.store,cache.path,error);
    if(cache.healthy) {
        char ignored[256]=""; io(4,NULL,NULL,0,NULL,NULL,ignored);
        refresh(error);
    }
    publish(error[0]?error:cache.store->count?"已恢复公共目录缓存，可断网播放":"暂无缓存，可在播放页缓存当前歌曲");
    for(;;) {
        pthread_mutex_lock(&cache.lock);
        while(cache.running && !cache.pending) pthread_cond_wait(&cache.cond,&cache.lock);
        if(!cache.running) { pthread_mutex_unlock(&cache.lock); break; }
        int action=cache.action; uint32_t id=cache.id; uint64_t limit=cache.requested_limit;
        SearchItem item=cache.input; cache.pending=false;
        pthread_mutex_unlock(&cache.lock);
        error[0]=0;
        if(cache.healthy) change(action,&item,id,limit,error);
        else snprintf(error,256,"缓存索引不可写，请备份数据后修复或重启");
        publish(error[0]?error:action==CACHE_DOWNLOAD?"缓存完成，可断网播放":"缓存管理已更新");
    }
    return NULL;
}
bool cache_start(const char *directory,CacheIo callback,SourceWake wake,void *context) {
    cache.store=calloc(1,sizeof(Store));
    pthread_mutex_lock(&cache.lock);
    if(!cache.lookup) cache.lookup=calloc(CACHE_LIMIT,sizeof(CacheEntry));
    bool allocated=cache.store && cache.lookup;
    pthread_mutex_unlock(&cache.lock);
    if(!allocated) goto fail;
    pthread_mutex_lock(&cache.lock);
    cache.io=callback; cache.wake=wake; cache.context=context; cache.running=true; cache.pending=false;
    cache.page=0; cache.view.busy=true; ++cache.view.revision;
    snprintf(cache.view.message,256,"正在读取缓存索引…");
    snprintf(cache.path,sizeof(cache.path),"%s/lx-cache-v1.bin",directory);
    pthread_mutex_unlock(&cache.lock);
    if(pthread_create(&cache.thread,NULL,worker,NULL)!=0) goto fail;
    cache.started=true; return true;
fail:
    free(cache.store); cache.store=NULL;
    cache.running=false; cache.started=false; cache.view.busy=false;
    cache.wake=NULL; cache.context=NULL;
    snprintf(cache.view.message,256,"缓存线程启动失败，请重启应用"); ++cache.view.revision;
    return false;
}
void cache_stop(void) {
    pthread_mutex_lock(&cache.lock); cache.running=false; pthread_cond_signal(&cache.cond); pthread_mutex_unlock(&cache.lock);
    if(cache.started) pthread_join(cache.thread,NULL);
    pthread_mutex_lock(&cache.lock);
    cache.started=false; cache.wake=NULL; cache.context=NULL;
    // 保留有界内存查找表，Activity退出后的通知切歌仍可离线命中。
    free(cache.store); cache.store=NULL;
    pthread_mutex_unlock(&cache.lock);
}
bool cache_command(int action,const SearchItem *item,uint32_t id,uint64_t limit) {
    pthread_mutex_lock(&cache.lock);
    bool ok=cache.running && !cache.view.busy;
    if(ok) {
        cache.action=action; cache.id=id; cache.requested_limit=limit;
        if(item) cache.input=*item;
        cache.pending=true; cache.view.busy=true;
        snprintf(cache.view.message,256,action==CACHE_DOWNLOAD?"正在下载到公共音乐目录…":"正在更新缓存…");
        ++cache.view.revision; pthread_cond_signal(&cache.cond);
        if(cache.wake) cache.wake(cache.context);
    }
    pthread_mutex_unlock(&cache.lock); return ok;
}
void cache_page(int delta) {
    pthread_mutex_lock(&cache.lock); cache.page+=delta; page_locked(); pthread_mutex_unlock(&cache.lock);
}
bool cache_view(CacheView *view) {
    pthread_mutex_lock(&cache.lock);
    bool changed=view->revision!=cache.view.revision;
    if(changed) *view=cache.view;
    pthread_mutex_unlock(&cache.lock); return changed;
}
/* 服务冷启动只读索引，不创建Activity、不探测或修改公共文件。 */
void cache_media_load(const char *directory) {
    Store *s=calloc(1,sizeof(*s)); if(!s) return;
    char path[1100],error[256]="";
    snprintf(path,sizeof(path),"%s/lx-cache-v1.bin",directory);
    if(load(s,path,error)) {
        pthread_mutex_lock(&cache.lock);
        if(!cache.running) {
            if(!cache.lookup) cache.lookup=calloc(CACHE_LIMIT,sizeof(CacheEntry));
            if(cache.lookup) {
                cache.lookup_count=(int)s->count;
                memcpy(cache.lookup,s->entries,s->count*sizeof(CacheEntry));
            }
        }
        pthread_mutex_unlock(&cache.lock);
    }
    free(s);
}
bool cache_lookup(const SearchItem *item,char uri[CACHE_URI_BYTES]) {
    bool found=false;
    pthread_mutex_lock(&cache.lock);
    for(int i=0;i<cache.lookup_count;++i) if(same(item,&cache.lookup[i].item) || !strcmp(item->song.url,cache.lookup[i].uri)) {
        snprintf(uri,CACHE_URI_BYTES,"%s",cache.lookup[i].uri); found=true; break;
    }
    pthread_mutex_unlock(&cache.lock); return found;
}
