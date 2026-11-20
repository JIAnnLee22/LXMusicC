#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const Color TEXT={.90f,.93f,.97f,1}, MUTED={.53f,.60f,.68f,1};
static const Color CARD={.075f,.094f,.12f,1}, LINE={.13f,.17f,.21f,1};
static const Color ACCENT={.34f,.88f,.72f,1}, TINT={.08f,.20f,.19f,1};
static const char *TABS[]={"歌曲","搜索","正在播放","设置"};
static const char *FILTERS[]={"收藏","最近播放","我的歌单"};
enum { ACT_TAB=1, ACT_FILTER, ACT_CHIP, ACT_SETTING, ACT_NOTICE };

static bool inside(Rect b,float x,float y) { return x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h; }
static float clamp(float x,float a,float b) { return fminf(fmaxf(x,a),b); }
static void hit(Ui *u,Rect b,int action,int value) {
    if(u->hit_count<40) u->hits[u->hit_count++]=(UiHit){b,action,value};
}
static void text(Renderer *r,float x,float y,float size,Color color,const char *s) {
    render_text(r,x,y,size,color,s);
}
static void center(Renderer *r,Rect b,float size,Color color,const char *s) {
    text(r,b.x+(b.w-render_text_width(s,size))/2,b.y+b.h/2+size*.34f,size,color,s);
}
static void button(Ui *u,Renderer *r,Rect b,const char *s,int action,int value,bool filled) {
    render_rect(r,b,12,filled?ACCENT:TINT);
    center(r,b,14,filled?(Color){.025f,.09f,.085f,1}:ACCENT,s); hit(u,b,action,value);
}
static void notice(Ui *u,const char *s,double now) {
    snprintf(u->toast,sizeof(u->toast),"%s",s); u->toast_until=now+2.6;
}

void ui_init(Ui *u,const UiState *saved) {
    memset(u,0,sizeof(*u)); u->state=(UiState){.version=UI_STATE_VERSION,.search_chip=-1,.settings_row=-1};
    u->pointer=-1;
    for(int i=0;i<4;++i) u->max_scroll[i]=-1; // Unmeasured: retain restored offsets until first layout.
    if(saved && saved->version==UI_STATE_VERSION && saved->tab>=0 && saved->tab<4) {
        u->state=*saved;
        u->state.library_filter=(int)clamp(u->state.library_filter,0,2);
        u->state.search_chip=(int)clamp(u->state.search_chip,-1,3);
        u->state.settings_row=(int)clamp(u->state.settings_row,-1,6);
        for(int i=0;i<4;++i) if(!isfinite(u->state.scroll[i]) || u->state.scroll[i]<0) u->state.scroll[i]=0;
    }
}
void ui_cancel(Ui *u) { u->pressed=u->dragging=false; u->velocity=0; u->pointer=-1; }
void ui_layout(Ui *u,Rect area) {
    u->area=area;
    float header=area.h<380?66:92;
    u->body=(Rect){area.x+20,area.y+header,area.w-40,fmaxf(0,area.h-header-76)};
}

static float library(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y, gap=8, fw=(w-2*gap)/3;
    for(int i=0;i<3;++i) {
        Rect b={x+i*(fw+gap),y,fw,42}; bool active=u->state.library_filter==i;
        render_rect(r,b,13,active?TINT:CARD); center(r,b,13,active?ACCENT:MUTED,FILTERS[i]);
        hit(u,b,ACT_FILTER,i);
    }
    y+=62;
    render_rect(r,(Rect){x,y,w,178},20,CARD);
    render_rect(r,(Rect){x+20,y+20,44,44},14,TINT); center(r,(Rect){x+20,y+20,44,44},24,ACCENT,"♪");
    const char *titles[]={"把喜欢的音乐留在这里","还没有播放记录","你的第一张歌单"};
    text(r,x+20,y+92,20,TEXT,titles[u->state.library_filter]);
    text(r,x+20,y+119,13,MUTED,"当前没有歌曲，先从音源开始");
    button(u,r,(Rect){x+20,y+135,128,32},"前往设置",ACT_TAB,3,false);
    y+=204;
    text(r,x,y+16,17,TEXT,"开始聆听"); y+=34;
    const char *names[]={"添加音乐来源","发现喜欢的歌曲","建立你的音乐库"};
    const char *descs[]={"在设置中查看音源入口","搜索歌名、歌手或专辑","收藏、历史与歌单尚未开放"};
    for(int i=0;i<3;++i) {
        render_rect(r,(Rect){x,y,w,76},16,CARD);
        char number[4]; snprintf(number,sizeof(number),"0%d",i+1);
        text(r,x+16,y+32,16,ACCENT,number); text(r,x+50,y+29,15,TEXT,names[i]);
        text(r,x+50,y+53,12,MUTED,descs[i]);
        hit(u,(Rect){x,y,w,76},i==0?ACT_TAB:i==1?ACT_TAB:ACT_NOTICE,i==0?3:i==1?1:0); y+=88;
    }
    return y-start+8;
}

