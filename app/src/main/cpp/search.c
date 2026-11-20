#include "search.h"
#include <android/log.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SEARCH_WORKERS 4
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool started, running, pending;
    atomic_uint_fast64_t token;
    char query[SEARCH_QUERY_BYTES];
    SearchView view;
    SearchItem results[SEARCH_RESULT_LIMIT];
    SearchHttp http;
    SourceWake wake;
    void *context;
} search={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};
typedef struct {
    uint64_t token;
    SourceScript sources[SOURCE_LIMIT];
    int count, next;
    char query[SEARCH_QUERY_BYTES], info[SEARCH_QUERY_BYTES*6+96];
} Job;

/* 以下发布函数均在 search.lock 内调用；只复制当前八项到 GL 快照。 */
static void publish(void) {
    SearchView *v=&search.view;
    int pages=(v->total+SEARCH_PAGE_SIZE-1)/SEARCH_PAGE_SIZE;
    if(v->page>=pages) v->page=pages?pages-1:0;
    v->count=0;
    for(int i=v->page*SEARCH_PAGE_SIZE;i<v->total && v->count<SEARCH_PAGE_SIZE;++i)
        v->items[v->count++]=search.results[i];
    ++v->revision;
    if(search.wake) search.wake(search.context);
}
bool search_is_cancelled(uint64_t token) {
    return token!=atomic_load_explicit(&search.token,memory_order_relaxed);
}
static bool cancelled(void *context) { return search_is_cancelled(((Job *)context)->token); }
static char *http(void *context,const char *request,char error[256]) {
    Job *job=context;
    if(cancelled(job)) { snprintf(error,256,"搜索已取消"); return NULL; }
    return search.http?search.http(search.context,request,job->token,error):NULL;
}
/* ASCII 大小写/空白归一化，不错误地逐字节 lower 中文 UTF-8。 */
static void normalize(const char *s,char *out,size_t cap) {
    size_t n=0;
    for(const unsigned char *p=(const unsigned char *)s;*p && n+1<cap;++p) {
        if(*p<128 && isspace(*p)) continue;
        out[n++]=*p<128?(char)tolower(*p):(char)*p;
    }
    out[n]=0;
}
static bool equal(const char *a,const char *b) {
    char x[256],y[256]; normalize(a,x,sizeof(x)); normalize(b,y,sizeof(y)); return !strcmp(x,y);
}
static bool duplicate(const SearchItem *a,const SearchItem *b) {
    if(a->source_id==b->source_id && a->song.id[0] && !strcmp(a->song.id,b->song.id)) return true;
    return equal(a->song.name,b->song.name) && equal(a->song.artist,b->song.artist) &&
        equal(a->song.album,b->song.album) && fabs(a->song.duration-b->song.duration)<=2;
}
static int score(const SourceSong *song,const char *keyword) {
    char name[256],artist[256],album[256],key[256];
    normalize(song->name,name,sizeof(name)); normalize(song->artist,artist,sizeof(artist));
    normalize(song->album,album,sizeof(album)); normalize(keyword,key,sizeof(key));
    if(!strcmp(name,key)) return 0;
    if(!strcmp(artist,key)) return 1;
    if(!strncmp(name,key,strlen(key))) return 2;
    if(strstr(name,key)) return 3;
    if(strstr(artist,key)) return 4;
    if(strstr(album,key)) return 5;
    return 6;
}
static int compare(const void *left,const void *right) {
    const SearchItem *a=left,*b=right;
    if(a->score!=b->score) return a->score-b->score;
    if(a->source_order!=b->source_order) return a->source_order-b->source_order;
    return a->song_order-b->song_order;
}
static void merge(Job *job,int index,const RuntimeResult *r) {
    SearchView *v=&search.view;
    for(int i=0;i<r->count;++i) {
        SearchItem item={.song=r->songs[i],.source_id=job->sources[index].meta.id,
            .source_order=index,.song_order=i,.score=score(&r->songs[i],job->query)};
        snprintf(item.source_name,sizeof(item.source_name),"%s",job->sources[index].meta.name);
        int same=-1;
        for(int k=0;k<v->total;++k) if(duplicate(&item,&search.results[k])) { same=k; break; }
        if(same>=0) {
            /* 网络先后不影响选源：始终保留设置中靠前的音源。 */
            SearchItem *old=&search.results[same];
            if(item.source_order<old->source_order ||
               (item.source_order==old->source_order && item.song_order<old->song_order)) *old=item;
        } else if(v->total<SEARCH_RESULT_LIMIT) search.results[v->total++]=item;
        else if(compare(&item,&search.results[v->total-1])<0) search.results[v->total-1]=item;
        qsort(search.results,(size_t)v->total,sizeof(SearchItem),compare);
    }
}
static void *worker(void *context) {
    Job *job=context;
    RuntimeResult *r=calloc(1,sizeof(*r));
    for(;;) {
        pthread_mutex_lock(&search.lock);
        int index=job->next++;
        pthread_mutex_unlock(&search.lock);
        if(index>=job->count || cancelled(job)) break;
        __android_log_print(ANDROID_LOG_INFO,"LXMusicC","search begin token=%llu source=%u",
            (unsigned long long)job->token,job->sources[index].meta.id);
        char error[256]="搜索内存不足，请重试";
        bool ok=r && runtime_call(job->sources[index].script,NULL,"search",job->info,
                                 http,cancelled,job,r,error);
        pthread_mutex_lock(&search.lock);
        if(!cancelled(job)) {
            if(ok) merge(job,index,r);
            else {
                ++search.view.failed;
                snprintf(search.view.error,sizeof(search.view.error),"%.60s: %.190s",job->sources[index].meta.name,error);
            }
            ++search.view.completed;
            snprintf(search.view.message,sizeof(search.view.message),"已完成 %d / %d 个音源，%d 首，%d 个失败",
                search.view.completed,search.view.sources,search.view.total,search.view.failed);
            publish();
            __android_log_print(ANDROID_LOG_INFO,"LXMusicC","search source=%u ok=%d count=%d done=%d/%d total=%d%s%s",
                job->sources[index].meta.id,ok,ok?r->count:0,search.view.completed,search.view.sources,search.view.total,
                ok?"":" error=",ok?"":error);
        }
        pthread_mutex_unlock(&search.lock);
        if(r) runtime_result_free(r);
        free(job->sources[index].script); job->sources[index].script=NULL;
    }
    free(r); return NULL;
}
static void encode_info(Job *job) {
    char *p=job->info;
    p+=sprintf(p,"{\"keyword\":\"");
    for(const unsigned char *s=(const unsigned char *)job->query;*s;++s) {
        if(*s=='"' || *s=='\\') { *p++='\\'; *p++=(char)*s; }
        else if(*s<32) p+=sprintf(p,"\\u%04x",*s);
        else *p++=(char)*s;
    }
    strcpy(p,"\",\"page\":1,\"limit\":30}");
}
static void *coordinator(void *unused) {
    (void)unused;
    for(;;) {
        pthread_mutex_lock(&search.lock);
        while(search.running && !search.pending) pthread_cond_wait(&search.cond,&search.lock);
        if(!search.running) { pthread_mutex_unlock(&search.lock); break; }
        Job *job=calloc(1,sizeof(*job));
        search.pending=false;
        if(!job) {
            search.view.busy=false; snprintf(search.view.message,sizeof(search.view.message),"搜索内存不足，请重试");
            publish(); pthread_mutex_unlock(&search.lock); continue;
        }
        job->token=atomic_load(&search.token); snprintf(job->query,sizeof(job->query),"%s",search.query);
        pthread_mutex_unlock(&search.lock);
        char error[256]="";
        job->count=source_snapshot(job->sources,error); encode_info(job);
        pthread_mutex_lock(&search.lock);
        if(!cancelled(job)) {
            search.view.sources=job->count>0?job->count:0;
            if(job->count<=0) snprintf(search.view.message,sizeof(search.view.message),"%s",
                job->count<0?error:"没有启用的音源，请到设置导入并启用支持搜索的音源");
            else snprintf(search.view.message,sizeof(search.view.message),"正在并发查询 %d 个启用音源…",job->count);
            publish();
        }
        pthread_mutex_unlock(&search.lock);
        pthread_t threads[SEARCH_WORKERS]; int started=0;
        if(job->count>0 && !cancelled(job)) {
            for(int i=0;i<SEARCH_WORKERS && i<job->count;++i)
                if(pthread_create(&threads[started],NULL,worker,job)==0) ++started;
            if(!started) worker(job); /* 线程资源不足时仍可串行重试。 */
            for(int i=0;i<started;++i) pthread_join(threads[i],NULL);
        }
        pthread_mutex_lock(&search.lock);
        if(!cancelled(job)) {
            SearchView *v=&search.view; v->busy=false;
            if(job->count>0) snprintf(v->message,sizeof(v->message),"%s：%d 首 / %d 个音源完成 / %d 个失败%s",
                v->total?"搜索完成":v->failed==v->sources?"搜索失败，请检查网络或音源后重试":"没有匹配歌曲，可更换关键词",
                v->total,v->completed,v->failed,v->total==SEARCH_RESULT_LIMIT?"（已达结果上限）":"");
            publish();
        }
        pthread_mutex_unlock(&search.lock);
        for(int i=0;i<job->count;++i) free(job->sources[i].script);
        free(job);
    }
    return NULL;
}
bool search_start(SearchHttp http_fn,SourceWake wake,void *context) {
    pthread_mutex_lock(&search.lock);
    if(search.started) { pthread_mutex_unlock(&search.lock); return false; }
    search.http=http_fn; search.wake=wake; search.context=context; search.running=true; search.pending=false;
    memset(&search.view,0,sizeof(search.view));
    snprintf(search.view.message,sizeof(search.view.message),"输入歌名、歌手或专辑，查询全部启用音源");
    atomic_fetch_add(&search.token,1); publish();
    search.started=pthread_create(&search.thread,NULL,coordinator,NULL)==0;
    if(!search.started) { search.running=false; snprintf(search.view.message,sizeof(search.view.message),"搜索线程启动失败，请重启应用"); publish(); }
    bool ok=search.started; pthread_mutex_unlock(&search.lock); return ok;
}
void search_stop(void) {
    pthread_mutex_lock(&search.lock);
    bool join=search.started; search.running=false; search.pending=false;
    atomic_fetch_add(&search.token,1); pthread_cond_signal(&search.cond);
    pthread_mutex_unlock(&search.lock);
    if(join) pthread_join(search.thread,NULL);
    pthread_mutex_lock(&search.lock);
    search.started=false; search.wake=NULL; search.context=NULL; search.http=NULL;
    pthread_mutex_unlock(&search.lock);
}
bool search_query(const char *keyword) {
    size_t n=keyword?strlen(keyword):0;
    if(n>=SEARCH_QUERY_BYTES || !source_utf8(keyword?keyword:"",n)) return false;
    char trimmed[SEARCH_QUERY_BYTES];
    while(n && (unsigned char)*keyword<=32) { ++keyword; --n; }
    while(n && (unsigned char)keyword[n-1]<=32) --n;
    if(!n) return false;
    memcpy(trimmed,keyword,n); trimmed[n]=0;
    pthread_mutex_lock(&search.lock);
    bool ok=search.running;
    if(ok) {
        unsigned revision=search.view.revision,paste_revision=search.view.paste_revision;
        char pasted[SEARCH_QUERY_BYTES]; memcpy(pasted,search.view.pasted,sizeof(pasted));
        memset(&search.view,0,sizeof(search.view));
        search.view.revision=revision; search.view.paste_revision=paste_revision;
        memcpy(search.view.pasted,pasted,sizeof(pasted));
        search.view.busy=true; snprintf(search.view.keyword,sizeof(search.view.keyword),"%s",trimmed);
        snprintf(search.query,sizeof(search.query),"%s",trimmed);
        snprintf(search.view.message,sizeof(search.view.message),"正在准备音源（旧请求若未释放，将在结束后开始）…");
        atomic_fetch_add(&search.token,1); search.pending=true;
        publish(); pthread_cond_signal(&search.cond);
    }
    pthread_mutex_unlock(&search.lock); return ok;
}
void search_cancel(void) {
    pthread_mutex_lock(&search.lock);
    if(search.view.busy) {
        atomic_fetch_add(&search.token,1); search.pending=false;
        search.view.busy=false; search.view.cancelled=true;
        snprintf(search.view.message,sizeof(search.view.message),"已取消，保留已到达的结果；可重新搜索"); publish();
    }
    pthread_mutex_unlock(&search.lock);
}
void search_page(int delta) {
    pthread_mutex_lock(&search.lock);
    int page=search.view.page+delta;
    if(page>=0 && page*SEARCH_PAGE_SIZE<search.view.total) { search.view.page=page; publish(); }
    pthread_mutex_unlock(&search.lock);
}
bool search_view(SearchView *view) {
    pthread_mutex_lock(&search.lock);
    bool changed=view->revision!=search.view.revision;
    if(changed) *view=search.view;
    pthread_mutex_unlock(&search.lock); return changed;
}
void search_paste(const char *text,size_t length,const char *error) {
    pthread_mutex_lock(&search.lock);
    if(!error && (!text || !length || length>=SEARCH_QUERY_BYTES || !source_utf8(text,length)))
        error="关键词为空、不是 UTF-8 或超过 255 字节";
    if(error) snprintf(search.view.error,sizeof(search.view.error),"%s",error);
    else {
        memcpy(search.view.pasted,text,length); search.view.pasted[length]=0;
        for(size_t i=0;i<length;++i) if((unsigned char)search.view.pasted[i]<32) search.view.pasted[i]=' ';
        ++search.view.paste_revision;
    }
    publish(); pthread_mutex_unlock(&search.lock);
}
