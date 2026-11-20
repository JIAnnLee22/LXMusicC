/*
 * 封面模块：单 C 工作线程 + 最新请求单槽 + 私有磁盘缓存 + stb_image 解码。
 *
 * - 请求切换：cover_request() 立刻更新代次、清空尚未 take 的待发布图片，旧请求的下载/解码结果
 *   即使晚到也不会发布（发布前再比较代次）。因此切歌不会闪现上一首的封面。
 * - 所有权：worker 解码出的 pixels 由本模块持有到 cover_take()；take 之后归调用者（GL 上传后 free）。
 *   父模块自己保留的“当前 CPU 图”不在这里管理，本模块不释放已 take 的指针。
 * - 磁盘缓存：64 槽，URL 的 FNV-1a 64 位哈希选槽，但槽内必须存完整 URL 并逐字节比对才算命中，
 *   避免哈希碰撞把别的歌的封面当成命中。缓存内容是“解码缩放后的 RGBA”（最长边 ≤512，≤1 MiB），
 *   所以单槽远低于 2 MiB + 头上限；原图不落盘。写盘为 0600 同目录 tmp + rename，损坏或碰撞时
 *   删除该槽、重新下载；缓存写失败只影响下次冷启动，不影响本次显示。
 * - 失败（无网络、非 JPEG/PNG、尺寸超限、解码失败）发布空封面（width=height=0）而不是旧图，
 *   父模块据此显示占位，并可以对同一地址再次 cover_request() 重试。
 * - 所有磁盘/网络/解码都在 worker；GL 线程只做 take/cancelled。
 */
#include "cover.h"
#include "cover_bridge.h"
#include <android/log.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#include "vendor/stb_image.h"

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,"LXMusicC",__VA_ARGS__)

#define COVER_SLOTS 64
#define COVER_BYTES (2u*1024u*1024u)          /* 单槽内容上限 2 MiB */
#define COVER_SOURCE_EDGE 2048                /* 原图解码前限制单边 */
#define COVER_SOURCE_PIXELS (4u*1024u*1024u)  /* 原图解码前限制总像素 */
#define COVER_OUT_EDGE 512                    /* 输出最长边 */
#define COVER_MAGIC "LXCV\1\0\0\0"
#define COVER_HEADER_BYTES 20u                /* magic + u32 宽 + u32 高 + u32 URL 长度 */
#define COVER_PATH_BYTES 1100

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool running,started,pending;
    char request[COVER_URL_BYTES];   /* 最新请求地址；空串表示取消 */
    uint64_t requested_token;        /* 最新请求的代次 */
    uint64_t current_token;          /* 当前有效的代次（只有它的结果可以发布） */
    uint64_t next_token;
    CoverImage ready;                /* 待 take 的图片，所有权属本模块 */
    bool has_ready;
    char directory[1024];            /* 仅在 worker 未运行时改写 */
    SourceWake wake;
    void *context;                   /* ANativeActivity，交给 cover_bridge_fetch */
} cover={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};

/* 取消位：与 worker 共享，Kotlin 下载线程只读比较，不进锁、不做 IO。 */
static atomic_uint_fast64_t generation;

static uint64_t fnv1a(const char *text) {
    uint64_t hash=14695981039346656037ull;
    for(const unsigned char *p=(const unsigned char *)text;*p;++p) {
        hash^=*p;
        hash*=1099511628211ull;
    }
    return hash;
}

static void slot_path(char *out,size_t capacity,const char *url,const char *suffix) {
    unsigned slot=(unsigned)(fnv1a(url)%COVER_SLOTS);
    snprintf(out,capacity,"%s/cover-%02u.%s",cover.directory,slot,suffix);
}

static bool put32(FILE *file,uint32_t value) {
    unsigned char bytes[4]={value&255,(value>>8)&255,(value>>16)&255,(value>>24)&255};
    return fwrite(bytes,1,4,file)==4;
}

static bool get32(FILE *file,uint32_t *value) {
    unsigned char bytes[4];
    if(fread(bytes,1,4,file)!=4) return false;
    *value=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
    return true;
}

