// Batched rounded rectangles and SDF text; approach inspired by CUIDemo.
#include "render.h"
#include <android/log.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct { uint32_t cp; int x, y, w, h, xo, yo; float advance; } Glyph;
#include "font_data.h"

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 p; layout(location=1) in vec2 uv;"
    "layout(location=2) in vec2 local; layout(location=3) in vec2 halfSize;"
    "layout(location=4) in vec4 color; layout(location=5) in float radius;"
    "layout(location=6) in float mode; uniform vec2 resolution;"
    "out vec2 vUV; out vec2 vLocal; out vec2 vHalf; out vec4 vColor;"
    "out float vRadius; out float vMode;"
    "void main(){ gl_Position=vec4(p.x/resolution.x*2.-1.,1.-p.y/resolution.y*2.,0,1);"
    "vUV=uv;vLocal=local;vHalf=halfSize;vColor=color;vRadius=radius;vMode=mode;}";
static const char *FS =
    "#version 300 es\nprecision highp float;"
    "in vec2 vUV; in vec2 vLocal; in vec2 vHalf; in vec4 vColor;"
    "in float vRadius; in float vMode; uniform sampler2D atlas; out vec4 frag;"
    "void main(){float a; if(vMode>0.5 && vMode<1.5){float d=texture(atlas,vUV).r;"
    "float aa=max(fwidth(d)*0.75,0.005); a=smoothstep(0.502-aa,0.502+aa,d);"
    "}else{vec2 q=abs(vLocal)-vHalf+vRadius;"
    "float d=length(max(q,vec2(0)))+min(max(q.x,q.y),0.)-vRadius;"
    "float aa=max(fwidth(d),0.25); a=1.-smoothstep(-aa,aa,d);}"
    "vec4 c=vColor; if(vMode>1.5){vec4 t=texture(atlas,vUV);c*=t;}"
    "a*=c.a;frag=vec4(c.rgb*a,a);}";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, "LXMusicC", "shader: %s", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

bool render_create(Renderer *r, const unsigned char *atlas) {
    GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    r->program = glCreateProgram();
    glAttachShader(r->program, vs); glAttachShader(r->program, fs); glLinkProgram(r->program);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok; glGetProgramiv(r->program, GL_LINK_STATUS, &ok);
    if (!ok) { render_destroy(r); return false; }
    r->resolution = glGetUniformLocation(r->program, "resolution");
    glGenVertexArrays(1, &r->vao); glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo); glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    const int sizes[] = {2,2,2,2,4,1,1};
    const size_t offsets[] = {offsetof(Vertex,pos),offsetof(Vertex,uv),offsetof(Vertex,local),
        offsetof(Vertex,half),offsetof(Vertex,color),offsetof(Vertex,radius),offsetof(Vertex,mode)};
    for (GLuint i = 0; i < 7; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsets[i]);
    }
    glGenTextures(1, &r->atlas); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, r->atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_SIZE, FONT_ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUseProgram(r->program); glUniform1i(glGetUniformLocation(r->program, "atlas"), 0);
    if (glGetError() != GL_NO_ERROR) { render_destroy(r); return false; }
    return true;
}

void render_destroy(Renderer *r) {
    glDeleteTextures(1, &r->atlas); glDeleteTextures(1, &r->cover); glDeleteBuffers(1, &r->vbo);
    glDeleteVertexArrays(1, &r->vao); glDeleteProgram(r->program);
    r->cover = r->atlas = r->vbo = r->vao = r->program = 0; r->count = 0;
}

void render_begin(Renderer *r, int width, int height, float scale) {
    r->width = width/scale; r->height = height/scale; r->scale = scale;
    r->pixel_w = width; r->pixel_h = height; r->count = 0;
    glViewport(0,0,width,height); glDisable(GL_SCISSOR_TEST);
    glClearColor(0.035f,0.047f,0.065f,1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r->program); glBindVertexArray(r->vao);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, r->atlas);
    glUniform2f(r->resolution, r->width, r->height);
}

void render_flush(Renderer *r) {
    if (!r->count) return;
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(r->count*sizeof(Vertex)), r->vertices, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES,0,r->count); r->count = 0;
}

void render_clip(Renderer *r, Rect rect) {
    render_flush(r); glEnable(GL_SCISSOR_TEST);
    int x = (int)ceilf(fmaxf(0,rect.x*r->scale));
    int top = (int)ceilf(fmaxf(0,rect.y*r->scale));
    int right = (int)floorf(fminf(r->pixel_w,(rect.x+rect.w)*r->scale));
    int bottom = (int)floorf(fminf(r->pixel_h,(rect.y+rect.h)*r->scale));
    glScissor(x, r->pixel_h-bottom, right>x?right-x:0, bottom>top?bottom-top:0);
}
void render_unclip(Renderer *r) { render_flush(r); glDisable(GL_SCISSOR_TEST); }

