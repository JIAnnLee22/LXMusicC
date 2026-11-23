#include "library_ui.h"
#include "ime_ui.h"
#include <stdio.h>
#include <string.h>

static const Color TEXT={.90f,.93f,.97f,1}, MUTED={.53f,.60f,.68f,1};
static const Color CARD={.075f,.094f,.12f,1};
static void hit(Ui *u,Rect b,int action,int value) {
    if(u->hit_count<128) u->hits[u->hit_count++]=(UiHit){b,action,value};
}
static void label(Renderer *r,float x,float y,float w,float size,Color c,const char *s) {
    char line[1028]; snprintf(line,sizeof(line),"%s",s); size_t n=strlen(line);
    if(render_text_width(line,size)>w) {
        while(n) {
            do { --n; } while(n && ((unsigned char)line[n]&0xc0)==0x80);
            line[n]=0;
            if(render_text_width(line,size)+render_text_width("...",size)<=w) break;
        }
        strcat(line,"...");
    }
    render_text(r,x,y,size,c,line);
}
static void button(Ui *u,Renderer *r,Rect b,const char *s,int action,int value) {
    bool active=action==LIBUI_FILTER && u->state.library_filter==value;
    render_rect(r,b,12,active?ACCENT:TINT);
    float width=render_text_width(s,13),size=width>b.w-12?13*(b.w-12)/width:13;
    render_text(r,b.x+(b.w-render_text_width(s,size))/2,b.y+b.h/2+size*.34f,size,active?CARD:ACCENT,s); hit(u,b,action,value);
}
static void notice(Ui *u,const char *s,double now) {
    snprintf(u->toast,sizeof(u->toast),"%s",s); u->toast_until=now+2.6;
}
void ui_library_refresh(Ui *u) {
    u->library_request=library_browse(u->library_id,u->library_page,u->library_query);
    u->state.scroll[0]=0; u->velocity=0; u->library_confirm=0;
}
static bool command(Ui *u,int action,uint32_t id,uint32_t song,const SearchItem *item,const char *name,double now) {
    if(library_command(action,id,song,item,name)) return true;
    notice(u,"音乐库未就绪或队列已满，请稍后重试",now); return false;
}
float ui_library_draw(Ui *u,Renderer *r,float x,float y,float w) {
    float start=y; LibraryView *v=&u->library;
    const char *filters[]={"收藏","最近播放","我的歌单"}; float fw=(w-16)/3;
    for(int i=0;i<3;++i) button(u,r,(Rect){x+i*(fw+8),y,fw,40},filters[i],LIBUI_FILTER,i);
    y+=52;
    label(r,x,y+19,w,18,TEXT,v->title[0]?v->title:"音乐库加载中"); y+=30;
    y=render_paragraph(r,x,y,w,12,ACCENT,v->busy?"正在保存 / 加载…":v->message); y+=9;
    if(u->library_adding) {
        label(r,x,y+16,w,13,ACCENT,"选择一张歌单添加当前歌曲"); y+=28;
        button(u,r,(Rect){x,y,w,32},"取消添加",LIBUI_CANCEL,0); y+=44;
    }
    if(u->library_id>=2) {
        float bw=(w-16)/3;
        button(u,r,(Rect){x,y,bw,34},"返回歌单",LIBUI_BACK,0);
        button(u,r,(Rect){x+bw+8,y,bw,34},"重命名",LIBUI_RENAME,0);
        button(u,r,(Rect){x+2*(bw+8),y,bw,34},u->library_confirm_action==LIBUI_DELETE && u->library_confirm==(uint32_t)u->library_id?"确认删除":"删除歌单",LIBUI_DELETE,u->library_id);
        y+=46;
    } else if(u->library_id==-1) {
        button(u,r,(Rect){x,y,w,36},"创建歌单",LIBUI_CREATE,0); y+=48;
    }
    const char *input=u->library_input>=2?u->library_name:u->library_query;
    Rect box={x,y,w,42};
    ui_ime_field(u,r,box,u->library_input==2?IME_CREATE:u->library_input==3?IME_RENAME:IME_LIBRARY,
        input,u->library_input>=2?"输入歌单名称":"本地搜索：歌名 / 歌手 / 专辑");
    hit(u,box,LIBUI_INPUT,0); y+=52;
    float bw=(w-16)/3;
    button(u,r,(Rect){x,y,bw,34},"粘贴文字",LIBUI_PASTE,0);
    button(u,r,(Rect){x+bw+8,y,bw,34},u->library_input>=2?"保存名称":"搜索",LIBUI_SAVE,0);
    button(u,r,(Rect){x+2*(bw+8),y,bw,34},u->library_input?"收起 / 取消":"清除搜索",LIBUI_CANCEL,0); y+=46;
    // 浏览请求尚在后台计算时不显示旧列表，避免把旧页索引当成新页数据。
    if(v->request!=u->library_request || v->list!=u->library_id || strcmp(v->query,u->library_query) || v->page!=u->library_page) {
        y=render_paragraph(r,x,y,w,14,MUTED,u->library_input==1?"输入后点击搜索":
            !v->busy && strstr(v->message,"失败")?v->message:"正在更新列表…");
        for(int i=0;i<3;++i) {
            render_rect(r,(Rect){x,y+14,w,54},12,CARD);
            render_rect(r,(Rect){x+12,y+28,w*.6f,6},3,TINT);
            render_rect(r,(Rect){x+12,y+46,w*.35f,4},2,TINT); y+=64;
        }
        button(u,r,(Rect){x,y,w,36},"重试加载列表",LIBUI_REFRESH,0); y+=48;
        return y+12-start;
    }
    char summary[100]; snprintf(summary,sizeof(summary),"%d 项 · 第 %d / %d 页",v->total,v->page+1,v->total?(v->total+5)/6:1);
    label(r,x,y+18,w,13,TEXT,summary); y+=30;
    if(!v->count) {
        y=render_paragraph(r,x+8,y+12,w-16,15,MUTED,u->library_query[0]?"没有匹配项，可清除关键词重试":u->library_id==-1?"暂无歌单，点击创建歌单":u->library_id==1?"暂无历史，实际播放后自动记录":"暂无歌曲，在播放页收藏或添加到歌单");
        button(u,r,(Rect){x,y+16,w,36},"前往搜索",LIBUI_PLAY,-1);
        return y+68-start;
    }
    button(u,r,(Rect){x,y,(w-10)/2,32},"上一页",LIBUI_PAGE,-1);
    button(u,r,(Rect){x+(w+10)/2,y,(w-10)/2,32},"下一页",LIBUI_PAGE,1); y+=44;
    for(int i=0;i<v->count;++i) {
        if(u->library_id==-1) {
            LibraryList *p=&v->lists[i]; Rect card={x,y,w,72};
            render_rect(r,card,14,CARD); hit(u,card,LIBUI_OPEN,(int)p->id);
            label(r,x+14,y+27,w-28,17,TEXT,p->name);
            snprintf(summary,sizeof(summary),"%d 首 · 点击%s",p->count,u->library_adding?"添加":"浏览");
            label(r,x+14,y+53,w-28,12,ACCENT,summary); y+=82;
        } else {
            LibraryTrack *t=&v->songs[i]; Rect card={x,y,w,84};
            render_rect(r,card,14,ui_current_song(u,&t->item)?TINT:CARD); hit(u,card,LIBUI_PLAY,(int)t->id);
            label(r,x+12,y+26,w-24,17,TEXT,t->item.song.name);
            label(r,x+12,y+50,w-24,13,MUTED,t->item.song.artist);
            label(r,x+12,y+72,w-24,11,ACCENT,t->item.source_name); y+=92;
            button(u,r,(Rect){x,y,w,32},u->library_confirm_action==LIBUI_REMOVE && u->library_confirm==t->id?"确认移除":"从此列表移除",LIBUI_REMOVE,(int)t->id); y+=44;
        }
    }
    if((v->page+1)*LIBRARY_PAGE_SIZE<v->total) {
        button(u,r,(Rect){x,y,w,40},"加载更多 / 下一页",LIBUI_PAGE,1); y+=52;
    }
    return y-start+12;
}
float ui_library_player(Ui *u,Renderer *r,float x,float y,float w) {
    if(!u->playback.count || u->queue_open) return 0;
    float bw=(w-10)/2;
    button(u,r,(Rect){x,y,bw,36},"收藏 / 取消收藏",LIBUI_FAVORITE,0);
    button(u,r,(Rect){x+bw+10,y,bw,36},"添加到歌单",LIBUI_PICK,0);
    float end=render_paragraph(r,x+2,y+46,w-4,12,MUTED,u->library.busy?"音乐库正在保存…":u->library.message);
    return end-y+12;
}
void ui_library_action(Ui *u,int action,int value,double now) {
    if(action!=LIBUI_DELETE && action!=LIBUI_REMOVE) u->library_confirm=0;
    switch(action) {
        case LIBUI_REFRESH: ui_library_refresh(u); break;
        case LIBUI_FILTER:
            u->state.library_filter=value; u->library_id=value==2?-1:value; u->library_page=0;
            u->library_input=0; u->library_query[0]=0; u->library_adding=false; ui_library_refresh(u); break;
        case LIBUI_PAGE: {
            int page=u->library_page+value;
            if(page>=0 && page*LIBRARY_PAGE_SIZE<u->library.total) { u->library_page=page; ui_library_refresh(u); }
            break;
        }
        case LIBUI_OPEN:
            if(u->library_adding) {
                command(u,LIB_ADD,(uint32_t)value,0,&u->library_candidate,NULL,now);
            } else { u->library_id=value; u->library_page=0; u->library_query[0]=0; ui_library_refresh(u); }
            break;
        case LIBUI_BACK:
            u->library_id=-1; u->library_page=0; u->library_query[0]=0; u->library_input=0; ui_library_refresh(u); break;
        case LIBUI_INPUT:
            if(!u->library_input) u->library_input=1;
            ui_ime_open(u,u->library_input==2?IME_CREATE:u->library_input==3?IME_RENAME:IME_LIBRARY); break;
        case LIBUI_CREATE: case LIBUI_RENAME:
            u->library_input=action==LIBUI_CREATE?2:3;
            snprintf(u->library_name,256,"%s",action==LIBUI_CREATE?"":u->library.title);
            ui_ime_open(u,action==LIBUI_CREATE?IME_CREATE:IME_RENAME);
            u->state.scroll[0]=0; break;
        case LIBUI_SAVE:
            if(u->library_input>=2) {
                if(command(u,u->library_input==2?LIB_CREATE:LIB_RENAME,(uint32_t)u->library_id,0,NULL,u->library_name,now)) u->library_input=0;
            } else { u->library_input=0; u->library_page=0; ui_library_refresh(u); }
            break;
        case LIBUI_CANCEL:
            u->ime_target=0;
            if(u->library_input) u->library_input=0;
            else if(u->library_adding) u->library_adding=false;
            else { u->library_query[0]=0; u->library_page=0; ui_library_refresh(u); }
            break;
        case LIBUI_PASTE:
            if(!u->library_input) u->library_input=1;
            ui_ime_open(u,u->library_input==2?IME_CREATE:u->library_input==3?IME_RENAME:IME_LIBRARY);
            u->ime_paste=true; break;
        case LIBUI_KEY: ui_library_key(u,value,now); break;
        case LIBUI_DELETE: case LIBUI_REMOVE:
            if(u->library_confirm_action==action && u->library_confirm==(uint32_t)value) {
                bool ok=command(u,action==LIBUI_DELETE?LIB_DELETE:LIB_REMOVE,(uint32_t)u->library_id,(uint32_t)value,NULL,NULL,now);
                u->library_confirm=0;
                if(ok && action==LIBUI_DELETE) { u->library_id=-1; u->library_page=0; u->library_query[0]=0; ui_library_refresh(u); }
            } else { u->library_confirm=(uint32_t)value; u->library_confirm_action=action; }
            break;
        case LIBUI_PLAY:
            if(value==-1) { u->state.tab=1; break; }
            int index=-1;
            for(int i=0;i<u->library.count;++i) if(u->library.songs[i].id==(uint32_t)value) index=i;
            if(index>=0) {
                SearchItem items[LIBRARY_PAGE_SIZE];
                for(int i=0;i<u->library.count;++i) items[i]=u->library.songs[i].item;
                if(playback_replace(items,u->library.count,index)) {
                    u->playback_requested=true; playback_view(&u->playback);
                    u->state.tab=2; u->state.scroll[2]=0; u->velocity=0;
                } else notice(u,"无法创建播放队列，请重试",now);
            }
            break;
        case LIBUI_FAVORITE: command(u,LIB_FAVORITE,0,0,&u->playback.current,NULL,now); break;
        case LIBUI_PICK:
            u->library_candidate=u->playback.current; u->library_adding=true;
            u->state.tab=0; u->state.library_filter=2; u->library_id=-1; u->library_page=0;
            u->library_query[0]=0; u->library_input=0; ui_library_refresh(u); break;
    }
}
bool ui_library_key(Ui *u,int ch,double now) {
    if(u->state.tab!=0 || !u->library_input) return false;
    char *s=u->library_input>=2?u->library_name:u->library_query; size_t n=strlen(s);
    if(ch==8) {
        if(n) { do { --n; } while(n && ((unsigned char)s[n]&0xc0)==0x80); s[n]=0; }
    } else if(ch==13) ui_library_action(u,LIBUI_SAVE,0,now);
    else if(ch==127) s[0]=0;
    else if(ch>=32 && ch<=126 && n+1<256) { s[n]=(char)ch; s[n+1]=0; }
    return true;
}
void ui_library_sync(Ui *u) {
    if(u->library_paste_revision!=u->library.paste_revision) {
        u->library_paste_revision=u->library.paste_revision;
        if(u->library_paste_target>=2) snprintf(u->library_name,256,"%s",u->library.pasted);
        else {
            snprintf(u->library_query,256,"%s",u->library.pasted);
            u->library_page=0; ui_library_refresh(u);
        }
    }
    int last=u->library.total?(u->library.total-1)/LIBRARY_PAGE_SIZE:0;
    if(u->library.request==u->library_request && u->library.list==u->library_id && !strcmp(u->library.query,u->library_query) && u->library_page>last)
        u->library_page=last;
}
bool ui_library_back(Ui *u) {
    if(u->state.tab!=0) return false;
    if(u->library_input) { u->library_input=0; return true; }
    if(u->library_confirm) { u->library_confirm=0; return true; }
    if(u->library_adding) { u->library_adding=false; return true; }
    if(u->library_id>=2) { ui_library_action(u,LIBUI_BACK,0,0); return true; }
    return false;
}