static float search(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y;
    render_rect(r,(Rect){x,y,w,54},16,CARD);
    text(r,x+18,y+34,15,u->state.search_chip<0?MUTED:TEXT,
        u->state.search_chip<0?"搜索歌曲、歌手、专辑":"已选择搜索分类");
    hit(u,(Rect){x,y,w,54},ACT_NOTICE,1); y+=82;
    text(r,x,y+16,17,TEXT,"从这里开始"); y+=34;
    const char *chips[]={"歌曲","歌手","专辑","纯音乐"};
    float cw=(w-24)/4;
    for(int i=0;i<4;++i) {
        Rect b={x+i*(cw+8),y,cw,38}; bool active=u->state.search_chip==i;
        render_rect(r,b,12,active?TINT:CARD); center(r,b,13,active?ACCENT:MUTED,chips[i]); hit(u,b,ACT_CHIP,i);
    }
    y+=62;
    render_rect(r,(Rect){x,y,w,210},20,CARD);
    center(r,(Rect){x,y+20,w,44},28,ACCENT,"♪");
    center(r,(Rect){x,y+74,w,28},19,TEXT,"还没有可用音源");
    center(r,(Rect){x,y+108,w,24},13,MUTED,"音源导入与真实搜索将在后续开放");
    button(u,r,(Rect){x+(w-136)/2,y+151,136,40},"查看设置",ACT_TAB,3,true);
    return y+234-start;
}

static float player(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; bool wide=w>600;
    float cover=wide?fminf(220,w*.34f):fminf(224,w-32);
    float cx=wide?x:x+(w-cover)/2;
    render_rect(r,(Rect){cx,y,cover,cover},28,TINT);
    render_rect(r,(Rect){cx+cover*.22f,y+cover*.22f,cover*.56f,cover*.56f},cover*.28f,LINE);
    center(r,(Rect){cx,y,cover,cover},58,ACCENT,"♪");
    float dx=wide?x+cover+36:x, dy=wide?y+8:y+cover+20, dw=wide?w-cover-36:w;
    center(r,(Rect){dx,dy,dw,36},22,TEXT,"还没有正在播放的歌曲");
    center(r,(Rect){dx,dy+42,dw,24},13,MUTED,"选择音乐后，播放信息会显示在这里");
    render_rect(r,(Rect){dx+12,dy+92,dw-24,3},2,LINE);
    text(r,dx+12,dy+119,11,MUTED,"--:--"); text(r,dx+dw-42,dy+119,11,MUTED,"--:--");
    button(u,r,(Rect){dx+(dw-148)/2,dy+144,148,44},"前往搜索",ACT_TAB,1,true);
    center(r,(Rect){dx,dy+206,dw,24},12,MUTED,"播放与队列尚未开放");
    return fmaxf(y+cover,dy+246)-start+20;
}

static float settings(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y;
    render_rect(r,(Rect){x,y,w,90},20,TINT);
    text(r,x+18,y+34,20,TEXT,"LX Music C"); text(r,x+18,y+63,13,ACCENT,"0.1.0  /  界面预览"); y+=112;
    const char *titles[]={"音源管理","缓存管理","仅 Wi-Fi 下载","主题外观","后台播放","数据与隐私","关于"};
    const char *values[]={"未添加音源","暂无缓存","尚未开放","深色 / 薄荷绿","尚未开放","本地优先","界面骨架"};
    const char *details[]={"LX 音源导入将在后续开放","公共音乐目录缓存将在后续开放","下载功能尚未开放","当前提供固定深色主题","播放服务与通知将在后续开放","当前预览不会发起网络请求","纯 C 界面 · Android 8.0 及以上"};
    for(int i=0;i<7;++i) {
        bool selected=u->state.settings_row==i;
        float h=selected?112:78;
        render_rect(r,(Rect){x,y,w,h},16,selected?TINT:CARD);
        text(r,x+16,y+29,16,TEXT,titles[i]); text(r,x+16,y+55,12,MUTED,values[i]);
        text(r,x+w-28,y+41,18,ACCENT,selected?"-":"+");
        if(selected) text(r,x+16,y+89,12,ACCENT,details[i]);
        hit(u,(Rect){x,y,w,h},ACT_SETTING,i); y+=h+10;
    }
    return y-start+12;
}

