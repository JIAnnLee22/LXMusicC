#include "playback.h"
#include "playback_bridge.h"
#include "cache.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    pthread_mutex_t lock;
    SearchItem *queue;
    PlaybackView view;
    ALooper *looper;
    bool starting;
} playback={.lock=PTHREAD_MUTEX_INITIALIZER};

/* 随机模式历史栈：容量等于队列长度（≤SEARCH_RESULT_LIMIT），静态有界、不分配。
 * history[0..history_at] 是当前曲目之前走过的随机路径，history_at 之后是被改写前丢弃的旧路径。 */
static int shuffle_history[SEARCH_RESULT_LIMIT];
static int history_len, history_at;
static uint64_t shuffle_seed;

static void history_reset(int index) {
    shuffle_history[0]=index; history_len=1; history_at=0;
}
static void history_push(int index) {
    int cap=playback.view.count;
    if(cap<1) return;
    if(cap>SEARCH_RESULT_LIMIT) cap=SEARCH_RESULT_LIMIT;
    if(history_len>0 && shuffle_history[history_at]==index) return;
    history_len=history_at+1;                 // 随机改向后，旧“未来”不再有效
    if(history_len>=cap) {                    // 历史有界：挤掉最旧一条
        memmove(shuffle_history,shuffle_history+1,(size_t)(history_len-1)*sizeof(int));
        --history_len;
    }
    shuffle_history[history_len++]=index;
    history_at=history_len-1;
}
static int history_previous(void) {
    if(history_at<=0) return -1;
    return shuffle_history[--history_at];
}
/* 除当前曲外均匀取一首，count<2 时只能返回当前曲。 */
static int shuffle_pick(int current,int count) {
    if(count<2) return current;
    if(!shuffle_seed) {
        shuffle_seed=(uint64_t)time(NULL)*0x9e3779b97f4a7c15ull^(uintptr_t)&shuffle_seed;
    }
    shuffle_seed^=shuffle_seed<<13; shuffle_seed^=shuffle_seed>>7; shuffle_seed^=shuffle_seed<<17;
    int pick=(int)(shuffle_seed%(uint64_t)(count-1));
    return pick>=current?pick+1:pick;
}
/* 手动“下一首”的目标；返回 -1 表示顺序模式已到末尾（自然结束由调用者判断模式）。 */
static int next_index(void) {
    PlaybackView *v=&playback.view;
    if(v->mode==PLAYBACK_MODE_SHUFFLE) return shuffle_pick(v->index,v->count);
    if(v->mode==PLAYBACK_MODE_LIST || v->mode==PLAYBACK_MODE_SINGLE)
        return v->index+1<v->count?v->index+1:0;      // 列表/单曲循环：手动切歌仍环绕
    return v->index+1<v->count?v->index+1:-1;         // 顺序模式保持原有边界
}
static int previous_index(void) {
    PlaybackView *v=&playback.view;
    if(v->mode==PLAYBACK_MODE_SHUFFLE) {
        int index=history_previous();
        return index>=0?index:shuffle_pick(v->index,v->count);
    }
    return v->index>0?v->index-1:v->count-1;          // 列表/单曲循环：首曲回绕到末曲
}
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
/* 换曲：清空旧曲状态并递增代次，服务与解码器的迟到回调按代次丢弃。 */
static void load(int index) {
    PlaybackView *v=&playback.view;
    v->index=index; v->current=playback.queue[index];
    v->local_uri[0]=0;
    cache_lookup(&v->current,v->local_uri);
    v->position=v->duration=0; v->state=2; v->playing=false;
    v->requested=true; v->error[0]=0; ++v->generation;
    history_push(index);
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
    history_reset(index);                 // 新队列从当前曲开始重置随机历史
    load(index);
    pthread_mutex_unlock(&playback.lock);
    return true;
}
void playback_command(int action,int64_t position) {
    pthread_mutex_lock(&playback.lock);
    PlaybackView *v=&playback.view;
    if(action==PLAYBACK_MODE) {
        // 模式属于播放器偏好，空队列也允许设置；越界忽略。
        if(position>=0 && position<=PLAYBACK_MODE_SHUFFLE) {
            v->mode=(int)position;
            history_reset(v->index);
            publish();
        }
        pthread_mutex_unlock(&playback.lock);
        return;
    }
    if(!v->count) goto done;
    switch(action) {
        case PLAYBACK_END:
            // position 此处为结束曲目的代次，迟到的结束事件不能跳过新队列。
            if((uint64_t)position!=v->generation) break;
            // 已停止/暂停时不再按模式自动续播，避免停止后排队的结束事件重新起播。
            if(!v->requested) break;
            // 单曲循环只在自然结束时重播本曲；列表/随机按各自策略前进。
            {
                int target=v->mode==PLAYBACK_MODE_SINGLE?v->index:next_index();
                if(target>=0) load(target);
                else { v->requested=false; apply(PLAYBACK_PAUSE); }
            }
            break;
        case PLAYBACK_NEXT: {
            int target=next_index();
            if(target>=0) load(target);
            break;
        }
        case PLAYBACK_PREVIOUS:
            if(v->mode==PLAYBACK_MODE_SEQUENCE) {
                if(v->index>0) load(v->index-1);
                else { v->position=0; apply(PLAYBACK_SEEK); }
            } else load(previous_index());
            break;
        case PLAYBACK_SELECT:
            if(position>=0 && position<v->count) load((int)position);
            break;
        case PLAYBACK_PLAY: {
            char uri[CACHE_URI_BYTES]="";
            cache_lookup(&v->current,uri);
            bool changed=strcmp(uri,v->local_uri)!=0;
            if(changed) snprintf(v->local_uri,sizeof(v->local_uri),"%s",uri);
            v->requested=true; v->error[0]=0;
            if(changed && v->connected) { ++v->generation; apply(PLAYBACK_LOAD); }
            else if(!v->connected) { ++v->generation; publish(); }
            else {
                if(v->state==4 || (v->duration>0 && v->position>=v->duration-100)) {
                    v->position=0; apply(PLAYBACK_SEEK);
                }
                apply(action);
            }
            break;
        }
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
/* 与各模式的实际切歌语义一致：顺序模式按原边界，列表/单曲循环与随机（count>1）总能切。 */
bool playback_can_skip(int delta) {
    pthread_mutex_lock(&playback.lock);
    int index=playback.view.index, count=playback.view.count, mode=playback.view.mode;
    bool result;
    if(mode!=PLAYBACK_MODE_SEQUENCE) result=count>1;
    else if(delta>0) result=index+1<count;
    else if(delta<0) result=index>0;
    else result=count>0;
    pthread_mutex_unlock(&playback.lock);
    return result;
}
/* 播放页每帧安全拉取当前页：持锁拷贝，页码夹取到有效范围。 */
void playback_queue(PlaybackQueue *queue,int page) {
    if(!queue) return;
    pthread_mutex_lock(&playback.lock);
    PlaybackView *v=&playback.view;
    int total=v->count, start=0, count=0;
    if(total>0) {
        int max_page=(total-1)/PLAYBACK_QUEUE_PAGE;
        if(page<0) page=0;
        if(page>max_page) page=max_page;
        start=page*PLAYBACK_QUEUE_PAGE;
        count=total-start;
        if(count>PLAYBACK_QUEUE_PAGE) count=PLAYBACK_QUEUE_PAGE;
    } else page=0;
    queue->count=count; queue->total=total; queue->page=page;
    queue->index=v->index; queue->revision=v->revision;
    for(int i=0;i<count;++i) queue->items[i]=playback.queue[start+i];
    pthread_mutex_unlock(&playback.lock);
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
