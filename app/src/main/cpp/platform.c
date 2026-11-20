#include "platform.h"
#include <android/log.h>
#include <jni.h>

static bool checked(JNIEnv *env) {
    if(!(*env)->ExceptionCheck(env)) return true;
    (*env)->ExceptionClear(env);
    __android_log_print(ANDROID_LOG_WARN,"LXMusicC","window insets JNI failed; retrying later");
    return false;
}
static jobject object_call(JNIEnv *env,jobject obj,const char *name,const char *signature) {
    if(!obj) return NULL;
    jclass cls=(*env)->GetObjectClass(env,obj);
    if(!checked(env) || !cls) return NULL;
    jmethodID method=(*env)->GetMethodID(env,cls,name,signature);
    if(!checked(env) || !method) return NULL;
    jobject result=(*env)->CallObjectMethod(env,obj,method);
    return checked(env)?result:NULL;
}
static bool read_sides(JNIEnv *env,jobject obj,const char *const methods[4],int out[4]) {
    jclass cls=(*env)->GetObjectClass(env,obj);
    if(!checked(env) || !cls) return false;
    for(int i=0;i<4;++i) {
        jmethodID method=(*env)->GetMethodID(env,cls,methods[i],"()I");
        if(!checked(env) || !method) return false;
        jint value=(*env)->CallIntMethod(env,obj,method);
        if(!checked(env) || value<0) return false;
        if(value>out[i]) out[i]=value;
    }
    return true;
}
bool platform_insets(ANativeActivity *activity,int insets[4]) {
    JNIEnv *env=NULL; bool detach=false,ok=false;
    jint status=(*activity->vm)->GetEnv(activity->vm,(void **)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED) {
        if((*activity->vm)->AttachCurrentThread(activity->vm,&env,NULL)!=JNI_OK) return false;
        detach=true;
    } else if(status!=JNI_OK) return false;
    if((*env)->PushLocalFrame(env,24)<0) { checked(env); goto end; }
    jobject window=object_call(env,activity->clazz,"getWindow","()Landroid/view/Window;");
    jobject decor=object_call(env,window,"getDecorView","()Landroid/view/View;");
    jobject root=object_call(env,decor,"getRootWindowInsets","()Landroid/view/WindowInsets;");
    int values[4]={0};
    const char *const bars[]={"getSystemWindowInsetLeft","getSystemWindowInsetTop","getSystemWindowInsetRight","getSystemWindowInsetBottom"};
    if(!root || !read_sides(env,root,bars,values)) goto pop;
    if(activity->sdkVersion>=28) {
        jobject cutout=object_call(env,root,"getDisplayCutout","()Landroid/view/DisplayCutout;");
        const char *const safe[]={"getSafeInsetLeft","getSafeInsetTop","getSafeInsetRight","getSafeInsetBottom"};
        if(cutout && !read_sides(env,cutout,safe,values)) goto pop;
    }
    for(int i=0;i<4;++i) insets[i]=values[i];
    ok=true;
pop:
    (*env)->PopLocalFrame(env,NULL);
end:
    if(detach) (*activity->vm)->DetachCurrentThread(activity->vm);
    return ok;
}
