#include "playback_store.h"
#include "library.h"
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

_Static_assert(sizeof(PlaybackSnapshot)==1296032,"playback v1 ABI changed: migrate explicitly");

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started,loaded,dirty;
    char path[1100];
} store={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};

/* 仅两个64位ABI；布局变更须升版本。恢复永不自动发声。 */
static void read_queue(PlaybackSnapshot *s) {
    FILE *f=fopen(store.path,"rb");
    if(!f) return;
    char magic[8]; uint32_t size;
    bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"LXPQ\1\0\0\0",8) &&
        fread(&size,4,1,f)==1 && size==sizeof(*s) && fread(s,sizeof(*s),1,f)==1 &&
        s->count>0 && s->count<=SEARCH_RESULT_LIMIT && s->index>=0 && s->index<s->count &&
        s->mode>=0 && s->mode<=PLAYBACK_MODE_SHUFFLE && s->position>=0 && s->position<=86400000;
    for(int i=0;ok && i<s->count;++i) ok=library_valid_item(&s->items[i]);
    if(ok) ok=fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if(ok) playback_restore(s);
    else __android_log_print(ANDROID_LOG_ERROR,"LXMusicC","Invalid playback snapshot; queue not restored");
}
static void write_queue(const PlaybackSnapshot *s) {
    char tmp[1150]; snprintf(tmp,sizeof(tmp),"%s.tmp",store.path);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);
    FILE *f=fd>=0?fdopen(fd,"wb"):NULL;
    if(!f) { if(fd>=0) close(fd); return; }
    uint32_t size=sizeof(*s);
    bool ok=fwrite("LXPQ\1\0\0\0",1,8,f)==8 && fwrite(&size,4,1,f)==1 && fwrite(s,sizeof(*s),1,f)==1;
    if(ok) ok=fflush(f)==0 && fsync(fd)==0;
    if(fclose(f)) ok=false;
    if(ok && !rename(tmp,store.path)) return;
    unlink(tmp);
    __android_log_print(ANDROID_LOG_ERROR,"LXMusicC","Cannot save playback queue; retry on next update");
}
static void *worker(void *unused) {
    PlaybackSnapshot *s=calloc(1,sizeof(*s));
    if(s) read_queue(s);
    pthread_mutex_lock(&store.lock);
    store.loaded=true; pthread_cond_broadcast(&store.cond);
    for(;;) {
        while(!store.dirty) pthread_cond_wait(&store.cond,&store.lock);
        store.dirty=false;
        pthread_mutex_unlock(&store.lock);
        /* 合并一次选曲中的LOAD/SEEK/状态回调；不在播放锁内做磁盘IO。 */
        struct timespec delay={.tv_nsec=250000000}; nanosleep(&delay,NULL);
        if(s && playback_snapshot(s)) write_queue(s);
        pthread_mutex_lock(&store.lock);
    }
    return NULL;
}
bool playback_store_start(const char *directory) {
    pthread_mutex_lock(&store.lock);
    if(!store.started && directory && strlen(directory)+32<sizeof(store.path)) {
        snprintf(store.path,sizeof(store.path),"%s/lx-playback-v1.bin",directory);
        pthread_t thread;
        if(!pthread_create(&thread,NULL,worker,NULL)) { store.started=true; pthread_detach(thread); }
    }
    bool ok=store.started;
    pthread_mutex_unlock(&store.lock); return ok;
}
void playback_store_wait(void) {
    pthread_mutex_lock(&store.lock);
    while(store.started && !store.loaded) pthread_cond_wait(&store.cond,&store.lock);
    pthread_mutex_unlock(&store.lock);
}
void playback_store_dirty(void) {
    pthread_mutex_lock(&store.lock);
    store.dirty=true; pthread_cond_signal(&store.cond);
    pthread_mutex_unlock(&store.lock);
}
