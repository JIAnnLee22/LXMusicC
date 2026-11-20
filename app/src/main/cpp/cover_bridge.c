/*
 * 封面 JNI 桥：C 工作线程同步调用 SourceActivity.requestCover([BJ)[B。
 * ByteArray 协议与 02/06 一致：首字节 1 + 原始图片字节（成功）或 0 + UTF-8 可读错误（失败），
 * 总长 ≤ 2 MiB + 1。二进制正文不经过 JNI modified UTF-8；URL 只允许 ASCII，用 NewByteArray 传入。
 */
#include "cover_bridge.h"
#include "cover.h"
#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COVER_FETCH_BYTES (2u*1024u*1024u)

static bool clean(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","cover bridge JNI exception");
    return false;
}

static JNIEnv *attach(ANativeActivity *activity,bool *detach) {
    JNIEnv *env=NULL;
    *detach=false;
    int result=(*activity->vm)->GetEnv(activity->vm,(void **)&env,JNI_VERSION_1_6);
    if(result==JNI_EDETACHED) {
        if((*activity->vm)->AttachCurrentThread(activity->vm,&env,NULL)!=JNI_OK) return NULL;
        *detach=true;
    } else if(result!=JNI_OK) return NULL;
    return env;
}

unsigned char *cover_bridge_fetch(void *context,const char *url,uint64_t token,size_t *size) {
    if(size) *size=0;
    ANativeActivity *activity=context;
    if(!activity || !activity->clazz || !activity->vm || !url || !token) return NULL;
    size_t length=strlen(url);
    if(!length || length>=COVER_URL_BYTES) {
        __android_log_print(ANDROID_LOG_WARN,"LXMusicC","cover: 地址为空或超过 2047 字节");
        return NULL;
    }
    /* 非 ASCII 需要百分号编码，避免 NewStringUTF/反射的编码歧义。 */
    for(const unsigned char *p=(const unsigned char *)url;*p;++p) if(*p<=32 || *p>=127) {
        __android_log_print(ANDROID_LOG_WARN,"LXMusicC","cover: 链接必须是百分号编码的 ASCII HTTP(S) 地址");
        return NULL;
    }
    bool detach=false;
    JNIEnv *env=attach(activity,&detach);
    unsigned char *output=NULL;
    if(!env) return NULL;
    if((*env)->PushLocalFrame(env,8)<0) { clean(env); goto end; }
    {
        jclass cls=(*env)->GetObjectClass(env,activity->clazz);
        if(!clean(env) || !cls) goto pop;
        jmethodID method=(*env)->GetMethodID(env,cls,"requestCover","([BJ)[B");
        if(!clean(env) || !method) goto pop;
        jbyteArray input=(*env)->NewByteArray(env,(jsize)length);
        if(!clean(env) || !input) goto pop;
        (*env)->SetByteArrayRegion(env,input,0,(jsize)length,(const jbyte *)url);
        if(!clean(env)) goto pop;
        jbyteArray bytes=(*env)->CallObjectMethod(env,activity->clazz,method,input,(jlong)token);
        if(!clean(env) || !bytes) goto pop;
        jsize n=(*env)->GetArrayLength(env,bytes);
        if(!clean(env) || n<1 || n>(jsize)(COVER_FETCH_BYTES+1)) goto pop;
        unsigned char *data=malloc((size_t)n);
        if(!data) goto pop;
        (*env)->GetByteArrayRegion(env,bytes,0,n,(jbyte *)data);
        if(!clean(env)) { free(data); goto pop; }
        if(data[0]!=1) {
            /* 错误文本可能不以 0 结尾，按长度打印；不复制到 C 字符串。 */
            if(data[0]==0 && n<=256 && (n==1 || source_utf8((const char *)data+1,(size_t)n-1)))
                __android_log_print(ANDROID_LOG_WARN,"LXMusicC","cover: 下载失败（%.*s）",(int)(n-1),(const char *)data+1);
            free(data);
            goto pop;
        }
        size_t total=(size_t)n-1;
        if(!total || total>COVER_FETCH_BYTES) { free(data); goto pop; }
        memmove(data,data+1,total);
        if(size) *size=total;
        output=data;
    }
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
    return output;
}

/* SourceActivity.nativeCoverCancelled(J)Z：Kotlin 下载线程在检查点调用，只读代次。 */
JNIEXPORT jboolean JNICALL Java_jiannlee22_lxmusicc_SourceActivity_nativeCoverCancelled(JNIEnv *env,jobject obj,jlong token) {
    (void)env; (void)obj;
    return cover_cancelled((uint64_t)token)?JNI_TRUE:JNI_FALSE;
}
