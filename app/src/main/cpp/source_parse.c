#include "source.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool source_utf8(const char *s,size_t n) {
    for(size_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i++],cp; int more;
        if(!c) return false;
        if(c<128) continue;
        if(c>=0xc2 && c<=0xdf) { cp=c&31; more=1; }
        else if(c>=0xe0 && c<=0xef) { cp=c&15; more=2; }
        else if(c>=0xf0 && c<=0xf4) { cp=c&7; more=3; }
        else return false;
        int count=more;
        while(more--) {
            if(i>=n || ((unsigned char)s[i]&0xc0)!=0x80) return false;
            cp=(cp<<6)|((unsigned char)s[i++]&63);
        }
        if((count==2 && cp<0x800) || (count==3 && cp<0x10000) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}

typedef struct { const char *p,*end; char *script; int candidates; } Json;
static void space(Json *j) { while(j->p<j->end && strchr(" \r\n\t",*j->p)) ++j->p; }
static int hex4(Json *j) {
    int v=0;
    for(int i=0;i<4;++i) {
        if(j->p==j->end) return -1;
        unsigned char c=(unsigned char)*j->p++;
        int h=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if(h<0) return -1;
        v=v*16+h;
    }
    return v;
}
static char *json_string(Json *j) {
    if(j->p==j->end || *j->p++!='"') return NULL;
    char *out=malloc((size_t)(j->end-j->p)+1); size_t n=0;
    if(!out) return NULL;
    while(j->p<j->end) {
        unsigned char c=(unsigned char)*j->p++;
        if(c=='"') { out[n]=0; return out; }
        if(c<32) break;
        if(c!='\\') { out[n++]=(char)c; continue; }
        if(j->p==j->end) break;
        c=(unsigned char)*j->p++;
        if(c=='u') {
            int cp=hex4(j);
            if(cp<0 || cp==0) break;
            if(cp>=0xd800 && cp<=0xdbff) {
                if(j->end-j->p<2 || j->p[0]!='\\' || j->p[1]!='u') break;
                j->p+=2; int low=hex4(j);
                if(low<0xdc00 || low>0xdfff) break;
                cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
            } else if(cp>=0xdc00 && cp<=0xdfff) break;
            if(cp<0x80) out[n++]=(char)cp;
            else if(cp<0x800) { out[n++]=(char)(0xc0|(cp>>6)); out[n++]=(char)(0x80|(cp&63)); }
            else if(cp<0x10000) { out[n++]=(char)(0xe0|(cp>>12)); out[n++]=(char)(0x80|((cp>>6)&63)); out[n++]=(char)(0x80|(cp&63)); }
            else { out[n++]=(char)(0xf0|(cp>>18)); out[n++]=(char)(0x80|((cp>>12)&63)); out[n++]=(char)(0x80|((cp>>6)&63)); out[n++]=(char)(0x80|(cp&63)); }
        } else {
            const char *from="\"\\/bfnrt",*to="\"\\/\b\f\n\r\t";
            const char *q=strchr(from,c);
            if(!q) break;
            out[n++]=to[q-from];
        }
    }
    free(out); return NULL;
}
static bool value(Json *j,int depth,bool capture) {
    if(depth>24) return false;
    space(j); if(j->p==j->end) return false;
    if(*j->p=='"') {
        char *s=json_string(j); if(!s) return false;
        if(capture) { ++j->candidates; if(!j->script) j->script=s; else free(s); }
        else free(s);
        return true;
    }
    if(*j->p=='{' || *j->p=='[') {
        bool object=*j->p++=='{'; char close=object?'}':']';
        space(j); if(j->p<j->end && *j->p==close) { ++j->p; return true; }
        for(;;) {
            bool take=false;
            if(object) {
                space(j); char *key=json_string(j); if(!key) return false;
                take=!strcmp(key,"script")||!strcmp(key,"code")||!strcmp(key,"content");
                free(key); space(j);
                if(j->p==j->end || *j->p++!=':') return false;
            }
            if(!value(j,depth+1,take)) return false;
            space(j); if(j->p==j->end) return false;
            char c=*j->p++; if(c==close) return true;
            if(c!=',') return false;
        }
    }
    const char *p=j->p;
    const char *literals[]={"true","false","null"};
    for(int i=0;i<3;++i) {
        size_t n=strlen(literals[i]);
        if((size_t)(j->end-p)>=n && !memcmp(p,literals[i],n)) { j->p+=n; return true; }
    }
    if(*j->p=='-') ++j->p;
    if(j->p==j->end) return false;
    if(*j->p=='0') ++j->p;
    else { if(*j->p<'1'||*j->p>'9') return false; while(j->p<j->end && isdigit((unsigned char)*j->p)) ++j->p; }
    if(j->p<j->end && *j->p=='.') {
        ++j->p; const char *start=j->p;
        while(j->p<j->end && isdigit((unsigned char)*j->p)) ++j->p;
        if(start==j->p) return false;
    }
    if(j->p<j->end && (*j->p=='e'||*j->p=='E')) {
        ++j->p; if(j->p<j->end && (*j->p=='+'||*j->p=='-')) ++j->p;
        const char *start=j->p;
        while(j->p<j->end && isdigit((unsigned char)*j->p)) ++j->p;
        if(start==j->p) return false;
    }
    return j->p>p;
}
static bool field(char *out,size_t cap,const char *p,const char *end) {
    while(p<end && (*p==' '||*p=='\t')) ++p;
    while(end>p && isspace((unsigned char)end[-1])) --end;
    size_t n=(size_t)(end-p);
    if(n>=cap) return false;
    for(size_t i=0;i<n;++i) if((unsigned char)p[i]<32 && p[i]!='\t') return false;
    memcpy(out,p,n); out[n]=0; return true;
}
bool source_parse(const char *input,size_t length,SourceMeta *meta,char **script,char error[256]) {
    *script=NULL; memset(meta,0,sizeof(*meta));
    const char *why="输入为空或超过 2 MiB";
    if(!input || !length || length>SOURCE_BYTES) goto fail;
    why="内容不是有效 UTF-8 文本";
    if(!source_utf8(input,length)) goto fail;
    char *copy=malloc(length+1);
    why="内存不足，请重试"; if(!copy) goto fail;
    memcpy(copy,input,length); copy[length]=0;
    char *start=copy;
    if(length>=3 && !memcmp(start,"\xef\xbb\xbf",3)) start+=3;
    while(isspace((unsigned char)*start)) ++start;
    if(*start=='{' || *start=='[' || *start=='"') {
        Json j={.p=start,.end=copy+length};
        bool ok=value(&j,0,*start=='"'); space(&j);
        free(copy); copy=j.script;
        why="JSON 无效，或必须仅包含一个 script/code/content 脚本";
        if(!ok || j.p!=j.end || j.candidates!=1 || !copy) { free(copy); goto fail; }
        start=copy;
        if(strlen(start)>=3 && !memcmp(start,"\xef\xbb\xbf",3)) start+=3;
        while(isspace((unsigned char)*start)) ++start;
    }
    why="缺少 LX 注释头：需要 /* 注释和 @name";
    if(strncmp(start,"/*",2)) { free(copy); goto fail; }
    char *end=strstr(start+2,"*/");
    if(!end || end-start>65536) { free(copy); goto fail; }
    const char *p=start+2;
    while(p<end) {
        const char *line=p; while(p<end && *p!='\n' && *p!='\r') ++p;
        const char *stop=p;
        while(p<end && (*p=='\n'||*p=='\r')) ++p;
        while(line<stop && (*line==' '||*line=='\t'||*line=='*')) ++line;
        if(line==stop || *line++!='@') continue;
        const char *key=line; while(line<stop && !isspace((unsigned char)*line)) ++line;
        size_t n=(size_t)(line-key); char *out=NULL; size_t cap=0;
#define FIELD(k,f) if(n==sizeof(k)-1 && !memcmp(key,k,n)) { out=meta->f; cap=sizeof(meta->f); }
        FIELD("name",name) else FIELD("description",description) else FIELD("version",version)
        else FIELD("author",author) else FIELD("homepage",homepage)
#undef FIELD
        if(out && !field(out,cap,line,stop)) {
            free(copy); why="元数据过长或包含控制字符"; goto fail;
        }
    }
    if(!meta->name[0]) { free(copy); goto fail; }
    p=end+2; while(isspace((unsigned char)*p)) ++p;
    if(!*p) { free(copy); why="音源只有注释，没有脚本内容"; goto fail; }
    meta->enabled=true; *script=copy; error[0]=0; return true;
fail:
    snprintf(error,256,"%s",why); return false;
}
