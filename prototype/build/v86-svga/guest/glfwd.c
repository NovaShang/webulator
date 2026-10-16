/* glfwd: a user-mode OpenGL 1.x runtime (opengl32.dll) that forwards to the emulator's GPU.
 *
 * OpenGL state is tracked here and turned into SVGA3D state right before each draw; the emulator's renderer does the
 * fixed-function transform, lighting and texture combiners. Vertex data (client arrays and glBegin/glEnd) is copied
 * into an arena in this process and sent to a ring of scratch buffers on the host; the arena is reused only after a
 * flush, so draws are batched until SwapBuffers. Texture images are read straight from the application's memory,
 * with a flush before returning.
 *
 * Conventions: the host renders in Direct3D's orientation (row 0 at the top) and expects clip z in [0, w]. OpenGL
 * matrices are passed unchanged (column-major OpenGL = row-vector Direct3D in memory) with the projection's z
 * remapped; viewports, scissors and read-backs flip y.
 *
 * Drop opengl32.dll next to an application's executable. */
#include "hc.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdlib.h>

#define MAX_UNITS 4
#define MAX_GL_LIGHTS 8
#define ARENA_BYTES (16 << 20)
#define SCRATCH_VB_BYTES (8 << 20)
#define SCRATCH_IB_BYTES (2 << 20)

typedef struct { float m[16]; } Mat;

typedef struct {
    DWORD sid;
    UINT w, h, levels;
    int compressed;
    GLenum base;                         /* GL base format: GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE, ... */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    float aniso;
    int genmip;
} Tex;

typedef struct { DWORD sid; UINT size; BYTE *mem; BYTE *mapped; } Buf;

typedef struct {
    int on;
    GLint size;                          /* 4 for GL_BGRA colours (bgra set) */
    int bgra;
    GLenum type;
    GLsizei stride;
    const BYTE *ptr;
    Buf *buf;
} Array;

typedef struct {
    GLenum mode, combine_rgb, combine_alpha;
    GLenum src_rgb[3], src_alpha[3], op_rgb[3], op_alpha[3];
    float rgb_scale, alpha_scale, color[4];
    int tex2d;
    GLuint bound;
    Mat stack[4];
    int depth;
    float tc[4];
} Unit;

typedef struct {
    float amb[4], dif[4], spec[4], pos[4], dir[3], spot_exp, spot_cut, att[3];
    int on;
} Light;

typedef struct {
    HDC dc;
    HWND wnd;
    DWORD cid, fb, depth;
    UINT fbw, fbh;
    int inited;

    /* matrices */
    Mat mv[32], proj[4];
    int mv_depth, proj_depth;
    GLenum matrix_mode;
    int xform_dirty;

    /* current vertex attributes */
    float color[4], normal[3];

    /* enables and fixed-function state */
    int depth_test, blend, alpha_test, cull, stencil, scissor_test, fog, lighting, poly_offset, color_material;
    GLenum depth_func, alpha_func, cull_face, front_face, shade, blend_eq, cm_face, cm_mode;
    GLboolean depth_mask, cmask[4];
    GLenum bsrc, bdst, bsrc_a, bdst_a;
    float blend_color[4], alpha_ref;
    GLenum st_func, st_fail, st_zfail, st_pass;
    GLint st_ref;
    GLuint st_mask, st_wmask;
    GLint scissor[4], viewport[4];
    double depth_range[2];
    GLenum fog_mode;
    float fog_start, fog_end, fog_density, fog_color[4];
    float po_factor, po_units, point_size;
    float clear_color[4];
    double clear_depth;
    GLint clear_stencil;
    float model_ambient[4];
    Light light[MAX_GL_LIGHTS];
    float mat_amb[4], mat_dif[4], mat_spec[4], mat_emis[4], mat_shin;

    /* textures */
    Unit unit[MAX_UNITS];
    int active_unit, client_unit;

    /* client arrays */
    Array vertex, colors, normals, texc[MAX_UNITS];
    GLuint array_buffer, element_buffer;
    GLint lock_first, lock_count;

    /* pixel store */
    GLint unpack_align, unpack_row, unpack_skip_rows, unpack_skip_pixels, pack_align;

    /* immediate mode */
    int in_begin;
    GLenum begin_mode;

    /* state already sent to the host */
    DWORD sent_rs[128];
    DWORD sent_ts[8][32];
    int sent_valid;

    GLenum error;
    int list_compiling;
} Ctx;

static Ctx *cur;
static BYTE *arena;
static UINT arena_pos;
static DWORD scratch_vb, scratch_ib;
static UINT scratch_vb_pos, scratch_ib_pos;

/* Objects by name. */
static Tex **textures;
static UINT ntextures;
static Buf **buffers;
static UINT nbuffers;
static GLuint next_tex_name = 1, next_buf_name = 1, next_list = 1;

static void gl_flush(void)
{
    flush();
    arena_pos = 0;
}

/* Make sure the next allocations totalling n bytes need no flush (a flush reuses the arena). */
static void arena_reserve(UINT n)
{
    if(arena_pos + n + 1024 > ARENA_BYTES) gl_flush();
}

/* Arena space for n bytes (16-aligned). */
static BYTE *arena_alloc(UINT n)
{
    n = (n + 15) & ~15u;
    if(arena_pos + n > ARENA_BYTES) gl_flush();
    if(n > ARENA_BYTES) return NULL;
    BYTE *p = arena + arena_pos;
    arena_pos += n;
    return p;
}

/* Send arena bytes to a scratch ring on the host; returns the offset there. Commands run in order, so a later DMA
 * that wraps around cannot clobber data an earlier draw still needs. */
static UINT to_scratch(DWORD sid, UINT *pos, UINT cap, const void *src, UINT n)
{
    n = (n + 15) & ~15u;
    if(*pos + n > cap) *pos = 0;
    UINT off = *pos;
    dma2(sid, 0, src, 0, XFER_TO_HOST, off, 0, n, 1, 0, 0);    /* source: src itself; destination: off */
    *pos += n;
    return off;
}

static Tex *get_tex(GLuint name, int create)
{
    if(!name) return NULL;
    if(name >= ntextures)
    {
        if(!create) return NULL;
        UINT n = ntextures ? ntextures : 256;
        while(n <= name) n *= 2;
        textures = realloc(textures, n * sizeof *textures);
        memset(textures + ntextures, 0, (n - ntextures) * sizeof *textures);
        ntextures = n;
    }
    if(!textures[name] && create)
    {
        Tex *t = calloc(1, sizeof *t);
        t->min_filter = GL_NEAREST_MIPMAP_LINEAR; t->mag_filter = GL_LINEAR;
        t->wrap_s = t->wrap_t = GL_REPEAT; t->aniso = 1;
        textures[name] = t;
    }
    return textures[name];
}

static Buf *get_buf(GLuint name, int create)
{
    if(!name) return NULL;
    if(name >= nbuffers)
    {
        if(!create) return NULL;
        UINT n = nbuffers ? nbuffers : 256;
        while(n <= name) n *= 2;
        buffers = realloc(buffers, n * sizeof *buffers);
        memset(buffers + nbuffers, 0, (n - nbuffers) * sizeof *buffers);
        nbuffers = n;
    }
    if(!buffers[name] && create) buffers[name] = calloc(1, sizeof(Buf));
    return buffers[name];
}

/* ---------- matrices ---------- */

static void mat_identity(Mat *m) { memset(m, 0, sizeof *m); m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1; }
static void mat_mul(Mat *r, const Mat *a, const Mat *b)       /* r = a * b (column-major) */
{
    Mat t;
    for(int c = 0; c < 4; c++) for(int rr = 0; rr < 4; rr++)
    {
        float s = 0;
        for(int k = 0; k < 4; k++) s += a->m[k * 4 + rr] * b->m[c * 4 + k];
        t.m[c * 4 + rr] = s;
    }
    *r = t;
}
static Mat *top(void)
{
    Ctx *c = cur;
    if(c->matrix_mode == GL_PROJECTION) return &c->proj[c->proj_depth];
    if(c->matrix_mode == GL_TEXTURE) { Unit *u = &c->unit[c->active_unit]; return &u->stack[u->depth]; }
    return &c->mv[c->mv_depth];
}
static void touched(void) { if(cur->matrix_mode != GL_TEXTURE) cur->xform_dirty = 1; }
static void mult(const Mat *m) { Mat *t = top(); mat_mul(t, t, m); touched(); }
static void xform4(const Mat *m, const float *v, float *o)
{
    float r[4];
    for(int i = 0; i < 4; i++) r[i] = m->m[i] * v[0] + m->m[4 + i] * v[1] + m->m[8 + i] * v[2] + m->m[12 + i] * v[3];
    memcpy(o, r, sizeof r);
}

/* ---------- framebuffer ---------- */

static void ensure_fb(Ctx *c)
{
    RECT r = { 0, 0, 1, 1 };
    if(c->wnd) GetClientRect(c->wnd, &r);
    UINT w = r.right > 0 ? r.right : 1, h = r.bottom > 0 ? r.bottom : 1;
    if(c->fb && w == c->fbw && h == c->fbh) return;
    if(c->fb) { surface_destroy(c->fb); surface_destroy(c->depth); }
    c->fb = new_id(); c->depth = new_id(); c->fbw = w; c->fbh = h;
    surface_define(c->fb, SF_X8R8G8B8, w, h, 1);
    surface_define(c->depth, SF_Z_D24S8, w, h, 1);
    DWORD *p = cmd(CMD_SETRENDERTARGET, 20);
    p[0] = c->cid; p[1] = RT_COLOR0; p[2] = c->fb; p[3] = 0; p[4] = 0;
    for(DWORD t = RT_DEPTH; t <= RT_STENCIL; t++)
    {
        p = cmd(CMD_SETRENDERTARGET, 20);
        p[0] = c->cid; p[1] = t; p[2] = c->depth; p[3] = 0; p[4] = 0;
    }
}

/* ---------- translating state ---------- */

static DWORD argb(const float *c)
{
    DWORD v = 0;
    for(int i = 0; i < 4; i++)
    {
        float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        DWORD b = (DWORD)(f * 255 + 0.5f);
        v |= b << (i == 3 ? 24 : i == 0 ? 16 : i == 1 ? 8 : 0);
    }
    return v;
}
static DWORD cmp_func(GLenum f) { return f >= GL_NEVER && f <= GL_ALWAYS ? f - GL_NEVER + 1 : 8; }
static DWORD blend_factor(GLenum f)
{
    switch(f)
    {
        case GL_ZERO: return 1; case GL_ONE: return 2; case GL_SRC_COLOR: return 3; case GL_ONE_MINUS_SRC_COLOR: return 4;
        case GL_SRC_ALPHA: return 5; case GL_ONE_MINUS_SRC_ALPHA: return 6; case GL_DST_ALPHA: return 7;
        case GL_ONE_MINUS_DST_ALPHA: return 8; case GL_DST_COLOR: return 9; case GL_ONE_MINUS_DST_COLOR: return 10;
        case GL_SRC_ALPHA_SATURATE: return 11; case GL_CONSTANT_COLOR: case GL_CONSTANT_ALPHA: return 12;
        case GL_ONE_MINUS_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_ALPHA: return 13;
        default: return 2;
    }
}
static DWORD stencil_op(GLenum o)
{
    switch(o)
    {
        case GL_KEEP: return 1; case GL_ZERO: return 2; case GL_REPLACE: return 3; case GL_INCR: return 4;
        case GL_DECR: return 5; case GL_INVERT: return 6; case GL_INCR_WRAP: return 7; case GL_DECR_WRAP: return 8;
        default: return 1;
    }
}
static DWORD blend_eq(GLenum e)
{
    switch(e)
    {
        case GL_FUNC_SUBTRACT: return 2; case GL_FUNC_REVERSE_SUBTRACT: return 3; case GL_MIN: return 4;
        case GL_MAX: return 5; default: return 1;
    }
}

static void set_rs(Ctx *c, DWORD st, DWORD v)
{
    if(c->sent_valid && c->sent_rs[st] == v) return;
    c->sent_rs[st] = v;
    DWORD *p = cmd(CMD_SETRENDERSTATE, 12);
    p[0] = c->cid; p[1] = st; p[2] = v;
}
static void set_ts(Ctx *c, DWORD stage, DWORD name, DWORD v)
{
    if(c->sent_valid && c->sent_ts[stage][name] == v) return;
    c->sent_ts[stage][name] = v;
    DWORD *p = cmd(CMD_SETTEXTURESTATE, 16);
    p[0] = c->cid; p[1] = stage; p[2] = name; p[3] = v;
}

enum { TA_TFACTOR = 1, TA_PREVIOUS = 2, TA_DIFFUSE = 3, TA_TEXTURE = 4, TM_ALPHA = 0x10, TM_ONE_MINUS = 0x20 };
enum { TC_DISABLE = 1, TC_SELECTARG1 = 2, TC_SELECTARG2 = 3, TC_MODULATE = 4, TC_ADD = 5, TC_ADDSIGNED = 6,
    TC_SUBTRACT = 7, TC_BLENDTEXTUREALPHA = 8, TC_MODULATE2X = 12, TC_MODULATE4X = 13, TC_DOTPRODUCT3 = 15,
    TC_ADDSIGNED2X = 17, TC_LERP = 26 };

static DWORD combine_arg(GLenum src, GLenum op)
{
    DWORD a = src == GL_TEXTURE ? TA_TEXTURE : src == GL_CONSTANT ? TA_TFACTOR : src == GL_PRIMARY_COLOR ? TA_DIFFUSE :
        src == GL_PREVIOUS ? TA_PREVIOUS : src >= GL_TEXTURE0 && src < GL_TEXTURE0 + 32 ? TA_TEXTURE : TA_PREVIOUS;
    if(op == GL_ONE_MINUS_SRC_COLOR) a |= TM_ONE_MINUS;
    else if(op == GL_SRC_ALPHA) a |= TM_ALPHA;
    else if(op == GL_ONE_MINUS_SRC_ALPHA) a |= TM_ALPHA | TM_ONE_MINUS;
    return a;
}
static void combine_op(GLenum op, float scale, const GLenum *src, const GLenum *opd, DWORD *top, DWORD *a1, DWORD *a2, DWORD *a0)
{
    DWORD s0 = combine_arg(src[0], opd[0]), s1 = combine_arg(src[1], opd[1]), s2 = combine_arg(src[2], opd[2]);
    *a1 = s0; *a2 = s1; *a0 = s2;
    switch(op)
    {
        case GL_REPLACE: *top = TC_SELECTARG1; break;
        case GL_MODULATE: *top = scale >= 4 ? TC_MODULATE4X : scale >= 2 ? TC_MODULATE2X : TC_MODULATE; break;
        case GL_ADD: *top = TC_ADD; break;
        case GL_ADD_SIGNED: *top = scale >= 2 ? TC_ADDSIGNED2X : TC_ADDSIGNED; break;
        case GL_INTERPOLATE: *top = TC_LERP; *a1 = s0; *a2 = s1; *a0 = s2; break;
        case GL_SUBTRACT: *top = TC_SUBTRACT; break;
        case GL_DOT3_RGB: case GL_DOT3_RGBA: *top = TC_DOTPRODUCT3; break;
        default: *top = TC_MODULATE; break;
    }
}

static DWORD wrap_mode(GLenum w)
{
    switch(w) { case GL_MIRRORED_REPEAT: return 2; case GL_CLAMP: case GL_CLAMP_TO_EDGE: return 3;
        case GL_CLAMP_TO_BORDER: return 4; default: return 1; }
}

