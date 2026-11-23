#ifndef LXM_RENDER_H
#define LXM_RENDER_H
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { float x, y, w, h; } Rect;
typedef struct { float r, g, b, a; } Color;
typedef struct {
    float pos[2], uv[2], local[2], half[2], color[4], radius, mode;
} Vertex;
#define RENDER_QUADS 4096
#define FONT_ATLAS_SIZE 4096
#define FONT_ATLAS_BYTES (FONT_ATLAS_SIZE * FONT_ATLAS_SIZE)
typedef struct {
    GLuint program, vao, vbo, atlas, cover;
    int cover_width, cover_height;
    GLint resolution;
    float width, height, scale, offset_x, opacity;
    int pixel_w, pixel_h, count;
    Rect clip; // CPU 剔除视口外图元，长页滚动不向 GPU 上传不可见内容。
    Vertex vertices[RENDER_QUADS * 6];
} Renderer;

// atlas 为 FONT_ATLAS_SIZE 方形单通道离线 SDF；调用者负责后台读取及释放。
bool render_create(Renderer *r, const unsigned char *atlas);
void render_destroy(Renderer *r);
void render_begin(Renderer *r, int width, int height, float scale);
void render_flush(Renderer *r);
/* 仅影响绘制，不改变逻辑布局；页面淡入/预测返回预览。 */
void render_transform(Renderer *r,float offset_x,float opacity);
void render_clip(Renderer *r, Rect rect);
void render_unclip(Renderer *r);
void render_rect(Renderer *r, Rect rect, float radius, Color color);
/* RGBA CPU 图由后台解码，GL 上传与销毁只在拥有上下文的线程执行。 */
bool render_cover_upload(Renderer *r,const unsigned char *pixels,int width,int height);
void render_cover(Renderer *r,Rect rect,float radius,float opacity);
void render_text(Renderer *r, float x, float baseline, float size, Color color, const char *text);
float render_text_width(const char *text, float size);
/* UTF-8 按实际字形宽度换行，返回下一行起点，用于状态/错误说明。 */
float render_paragraph(Renderer *r,float x,float y,float width,float size,Color color,const char *text);
#endif
