#include "playback.h"
#include "playback_bridge.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    pthread_mutex_t lock;
    SearchItem *queue;
    PlaybackView view;
    ALooper *looper;
    bool starting;
} playback={.lock=PTHREAD_MUTEX_INITIALIZER};

static void publish(void) {
    ++playback.view.revision;
    if(playback.looper) ALooper_wake(playback.looper);
}
static void apply(int action) {
    if(playback.view.connected && !playback_bridge_apply(action,&playback.view)) {
        snprintf(playback.view.error,256,"播放命令失败，请重新点击播放");
        playback.view.requested=false;
    }
    publish();
}
static void load(int index) {
    PlaybackView *v=&playback.view;
    v->index=index; v->current=playback.queue[index];
    v->position=v->duration=0; v->state=2; v->playing=false;
    v->requested=true; v->error[0]=0; ++v->generation;
    apply(PLAYBACK_LOAD);
}
void playback_ui(ALooper *looper) {
    pthread_mutex_lock(&playback.lock);
    if(playback.looper) ALooper_release(playback.looper);
    playback.looper=looper;
    if(looper) ALooper_acquire(looper);
    pthread_mutex_unlock(&playback.lock);
}
bool playback_view(PlaybackView *view) {
    pthread_mutex_lock(&playback.lock);
    bool changed=view->revision!=playback.view.revision;
    if(changed) *view=playback.view;
    pthread_mutex_unlock(&playback.lock);
    return changed;
}
bool playback_replace(const SearchItem *items,int count,int index) {
    if(!items || count<1 || count>SEARCH_RESULT_LIMIT || index<0 || index>=count) return false;
    SearchItem *queue=malloc((size_t)count*sizeof(*queue));
    if(!queue) return false;
    memcpy(queue,items,(size_t)count*sizeof(*queue));
    pthread_mutex_lock(&playback.lock);
    free(playback.queue); playback.queue=queue; playback.view.count=count;
    load(index);
    pthread_mutex_unlock(&playback.lock);
    return true;
}
void playback_command(int action,int64_t position) {
    pthread_mutex_lock(&playback.lock);
    PlaybackView *v=&playback.view;
    if(!v->count) goto done;
    switch(action) {
        case PLAYBACK_END:
            // position 此处为结束曲目的代次，迟到的结束事件不能跳过新队列。
            if((uint64_t)position!=v->generation) break;
            if(v->index+1<v->count) load(v->index+1);
            else { v->requested=false; apply(PLAYBACK_PAUSE); }
            break;
        case PLAYBACK_NEXT:
            if(v->index+1<v->count) load(v->index+1);
            break;
        case PLAYBACK_PREVIOUS:
            if(v->index>0) load(v->index-1);
            else { v->position=0; apply(PLAYBACK_SEEK); }
            break;
        case PLAYBACK_PLAY:
            v->requested=true; v->error[0]=0;
            if(!v->connected) { ++v->generation; publish(); }
            else {
                if(v->state==4 || (v->duration>0 && v->position>=v->duration-100)) {
                    v->position=0; apply(PLAYBACK_SEEK);
                }
                apply(action);
            }
            break;
        case PLAYBACK_PAUSE: case PLAYBACK_STOP:
            v->requested=false; apply(action); break;
        case PLAYBACK_SEEK:
            v->position=position<0?0:position;
            if(v->duration>0 && v->position>v->duration) v->position=v->duration;
            apply(action); break;
        default: break;
    }
done:
    pthread_mutex_unlock(&playback.lock);
}
bool playback_can_skip(int delta) {
    pthread_mutex_lock(&playback.lock);
    int target=playback.view.index+delta;
    bool result=target>=0 && target<playback.view.count;
    pthread_mutex_unlock(&playback.lock);
    return result;
}
void playback_ready(void) {
    pthread_mutex_lock(&playback.lock);
    playback.starting=false; playback.view.connected=true;
    if(playback.view.count) apply(PLAYBACK_LOAD); else publish();
    pthread_mutex_unlock(&playback.lock);
}
void playback_gone(void) {
    pthread_mutex_lock(&playback.lock);
    playback.starting=false; playback.view.connected=false;
    playback.view.playing=playback.view.requested=false; publish();
    pthread_mutex_unlock(&playback.lock);
}
void playback_failed(void) {
    pthread_mutex_lock(&playback.lock);
    playback.starting=false; playback.view.requested=false;
    snprintf(playback.view.error,256,"无法启动播放服务，请回到前台点击播放重试"); publish();
    pthread_mutex_unlock(&playback.lock);
}
void playback_state(uint64_t generation,int state,bool playing,bool requested,
                    int64_t position,int64_t duration,const char *error) {
    pthread_mutex_lock(&playback.lock);
    PlaybackView *v=&playback.view;
    if(generation==v->generation && v->connected &&
       (v->state!=state || v->playing!=playing || v->requested!=requested ||
        v->position!=position || v->duration!=duration || strcmp(v->error,error?error:""))) {
        v->state=state; v->playing=playing; v->requested=requested;
        v->position=position; v->duration=duration;
        snprintf(v->error,256,"%s",error?error:""); publish();
    }
    pthread_mutex_unlock(&playback.lock);
}
void playback_ensure(ANativeActivity *activity) {
    pthread_mutex_lock(&playback.lock);
    bool start=playback.view.count && !playback.view.connected && !playback.starting;
    if(start) playback.starting=true;
    pthread_mutex_unlock(&playback.lock);
    if(start && !playback_bridge_start(activity)) playback_failed();
}
