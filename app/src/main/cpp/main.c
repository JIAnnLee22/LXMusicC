// LXMusicC 骨架：NativeActivity + EGL + GLES 3.0 的最小可运行循环。
// 界面、渲染、交互会在后续功能集中逐步替换（见 PLAN.md 的 01-ui-shell）。

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "LXMusicC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct Engine {
    struct android_app *app;
    ANativeWindow *window;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    bool ready;
    double start_time;
} Engine;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static bool engine_create(Engine *e) {
    if (e->ready || !e->window) {
        return false;
    }
    e->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, NULL, NULL)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint config_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(e->display, config_attrs, &config, 1, &config_count) || config_count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    EGLint native_visual = 0;
    eglGetConfigAttrib(e->display, config, EGL_NATIVE_VISUAL_ID, &native_visual);
    ANativeWindow_setBuffersGeometry(e->window, 0, 0, native_visual);

    e->surface = eglCreateWindowSurface(e->display, config, e->window, NULL);
    if (e->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e->context = eglCreateContext(e->display, config, EGL_NO_CONTEXT, context_attrs);
    if (e->context == EGL_NO_CONTEXT || !eglMakeCurrent(e->display, e->surface, e->surface, e->context)) {
        LOGE("eglCreateContext/eglMakeCurrent failed");
        return false;
    }
    eglSwapInterval(e->display, 1);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    e->ready = true;
    LOGI("GLES ready: %s", (const char *) glGetString(GL_VERSION));
    return true;
}

static void engine_destroy(Engine *e) {
    if (e->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (e->context != EGL_NO_CONTEXT) eglDestroyContext(e->display, e->context);
        if (e->surface != EGL_NO_SURFACE) eglDestroySurface(e->display, e->surface);
        eglTerminate(e->display);
    }
    e->context = EGL_NO_CONTEXT;
    e->surface = EGL_NO_SURFACE;
    e->display = EGL_NO_DISPLAY;
    e->ready = false;
}

static void engine_draw(Engine *e) {
    if (!e->ready) {
        return;
    }
    EGLint width = 0;
    EGLint height = 0;
    eglQuerySurface(e->display, e->surface, EGL_WIDTH, &width);
    eglQuerySurface(e->display, e->surface, EGL_HEIGHT, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    // 骨架画面：呼吸的深色背景 + 一条随时间移动的高亮带，用来确认渲染循环在跑。
    const float t = (float) (now_seconds() - e->start_time);
    const float pulse = 0.5f + 0.5f * sinf(t * 1.2f);
    glViewport(0, 0, width, height);
    glClearColor(0.02f + 0.03f * pulse, 0.03f, 0.07f + 0.05f * pulse, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, (int) ((0.5f + 0.4f * sinf(t * 0.7f)) * (float) height), width, height / 24);
    glClearColor(0.25f, 0.45f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    eglSwapBuffers(e->display, e->surface);
}

static void on_app_cmd(struct android_app *app, int32_t cmd) {
    Engine *e = app->userData;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            e->window = app->window;
            engine_create(e);
            break;
        case APP_CMD_TERM_WINDOW:
            engine_destroy(e);
            e->window = NULL;
            break;
        case APP_CMD_GAINED_FOCUS:
            e->start_time = now_seconds();
            break;
        default:
            break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *event) {
    (void) app;
    (void) event;
    return 0;   // 01-ui-shell 里接管触摸
}

void android_main(struct android_app *app) {
    Engine engine;
    memset(&engine, 0, sizeof(engine));
    engine.app = app;
    engine.display = EGL_NO_DISPLAY;
    engine.surface = EGL_NO_SURFACE;
    engine.context = EGL_NO_CONTEXT;
    engine.start_time = now_seconds();

    app->userData = &engine;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;

    while (!app->destroyRequested) {
        int events = 0;
        struct android_poll_source *source = NULL;
        const int timeout = engine.ready ? 16 : -1;
        if (ALooper_pollOnce(timeout, NULL, &events, (void **) &source) >= 0 && source) {
            source->process(app, source);
        }
        engine_draw(&engine);
    }

    engine_destroy(&engine);
    LOGI("native activity finished");
}
