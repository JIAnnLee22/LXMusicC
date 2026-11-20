#include "playback_bridge.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t bridge_lock=PTHREAD_MUTEX_INITIALIZER;
static JavaVM *vm;
static jobject service;
static jmethodID apply_method;
static bool clean(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_ERROR,"LXMusicC","Playback JNI exception");
    return false;
}
static JNIEnv *environment(JavaVM *machine,bool *attached) {
    JNIEnv *env=NULL; *attached=false;
    jint status=(*machine)->GetEnv(machine,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*machine)->AttachCurrentThread(machine,&env,NULL)!=JNI_OK) return NULL;
        *attached=true;
    } else if(status!=JNI_OK) return NULL;
    return env;
}
static jbyteArray bytes(JNIEnv *env,const char *s) {
    jsize n=(jsize)strlen(s);
    jbyteArray result=(*env)->NewByteArray(env,n);
    if(!clean(env) || !result) return NULL;
    (*env)->SetByteArrayRegion(env,result,0,n,(const jbyte *)s);
    return clean(env)?result:NULL;
}
bool playback_bridge_apply(int action,const PlaybackView *view) {
    pthread_mutex_lock(&bridge_lock);
    bool attached=false,ok=false;
    JNIEnv *env=vm?environment(vm,&attached):NULL;
    if(env && service && apply_method) {
        jint frame=(*env)->PushLocalFrame(env,8);
        if(clean(env) && frame==JNI_OK) {
            jbyteArray url=NULL,title=NULL,artist=NULL;
            if(action==PLAYBACK_LOAD) {
                url=bytes(env,view->current.song.url);
                title=bytes(env,view->current.song.name);
                artist=bytes(env,view->current.song.artist);
            }
            if(action!=PLAYBACK_LOAD || (url && title && artist)) {
                jboolean result=(*env)->CallBooleanMethod(env,service,apply_method,(jint)action,
                    url,title,artist,(jlong)view->position,(jlong)view->generation,(jboolean)view->requested);
                ok=clean(env) && result==JNI_TRUE;
            }
            (*env)->PopLocalFrame(env,NULL);
        }
    }
    if(attached) (*vm)->DetachCurrentThread(vm);
    pthread_mutex_unlock(&bridge_lock);
    return ok;
}
bool playback_bridge_start(ANativeActivity *activity) {
    bool attached=false,ok=false;
    JNIEnv *env=environment(activity->vm,&attached);
    if(!env) return false;
    jint frame=(*env)->PushLocalFrame(env,4);
    if(clean(env) && frame==JNI_OK) {
        jclass cls=(*env)->GetObjectClass(env,activity->clazz);
        if(clean(env) && cls) {
            jmethodID method=(*env)->GetMethodID(env,cls,"requestPlaybackStart","()Z");
            if(clean(env) && method) {
                jboolean result=(*env)->CallBooleanMethod(env,activity->clazz,method);
                ok=clean(env) && result==JNI_TRUE;
            }
        }
        (*env)->PopLocalFrame(env,NULL);
    }
    if(attached) (*activity->vm)->DetachCurrentThread(activity->vm);
    return ok;
}
JNIEXPORT jboolean JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeReady(JNIEnv *env,jobject self) {
    bool ok=false;
    pthread_mutex_lock(&bridge_lock);
    JavaVM *machine=NULL;
    if((*env)->GetJavaVM(env,&machine)==JNI_OK) {
        jclass cls=(*env)->GetObjectClass(env,self);
        if(clean(env) && cls) {
            jmethodID method=(*env)->GetMethodID(env,cls,"applyPlayback","(I[B[B[BJJZ)Z");
            if(clean(env) && method) {
                jobject ref=(*env)->NewGlobalRef(env,self);
                if(clean(env) && ref) {
                    if(service) (*env)->DeleteGlobalRef(env,service);
                    service=ref; vm=machine; apply_method=method; ok=true;
                }
            }
            (*env)->DeleteLocalRef(env,cls);
        }
    }
    pthread_mutex_unlock(&bridge_lock);
    if(ok) playback_ready(); else playback_failed();
    return ok?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeGone(JNIEnv *env,jobject self) {
    pthread_mutex_lock(&bridge_lock);
    if(service) (*env)->DeleteGlobalRef(env,service);
    service=NULL; apply_method=NULL;
    pthread_mutex_unlock(&bridge_lock);
    playback_gone();
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_PlaybackStart_nativeFailure(JNIEnv *env,jobject self) {
    playback_failed();
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeCommand(JNIEnv *env,jobject self,jint action,jlong position) {
    playback_command(action,position);
}
JNIEXPORT jboolean JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeCanSkip(JNIEnv *env,jobject self,jint delta) {
    return playback_can_skip(delta)?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_jiannlee22_lxmusicc_PlaybackService_nativeState(JNIEnv *env,jobject self,
    jlong token,jint state,jboolean playing,jboolean requested,jlong position,jlong duration,jbyteArray error) {
    char message[256]="";
    if(error) {
        jsize n=(*env)->GetArrayLength(env,error);
        if(!clean(env) || n<0 || n>255) return;
        (*env)->GetByteArrayRegion(env,error,0,n,(jbyte *)message);
        if(!clean(env) || !source_utf8(message,(size_t)n)) return;
    }
    if(state<1 || state>4 || position<0 || duration<0) return;
    playback_state((uint64_t)token,state,playing==JNI_TRUE,requested==JNI_TRUE,position,duration,message);
}
