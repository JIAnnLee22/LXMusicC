// NativeActivity owns the event loop and GL context; all disk/network IO runs on workers.
#include "render.h"
#include "ui.h"
#include "platform.h"
#include "source_bridge.h"
#include "cache_bridge.h"
#include "cover.h"
#include "player_ui.h"
#include "library_ui.h"
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"LXMusicC",__VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,"LXMusicC",__VA_ARGS__)

typedef struct {
    struct android_app *app;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    bool ready, focused, resumed, dirty;
    Renderer renderer;
    Ui ui;
    pthread_t loader;
    atomic_int font_status;
    unsigned char *atlas;
    CoverImage cover;
    char cover_url[2048];
    double retry_at;
    float scale;
    int insets[4];
    bool insets_dirty;
} Engine;

static void source_wake(void *context) {
    ANativeActivity *activity=context;
    struct android_app *app=activity->instance;
    if(app && app->looper) ALooper_wake(app->looper);
}

static double seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}

static void *load_font(void *arg) {
    Engine *e=arg;
    AAsset *a=AAssetManager_open(e->app->activity->assetManager,"fonts/ui.sdf",AASSET_MODE_STREAMING);
    bool ok=false;
    if(a && AAsset_getLength(a)==FONT_ATLAS_BYTES) {
        e->atlas=malloc(FONT_ATLAS_BYTES);
        if(e->atlas) {
            size_t done=0;
            while(done<FONT_ATLAS_BYTES) {
                int n=AAsset_read(a,e->atlas+done,FONT_ATLAS_BYTES-done);
                if(n<=0) break;
                done+=(size_t)n;
            }
            ok=done==FONT_ATLAS_BYTES;
        }
    }
    if(a) AAsset_close(a);
    atomic_store_explicit(&e->font_status,ok?1:-1,memory_order_release);
    ALooper_wake(e->app->looper);
    return NULL;
}