/* 写盘失败不是错误路径：返回 false 时只记日志，本次图片仍然发布。 */
static bool save_slot(const char *url,const unsigned char *pixels,int width,int height) {
    size_t url_bytes=strlen(url),pixel_bytes=(size_t)width*(size_t)height*4u;
    if(!url_bytes || url_bytes>=COVER_URL_BYTES || width<=0 || height<=0) return false;
    if(pixel_bytes>COVER_BYTES || COVER_HEADER_BYTES+url_bytes+pixel_bytes>COVER_BYTES+COVER_HEADER_BYTES+COVER_URL_BYTES)
        return false;
    char temporary[COVER_PATH_BYTES],path[COVER_PATH_BYTES];
    slot_path(temporary,sizeof(temporary),url,"tmp");
    slot_path(path,sizeof(path),url,"bin");
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC,0600);
    FILE *file=fd>=0?fdopen(fd,"wb"):NULL;
    if(!file) { if(fd>=0) close(fd); return false; }
    bool ok=fwrite(COVER_MAGIC,1,8,file)==8 && put32(file,(uint32_t)width) && put32(file,(uint32_t)height) &&
        put32(file,(uint32_t)url_bytes) && fwrite(url,1,url_bytes,file)==url_bytes &&
        fwrite(pixels,1,pixel_bytes,file)==pixel_bytes;
    if(ok) ok=fflush(file)==0 && fsync(fd)==0;
    if(fclose(file)!=0) ok=false;
    if(ok && rename(temporary,path)==0) return true;
    unlink(temporary);
    return false;
}

/* 命中返回 true 并给出带 token 的图片；任何不一致都删除该槽并让 worker 重新下载。 */
static bool load_slot(const char *url,uint64_t token,CoverImage *out) {
    char path[COVER_PATH_BYTES];
    slot_path(path,sizeof(path),url,"bin");
    FILE *file=fopen(path,"rb");
    if(!file) return false;
    char magic[8],stored[COVER_URL_BYTES];
    uint32_t width=0,height=0,url_bytes=0;
    size_t expected=strlen(url);
    bool ok=fread(magic,1,8,file)==8 && !memcmp(magic,COVER_MAGIC,8) && get32(file,&width) && get32(file,&height) &&
        get32(file,&url_bytes) && width>0 && height>0 && width<=COVER_OUT_EDGE && height<=COVER_OUT_EDGE &&
        url_bytes>0 && url_bytes<COVER_URL_BYTES && (size_t)url_bytes==expected;
    unsigned char *pixels=NULL;
    if(ok) {
        ok=fread(stored,1,url_bytes,file)==url_bytes && !memcmp(stored,url,url_bytes);
        size_t pixel_bytes=(size_t)width*(size_t)height*4u;
        if(ok && pixel_bytes<=COVER_BYTES) {
            pixels=malloc(pixel_bytes);
            ok=pixels && fread(pixels,1,pixel_bytes,file)==pixel_bytes;
        } else ok=false;
    }
    if(ok) ok=fgetc(file)==EOF && !ferror(file);
    fclose(file);
    if(!ok) {
        /* 截断、版本不符或 URL 不匹配（哈希碰撞）都按损坏处理。 */
        free(pixels);
        unlink(path);
        return false;
    }
    out->pixels=pixels;
    out->width=(int)width;
    out->height=(int)height;
    out->token=token;
    return true;
}

/* 只认 JPEG/PNG 魔数；BMP/WebP/GIF/AVIF 等直接拒绝，避免误信扩展名或 Content-Type。 */
static bool jpeg_or_png(const unsigned char *data,size_t size) {
    static const unsigned char png[8]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if(size>=3 && data[0]==0xff && data[1]==0xd8 && data[2]==0xff) return true;
    return size>=8 && !memcmp(data,png,8);
}

/* 中心对齐双线性缩放；输出边长 ≤512，成本可忽略。返回新缓冲区。 */
static unsigned char *scale_rgba(const unsigned char *source,int width,int height,int out_width,int out_height) {
    unsigned char *output=malloc((size_t)out_width*(size_t)out_height*4u);
    if(!output) return NULL;
    for(int y=0;y<out_height;++y) {
        float fy=((float)y+0.5f)*(float)height/(float)out_height-0.5f;
        int y0=(int)floorf(fy);
        float ty=fy-(float)y0;
        int y1=y0+1;
        if(y0<0) { y0=0; ty=0; }
        if(y1>height-1) y1=height-1;
        const unsigned char *row0=source+(size_t)y0*(size_t)width*4u;
        const unsigned char *row1=source+(size_t)y1*(size_t)width*4u;
        unsigned char *row=output+(size_t)y*(size_t)out_width*4u;
        for(int x=0;x<out_width;++x) {
            float fx=((float)x+0.5f)*(float)width/(float)out_width-0.5f;
            int x0=(int)floorf(fx);
            float tx=fx-(float)x0;
            int x1=x0+1;
            if(x0<0) { x0=0; tx=0; }
            if(x1>width-1) x1=width-1;
            const unsigned char *p00=row0+(size_t)x0*4u,*p01=row0+(size_t)x1*4u;
            const unsigned char *p10=row1+(size_t)x0*4u,*p11=row1+(size_t)x1*4u;
            for(int c=0;c<4;++c) {
                float top=(float)p00[c]+((float)p01[c]-(float)p00[c])*tx;
                float bottom=(float)p10[c]+((float)p11[c]-(float)p10[c])*tx;
                float value=top+(bottom-top)*ty;
                row[x*4+c]=(unsigned char)(value<0.0f?0:value>255.0f?255:value+0.5f);
            }
        }
    }
    return output;
}