/* Texture stages from the texture units. */
static void flush_stages(Ctx *c)
{
    int last = -1;
    for(int i = 0; i < MAX_UNITS; i++)
    {
        Tex *t = get_tex(c->unit[i].bound, 0);
        if(c->unit[i].tex2d && t && t->sid) last = i;
    }
    DWORD tfactor = 0xFFFFFFFF;
    for(int i = 0; i < MAX_UNITS; i++)
    {
        Unit *u = &c->unit[i];
        Tex *t = get_tex(u->bound, 0);
        int on = u->tex2d && t && t->sid;
        if(i > last)
        {
            set_ts(c, i, TS_COLOROP, TC_DISABLE); set_ts(c, i, TS_ALPHAOP, TC_DISABLE);
            set_ts(c, i, TS_BIND_TEXTURE, INVALID_ID);
            continue;
        }
        DWORD cop = TC_SELECTARG1, c1 = TA_PREVIOUS, c2 = TA_PREVIOUS, c0 = TA_PREVIOUS;
        DWORD aop = TC_SELECTARG1, a1 = TA_PREVIOUS, a2 = TA_PREVIOUS, a0 = TA_PREVIOUS;
        if(on)
        {
            int has_color = t->base != GL_ALPHA, has_alpha = t->base == GL_RGBA || t->base == GL_ALPHA ||
                t->base == GL_LUMINANCE_ALPHA || t->base == GL_INTENSITY;
            switch(u->mode)
            {
                case GL_REPLACE:
                    if(has_color) { cop = TC_SELECTARG1; c1 = TA_TEXTURE; }
                    if(has_alpha) { aop = TC_SELECTARG1; a1 = TA_TEXTURE; }
                    break;
                case GL_DECAL:
                    if(has_color) { cop = TC_BLENDTEXTUREALPHA; c1 = TA_TEXTURE; c2 = TA_PREVIOUS; }
                    break;
                case GL_ADD:
                    if(has_color) { cop = TC_ADD; c1 = TA_TEXTURE; c2 = TA_PREVIOUS; }
                    if(has_alpha) { aop = TC_MODULATE; a1 = TA_TEXTURE; a2 = TA_PREVIOUS; }
                    break;
                case GL_BLEND:
                    if(has_color) { cop = TC_LERP; c0 = TA_TEXTURE; c1 = TA_TFACTOR; c2 = TA_PREVIOUS; tfactor = argb(u->color); }
                    if(has_alpha) { aop = TC_MODULATE; a1 = TA_TEXTURE; a2 = TA_PREVIOUS; }
                    break;
                case GL_COMBINE:
                    combine_op(u->combine_rgb, u->rgb_scale, u->src_rgb, u->op_rgb, &cop, &c1, &c2, &c0);
                    combine_op(u->combine_alpha, u->alpha_scale, u->src_alpha, u->op_alpha, &aop, &a1, &a2, &a0);
                    tfactor = argb(u->color);
                    break;
                default:                                     /* GL_MODULATE */
                    if(has_color) { cop = TC_MODULATE; c1 = TA_TEXTURE; c2 = TA_PREVIOUS; }
                    if(has_alpha) { aop = TC_MODULATE; a1 = TA_TEXTURE; a2 = TA_PREVIOUS; }
                    break;
            }
            set_ts(c, i, TS_BIND_TEXTURE, t->sid);
            GLenum mf = t->min_filter;
            DWORD minf = mf == GL_NEAREST || mf == GL_NEAREST_MIPMAP_NEAREST || mf == GL_NEAREST_MIPMAP_LINEAR ? 1 : 2;
            DWORD mipf = mf == GL_NEAREST || mf == GL_LINEAR ? 0 :
                mf == GL_NEAREST_MIPMAP_NEAREST || mf == GL_LINEAR_MIPMAP_NEAREST ? 1 : 2;
            if(t->levels <= 1) mipf = 0;
            set_ts(c, i, TS_MINFILTER, t->aniso > 1 ? 3 : minf);
            set_ts(c, i, TS_MAGFILTER, t->mag_filter == GL_NEAREST ? 1 : 2);
            set_ts(c, i, TS_MIPFILTER, mipf);
            set_ts(c, i, TS_ADDRESSU, wrap_mode(t->wrap_s));
            set_ts(c, i, TS_ADDRESSV, wrap_mode(t->wrap_t));
            set_ts(c, i, TS_TEXTURE_ANISOTROPIC_LEVEL, (DWORD)t->aniso);
        }
        else set_ts(c, i, TS_BIND_TEXTURE, INVALID_ID);
        set_ts(c, i, TS_COLOROP, cop); set_ts(c, i, TS_COLORARG1, c1); set_ts(c, i, TS_COLORARG2, c2);
        set_ts(c, i, TS_COLORARG0, c0);
        set_ts(c, i, TS_ALPHAOP, aop); set_ts(c, i, TS_ALPHAARG1, a1); set_ts(c, i, TS_ALPHAARG2, a2);
        set_ts(c, i, TS_ALPHAARG0, a0);
        set_ts(c, i, TS_TEXCOORDINDEX, i);
    }
    for(int i = MAX_UNITS; i < 8; i++) { set_ts(c, i, TS_COLOROP, TC_DISABLE); set_ts(c, i, TS_ALPHAOP, TC_DISABLE); }
    set_rs(c, RS_TEXTUREFACTOR, tfactor);
}

static void flush_transforms(Ctx *c)
{
    if(!c->xform_dirty) return;
    c->xform_dirty = 0;
    DWORD *p = cmd(CMD_SETTRANSFORM, 18 * 4);
    p[0] = c->cid; p[1] = 1;                            /* world = modelview */
    memcpy(p + 2, c->mv[c->mv_depth].m, 64);
    /* projection with clip z moved from [-w, w] to [0, w] */
    Mat pr = c->proj[c->proj_depth];
    for(int col = 0; col < 4; col++) pr.m[col * 4 + 2] = 0.5f * pr.m[col * 4 + 2] + 0.5f * pr.m[col * 4 + 3];
    p = cmd(CMD_SETTRANSFORM, 18 * 4);
    p[0] = c->cid; p[1] = 3;
    memcpy(p + 2, pr.m, 64);
}

static void flush_lights(Ctx *c)
{
    for(int i = 0; i < MAX_GL_LIGHTS; i++)
    {
        Light *l = &c->light[i];
        DWORD *p = cmd(CMD_SETLIGHTENABLED, 12);
        p[0] = c->cid; p[1] = i; p[2] = c->lighting && l->on;
        if(!(c->lighting && l->on)) continue;
        p = cmd(CMD_SETLIGHTDATA, 31 * 4);
        float *f = (float *)p;
        p[0] = c->cid; p[1] = i;
        int directional = l->pos[3] == 0, spot = !directional && l->spot_cut != 180;
        p[2] = directional ? 4 : spot ? 3 : 1;
        p[3] = 1;
        memcpy(f + 4, l->dif, 16); memcpy(f + 8, l->spec, 16); memcpy(f + 12, l->amb, 16);
        f[16] = l->pos[0]; f[17] = l->pos[1]; f[18] = l->pos[2]; f[19] = 1;
        if(directional) { f[20] = -l->pos[0]; f[21] = -l->pos[1]; f[22] = -l->pos[2]; }
        else { f[20] = l->dir[0]; f[21] = l->dir[1]; f[22] = l->dir[2]; }
        f[23] = 0;
        f[24] = 3.4e38f; f[25] = l->spot_exp; f[26] = l->att[0]; f[27] = l->att[1]; f[28] = l->att[2];
        f[29] = 0; f[30] = (float)(l->spot_cut * 3.14159265 / 90);
    }
    DWORD *p = cmd(CMD_SETMATERIAL, 19 * 4);
    p[0] = c->cid; p[1] = 4;
    memcpy(p + 2, c->mat_dif, 16); memcpy(p + 6, c->mat_amb, 16); memcpy(p + 10, c->mat_spec, 16);
    memcpy(p + 14, c->mat_emis, 16); memcpy(p + 18, &c->mat_shin, 4);
}

/* Everything the next draw depends on. */
static int lights_dirty = 1;
static void flush_state(Ctx *c)
{
    ensure_fb(c);
    set_rs(c, RS_ZENABLE, c->depth_test);
    set_rs(c, RS_ZFUNC, cmp_func(c->depth_func));
    set_rs(c, RS_ZWRITEENABLE, c->depth_mask);
    set_rs(c, RS_BLENDENABLE, c->blend);
    int sep = c->bsrc != c->bsrc_a || c->bdst != c->bdst_a;
    set_rs(c, RS_SRCBLEND, blend_factor(c->bsrc)); set_rs(c, RS_DSTBLEND, blend_factor(c->bdst));
    set_rs(c, RS_SEPARATEALPHABLENDENABLE, sep);
    set_rs(c, RS_SRCBLENDALPHA, blend_factor(c->bsrc_a)); set_rs(c, RS_DSTBLENDALPHA, blend_factor(c->bdst_a));
    set_rs(c, RS_BLENDEQUATION, blend_eq(c->blend_eq)); set_rs(c, RS_BLENDEQUATIONALPHA, blend_eq(c->blend_eq));
    set_rs(c, RS_BLENDCOLOR, argb(c->blend_color));
    set_rs(c, RS_ALPHATESTENABLE, c->alpha_test);
    set_rs(c, RS_ALPHAFUNC, cmp_func(c->alpha_func));
    set_rs(c, RS_ALPHAREF, (DWORD)(c->alpha_ref * 255 + 0.5f));
    /* The y flip into the host's orientation turns OpenGL's counter-clockwise into the host's CCW winding. */
    set_rs(c, RS_FRONTWINDING, c->front_face == GL_CW ? 1 : 2);
    set_rs(c, RS_CULLMODE, !c->cull ? 1 : c->cull_face == GL_FRONT ? 2 : c->cull_face == GL_BACK ? 3 : 4);
    set_rs(c, RS_STENCILENABLE, c->stencil);
    set_rs(c, RS_STENCILFUNC, cmp_func(c->st_func)); set_rs(c, RS_STENCILREF, c->st_ref);
    set_rs(c, RS_STENCILMASK, c->st_mask); set_rs(c, RS_STENCILWRITEMASK, c->st_wmask);
    set_rs(c, RS_STENCILFAIL, stencil_op(c->st_fail)); set_rs(c, RS_STENCILZFAIL, stencil_op(c->st_zfail));
    set_rs(c, RS_STENCILPASS, stencil_op(c->st_pass));
    set_rs(c, RS_COLORWRITEENABLE, (c->cmask[0] ? 1 : 0) | (c->cmask[1] ? 2 : 0) | (c->cmask[2] ? 4 : 0) | (c->cmask[3] ? 8 : 0));
    set_rs(c, RS_SHADEMODE, c->shade == GL_FLAT ? 1 : 2);
    set_rs(c, RS_FOGENABLE, c->fog);
    set_rs(c, RS_FOGMODE, (c->fog_mode == GL_EXP ? 1 : c->fog_mode == GL_EXP2 ? 2 : 3) | 1 << 16 | 1 << 24);
    set_rs(c, RS_FOGSTART, fbits(c->fog_start)); set_rs(c, RS_FOGEND, fbits(c->fog_end));
    set_rs(c, RS_FOGDENSITY, fbits(c->fog_density)); set_rs(c, RS_FOGCOLOR, argb(c->fog_color));
    set_rs(c, RS_DEPTHBIAS, fbits(c->poly_offset ? c->po_units / 16777216.0f : 0));
    set_rs(c, RS_SLOPESCALEDEPTHBIAS, fbits(c->poly_offset ? c->po_factor : 0));
    set_rs(c, RS_POINTSIZE, fbits(c->point_size));
    set_rs(c, RS_LIGHTINGENABLE, c->lighting);
    set_rs(c, RS_LOCALVIEWER, 1);
    set_rs(c, RS_NORMALIZENORMALS, 1);
    set_rs(c, RS_SPECULARENABLE, c->lighting);
    set_rs(c, RS_AMBIENT, argb(c->model_ambient));
    set_rs(c, RS_VERTEXMATERIALENABLE, c->color_material);
    {
        GLenum m = c->cm_mode;
        set_rs(c, RS_DIFFUSEMATERIALSOURCE, m == GL_DIFFUSE || m == GL_AMBIENT_AND_DIFFUSE ? 1 : 0);
        set_rs(c, RS_AMBIENTMATERIALSOURCE, m == GL_AMBIENT || m == GL_AMBIENT_AND_DIFFUSE ? 1 : 0);
        set_rs(c, RS_SPECULARMATERIALSOURCE, m == GL_SPECULAR ? 1 : 0);
        set_rs(c, RS_EMISSIVEMATERIALSOURCE, m == GL_EMISSION ? 1 : 0);
    }
    set_rs(c, RS_SCISSORTESTENABLE, c->scissor_test);
    if(!c->sent_valid || 1)
    {
        /* viewport, depth range and scissor flip y */
        static GLint last_vp[4] = { -1 }, last_sc[4] = { -1 };
        static UINT last_h;
        static double last_dr[2] = { -1, -1 };
        if(memcmp(last_vp, c->viewport, sizeof last_vp) || last_h != c->fbh || memcmp(last_dr, c->depth_range, sizeof last_dr) || !c->sent_valid)
        {
            memcpy(last_vp, c->viewport, sizeof last_vp); memcpy(last_dr, c->depth_range, sizeof last_dr);
            DWORD *p = cmd(CMD_SETVIEWPORT, 20);
            p[0] = c->cid; p[1] = c->viewport[0]; p[2] = (int)c->fbh - c->viewport[1] - c->viewport[3];
            p[3] = c->viewport[2]; p[4] = c->viewport[3];
            p = cmd(CMD_SETZRANGE, 12);
            p[0] = c->cid; p[1] = fbits((float)c->depth_range[0]); p[2] = fbits((float)c->depth_range[1]);
        }
        if(memcmp(last_sc, c->scissor, sizeof last_sc) || last_h != c->fbh || !c->sent_valid)
        {
            memcpy(last_sc, c->scissor, sizeof last_sc);
            DWORD *p = cmd(CMD_SETSCISSORRECT, 20);
            p[0] = c->cid; p[1] = c->scissor[0]; p[2] = (int)c->fbh - c->scissor[1] - c->scissor[3];
            p[3] = c->scissor[2]; p[4] = c->scissor[3];
        }
        last_h = c->fbh;
    }
    flush_stages(c);
    if(!c->sent_valid) c->xform_dirty = 1;
    flush_transforms(c);
    if(lights_dirty && c->lighting) { flush_lights(c); lights_dirty = 0; }
    if(!c->sent_valid)
    {
        DWORD *p = cmd(CMD_SETTRANSFORM, 18 * 4);
        Mat id; mat_identity(&id);
        p[0] = c->cid; p[1] = 2; memcpy(p + 2, id.m, 64);
    }
    c->sent_valid = 1;
}

/* ---------- drawing ---------- */

enum { DT_FLOAT1 = 0, DT_FLOAT2 = 1, DT_FLOAT3 = 2, DT_FLOAT4 = 3, DT_D3DCOLOR = 4, DT_UBYTE4 = 5, DT_SHORT2 = 6,
    DT_SHORT4 = 7, DT_UBYTE4N = 8 };
