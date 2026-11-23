#include "player_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const Color TEXT={.90f,.93f,.97f,1}, MUTED={.53f,.60f,.68f,1};
static const Color CARD={.075f,.094f,.12f,1}, LINE={.13f,.17f,.21f,1};
static float clamp(float v,float a,float b) { return fminf(fmaxf(v,a),b); }
static bool inside(Rect r,float x,float y) { return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }
static void label(Renderer *r,float x,float y,float w,float size,Color c,const char *s) {
    char line[1028]; snprintf(line,sizeof(line),"%s",s); size_t n=strlen(line);
    if(render_text_width(line,size)>w) {
        do {
            do { --n; } while(n && ((unsigned char)line[n]&0xc0)==0x80);
            line[n]=0;
        } while(n && render_text_width(line,size)+render_text_width("...",size)>w);
        strcat(line,"...");
    }
    render_text(r,x,y,size,c,line);
}
static void button(Ui *u,Renderer *r,Rect b,const char *s,int action,int value,bool active) {
    render_rect(r,b,12,active?ACCENT:TINT);
    float width=render_text_width(s,13),size=width>b.w-12?13*fmaxf(0,b.w-12)/width:13;
    render_text(r,b.x+(b.w-render_text_width(s,size))/2,b.y+b.h/2+size*.34f,size,active?CARD:ACCENT,s);
    if(u->hit_count<128) u->hits[u->hit_count++]=(UiHit){b,action,value};
}
static void clock_text(char out[32],int64_t ms) {
    if(ms<0) ms=0;
    snprintf(out,32,"%lld:%02lld",(long long)ms/60000,(long long)ms/1000%60);
}
static float queue_draw(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; y+=(1-u->queue_mix)*64;
    PlaybackQueue q; playback_queue(&q,u->queue_page); u->queue_page=q.page;
    button(u,r,(Rect){x,y,w,38},"收起播放队列 / 向下滑动",PLAYER_TOGGLE_QUEUE,0,false); y+=52;
    char info[100]; snprintf(info,sizeof(info),"播放队列 %d 首 · 第 %d / %d 页",q.total,q.page+1,
        q.total?(q.total+PLAYBACK_QUEUE_PAGE-1)/PLAYBACK_QUEUE_PAGE:1);
    label(r,x+4,y+18,w-8,15,TEXT,info); y+=32;
    button(u,r,(Rect){x,y,(w-10)/2,34},"上一页",PLAYER_QUEUE_PAGE,-1,false);
    button(u,r,(Rect){x+(w+10)/2,y,(w-10)/2,34},"下一页",PLAYER_QUEUE_PAGE,1,false); y+=46;
    for(int i=0;i<q.count;++i) {
        int index=q.page*PLAYBACK_QUEUE_PAGE+i; bool current=index==q.index;
        Rect b={x,y,w,70}; render_rect(r,b,14,current?TINT:CARD);
        if(u->hit_count<128) u->hits[u->hit_count++]=(UiHit){b,PLAYER_SELECT,index};
        snprintf(info,sizeof(info),"%s%d",current?"> ":"",index+1);
        render_text(r,x+12,y+31,13,current?ACCENT:MUTED,info);
        label(r,x+50,y+28,w-64,16,current?ACCENT:TEXT,q.items[i].song.name);
        label(r,x+50,y+52,w-64,12,MUTED,q.items[i].song.artist); y+=80;
    }
    if((q.page+1)*PLAYBACK_QUEUE_PAGE<q.total) {
        button(u,r,(Rect){x,y,w,40},"加载更多 / 下一页",PLAYER_QUEUE_PAGE,1,false); y+=52;
    }
    if(!q.total) y=render_paragraph(r,x+12,y+12,w-24,15,MUTED,"暂无播放队列，请先搜索选曲")+12;
    return y-start+12;
}
float ui_player_draw(Ui *u,Renderer *r,float x,float y,float w) {
    u->seek_rect=(Rect){0};
    if(u->queue_open || u->queue_mix>.001f) return queue_draw(u,r,x,y,w);
    float start=y; bool wide=w>600; PlaybackView *p=&u->playback;
    // 短屏也保持控制可达；窄屏内容仍可滚动，宽屏使用封面/控制双栏。
    float cover=wide?fminf(220,w*.31f):fminf(208,fminf(w-32,fmaxf(100,u->body.h*.32f)));
    float cx=wide?x:x+(w-cover)/2;
    Rect art={cx,y+(1-u->player_appear)*10,cover,cover}; u->art_rect=art;
    render_rect(r,art,24,TINT);
    render_rect(r,(Rect){art.x+cover*.22f,art.y+cover*.22f,cover*.56f,cover*.56f},cover*.28f,LINE);
    render_text(r,cx+(cover-render_text_width("♪",52))/2,art.y+cover/2+18,52,ACCENT,"♪");
    render_cover(r,art,24,u->cover_fade);
    float dx=wide?x+cover+28:x, dy=wide?y:y+cover+12, dw=wide?w-cover-28:w;
    if(!p->count) {
        label(r,dx+8,dy+28,dw-16,21,TEXT,"还没有正在播放的歌曲");
        label(r,dx+8,dy+59,dw-16,13,MUTED,"选择音乐后显示封面、进度与队列");
        button(u,r,(Rect){dx,dy+76,dw,40},"前往搜索",PLAYER_SELECT,-9,true);
        return fmaxf(y+cover,dy+128)-start+16;
    }
    label(r,dx+8,dy+26,dw-16,21,TEXT,p->current.song.name);
    label(r,dx+8,dy+52,dw-16,13,MUTED,p->current.song.artist);
    label(r,dx+8,dy+72,dw-16,11,ACCENT,p->current.song.album);
    int64_t pos=u->seeking?u->seek_position:(int64_t)u->shown_position;
    float fraction=p->duration>0?clamp((float)((double)pos/p->duration),0,1):0;
    Rect track={dx+12,dy+92,dw-24,4};
    render_rect(r,track,2,LINE);
    render_rect(r,(Rect){track.x,track.y,track.w*fraction,4},2,ACCENT);
    if(p->duration>0) {
        render_rect(r,(Rect){track.x+track.w*fraction-6,track.y-4,12,12},6,ACCENT);
        u->seek_rect=(Rect){track.x,dy+80,track.w,28};
    }
    char a[32],b[32]="--:--"; clock_text(a,pos);
    if(p->duration>0) clock_text(b,p->duration);
    render_text(r,dx+12,dy+123,12,u->seeking?ACCENT:MUTED,a);
    render_text(r,dx+dw-12-render_text_width(b,12),dy+123,12,MUTED,b);
    // 下列通用控制仍由 ui.c 下发，数值与其 ACT_PLAYBACK/SEEK 区间解耦。
    float bw=(dw-16)/3;
    button(u,r,(Rect){dx,dy+138,bw,42},"上一首",PLAYER_SELECT,-2,false);
    button(u,r,(Rect){dx+bw+8,dy+138,bw,42},p->error[0]?"重试":p->requested?"暂停":"播放",PLAYER_SELECT,-3,true);
    Rect next={dx+2*bw+16,dy+138,bw,42};
    if(p->mode==PLAYBACK_MODE_SEQUENCE && p->index+1==p->count) {
        render_rect(r,next,12,CARD);
        render_text(r,next.x+(next.w-render_text_width("队列末曲",13))/2,next.y+25,13,MUTED,"队列末曲");
    } else button(u,r,next,"下一首",PLAYER_SELECT,-4,false);
    button(u,r,(Rect){dx,dy+190,(dw-8)/2,32},"后退 10 秒",PLAYER_SELECT,-5,false);
    button(u,r,(Rect){dx+(dw+8)/2,dy+190,(dw-8)/2,32},"前进 10 秒",PLAYER_SELECT,-6,false);
    const char *modes[]={"顺序播放","列表循环","单曲循环","随机播放"};
    button(u,r,(Rect){dx,dy+232,(dw-8)/2,34},modes[p->mode],PLAYER_MODE,0,false);
    button(u,r,(Rect){dx+(dw+8)/2,dy+232,(dw-8)/2,34},"上滑展开队列",PLAYER_TOGGLE_QUEUE,0,false);
    const char *status=p->error[0]?p->error:p->playing?"正在播放":p->state==2?"正在加载…":
        p->state==4?"播放结束":p->requested?"等待音频焦点":"已暂停";
    float sy=render_paragraph(r,dx+6,dy+276,dw-12,12,ACCENT,status);
    char info[320]; snprintf(info,sizeof(info),"%d / %d · %s",p->index+1,p->count,p->current.source_name);
    label(r,dx+6,sy+14,dw-12,11,MUTED,info); sy+=26;
    // 状态按实际行数推进，窄屏长错误不与缓存按钮重叠，全部可滚动到达。
    button(u,r,(Rect){dx,sy,(dw-8)/2,34},u->cache.busy?"缓存处理中…":"缓存当前歌曲",PLAYER_SELECT,-7,false);
    button(u,r,(Rect){dx+(dw+8)/2,sy,(dw-8)/2,34},p->local_uri[0]?"离线播放 / 管理":"缓存管理",PLAYER_SELECT,-8,false); sy+=46;
    sy=render_paragraph(r,dx+6,sy,dw-12,11,ACCENT,u->cache.message);
    sy=render_paragraph(r,dx+6,sy+4,dw-12,11,MUTED,"音量键调节媒体音量，输出跟随系统"); sy+=10;
    if(p->current.song.cover[0] && !r->cover) {
        if(u->cover_loading) label(r,dx+6,sy+20,dw-12,11,MUTED,"正在加载封面…");
        else button(u,r,(Rect){dx,sy,dw,30},"暂无封面 / 点击重试",PLAYER_RETRY_COVER,0,false);
        sy+=38;
    }
    return fmaxf(y+cover,sy)-start+16;
}
void ui_player_sync(Ui *u) {
    PlaybackView *p=&u->playback;
    if(u->player_generation!=p->generation) {
        u->player_generation=p->generation; u->player_appear=0;
        u->queue_page=p->index/PLAYBACK_QUEUE_PAGE;
        // 切歌使未提交的手势失效，不用旧时长 seek 到下一首。
        if(u->seeking || (u->state.tab==2 && u->pressed)) ui_cancel(u);
        u->seek_rect=(Rect){0}; u->seek_position=0;
    }
    u->shown_position=p->position;
}
static void preview(Ui *u,float x) {
    double fraction=clamp((x-u->seek_rect.x)/u->seek_rect.w,0,1);
    u->seek_position=(int64_t)llround(fraction*u->playback.duration);
}
bool ui_player_touch(Ui *u,int action,float x,float y,double now) {
    (void)now;
    if(u->state.tab!=2) return false;
    if(action==0 && !u->queue_open && u->queue_mix<.001f && u->playback.duration>0 &&
       inside(u->body,x,y) && inside(u->seek_rect,x,y)) {
        u->seeking=true; u->pressed=true; u->dragging=false; u->velocity=0;
        u->seek_generation=u->playback.generation; preview(u,x); return true;
    }
    if(!u->seeking) return false;
    if(action==2) preview(u,x);
    if(action==1) {
        preview(u,x);
        if(u->seek_generation==u->playback.generation && u->playback.duration>0)
            playback_command(PLAYBACK_SEEK,u->seek_position);
        u->shown_position=u->seek_position; u->seeking=false; u->pressed=false;
    } else if(action!=2) ui_cancel(u);
    return true;
}
void ui_player_action(Ui *u,int action,int value) {
    switch(action) {
        case PLAYER_TOGGLE_QUEUE:
            u->queue_open=!u->queue_open;
            u->queue_page=u->playback.index/PLAYBACK_QUEUE_PAGE;
            u->state.scroll[2]=0; u->velocity=0; break;
        case PLAYER_QUEUE_PAGE: {
            PlaybackQueue q; playback_queue(&q,u->queue_page+value); u->queue_page=q.page;
            u->state.scroll[2]=0; u->velocity=0; break;
        }
        case PLAYER_MODE: playback_command(PLAYBACK_MODE,(u->playback.mode+1)%4); break;
        case PLAYER_RETRY_COVER: u->cover_retry_requested=true; break;
        case PLAYER_SELECT:
            if(value>=0) {
                playback_command(PLAYBACK_SELECT,value); u->queue_open=false; u->queue_mix=0;
                u->state.scroll[2]=0;
            } else switch(value) {
                case -2: playback_command(PLAYBACK_PREVIOUS,0); break;
                case -3: playback_command(u->playback.requested&&!u->playback.error[0]?PLAYBACK_PAUSE:PLAYBACK_PLAY,0); break;
                case -4: playback_command(PLAYBACK_NEXT,0); break;
                case -5: playback_command(PLAYBACK_SEEK,(int64_t)u->shown_position-10000); break;
                case -6: playback_command(PLAYBACK_SEEK,(int64_t)u->shown_position+10000); break;
                case -7: cache_command(CACHE_DOWNLOAD,&u->playback.current,0,0); break;
                case -8: u->state.tab=3; u->source_open=false; u->cache_open=true; u->state.scroll[3]=0; break;
                case -9: u->state.tab=1; break;
            }
            if(value>=0 || (value<=-2 && value>=-4)) u->playback_requested=true;
            break;
    }
}
bool ui_player_tick(Ui *u,float dt) {
    if(u->state.tab!=2 || dt<=0) return false;
    bool dirty=false;
    float *values[]={&u->player_appear,&u->cover_fade,&u->queue_mix};
    const float targets[]={1,1,u->queue_open?1:0};
    for(int i=0;i<3;++i) {
        float old=*values[i], step=dt*5;
        *values[i]=old<targets[i]?fminf(targets[i],old+step):fmaxf(targets[i],old-step);
        if(old!=*values[i]) dirty=true;
    }
    if(u->playback.playing && !u->seeking && u->playback.duration>0) {
        u->shown_position=fmin(u->playback.duration,u->shown_position+dt*1000); dirty=true;
    }
    return dirty;
}
bool ui_player_animating(const Ui *u) {
    return u->state.tab==2 && (u->player_appear<1 || u->cover_fade<1 ||
        u->queue_mix!=(u->queue_open?1.f:0.f) || (u->playback.playing && u->playback.duration>0));
}
