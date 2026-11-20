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
typedef struct {
    GLuint program, vao, vbo, atlas;
    GLint resolution;
    float width, height, scale;
    int pixel_w, pixel_h, count;
    Vertex vertices[RENDER_QUADS * 6];
} Renderer;

// atlas 为 1024x1024 单通道离线 SDF；调用者负责后台读取及释放。
bool render_create(Renderer *r, const unsigned char *atlas);
void render_destroy(Renderer *r);
void render_begin(Renderer *r, int width, int height, float scale);
void render_flush(Renderer *r);
void render_clip(Renderer *r, Rect rect);
void render_unclip(Renderer *r);
void render_rect(Renderer *r, Rect rect, float radius, Color color);
void render_text(Renderer *r, float x, float baseline, float size, Color color, const char *text);
float render_text_width(const char *text, float size);
#endif