enum { DU_POSITION = 0, DU_NORMAL = 3, DU_TEXCOORD = 5, DU_COLOR = 10 };

typedef struct { DWORD type, usage, index, sid, offset, stride; } VDecl;

static DWORD prim(GLenum mode, UINT n, UINT *count)
{
    switch(mode)
    {
        case GL_POINTS: *count = n; return 2;
        case GL_LINES: *count = n / 2; return 3;
        case GL_LINE_STRIP: *count = n > 1 ? n - 1 : 0; return 4;
        case GL_LINE_LOOP: *count = n > 1 ? n - 1 : 0; return 4;    /* closing segment drawn separately */
        case GL_TRIANGLES: *count = n / 3; return 1;
        case GL_TRIANGLE_STRIP: case GL_QUAD_STRIP: *count = n > 2 ? n - 2 : 0; return 5;
        case GL_TRIANGLE_FAN: case GL_POLYGON: *count = n > 2 ? n - 2 : 0; return 6;
        default: *count = 0; return 1;
    }
}

static void emit_bias(Ctx *c, const VDecl *d, UINT nd, DWORD ptype, UINT count, DWORD isid, UINT ioff, UINT iw, INT bias);
static int decl_type(GLenum type, GLint size, int color, DWORD *dt);
static UINT type_size(GLenum t);
static void emit(Ctx *c, const VDecl *d, UINT nd, DWORD ptype, UINT count, DWORD isid, UINT ioff, UINT iw)
{
    emit_bias(c, d, nd, ptype, count, isid, ioff, iw, 0);
}
static void emit_bias(Ctx *c, const VDecl *d, UINT nd, DWORD ptype, UINT count, DWORD isid, UINT ioff, UINT iw, INT bias)
{
    if(!count) return;
    DWORD *p = cmd(CMD_DRAW_PRIMITIVES, (3 + nd * 9 + 7) * 4);
    p[0] = c->cid; p[1] = nd; p[2] = 1;
    DWORD *q = p + 3;
    for(UINT i = 0; i < nd; i++, q += 9)
    {
        q[0] = d[i].type; q[1] = 0; q[2] = d[i].usage; q[3] = d[i].index;
        q[4] = d[i].sid; q[5] = d[i].offset; q[6] = d[i].stride; q[7] = 0; q[8] = 0;
    }
    q[0] = ptype; q[1] = count; q[2] = isid; q[3] = ioff; q[4] = iw; q[5] = iw; q[6] = (DWORD)bias;
}

/* Space in a scratch ring (16-aligned); data goes there with a separate DMA of the exact length. */
static UINT ring_alloc(UINT *pos, UINT cap, UINT n)
{
    n = (n + 15) & ~15u;
    if(*pos + n > cap) *pos = 0;
    UINT off = *pos;
    *pos += n;
    return off;
}

/* Can every enabled array be read as it is (no conversion)? */
static int direct_off = -1;
static int direct_ok(Ctx *c, GLenum mode)
{
    DWORD dt;
    if(direct_off < 0) direct_off = getenv("GLFWD_NODIRECT") != NULL;
    if(direct_off) return 0;
    if(mode == GL_QUADS || mode == GL_LINE_LOOP) return 0;
    if(!decl_type(c->vertex.type, c->vertex.size, 0, &dt)) return 0;
    if(c->colors.on && !decl_type(c->colors.type, c->colors.size, c->colors.bgra ? 2 : 1, &dt)) return 0;
    if(c->lighting && c->normals.on && !decl_type(c->normals.type, 3, 0, &dt)) return 0;
    for(int u = 0; u < MAX_UNITS; u++)
        if(c->unit[u].tex2d && c->texc[u].on && !decl_type(c->texc[u].type, c->texc[u].size, 0, &dt)) return 0;
    return 1;
}

/* Draw straight from the application's arrays: the host reads them (and the indices) through the page tables
 * during the flush at the end, so nothing is copied here. Vertices [first, first + n) are sent; indices stay as
 * the application wrote them and the host rebases them by -first. */
static void draw_direct(Ctx *c, GLenum mode, UINT first, UINT n, const void *indices, UINT nidx, UINT iw, Buf *ibuf)
{
    flush_state(c);
    struct { const BYTE *start, *end; UINT stride, ring; } span[3 + MAX_UNITS];
    UINT nspan = 0, nd = 0;
    VDecl d[3 + MAX_UNITS];
    int span_of[3 + MAX_UNITS];
    const Array *arr[3 + MAX_UNITS];
    DWORD usage[3 + MAX_UNITS], index[3 + MAX_UNITS];
    int color[3 + MAX_UNITS];
    UINT na = 0;
#define ADD(a_, u_, i_, c_) do { arr[na] = (a_); usage[na] = (u_); index[na] = (i_); color[na] = (c_); na++; } while(0)
    ADD(&c->vertex, DU_POSITION, 0, 0);
    if(c->colors.on) ADD(&c->colors, DU_COLOR, 0, c->colors.bgra ? 2 : 1);
    if(c->lighting && c->normals.on) ADD(&c->normals, DU_NORMAL, 0, 0);
    for(int u = 0; u < MAX_UNITS; u++) if(c->unit[u].tex2d && c->texc[u].on) ADD(&c->texc[u], DU_TEXCOORD, u, 0);
#undef ADD
    for(UINT i = 0; i < na; i++)
    {
        const Array *a = arr[i];
        DWORD dt;
        UINT size = a == &c->normals ? 3 : a->size;
        decl_type(a->type, size, color[i], &dt);
        UINT esize = size * type_size(a->type), stride = a->stride ? (UINT)a->stride : esize;
        d[nd].type = dt; d[nd].usage = usage[i]; d[nd].index = index[i]; d[nd].stride = stride;
        span_of[nd] = -1;
        if(a->buf && a->buf->sid) { d[nd].sid = a->buf->sid; d[nd].offset = (UINT)(size_t)a->ptr + first * stride; nd++; continue; }
        const BYTE *st = a->ptr + first * stride, *en = st + (n - 1) * stride + esize;
        UINT k;
        for(k = 0; k < nspan; k++)
            if(span[k].stride == stride && st < span[k].end + stride && en + stride > span[k].start) break;
        if(k == nspan) { span[k].start = st; span[k].end = en; span[k].stride = stride; nspan++; }
        else { if(st < span[k].start) span[k].start = st; if(en > span[k].end) span[k].end = en; }
        d[nd].sid = scratch_vb; d[nd].offset = (UINT)(size_t)st;     /* fixed up below */
        span_of[nd] = k;
        nd++;
    }
    for(UINT k = 0; k < nspan; k++)
    {
        UINT len = span[k].end - span[k].start;
        span[k].ring = ring_alloc(&scratch_vb_pos, SCRATCH_VB_BYTES, len);
        dma2(scratch_vb, 0, span[k].start, 0, XFER_TO_HOST, span[k].ring, 0, len, 1, 0, 0);
    }
    for(UINT i = 0; i < nd; i++)
        if(span_of[i] >= 0) d[i].offset = span[span_of[i]].ring + (UINT)((const BYTE *)(size_t)d[i].offset - span[span_of[i]].start);
    /* current values for what has no array */
    BYTE *cst = arena_alloc(16 * (2 + MAX_UNITS));
    UINT used = 0;
#define CONST_ATTR(usage_, index_, type_, bytes_, src_) do { memcpy(cst + used, src_, bytes_);     d[nd].type = type_; d[nd].usage = usage_; d[nd].index = index_; d[nd].sid = scratch_vb; d[nd].offset = used;     d[nd].stride = 0; span_of[nd] = -2; nd++; used += 16; } while(0)
    if(!c->colors.on)
    {
        BYTE rgba[4];
        for(int k = 0; k < 4; k++) { float v = c->color[k]; rgba[k] = (BYTE)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f); }
        CONST_ATTR(DU_COLOR, 0, DT_UBYTE4N, 4, rgba);
    }
    if(c->lighting && !c->normals.on) CONST_ATTR(DU_NORMAL, 0, DT_FLOAT3, 12, c->normal);
    for(int u = 0; u < MAX_UNITS; u++) if(c->unit[u].tex2d && !c->texc[u].on) CONST_ATTR(DU_TEXCOORD, u, DT_FLOAT4, 16, c->unit[u].tc);
#undef CONST_ATTR
    if(used)
    {
        UINT base = to_scratch(scratch_vb, &scratch_vb_pos, SCRATCH_VB_BYTES, cst, used);
        for(UINT i = 0; i < nd; i++) if(span_of[i] == -2) d[i].offset += base;
    }
    UINT count;
    DWORD pt = prim(mode, indices || ibuf ? nidx : n, &count);
    if(ibuf) emit_bias(c, d, nd, pt, count, ibuf->sid, (UINT)(size_t)indices, iw, -(INT)first);
    else if(indices)
    {
        UINT off = ring_alloc(&scratch_ib_pos, SCRATCH_IB_BYTES, nidx * iw);
        dma2(scratch_ib, 0, indices, 0, XFER_TO_HOST, off, 0, nidx * iw, 1, 0, 0);
        emit_bias(c, d, nd, pt, count, scratch_ib, off, iw, -(INT)first);
    }
    else emit(c, d, nd, pt, count, INVALID_ID, 0, 0);
    gl_flush();                                  /* the application may change its arrays after we return */
}

/* Quads → triangles: 16-bit indices for vertices [0, n). */
static UINT quad_indices(UINT n, WORD *out)
{
    UINT k = 0;
    for(UINT q = 0; q + 3 < n; q += 4)
    {
        out[k++] = q; out[k++] = q + 1; out[k++] = q + 2;
        out[k++] = q; out[k++] = q + 2; out[k++] = q + 3;
    }
    return k;
}

static int decl_type(GLenum type, GLint size, int color, DWORD *dt)
{
    if(color == 2 && type == GL_UNSIGNED_BYTE && size == 4) { *dt = DT_D3DCOLOR; return 1; }   /* GL_BGRA */
    if(type == GL_FLOAT && size >= 1 && size <= 4) { *dt = DT_FLOAT1 + size - 1; return 1; }
    if(type == GL_UNSIGNED_BYTE && size == 4 && color) { *dt = DT_UBYTE4N; return 1; }
    if(type == GL_SHORT && size == 2) { *dt = DT_SHORT2; return 1; }
    if(type == GL_SHORT && size == 4) { *dt = DT_SHORT4; return 1; }
    return 0;
}
static UINT type_size(GLenum t)
{
    switch(t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_DOUBLE: return 8; default: return 4; }
}
static float read_comp(const BYTE *p, GLenum t, int norm)
{
    switch(t)
    {
        case GL_BYTE: return norm ? *(const signed char *)p / 127.0f : *(const signed char *)p;
        case GL_UNSIGNED_BYTE: return norm ? *p / 255.0f : *p;
        case GL_SHORT: return norm ? *(const short *)p / 32767.0f : *(const short *)p;
        case GL_UNSIGNED_SHORT: return norm ? *(const unsigned short *)p / 65535.0f : *(const unsigned short *)p;
        case GL_INT: return norm ? *(const int *)p / 2147483647.0f : (float)*(const int *)p;
        case GL_UNSIGNED_INT: return norm ? *(const unsigned *)p / 4294967295.0f : (float)*(const unsigned *)p;
        case GL_DOUBLE: return (float)*(const double *)p;
        default: return *(const float *)p;
    }
}

/* One client array for vertices [first, first + n): an arena copy (or the bound buffer object). */
static int array_decl(Ctx *c, const Array *a, DWORD usage, DWORD index, int color, UINT first, UINT n, VDecl *d,
    BYTE *chunk, UINT *chunk_used)
{
    DWORD dt;
    UINT esize = a->size * type_size(a->type);
    UINT stride = a->stride ? (UINT)a->stride : esize;
    d->usage = usage; d->index = index;
    if(a->buf && a->buf->sid && decl_type(a->type, a->size, color, &dt))
    {
        d->type = dt; d->sid = a->buf->sid; d->offset = (UINT)(size_t)a->ptr + first * stride; d->stride = stride;
        return 0;
    }
    const BYTE *src = a->buf ? a->buf->mem + (size_t)a->ptr : a->ptr;
    if(!src) return -1;
    BYTE *dst = chunk + *chunk_used;
    if(decl_type(a->type, a->size, color, &dt))
    {
        /* copy the rows as they are */
        if(stride == esize) memcpy(dst, src + first * stride, n * stride);
        else for(UINT i = 0; i < n; i++) memcpy(dst + i * stride, src + (first + i) * stride, esize);
        d->type = dt; d->stride = stride;
        *chunk_used += (n * stride + 15) & ~15u;
    }
    else
    {
        /* convert to floats (or RGBA bytes for colours) */
        if(color)
        {
            for(UINT i = 0; i < n; i++)
            {
                const BYTE *s = src + (first + i) * stride;
                for(int k = 0; k < 4; k++)
                {
                    float v = k < a->size ? read_comp(s + k * type_size(a->type), a->type, 1) : 1.0f;
                    dst[i * 4 + k] = (BYTE)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f);
                }
            }
            d->type = DT_UBYTE4N; d->stride = 4;
            *chunk_used += (n * 4 + 15) & ~15u;
        }
        else
        {
            float *f = (float *)dst;
            for(UINT i = 0; i < n; i++)
            {
                const BYTE *s = src + (first + i) * stride;
                for(int k = 0; k < 4; k++)
                    f[i * 4 + k] = k < a->size ? read_comp(s + k * type_size(a->type), a->type, usage == DU_NORMAL) : k == 3 ? 1.0f : 0.0f;
            }
            d->type = usage == DU_NORMAL ? DT_FLOAT3 : DT_FLOAT4; d->stride = 16;
            if(usage == DU_NORMAL) { for(UINT i = 0; i < n; i++) memmove(f + i * 3, f + i * 4, 12); d->stride = 12; }
            *chunk_used += (n * d->stride + 15) & ~15u;
        }
    }
    d->sid = 0; d->offset = (UINT)(dst - chunk);              /* fixed up after the chunk is sent */
    return 1;
}

static UINT array_bytes(const Array *a, UINT n)
{
    UINT esize = a->size * type_size(a->type), stride = a->stride ? (UINT)a->stride : esize;
    UINT m = n * (stride > 16 ? stride : 16);
    return (m + 15) & ~15u;
}

