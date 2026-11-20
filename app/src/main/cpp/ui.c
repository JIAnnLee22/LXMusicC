#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const Color TEXT={.90f,.93f,.97f,1}, MUTED={.53f,.60f,.68f,1};
static const Color CARD={.075f,.094f,.12f,1}, LINE={.13f,.17f,.21f,1};
static const Color ACCENT={.34f,.88f,.72f,1}, TINT={.08f,.20f,.19f,1};
static const char *TABS[]={"歌曲","搜索","正在播放","设置"};
static const char *FILTERS[]={"收藏","最近播放","我的歌单"};
enum { ACT_TAB=1, ACT_FILTER, ACT_CHIP, ACT_SETTING, ACT_NOTICE,
    ACT_SOURCE_OPEN, ACT_SOURCE_BACK, ACT_SOURCE_PASTE, ACT_SOURCE_PAGE,
    ACT_SOURCE_TOGGLE, ACT_SOURCE_DELETE, ACT_SOURCE_UP, ACT_SOURCE_DOWN, ACT_SOURCE_CHECK,
    ACT_SEARCH_INPUT, ACT_SEARCH_SUBMIT, ACT_SEARCH_PASTE, ACT_SEARCH_CANCEL,
    ACT_SEARCH_RETRY, ACT_SEARCH_PAGE, ACT_SEARCH_SELECT, ACT_SEARCH_KEY, ACT_SEARCH_CLOSE };
static float wrapped(Renderer *r,float x,float y,float w,float size,Color color,const char *s);

