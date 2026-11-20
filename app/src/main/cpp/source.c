#include "source.h"
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { SourceMeta meta; char *script; } Entry;
typedef struct { Entry entries[SOURCE_LIMIT]; int count; uint32_t next_id; } Store;
static struct {
    pthread_mutex_t lock;
    pthread_mutex_t store_lock;
    Store *store;
    pthread_cond_t cond;
    pthread_t thread;
    bool running, pending, thread_started;
    int action;
    uint32_t id;
    char *input;
    char path[1024];
    SourceView view;
    SourceFetch fetch;
    RuntimeHttp http;
    SourceWake wake;
    void *context;
} service={.lock=PTHREAD_MUTEX_INITIALIZER,.store_lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};

int source_snapshot(SourceScript items[SOURCE_LIMIT],char error[256]) {
    int count=0;
    pthread_mutex_lock(&service.store_lock);
    if(!service.store) { snprintf(error,256,"音源尚未加载，请稍后重试"); count=-1; }
    else for(int i=0;i<service.store->count;++i) {
        Entry *e=&service.store->entries[i];
        if(!e->meta.enabled) continue;
        char *copy=strdup(e->script);
        if(!copy) {
            for(int k=0;k<count;++k) free(items[k].script);
            count=-1; snprintf(error,256,"音源快照内存不足，请重试"); break;
        }
        items[count++]=(SourceScript){e->meta,copy};
    }
    pthread_mutex_unlock(&service.store_lock);
    return count;
}

static void publish(const Store *s,const char *message) {
    pthread_mutex_lock(&service.lock);
    service.view.count=s->count;
    for(int i=0;i<s->count;++i) service.view.items[i]=s->entries[i].meta;
    service.view.busy=false;
    snprintf(service.view.message,sizeof(service.view.message),"%s",message);
    ++service.view.revision;
    if(service.wake) service.wake(service.context);
    pthread_mutex_unlock(&service.lock);
}
void source_message(const char *message) {
    pthread_mutex_lock(&service.lock);
    snprintf(service.view.message,sizeof(service.view.message),"%s",message);
    ++service.view.revision;
    if(service.wake) service.wake(service.context);
    pthread_mutex_unlock(&service.lock);
}
bool source_view(SourceView *view) {
    pthread_mutex_lock(&service.lock);
    bool changed=view->revision!=service.view.revision;
    if(changed) *view=service.view;
    pthread_mutex_unlock(&service.lock);
    return changed;
}
static bool put32(FILE *f,uint32_t v) {
    unsigned char b[4]={v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255};
    return fwrite(b,1,4,f)==4;
}
static bool get32(FILE *f,uint32_t *v) {
    unsigned char b[4]; if(fread(b,1,4,f)!=4) return false;
    *v=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); return true;
}
static bool save(const Store *s,char error[256]) {
    char temporary[1100]; snprintf(temporary,sizeof(temporary),"%s.tmp",service.path);
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC,0600);
    FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
    if(!f) { if(fd>=0) close(fd); goto fail; }
    bool ok=fwrite("LXSC\1\0\0\0",1,8,f)==8 && put32(f,(uint32_t)s->count) && put32(f,s->next_id);
    for(int i=0;ok && i<s->count;++i) {
        const Entry *e=&s->entries[i]; size_t n=strlen(e->script);
        ok=put32(f,e->meta.id) && put32(f,e->meta.enabled?1:0) && put32(f,(uint32_t)n) && fwrite(e->script,1,n,f)==n;
    }
    if(ok) ok=fflush(f)==0 && fsync(fd)==0;
    if(fclose(f)!=0) ok=false;
    if(ok && rename(temporary,service.path)==0) return true;
    unlink(temporary);
