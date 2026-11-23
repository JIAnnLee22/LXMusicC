#include "media_catalog.h"
#include "playback_store.h"
#include "library.h"
#include "cache.h"
#include "vendor/quickjs/quickjs.h"
#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static void text(JSContext *c,JSValue o,const char *key,const char *value) {
    JS_SetPropertyStr(c,o,key,JS_NewString(c,value));
}
static void number(JSContext *c,JSValue o,const char *key,int64_t n) {
    JS_SetPropertyStr(c,o,key,JS_NewInt64(c,n));
}
static JSValue folder(JSContext *c,const char *id,const char *title) {
    JSValue o=JS_NewObject(c); text(c,o,"id",id); text(c,o,"title",title); number(c,o,"folder",1); return o;
}
static JSValue song(JSContext *c,const char *id,const SearchItem *item,bool uri) {
    JSValue o=JS_NewObject(c); text(c,o,"id",id); text(c,o,"title",item->song.name);
    text(c,o,"artist",item->song.artist); text(c,o,"album",item->song.album);
    number(c,o,"duration",(int64_t)(item->song.duration*1000));
    if(uri) {
        char local[CACHE_URI_BYTES]=""; cache_lookup(item,local);
        text(c,o,"url",local[0]?local:item->song.url);
    }
    return o;
}
static char *json(JSContext *c,JSValue value) {
    JSValue encoded=JS_JSONStringify(c,value,JS_UNDEFINED,JS_UNDEFINED);
    const char *s=JS_IsException(encoded)?NULL:JS_ToCString(c,encoded);
    char *copy=s?strdup(s):NULL;
    if(s) JS_FreeCString(c,s);
    JS_FreeValue(c,encoded); JS_FreeValue(c,value); return copy;
}
static JSContext *context(JSRuntime **runtime) {
    *runtime=JS_NewRuntime(); if(!*runtime) return NULL;
    JS_SetMemoryLimit(*runtime,16*1024*1024); JS_SetMaxStackSize(*runtime,512*1024);
    return JS_NewContext(*runtime);
}
char *media_queue_json(const SearchItem *items,const PlaybackView *view,unsigned serial) {
    JSRuntime *r; JSContext *c=context(&r); if(!c) { if(r) JS_FreeRuntime(r); return NULL; }
    JSValue o=JS_NewObject(c),a=JS_NewArray(c);
    number(c,o,"index",view->index); number(c,o,"position",view->position);
    for(int i=0;i<view->count;++i) {
        char id[96]; snprintf(id,sizeof(id),"queue/%u/%d",serial,i);
        JS_SetPropertyUint32(c,a,(uint32_t)i,song(c,id,items+i,true));
    }
    JS_SetPropertyStr(c,o,"items",a);
    char *s=json(c,o); JS_FreeContext(c); JS_FreeRuntime(r); return s;
}
static const char *root_ids[]={"search","favorites","playlists","recent","queue"};
static const char *root_titles[]={"搜索结果","收藏","歌单","最近播放","播放队列"};
static bool integer(const char **cursor,unsigned *value) {
    const char *s=*cursor;
    if(*s<'0' || *s>'9') return false;
    errno=0; char *end; unsigned long n=strtoul(s,&end,10);
    if(errno || n>UINT_MAX || (end>s+1 && *s=='0')) return false;
    *value=(unsigned)n; *cursor=end; return true;
}
static bool pair(const char *id,const char *prefix,unsigned *a,unsigned *b) {
    size_t n=strlen(prefix); if(strncmp(id,prefix,n)) return false;
    const char *p=id+n;
    return integer(&p,a) && *p++=='/' && integer(&p,b) && !*p;
}
static bool list_id(const char *id,int *list) {
    if(!strcmp(id,"favorites")) { *list=0; return true; }
    if(!strcmp(id,"recent")) { *list=1; return true; }
    if(strncmp(id,"list/",5)) return false;
    const char *p=id+5; unsigned n;
    if(!integer(&p,&n) || *p || n<2 || n>INT_MAX) return false;
    *list=(int)n; return true;
}
/* 媒体ID只引用C持有的内容；外部URI/元数据不进入播放器。过期搜索/队列ID拒绝。 */
static char *query(int action,const char *id,int page,int size,int64_t position) {
    PlaybackSnapshot *snap=calloc(1,sizeof(*snap));
    SearchItem *items=calloc(SEARCH_RESULT_LIMIT,sizeof(*items));
    LibraryTrack *tracks=calloc(SEARCH_RESULT_LIMIT,sizeof(*tracks));
    if(!snap || !items || !tracks) { free(snap); free(items); free(tracks); return NULL; }
    if(action==3) {
        bool have=playback_snapshot(snap);
        PlaybackView v={.count=snap->count,.index=snap->index,.position=snap->position};
        char *result=have?media_queue_json(snap->items,&v,snap->serial):NULL;
        free(snap); free(items); free(tracks); return result;
    }
    JSRuntime *r; JSContext *c=context(&r); char *result=NULL;
    if(!c) { if(r) JS_FreeRuntime(r); goto finish; }
    JSValue o=JS_NewObject(c),a=JS_NewArray(c);
    int total=-1,out=0,offset=page*size;
    unsigned directory_revision=library_revision();
    if(action==0) {
        if(!strcmp(id,"root")) {
            total=5;
            for(int i=offset;i<5 && out<size;++i) JS_SetPropertyUint32(c,a,(uint32_t)out++,folder(c,root_ids[i],root_titles[i]));
        } else if(!strcmp(id,"playlists")) {
            LibraryList lists[LIBRARY_LIST_LIMIT]={0};
            total=library_media(-1,NULL,lists,offset,size<LIBRARY_LIST_LIMIT?size:LIBRARY_LIST_LIMIT);
            for(int i=0;i<size && offset+i<total;++i) {
                char key[48]; snprintf(key,sizeof(key),"list/%u",lists[i].id);
                JS_SetPropertyUint32(c,a,(uint32_t)out++,folder(c,key,lists[i].name));
            }
        } else {
            int list; unsigned revision=0;
            if(list_id(id,&list)) total=library_media(list,tracks,NULL,offset,size);
            else if(!strcmp(id,"search")) { total=search_media(items,offset,size,&revision); directory_revision=revision; }
            else if(!strcmp(id,"queue")) {
                playback_snapshot(snap); total=snap->count; directory_revision=snap->serial;
            }
            for(int i=0;i<size && offset+i<total;++i) {
                char key[96]; const SearchItem *item;
                if(list_id(id,&list)) { snprintf(key,sizeof(key),"lib/%d/%u",list,tracks[i].id); item=&tracks[i].item; }
                else if(!strcmp(id,"search")) { snprintf(key,sizeof(key),"search/%u/%d",revision,offset+i); item=&items[i]; }
                else { snprintf(key,sizeof(key),"queue/%u/%d",snap->serial,offset+i); item=&snap->items[offset+i]; }
                JS_SetPropertyUint32(c,a,(uint32_t)out++,song(c,key,item,false));
            }
        }
    } else if(action==1 || action==2) {
        if(action==1 && !strcmp(id,"root")) { total=1; JS_SetPropertyUint32(c,a,0,folder(c,"root","LXMusicC")); }
        for(int i=0;action==1 && i<5;++i) if(!strcmp(id,root_ids[i])) {
            total=1; JS_SetPropertyUint32(c,a,0,folder(c,id,root_titles[i]));
        }
        int list,index=-1,count=0; unsigned key=0,first=0,second=0;
        if(action==1 && list_id(id,&list) && list>=2) {
            LibraryList lists[LIBRARY_LIST_LIMIT]={0}; int n=library_media(-1,NULL,lists,0,LIBRARY_LIST_LIMIT);
            for(int i=0;i<n;++i) if(lists[i].id==(uint32_t)list) { total=1; JS_SetPropertyUint32(c,a,0,folder(c,id,lists[i].name)); }
        } else if(pair(id,"lib/",&first,&key) && first<=INT_MAX) {
            list=(int)first;
            /* 收藏最多512首：只把被选歌曲所在的有界窗口装入队列。 */
            for(int start=0;start<LIBRARY_TRACK_LIMIT;start+=SEARCH_RESULT_LIMIT) {
                int n=library_media(list,tracks,NULL,start,SEARCH_RESULT_LIMIT);
                count=n-start; if(count>SEARCH_RESULT_LIMIT) count=SEARCH_RESULT_LIMIT;
                for(int i=0;i<count;++i) if(tracks[i].id==key) index=i;
                if(index>=0) { for(int i=0;i<count;++i) items[i]=tracks[i].item; break; }
                if(n<=start+SEARCH_RESULT_LIMIT) break;
            }
        } else if(pair(id,"search/",&key,&second) && second<SEARCH_RESULT_LIMIT) {
            index=(int)second;
            unsigned revision=key; count=search_media(items,0,SEARCH_RESULT_LIMIT,&revision);
        } else if(pair(id,"queue/",&key,&second) && second<SEARCH_RESULT_LIMIT) {
            index=(int)second;
            playback_snapshot(snap);
            if(key==snap->serial) { count=snap->count; memcpy(items,snap->items,(size_t)count*sizeof(*items)); }
        }
        if(index>=0 && index<count) {
            if(action==1) { total=1; JS_SetPropertyUint32(c,a,0,song(c,id,items+index,false)); }
            else if(playback_replace_at(items,count,index,position,false)) {
                playback_snapshot(snap);
                PlaybackView v={.count=snap->count,.index=snap->index,.position=snap->position};
                result=media_queue_json(snap->items,&v,snap->serial);
            }
        }
    }
    JS_SetPropertyStr(c,o,"items",a); number(c,o,"total",total); number(c,o,"revision",directory_revision);
    if(!result && total>=0) result=json(c,o); else JS_FreeValue(c,o);
    JS_FreeContext(c); JS_FreeRuntime(r);
finish:
    free(snap); free(items); free(tracks); return result;
}
static bool clean(JNIEnv *e) {
    if(!(*e)->ExceptionCheck(e)) return true;
    (*e)->ExceptionClear(e);
    __android_log_print(ANDROID_LOG_ERROR,"LXMusicC","Media catalog JNI exception"); return false;
}
static bool input(JNIEnv *e,jbyteArray data,char *out,int cap) {
    if(!data) return false;
    jsize n=(*e)->GetArrayLength(e,data);
    if(!clean(e) || n<=0 || n>=cap) return false;
    (*e)->GetByteArrayRegion(e,data,0,n,(jbyte *)out);
    if(!clean(e) || memchr(out,0,(size_t)n) || !source_utf8(out,(size_t)n)) return false;
    out[n]=0; return true;
}
JNIEXPORT jboolean JNICALL Java_jiannlee22_lxmusicc_MediaCatalog_nativeBoot(JNIEnv *e,jobject self,jbyteArray directory) {
    char path[1024]; if(!input(e,directory,path,sizeof(path))) return JNI_FALSE;
    cache_media_load(path);
    if(!library_start(path,NULL) || !playback_store_start(path)) return JNI_FALSE;
    playback_store_wait(); return JNI_TRUE;
}
JNIEXPORT jbyteArray JNICALL Java_jiannlee22_lxmusicc_MediaCatalog_nativeQuery(JNIEnv *e,jobject self,
    jint action,jbyteArray data,jint page,jint size,jlong position) {
    char id[256]; if(!input(e,data,id,sizeof(id)) || action<0 || action>3 || page<0 || size<1 || size>100 ||
        page>INT_MAX/size || position<0 || position>86400000) return NULL;
    char *s=query(action,id,page,size,position); if(!s) return NULL;
    size_t n=strlen(s); jbyteArray out=(*e)->NewByteArray(e,(jsize)n);
    if(!clean(e) || !out) { free(s); return NULL; }
    (*e)->SetByteArrayRegion(e,out,0,(jsize)n,(const jbyte *)s); free(s);
    return clean(e)?out:NULL;
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeSeek(JNIEnv *e,jobject self,jbyteArray data,jlong position) {
    char id[96]; unsigned serial,index;
    if(input(e,data,id,sizeof(id)) && position>=0 && position<=86400000 &&
        pair(id,"queue/",&serial,&index) && index<SEARCH_RESULT_LIMIT) playback_seek_index(serial,(int)index,position);
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_SourceActivity_nativeOpenPlayer(JNIEnv *e,jobject self) { playback_open_player(); }