static bool inside(Rect b,float x,float y) { return x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h; }
static float clamp(float x,float a,float b) { return fminf(fmaxf(x,a),b); }
static void hit(Ui *u,Rect b,int action,int value) {
    if(u->hit_count<128) u->hits[u->hit_count++]=(UiHit){b,action,value};
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

/* 外部歌名仅在可用宽度内显示，不能画到按钮/安全区之外。 */
static void fitted(Renderer *r,float x,float y,float w,float size,Color color,const char *s) {
    char line[1028]; snprintf(line,sizeof(line),"%s",s);
    size_t n=strlen(line);
    if(render_text_width(line,size)>w) {
        while(n) {
            do { --n; } while(n && ((unsigned char)line[n]&0xc0)==0x80);
            line[n]=0;
            if(render_text_width(line,size)+render_text_width("...",size)<=w) break;
        }
        strcat(line,"...");
    }
    text(r,x,y,size,color,line);
}
static void submit(Ui *u,double now) {
    if(search_query(u->query)) {
        u->input_open=false; u->state.scroll[1]=0; u->velocity=0;
    } else notice(u,"请输入关键词，或稍后重试",now);
}
bool ui_search_key(Ui *u,int ch,double now) {
    if(u->state.tab!=1 || !u->input_open) return false;
    size_t n=strlen(u->query);
    if(ch==8) {
        if(n) { do { --n; } while(n && ((unsigned char)u->query[n]&0xc0)==0x80); u->query[n]=0; }
    } else if(ch==13) submit(u,now);
    else if(ch==127) u->query[0]=0;
    else if(ch>=32 && ch<=126 && n+1<sizeof(u->query)) { u->query[n]=(char)ch; u->query[n+1]=0; }
    return true;
}
void ui_search_sync(Ui *u) {
    if(u->paste_revision!=u->search.paste_revision) {
        u->paste_revision=u->search.paste_revision;
        snprintf(u->query,sizeof(u->query),"%s",u->search.pasted);
        u->state.scroll[1]=0;
    }
}
static float keyboard(Ui *u,Renderer *r,float x,float y,float w) {
    const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm'-"};
    for(int row=0;row<4;++row) {
        size_t n=strlen(rows[row]); float kw=(w-4*(n-1))/n;
        for(size_t k=0;k<n;++k) {
            char label[2]={rows[row][k],0};
            button(u,r,(Rect){x+k*(kw+4),y,kw,30},label,ACT_SEARCH_KEY,rows[row][k],false);
        }
        y+=35;
    }
    float kw=(w-18)/4;
    const char *labels[]={"空格","退格","清空","收起"};
    const int keys[]={32,8,127,0};
    for(int k=0;k<4;++k) button(u,r,(Rect){x+k*(kw+6),y,kw,32},labels[k],
        k==3?ACT_SEARCH_CLOSE:ACT_SEARCH_KEY,keys[k],false);
    return y+44;
}
static float search(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; SearchView *v=&u->search;
    Rect input={x,y,w-82,50};
    render_rect(r,input,14,u->input_open?TINT:CARD);
    fitted(r,x+12,y+31,input.w-24,14,u->query[0]?TEXT:MUTED,u->query[0]?u->query:"输入歌曲、歌手、专辑");
    hit(u,input,ACT_SEARCH_INPUT,0);
    button(u,r,(Rect){x+w-74,y,74,50},v->busy?"重搜":"搜索",ACT_SEARCH_SUBMIT,0,true); y+=60;
    float bw=(w-16)/3;
    button(u,r,(Rect){x,y,bw,34},"粘贴关键词",ACT_SEARCH_PASTE,0,false);
    button(u,r,(Rect){x+bw+8,y,bw,34},v->busy?"取消":"重试",v->busy?ACT_SEARCH_CANCEL:ACT_SEARCH_RETRY,0,false);
    button(u,r,(Rect){x+2*(bw+8),y,bw,34},"音源管理",ACT_SOURCE_OPEN,0,false); y+=46;
    if(u->input_open) {
        y=keyboard(u,r,x,y,w);
        y=wrapped(r,x,y,w,12,MUTED,"字母数字可直接输入；中文请复制后粘贴"); y+=10;
    }
    if(v->keyword[0]) {
        char query[300]; snprintf(query,sizeof(query),"搜索：%s",v->keyword);
        fitted(r,x,y+16,w,14,TEXT,query); y+=28;
    }
    y=wrapped(r,x,y,w,13,ACCENT,v->message); y+=10;
    if(v->error[0]) { y=wrapped(r,x,y,w,12,MUTED,v->error); y+=12; }
    if(!v->total) {
        int enabled=0;
        for(int i=0;i<u->sources.count;++i) if(u->sources.items[i].enabled) ++enabled;
        render_rect(r,(Rect){x,y,w,106},18,CARD);
        center(r,(Rect){x,y+10,w,36},22,ACCENT,v->busy?"…":"♪");
        center(r,(Rect){x,y+53,w,32},14,TEXT,v->busy?"正在搜索，可随时取消":
            !enabled?"暂无启用音源，请先导入并启用":v->keyword[0]?"暂无结果，可重试或换个关键词":"准备好后点击搜索");
        return y+122-start;
    }
    char page[96]; snprintf(page,sizeof(page),"%d 首 · 第 %d / %d 页",v->total,v->page+1,(v->total+SEARCH_PAGE_SIZE-1)/SEARCH_PAGE_SIZE);
    fitted(r,x,y+17,w,13,TEXT,page); y+=28;
    button(u,r,(Rect){x,y,(w-12)/2,32},"上一页",ACT_SEARCH_PAGE,-1,false);
    button(u,r,(Rect){x+(w+12)/2,y,(w-12)/2,32},"下一页",ACT_SEARCH_PAGE,1,false); y+=44;
    for(int i=0;i<v->count;++i) {
        SearchItem *item=&v->items[i]; SourceSong *s=&item->song;
        Rect card={x,y,w,110}; render_rect(r,card,16,CARD); hit(u,card,ACT_SEARCH_SELECT,i);
        fitted(r,x+14,y+27,w-28,17,TEXT,s->name);
        fitted(r,x+14,y+51,w-28,13,MUTED,s->artist);
        char details[180]; int duration=(int)s->duration;
        snprintf(details,sizeof(details),"%d:%02d  /  %s",duration/60,duration%60,s->quality);
        fitted(r,x+14,y+75,w-28,12,ACCENT,details);
        fitted(r,x+14,y+96,w-28,11,MUTED,item->source_name); y+=120;
    }
    return y-start+10;
}

static float player(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; bool wide=w>600;
    float cover=wide?fminf(220,w*.34f):fminf(224,w-32);
    float cx=wide?x:x+(w-cover)/2;
    render_rect(r,(Rect){cx,y,cover,cover},28,TINT);
    render_rect(r,(Rect){cx+cover*.22f,y+cover*.22f,cover*.56f,cover*.56f},cover*.28f,LINE);
    center(r,(Rect){cx,y,cover,cover},58,ACCENT,"♪");
    float dx=wide?x+cover+36:x, dy=wide?y+8:y+cover+20, dw=wide?w-cover-36:w;
    if(u->has_selected) {
        fitted(r,dx+12,dy+28,dw-24,22,TEXT,u->selected.song.name);
        fitted(r,dx+12,dy+63,dw-24,14,MUTED,u->selected.song.artist);
        fitted(r,dx+12,dy+84,dw-24,11,ACCENT,u->selected.song.quality);
    } else {
        center(r,(Rect){dx,dy,dw,36},22,TEXT,"还没有正在播放的歌曲");
        center(r,(Rect){dx,dy+42,dw,24},13,MUTED,"选择音乐后，播放信息会显示在这里");
    }
    render_rect(r,(Rect){dx+12,dy+92,dw-24,3},2,LINE);
    text(r,dx+12,dy+119,11,MUTED,"--:--");
    char duration[24]="--:--";
    if(u->has_selected) snprintf(duration,sizeof(duration),"%d:%02d",(int)u->selected.song.duration/60,(int)u->selected.song.duration%60);
    text(r,dx+dw-60,dy+119,11,MUTED,duration);
    button(u,r,(Rect){dx+(dw-148)/2,dy+144,148,44},"前往搜索",ACT_TAB,1,true);
    fitted(r,dx+12,dy+224,dw-24,12,MUTED,"音频输出将在前台播放服务开放");
    if(u->has_selected) fitted(r,dx+12,dy+246,dw-24,11,MUTED,u->selected.source_name);
    return fmaxf(y+cover,dy+266)-start+20;
}

// UTF-8 按可用宽度换行，外部元数据不参与格式化或执行。
static float wrapped(Renderer *r,float x,float y,float w,float size,Color color,const char *s) {
    char line[1028]; size_t n=0;
    for(const char *p=s;;) {
        unsigned char c=(unsigned char)*p;
        size_t step=c<128?1:c>=0xc2&&c<224?2:c>=224&&c<240?3:c>=240&&c<245?4:1;
        for(size_t k=1;k<step;++k) if(!p[k] || ((unsigned char)p[k]&0xc0)!=0x80) { step=1; break; }
        if(!c || c=='\n' || n+step>=sizeof(line)) {
            line[n]=0; text(r,x,y+size,size,color,line); y+=size+7; n=0;
            if(!c) break;
            if(c=='\n') { ++p; continue; }
        }
        memcpy(line+n,p,step); line[n+step]=0;
        if(n && render_text_width(line,size)>w) {
            line[n]=0; text(r,x,y+size,size,color,line); y+=size+7; n=0;
        }
        memcpy(line+n,p,step); n+=step; p+=step;
    }
    return y;
}
static float sources(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y;
    button(u,r,(Rect){x,y,88,36},"返回设置",ACT_SOURCE_BACK,0,false);
    button(u,r,(Rect){x+100,y,w-100,36},u->sources.busy?"处理中…":"粘贴导入",ACT_SOURCE_PASTE,0,!u->sources.busy);
    y+=50;
    y=wrapped(r,x,y,w,12,MUTED,"复制 HTTP(S) 链接、JSON 或 JS 文件内容");
    y=wrapped(r,x,y+4,w,13,ACCENT,u->sources.message); y+=14;
    if(!u->sources.count) {
        render_rect(r,(Rect){x,y,w,100},16,CARD);
        center(r,(Rect){x,y+12,w,30},18,TEXT,"还没有音源");
        center(r,(Rect){x,y+50,w,28},12,MUTED,"导入只保存脚本，不会执行代码");
        return y+120-start;
    }
    const int page_size=3;
    int pages=(u->sources.count+page_size-1)/page_size;
    if(u->source_page>=pages) u->source_page=pages-1;
    char page[64]; snprintf(page,sizeof(page),"音源 %d 个  /  第 %d / %d 页",u->sources.count,u->source_page+1,pages);
    text(r,x,y+16,14,TEXT,page); y+=32;
    if(pages>1) {
        button(u,r,(Rect){x,y,(w-12)/2,34},"上一页",ACT_SOURCE_PAGE,-1,false);
        button(u,r,(Rect){x+(w+12)/2,y,(w-12)/2,34},"下一页",ACT_SOURCE_PAGE,1,false); y+=46;
    }
    for(int i=u->source_page*page_size;i<u->sources.count && i<(u->source_page+1)*page_size;++i) {
        SourceMeta *m=&u->sources.items[i];
        render_rect(r,(Rect){x,y,w,3},1,LINE); y+=16;
        y=wrapped(r,x+4,y,w-8,18,TEXT,m->name);
        const char *labels[]={"描述","版本","作者","主页"};
        const char *values[]={m->description,m->version,m->author,m->homepage};
        for(int k=0;k<4;++k) {
            char detail[1100]; snprintf(detail,sizeof(detail),"%s: %s",labels[k],values[k][0]?values[k]:"未提供");
            y=wrapped(r,x+4,y+5,w-8,12,MUTED,detail);
        }
        y+=10; float bw=(w-18)/4;
        button(u,r,(Rect){x,y,bw,36},m->enabled?"已启用":"已禁用",ACT_SOURCE_TOGGLE,(int)m->id,m->enabled);
        button(u,r,(Rect){x+bw+6,y,bw,36},"上移",ACT_SOURCE_UP,(int)m->id,false);
        button(u,r,(Rect){x+2*(bw+6),y,bw,36},"下移",ACT_SOURCE_DOWN,(int)m->id,false);
        button(u,r,(Rect){x+3*(bw+6),y,bw,36},u->delete_id==m->id?"确认删除":"删除",ACT_SOURCE_DELETE,(int)m->id,false);
        y+=46;
        button(u,r,(Rect){x,y,w,36},"搜索自检（love）",ACT_SOURCE_CHECK,(int)m->id,false);
        y+=58;
    }
    return y-start;
}
static float settings(Ui *u,Renderer *r,float x,float y,float w) {
    if(u->source_open) return sources(u,r,x,y,w);
    float start=y;
    render_rect(r,(Rect){x,y,w,90},20,TINT);
    text(r,x+18,y+34,20,TEXT,"LX Music C"); text(r,x+18,y+63,13,ACCENT,"0.1.0  /  音源管理"); y+=112;
    const char *titles[]={"音源管理","缓存管理","仅 Wi-Fi 下载","主题外观","后台播放","数据与隐私","关于"};
    char source_count[64]; snprintf(source_count,sizeof(source_count),"%d 个音源 / 点击管理",u->sources.count);
    const char *values[]={source_count,"暂无缓存","尚未开放","深色 / 薄荷绿","尚未开放","本地优先","音源导入"};
    const char *details[]={"粘贴导入与音源管理","公共音乐目录缓存将在后续开放","下载功能尚未开放","当前提供固定深色主题","播放服务与通知将在后续开放","导入不执行；搜索与自检运行可信音源并联网","纯 C 界面 · Android 8.0 及以上"};
    for(int i=0;i<7;++i) {
        bool selected=u->state.settings_row==i;
        float h=selected?112:78;
        render_rect(r,(Rect){x,y,w,h},16,selected?TINT:CARD);
        text(r,x+16,y+29,16,TEXT,titles[i]); text(r,x+16,y+55,12,MUTED,values[i]);
        text(r,x+w-28,y+41,18,ACCENT,selected?"-":"+");
        if(selected) text(r,x+16,y+89,12,ACCENT,details[i]);
        hit(u,(Rect){x,y,w,h},i==0?ACT_SOURCE_OPEN:ACT_SETTING,i); y+=h+10;
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
        if(h.action!=ACT_SOURCE_DELETE) u->delete_id=0;
        switch(h.action) {
            case ACT_TAB: u->state.tab=h.value; u->velocity=0; break;
            case ACT_FILTER: u->state.library_filter=h.value; break;
            case ACT_CHIP: u->state.search_chip=u->state.search_chip==h.value?-1:h.value; break;
            case ACT_SETTING: u->state.settings_row=u->state.settings_row==h.value?-1:h.value; break;
            case ACT_NOTICE: notice(u,h.value?"搜索输入将在后续开放":"音乐库功能将在后续开放",now); break;
            case ACT_SOURCE_OPEN: u->state.tab=3; u->source_open=true; u->state.scroll[3]=0; u->delete_id=0; break;
            case ACT_SEARCH_INPUT: u->input_open=!u->input_open; u->state.scroll[1]=0; break;
            case ACT_SEARCH_SUBMIT: submit(u,now); break;
            case ACT_SEARCH_PASTE: u->search_paste_requested=true; break;
            case ACT_SEARCH_CANCEL: search_cancel(); break;
            case ACT_SEARCH_RETRY:
                if(u->search.keyword[0]) snprintf(u->query,sizeof(u->query),"%s",u->search.keyword);
                submit(u,now); break;
            case ACT_SEARCH_PAGE: search_page(h.value); u->state.scroll[1]=0; u->velocity=0; break;
            case ACT_SEARCH_KEY: ui_search_key(u,h.value,now); break;
            case ACT_SEARCH_CLOSE: u->input_open=false; break;
            case ACT_SEARCH_SELECT:
                if(h.value>=0 && h.value<u->search.count) {
                    u->selected=u->search.items[h.value]; u->has_selected=true;
                    u->state.tab=2; u->state.scroll[2]=0; u->velocity=0;
                }
                break;
            case ACT_SOURCE_BACK: u->source_open=false; u->state.scroll[3]=0; break;
            case ACT_SOURCE_PASTE: if(!u->sources.busy) u->paste_requested=true; break;
            case ACT_SOURCE_PAGE:
                u->source_page=(int)clamp(u->source_page+h.value,0,fmaxf(0,(u->sources.count-1)/3));
                u->state.scroll[3]=0; u->delete_id=0; break;
            case ACT_SOURCE_TOGGLE: source_command(SOURCE_TOGGLE,(uint32_t)h.value,NULL,0); break;
            case ACT_SOURCE_UP: source_command(SOURCE_UP,(uint32_t)h.value,NULL,0); break;
            case ACT_SOURCE_DOWN: source_command(SOURCE_DOWN,(uint32_t)h.value,NULL,0); break;
            case ACT_SOURCE_CHECK: source_command(SOURCE_CHECK,(uint32_t)h.value,NULL,0); break;
            case ACT_SOURCE_DELETE:
                if(u->delete_id==(uint32_t)h.value) { source_command(SOURCE_DELETE,u->delete_id,NULL,0); u->delete_id=0; }
                else u->delete_id=(uint32_t)h.value;
                break;
        }
        break;
    }
}

bool ui_back(Ui *u) {
    ui_cancel(u);
    if(u->toast_until>0) { u->toast_until=0; return true; }
    if(u->state.tab==1 && u->input_open) { u->input_open=false; return true; }
    if(u->state.tab==1 && u->search.busy) { search_cancel(); return true; }
    if(u->state.tab==3 && u->source_open) { u->source_open=false; u->state.scroll[3]=0; return true; }
    if(u->state.tab==3 && u->state.settings_row>=0) { u->state.settings_row=-1; return true; }
    if(u->state.tab!=0) { u->state.tab=0; return true; }
    return false;
}
