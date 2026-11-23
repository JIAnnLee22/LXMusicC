#include "ime_ui.h"
#include "ime_bridge.h"
#include "library_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static char *input(Ui *u,int target) {
    return target==IME_SEARCH?u->query:target==IME_LIBRARY?u->library_query:
        target==IME_SOURCE?u->source_url:u->library_name;
}
static bool valid(Ui *u,int target) {
    if(target==IME_SEARCH) return u->state.tab==1 && u->input_open;
    if(target==IME_SOURCE) return u->state.tab==3 && u->source_open;
    return u->state.tab==0 && (target==IME_LIBRARY?u->library_input==1:
        target==IME_CREATE?u->library_input==2:target==IME_RENAME && u->library_input==3);
}
static void notice(Ui *u,const char *s,double now) {
    snprintf(u->toast,sizeof(u->toast),"%s",s); u->toast_until=now+2.6;
}
void ui_ime_open(Ui *u,int target) {
    if(target!=u->ime_target) {
        u->ime_start=u->ime_end=(int)strlen(input(u,target)); u->ime_offset=0;
        u->ime_cs=u->ime_ce=-1; u->ime_menu=false;
    }
    u->ime_target=target; u->ime_show=true;
}
void ui_ime_submit(Ui *u,double now) {
    int target=u->ime_target;
    if(target==IME_SEARCH) ui_search_key(u,13,now);
    else if(target==IME_SOURCE) {
        size_t n=strlen(u->source_url);
        if(strncmp(u->source_url,"https://",8) && strncmp(u->source_url,"http://",7)) {
            notice(u,"请输入 HTTP(S) 音源地址",now); return;
        }
        if(!source_command(SOURCE_IMPORT,0,u->source_url,n)) {
            notice(u,"音源正在处理，请稍后重试",now); return;
        }
    } else if(target) ui_library_key(u,13,now);
    if(target && (target==IME_SOURCE || !valid(u,target))) u->ime_target=0;
}
bool ui_ime_sync(Ui *u,ANativeActivity *activity,double now) {
    static int serial=0; // 跨 Activity 不复用 token，拒绝迟到的主线程回调。
    bool dirty=false;
    if(u->ime_target && !valid(u,u->ime_target)) u->ime_target=0;
    ImeEvent event={0}; int bottom=0,reference=0;
    bool pending=ime_bridge_take(&event,&bottom,&reference);
    if(bottom!=u->ime_bottom || reference!=u->ime_reference_height) {
        u->ime_bottom=bottom; u->ime_reference_height=reference; dirty=true;
    }
    if(pending && event.session==u->ime_session && u->ime_target==u->ime_active && u->ime_target) {
        if(event.error[0]) notice(u,event.error,now);
        if(event.changed) {
            size_t n=strlen(event.text),cap=u->ime_target==IME_SOURCE?2048:256;
            if(n<cap) {
                bool accept_selection=!u->ime_selection_dirty || strcmp(event.text,u->ime_mirror);
                memcpy(input(u,u->ime_target),event.text,n+1);
                memcpy(u->ime_mirror,event.text,n+1);
                if(accept_selection) {
                    u->ime_start=event.start; u->ime_end=event.end; u->ime_selection_dirty=false;
                }
                u->ime_cs=event.composing_start; u->ime_ce=event.composing_end;
            }
        }
        if(event.submit) ui_ime_submit(u,now);
        if(event.hidden || event.failed) { u->ime_target=0; u->input_open=false; }
        dirty=true;
    }
    if(u->ime_target!=u->ime_active || (u->ime_show && u->ime_target && !u->ime_bottom)) {
        int action=u->ime_target?1:0;
        const char *s=u->ime_target?input(u,u->ime_target):"";
        u->ime_session=++serial;
        if(!ime_bridge_request(activity,action,u->ime_session,s,u->ime_target?u->ime_start:0,
            u->ime_target?u->ime_end:0,u->ime_target==IME_SEARCH || u->ime_target==IME_LIBRARY?1:
            u->ime_target==IME_SOURCE?3:2)) {
            notice(u,"无法打开系统键盘，请再次点击输入框",now); u->ime_target=0;
        }
        u->ime_active=u->ime_target;
        snprintf(u->ime_mirror,sizeof(u->ime_mirror),"%s",s);
        u->ime_selection_dirty=false; u->ime_menu=false; dirty=true;
    } else if(u->ime_target) {
        const char *s=input(u,u->ime_target);
        if(u->ime_selection_dirty || strcmp(s,u->ime_mirror)) {
            int n=(int)strlen(s);
            if(strcmp(s,u->ime_mirror)) u->ime_start=u->ime_end=n;
            if(u->ime_start>n) u->ime_start=n;
            if(u->ime_end>n) u->ime_end=n;
            if(!ime_bridge_request(activity,2,u->ime_session,s,u->ime_start,u->ime_end,0))
                notice(u,"输入同步失败，请重新点击输入框",now);
            snprintf(u->ime_mirror,sizeof(u->ime_mirror),"%s",s);
            u->ime_selection_dirty=false; dirty=true;
        }
    }
    u->ime_show=false;
    if(!u->ime_target) u->ime_paste=false;
    if(u->ime_paste && u->ime_target) {
        u->ime_paste=false;
        if(!ime_bridge_request(activity,3,u->ime_session,input(u,u->ime_target),u->ime_start,u->ime_end,0))
            notice(u,"无法粘贴，请重试",now);
        dirty=true;
    }
    return dirty;
}
static float prefix(const char *s,int n) {
    char buf[IME_BYTES]; int length=(int)strlen(s);
    if(n<0) n=0;
    if(n>length) n=length;
    memcpy(buf,s,(size_t)n); buf[n]=0; return render_text_width(buf,14);
}
void ui_ime_field(Ui *u,Renderer *r,Rect box,int target,const char *s,const char *hint) {
    const Color text={.90f,.93f,.97f,1},muted={.53f,.60f,.68f,1},card={.075f,.094f,.12f,1};
    bool active=u->ime_target==target;
    render_rect(r,box,12,active?TINT:card);
    Rect clip={box.x+8,fmaxf(box.y,u->body.y),fmaxf(0,box.w-16),
        fmaxf(0,fminf(box.y+box.h,u->body.y+u->body.h)-fmaxf(box.y,u->body.y))};
    if(active) {
        u->ime_rect=box;
        // 先滚到输入框；本帧结束 ui_tick 会标记重画，键盘/旋转不裁切编辑目标。
        float shift=box.y<u->body.y?box.y-u->body.y:
            box.y+box.h>u->body.y+u->body.h?box.y+box.h-u->body.y-u->body.h:0;
        if(fabsf(shift)>.5f) {
            u->state.scroll[u->state.tab]=fmaxf(0,u->state.scroll[u->state.tab]+shift);
            u->ime_relayout=true;
        }
        float caret=prefix(s,u->ime_end);
        if(caret-u->ime_offset>clip.w-3) u->ime_offset=caret-clip.w+3;
        if(caret<u->ime_offset) u->ime_offset=caret;
    }
    float offset=active?u->ime_offset:0,base=box.y+box.h*.5f+5;
    render_clip(r,clip);
    if(active) {
        float a=prefix(s,u->ime_start),b=prefix(s,u->ime_end);
        Color selection=ACCENT; selection.a=.28f;
        if(a!=b) render_rect(r,(Rect){box.x+8+fminf(a,b)-offset,base-16,fabsf(a-b),20},0,selection);
        render_rect(r,(Rect){box.x+8+b-offset,base-16,1.5f,20},0,ACCENT);
        if(u->ime_cs>=0 && u->ime_ce>=0) {
            float c=prefix(s,u->ime_cs),d=prefix(s,u->ime_ce);
            render_rect(r,(Rect){box.x+8+fminf(c,d)-offset,base+4,fabsf(d-c),1.5f},0,ACCENT);
        }
    }
    render_text(r,box.x+8-offset,base,14,s[0]?text:muted,s[0]?s:hint);
    render_clip(r,u->body);
}
static Rect menu_rect(Ui *u) {
    Rect box=u->ime_rect;
    float y=box.y+box.h+34>u->body.y+u->body.h?box.y-34:box.y+box.h+2;
    y=fmaxf(u->body.y,fminf(y,u->body.y+u->body.h-32));
    return (Rect){box.x,y,fminf(box.w,180),32};
}
void ui_ime_menu_draw(Ui *u,Renderer *r) {
    if(!u->ime_target || !u->ime_menu) return;
    Rect menu=menu_rect(u);
    render_rect(r,menu,8,TINT);
    render_text(r,menu.x+12,menu.y+21,13,ACCENT,"全选");
    render_text(r,menu.x+menu.w/2+12,menu.y+21,13,ACCENT,"粘贴");
}
static bool inside(Rect r,float x,float y) { return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }
static int offset_at(Ui *u,float x) {
    const char *s=input(u,u->ime_target);
    float target=x-u->ime_rect.x-8+u->ime_offset,previous=0;
    int n=(int)strlen(s);
    for(int i=0;i<n;) {
        int next=i+1; while(next<n && ((unsigned char)s[next]&0xc0)==0x80) ++next;
        float width=prefix(s,next);
        if(target<(previous+width)/2) return i;
        previous=width; i=next;
    }
    return n;
}
bool ui_ime_touch(Ui *u,int action,float x,float y,double now) {
    if(!u->ime_target) return false;
    Rect menu=menu_rect(u);
    if(action==0) {
        if(u->ime_menu && inside(menu,x,y)) {
            if(x<menu.x+menu.w/2) {
                u->ime_start=0; u->ime_end=(int)strlen(input(u,u->ime_target)); u->ime_selection_dirty=true;
            } else { u->ime_paste=true; u->ime_menu=false; }
            u->ime_gesture=true; u->ime_down_time=-1; return true;
        }
        u->ime_menu=false;
        if(!inside(u->ime_rect,x,y) || !inside(u->body,x,y)) return false;
        u->ime_gesture=true; u->ime_selecting=false; u->ime_down_time=now;
        u->ime_anchor=offset_at(u,x); u->down_x=x; u->down_y=y; return true;
    }
    if(!u->ime_gesture) return false;
    if(u->ime_down_time>=0) {
        if(action==2 && fabsf(x-u->down_x)>8) u->ime_selecting=true;
        if(action==1 || u->ime_selecting) {
            u->ime_start=u->ime_anchor; u->ime_end=offset_at(u,x); u->ime_selection_dirty=true;
            if(action==1 && !u->ime_selecting && now-u->ime_down_time>.45) u->ime_menu=true;
        }
    }
    if(action==1 || action==3) u->ime_gesture=false;
    return true;
}