/* Draw vertices [first, first + n) of the client arrays, with optional 16-bit indices (relative to first). */
static void draw_arrays(Ctx *c, GLenum mode, UINT first, UINT n, const WORD *idx, UINT nidx, Buf *ibuf, UINT ioff, UINT iw)
{
    if(!c->vertex.on || !n) return;
    if(!idx) arena_reserve(n * 16 * (4 + MAX_UNITS) + n * 4 + 4096);
    flush_state(c);
    UINT need = array_bytes(&c->vertex, n) + array_bytes(&c->colors, n) + array_bytes(&c->normals, n) + 64 * 6;
    for(int u = 0; u < MAX_UNITS; u++) need += array_bytes(&c->texc[u], n);
    BYTE *chunk = arena_alloc(need);
    if(!chunk) { hlog("draw: %u vertices do not fit the arena", n); return; }
    UINT used = 0, nd = 0;
    VDecl d[3 + MAX_UNITS];
    int fix[3 + MAX_UNITS];
    if((fix[nd] = array_decl(c, &c->vertex, DU_POSITION, 0, 0, first, n, &d[nd], chunk, &used)) >= 0) nd++;
    /* colour, normal and texture coordinates: arrays, or the current values with stride 0 */
#define CONST_ATTR(usage_, index_, type_, bytes_, src_) do { memcpy(chunk + used, src_, bytes_); \
    d[nd].type = type_; d[nd].usage = usage_; d[nd].index = index_; d[nd].sid = 0; d[nd].offset = used; d[nd].stride = 0; \
    fix[nd++] = 1; used += 16; } while(0)
    if(c->colors.on && (fix[nd] = array_decl(c, &c->colors, DU_COLOR, 0, c->colors.bgra ? 2 : 1, first, n, &d[nd], chunk, &used)) >= 0) nd++;
    else if(!c->colors.on)
    {
        BYTE rgba[4];
        for(int k = 0; k < 4; k++) { float v = c->color[k]; rgba[k] = (BYTE)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f); }
        CONST_ATTR(DU_COLOR, 0, DT_UBYTE4N, 4, rgba);
    }
    if(c->lighting)
    {
        if(c->normals.on && (fix[nd] = array_decl(c, &c->normals, DU_NORMAL, 0, 0, first, n, &d[nd], chunk, &used)) >= 0) nd++;
        else if(!c->normals.on) CONST_ATTR(DU_NORMAL, 0, DT_FLOAT3, 12, c->normal);
    }
    for(int u = 0; u < MAX_UNITS; u++)
    {
        if(!c->unit[u].tex2d) continue;
        if(c->texc[u].on && (fix[nd] = array_decl(c, &c->texc[u], DU_TEXCOORD, u, 0, first, n, &d[nd], chunk, &used)) >= 0) nd++;
        else if(!c->texc[u].on) CONST_ATTR(DU_TEXCOORD, u, DT_FLOAT4, 16, c->unit[u].tc);
    }
#undef CONST_ATTR
    UINT base = used ? to_scratch(scratch_vb, &scratch_vb_pos, SCRATCH_VB_BYTES, chunk, used) : 0;
    for(UINT i = 0; i < nd; i++) if(fix[i] == 1) { d[i].sid = scratch_vb; d[i].offset += base; }

    UINT count;
    DWORD pt = prim(mode, idx || ibuf ? nidx : n, &count);
    if(mode == GL_QUADS)
    {
        /* quads become indexed triangles */
        UINT nv = idx ? nidx : n;
        WORD *qi = (WORD *)arena_alloc((nv / 4) * 6 * 2 + 16), *q2 = NULL;
        if(!qi) return;
        if(idx)
        {
            q2 = (WORD *)arena_alloc((nv / 4) * 6 * 2 + 16);
            UINT k = quad_indices(nv, qi);
            for(UINT i = 0; i < k; i++) q2[i] = idx[qi[i]];
            qi = q2;
        }
        UINT k = idx ? (nv / 4) * 6 : quad_indices(nv, qi);
        UINT off = to_scratch(scratch_ib, &scratch_ib_pos, SCRATCH_IB_BYTES, qi, k * 2);
        emit(c, d, nd, 1, k / 3, scratch_ib, off, 2);
        return;
    }
    if(ibuf) { emit(c, d, nd, pt, count, ibuf->sid, ioff, iw); return; }
    if(idx)
    {
        UINT off = to_scratch(scratch_ib, &scratch_ib_pos, SCRATCH_IB_BYTES, idx, nidx * 2);
        emit(c, d, nd, pt, count, scratch_ib, off, 2);
        if(mode == GL_LINE_LOOP && nidx > 2)
        {
            WORD *li = (WORD *)arena_alloc(16);
            li[0] = idx[nidx - 1]; li[1] = idx[0];
            off = to_scratch(scratch_ib, &scratch_ib_pos, SCRATCH_IB_BYTES, li, 4);
            emit(c, d, nd, 3, 1, scratch_ib, off, 2);
        }
        return;
    }
    emit(c, d, nd, pt, count, INVALID_ID, 0, 0);
    if(mode == GL_LINE_LOOP && n > 2)
    {
        WORD *li = (WORD *)arena_alloc(16);
        li[0] = n - 1; li[1] = 0;
        UINT off = to_scratch(scratch_ib, &scratch_ib_pos, SCRATCH_IB_BYTES, li, 4);
        emit(c, d, nd, 3, 1, scratch_ib, off, 2);
    }
}

/* ---------- immediate mode: glBegin/glEnd become client arrays over a vertex list ---------- */

typedef struct { float pos[4], normal[3]; BYTE color[4]; float tc[MAX_UNITS][4]; } IVert;
static IVert *imm;
static UINT imm_n, imm_cap;

