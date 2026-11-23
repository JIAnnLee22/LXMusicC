#include "ui.h"
#include "library_ui.h"
#include <math.h>
#include <string.h>

UiRoute ui_route(const Ui *u) {
    UiRoute r={.tab=u->state.tab,.library_filter=u->state.library_filter,
        .library_id=u->library_id,.library_page=u->library_page,.source_page=u->source_page,
        .queue_page=u->queue_page,.source_open=u->source_open,.cache_open=u->cache_open,
        .queue_open=u->queue_open,.scroll=u->state.scroll[u->state.tab]};
    memcpy(r.library_query,u->library_query,sizeof(r.library_query)); return r;
}
static bool same(UiRoute a,UiRoute b) {
    if(a.tab!=b.tab) return false;
    if(a.tab==0) return a.library_id==b.library_id;
    if(a.tab==2) return a.queue_open==b.queue_open;
    if(a.tab==3) return a.source_open==b.source_open && a.cache_open==b.cache_open;
    return true;
}
static void restore(Ui *u,UiRoute r) {
    bool browse=r.tab==0 && (u->library_id!=r.library_id || u->library_page!=r.library_page ||
        strcmp(u->library_query,r.library_query));
    u->state.tab=r.tab;
    // 其它 tab 的局部上下文不随返回历史回滚；保留各页独立的选项和偏移。
    if(r.tab==0) {
        u->state.library_filter=r.library_filter; u->library_id=r.library_id; u->library_page=r.library_page;
        memcpy(u->library_query,r.library_query,sizeof(u->library_query));
        if(browse) ui_library_refresh(u);
    }
    if(r.tab==2) { u->queue_open=r.queue_open; u->queue_page=r.queue_page; }
    if(r.tab==3) { u->source_open=r.source_open; u->cache_open=r.cache_open; u->source_page=r.source_page; }
    u->state.scroll[r.tab]=r.scroll; u->max_scroll[r.tab]=-1;
    u->ime_target=0; u->input_open=false; u->transition=1; ui_cancel(u);
}
void ui_route_record(Ui *u,UiRoute previous) {
    UiRoute next=ui_route(u);
    if(same(previous,next)) return;
    for(int i=u->history_count-1;i>=0;--i) if(same(u->history[i],next)) {
        UiRoute saved=u->history[i]; u->history_count=i; restore(u,saved); return;
    }
    if(u->history_count==16) {
        memmove(u->history,u->history+1,15*sizeof(*u->history)); --u->history_count;
    }
    u->history[u->history_count++]=previous;
    u->ime_target=0; u->input_open=false; u->max_scroll[u->state.tab]=-1;
    u->transition=1; ui_cancel(u);
}
bool ui_route_pop(Ui *u) {
    if(!u->history_count) return false;
    restore(u,u->history[--u->history_count]); return true;
}
bool ui_can_back(const Ui *u) {
    return u->ime_target || u->input_open || u->history_count || u->state.tab!=0 ||
        u->library_input || u->library_confirm || u->library_adding || u->library_id>=2 ||
        u->cache_delete_id || u->cache_clear_confirm || u->delete_id ||
        (u->state.tab==3 && u->state.settings_row>=0);
}
void ui_pressed_draw(Ui *u,Renderer *r,bool navigation) {
    if(!u->pressed || u->dragging || u->seeking || u->ime_gesture) return;
    for(int i=u->hit_count-1;i>=0;--i) {
        Rect b=u->hits[i].rect;
        bool nav=b.y>=u->body.y+u->body.h;
        if(nav!=navigation) continue;
        if(u->down_x>=b.x && u->down_x<b.x+b.w && u->down_y>=b.y && u->down_y<b.y+b.h) {
            render_rect(r,b,12,(Color){1,1,1,.12f}); return;
        }
    }
}
bool ui_current_song(const Ui *u,const SearchItem *item) {
    const SearchItem *current=&u->playback.current;
    return u->playback.count && current->source_id==item->source_id &&
        !strcmp(current->song.id,item->song.id) && !strcmp(current->song.name,item->song.name);
}