/* 返回 RGBA8 缓冲区与输出尺寸；失败返回 NULL 并记录可读原因。 */
static unsigned char *decode(const unsigned char *data,size_t size,int *out_width,int *out_height) {
    if(!jpeg_or_png(data,size)) {
        __android_log_print(ANDROID_LOG_WARN,"LXMusicC","cover: 只支持 JPEG/PNG 封面");
        return NULL;
    }
    int width=0,height=0,components=0;
    if(!stbi_info_from_memory(data,(int)size,&width,&height,&components)) {
        LOGW("cover: 图片头解析失败"); return NULL;
    }
    if(width<=0 || height<=0 || width>COVER_SOURCE_EDGE || height>COVER_SOURCE_EDGE ||
       (uint64_t)width*(uint64_t)height>COVER_SOURCE_PIXELS) {
        LOGW("cover: 原图尺寸超限 %dx%d",width,height); return NULL;
    }
    unsigned char *pixels=stbi_load_from_memory(data,(int)size,&width,&height,&components,4);
    if(!pixels) { LOGW("cover: 解码失败 %.80s",stbi_failure_reason()?stbi_failure_reason():""); return NULL; }
    int longest=width>height?width:height;
    if(longest<=COVER_OUT_EDGE) {
        *out_width=width; *out_height=height; return pixels;
    }
    int scaled_w=(int)((int64_t)width*COVER_OUT_EDGE/longest),scaled_h=(int)((int64_t)height*COVER_OUT_EDGE/longest);
    scaled_w=scaled_w<1?1:scaled_w;
    scaled_h=scaled_h<1?1:scaled_h;
    unsigned char *scaled=scale_rgba(pixels,width,height,scaled_w,scaled_h);
    free(pixels);
    if(!scaled) { LOGW("cover: 缩放内存不足"); return NULL; }
    *out_width=scaled_w; *out_height=scaled_h;
    return scaled;
}

/* 只有代次仍然有效才发布；切换请求后旧结果在这里被丢弃。 */
static void publish(uint64_t token,unsigned char *pixels,int width,int height) {
    SourceWake wake;
    void *context;
    pthread_mutex_lock(&cover.lock);
    bool live=cover.running && token!=0 && token==cover.current_token;
    if(live) {
        free(cover.ready.pixels);
        cover.ready.pixels=pixels;
        cover.ready.width=width;
        cover.ready.height=height;
        cover.ready.token=token;
        cover.has_ready=true;
    }
    wake=live?cover.wake:NULL;
    context=cover.context;
    pthread_mutex_unlock(&cover.lock);
    if(!live) free(pixels);
    else if(wake) wake(context);
}

static void process(const char *url,uint64_t token) {
    if(!url[0]) { publish(token,NULL,0,0); return; }   /* 取消：发布空封面让 UI 回到占位 */
    CoverImage cached={0};
    if(load_slot(url,token,&cached)) {
        if(cover_cancelled(token)) { free(cached.pixels); return; }
        publish(token,cached.pixels,cached.width,cached.height);
        return;
    }
    if(cover_cancelled(token)) return;
    size_t size=0;
    unsigned char *data=cover_bridge_fetch(cover.context,url,token,&size);
    if(cover_cancelled(token) || !data || !size || size>COVER_BYTES) {
        free(data);
        if(!cover_cancelled(token)) publish(token,NULL,0,0);
        return;
    }
    int width=0,height=0;
    unsigned char *pixels=decode(data,size,&width,&height);
    free(data);
    if(!pixels) { publish(token,NULL,0,0); return; }
    if(cover_cancelled(token)) { free(pixels); return; }
    if(!save_slot(url,pixels,width,height)) LOGW("cover: 缓存写入失败，本次仍显示");
    publish(token,pixels,width,height);
}