static void imm_vertex(float x, float y, float z, float w)
{
    Ctx *c = cur;
    if(!c->in_begin) return;
    if(imm_n == imm_cap) { imm_cap = imm_cap ? imm_cap * 2 : 4096; imm = realloc(imm, imm_cap * sizeof *imm); }
    IVert *v = &imm[imm_n++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
    memcpy(v->normal, c->normal, 12);
    for(int k = 0; k < 4; k++) { float f = c->color[k]; v->color[k] = (BYTE)(f < 0 ? 0 : f > 1 ? 255 : f * 255 + 0.5f); }
    for(int u = 0; u < MAX_UNITS; u++) memcpy(v->tc[u], c->unit[u].tc, 16);
}

static void imm_end(Ctx *c)
{
    if(!imm_n) return;
    Array save_v = c->vertex, save_c = c->colors, save_n = c->normals, save_t[MAX_UNITS];
    memcpy(save_t, c->texc, sizeof save_t);
    Array a = { .on = 1, .size = 4, .type = GL_FLOAT, .stride = sizeof(IVert), .ptr = (const BYTE *)imm[0].pos };
    c->vertex = a;
    Array col = { .on = 1, .size = 4, .type = GL_UNSIGNED_BYTE, .stride = sizeof(IVert), .ptr = imm[0].color };
    c->colors = col;
    Array nor = { .on = 1, .size = 3, .type = GL_FLOAT, .stride = sizeof(IVert), .ptr = (const BYTE *)imm[0].normal };
    c->normals = nor;
    for(int u = 0; u < MAX_UNITS; u++) { Array t = { .on = 1, .size = 4, .type = GL_FLOAT, .stride = sizeof(IVert), .ptr = (const BYTE *)imm[0].tc[u] }; c->texc[u] = t; }
    draw_arrays(c, c->begin_mode, 0, imm_n, NULL, 0, NULL, 0, 0);
    c->vertex = save_v; c->colors = save_c; c->normals = save_n;
    memcpy(c->texc, save_t, sizeof save_t);
    imm_n = 0;
}

/* ---------- WGL ---------- */

typedef struct { int dummy; } GLRC;

static void ctx_defaults(Ctx *c)
{
    for(int i = 0; i < 32; i++) mat_identity(&c->mv[i]);
    for(int i = 0; i < 4; i++) mat_identity(&c->proj[i]);
    c->matrix_mode = GL_MODELVIEW; c->xform_dirty = 1;
    c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1; c->normal[2] = 1;
    c->depth_func = GL_LESS; c->depth_mask = GL_TRUE; c->alpha_func = GL_ALWAYS; c->cull_face = GL_BACK;
    c->front_face = GL_CCW; c->shade = GL_SMOOTH; c->blend_eq = GL_FUNC_ADD;
    c->cmask[0] = c->cmask[1] = c->cmask[2] = c->cmask[3] = GL_TRUE;
    c->bsrc = c->bsrc_a = GL_ONE; c->bdst = c->bdst_a = GL_ZERO;
    c->st_func = GL_ALWAYS; c->st_mask = c->st_wmask = 0xFFFFFFFF; c->st_fail = c->st_zfail = c->st_pass = GL_KEEP;
    c->depth_range[1] = 1;
    c->fog_mode = GL_EXP; c->fog_end = 1; c->fog_density = 1;
    c->point_size = 1;
    c->clear_depth = 1;
    c->model_ambient[0] = c->model_ambient[1] = c->model_ambient[2] = 0.2f; c->model_ambient[3] = 1;
    for(int i = 0; i < MAX_GL_LIGHTS; i++)
    {
        Light *l = &c->light[i];
        memset(l, 0, sizeof *l);
        l->amb[3] = 1; l->pos[2] = 1; l->dir[2] = -1; l->spot_cut = 180; l->att[0] = 1;
        l->dif[3] = l->spec[3] = 1;
        if(!i) l->dif[0] = l->dif[1] = l->dif[2] = l->spec[0] = l->spec[1] = l->spec[2] = 1;
    }
    c->mat_amb[0] = c->mat_amb[1] = c->mat_amb[2] = 0.2f; c->mat_amb[3] = 1;
    c->mat_dif[0] = c->mat_dif[1] = c->mat_dif[2] = 0.8f; c->mat_dif[3] = 1;
    c->mat_spec[3] = 1; c->mat_emis[3] = 1;
    c->cm_face = GL_FRONT_AND_BACK; c->cm_mode = GL_AMBIENT_AND_DIFFUSE;
    for(int u = 0; u < MAX_UNITS; u++)
    {
        Unit *x = &c->unit[u];
        x->mode = GL_MODULATE; x->combine_rgb = x->combine_alpha = GL_MODULATE;
        x->src_rgb[0] = x->src_alpha[0] = GL_TEXTURE; x->src_rgb[1] = x->src_alpha[1] = GL_PREVIOUS;
        x->src_rgb[2] = x->src_alpha[2] = GL_CONSTANT;
        x->op_rgb[0] = x->op_rgb[1] = GL_SRC_COLOR; x->op_rgb[2] = GL_SRC_ALPHA;
        x->op_alpha[0] = x->op_alpha[1] = x->op_alpha[2] = GL_SRC_ALPHA;
        x->rgb_scale = x->alpha_scale = 1;
        for(int i = 0; i < 4; i++) mat_identity(&x->stack[i]);
        x->tc[3] = 1;
    }
    c->unpack_align = c->pack_align = 4;
}

static Ctx *the_ctx;

__declspec(dllexport) HGLRC WINAPI wglCreateContext(HDC dc)
{
    hc_init();
    if(!arena)
    {
        arena = VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        EnterCriticalSection(&lock);
        scratch_vb = new_id(); scratch_ib = new_id();
        DWORD *p = cmd(CMD_SURFACE_DEFINE, 12 * 4);
        p[0] = scratch_vb; p[1] = SURFACE_STREAM; p[2] = SF_BUFFER; p[3] = 1; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
        p[9] = SCRATCH_VB_BYTES; p[10] = 1; p[11] = 1;
        p = cmd(CMD_SURFACE_DEFINE, 12 * 4);
        p[0] = scratch_ib; p[1] = SURFACE_STREAM; p[2] = SF_BUFFER; p[3] = 1; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
        p[9] = SCRATCH_IB_BYTES; p[10] = 1; p[11] = 1;
        LeaveCriticalSection(&lock);
    }
    Ctx *c = calloc(1, sizeof *c);
    c->dc = dc; c->wnd = WindowFromDC(dc);
    c->cid = new_id();
    ctx_defaults(c);
    RECT r = { 0, 0, 1, 1 };
    if(c->wnd) GetClientRect(c->wnd, &r);
    c->viewport[2] = c->scissor[2] = r.right; c->viewport[3] = c->scissor[3] = r.bottom;
    EnterCriticalSection(&lock);
    *cmd(CMD_CONTEXT_DEFINE, 4) = c->cid;
    hlog("glfwd: wglCreateContext dc %p window %p %ldx%ld", dc, c->wnd, r.right, r.bottom);
    LeaveCriticalSection(&lock);
    if(!the_ctx) the_ctx = c;
    return (HGLRC)c;
}
__declspec(dllexport) BOOL WINAPI wglDeleteContext(HGLRC rc)
{
    Ctx *c = (Ctx *)rc;
    if(!c) return FALSE;
    EnterCriticalSection(&lock);
    if(cur == c) { gl_flush(); cur = NULL; }
    *cmd(CMD_CONTEXT_DESTROY, 4) = c->cid;
    LeaveCriticalSection(&lock);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI wglMakeCurrent(HDC dc, HGLRC rc)
{
    if(cur) { EnterCriticalSection(&lock); gl_flush(); LeaveCriticalSection(&lock); }
    cur = (Ctx *)rc;
    if(cur && dc) { cur->dc = dc; cur->wnd = WindowFromDC(dc); }
    return TRUE;
}
__declspec(dllexport) HGLRC WINAPI wglGetCurrentContext(void) { return (HGLRC)cur; }
__declspec(dllexport) HDC WINAPI wglGetCurrentDC(void) { return cur ? cur->dc : NULL; }
__declspec(dllexport) BOOL WINAPI wglShareLists(HGLRC a, HGLRC b) { return TRUE; }
__declspec(dllexport) BOOL WINAPI wglCopyContext(HGLRC a, HGLRC b, UINT m) { return FALSE; }
__declspec(dllexport) HGLRC WINAPI wglCreateLayerContext(HDC dc, int l) { return l ? NULL : wglCreateContext(dc); }

static const PIXELFORMATDESCRIPTOR the_pfd = {
    .nSize = sizeof(PIXELFORMATDESCRIPTOR), .nVersion = 1,
    .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_COPY,
    .iPixelType = PFD_TYPE_RGBA, .cColorBits = 32, .cRedBits = 8, .cRedShift = 16, .cGreenBits = 8, .cGreenShift = 8,
    .cBlueBits = 8, .cBlueShift = 0, .cAlphaBits = 8, .cAlphaShift = 24, .cDepthBits = 24, .cStencilBits = 8,
    .iLayerType = PFD_MAIN_PLANE,
};
__declspec(dllexport) int WINAPI wglChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *p) { return 1; }
__declspec(dllexport) int WINAPI wglDescribePixelFormat(HDC dc, int f, UINT n, PIXELFORMATDESCRIPTOR *p)
{
    if(p && n >= sizeof *p) *p = the_pfd;
    return 1;
}
__declspec(dllexport) int WINAPI wglGetPixelFormat(HDC dc) { return 1; }
__declspec(dllexport) BOOL WINAPI wglSetPixelFormat(HDC dc, int f, const PIXELFORMATDESCRIPTOR *p) { return TRUE; }
__declspec(dllexport) BOOL WINAPI wglDescribeLayerPlane(HDC dc, int f, int l, UINT n, LPLAYERPLANEDESCRIPTOR p) { return FALSE; }
__declspec(dllexport) int WINAPI wglGetLayerPaletteEntries(HDC dc, int l, int s, int n, COLORREF *c) { return 0; }
__declspec(dllexport) int WINAPI wglSetLayerPaletteEntries(HDC dc, int l, int s, int n, const COLORREF *c) { return 0; }
__declspec(dllexport) BOOL WINAPI wglRealizeLayerPalette(HDC dc, int l, BOOL r) { return FALSE; }
__declspec(dllexport) BOOL WINAPI wglUseFontBitmapsA(HDC dc, DWORD a, DWORD b, DWORD c) { return FALSE; }
__declspec(dllexport) BOOL WINAPI wglUseFontBitmapsW(HDC dc, DWORD a, DWORD b, DWORD c) { return FALSE; }
__declspec(dllexport) BOOL WINAPI wglUseFontOutlinesA(HDC dc, DWORD a, DWORD b, DWORD c, FLOAT d, FLOAT e, int f, LPGLYPHMETRICSFLOAT g) { return FALSE; }
__declspec(dllexport) BOOL WINAPI wglUseFontOutlinesW(HDC dc, DWORD a, DWORD b, DWORD c, FLOAT d, FLOAT e, int f, LPGLYPHMETRICSFLOAT g) { return FALSE; }

static UINT swaps;
__declspec(dllexport) BOOL WINAPI wglSwapBuffers(HDC dc)
{
    Ctx *c = cur;
    if(!c) return FALSE;
    EnterCriticalSection(&lock);
    if(c->fb && c->wnd && !IsIconic(c->wnd))
    {
        POINT o = { 0, 0 };
        RECT cr;
        GetClientRect(c->wnd, &cr);
        ClientToScreen(c->wnd, &o);
        LONG x = o.x, y = o.y, sx = 0, sy = 0;
        LONG w = cr.right < (LONG)c->fbw ? cr.right : (LONG)c->fbw, h = cr.bottom < (LONG)c->fbh ? cr.bottom : (LONG)c->fbh;
        if(x < 0) { sx = -x; w += x; x = 0; }
        if(y < 0) { sy = -y; h += y; y = 0; }
        if(w > 0 && h > 0)
        {
            DWORD *p = cmd(CMD_PRESENT, 28);
            p[0] = c->fb; p[1] = x; p[2] = y; p[3] = w; p[4] = h; p[5] = sx; p[6] = sy;
        }
    }
    gl_flush();
    LeaveCriticalSection(&lock);
    swaps++;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) { return wglSwapBuffers(dc); }

/* ---------- GL entry points ---------- */

#define C Ctx *c = cur; if(!c) return
#define LOCK EnterCriticalSection(&lock)
#define UNLOCK LeaveCriticalSection(&lock)

static void stub(const char *name)
{
    static char seen[64][40];
    static int n;
    for(int i = 0; i < n; i++) if(!strcmp(seen[i], name)) return;
    if(n < 64) { strncpy(seen[n], name, 39); n++; }
    hlog("glfwd: %s not implemented", name);
}

void APIENTRY glEnable(GLenum cap);
void APIENTRY glDisable(GLenum cap);
static void set_cap(GLenum cap, int on)
{
    C;
    switch(cap)
    {
        case GL_DEPTH_TEST: c->depth_test = on; break;
        case GL_BLEND: c->blend = on; break;
        case GL_ALPHA_TEST: c->alpha_test = on; break;
        case GL_CULL_FACE: c->cull = on; break;
        case GL_STENCIL_TEST: c->stencil = on; break;
        case GL_SCISSOR_TEST: c->scissor_test = on; break;
        case GL_FOG: c->fog = on; break;
        case GL_LIGHTING: c->lighting = on; lights_dirty = 1; break;
        case GL_POLYGON_OFFSET_FILL: c->poly_offset = on; break;
        case GL_COLOR_MATERIAL: c->color_material = on; break;
        case GL_TEXTURE_2D: case GL_TEXTURE_1D: c->unit[c->active_unit].tex2d = on; break;
        default:
            if(cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_GL_LIGHTS) { c->light[cap - GL_LIGHT0].on = on; lights_dirty = 1; }
            break;
    }
}
void APIENTRY glEnable(GLenum cap) { set_cap(cap, 1); }
void APIENTRY glDisable(GLenum cap) { set_cap(cap, 0); }
GLboolean APIENTRY glIsEnabled(GLenum cap)
{
    Ctx *c = cur;
    if(!c) return 0;
    switch(cap)
    {
        case GL_DEPTH_TEST: return c->depth_test; case GL_BLEND: return c->blend; case GL_ALPHA_TEST: return c->alpha_test;
        case GL_CULL_FACE: return c->cull; case GL_STENCIL_TEST: return c->stencil; case GL_SCISSOR_TEST: return c->scissor_test;
        case GL_FOG: return c->fog; case GL_LIGHTING: return c->lighting; case GL_TEXTURE_2D: return c->unit[c->active_unit].tex2d;
        case GL_VERTEX_ARRAY: return c->vertex.on; case GL_COLOR_ARRAY: return c->colors.on;
        case GL_NORMAL_ARRAY: return c->normals.on; case GL_TEXTURE_COORD_ARRAY: return c->texc[c->client_unit].on;
        default: return 0;
    }
}

void APIENTRY glDepthFunc(GLenum f) { C; c->depth_func = f; }
void APIENTRY glDepthMask(GLboolean m) { C; c->depth_mask = m; }
void APIENTRY glDepthRange(GLclampd n, GLclampd f) { C; c->depth_range[0] = n; c->depth_range[1] = f; }
void APIENTRY glBlendFunc(GLenum s, GLenum d) { C; c->bsrc = c->bsrc_a = s; c->bdst = c->bdst_a = d; }
static void APIENTRY glBlendFuncSeparate_(GLenum s, GLenum d, GLenum sa, GLenum da) { C; c->bsrc = s; c->bdst = d; c->bsrc_a = sa; c->bdst_a = da; }
static void APIENTRY glBlendEquation_(GLenum e) { C; c->blend_eq = e; }
static void APIENTRY glBlendColor_(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { C; c->blend_color[0] = r; c->blend_color[1] = g; c->blend_color[2] = b; c->blend_color[3] = a; }
void APIENTRY glAlphaFunc(GLenum f, GLclampf r) { C; c->alpha_func = f; c->alpha_ref = r; }
void APIENTRY glCullFace(GLenum m) { C; c->cull_face = m; }
void APIENTRY glFrontFace(GLenum m) { C; c->front_face = m; }
void APIENTRY glShadeModel(GLenum m) { C; c->shade = m; }
void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { C; c->cmask[0] = r; c->cmask[1] = g; c->cmask[2] = b; c->cmask[3] = a; }
void APIENTRY glStencilFunc(GLenum f, GLint ref, GLuint m) { C; c->st_func = f; c->st_ref = ref; c->st_mask = m; }
void APIENTRY glStencilOp(GLenum f, GLenum zf, GLenum zp) { C; c->st_fail = f; c->st_zfail = zf; c->st_pass = zp; }
void APIENTRY glStencilMask(GLuint m) { C; c->st_wmask = m; }
void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { C; c->scissor[0] = x; c->scissor[1] = y; c->scissor[2] = w; c->scissor[3] = h; }
void APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { C; c->viewport[0] = x; c->viewport[1] = y; c->viewport[2] = w; c->viewport[3] = h; }
void APIENTRY glPolygonOffset(GLfloat f, GLfloat u) { C; c->po_factor = f; c->po_units = u; }
void APIENTRY glPointSize(GLfloat s) { C; c->point_size = s; }
void APIENTRY glLineWidth(GLfloat w) {}
void APIENTRY glPolygonMode(GLenum f, GLenum m) {}
void APIENTRY glHint(GLenum t, GLenum m) {}
void APIENTRY glDrawBuffer(GLenum m) {}
void APIENTRY glReadBuffer(GLenum m) {}
void APIENTRY glClipPlane(GLenum p, const GLdouble *e) { stub("glClipPlane"); }
void APIENTRY glLogicOp(GLenum o) {}
void APIENTRY glLineStipple(GLint f, GLushort p) {}
void APIENTRY glPolygonStipple(const GLubyte *m) {}

void APIENTRY glFogf(GLenum p, GLfloat v)
{
    C;
    switch(p)
    {
        case GL_FOG_MODE: c->fog_mode = (GLenum)v; break;
        case GL_FOG_START: c->fog_start = v; break;
        case GL_FOG_END: c->fog_end = v; break;
        case GL_FOG_DENSITY: c->fog_density = v; break;
    }
}
void APIENTRY glFogi(GLenum p, GLint v) { glFogf(p, p == GL_FOG_MODE ? (GLfloat)(GLenum)v : (GLfloat)v); }
void APIENTRY glFogfv(GLenum p, const GLfloat *v)
{
    C;
    if(p == GL_FOG_COLOR) memcpy(c->fog_color, v, 16);
    else glFogf(p, v[0]);
}
void APIENTRY glFogiv(GLenum p, const GLint *v)
{
    C;
    if(p == GL_FOG_COLOR) for(int i = 0; i < 4; i++) c->fog_color[i] = v[i] / 2147483647.0f;
    else glFogi(p, v[0]);
}

/* clearing */
void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { C; c->clear_color[0] = r; c->clear_color[1] = g; c->clear_color[2] = b; c->clear_color[3] = a; }
void APIENTRY glClearDepth(GLclampd d) { C; c->clear_depth = d; }
void APIENTRY glClearStencil(GLint s) { C; c->clear_stencil = s; }
void APIENTRY glClear(GLbitfield mask)
{
    C;
    LOCK;
    flush_state(c);
    DWORD flags = (mask & GL_COLOR_BUFFER_BIT ? 1 : 0) | (mask & GL_DEPTH_BUFFER_BIT ? 2 : 0) | (mask & GL_STENCIL_BUFFER_BIT ? 4 : 0);
    DWORD *p = cmd(CMD_CLEAR, 9 * 4);
    p[0] = c->cid; p[1] = flags; p[2] = argb(c->clear_color); p[3] = fbits((float)c->clear_depth); p[4] = c->clear_stencil;
    if(c->scissor_test)
    {
        p[5] = c->scissor[0]; p[6] = (int)c->fbh - c->scissor[1] - c->scissor[3]; p[7] = c->scissor[2]; p[8] = c->scissor[3];
    }
    else { p[5] = 0; p[6] = 0; p[7] = c->fbw; p[8] = c->fbh; }
    UNLOCK;
}

/* matrices */
void APIENTRY glMatrixMode(GLenum m) { C; c->matrix_mode = m; }
void APIENTRY glLoadIdentity(void) { C; mat_identity(top()); touched(); }
void APIENTRY glLoadMatrixf(const GLfloat *m) { C; memcpy(top()->m, m, 64); touched(); }
void APIENTRY glLoadMatrixd(const GLdouble *m) { C; for(int i = 0; i < 16; i++) top()->m[i] = (float)m[i]; touched(); }
void APIENTRY glMultMatrixf(const GLfloat *m) { C; Mat t; memcpy(t.m, m, 64); mult(&t); }
void APIENTRY glMultMatrixd(const GLdouble *m) { C; Mat t; for(int i = 0; i < 16; i++) t.m[i] = (float)m[i]; mult(&t); }
void APIENTRY glPushMatrix(void)
{
    C;
    if(c->matrix_mode == GL_PROJECTION) { if(c->proj_depth < 3) { c->proj[c->proj_depth + 1] = c->proj[c->proj_depth]; c->proj_depth++; } }
    else if(c->matrix_mode == GL_TEXTURE) { Unit *u = &c->unit[c->active_unit]; if(u->depth < 3) { u->stack[u->depth + 1] = u->stack[u->depth]; u->depth++; } }
    else if(c->mv_depth < 31) { c->mv[c->mv_depth + 1] = c->mv[c->mv_depth]; c->mv_depth++; }
}
void APIENTRY glPopMatrix(void)
{
    C;
    if(c->matrix_mode == GL_PROJECTION) { if(c->proj_depth) c->proj_depth--; }
    else if(c->matrix_mode == GL_TEXTURE) { Unit *u = &c->unit[c->active_unit]; if(u->depth) u->depth--; }
    else if(c->mv_depth) c->mv_depth--;
    touched();
}
void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    C; Mat t; mat_identity(&t); t.m[12] = x; t.m[13] = y; t.m[14] = z; mult(&t);
}
void APIENTRY glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }
void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z) { C; Mat t; mat_identity(&t); t.m[0] = x; t.m[5] = y; t.m[10] = z; mult(&t); }
void APIENTRY glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }
void APIENTRY glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
    C;
    float l = sqrtf(x * x + y * y + z * z);
    if(l == 0) return;
    x /= l; y /= l; z /= l;
    float r = a * 3.14159265f / 180, s = sinf(r), co = cosf(r), t = 1 - co;
    Mat m; mat_identity(&m);
    m.m[0] = t * x * x + co; m.m[4] = t * x * y - s * z; m.m[8] = t * x * z + s * y;
    m.m[1] = t * x * y + s * z; m.m[5] = t * y * y + co; m.m[9] = t * y * z - s * x;
    m.m[2] = t * x * z - s * y; m.m[6] = t * y * z + s * x; m.m[10] = t * z * z + co;
    mult(&m);
}
void APIENTRY glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }
void APIENTRY glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    C; Mat m; mat_identity(&m);
    m.m[0] = (float)(2 / (r - l)); m.m[5] = (float)(2 / (t - b)); m.m[10] = (float)(-2 / (f - n));
    m.m[12] = (float)(-(r + l) / (r - l)); m.m[13] = (float)(-(t + b) / (t - b)); m.m[14] = (float)(-(f + n) / (f - n));
    mult(&m);
}
void APIENTRY glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    C; Mat m; memset(&m, 0, sizeof m);
    m.m[0] = (float)(2 * n / (r - l)); m.m[5] = (float)(2 * n / (t - b));
    m.m[8] = (float)((r + l) / (r - l)); m.m[9] = (float)((t + b) / (t - b)); m.m[10] = (float)(-(f + n) / (f - n)); m.m[11] = -1;
    m.m[14] = (float)(-2 * f * n / (f - n));
    mult(&m);
}

/* lighting and materials */
static void light_param(Ctx *c, GLenum li, GLenum p, const GLfloat *v)
{
    if(li < GL_LIGHT0 || li >= GL_LIGHT0 + MAX_GL_LIGHTS) return;
    Light *l = &c->light[li - GL_LIGHT0];
    switch(p)
    {
        case GL_AMBIENT: memcpy(l->amb, v, 16); break;
        case GL_DIFFUSE: memcpy(l->dif, v, 16); break;
        case GL_SPECULAR: memcpy(l->spec, v, 16); break;
        case GL_POSITION: xform4(&c->mv[c->mv_depth], v, l->pos); break;
        case GL_SPOT_DIRECTION: { float d[4] = { v[0], v[1], v[2], 0 }, o[4]; xform4(&c->mv[c->mv_depth], d, o); memcpy(l->dir, o, 12); break; }
        case GL_SPOT_EXPONENT: l->spot_exp = v[0]; break;
        case GL_SPOT_CUTOFF: l->spot_cut = v[0]; break;
        case GL_CONSTANT_ATTENUATION: l->att[0] = v[0]; break;
        case GL_LINEAR_ATTENUATION: l->att[1] = v[0]; break;
        case GL_QUADRATIC_ATTENUATION: l->att[2] = v[0]; break;
    }
    lights_dirty = 1;
}
void APIENTRY glLightfv(GLenum l, GLenum p, const GLfloat *v) { C; light_param(c, l, p, v); }
void APIENTRY glLightf(GLenum l, GLenum p, GLfloat v) { C; light_param(c, l, p, &v); }
void APIENTRY glLightiv(GLenum l, GLenum p, const GLint *v) { C; float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }; light_param(c, l, p, f); }
void APIENTRY glLighti(GLenum l, GLenum p, GLint v) { C; float f = (float)v; light_param(c, l, p, &f); }
void APIENTRY glLightModelfv(GLenum p, const GLfloat *v) { C; if(p == GL_LIGHT_MODEL_AMBIENT) memcpy(c->model_ambient, v, 16); }
void APIENTRY glLightModelf(GLenum p, GLfloat v) {}
void APIENTRY glLightModeli(GLenum p, GLint v) {}
void APIENTRY glLightModeliv(GLenum p, const GLint *v) {}
static void material(Ctx *c, GLenum p, const GLfloat *v)
{
    switch(p)
    {
        case GL_AMBIENT: memcpy(c->mat_amb, v, 16); break;
        case GL_DIFFUSE: memcpy(c->mat_dif, v, 16); break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(c->mat_amb, v, 16); memcpy(c->mat_dif, v, 16); break;
        case GL_SPECULAR: memcpy(c->mat_spec, v, 16); break;
        case GL_EMISSION: memcpy(c->mat_emis, v, 16); break;
        case GL_SHININESS: c->mat_shin = v[0]; break;
    }
    lights_dirty = 1;
}
void APIENTRY glMaterialfv(GLenum f, GLenum p, const GLfloat *v) { C; if(f != GL_BACK) material(c, p, v); }
void APIENTRY glMaterialf(GLenum f, GLenum p, GLfloat v) { C; if(f != GL_BACK) material(c, p, &v); }
void APIENTRY glMaterialiv(GLenum f, GLenum p, const GLint *v) { C; float x[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f }; if(p == GL_SHININESS) x[0] = (float)v[0]; if(f != GL_BACK) material(c, p, x); }
void APIENTRY glMateriali(GLenum f, GLenum p, GLint v) { C; float x = (float)v; if(f != GL_BACK) material(c, p, &x); }
void APIENTRY glColorMaterial(GLenum f, GLenum m) { C; c->cm_face = f; c->cm_mode = m; }