static void engine_destroy(Engine *e) {
    if(e->display!=EGL_NO_DISPLAY) {
        if(e->ready) render_destroy(&e->renderer);
        eglMakeCurrent(e->display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if(e->context!=EGL_NO_CONTEXT) eglDestroyContext(e->display,e->context);
        if(e->surface!=EGL_NO_SURFACE) eglDestroySurface(e->display,e->surface);
        eglTerminate(e->display);
    }
    e->display=EGL_NO_DISPLAY; e->surface=EGL_NO_SURFACE; e->context=EGL_NO_CONTEXT;
    e->ready=false; ui_cancel(&e->ui);
}

static bool engine_create(Engine *e) {
    e->display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(e->display==EGL_NO_DISPLAY || !eglInitialize(e->display,NULL,NULL)) goto fail;
    const EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config; EGLint count,visual;
    if(!eglChooseConfig(e->display,attrs,&config,1,&count) || count<1) goto fail;
    if(!eglGetConfigAttrib(e->display,config,EGL_NATIVE_VISUAL_ID,&visual)) goto fail;
    if(ANativeWindow_setBuffersGeometry(e->app->window,0,0,visual)<0) goto fail;
    e->surface=eglCreateWindowSurface(e->display,config,e->app->window,NULL);
    if(e->surface==EGL_NO_SURFACE) goto fail;
    const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    e->context=eglCreateContext(e->display,config,EGL_NO_CONTEXT,ctx);
    if(e->context==EGL_NO_CONTEXT || !eglMakeCurrent(e->display,e->surface,e->surface,e->context)) goto fail;
    if(!eglSwapInterval(e->display,1)) goto fail;
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    if(!render_create(&e->renderer,e->atlas)) goto fail;
    render_cover_upload(&e->renderer,e->cover.pixels,e->cover.width,e->cover.height);
    e->ready=true; e->dirty=true; e->insets_dirty=true;
    LOGI("UI GLES ready: %s",(const char *)glGetString(GL_VERSION));
    return true;
fail:
    LOGE("EGL/renderer initialization failed: 0x%x; retrying",eglGetError());
    engine_destroy(e); e->retry_at=seconds()+1;
    return false;
}

static bool draw(Engine *e,double now) {
    EGLint w=0,h=0;
    if(!eglQuerySurface(e->display,e->surface,EGL_WIDTH,&w) ||
       !eglQuerySurface(e->display,e->surface,EGL_HEIGHT,&h)) return false;
    if(w<=0 || h<=0) return true;
    int density=AConfiguration_getDensity(e->app->config);
    if(density<=0 || density>=ACONFIGURATION_DENSITY_ANY) density=160;
    e->scale=density/160.f;
    ARect c=e->app->contentRect;
    // contentRect may span system navigation on edge-to-edge Android; intersect, do not add.
    if(e->insets_dirty) e->insets_dirty=!platform_insets(e->app->activity,e->insets);
    if(c.right<=c.left || c.bottom<=c.top) c=(ARect){0,0,w,h};
    c.left=c.left<e->insets[0]?e->insets[0]:c.left;
    c.top=c.top<e->insets[1]?e->insets[1]:c.top;
    c.right=c.right>w-e->insets[2]?w-e->insets[2]:c.right;
    c.bottom=c.bottom>h-e->insets[3]?h-e->insets[3]:c.bottom;
    if(c.right<=c.left || c.bottom<=c.top) {
        // Insets/contentRect may still belong to the previous orientation for one callback.
        c=(ARect){0,0,w,h}; e->insets_dirty=true;
    }
    ui_layout(&e->ui,(Rect){c.left/e->scale,c.top/e->scale,
        (c.right-c.left)/e->scale,(c.bottom-c.top)/e->scale});
    render_begin(&e->renderer,w,h,e->scale);
    ui_draw(&e->ui,&e->renderer,now);
    // Re-clamp after content measurement on rotation/expanded settings, before presenting.
    if(ui_tick(&e->ui,0)) {
        render_begin(&e->renderer,w,h,e->scale); ui_draw(&e->ui,&e->renderer,now);
    }
    if(!eglSwapBuffers(e->display,e->surface)) return false;
    e->dirty=e->insets_dirty; return true;
}

static void on_cmd(struct android_app *app,int32_t cmd) {
    Engine *e=app->userData;
    e->insets_dirty=true;
    switch(cmd) {
        case APP_CMD_INIT_WINDOW:
            if(e->ready) engine_destroy(e);
            e->retry_at=0; e->dirty=true; break;
        case APP_CMD_TERM_WINDOW: engine_destroy(e); break;
        case APP_CMD_RESUME: playback_ui(app->looper); e->resumed=true; e->dirty=true; break;
        case APP_CMD_PAUSE: playback_ui(NULL); e->resumed=false; ui_cancel(&e->ui); break;
        case APP_CMD_GAINED_FOCUS: e->focused=true; e->dirty=true; break;
        case APP_CMD_LOST_FOCUS: e->focused=false; ui_cancel(&e->ui); break;
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            ui_cancel(&e->ui); e->dirty=true; break;
        case APP_CMD_WINDOW_REDRAW_NEEDED: e->dirty=true; break;
        case APP_CMD_SAVE_STATE: {
            UiState *saved=malloc(sizeof(*saved));
            if(saved) { *saved=e->ui.state; app->savedState=saved; app->savedStateSize=sizeof(*saved); }
            break;
        }
        default: break;
    }
}

static int32_t on_input(struct android_app *app,AInputEvent *event) {
    Engine *e=app->userData; Ui *u=&e->ui;
    if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY) {
        int key=AKeyEvent_getKeyCode(event);
        // 即使搜索软键盘展开，也把硬件音量交还系统媒体流处理。
        if(key==AKEYCODE_VOLUME_UP || key==AKEYCODE_VOLUME_DOWN || key==AKEYCODE_VOLUME_MUTE) return 0;
        if(key!=AKEYCODE_BACK) {
            if(!((u->state.tab==1 && u->input_open) || (u->state.tab==0 && u->library_input))) return 0;
            if(AKeyEvent_getAction(event)!=AKEY_EVENT_ACTION_DOWN) return 1;
            int ch=0;
            bool shift=(AKeyEvent_getMetaState(event)&AMETA_SHIFT_ON)!=0;
            if(key>=AKEYCODE_A && key<=AKEYCODE_Z) ch=(shift?'A':'a')+key-AKEYCODE_A;
            else if(key>=AKEYCODE_0 && key<=AKEYCODE_9) ch='0'+key-AKEYCODE_0;
            else if(key==AKEYCODE_SPACE) ch=' ';
            else if(key==AKEYCODE_DEL) ch=8;
            else if(key==AKEYCODE_ENTER || key==AKEYCODE_NUMPAD_ENTER) ch=13;
            else if(key==AKEYCODE_MINUS) ch='-';
            else if(key==AKEYCODE_APOSTROPHE) ch='\'';
            else if(key==AKEYCODE_PERIOD) ch='.';
            if(ch && (ui_library_key(u,ch,seconds()) || ui_search_key(u,ch,seconds()))) e->dirty=true;
            return 1;
        }
        if(AKeyEvent_getAction(event)==AKEY_EVENT_ACTION_UP) {
            if(!ui_back(u)) ANativeActivity_finish(app->activity);
            e->dirty=true;
        }
        return 1;
    }
    if(AInputEvent_getType(event)!=AINPUT_EVENT_TYPE_MOTION || !e->ready || e->scale<=0) return 0;
    int raw=AMotionEvent_getAction(event),action=raw&AMOTION_EVENT_ACTION_MASK;
    if(action==AMOTION_EVENT_ACTION_CANCEL) { ui_cancel(u); return 1; }
    if(action==AMOTION_EVENT_ACTION_DOWN) u->pointer=AMotionEvent_getPointerId(event,0);
    size_t count=AMotionEvent_getPointerCount(event),index=count;
    for(size_t i=0;i<count;++i) if(AMotionEvent_getPointerId(event,i)==u->pointer) { index=i; break; }
    if(index==count) return 1;
    if(action==AMOTION_EVENT_ACTION_POINTER_UP) {
        int lifted=(raw&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        if((size_t)lifted==index) ui_cancel(u);
        return 1;
    }
    if(action!=AMOTION_EVENT_ACTION_DOWN && action!=AMOTION_EVENT_ACTION_UP && action!=AMOTION_EVENT_ACTION_MOVE) return 1;
    ui_touch(u,action,AMotionEvent_getX(event,index)/e->scale,AMotionEvent_getY(event,index)/e->scale,seconds());
    if(action==AMOTION_EVENT_ACTION_UP) u->pointer=-1;
    e->dirty=true; return 1;
}

void android_main(struct android_app *app) {
    // Large vertex staging array stays off the NativeActivity thread's small stack.
    static Engine e;
    memset(&e,0,sizeof(e)); e.app=app; e.scale=1;
    e.display=EGL_NO_DISPLAY; e.surface=EGL_NO_SURFACE; e.context=EGL_NO_CONTEXT;
    UiState saved,*restore=NULL;
    if(app->savedState && app->savedStateSize==sizeof(saved)) { memcpy(&saved,app->savedState,sizeof(saved)); restore=&saved; }
    ui_init(&e.ui,restore); atomic_init(&e.font_status,0);
    app->userData=&e; app->onAppCmd=on_cmd; app->onInputEvent=on_input;
    if(pthread_create(&e.loader,NULL,load_font,&e)!=0) { LOGE("font worker failed"); ANativeActivity_finish(app->activity); return; }
    playback_ui(app->looper);
    if(!library_start(app->activity->internalDataPath,app->looper)) {
        snprintf(e.ui.library.message,256,"音乐库启动失败，请退出后重试");
        LOGE("library worker failed");
    }
    ui_library_refresh(&e.ui);
    playback_view(&e.ui.playback); ui_player_sync(&e.ui);
    bool settings_started=settings_start(app->activity->internalDataPath,source_wake,app->activity);
    bool cover_started=cover_start(app->activity->internalDataPath,source_wake,app->activity);
    bool sources_started=source_start(app->activity->internalDataPath,source_bridge_fetch,source_bridge_http,source_wake,app->activity);
    bool search_started=search_start(source_bridge_search_http,source_wake,app->activity);
    bool cache_started=cache_start(app->activity->internalDataPath,cache_bridge_io,source_wake,app->activity);
    double previous=seconds();
    while(!app->destroyRequested) {
        int status=atomic_load_explicit(&e.font_status,memory_order_acquire);
        if(status<0) { LOGE("font asset unavailable"); ANativeActivity_finish(app->activity); }
        bool visible=app->window && e.resumed && e.focused;
        double now=seconds();
        if(visible && status==1 && !e.ready && now>=e.retry_at) engine_create(&e);
        bool animate=visible && ((!e.ui.pressed && fabsf(e.ui.velocity)>3) || e.ui.toast_until>0 || ui_player_animating(&e.ui));
        int timeout=visible && e.ready && (e.dirty||animate)?0:visible && !e.ready?100:-1;
        int events; struct android_poll_source *source=NULL;
        int result=ALooper_pollOnce(timeout,NULL,&events,(void **)&source);
        if(result==ALOOPER_POLL_ERROR) { LOGE("looper failed"); break; }
        if(result>=0 && source) source->process(app,source);
        if(app->destroyRequested) break;
        if(playback_view(&e.ui.playback)) { ui_player_sync(&e.ui); e.dirty=true; }
        const char *cover_url=e.ui.playback.count?e.ui.playback.current.song.cover:"";
        if(strcmp(cover_url,e.cover_url) || e.ui.cover_retry_requested) {
            snprintf(e.cover_url,sizeof(e.cover_url),"%s",cover_url);
            e.ui.cover_retry_requested=false;
            free(e.cover.pixels); e.cover=(CoverImage){0};
            if(e.ready) render_cover_upload(&e.renderer,NULL,0,0);
            e.ui.cover_loading=cover_started && cover_url[0];
            if(cover_started) cover_request(cover_url);
            e.ui.cover_fade=0; e.dirty=true;
        }
        CoverImage next={0};
        if(cover_started && cover_take(&next)) {
            free(e.cover.pixels); e.cover=next; e.ui.cover_loading=false;
            if(e.ready) render_cover_upload(&e.renderer,next.pixels,next.width,next.height);
            e.ui.cover_fade=0; e.dirty=true;
        }
        if(e.ui.playback_requested) { e.ui.playback_requested=false; playback_ensure(app->activity); }
        if(library_view(&e.ui.library)) { ui_library_sync(&e.ui); e.dirty=true; }
        if(settings_view(&e.ui.settings)) { settings_palette(e.ui.settings.theme); e.dirty=true; }
        if(cache_view(&e.ui.cache)) e.dirty=true;
        if(source_view(&e.ui.sources)) e.dirty=true;
        if(search_view(&e.ui.search)) { ui_search_sync(&e.ui); e.dirty=true; }
        if(e.ui.paste_requested) { e.ui.paste_requested=false; source_bridge_paste(app->activity,false); }
        if(e.ui.search_paste_requested) { e.ui.search_paste_requested=false; source_bridge_paste(app->activity,1); }
        if(e.ui.library_paste_requested) { e.ui.library_paste_requested=false; source_bridge_paste(app->activity,2); }
        now=seconds(); float dt=(float)fmin(now-previous,.05); previous=now;
        if(app->window && e.ready && e.resumed && e.focused) {
            bool moved=ui_tick(&e.ui,dt);
            if(e.dirty||moved||e.ui.toast_until>0) {
                if(!draw(&e,now)) { LOGE("surface lost: 0x%x",eglGetError()); engine_destroy(&e); e.retry_at=now+1; }
            }
        }
    }
    library_ui(NULL); // 库线程为进程级，继续处理后台实际播放的历史记录。
    playback_ui(NULL); // 只解除 UI 订阅，进程级队列与前台服务继续播放。
    source_bridge_cancel(app->activity);
    if(cover_started) cover_stop();
    free(e.cover.pixels);
    if(cache_started) cache_stop();
    if(settings_started) settings_stop();
    if(search_started) search_stop();
    if(sources_started) source_stop();
    engine_destroy(&e); pthread_join(e.loader,NULL); free(e.atlas);
    LOGI("native activity finished");
}