static void *worker(void *unused) {
    (void)unused;
    for(;;) {
        pthread_mutex_lock(&cover.lock);
        while(cover.running && !cover.pending) pthread_cond_wait(&cover.cond,&cover.lock);
        if(!cover.running) { pthread_mutex_unlock(&cover.lock); break; }
        char url[COVER_URL_BYTES];
        memcpy(url,cover.request,sizeof(url));
        uint64_t token=cover.requested_token;
        cover.pending=false;
        pthread_mutex_unlock(&cover.lock);
        process(url,token);
    }
    return NULL;
}

bool cover_start(const char *directory,SourceWake wake,void *context) {
    if(!directory || !directory[0]) return false;
    pthread_mutex_lock(&cover.lock);
    bool ok=!cover.started &&
        snprintf(cover.directory,sizeof(cover.directory),"%s",directory)<(int)sizeof(cover.directory);
    if(ok) {
        free(cover.ready.pixels);
        memset(&cover.ready,0,sizeof(cover.ready));
        cover.has_ready=false;
        cover.request[0]=0;
        cover.requested_token=cover.current_token=0;
        cover.next_token=1;
        cover.pending=false;
        cover.wake=wake;
        cover.context=context;
        cover.running=true;
        atomic_store(&generation,0);
    }
    pthread_mutex_unlock(&cover.lock);
    if(!ok) return false;
    if(pthread_create(&cover.thread,NULL,worker,NULL)==0) {
        pthread_mutex_lock(&cover.lock); cover.started=true; pthread_mutex_unlock(&cover.lock);
        return true;
    }
    pthread_mutex_lock(&cover.lock);
    cover.running=false;
    cover.wake=NULL;
    cover.context=NULL;
    cover.directory[0]=0;
    pthread_mutex_unlock(&cover.lock);
    LOGW("cover: 工作线程启动失败");
    return false;
}

void cover_stop(void) {
    pthread_mutex_lock(&cover.lock);
    bool join=cover.started;
    cover.running=false;
    atomic_store(&generation,0);   /* 让 Kotlin 取消回调和旧代次立刻失效 */
    pthread_cond_signal(&cover.cond);
    pthread_mutex_unlock(&cover.lock);
    if(join) pthread_join(cover.thread,NULL);
    pthread_mutex_lock(&cover.lock);
    cover.started=false;
    cover.pending=false;
    cover.request[0]=0;
    cover.wake=NULL;
    cover.context=NULL;
    cover.directory[0]=0;
    free(cover.ready.pixels);
    memset(&cover.ready,0,sizeof(cover.ready));
    cover.has_ready=false;
    pthread_mutex_unlock(&cover.lock);
}

void cover_request(const char *url) {
    size_t length=url?strnlen(url,COVER_URL_BYTES):0;
    pthread_mutex_lock(&cover.lock);
    if(!cover.started) { pthread_mutex_unlock(&cover.lock); return; }
    uint64_t token=++cover.next_token;
    if(token==0) token=++cover.next_token;   /* 0 保留给“未请求/已停止” */
    cover.requested_token=token;
    cover.current_token=token;
    atomic_store(&generation,token);
    /* 超长地址按取消处理，避免继续展示上一张封面。 */
    if(length>=COVER_URL_BYTES) length=0;
    memset(cover.request,0,sizeof(cover.request));
    if(length) memcpy(cover.request,url,length);
    /* 切换请求立刻丢弃待发布图片，旧结果不可能再被 take 到。 */
    free(cover.ready.pixels);
    memset(&cover.ready,0,sizeof(cover.ready));
    cover.has_ready=false;
    cover.pending=true;
    pthread_cond_signal(&cover.cond);
    SourceWake wake=cover.wake;
    void *context=cover.context;
    pthread_mutex_unlock(&cover.lock);
    if(wake) wake(context);
}

bool cover_take(CoverImage *out) {
    if(!out) return false;
    bool available=false;
    pthread_mutex_lock(&cover.lock);
    if(cover.has_ready) {
        *out=cover.ready;
        memset(&cover.ready,0,sizeof(cover.ready));
        cover.has_ready=false;
        available=true;
    }
    pthread_mutex_unlock(&cover.lock);
    return available;
}

bool cover_cancelled(uint64_t token) {
    return token==0 || atomic_load(&generation)!=token;
}