/* current attributes */
#define COL4(r, g, b, a) do { C; c->color[0] = (r); c->color[1] = (g); c->color[2] = (b); c->color[3] = (a); } while(0)
void APIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b) { COL4(r, g, b, 1); }
void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { COL4(r, g, b, a); }
void APIENTRY glColor3fv(const GLfloat *v) { COL4(v[0], v[1], v[2], 1); }
void APIENTRY glColor4fv(const GLfloat *v) { COL4(v[0], v[1], v[2], v[3]); }
void APIENTRY glColor3d(GLdouble r, GLdouble g, GLdouble b) { COL4((float)r, (float)g, (float)b, 1); }
void APIENTRY glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { COL4((float)r, (float)g, (float)b, (float)a); }
void APIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b) { COL4(r / 255.f, g / 255.f, b / 255.f, 1); }
void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { COL4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
void APIENTRY glColor3ubv(const GLubyte *v) { COL4(v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, 1); }
void APIENTRY glColor4ubv(const GLubyte *v) { COL4(v[0] / 255.f, v[1] / 255.f, v[2] / 255.f, v[3] / 255.f); }
void APIENTRY glColor3i(GLint r, GLint g, GLint b) { COL4(r / 2147483647.f, g / 2147483647.f, b / 2147483647.f, 1); }
void APIENTRY glColor4i(GLint r, GLint g, GLint b, GLint a) { COL4(r / 2147483647.f, g / 2147483647.f, b / 2147483647.f, a / 2147483647.f); }
void APIENTRY glNormal3f(GLfloat x, GLfloat y, GLfloat z) { C; c->normal[0] = x; c->normal[1] = y; c->normal[2] = z; }
void APIENTRY glNormal3fv(const GLfloat *v) { glNormal3f(v[0], v[1], v[2]); }
void APIENTRY glNormal3d(GLdouble x, GLdouble y, GLdouble z) { glNormal3f((float)x, (float)y, (float)z); }
void APIENTRY glNormal3dv(const GLdouble *v) { glNormal3f((float)v[0], (float)v[1], (float)v[2]); }
static void tc4(int u, float s, float t, float r, float q) { C; float *x = c->unit[u].tc; x[0] = s; x[1] = t; x[2] = r; x[3] = q; }
void APIENTRY glTexCoord1f(GLfloat s) { tc4(0, s, 0, 0, 1); }
void APIENTRY glTexCoord2f(GLfloat s, GLfloat t) { tc4(0, s, t, 0, 1); }
void APIENTRY glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { tc4(0, s, t, r, 1); }
void APIENTRY glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { tc4(0, s, t, r, q); }
void APIENTRY glTexCoord2fv(const GLfloat *v) { tc4(0, v[0], v[1], 0, 1); }
void APIENTRY glTexCoord3fv(const GLfloat *v) { tc4(0, v[0], v[1], v[2], 1); }
void APIENTRY glTexCoord4fv(const GLfloat *v) { tc4(0, v[0], v[1], v[2], v[3]); }
void APIENTRY glTexCoord2d(GLdouble s, GLdouble t) { tc4(0, (float)s, (float)t, 0, 1); }
void APIENTRY glTexCoord2dv(const GLdouble *v) { tc4(0, (float)v[0], (float)v[1], 0, 1); }
void APIENTRY glTexCoord2i(GLint s, GLint t) { tc4(0, (float)s, (float)t, 0, 1); }
static void APIENTRY glMultiTexCoord2f_(GLenum u, GLfloat s, GLfloat t) { tc4((u - GL_TEXTURE0) & (MAX_UNITS - 1), s, t, 0, 1); }
static void APIENTRY glMultiTexCoord2fv_(GLenum u, const GLfloat *v) { tc4((u - GL_TEXTURE0) & (MAX_UNITS - 1), v[0], v[1], 0, 1); }
static void APIENTRY glMultiTexCoord3f_(GLenum u, GLfloat s, GLfloat t, GLfloat r) { tc4((u - GL_TEXTURE0) & (MAX_UNITS - 1), s, t, r, 1); }
static void APIENTRY glMultiTexCoord4f_(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { tc4((u - GL_TEXTURE0) & (MAX_UNITS - 1), s, t, r, q); }

void APIENTRY glVertex2f(GLfloat x, GLfloat y) { imm_vertex(x, y, 0, 1); }
void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z) { imm_vertex(x, y, z, 1); }
void APIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { imm_vertex(x, y, z, w); }
void APIENTRY glVertex2fv(const GLfloat *v) { imm_vertex(v[0], v[1], 0, 1); }
void APIENTRY glVertex3fv(const GLfloat *v) { imm_vertex(v[0], v[1], v[2], 1); }
void APIENTRY glVertex4fv(const GLfloat *v) { imm_vertex(v[0], v[1], v[2], v[3]); }
void APIENTRY glVertex2i(GLint x, GLint y) { imm_vertex((float)x, (float)y, 0, 1); }
void APIENTRY glVertex3i(GLint x, GLint y, GLint z) { imm_vertex((float)x, (float)y, (float)z, 1); }
void APIENTRY glVertex2d(GLdouble x, GLdouble y) { imm_vertex((float)x, (float)y, 0, 1); }
void APIENTRY glVertex3d(GLdouble x, GLdouble y, GLdouble z) { imm_vertex((float)x, (float)y, (float)z, 1); }
void APIENTRY glVertex3dv(const GLdouble *v) { imm_vertex((float)v[0], (float)v[1], (float)v[2], 1); }
void APIENTRY glVertex2s(GLshort x, GLshort y) { imm_vertex(x, y, 0, 1); }
void APIENTRY glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
    C;
    c->in_begin = 1; c->begin_mode = GL_QUADS;
    imm_vertex(x1, y1, 0, 1); imm_vertex(x2, y1, 0, 1); imm_vertex(x2, y2, 0, 1); imm_vertex(x1, y2, 0, 1);
    c->in_begin = 0;
    LOCK; imm_end(c); UNLOCK;
}
void APIENTRY glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }

void APIENTRY glBegin(GLenum mode) { C; c->in_begin = 1; c->begin_mode = mode; imm_n = 0; }
void APIENTRY glEnd(void)
{
    C;
    c->in_begin = 0;
    LOCK; imm_end(c); UNLOCK;
}

/* client arrays */
static void set_array(Array *a, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    Ctx *c = cur;
    a->bgra = size == GL_BGRA;
    if(a->bgra) size = 4;
    a->size = size; a->type = type; a->stride = stride; a->ptr = ptr;
    a->buf = c->array_buffer ? get_buf(c->array_buffer, 0) : NULL;
}
void APIENTRY glVertexPointer(GLint s, GLenum t, GLsizei st, const GLvoid *p) { C; set_array(&c->vertex, s, t, st, p); }
void APIENTRY glColorPointer(GLint s, GLenum t, GLsizei st, const GLvoid *p) { C; set_array(&c->colors, s, t, st, p); }
void APIENTRY glNormalPointer(GLenum t, GLsizei st, const GLvoid *p) { C; set_array(&c->normals, 3, t, st, p); }
void APIENTRY glTexCoordPointer(GLint s, GLenum t, GLsizei st, const GLvoid *p) { C; set_array(&c->texc[c->client_unit], s, t, st, p); }
static Array *array_for(GLenum cap)
{
    Ctx *c = cur;
    switch(cap)
    {
        case GL_VERTEX_ARRAY: return &c->vertex; case GL_COLOR_ARRAY: return &c->colors;
        case GL_NORMAL_ARRAY: return &c->normals; case GL_TEXTURE_COORD_ARRAY: return &c->texc[c->client_unit];
        default: return NULL;
    }
}
void APIENTRY glEnableClientState(GLenum cap) { C; Array *a = array_for(cap); if(a) a->on = 1; }
void APIENTRY glDisableClientState(GLenum cap) { C; Array *a = array_for(cap); if(a) a->on = 0; }
void APIENTRY glInterleavedArrays(GLenum f, GLsizei s, const GLvoid *p) { stub("glInterleavedArrays"); }
void APIENTRY glArrayElement(GLint i)
{
    C;
    if(!c->in_begin || !c->vertex.on) return;
    float v[4] = { 0, 0, 0, 1 };
    const Array *a;
    int u;
    for(u = 0; u < MAX_UNITS; u++)
    {
        a = &c->texc[u];
        if(!a->on) continue;
        const BYTE *p = (a->buf ? a->buf->mem + (size_t)a->ptr : a->ptr) + i * (a->stride ? a->stride : a->size * type_size(a->type));
        float t[4] = { 0, 0, 0, 1 };
        for(int k = 0; k < a->size; k++) t[k] = read_comp(p + k * type_size(a->type), a->type, 0);
        memcpy(c->unit[u].tc, t, 16);
    }
    a = &c->colors;
    if(a->on)
    {
        const BYTE *p = (a->buf ? a->buf->mem + (size_t)a->ptr : a->ptr) + i * (a->stride ? a->stride : a->size * type_size(a->type));
        for(int k = 0; k < 4; k++) c->color[k] = k < a->size ? read_comp(p + k * type_size(a->type), a->type, 1) : 1;
    }
    a = &c->normals;
    if(a->on)
    {
        const BYTE *p = (a->buf ? a->buf->mem + (size_t)a->ptr : a->ptr) + i * (a->stride ? a->stride : 3 * type_size(a->type));
        for(int k = 0; k < 3; k++) c->normal[k] = read_comp(p + k * type_size(a->type), a->type, 1);
    }
    a = &c->vertex;
    const BYTE *p = (a->buf ? a->buf->mem + (size_t)a->ptr : a->ptr) + i * (a->stride ? a->stride : a->size * type_size(a->type));
    for(int k = 0; k < a->size; k++) v[k] = read_comp(p + k * type_size(a->type), a->type, 0);
    imm_vertex(v[0], v[1], v[2], v[3]);
}
void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei n)
{
    C;
    if(!c->vertex.on || n <= 0) return;
    LOCK;
    if(direct_ok(c, mode)) draw_direct(c, mode, first, n, NULL, 0, 0, NULL);
    else draw_arrays(c, mode, first, n, NULL, 0, NULL, 0, 0);
    UNLOCK;
}
static void draw_elements(GLenum mode, GLsizei n, GLenum type, const GLvoid *ind, int have_range, UINT lo, UINT hi)
{
    C;
    Buf *eb = c->element_buffer ? get_buf(c->element_buffer, 0) : NULL;
    const BYTE *src = eb ? eb->mem + (size_t)ind : ind;
    if(!src || n <= 0) return;
    UINT isz = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
    if(!have_range)
    {
        if(c->lock_count) { lo = c->lock_first; hi = c->lock_first + c->lock_count - 1; }
        else
        {
            lo = 0xFFFFFFFF; hi = 0;
            for(GLsizei i = 0; i < n; i++)
            {
                UINT v = isz == 1 ? src[i] : isz == 2 ? ((const WORD *)src)[i] : ((const DWORD *)src)[i];
                if(v < lo) lo = v;
                if(v > hi) hi = v;
            }
        }
    }
    LOCK;
    if(c->vertex.on && hi >= lo && direct_ok(c, mode))
    {
        draw_direct(c, mode, lo, hi - lo + 1, eb ? ind : src, n, isz, eb);
        UNLOCK;
        return;
    }
    if(hi - lo + 1 > 65535 || hi < lo) { hlog("glDrawElements: index range %u..%u not supported", lo, hi); UNLOCK; return; }
    arena_reserve(n * 6 + (hi - lo + 1) * 16 * (4 + MAX_UNITS) + 4096);
    WORD *idx = (WORD *)arena_alloc(n * 2 + 16);
    if(!idx) { UNLOCK; return; }
    for(GLsizei i = 0; i < n; i++)
    {
        UINT v = isz == 1 ? src[i] : isz == 2 ? ((const WORD *)src)[i] : ((const DWORD *)src)[i];
        idx[i] = (WORD)(v - lo);
    }
    draw_arrays(c, mode, lo, hi - lo + 1, idx, n, NULL, 0, 0);
    UNLOCK;
}
void APIENTRY glDrawElements(GLenum mode, GLsizei n, GLenum type, const GLvoid *ind) { draw_elements(mode, n, type, ind, 0, 0, 0); }
static void APIENTRY glDrawRangeElements_(GLenum mode, GLuint lo, GLuint hi, GLsizei n, GLenum type, const GLvoid *ind) { draw_elements(mode, n, type, ind, 1, lo, hi); }
static void APIENTRY glLockArraysEXT_(GLint first, GLsizei count) { C; c->lock_first = first; c->lock_count = count; }
static void APIENTRY glUnlockArraysEXT_(void) { C; c->lock_count = 0; }

/* ---------- textures ---------- */

