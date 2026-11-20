#include "cache_bridge.h"
#include <android/native_activity.h>
#include <android/log.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool clean(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","Cache JNI exception"); return false;
}
static jbyteArray bytes(JNIEnv *env,const char *s) {
    if(!s) return NULL;
    jsize n=(jsize)strlen(s);
    jbyteArray b=(*env)->NewByteArray(env,n);
    if(!clean(env) || !b) return NULL;
    (*env)->SetByteArrayRegion(env,b,0,n,(const jbyte *)s);
    return clean(env)?b:NULL;
}
/* 清理路径段，按UTF-8边界截断，保留常用中文；拒绝路径穿越和控制符。 */
static void segment(char *out,size_t capacity,const char *input) {
    size_t n=0;
    for(const unsigned char *p=(const unsigned char *)input;*p;) {
        size_t step=*p<128?1:*p<224?2:*p<240?3:4;
        if(n+step>=capacity) break;
        if(step==1 && (*p<32 || strchr("/\\:*?\"<>|.",*p))) out[n++]='_';
        else { memcpy(out+n,p,step); n+=step; }
        p+=step;
    }
    while(n && out[n-1]==' ') --n;
    out[n]=0; if(!n) snprintf(out,capacity,"unknown");
}
static const char *extension(const SourceSong *song) {
    const char *url=song->url;
    const char *end=url+strcspn(url,"?#"),*dot=NULL;
    for(const char *p=url;p<end;++p) { if(*p=='/') dot=NULL; else if(*p=='.') dot=p; }
    const char *types[]={"m4a","mp3","flac","aac","ogg","opus","wav","mp4","webm"};
    if(dot) for(size_t i=0;i<sizeof(types)/sizeof(*types);++i)
        if((size_t)(end-dot-1)==strlen(types[i]) && !strncasecmp(dot+1,types[i],strlen(types[i]))) return types[i];
    if(strstr(song->quality,"flac")) return "flac";
    if(strstr(song->quality,"mp3") || !strcmp(song->quality,"128k") || !strcmp(song->quality,"320k")) return "mp3";
    if(strstr(song->quality,"ogg")) return "ogg";
    if(strstr(song->quality,"opus")) return "opus";
    return "m4a";
}
bool cache_bridge_io(void *context,int op,const SearchItem *item,const char *uri,
                     uint64_t maximum,char result[CACHE_URI_BYTES],uint64_t *size,char error[256]) {
    ANativeActivity *a=context; JNIEnv *env=NULL; bool attached=false,ok=false;
    snprintf(error,256,"公共缓存接口失败，请重试");
    jint status=(*a->vm)->GetEnv(a->vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*a->vm)->AttachCurrentThread(a->vm,&env,NULL)!=JNI_OK) return false;
        attached=true;
    } else if(status!=JNI_OK) return false;
    jint frame=(*env)->PushLocalFrame(env,10);
    if(!clean(env) || frame!=JNI_OK) goto end;
    jclass cls=(*env)->GetObjectClass(env,a->clazz);
    if(!clean(env) || !cls) goto pop;
    jmethodID method=(*env)->GetMethodID(env,cls,"requestCache","(I[B[B[B[BJ)[B");
    if(!clean(env) || !method) goto pop;
    char folder[100],artist[85],name[85],filename[200];
    if(item) {
        segment(folder,sizeof(folder),item->source_name); segment(artist,sizeof(artist),item->song.artist);
        segment(name,sizeof(name),item->song.name);
        snprintf(filename,sizeof(filename),"%s - %s.%s",artist,name,extension(&item->song));
    }
    jbyteArray url=item?bytes(env,item->song.url):NULL;
    jbyteArray dir=item?bytes(env,folder):NULL, file=item?bytes(env,filename):NULL;
    jbyteArray address=uri?bytes(env,uri):NULL;
    if((item && (!url || !dir || !file)) || (uri && !address)) goto pop;
    jbyteArray response=(*env)->CallObjectMethod(env,a->clazz,method,(jint)op,url,dir,file,address,(jlong)maximum);
    if(!clean(env) || !response) goto pop;
    jsize n=(*env)->GetArrayLength(env,response);
    if(!clean(env) || n<1 || n>1023) goto pop;
    char data[1024]; (*env)->GetByteArrayRegion(env,response,0,n,(jbyte *)data);
    if(!clean(env)) goto pop;
    data[n]=0;
    if(n>1 && !source_utf8(data+1,(size_t)n-1)) goto pop;
    if(data[0]!=1) { snprintf(error,256,"%.255s",data+1); goto pop; }
    if(op==1 || op==2) {
        char *tail=NULL; unsigned long long length=strtoull(data+1,&tail,10);
        if(!tail || tail==data+1 || *tail!='\n' || !length || length>16ull*1024*CACHE_MIB ||
           strncmp(tail+1,"content://media/",16) || strlen(tail+1)>=CACHE_URI_BYTES) goto pop;
        if(result) snprintf(result,CACHE_URI_BYTES,"%s",tail+1);
        if(size) *size=length;
    }
    error[0]=0; ok=true;
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(attached) (*a->vm)->DetachCurrentThread(a->vm);
    return ok;
}