static void quad(Renderer *r, Rect b, Rect uv, float radius, Color c, float mode) {
    if (b.w <= 0 || b.h <= 0) return;
    if (r->count+6 > RENDER_QUADS*6) render_flush(r);
    const int corners[6] = {0,1,2,0,2,3};
    for (int i=0; i<6; ++i) {
        int k=corners[i]; float x=(k==1||k==2)?1:0, y=k>=2?1:0;
        Vertex v = {{b.x+x*b.w,b.y+y*b.h},{uv.x+x*uv.w,uv.y+y*uv.h},
            {(x-.5f)*b.w,(y-.5f)*b.h},{b.w/2,b.h/2},{c.r,c.g,c.b,c.a},radius,mode};
        r->vertices[r->count++] = v;
    }
}
void render_rect(Renderer *r, Rect b, float radius, Color c) {
    quad(r,b,(Rect){0},fminf(radius,fminf(b.w,b.h)/2),c,0);
}

bool render_cover_upload(Renderer *r,const unsigned char *pixels,int width,int height) {
    render_flush(r);
    glDeleteTextures(1,&r->cover); r->cover=0;
    r->cover_width=r->cover_height=0;
    if(!pixels) return true;
    if(width<1 || height<1 || width>512 || height>512) return false;
    glGenTextures(1,&r->cover); glBindTexture(GL_TEXTURE_2D,r->cover);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    bool ok=glGetError()==GL_NO_ERROR;
    if(!ok) { glDeleteTextures(1,&r->cover); r->cover=0; }
    else { r->cover_width=width; r->cover_height=height; }
    glBindTexture(GL_TEXTURE_2D,r->atlas); return ok;
}
void render_cover(Renderer *r,Rect b,float radius,float opacity) {
    if(!r->cover) return;
    float aspect=(float)r->cover_width/r->cover_height;
    Rect uv={0,0,1,1};
    if(aspect>1) { uv.w=1/aspect; uv.x=(1-uv.w)/2; }
    else { uv.h=aspect; uv.y=(1-uv.h)/2; }
    render_flush(r); glBindTexture(GL_TEXTURE_2D,r->cover);
    quad(r,b,uv,radius,(Color){1,1,1,opacity},2);
    render_flush(r); glBindTexture(GL_TEXTURE_2D,r->atlas);
}

static uint32_t next_cp(const char **p) {
    const unsigned char *s=(const unsigned char *)*p;
    uint32_t cp=*s++; int n=0;
    if (cp>=0xc2 && cp<0xe0) { cp &= 31; n=1; }
    else if (cp>=0xe0 && cp<0xf0) { cp &= 15; n=2; }
    else if (cp>=0xf0 && cp<0xf5) { cp &= 7; n=3; }
    else if (cp>=128) cp='?';
    while (n--) {
        if ((*s&0xc0)!=0x80) { cp='?'; break; }
        cp=(cp<<6)|(*s++&63);
    }
    *p=(const char *)s; return cp;
}
static const Glyph *glyph(uint32_t cp) {
    size_t lo=0,hi=sizeof(GLYPHS)/sizeof(GLYPHS[0]);
    while (lo<hi) { size_t m=(lo+hi)/2;
        if (GLYPHS[m].cp<cp) lo=m+1; else hi=m;
    }
    if (lo<sizeof(GLYPHS)/sizeof(GLYPHS[0]) && GLYPHS[lo].cp==cp) return &GLYPHS[lo];
    return glyph('?');
}
void render_text(Renderer *r, float x, float baseline, float size, Color color, const char *text) {
    float s=size/40;
    while (*text) {
        const Glyph *g=glyph(next_cp(&text));
        quad(r,(Rect){x+g->xo*s,baseline+g->yo*s,g->w*s,g->h*s},
            (Rect){g->x/(float)FONT_ATLAS_SIZE,g->y/(float)FONT_ATLAS_SIZE,
                g->w/(float)FONT_ATLAS_SIZE,g->h/(float)FONT_ATLAS_SIZE},0,color,1);
        x+=g->advance*s;
    }
}
float render_text_width(const char *text, float size) {
    float w=0; while (*text) w+=glyph(next_cp(&text))->advance*size/40; return w;
}