void APIENTRY glGenTextures(GLsizei n, GLuint *t)
{
    for(GLsizei i = 0; i < n; i++)
    {
        while(next_tex_name < ntextures && textures[next_tex_name]) next_tex_name++;
        t[i] = next_tex_name++;
        get_tex(t[i], 1);
    }
}
void APIENTRY glDeleteTextures(GLsizei n, const GLuint *t)
{
    C;
    LOCK;
    for(GLsizei i = 0; i < n; i++)
    {
        Tex *x = get_tex(t[i], 0);
        if(!x) continue;
        if(x->sid) surface_destroy(x->sid);
        free(x);
        textures[t[i]] = NULL;
        for(int u = 0; u < MAX_UNITS; u++) if(c->unit[u].bound == t[i]) c->unit[u].bound = 0;
    }
    UNLOCK;
}
void APIENTRY glBindTexture(GLenum target, GLuint t) { C; get_tex(t, 1); c->unit[c->active_unit].bound = t; }
GLboolean APIENTRY glIsTexture(GLuint t) { return get_tex(t, 0) != NULL; }
GLboolean APIENTRY glAreTexturesResident(GLsizei n, const GLuint *t, GLboolean *r) { for(GLsizei i = 0; i < n; i++) r[i] = 1; return 1; }
void APIENTRY glPrioritizeTextures(GLsizei n, const GLuint *t, const GLclampf *p) {}
static Tex *bound_tex(void) { Ctx *c = cur; return get_tex(c->unit[c->active_unit].bound, 1); }
static void tex_param(GLenum p, float v)
{
    Tex *t = bound_tex();
    if(!t) return;
    switch(p)
    {
        case GL_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)v; break;
        case GL_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)v; break;
        case GL_TEXTURE_WRAP_S: t->wrap_s = (GLenum)v; break;
        case GL_TEXTURE_WRAP_T: t->wrap_t = (GLenum)v; break;
        case GL_TEXTURE_MAX_ANISOTROPY_EXT: t->aniso = v; break;
        case GL_GENERATE_MIPMAP: t->genmip = v != 0; break;
    }
}
void APIENTRY glTexParameteri(GLenum target, GLenum p, GLint v) { C; tex_param(p, p == GL_TEXTURE_MAX_ANISOTROPY_EXT ? (float)v : (float)(GLenum)v); }
void APIENTRY glTexParameterf(GLenum target, GLenum p, GLfloat v) { C; tex_param(p, v); }
void APIENTRY glTexParameteriv(GLenum target, GLenum p, const GLint *v) { glTexParameteri(target, p, v[0]); }
void APIENTRY glTexParameterfv(GLenum target, GLenum p, const GLfloat *v) { glTexParameterf(target, p, v[0]); }
void APIENTRY glGetTexParameteriv(GLenum target, GLenum p, GLint *v)
{
    Tex *t = bound_tex();
    *v = 0;
    if(!t) return;
    switch(p)
    {
        case GL_TEXTURE_MIN_FILTER: *v = t->min_filter; break;
        case GL_TEXTURE_MAG_FILTER: *v = t->mag_filter; break;
        case GL_TEXTURE_WRAP_S: *v = t->wrap_s; break;
        case GL_TEXTURE_WRAP_T: *v = t->wrap_t; break;
    }
}
void APIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum p, GLint *v)
{
    Tex *t = bound_tex();
    *v = 0;
    if(!t || !t->sid) return;
    UINT w = t->w >> level, h = t->h >> level;
    if(p == GL_TEXTURE_WIDTH) *v = w ? w : 1;
    else if(p == GL_TEXTURE_HEIGHT) *v = h ? h : 1;
    else if(p == GL_TEXTURE_INTERNAL_FORMAT) *v = t->base;
}

void APIENTRY glTexEnvf(GLenum target, GLenum p, GLfloat v)
{
    C;
    Unit *u = &c->unit[c->active_unit];
    GLenum e = (GLenum)v;
    switch(p)
    {
        case GL_TEXTURE_ENV_MODE: u->mode = e; break;
        case GL_COMBINE_RGB: u->combine_rgb = e; break;
        case GL_COMBINE_ALPHA: u->combine_alpha = e; break;
        case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB: u->src_rgb[p - GL_SOURCE0_RGB] = e; break;
        case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA: u->src_alpha[p - GL_SOURCE0_ALPHA] = e; break;
        case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: u->op_rgb[p - GL_OPERAND0_RGB] = e; break;
        case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: u->op_alpha[p - GL_OPERAND0_ALPHA] = e; break;
        case GL_RGB_SCALE: u->rgb_scale = v; break;
        case GL_ALPHA_SCALE: u->alpha_scale = v; break;
    }
}
void APIENTRY glTexEnvi(GLenum target, GLenum p, GLint v) { glTexEnvf(target, p, (p == GL_RGB_SCALE || p == GL_ALPHA_SCALE) ? (float)v : (float)(GLenum)v); }
void APIENTRY glTexEnvfv(GLenum target, GLenum p, const GLfloat *v)
{
    C;
    if(p == GL_TEXTURE_ENV_COLOR) memcpy(c->unit[c->active_unit].color, v, 16);
    else glTexEnvf(target, p, v[0]);
}
void APIENTRY glTexEnviv(GLenum target, GLenum p, const GLint *v)
{
    C;
    if(p == GL_TEXTURE_ENV_COLOR) for(int i = 0; i < 4; i++) c->unit[c->active_unit].color[i] = v[i] / 2147483647.0f;
    else glTexEnvi(target, p, v[0]);
}
void APIENTRY glTexGeni(GLenum c_, GLenum p, GLint v) { stub("glTexGen"); }
void APIENTRY glTexGenf(GLenum c_, GLenum p, GLfloat v) { stub("glTexGen"); }
void APIENTRY glTexGenfv(GLenum c_, GLenum p, const GLfloat *v) { stub("glTexGen"); }
void APIENTRY glPixelStorei(GLenum p, GLint v)
{
    C;
    switch(p)
    {
        case GL_UNPACK_ALIGNMENT: c->unpack_align = v; break;
        case GL_UNPACK_ROW_LENGTH: c->unpack_row = v; break;
        case GL_UNPACK_SKIP_ROWS: c->unpack_skip_rows = v; break;
        case GL_UNPACK_SKIP_PIXELS: c->unpack_skip_pixels = v; break;
        case GL_PACK_ALIGNMENT: c->pack_align = v; break;
    }
}
void APIENTRY glPixelStoref(GLenum p, GLfloat v) { glPixelStorei(p, (GLint)v); }

/* Host format and bytes per pixel of client pixel data; 0 if it needs converting. */
static DWORD client_format(GLenum format, GLenum type, UINT *bpp)
{
    /* Packed types whose little-endian layout is an SVGA3D format. */
    if(format == GL_BGRA_EXT && type == GL_UNSIGNED_INT_8_8_8_8_REV) { *bpp = 4; return SF_A8R8G8B8; }
    if(format == GL_RGBA && type == GL_UNSIGNED_INT_8_8_8_8_REV) { *bpp = 4; return SF_RGBA8; }
    if(format == GL_BGRA_EXT && type == GL_UNSIGNED_SHORT_1_5_5_5_REV) { *bpp = 2; return SF_A1R5G5B5; }
    if(format == GL_BGRA_EXT && type == GL_UNSIGNED_SHORT_4_4_4_4_REV) { *bpp = 2; return SF_A4R4G4B4; }
    if(format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) { *bpp = 2; return SF_R5G6B5; }
    if(type != GL_UNSIGNED_BYTE) return 0;
    switch(format)
    {
        case GL_RGBA: *bpp = 4; return SF_RGBA8;
        case GL_BGRA_EXT: *bpp = 4; return SF_A8R8G8B8;
        case GL_RGB: *bpp = 3; return SF_RGB8;
        case GL_BGR_EXT: *bpp = 3; return SF_BGR8;
        case GL_LUMINANCE: *bpp = 1; return SF_LUMINANCE8;
        case GL_LUMINANCE_ALPHA: *bpp = 2; return SF_LUMINANCE8_ALPHA8;
        case GL_ALPHA: *bpp = 1; return SF_ALPHA8;
        case GL_INTENSITY: *bpp = 1; return SF_INTENSITY8;
        default: return 0;
    }
}
static GLenum base_format(GLint internal)
{
    switch(internal)
    {
        case 1: case GL_LUMINANCE: case GL_LUMINANCE8: case GL_LUMINANCE4: case GL_LUMINANCE16: return GL_LUMINANCE;
        case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE4_ALPHA4: return GL_LUMINANCE_ALPHA;
        case GL_ALPHA: case GL_ALPHA8: case GL_ALPHA4: case GL_ALPHA16: return GL_ALPHA;
        case GL_INTENSITY: case GL_INTENSITY8: case GL_INTENSITY4: case GL_INTENSITY16: return GL_INTENSITY;
        case 3: case GL_RGB: case GL_RGB8: case GL_RGB5: case GL_RGB4: case GL_RGB16: case GL_RGB10:
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: case GL_COMPRESSED_RGB_ARB: return GL_RGB;
        default: return GL_RGBA;
    }
}

/* (Re)define the texture's host surface when level 0 changes size or kind. */
static void define_tex(Tex *t, UINT w, UINT h, int compressed, DWORD fmt)
{
    if(t->sid && t->w == w && t->h == h && t->compressed == compressed) return;
    if(t->sid) surface_destroy(t->sid);
    UINT levels = 1;
    while((w >> levels) || (h >> levels)) levels++;
    t->sid = new_id(); t->w = w; t->h = h; t->levels = levels; t->compressed = compressed;
    surface_define(t->sid, fmt, w, h, levels);
}

static void tex_upload(Tex *t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    Ctx *c = cur;
    if(!pixels || w <= 0 || h <= 0) return;
    if(c->unpack_align == 0) c->unpack_align = 4;
    UINT bpp;
    DWORD fmt = client_format(format, type, &bpp);
    const BYTE *src = pixels;
    BYTE *conv = NULL;
    UINT row = c->unpack_row ? (UINT)c->unpack_row : (UINT)w;
    UINT pitch;
    if(!fmt)
    {
        /* Convert to RGBA bytes. */
        conv = malloc(w * h * 4);
        UINT comps = format == GL_RGBA || format == GL_BGRA_EXT ? 4 : format == GL_RGB || format == GL_BGR_EXT ? 3 :
            format == GL_LUMINANCE_ALPHA ? 2 : 1;
        UINT tsz = type_size(type);
        if(type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) { comps = 1; tsz = 2; }
        else if(type != GL_UNSIGNED_BYTE && type != GL_BYTE && type != GL_UNSIGNED_SHORT && type != GL_SHORT &&
            type != GL_UNSIGNED_INT && type != GL_INT && type != GL_FLOAT)
        {
            char t[64];
            snprintf(t, sizeof t, "glTexImage(format 0x%x type 0x%x)", format, type);
            stub(t);
            free(conv);
            return;
        }
        UINT sp = (row * comps * tsz + c->unpack_align - 1) & ~(c->unpack_align - 1);
        for(GLsizei j = 0; j < h; j++)
        {
            const BYTE *s = src + (j + c->unpack_skip_rows) * sp + c->unpack_skip_pixels * comps * tsz;
            for(GLsizei i = 0; i < w; i++)
            {
                BYTE *o = conv + (j * w + i) * 4;
                if(type == GL_UNSIGNED_SHORT_5_6_5) { WORD v = ((const WORD *)s)[i]; o[0] = (v >> 11) * 255 / 31; o[1] = (v >> 5 & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; continue; }
                if(type == GL_UNSIGNED_SHORT_4_4_4_4) { WORD v = ((const WORD *)s)[i]; o[0] = (v >> 12) * 17; o[1] = (v >> 8 & 15) * 17; o[2] = (v >> 4 & 15) * 17; o[3] = (v & 15) * 17; continue; }
                if(type == GL_UNSIGNED_SHORT_5_5_5_1) { WORD v = ((const WORD *)s)[i]; o[0] = (v >> 11) * 255 / 31; o[1] = (v >> 6 & 31) * 255 / 31; o[2] = (v >> 1 & 31) * 255 / 31; o[3] = v & 1 ? 255 : 0; continue; }
                float v[4] = { 0, 0, 0, 1 };
                for(UINT k = 0; k < comps; k++) v[k] = read_comp(s + (i * comps + k) * tsz, type, 1);
                if(format == GL_BGRA_EXT || format == GL_BGR_EXT) { float tmp = v[0]; v[0] = v[2]; v[2] = tmp; }
                if(format == GL_LUMINANCE || format == GL_INTENSITY) { v[1] = v[2] = v[0]; if(format == GL_INTENSITY) v[3] = v[0]; }
                if(format == GL_LUMINANCE_ALPHA) { v[3] = v[1]; v[1] = v[2] = v[0]; }
                if(format == GL_ALPHA) { v[3] = v[0]; v[0] = v[1] = v[2] = 0; }
                for(int k = 0; k < 4; k++) o[k] = (BYTE)(v[k] < 0 ? 0 : v[k] > 1 ? 255 : v[k] * 255 + 0.5f);
            }
        }
        fmt = SF_RGBA8; bpp = 4; src = conv; pitch = w * 4;
    }
    else
    {
        pitch = (row * bpp + c->unpack_align - 1) & ~(c->unpack_align - 1);
        src += c->unpack_skip_rows * pitch + c->unpack_skip_pixels * bpp;
    }
    dma_format(fmt, t->sid, level, src, pitch, x, y, w, h, 0, 0);
    if(t->genmip && level == 0) { DWORD *p = cmd(CMD_GENERATE_MIPMAPS, 8); p[0] = t->sid; p[1] = 2; }
    gl_flush();
    free(conv);
}

void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
    GLenum format, GLenum type, const GLvoid *pixels)
{
    C;
    if(target != GL_TEXTURE_2D) { stub("glTexImage2D(target)"); return; }
    Tex *t = bound_tex();
    if(!t) return;
    LOCK;
    if(level == 0) { define_tex(t, w, h, 0, SF_A8R8G8B8); t->base = base_format(internal); }
    else if(!t->sid) define_tex(t, w << level, h << level, 0, SF_A8R8G8B8);
    tex_upload(t, level, 0, 0, w, h, format, type, pixels);
    UNLOCK;
}
void APIENTRY glTexImage1D(GLenum target, GLint level, GLint internal, GLsizei w, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
    glTexImage2D(GL_TEXTURE_2D, level, internal, w, 1, border, format, type, pixels);
}
void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
    GLenum type, const GLvoid *pixels)
{
    C;
    Tex *t = bound_tex();
    if(!t || !t->sid) return;
    LOCK;
    tex_upload(t, level, x, y, w, h, format, type, pixels);
    UNLOCK;
}
static void APIENTRY glCompressedTexImage2D_(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h,
    GLint border, GLsizei size, const GLvoid *data)
{
    C;
    Tex *t = bound_tex();
    if(!t || !data) return;
    DWORD fmt = internal == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? SF_DXT1 :
        internal == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? SF_DXT3 : internal == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? SF_DXT5 : 0;
    if(!fmt) { stub("glCompressedTexImage2D(format)"); return; }
    LOCK;
    if(level == 0) { define_tex(t, w, h, (int)fmt, fmt); t->base = base_format(internal); }
    UINT bw = (w + 3) / 4, block = fmt == SF_DXT1 ? 8 : 16;
    dma(t->sid, level, data, (bw ? bw : 1) * block, XFER_TO_HOST, 0, 0, w, h);
    gl_flush();
    UNLOCK;
}
void APIENTRY glCopyTexImage2D(GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) { stub("glCopyTexImage2D"); }
void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) { stub("glCopyTexSubImage2D"); }

static void APIENTRY glActiveTexture_(GLenum u) { C; c->active_unit = (u - GL_TEXTURE0) & (MAX_UNITS - 1); }
static void APIENTRY glClientActiveTexture_(GLenum u) { C; c->client_unit = (u - GL_TEXTURE0) & (MAX_UNITS - 1); }

/* ---------- buffer objects ---------- */

