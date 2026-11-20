#include "source_runtime.h"
#include "source.h"
#include "quickjs.h"
#include "lx_compat.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TASK_LIMIT 32
#define RESPONSE_LIMIT (RUNTIME_HTTP_BYTES*6u+65536u)
typedef struct { unsigned id; char *request; JSValue callback; double due; } Task;
typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    RuntimeHttp http;
    RuntimeCancelled cancelled;
    void *context;
    double deadline;
    unsigned next_id, requests, tasks;
    Task queue[TASK_LIMIT];
} Runtime;
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
}
static bool stopped(Runtime *r) {
    return now()>r->deadline || (r->cancelled && r->cancelled(r->context));
}
static int interrupt(JSRuntime *rt,void *opaque) { (void)rt; return stopped(opaque); }
static void exception(Runtime *r,char error[256]) {
    JSValue e=JS_GetException(r->ctx);
    const char *s=JS_ToCString(r->ctx,e);
    snprintf(error,256,"JS 执行失败: %.210s",s?s:"内存不足/执行中断");
    if(s) JS_FreeCString(r->ctx,s);
    JS_FreeValue(r->ctx,e);
}
static JSValue enqueue(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    Runtime *r=JS_GetContextOpaque(ctx);
    if(argc!=3 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx,argv[1])) return JS_ThrowTypeError(ctx,"无效异步任务");
    if(++r->tasks>256) return JS_ThrowRangeError(ctx,"异步任务过多");
    int slot=-1;
    for(int i=0;i<TASK_LIMIT;++i) if(!r->queue[i].id) { slot=i; break; }
    if(slot<0) return JS_ThrowRangeError(ctx,"异步队列已满");
    size_t n; const char *text=JS_ToCStringLen(ctx,&n,argv[0]);
    if(!text) return JS_EXCEPTION;
    double delay;
    if(JS_ToFloat64(ctx,&delay,argv[2])<0) { JS_FreeCString(ctx,text); return JS_EXCEPTION; }
    if(n>RUNTIME_HTTP_BYTES || strlen(text)!=n || !isfinite(delay)) {
        JS_FreeCString(ctx,text); return JS_ThrowRangeError(ctx,"请求过大或无效");
    }
    if(n && ++r->requests>16) { JS_FreeCString(ctx,text); return JS_ThrowRangeError(ctx,"单次 HTTP 请求超过 16 次"); }
    char *copy=strdup(text); JS_FreeCString(ctx,text);
    if(!copy) return JS_ThrowOutOfMemory(ctx);
    Task *t=&r->queue[slot];
    *t=(Task){++r->next_id,copy,JS_DupValue(ctx,argv[1]),now()+fmax(0,delay)/1000};
    return JS_NewUint32(ctx,t->id);
}
static JSValue cancel(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self; Runtime *r=JS_GetContextOpaque(ctx); uint32_t id;
    if(!argc || JS_ToUint32(ctx,&id,argv[0])<0) return JS_EXCEPTION;
    for(int i=0;i<TASK_LIMIT;++i) if(r->queue[i].id==id) {
        free(r->queue[i].request); JS_FreeValue(ctx,r->queue[i].callback); r->queue[i].id=0; break;
    }
    return JS_UNDEFINED;
}
static JSValue global_call(Runtime *r,const char *name,int argc,JSValueConst *args) {
    JSValue global=JS_GetGlobalObject(r->ctx),fn=JS_GetPropertyStr(r->ctx,global,name);
    JSValue value=JS_IsException(fn)?JS_EXCEPTION:JS_Call(r->ctx,fn,global,argc,args);
    JS_FreeValue(r->ctx,fn); JS_FreeValue(r->ctx,global); return value;
}
/* 返回 1=做过任务/等待 timer，0=无任务，-1=错误。HTTP 只在这里执行，不占 GL 线程。 */
static int step(Runtime *r,char error[256]) {
    if(stopped(r)) { snprintf(error,256,"音源执行已取消或超过 40 秒，请重试"); return -1; }
    JSContext *job_ctx=NULL;
    int n=JS_ExecutePendingJob(r->rt,&job_ctx);
    if(n<0) { exception(r,error); return -1; }
    if(n>0) return 1;
    int next=-1;
    for(int i=0;i<TASK_LIMIT;++i) if(r->queue[i].id && (next<0 || r->queue[i].due<r->queue[next].due)) next=i;
    if(next<0) return 0;
    if(r->queue[next].due>now()) { struct timespec pause={0,10000000}; nanosleep(&pause,NULL); return 1; }
    Task task=r->queue[next]; r->queue[next].id=0;
    JSValue argument=JS_UNDEFINED,ret;
    if(task.request[0]) {
        char http_error[256]="HTTP 桥接不可用";
        char *response=r->http?r->http(r->context,task.request,http_error):NULL;
        if(response && strlen(response)<=RESPONSE_LIMIT && source_utf8(response,strlen(response))) {
            argument=JS_NewString(r->ctx,response);
        } else {
            JSValue object=JS_NewObject(r->ctx);
            JS_SetPropertyStr(r->ctx,object,"error",JS_NewString(r->ctx,response?"HTTP 响应无效或过大":http_error));
            argument=JS_JSONStringify(r->ctx,object,JS_UNDEFINED,JS_UNDEFINED);
            JS_FreeValue(r->ctx,object);
        }
        free(response);
        ret=JS_IsException(argument)?JS_EXCEPTION:JS_Call(r->ctx,task.callback,JS_UNDEFINED,1,&argument);
    } else ret=JS_Call(r->ctx,task.callback,JS_UNDEFINED,0,NULL);
    free(task.request); JS_FreeValue(r->ctx,task.callback); JS_FreeValue(r->ctx,argument);
    bool ok=!JS_IsException(ret); JS_FreeValue(r->ctx,ret);
    if(!ok) { exception(r,error); return -1; }
    return 1;
}
static bool field(Runtime *r,JSValueConst object,const char *key,char *out,size_t cap,bool required,char error[256]) {
    JSValue value=JS_GetPropertyStr(r->ctx,object,key);
    bool ok=false;
    if(JS_IsException(value)) { exception(r,error); goto end; }
    if(JS_IsUndefined(value) && !required) { out[0]=0; ok=true; goto end; }
    if(!JS_IsString(value)) goto invalid;
    size_t n; const char *s=JS_ToCStringLen(r->ctx,&n,value);
    if(!s) { exception(r,error); goto end; }
    ok=n<cap && n==strlen(s) && source_utf8(s,n) && (!required || n>0);
    if(ok) memcpy(out,s,n+1);
    JS_FreeCString(r->ctx,s);
    if(ok) goto end;
invalid:
    snprintf(error,256,"歌曲字段 %s 缺失、类型错误或超长",key);
end:
    JS_FreeValue(r->ctx,value); return ok;
}
static bool songs(Runtime *r,JSValueConst value,RuntimeResult *result,char error[256]) {
    int is_array=JS_IsArray(r->ctx,value);
    if(is_array<=0) {
        if(is_array<0) exception(r,error); else snprintf(error,256,"search 必须返回歌曲数组");
        return false;
    }
    JSValue length=JS_GetPropertyStr(r->ctx,value,"length"); uint32_t count=0;
    int rc=JS_ToUint32(r->ctx,&count,length); JS_FreeValue(r->ctx,length);
    if(rc<0) { exception(r,error); return false; }
    result->has_more=count>RUNTIME_SONG_LIMIT;
    for(uint32_t i=0;i<count && i<RUNTIME_SONG_LIMIT;++i) {
        JSValue song=JS_GetPropertyUint32(r->ctx,value,i); SourceSong *s=&result->songs[i];
        bool ok=field(r,song,"id",s->id,sizeof(s->id),false,error) &&
            field(r,song,"name",s->name,sizeof(s->name),true,error) &&
            field(r,song,"artist",s->artist,sizeof(s->artist),true,error) &&
            field(r,song,"album",s->album,sizeof(s->album),false,error) &&
            field(r,song,"quality",s->quality,sizeof(s->quality),true,error) &&
            field(r,song,"url",s->url,sizeof(s->url),true,error);
        // 封面可选；坏地址只回退占位，不能使可播放的歌曲整个搜索失败。
        char ignored[256];
        if(!field(r,song,"cover",s->cover,sizeof(s->cover),false,ignored) ||
           (s->cover[0] && strncmp(s->cover,"https://",8) && strncmp(s->cover,"http://",7))) s->cover[0]=0;
        if(ok && strncmp(s->url,"https://",8) && strncmp(s->url,"http://",7)) {
            snprintf(error,256,"歌曲下载地址必须为 HTTP(S)"); ok=false;
        }
        if(ok) {
            JSValue duration=JS_GetPropertyStr(r->ctx,song,"duration");
            ok=JS_IsNumber(duration) && JS_ToFloat64(r->ctx,&s->duration,duration)==0 &&
                isfinite(s->duration) && s->duration>=0 && s->duration<=86400;
            JS_FreeValue(r->ctx,duration);
            if(!ok) snprintf(error,256,"歌曲时长必须为 0..86400 秒");
        }
        JS_FreeValue(r->ctx,song);
        if(!ok) return false;
        ++result->count;
    }
    return true;
}
static char *stringify(Runtime *r,JSValueConst value) {
    JSValue json=JS_JSONStringify(r->ctx,value,JS_UNDEFINED,JS_UNDEFINED);
    char *copy=NULL;
    if(!JS_IsException(json) && !JS_IsUndefined(json)) {
        size_t size; const char *text=JS_ToCStringLen(r->ctx,&size,json);
        if(text && size<=RUNTIME_HTTP_BYTES && size==strlen(text)) copy=strdup(text);
        if(text) JS_FreeCString(r->ctx,text);
    }
    JS_FreeValue(r->ctx,json); return copy;
}
void runtime_result_free(RuntimeResult *result) {
    free(result->json); free(result->info_json); memset(result,0,sizeof(*result));
}
bool runtime_call(const char *script,const char *provider,const char *action,const char *info_json,
                  RuntimeHttp http,RuntimeCancelled cancelled,void *context,
                  RuntimeResult *result,char error[256]) {
    memset(result,0,sizeof(*result)); snprintf(error,256,"无法创建 JS 运行时");
    if(!script || !action) { snprintf(error,256,"缺少脚本或请求动作"); return false; }
    if(!info_json) info_json="{}";
    if(strlen(info_json)>RUNTIME_HTTP_BYTES) { snprintf(error,256,"请求参数超过 2 MiB"); return false; }
    Runtime r={.http=http,.cancelled=cancelled,.context=context,.deadline=now()+40};
    bool ok=false; JSValue value=JS_UNDEFINED,promise=JS_UNDEFINED;
    r.rt=JS_NewRuntime(); if(!r.rt) return false;
    JS_SetMemoryLimit(r.rt,32u*1024u*1024u); JS_SetMaxStackSize(r.rt,512u*1024u);
    JS_SetInterruptHandler(r.rt,interrupt,&r);
    r.ctx=JS_NewContext(r.rt); if(!r.ctx) goto cleanup;
    JS_SetContextOpaque(r.ctx,&r);
    SourceMeta meta; char *parsed=NULL;
    if(!source_parse(script,strlen(script),&meta,&parsed,error)) goto cleanup;
    free(parsed);
    JSValue script_info=JS_NewObject(r.ctx);
    const char *keys[]={"name","description","version","author","homepage","rawScript"};
    const char *values[]={meta.name,meta.description,meta.version,meta.author,meta.homepage,script};
    for(int i=0;i<6;++i) JS_SetPropertyStr(r.ctx,script_info,keys[i],JS_NewString(r.ctx,values[i]));
    JSValue global=JS_GetGlobalObject(r.ctx);
    JS_SetPropertyStr(r.ctx,global,"__lxScriptInfo",script_info);
    JS_SetPropertyStr(r.ctx,global,"__lxQueue",JS_NewCFunction(r.ctx,enqueue,"__lxQueue",3));
    JS_SetPropertyStr(r.ctx,global,"__lxCancel",JS_NewCFunction(r.ctx,cancel,"__lxCancel",1));
    JS_FreeValue(r.ctx,global);
    value=JS_Eval(r.ctx,lx_compat,sizeof(lx_compat)-1,"lx-compat.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)) goto js_fail;
    JS_FreeValue(r.ctx,value);
    value=JS_Eval(r.ctx,script,strlen(script),"source.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)) goto js_fail;
    JS_FreeValue(r.ctx,value); value=JS_UNDEFINED;
    for(;;) {
        JSValue ready=global_call(&r,"__lxReady",0,NULL);
        if(JS_IsException(ready)) { JS_FreeValue(r.ctx,ready); goto js_fail; }
        int yes=JS_ToBool(r.ctx,ready); JS_FreeValue(r.ctx,ready);
        if(yes>0) break;
        int progress=step(&r,error);
        if(progress<0) goto cleanup;
        if(!progress) { snprintf(error,256,"音源未发送 inited，或初始化异步任务未完成"); goto cleanup; }
    }
    JSValue args[]={JS_NewString(r.ctx,provider?provider:""),JS_NewString(r.ctx,action),
        JS_ParseJSON(r.ctx,info_json,strlen(info_json),"request-info.json")};
    bool args_ok=!JS_IsException(args[0]) && !JS_IsException(args[1]) && !JS_IsException(args[2]);
    if(args_ok) promise=global_call(&r,"__lxInvoke",3,args);
    for(int i=0;i<3;++i) JS_FreeValue(r.ctx,args[i]);
    if(!args_ok || JS_IsException(promise)) goto js_fail;
    while(JS_PromiseState(r.ctx,promise)==JS_PROMISE_PENDING) {
        int progress=step(&r,error);
        if(progress<0) goto cleanup;
        if(!progress) { snprintf(error,256,"handler 的 Promise 没有完成（可能已取消请求）"); goto cleanup; }
    }
    value=JS_PromiseResult(r.ctx,promise);
    if(JS_PromiseState(r.ctx,promise)==JS_PROMISE_REJECTED) {
        const char *s=JS_ToCString(r.ctx,value); snprintf(error,256,"音源请求失败: %.210s",s?s:"未知错误");
        if(s) JS_FreeCString(r.ctx,s);
        goto cleanup;
    }
    if(!strcmp(action,"search")) {
        if(!songs(&r,value,result,error)) goto cleanup;
        if(result->count) {
            JSValue info=JS_NewObject(r.ctx);
            JS_SetPropertyStr(r.ctx,info,"type",JS_NewString(r.ctx,result->songs[0].quality));
            JS_SetPropertyStr(r.ctx,info,"musicInfo",JS_GetPropertyUint32(r.ctx,value,0));
            result->info_json=stringify(&r,info); JS_FreeValue(r.ctx,info);
            if(!result->info_json) { snprintf(error,256,"无法编码歌曲请求信息"); goto cleanup; }
        }
    } else if(!strcmp(action,"musicUrl")) {
        size_t size=0;
        const char *url=JS_IsString(value)?JS_ToCStringLen(r.ctx,&size,value):NULL;
        bool valid=url && size==strlen(url) && size<2048 && source_utf8(url,size) &&
            (!strncmp(url,"https://",8) || !strncmp(url,"http://",7));
        if(url) JS_FreeCString(r.ctx,url);
        if(!valid) { snprintf(error,256,"musicUrl 必须返回 HTTP(S) 直链字符串"); goto cleanup; }
    } else if(!strcmp(action,"lyric")) {
        JSValue lyric=JS_GetPropertyStr(r.ctx,value,"lyric");
        bool valid=JS_IsString(lyric); JS_FreeValue(r.ctx,lyric);
        if(!valid) { snprintf(error,256,"lyric 必须返回包含 lyric 字符串的对象"); goto cleanup; }
    }
    result->json=stringify(&r,value);
    if(!result->json) { snprintf(error,256,"handler 返回值不是 JSON、过大或内存不足"); goto cleanup; }
    ok=true; goto cleanup;
js_fail:
    exception(&r,error);
cleanup:
    if(r.ctx) {
        JS_FreeValue(r.ctx,value); JS_FreeValue(r.ctx,promise);
        for(int i=0;i<TASK_LIMIT;++i) if(r.queue[i].id) {
            free(r.queue[i].request); JS_FreeValue(r.ctx,r.queue[i].callback);
        }
        JS_FreeContext(r.ctx);
    }
    JS_FreeRuntime(r.rt);
    if(!ok) runtime_result_free(result);
    return ok;
}