fail:
    snprintf(error,256,"保存音源失败，请检查可用空间后重试"); return false;
}
static bool load(Store *s,char error[256]) {
    FILE *f=fopen(service.path,"rb");
    if(!f) {
        if(errno==ENOENT) return true;
        snprintf(error,256,"无法读取音源存储，请重启重试"); return false;
    }
    char magic[8]; uint32_t count,next; size_t total=0;
    bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"LXSC\1\0\0\0",8) && get32(f,&count) && count<=SOURCE_LIMIT && get32(f,&next) && next>0;
    for(uint32_t i=0;ok && i<count;++i) {
        uint32_t id,enabled,n;
        ok=get32(f,&id) && get32(f,&enabled) && get32(f,&n) && id>0 && id<next && enabled<=1 && n>0 && n<=SOURCE_BYTES;
        if(!ok) break;
        for(int k=0;k<s->count;++k) if(s->entries[k].meta.id==id) ok=false;
        total+=n; if(total>SOURCE_TOTAL || !ok) { ok=false; break; }
        char *input=malloc((size_t)n+1),*script=NULL; SourceMeta meta;
        if(!input) { ok=false; break; }
        ok=fread(input,1,n,f)==n;
        input[n]=0;
        if(ok) ok=source_parse(input,n,&meta,&script,error);
        free(input);
        if(!ok) break;
        meta.id=id; meta.enabled=enabled!=0;
        s->entries[s->count++]=(Entry){meta,script};
    }
    if(ok) ok=fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if(ok) { s->next_id=next; return true; }
    for(int i=0;i<s->count;++i) free(s->entries[i].script);
    memset(s,0,sizeof(*s)); s->next_id=1;
    snprintf(error,256,"音源存储损坏，已停止写入以保护原文件"); return false;
}
static char *resolve(char *input,char error[256]) {
    char *p=input; while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t') ++p;
    size_t n=strlen(p); while(n && (p[n-1]==' '||p[n-1]=='\n'||p[n-1]=='\r'||p[n-1]=='\t')) --n;
    p[n]=0;
    if(!strncmp(p,"https://",8)||!strncmp(p,"http://",7)) {
        if(n>4096 || strpbrk(p," \r\n\t")) { snprintf(error,256,"音源链接无效"); return NULL; }
        if(service.fetch) return service.fetch(service.context,p,error);
        snprintf(error,256,"系统下载接口不可用，请重启重试"); return NULL;
    }
    char *result=malloc(n+1);
    if(result) memcpy(result,p,n+1); else snprintf(error,256,"内存不足，请重试");
    return result;
}
static void change(Store *s,int action,uint32_t id,char *input,char error[256]) {
    /* 浅拷贝作为事务回滚点；仅在保存成功后释放被删除的脚本。 */
    Store *before=malloc(sizeof(*before));
    if(!before) { snprintf(error,256,"内存不足，请重试"); return; }
    *before=*s;
    char *added=NULL,*removed=NULL;
    int index=-1;
    for(int i=0;i<s->count;++i) if(s->entries[i].meta.id==id) index=i;
    if(action==SOURCE_IMPORT) {
        if(s->count==SOURCE_LIMIT) { snprintf(error,256,"最多保存 32 个音源，请先删除"); goto end; }
        char *data=resolve(input,error); SourceMeta meta;
        if(!data) goto end;
        bool ok=source_parse(data,strlen(data),&meta,&added,error); free(data);
        if(!ok) goto end;
        size_t total=strlen(added);
        for(int i=0;i<s->count;++i) {
            total+=strlen(s->entries[i].script);
            if(!strcmp(added,s->entries[i].script)) { snprintf(error,256,"该音源已经导入"); goto end; }
        }
        if(total>SOURCE_TOTAL || s->next_id==UINT32_MAX) { snprintf(error,256,"音源存储已达上限，请先删除"); goto end; }
        meta.id=s->next_id++; s->entries[s->count++]=(Entry){meta,added};
    } else {
        if(index<0) { snprintf(error,256,"音源已不存在，请重试"); goto end; }
        if(action==SOURCE_TOGGLE) s->entries[index].meta.enabled=!s->entries[index].meta.enabled;
        else if(action==SOURCE_DELETE) {
            removed=s->entries[index].script;
            memmove(&s->entries[index],&s->entries[index+1],(size_t)(s->count-index-1)*sizeof(Entry)); --s->count;
        } else if(action==SOURCE_UP || action==SOURCE_DOWN) {
            int target=index+(action==SOURCE_UP?-1:1);
            if(target<0 || target>=s->count) { snprintf(error,256,"已经位于列表边界"); goto end; }
            Entry swap=s->entries[index]; s->entries[index]=s->entries[target]; s->entries[target]=swap;
        } else { snprintf(error,256,"未知音源操作"); goto end; }
    }
    if(save(s,error)) {
        free(removed); added=NULL;
        snprintf(error,256,"%s",action==SOURCE_IMPORT?"导入成功（仅保存，尚不执行脚本）":"已保存音源设置");
    } else *s=*before;
end:
    free(added); free(before);
}
static bool cancelled(void *context) {
    (void)context;
    pthread_mutex_lock(&service.lock); bool stop=!service.running; pthread_mutex_unlock(&service.lock);
    return stop;
}
static void selfcheck(Store *store,uint32_t id,char message[256]) {
    Entry *entry=NULL;
    for(int i=0;i<store->count;++i) if(store->entries[i].meta.id==id) entry=&store->entries[i];
    if(!entry || !entry->meta.enabled) { snprintf(message,256,"请先启用音源再执行自检"); return; }
    RuntimeResult *result=calloc(1,sizeof(*result)),*next=calloc(1,sizeof(*next));
    if(!result || !next) { free(result); free(next); snprintf(message,256,"内存不足，请重试"); return; }
    if(!runtime_call(entry->script,NULL,"search","{\"keyword\":\"love\",\"page\":1,\"limit\":5}",
                     service.http,cancelled,service.context,result,message)) goto end;
    if(!result->count) { snprintf(message,256,"真实搜索返回空数组，可再次自检重试"); goto end; }
    SourceSong *song=&result->songs[0];
    __android_log_print(ANDROID_LOG_INFO,"LXMusicC","runtime search: count=%d name=%s artist=%s album=%s duration=%.3f quality=%s httpUrl=%d",
        result->count,song->name,song->artist,song->album,song->duration,song->quality,song->url[0]!=0);
    if(!runtime_call(entry->script,NULL,"musicUrl",result->info_json,service.http,cancelled,service.context,next,message)) goto end;
    runtime_result_free(next);
    if(!runtime_call(entry->script,NULL,"lyric",result->info_json,service.http,cancelled,service.context,next,message)) goto end;
    __android_log_print(ANDROID_LOG_INFO,"LXMusicC","runtime handlers: musicUrl OK, lyric OK (%zu JSON bytes)",strlen(next->json));
    snprintf(message,256,"自检成功：%d 首 / 直链与歌词 handler 完成；首曲 %.60s / %.60s（详情见日志）",result->count,song->name,song->artist);
end:
    __android_log_print(ANDROID_LOG_INFO,"LXMusicC","runtime: %s",message);
    runtime_result_free(result); runtime_result_free(next); free(result); free(next);
}
static void *worker(void *unused) {
    (void)unused;
    Store *store=calloc(1,sizeof(*store));
    if(!store) {
        pthread_mutex_lock(&service.lock);
        service.running=false; service.view.busy=false;
        pthread_mutex_unlock(&service.lock);
        source_message("无法启动音源管理：内存不足，请重启重试"); return NULL;
    }
    store->next_id=1;
    char error[256]="复制音源链接、JSON 或 JS 文件内容后点击粘贴导入";
    bool writable=load(store,error);
    if(writable && store->count) snprintf(error,sizeof(error),"已恢复 %d 个音源及启用状态、顺序",store->count);
    pthread_mutex_lock(&service.store_lock); service.store=store; pthread_mutex_unlock(&service.store_lock);
    publish(store,error);
    for(;;) {
        pthread_mutex_lock(&service.lock);
        while(service.running && !service.pending) pthread_cond_wait(&service.cond,&service.lock);
        if(!service.running) { pthread_mutex_unlock(&service.lock); break; }
        int action=service.action; uint32_t id=service.id; char *input=service.input;
        service.input=NULL; service.pending=false;
        pthread_mutex_unlock(&service.lock);
        if(action==SOURCE_CHECK) selfcheck(store,id,error);
        else if(writable) {
            pthread_mutex_lock(&service.store_lock);
            change(store,action,id,input,error);
            pthread_mutex_unlock(&service.store_lock);
        }
        else snprintf(error,256,"音源存储损坏，未覆盖原文件；请备份后清除应用数据");
        free(input); publish(store,error);
    }
    pthread_mutex_lock(&service.store_lock); service.store=NULL;
    for(int i=0;i<store->count;++i) free(store->entries[i].script);
    pthread_mutex_unlock(&service.store_lock);
    free(store); return NULL;
}
bool source_start(const char *directory,SourceFetch fetch,RuntimeHttp http,SourceWake wake,void *context) {
    if(service.thread_started) return false;
    if(!directory || snprintf(service.path,sizeof(service.path),"%s/lx-sources-v1.bin",directory)>=(int)sizeof(service.path)) {
        source_message("私有存储路径不可用，请重启重试"); return false;
    }
    pthread_mutex_lock(&service.lock);
    service.fetch=fetch; service.http=http; service.wake=wake; service.context=context;
    memset(&service.view,0,sizeof(service.view)); service.view.revision=1; service.view.busy=true;
    service.running=true; service.pending=false; service.input=NULL;
    pthread_mutex_unlock(&service.lock);
    if(pthread_create(&service.thread,NULL,worker,NULL)==0) { service.thread_started=true; return true; }
    pthread_mutex_lock(&service.lock); service.running=false; service.view.busy=false;
    service.wake=NULL; service.context=NULL; pthread_mutex_unlock(&service.lock);
    source_message("无法启动音源工作线程，请重启重试"); return false;
}
void source_stop(void) {
    pthread_mutex_lock(&service.lock);
    bool join=service.thread_started; service.running=false; pthread_cond_signal(&service.cond);
    pthread_mutex_unlock(&service.lock);
    if(join) pthread_join(service.thread,NULL);
    pthread_mutex_lock(&service.lock);
    free(service.input); service.input=NULL; service.pending=false;
    service.wake=NULL; service.context=NULL; service.fetch=NULL; service.http=NULL; service.thread_started=false;
    pthread_mutex_unlock(&service.lock);
}
bool source_command(int action,uint32_t id,const char *input,size_t length) {
    char *copy=NULL;
    if(action==SOURCE_IMPORT) {
        if(!input || !length || length>SOURCE_BYTES || !source_utf8(input,length)) {
            source_message("剪贴板为空、不是 UTF-8 文本或超过 2 MiB"); return false;
        }
        copy=malloc(length+1);
        if(!copy) { source_message("内存不足，请重试"); return false; }
        memcpy(copy,input,length); copy[length]=0;
    }
    pthread_mutex_lock(&service.lock);
    bool ok=service.running && !service.view.busy;
    if(ok) {
        service.input=copy; service.action=action; service.id=id; service.pending=true; service.view.busy=true;
        snprintf(service.view.message,sizeof(service.view.message),"正在处理，请稍候…");
        ++service.view.revision; pthread_cond_signal(&service.cond);
        if(service.wake) service.wake(service.context);
    }
    pthread_mutex_unlock(&service.lock);
    if(!ok) { free(copy); source_message("音源正在处理或尚未就绪，请稍后重试"); }
    return ok;
}
