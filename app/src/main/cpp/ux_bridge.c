#include "ux_bridge.h"
#include <jni.h>
#include <pthread.h>
#include <math.h>
#include <android/log.h>

static struct {
    pthread_mutex_t lock;
    ALooper *looper;
    UxEvent event;
} bridge={.lock=PTHREAD_MUTEX_INITIALIZER,.event={.font_scale=1,.notifications=-1}};
static bool checked(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","UX bridge JNI failed"); return false;
}
void ux_bridge_start(ALooper *looper) {
    pthread_mutex_lock(&bridge.lock);
    bridge.looper=looper; ALooper_acquire(looper);
    bridge.event.back=false; bridge.event.back_progress=0; bridge.event.changed=true;
    pthread_mutex_unlock(&bridge.lock);
}
void ux_bridge_stop(void) {
    pthread_mutex_lock(&bridge.lock);
    if(bridge.looper) ALooper_release(bridge.looper);
    bridge.looper=NULL; bridge.event.back=false; bridge.event.back_progress=0;
    pthread_mutex_unlock(&bridge.lock);
}
UxEvent ux_bridge_take(void) {
    pthread_mutex_lock(&bridge.lock);
    UxEvent e=bridge.event; bridge.event.back=false; bridge.event.changed=false;
    pthread_mutex_unlock(&bridge.lock); return e;
}
bool ux_bridge_request(ANativeActivity *activity,int action,int value) {
    JNIEnv *env=NULL; bool detach=false,ok=false;
    int status=(*activity->vm)->GetEnv(activity->vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*activity->vm)->AttachCurrentThread(activity->vm,&env,NULL)!=JNI_OK) return false;
        detach=true;
    } else if(status!=JNI_OK) return false;
    if((*env)->PushLocalFrame(env,4)<0) { checked(env); goto end; }
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    if(!checked(env) || !cls) goto pop;
    jmethodID method=(*env)->GetMethodID(env,cls,"requestSystem","(II)Z");
    if(!checked(env) || !method) goto pop;
    jboolean accepted=(*env)->CallBooleanMethod(env,activity->clazz,method,action,value);
    ok=checked(env) && accepted==JNI_TRUE;
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
    return ok;
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_SystemBridge_nativeSystem(JNIEnv *env,jobject self,jfloat scale,jint notifications) {
    if(!isfinite(scale) || scale<.5f || scale>5 || notifications<0 || notifications>3) return;
    pthread_mutex_lock(&bridge.lock);
    bridge.event.font_scale=scale; bridge.event.notifications=notifications; bridge.event.changed=true;
    if(bridge.looper) ALooper_wake(bridge.looper);
    pthread_mutex_unlock(&bridge.lock);
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_SystemBridge_nativeBack(JNIEnv *env,jobject self,jint kind,jfloat progress) {
    if(kind<0 || kind>2 || !isfinite(progress) || progress<0 || progress>1) return;
    pthread_mutex_lock(&bridge.lock);
    if(bridge.looper) {
        bridge.event.back|=kind==0; bridge.event.back_progress=kind==1?progress:0;
        bridge.event.changed=true; ALooper_wake(bridge.looper);
    }
    pthread_mutex_unlock(&bridge.lock);
}
