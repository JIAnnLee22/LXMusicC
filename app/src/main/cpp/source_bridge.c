#include "source_bridge.h"
#include "source.h"
#include <android/log.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool checked(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","source bridge JNI failed"); return false;
}
static JNIEnv *attach(ANativeActivity *activity,bool *detach) {
    JNIEnv *env=NULL; *detach=false;
    int result=(*activity->vm)->GetEnv(activity->vm,(void **)&env,JNI_VERSION_1_6);
    if(result==JNI_EDETACHED) {
        if((*activity->vm)->AttachCurrentThread(activity->vm,&env,NULL)!=JNI_OK) return NULL;
        *detach=true;
    } else if(result!=JNI_OK) return NULL;
    return env;
}
bool source_bridge_paste(ANativeActivity *activity) {
    bool detach,ok=false; JNIEnv *env=attach(activity,&detach);
    if(!env) goto fail;
    if((*env)->PushLocalFrame(env,4)<0) { checked(env); goto end; }
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    if(!checked(env) || !cls) goto pop;
    jmethodID method=(*env)->GetMethodID(env,cls,"requestSourcePaste","()Z");
    if(!checked(env) || !method) goto pop;
    jboolean result=(*env)->CallBooleanMethod(env,activity->clazz,method);
    ok=checked(env) && result==JNI_TRUE;
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
fail:
    if(!ok) source_message("无法读取剪贴板，请重试");
    return ok;
}
void source_bridge_cancel(ANativeActivity *activity) {
    bool detach; JNIEnv *env=attach(activity,&detach);
    if(!env) return;
    if((*env)->PushLocalFrame(env,4)<0) { checked(env); goto end; }
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    if(checked(env) && cls) {
        jmethodID method=(*env)->GetMethodID(env,cls,"cancelSourceFetch","()V");
        if(checked(env) && method) { (*env)->CallVoidMethod(env,activity->clazz,method); checked(env); }
    }
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
}
char *source_bridge_fetch(void *context,const char *url,char error[256]) {
    ANativeActivity *activity=context;
    bool detach; JNIEnv *env=attach(activity,&detach); char *output=NULL;
    snprintf(error,256,"系统下载接口失败，请重试");
    if(!env) return NULL;
    if((*env)->PushLocalFrame(env,8)<0) { checked(env); goto end; }
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    if(!checked(env) || !cls) goto pop;
    jmethodID method=(*env)->GetMethodID(env,cls,"fetchSource","(Ljava/lang/String;)[B");
    if(!checked(env) || !method) goto pop;
    // URL 使用 ASCII（非 ASCII 路径应百分号编码），避免 JNI modified UTF-8 歧义。
    for(const unsigned char *p=(const unsigned char *)url;*p;++p) if(*p<=32 || *p>=127) {
        snprintf(error,256,"链接请使用百分号编码的 HTTP(S) 地址"); goto pop;
    }
    jstring address=(*env)->NewStringUTF(env,url);
    if(!checked(env) || !address) goto pop;
    jbyteArray bytes=(*env)->CallObjectMethod(env,activity->clazz,method,address);
    if(!checked(env) || !bytes) goto pop;
    jsize n=(*env)->GetArrayLength(env,bytes);
    if(!checked(env) || n<1 || n>(jsize)SOURCE_BYTES+1) goto pop;
    char *data=malloc((size_t)n+1);
    if(!data) { snprintf(error,256,"内存不足，请重试"); goto pop; }
    (*env)->GetByteArrayRegion(env,bytes,0,n,(jbyte *)data);
    if(!checked(env)) { free(data); goto pop; }
    data[n]=0;
    if(data[0]!=1) { snprintf(error,256,"%s",data+1); free(data); goto pop; }
    if(n<2 || !source_utf8(data+1,(size_t)n-1)) {
        snprintf(error,256,"下载内容为空或不是 UTF-8 文本"); free(data); goto pop;
    }
    memmove(data,data+1,(size_t)n); output=data;
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
    return output;
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_SourceActivity_nativePaste(JNIEnv *env,jobject obj,jbyteArray bytes,jstring error) {
    (void)obj;
    if(error) {
        const char *message=(*env)->GetStringUTFChars(env,error,NULL);
        if(checked(env) && message) { source_message(message); (*env)->ReleaseStringUTFChars(env,error,message); }
        else source_message("读取剪贴板失败，请重试");
        return;
    }
    if(!bytes) { source_message("剪贴板没有文本"); return; }
    jsize n=(*env)->GetArrayLength(env,bytes);
    if(!checked(env) || n<=0 || n>(jsize)SOURCE_BYTES) { source_message("剪贴板为空或超过 2 MiB"); return; }
    char *data=malloc((size_t)n);
    if(!data) { source_message("内存不足，请重试"); return; }
    (*env)->GetByteArrayRegion(env,bytes,0,n,(jbyte *)data);
    if(checked(env)) source_command(SOURCE_IMPORT,0,data,(size_t)n);
    else source_message("读取剪贴板失败，请重试");
    free(data);
}
