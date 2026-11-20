#include "ui.h"
#include "player_ui.h"
#include "library_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const Color TEXT={.90f,.93f,.97f,1}, MUTED={.53f,.60f,.68f,1};
static const Color CARD={.075f,.094f,.12f,1}, LINE={.13f,.17f,.21f,1};
static const char *TABS[]={"歌曲","搜索","正在播放","设置"};
enum { ACT_TAB=1, ACT_FILTER, ACT_CHIP, ACT_SETTING, ACT_NOTICE,
    ACT_SOURCE_OPEN, ACT_SOURCE_BACK, ACT_SOURCE_PASTE, ACT_SOURCE_PAGE,
    ACT_SOURCE_TOGGLE, ACT_SOURCE_DELETE, ACT_SOURCE_UP, ACT_SOURCE_DOWN, ACT_SOURCE_CHECK,
    ACT_SEARCH_INPUT, ACT_SEARCH_SUBMIT, ACT_SEARCH_PASTE, ACT_SEARCH_CANCEL,
    ACT_SEARCH_RETRY, ACT_SEARCH_PAGE, ACT_SEARCH_SELECT, ACT_SEARCH_KEY, ACT_SEARCH_CLOSE,
    ACT_PLAYBACK, ACT_PLAYBACK_SEEK, ACT_CACHE_OPEN, ACT_CACHE_BACK, ACT_CACHE_DOWNLOAD,
    ACT_CACHE_PAGE, ACT_CACHE_PLAY, ACT_CACHE_DELETE, ACT_CACHE_CLEAR, ACT_CACHE_LIMIT, ACT_CACHE_REFRESH,
    ACT_PREF_WIFI, ACT_PREF_THEME, ACT_PREF_RETRY };
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
    float width=render_text_width(s,size);
    if(width>b.w-12 && width>0) size*=fmaxf(0,b.w-12)/width;
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
    u->library_id=u->state.library_filter==2?-1:u->state.library_filter;
}
void ui_cancel(Ui *u) { u->pressed=u->dragging=u->seeking=false; u->velocity=0; u->pointer=-1; }
void ui_layout(Ui *u,Rect area) {
    u->area=area;
    float header=area.h<380?66:92;
    float padding=area.w<360?12:20;
    u->body=(Rect){area.x+padding,area.y+header,fmaxf(0,area.w-2*padding),fmaxf(0,area.h-header-76)};
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
        if(!v->busy) hit(u,(Rect){x,y,w,106},enabled?ACT_SEARCH_RETRY:ACT_SOURCE_OPEN,0);
        center(r,(Rect){x,y+10,w,36},22,ACCENT,v->busy?"…":"♪");
        fitted(r,x+12,y+76,w-24,13,TEXT,v->busy?"正在搜索，可随时取消":
            !enabled?"暂无启用音源，请先导入并启用":v->keyword[0]?"暂无结果，可重试或换个关键词":"准备好后点击搜索");
        if(v->busy) {
            render_rect(r,(Rect){x+12,y+90,w*.60f,4},2,LINE);
            render_rect(r,(Rect){x+12,y+98,w*.38f,4},2,LINE);
        }
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

// UTF-8 按可用宽度换行，外部元数据不参与格式化或执行。
static float wrapped(Renderer *r,float x,float y,float w,float size,Color color,const char *s) {
    return render_paragraph(r,x+2,y,w-4,size,color,s);
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
        center(r,(Rect){x,y+12,w,30},18,TEXT,u->sources.busy?"正在加载 / 导入音源…":"还没有音源");
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
static float cache_limits(Ui *u,Renderer *r,float x,float y,float w) {
    const int limits[]={256,512,1024,2048};
    int columns=w<360?2:4; float bw=(w-6*(columns-1))/columns;
    for(int i=0;i<4;++i) {
        char label[24]; snprintf(label,sizeof(label),"%d MiB",limits[i]);
        button(u,r,(Rect){x+(i%columns)*(bw+6),y+(i/columns)*42,bw,34},label,
            ACT_CACHE_LIMIT,limits[i],u->cache.limit==(uint64_t)limits[i]*CACHE_MIB);
    }
    return y+(4/columns)*42+2;
}
static float caches(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; CacheView *v=&u->cache;
    button(u,r,(Rect){x,y,88,36},"返回设置",ACT_CACHE_BACK,0,false);
    button(u,r,(Rect){x+100,y,w-100,36},v->busy?"处理中…":"检查文件",ACT_CACHE_REFRESH,0,false); y+=48;
    char stats[128]; snprintf(stats,sizeof(stats),"%d 首 / %.1f MiB / 上限 %llu MiB",v->total,
        (double)v->bytes/CACHE_MIB,(unsigned long long)(v->limit/CACHE_MIB));
    y=wrapped(r,x,y,w,14,TEXT,stats);
    y=wrapped(r,x,y+6,w,12,ACCENT,v->message); y+=14;
    y=wrapped(r,x,y,w,12,MUTED,"Music/LXMusicC/音源/歌手 - 歌名；点击歌曲离线播放"); y+=12;
    y=cache_limits(u,r,x,y,w);
    button(u,r,(Rect){x,y,w,34},u->cache_clear_confirm?"确认清空全部缓存":"清空缓存",ACT_CACHE_CLEAR,0,false); y+=46;
    if(!v->total) {
        render_rect(r,(Rect){x,y,w,92},16,CARD);
        center(r,(Rect){x,y+10,w,32},18,TEXT,v->busy?"正在读取缓存…":"暂无离线缓存");
        center(r,(Rect){x,y+47,w,28},12,MUTED,"先搜索播放，再点击缓存当前歌曲");
        return y+110-start;
    }
    snprintf(stats,sizeof(stats),"第 %d / %d 页",v->page+1,(v->total+CACHE_PAGE_SIZE-1)/CACHE_PAGE_SIZE);
    text(r,x,y+17,13,TEXT,stats); y+=28;
    button(u,r,(Rect){x,y,(w-10)/2,32},"上一页",ACT_CACHE_PAGE,-1,false);
    button(u,r,(Rect){x+(w+10)/2,y,(w-10)/2,32},"下一页",ACT_CACHE_PAGE,1,false); y+=44;
    for(int i=0;i<v->count;++i) {
        CacheEntry *e=&v->items[i];
        Rect card={x,y,w,104}; render_rect(r,card,14,CARD);
        hit(u,card,ACT_CACHE_PLAY,i);
        fitted(r,x+12,y+26,w-24,16,TEXT,e->item.song.name);
        fitted(r,x+12,y+49,w-24,12,MUTED,e->item.song.artist);
        snprintf(stats,sizeof(stats),"%.2f MiB · %s",(double)e->bytes/CACHE_MIB,e->item.song.quality);
        fitted(r,x+12,y+78,w-108,11,ACCENT,stats);
        button(u,r,(Rect){x+w-88,y+62,76,30},u->cache_delete_id==e->id?"确认删除":"删除",ACT_CACHE_DELETE,(int)e->id,false);
        y+=114;
    }
    return y-start+10;
}
static float settings(Ui *u,Renderer *r,float x,float y,float w) {
    if(u->cache_open) return caches(u,r,x,y,w);
    if(u->source_open) return sources(u,r,x,y,w);
    float start=y;
    render_rect(r,(Rect){x,y,w,82},20,TINT);
    text(r,x+18,y+32,20,TEXT,"LX Music C"); text(r,x+18,y+60,13,ACCENT,"1.0.0 / 设置"); y+=96;
    y=wrapped(r,x,y,w,12,ACCENT,u->settings.message); y+=10;
    button(u,r,(Rect){x,y,w,32},u->settings.busy?"正在保存…":"重试保存设置",ACT_PREF_RETRY,0,false); y+=44;
    const char *titles[]={"音源管理","缓存管理","仅 Wi-Fi 下载","主题色","后台播放","数据与隐私","关于"};
    const char *themes[]={"深色 / 薄荷绿","深色 / 天空蓝","深色 / 淡紫色"};
    char source_count[64]; snprintf(source_count,sizeof(source_count),"%d 个音源 / 点击管理",u->sources.count);
    char cache_count[96]; snprintf(cache_count,sizeof(cache_count),"%d 首 / %.1f MiB / 上限 %llu MiB",u->cache.total,
        (double)u->cache.bytes/CACHE_MIB,(unsigned long long)(u->cache.limit/CACHE_MIB));
    const char *values[]={source_count,cache_count,u->settings.wifi_only?"已开启 / 点击关闭":"已关闭 / 点击开启",
        themes[u->settings.theme],"播放服务与通知","本地优先","LXMusicC 1.0.0 / Android 8.0+"};
    const char *details[]={"粘贴导入与音源管理","公共 Music 目录 / 离线播放", "", "",
        "支持通知栏、锁屏与耳机控制；输出跟随系统；来电暂时打断可恢复",
        "数据保存在本机；音源导入不执行，搜索和自检会运行可信脚本并联网；清除应用数据会丢失列表与设置",
        "纯 C 音乐播放器，兼容部分 LX 自定义音源；示例只提供短预览。字体 Noto Sans SC（OFL），QuickJS（MIT），stb_image（MIT / 公有领域）。仅运行可信音源"};
    for(int i=0;i<7;++i) {
        bool selected=u->state.settings_row==i;
        render_rect(r,(Rect){x,y,w,78},16,selected?TINT:CARD);
        text(r,x+16,y+29,16,TEXT,titles[i]); fitted(r,x+16,y+55,w-56,12,MUTED,values[i]);
        text(r,x+w-28,y+41,18,ACCENT,i<2?">":i==2?"~":selected?"-":"+");
        hit(u,(Rect){x,y,w,78},i==0?ACT_SOURCE_OPEN:i==1?ACT_CACHE_OPEN:i==2?ACT_PREF_WIFI:ACT_SETTING,i);
        y+=88;
        if(i==1) {
            text(r,x,y+16,13,TEXT,"缓存上限（自动保存）"); y=cache_limits(u,r,x,y+28,w);
            y=wrapped(r,x+4,y,w-8,12,MUTED,u->cache.message); y+=14;
        }
        if(i==2) {
            y=wrapped(r,x+4,y,w-8,12,MUTED,"开启后仅在已联网的非计费 Wi-Fi 缓存音频；网络切换会中止下载，可手动重试。不限制搜索、封面或在线播放"); y+=14;
        }
        if(i==3) {
            const char *names[]={"薄荷绿","天空蓝","淡紫色"}; float bw=(w-12)/3;
            for(int k=0;k<3;++k) button(u,r,(Rect){x+k*(bw+6),y,bw,36},names[k],ACT_PREF_THEME,k,u->settings.theme==k);
            y+=50;
        }
        if(selected && details[i][0]) { y=wrapped(r,x+8,y,w-16,12,ACCENT,details[i]); y+=16; }
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
        case 0: extent=ui_library_draw(u,r,b.x,y,b.w); break;
        case 1: extent=search(u,r,b.x,y,b.w); break;
        case 2:
            extent=ui_player_draw(u,r,b.x,y,b.w);
            extent+=ui_library_player(u,r,b.x,y+extent,b.w); break;
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
        render_rect(r,t,14,LINE); fitted(r,t.x+12,t.y+27,t.w-24,12,TEXT,u->toast);
    }
    render_flush(r);
}

bool ui_tick(Ui *u,float dt) {
    if(u->max_scroll[u->state.tab]<0) return false;
    float *s=&u->state.scroll[u->state.tab];
    float clamped=clamp(*s,0,u->max_scroll[u->state.tab]);
    bool dirty=clamped!=*s; *s=clamped;
    dirty=ui_player_tick(u,dt)||dirty;
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
    if(ui_player_touch(u,action,x,y,now)) return;
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
    if(u->dragging) {
        float dy=y-u->down_y, dx=x-u->down_x;
        if(u->state.tab==2 && fabsf(dy)>56 && fabsf(dx)<fabsf(dy)*.7f &&
           inside(u->body,u->down_x,u->down_y) &&
           ((!u->queue_open && dy<0 && inside(u->art_rect,u->down_x,u->down_y)) ||
            (u->queue_open && dy>0 && u->down_y<u->body.y+80)))
            ui_player_action(u,PLAYER_TOGGLE_QUEUE,0);
        if(now-u->last_time>.12) u->velocity=0;
        return;
    }
    for(int i=u->hit_count-1;i>=0;--i) {
        UiHit h=u->hits[i];
        if(!inside(h.rect,x,y) || !inside(h.rect,u->down_x,u->down_y)) continue;
        bool nav=h.rect.y>=u->area.y+u->area.h-68;
        if(!nav && (!inside(u->body,x,y)||!inside(u->body,u->down_x,u->down_y))) continue;
        if(h.action!=ACT_SOURCE_DELETE) u->delete_id=0;
        if(h.action!=ACT_CACHE_DELETE) u->cache_delete_id=0;
        if(h.action!=ACT_CACHE_CLEAR) u->cache_clear_confirm=false;
        if(h.action>=PLAYER_TOGGLE_QUEUE) { ui_player_action(u,h.action,h.value); break; }
        if(h.action>=LIBUI_FILTER) { ui_library_action(u,h.action,h.value,now); break; }
        switch(h.action) {
            case ACT_TAB: u->state.tab=h.value; u->velocity=0; break;
            case ACT_FILTER: u->state.library_filter=h.value; break;
            case ACT_CHIP: u->state.search_chip=u->state.search_chip==h.value?-1:h.value; break;
            case ACT_SETTING: u->state.settings_row=u->state.settings_row==h.value?-1:h.value; break;
            case ACT_PREF_WIFI: case ACT_PREF_THEME: case ACT_PREF_RETRY:
                if(!settings_command(h.action==ACT_PREF_WIFI?SETTINGS_WIFI:h.action==ACT_PREF_THEME?SETTINGS_THEME:SETTINGS_RETRY,
                    h.action==ACT_PREF_WIFI?!u->settings.wifi_only:h.value)) notice(u,"设置尚未就绪，请稍后重试",now);
                break;
            case ACT_NOTICE: notice(u,h.value?"搜索输入将在后续开放":"音乐库功能将在后续开放",now); break;
            case ACT_SOURCE_OPEN: u->state.tab=3; u->cache_open=false; u->source_open=true; u->state.scroll[3]=0; u->delete_id=0; break;
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
                    // 只复制当前可见的一页，后续搜索/删除音源不修改播放队列。
                    if(playback_replace(u->search.items,u->search.count,h.value)) {
                        u->playback_requested=true;
                        playback_view(&u->playback);
                        u->state.tab=2; u->state.scroll[2]=0; u->velocity=0;
                    } else notice(u,"无法创建播放队列，请重试",now);
                }
                break;
            case ACT_CACHE_OPEN:
                u->state.tab=3; u->source_open=false; u->cache_open=true; u->state.scroll[3]=0; u->velocity=0; break;
            case ACT_CACHE_BACK: u->cache_open=false; u->state.scroll[3]=0; break;
            case ACT_CACHE_DOWNLOAD:
                if(!cache_command(CACHE_DOWNLOAD,&u->playback.current,0,0)) notice(u,"缓存忙碌，请稍后重试",now);
                break;
            case ACT_CACHE_REFRESH:
                if(!cache_command(CACHE_REFRESH,NULL,0,0)) notice(u,"缓存忙碌，请稍后重试",now);
                break;
            case ACT_CACHE_PAGE: cache_page(h.value); u->state.scroll[3]=0; u->velocity=0; break;
            case ACT_CACHE_LIMIT:
                if(!cache_command(CACHE_SET_LIMIT,NULL,0,(uint64_t)h.value*CACHE_MIB)) notice(u,"缓存忙碌，请稍后重试",now);
                break;
            case ACT_CACHE_CLEAR:
                if(u->cache_clear_confirm) {
                    if(!cache_command(CACHE_CLEAR,NULL,0,0)) notice(u,"缓存忙碌，请稍后重试",now);
                    u->cache_clear_confirm=false;
                } else u->cache_clear_confirm=true;
                break;
            case ACT_CACHE_DELETE:
                if(u->cache_delete_id==(uint32_t)h.value) {
                    if(!cache_command(CACHE_DELETE,NULL,(uint32_t)h.value,0)) notice(u,"缓存忙碌，请稍后重试",now);
                    u->cache_delete_id=0;
                } else u->cache_delete_id=(uint32_t)h.value;
                break;
            case ACT_CACHE_PLAY:
                if(h.value>=0 && h.value<u->cache.count && playback_replace(&u->cache.items[h.value].item,1,0)) {
                    u->playback_requested=true; playback_view(&u->playback);
                    u->state.tab=2; u->state.scroll[2]=0; u->velocity=0;
                }
                break;
            case ACT_PLAYBACK:
                playback_command(h.value,0); u->playback_requested=true; break;
            case ACT_PLAYBACK_SEEK:
                playback_command(PLAYBACK_SEEK,u->playback.position+(int64_t)h.value*1000); break;
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
    if(u->state.tab==2 && u->queue_open) {
        ui_player_action(u,PLAYER_TOGGLE_QUEUE,0); return true;
    }
    if(ui_library_back(u)) return true;
    if(u->state.tab==1 && u->input_open) { u->input_open=false; return true; }
    if(u->state.tab==1 && u->search.busy) { search_cancel(); return true; }
    if(u->state.tab==3 && u->cache_open) {
        u->cache_open=false; u->cache_clear_confirm=false; u->cache_delete_id=0; u->state.scroll[3]=0; return true;
    }
    if(u->state.tab==3 && u->source_open) { u->source_open=false; u->state.scroll[3]=0; return true; }
    if(u->state.tab==3 && u->state.settings_row>=0) { u->state.settings_row=-1; return true; }
    if(u->state.tab!=0) { u->state.tab=0; return true; }
    return false;
}