static void APIENTRY glGenBuffers_(GLsizei n, GLuint *b)
{
    for(GLsizei i = 0; i < n; i++)
    {
        while(next_buf_name < nbuffers && buffers[next_buf_name]) next_buf_name++;
        b[i] = next_buf_name++;
        get_buf(b[i], 1);
    }
}
static void APIENTRY glDeleteBuffers_(GLsizei n, const GLuint *b)
{
    LOCK;
    for(GLsizei i = 0; i < n; i++)
    {
        Buf *x = get_buf(b[i], 0);
        if(!x) continue;
        if(x->sid) surface_destroy(x->sid);
        if(x->mem) VirtualFree(x->mem, 0, MEM_RELEASE);
        free(x);
        buffers[b[i]] = NULL;
    }
    UNLOCK;
}
static void APIENTRY glBindBuffer_(GLenum target, GLuint b)
{
    C;
    get_buf(b, 1);
    if(target == GL_ELEMENT_ARRAY_BUFFER) c->element_buffer = b; else c->array_buffer = b;
}
static Buf *target_buf(GLenum target) { Ctx *c = cur; return get_buf(target == GL_ELEMENT_ARRAY_BUFFER ? c->element_buffer : c->array_buffer, 0); }
static void APIENTRY glBufferData_(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
    C;
    Buf *b = target_buf(target);
    if(!b) return;
    LOCK;
    if(b->size != (UINT)size)
    {
        if(b->sid) surface_destroy(b->sid);
        if(b->mem) VirtualFree(b->mem, 0, MEM_RELEASE);
        b->size = size; b->mem = VirtualAlloc(0, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        b->sid = new_id();
        DWORD *p = cmd(CMD_SURFACE_DEFINE, 12 * 4);
        p[0] = b->sid; p[1] = 0; p[2] = SF_BUFFER; p[3] = 1; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
        p[9] = size; p[10] = 1; p[11] = 1;
    }
    if(data && size)
    {
        memcpy(b->mem, data, size);
        dma(b->sid, 0, b->mem, 0, XFER_TO_HOST, 0, 0, size, 1);
        gl_flush();
    }
    UNLOCK;
}
static void APIENTRY glBufferSubData_(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data)
{
    C;
    Buf *b = target_buf(target);
    if(!b || !b->mem || off + size > b->size) return;
    LOCK;
    memcpy(b->mem + off, data, size);
    dma(b->sid, 0, b->mem, 0, XFER_TO_HOST, off, 0, size, 1);
    gl_flush();
    UNLOCK;
}
static GLvoid *APIENTRY glMapBuffer_(GLenum target, GLenum access) { C 0; Buf *b = target_buf(target); if(!b) return NULL; b->mapped = b->mem; return b->mem; }
static GLboolean APIENTRY glUnmapBuffer_(GLenum target)
{
    C 0;
    Buf *b = target_buf(target);
    if(!b || !b->mapped) return 0;
    b->mapped = NULL;
    LOCK;
    dma(b->sid, 0, b->mem, 0, XFER_TO_HOST, 0, 0, b->size, 1);
    gl_flush();
    UNLOCK;
    return 1;
}
static void APIENTRY glGetBufferParameteriv_(GLenum target, GLenum p, GLint *v)
{
    C; Buf *b = target_buf(target);
    *v = !b ? 0 : p == GL_BUFFER_SIZE ? (GLint)b->size : p == GL_BUFFER_USAGE ? GL_STATIC_DRAW : 0;
}
static GLboolean APIENTRY glIsBuffer_(GLuint b) { return get_buf(b, 0) != NULL; }

/* ---------- queries ---------- */

static const char *extensions =
    "GL_ARB_multitexture GL_ARB_texture_env_add GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 "
    "GL_EXT_texture_env_add GL_EXT_texture_env_combine GL_EXT_compiled_vertex_array GL_EXT_texture_edge_clamp "
    "GL_SGIS_texture_edge_clamp GL_ARB_texture_border_clamp GL_ARB_texture_mirrored_repeat GL_EXT_bgra "
    "GL_EXT_texture_filter_anisotropic GL_ARB_texture_compression GL_EXT_texture_compression_s3tc "
    "GL_SGIS_generate_mipmap GL_EXT_blend_func_separate GL_EXT_blend_color GL_EXT_blend_minmax GL_EXT_blend_subtract "
    "GL_EXT_draw_range_elements GL_ARB_vertex_buffer_object GL_EXT_stencil_wrap GL_ARB_texture_non_power_of_two "
    "GL_EXT_vertex_array_bgra WGL_EXT_swap_control";

const GLubyte *APIENTRY glGetString(GLenum name)
{
    switch(name)
    {
        case GL_VENDOR: return (const GLubyte *)"Webulator";
        case GL_RENDERER: return (const GLubyte *)"Webulator OpenGL forwarding (WebGL2)";
        case GL_VERSION: return (const GLubyte *)"1.5 Webulator";
        case GL_EXTENSIONS: return (const GLubyte *)extensions;
        default: return (const GLubyte *)"";
    }
}
GLenum APIENTRY glGetError(void) { return GL_NO_ERROR; }

static int get_values(GLenum p, float *v)
{
    Ctx *c = cur;
    switch(p)
    {
        case GL_MAX_TEXTURE_SIZE: v[0] = 4096; return 1;
        case GL_MAX_TEXTURE_UNITS: v[0] = MAX_UNITS; return 1;
        case GL_MAX_LIGHTS: v[0] = MAX_GL_LIGHTS; return 1;
        case GL_MAX_CLIP_PLANES: v[0] = 6; return 1;
        case GL_MAX_MODELVIEW_STACK_DEPTH: v[0] = 32; return 1;
        case GL_MAX_PROJECTION_STACK_DEPTH: case GL_MAX_TEXTURE_STACK_DEPTH: v[0] = 4; return 1;
        case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: v[0] = 65535; return 1;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v[0] = 16; return 1;
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: case GL_STENCIL_BITS: v[0] = 8; return 1;
        case GL_DEPTH_BITS: v[0] = 24; return 1;
        case GL_DOUBLEBUFFER: v[0] = 1; return 1;
        case GL_VIEWPORT: for(int i = 0; i < 4; i++) v[i] = (float)c->viewport[i]; return 4;
        case GL_SCISSOR_BOX: for(int i = 0; i < 4; i++) v[i] = (float)c->scissor[i]; return 4;
        case GL_MATRIX_MODE: v[0] = (float)c->matrix_mode; return 1;
        case GL_MODELVIEW_MATRIX: memcpy(v, c->mv[c->mv_depth].m, 64); return 16;
        case GL_PROJECTION_MATRIX: memcpy(v, c->proj[c->proj_depth].m, 64); return 16;
        case GL_TEXTURE_MATRIX: memcpy(v, c->unit[c->active_unit].stack[c->unit[c->active_unit].depth].m, 64); return 16;
        case GL_TEXTURE_BINDING_2D: v[0] = (float)c->unit[c->active_unit].bound; return 1;
        case GL_ACTIVE_TEXTURE: v[0] = (float)(GL_TEXTURE0 + c->active_unit); return 1;
        case GL_CLIENT_ACTIVE_TEXTURE: v[0] = (float)(GL_TEXTURE0 + c->client_unit); return 1;
        case GL_UNPACK_ALIGNMENT: v[0] = (float)c->unpack_align; return 1;
        case GL_PACK_ALIGNMENT: v[0] = (float)c->pack_align; return 1;
        case GL_CURRENT_COLOR: memcpy(v, c->color, 16); return 4;
        case GL_DEPTH_FUNC: v[0] = (float)c->depth_func; return 1;
        case GL_DEPTH_WRITEMASK: v[0] = c->depth_mask; return 1;
        case GL_BLEND_SRC: v[0] = (float)c->bsrc; return 1;
        case GL_BLEND_DST: v[0] = (float)c->bdst; return 1;
        case GL_COLOR_CLEAR_VALUE: memcpy(v, c->clear_color, 16); return 4;
        case GL_ARRAY_BUFFER_BINDING: v[0] = (float)c->array_buffer; return 1;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = (float)c->element_buffer; return 1;
        case GL_SUBPIXEL_BITS: v[0] = 4; return 1;
        case GL_ALIASED_POINT_SIZE_RANGE: case GL_POINT_SIZE_RANGE: v[0] = 1; v[1] = 64; return 2;
        case GL_ALIASED_LINE_WIDTH_RANGE: case GL_LINE_WIDTH_RANGE: v[0] = v[1] = 1; return 2;
        case GL_AUX_BUFFERS: v[0] = 0; return 1;
        case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 4096; return 2;
        case GL_NUM_COMPRESSED_TEXTURE_FORMATS: v[0] = 3; return 1;
        case GL_COMPRESSED_TEXTURE_FORMATS: v[0] = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; v[1] = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; v[2] = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; return 3;
        default:
        {
            char t[48];
            snprintf(t, sizeof t, "glGet(0x%x)", p);
            stub(t);
            v[0] = 0;
            return 1;
        }
    }
}
void APIENTRY glGetFloatv(GLenum p, GLfloat *v) { C; float t[16]; int n = get_values(p, t); memcpy(v, t, n * 4); }
void APIENTRY glGetDoublev(GLenum p, GLdouble *v) { C; float t[16]; int n = get_values(p, t); for(int i = 0; i < n; i++) v[i] = t[i]; }
void APIENTRY glGetIntegerv(GLenum p, GLint *v) { C; float t[16]; int n = get_values(p, t); for(int i = 0; i < n; i++) v[i] = (GLint)t[i]; }
void APIENTRY glGetBooleanv(GLenum p, GLboolean *v) { C; float t[16]; int n = get_values(p, t); for(int i = 0; i < n; i++) v[i] = t[i] != 0; }
void APIENTRY glGetPointerv(GLenum p, GLvoid **v) { *v = NULL; }
void APIENTRY glGetLightfv(GLenum l, GLenum p, GLfloat *v) { memset(v, 0, 16); }
void APIENTRY glGetMaterialfv(GLenum f, GLenum p, GLfloat *v) { memset(v, 0, 16); }
void APIENTRY glGetTexEnviv(GLenum t, GLenum p, GLint *v) { *v = 0; }
void APIENTRY glGetTexEnvfv(GLenum t, GLenum p, GLfloat *v) { *v = 0; }

void APIENTRY glFlush(void) { if(cur) { LOCK; gl_flush(); UNLOCK; } }
void APIENTRY glFinish(void) { glFlush(); }

void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    C;
    if(w <= 0 || h <= 0 || !c->fb) return;
    if(type != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA && format != GL_BGR_EXT && format != GL_BGRA_EXT))
    {
        stub("glReadPixels(format)");
        return;
    }
    LOCK;
    BYTE *tmp = VirtualAlloc(0, w * h * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int top_row = (int)c->fbh - y - h;
    dma2(c->fb, 0, tmp, w * 4, XFER_FROM_HOST, x, top_row < 0 ? 0 : top_row, w, h, 0, 0);
    gl_flush();
    UNLOCK;
    UINT comps = format == GL_RGB || format == GL_BGR_EXT ? 3 : 4;
    UINT pitch = (w * comps + c->pack_align - 1) & ~(c->pack_align - 1);
    for(GLsizei j = 0; j < h; j++)
    {
        const BYTE *s = tmp + (h - 1 - j) * w * 4;                /* bottom row first */
        BYTE *o = (BYTE *)pixels + j * pitch;
        for(GLsizei i = 0; i < w; i++, s += 4, o += comps)
        {
            int bgr = format == GL_BGR_EXT || format == GL_BGRA_EXT;
            o[0] = bgr ? s[0] : s[2]; o[1] = s[1]; o[2] = bgr ? s[2] : s[0];
            if(comps == 4) o[3] = s[3];
        }
    }
    VirtualFree(tmp, 0, MEM_RELEASE);
}

/* display lists: compiled lists are dropped (nothing we run uses them yet) */
GLuint APIENTRY glGenLists(GLsizei range) { GLuint b = next_list; next_list += range; return b; }
void APIENTRY glNewList(GLuint list, GLenum mode) { C; stub("glNewList"); c->list_compiling = mode == GL_COMPILE; }
void APIENTRY glEndList(void) { C; c->list_compiling = 0; }
void APIENTRY glCallList(GLuint list) {}
void APIENTRY glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {}
void APIENTRY glDeleteLists(GLuint list, GLsizei range) {}
GLboolean APIENTRY glIsList(GLuint list) { return 0; }
void APIENTRY glListBase(GLuint base) {}

static BOOL WINAPI wglSwapIntervalEXT_(int i) { return TRUE; }
static int WINAPI wglGetSwapIntervalEXT_(void) { return 0; }

/* ---------- wglGetProcAddress ---------- */

static const struct { const char *name; PROC fn; } procs[] = {
#define P(n, f) { n, (PROC)f }
    P("glActiveTexture", glActiveTexture_), P("glActiveTextureARB", glActiveTexture_),
    P("glClientActiveTexture", glClientActiveTexture_), P("glClientActiveTextureARB", glClientActiveTexture_),
    P("glMultiTexCoord2f", glMultiTexCoord2f_), P("glMultiTexCoord2fARB", glMultiTexCoord2f_),
    P("glMultiTexCoord2fv", glMultiTexCoord2fv_), P("glMultiTexCoord2fvARB", glMultiTexCoord2fv_),
    P("glMultiTexCoord3f", glMultiTexCoord3f_), P("glMultiTexCoord3fARB", glMultiTexCoord3f_),
    P("glMultiTexCoord4f", glMultiTexCoord4f_), P("glMultiTexCoord4fARB", glMultiTexCoord4f_),
    P("glLockArraysEXT", glLockArraysEXT_), P("glUnlockArraysEXT", glUnlockArraysEXT_),
    P("glDrawRangeElements", glDrawRangeElements_), P("glDrawRangeElementsEXT", glDrawRangeElements_),
    P("glBlendFuncSeparate", glBlendFuncSeparate_), P("glBlendFuncSeparateEXT", glBlendFuncSeparate_),
    P("glBlendEquation", glBlendEquation_), P("glBlendEquationEXT", glBlendEquation_),
    P("glBlendColor", glBlendColor_), P("glBlendColorEXT", glBlendColor_),
    P("glCompressedTexImage2D", glCompressedTexImage2D_), P("glCompressedTexImage2DARB", glCompressedTexImage2D_),
    P("glGenBuffers", glGenBuffers_), P("glGenBuffersARB", glGenBuffers_),
    P("glDeleteBuffers", glDeleteBuffers_), P("glDeleteBuffersARB", glDeleteBuffers_),
    P("glBindBuffer", glBindBuffer_), P("glBindBufferARB", glBindBuffer_),
    P("glBufferData", glBufferData_), P("glBufferDataARB", glBufferData_),
    P("glBufferSubData", glBufferSubData_), P("glBufferSubDataARB", glBufferSubData_),
    P("glMapBuffer", glMapBuffer_), P("glMapBufferARB", glMapBuffer_),
    P("glUnmapBuffer", glUnmapBuffer_), P("glUnmapBufferARB", glUnmapBuffer_),
    P("glGetBufferParameteriv", glGetBufferParameteriv_), P("glGetBufferParameterivARB", glGetBufferParameteriv_),
    P("glIsBuffer", glIsBuffer_), P("glIsBufferARB", glIsBuffer_),
    P("wglSwapIntervalEXT", wglSwapIntervalEXT_), P("wglGetSwapIntervalEXT", wglGetSwapIntervalEXT_),
#undef P
};
__declspec(dllexport) PROC WINAPI wglGetProcAddress(LPCSTR name)
{
    for(UINT i = 0; i < sizeof procs / sizeof *procs; i++) if(!strcmp(procs[i].name, name)) return procs[i].fn;
    if(getenv("GLFWD_TRACE")) hlog("glfwd: wglGetProcAddress(%s)", name);
    /* Core 1.1 entry points are exports of this DLL. */
    PROC p = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
    if(!p) hlog("glfwd: wglGetProcAddress(%s) not found", name);
    return p;
}

#include "glstubs.inc"
