#include "ime_bridge.h"
#include "source.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>

static struct {
    pthread_mutex_t lock;
    ALooper *looper;
    ImeEvent event;
    int bottom, reference_height, session;
    bool pending;
} bridge={.lock=PTHREAD_MUTEX_INITIALIZER};
static bool checked(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","IME bridge JNI failed"); return false;
}
void ime_bridge_start(ALooper *looper) {
    pthread_mutex_lock(&bridge.lock);
    bridge.event=(ImeEvent){0}; bridge.pending=false; bridge.bottom=0; bridge.reference_height=0; bridge.session=0;
    bridge.looper=looper; if(looper) ALooper_acquire(looper);
    pthread_mutex_unlock(&bridge.lock);
}
void ime_bridge_stop(void) {
    pthread_mutex_lock(&bridge.lock);
    if(bridge.looper) ALooper_release(bridge.looper);
    bridge.looper=NULL; bridge.session=0; bridge.pending=false;
    pthread_mutex_unlock(&bridge.lock);
}
bool ime_bridge_take(ImeEvent *event,int *bottom,int *reference_height) {
    pthread_mutex_lock(&bridge.lock);
    bool pending=bridge.pending; *event=bridge.event; *bottom=bridge.bottom; *reference_height=bridge.reference_height;
    bridge.event=(ImeEvent){0}; bridge.pending=false;
    pthread_mutex_unlock(&bridge.lock); return pending;
}
bool ime_bridge_request(ANativeActivity *activity,int action,int session,const char *text,int start,int end,int kind) {
    JNIEnv *env=NULL; bool detach=false,ok=false;
    int result=(*activity->vm)->GetEnv(activity->vm,(void **)&env,JNI_VERSION_1_6);
    if(result==JNI_EDETACHED) {
        if((*activity->vm)->AttachCurrentThread(activity->vm,&env,NULL)!=JNI_OK) return false;
        detach=true;
    } else if(result!=JNI_OK) return false;
    if((*env)->PushLocalFrame(env,4)<0) { checked(env); goto out; }
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    if(!checked(env) || !cls) goto pop;
    jmethodID method=(*env)->GetMethodID(env,cls,"requestIme","(II[BIII)Z");
    if(!checked(env) || !method) goto pop;
    size_t n=strlen(text);
    if(n>=IME_BYTES || start<0 || end<0 || start>(int)n || end>(int)n) goto pop;
    jbyteArray bytes=(*env)->NewByteArray(env,(jsize)n);
    if(!checked(env) || !bytes) goto pop;
    (*env)->SetByteArrayRegion(env,bytes,0,(jsize)n,(const jbyte *)text);
    if(!checked(env)) goto pop;
    if(action==0 || action==1) {
        pthread_mutex_lock(&bridge.lock);
        bridge.session=action==1?session:0; bridge.event=(ImeEvent){0}; bridge.pending=false;
        pthread_mutex_unlock(&bridge.lock);
    }
    jboolean accepted=(*env)->CallBooleanMethod(env,activity->clazz,method,action,session,bytes,start,end,kind);
    ok=checked(env) && accepted==JNI_TRUE;
pop:
    (*env)->PopLocalFrame(env,NULL);
out:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
    return ok;
}
static bool boundary(const char *s,int n,int offset,bool optional) {
    return (optional && offset==-1) || (offset>=0 && offset<=n && ((unsigned char)s[offset]&0xc0)!=0x80);
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_ImeBridge_nativeIme(JNIEnv *env,jobject self,
        jint session,jbyteArray bytes,jint start,jint end,jint cs,jint ce,jint event) {
    if(!bytes || event<0 || event>4) return;
    jsize n=(*env)->GetArrayLength(env,bytes);
    if(!checked(env) || n<0 || n>=IME_BYTES) return;
    char text[IME_BYTES]; (*env)->GetByteArrayRegion(env,bytes,0,n,(jbyte *)text);
    if(!checked(env)) return;
    text[n]=0;
    if(memchr(text,0,(size_t)n) || (n && !source_utf8(text,(size_t)n))) return;
    if(!boundary(text,n,start,false) || !boundary(text,n,end,false) ||
       !boundary(text,n,cs,true) || !boundary(text,n,ce,true)) return;
    pthread_mutex_lock(&bridge.lock);
    if(bridge.looper && session==bridge.session && session>0) {
        ImeEvent *e=&bridge.event; e->session=session;
        if(event>=3) {
            e->failed|=event==4;
            size_t length=(size_t)n<sizeof(e->error)-1?(size_t)n:sizeof(e->error)-1;
            while(length && ((unsigned char)text[length]&0xc0)==0x80) --length;
            memcpy(e->error,text,length); e->error[length]=0;
        } else {
            memcpy(e->text,text,(size_t)n+1); e->changed=true;
            e->start=start; e->end=end; e->composing_start=cs; e->composing_end=ce;
            e->submit|=event==1; e->hidden|=event==2;
        }
        bridge.pending=true; ALooper_wake(bridge.looper);
    }
    pthread_mutex_unlock(&bridge.lock);
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_ImeBridge_nativeImeInsets(JNIEnv *env,jobject self,jint bottom,jint reference_height) {
    if(bottom<0 || bottom>16384 || reference_height<0 || reference_height>16384) return;
    pthread_mutex_lock(&bridge.lock);
    if(bridge.looper && (bottom!=bridge.bottom || reference_height!=bridge.reference_height)) {
        bridge.bottom=bottom; bridge.reference_height=reference_height; bridge.pending=true; ALooper_wake(bridge.looper);
    }
    pthread_mutex_unlock(&bridge.lock);
}