void ui_draw(Ui *u,Renderer *r,double now) {
    Rect a=u->area,b=u->body; u->hit_count=0;
    const float maxw=900;
    if(b.w>maxw) { b.x+=(b.w-maxw)/2; b.w=maxw; }
    text(r,a.x+22,a.y+27,11,ACCENT,"LX MUSIC C");
    text(r,a.x+20,a.y+(a.h<380?56:64),27,TEXT,TABS[u->state.tab]);
    if(a.w>340) text(r,a.x+a.w-115,a.y+27,11,MUTED,"本地音乐空间");
    float *scroll=&u->state.scroll[u->state.tab];
    render_clip(r,u->body);
    float extent=0,y=b.y-*scroll;
    switch(u->state.tab) {
        case 0: extent=library(u,r,b.x,y,b.w); break;
        case 1: extent=search(u,r,b.x,y,b.w); break;
        case 2: extent=player(u,r,b.x,y,b.w); break;
        case 3: extent=settings(u,r,b.x,y,b.w); break;
    }
    u->max_scroll[u->state.tab]=fmaxf(0,extent-b.h);
    // A resized viewport may clamp the old offset on the next requested frame.
    if(u->max_scroll[u->state.tab]>0) {
        float thumb=fmaxf(22,b.h*b.h/extent);
        float sy=b.y+(*scroll/u->max_scroll[u->state.tab])*(b.h-thumb);
        render_rect(r,(Rect){u->body.x+u->body.w-3,sy,3,thumb},2,LINE);
    }
    render_unclip(r);
    float nav_y=a.y+a.h-68, item_w=a.w/4;
    render_rect(r,(Rect){a.x,nav_y-8,a.w,1},0,LINE);
    for(int i=0;i<4;++i) {
        Rect item={a.x+i*item_w,nav_y,item_w,60}; bool active=u->state.tab==i;
        if(active) render_rect(r,(Rect){item.x+8,item.y+1,item.w-16,56},16,TINT);
        const char *icons[]={"♪","?","▶","≡"};
        center(r,(Rect){item.x,item.y+3,item.w,24},19,active?ACCENT:MUTED,icons[i]);
        center(r,(Rect){item.x,item.y+30,item.w,23},12,active?TEXT:MUTED,TABS[i]);
        hit(u,item,ACT_TAB,i);
    }
    if(u->toast_until<=now) u->toast_until=0;
    if(u->toast_until>now) {
        float tw=fminf(a.w-32,render_text_width(u->toast,12)+28);
        Rect t={a.x+(a.w-tw)/2,nav_y-62,tw,44};
        render_rect(r,t,14,LINE); center(r,t,12,TEXT,u->toast);
    }
    render_flush(r);
}

bool ui_tick(Ui *u,float dt) {
    if(u->max_scroll[u->state.tab]<0) return false;
    float *s=&u->state.scroll[u->state.tab];
    float clamped=clamp(*s,0,u->max_scroll[u->state.tab]);
    bool dirty=clamped!=*s; *s=clamped;
    if(dt<=0) return dirty;
    if(!u->pressed && fabsf(u->velocity)>3) {
        float prev=*s; *s=clamp(*s+u->velocity*dt,0,u->max_scroll[u->state.tab]);
        u->velocity*=expf(-7*dt);
        if(prev==*s) u->velocity=0;
        dirty=true;
    }
    return dirty;
}

void ui_touch(Ui *u,int action,float x,float y,double now) {
    if(action==0) {
        u->pressed=true; u->dragging=false; u->velocity=0;
        u->down_x=x; u->down_y=u->last_y=y; u->last_time=now; return;
    }
    if(!u->pressed) return;
    if(action==2) {
        if(hypotf(x-u->down_x,y-u->down_y)>8) u->dragging=true;
        if(u->dragging && inside(u->body,u->down_x,u->down_y)) {
            float delta=u->last_y-y;
            float dt=(float)(now-u->last_time);
            u->state.scroll[u->state.tab]=clamp(u->state.scroll[u->state.tab]+delta,0,u->max_scroll[u->state.tab]);
            if(dt>.001f) u->velocity=clamp(delta/dt,-2200,2200);
        }
        u->last_y=y; u->last_time=now; return;
    }
    if(action!=1) { ui_cancel(u); return; }
    u->pressed=false;
    if(u->dragging) { if(now-u->last_time>.12) u->velocity=0; return; }
    for(int i=u->hit_count-1;i>=0;--i) {
        UiHit h=u->hits[i];
        if(!inside(h.rect,x,y) || !inside(h.rect,u->down_x,u->down_y)) continue;
        bool nav=h.rect.y>=u->area.y+u->area.h-68;
        if(!nav && (!inside(u->body,x,y)||!inside(u->body,u->down_x,u->down_y))) continue;
        switch(h.action) {
            case ACT_TAB: u->state.tab=h.value; u->velocity=0; break;
            case ACT_FILTER: u->state.library_filter=h.value; break;
            case ACT_CHIP: u->state.search_chip=u->state.search_chip==h.value?-1:h.value; break;
            case ACT_SETTING: u->state.settings_row=u->state.settings_row==h.value?-1:h.value; break;
            case ACT_NOTICE: notice(u,h.value?"搜索输入将在后续开放":"音乐库功能将在后续开放",now); break;
        }
        break;
    }
}

bool ui_back(Ui *u) {
    ui_cancel(u);
    if(u->toast_until>0) { u->toast_until=0; return true; }
    if(u->state.tab==3 && u->state.settings_row>=0) { u->state.settings_row=-1; return true; }
    if(u->state.tab!=0) { u->state.tab=0; return true; }
    return false;
}
