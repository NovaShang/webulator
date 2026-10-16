/* d3d9fwd: a user-mode Direct3D 9 runtime that forwards to the emulator's GPU.
 *
 * Every call becomes SVGA3D commands (the protocol of the emulated VMware SVGA II device) in a command buffer in
 * this process; a hypercall (`out 0x5680, eax`) hands the buffer to the emulator, which renders with WebGL2. Data
 * (vertices, indices, texels) is not copied: commands point at it by linear address and the emulator reads it
 * through the guest page tables while the hypercall runs. So every command that references application or shadow
 * memory is followed by a flush before that memory can change.
 *
 * Drop d3d9.dll next to an application's executable. Built with mingw-w64 (see Makefile). */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "hc.h"

/* ---------- formats ---------- */

static DWORD svga_format(D3DFORMAT f)
{
    switch(f)
    {
        case D3DFMT_X8R8G8B8: return SF_X8R8G8B8;
        case D3DFMT_A8R8G8B8: return SF_A8R8G8B8;
        case D3DFMT_R5G6B5: return SF_R5G6B5;
        case D3DFMT_X1R5G5B5: return SF_X1R5G5B5;
        case D3DFMT_A1R5G5B5: return SF_A1R5G5B5;
        case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: return SF_A4R4G4B4;
        case D3DFMT_L8: return SF_LUMINANCE8;
        case D3DFMT_A4L4: return SF_LUMINANCE4_ALPHA4;
        case D3DFMT_L16: return SF_LUMINANCE16;
        case D3DFMT_A8L8: return SF_LUMINANCE8_ALPHA8;
        case D3DFMT_A8: return SF_ALPHA8;
        case D3DFMT_DXT1: return SF_DXT1;
        case D3DFMT_DXT2: return SF_DXT2;
        case D3DFMT_DXT3: return SF_DXT3;
        case D3DFMT_DXT4: return SF_DXT4;
        case D3DFMT_DXT5: return SF_DXT5;
        case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: return SF_Z_D16;
        case D3DFMT_D32: return SF_Z_D32;
        case D3DFMT_D24S8: case D3DFMT_D24X4S4: return SF_Z_D24S8;
        case D3DFMT_D24X8: return SF_Z_D24X8;
        case D3DFMT_D15S1: return SF_Z_D15S1;
        default: return 0;
    }
}
static int is_depth(D3DFORMAT f)
{
    return f == D3DFMT_D16 || f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D32 || f == D3DFMT_D24S8 || f == D3DFMT_D24X4S4 ||
        f == D3DFMT_D24X8 || f == D3DFMT_D15S1;
}
static int dxt_block(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 ? 8 : f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ? 16 : 0;
}
static int bytes_pp(D3DFORMAT f)
{
    switch(f)
    {
        case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_A4L4: return 1;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: return 2;
        default: return 4;
    }
}
/* Pitch and size of one mip level. */
static void level_layout(D3DFORMAT f, UINT w, UINT h, UINT *pitch, UINT *size)
{
    int b = dxt_block(f);
    if(b) { UINT bw = (w + 3) / 4, bh = (h + 3) / 4; *pitch = (bw ? bw : 1) * b; *size = *pitch * (bh ? bh : 1); }
    else { *pitch = (w * bytes_pp(f) + 3) & ~3u; *size = *pitch * h; }
}

/* ---------- objects ---------- */

typedef struct Device Device;

/* Every object starts with this. */
typedef struct { void *vtbl; LONG ref; Device *dev; } Obj;

typedef struct {
    Obj o;
    D3DRESOURCETYPE type;                /* D3DRTYPE_VERTEXBUFFER or D3DRTYPE_INDEXBUFFER */
    DWORD sid, size, usage, fvf;
    D3DFORMAT format;
    D3DPOOL pool;
    BYTE *mem;
    UINT lock_lo, lock_hi;
    int locked, write_locked;
} Buffer;

typedef struct Texture Texture;
typedef struct {
    Obj o;
    Texture *tex;                        /* container (levels of a texture) or NULL */
    UINT level;
    DWORD sid, usage;
    UINT w, h, pitch, size;
    D3DFORMAT format;
    D3DPOOL pool;
    BYTE *mem;
    int locked;
    RECT lock_rect;
} Surface;

struct Texture {
    Obj o;
    DWORD sid, usage;
    UINT w, h, levels;
    D3DFORMAT format;
    D3DPOOL pool;
    BYTE *mem[16];
    UINT pitch[16], size[16];
    Surface *surf[16];
    DWORD lod;
};

typedef struct {
    Obj o;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n;
    DWORD fvf;
} Decl;

typedef struct {
    Obj o;
    DWORD shid, type, ntok;
    DWORD *tok;
} Shader;

typedef struct { Obj o; D3DQUERYTYPE type; } Query;

typedef struct { Obj o; } SwapChain;

/* State blocks record (kind, index, value) entries. */
enum { SB_RS, SB_TSS, SB_SAMP, SB_TRANSFORM, SB_LIGHT, SB_LIGHTENABLE, SB_MATERIAL, SB_TEXTURE, SB_STREAM,
    SB_INDICES, SB_FVF, SB_DECL, SB_VS, SB_PS, SB_VSCONST, SB_PSCONST, SB_VIEWPORT, SB_SCISSOR };
typedef struct { DWORD kind, a, b; } SBEntry;
typedef struct { Obj o; SBEntry *e; UINT n, cap; } StateBlock;

#define MAX_STREAMS 16
#define MAX_STAGES 8
#define MAX_SAMPLERS 16
#define MAX_LIGHTS 64

typedef struct {
    DWORD rs[256];
    DWORD tss[MAX_STAGES][33];
    DWORD samp[MAX_SAMPLERS][14];
    D3DMATRIX transform[512];            /* indexed by D3DTRANSFORMSTATETYPE (world = 256) */
    D3DLIGHT9 light[MAX_LIGHTS];
    BOOL light_enable[MAX_LIGHTS], light_set[MAX_LIGHTS];
    D3DMATERIAL9 material;
    IDirect3DBaseTexture9 *texture[MAX_SAMPLERS];
    Buffer *stream[MAX_STREAMS];
    UINT stream_offset[MAX_STREAMS], stream_stride[MAX_STREAMS];
    Buffer *indices;
    DWORD fvf;
    Decl *decl;
    Shader *vs, *ps;
    float vsc[256][4], psc[224][4];
    D3DVIEWPORT9 viewport;
    RECT scissor;
} State;

struct Device {
    Obj o;
    IDirect3D9 *d3d;
    HWND focus, wnd;
    DWORD behavior;
    D3DPRESENT_PARAMETERS pp;
    DWORD cid;
    Surface *backbuffer, *auto_depth, *rt0, *ds;
    SwapChain swapchain;
    State s;
    StateBlock *recording;
    Decl *fvf_decl;                      /* cached declaration for s.fvf */
    DWORD fvf_decl_fvf;
    DWORD up_vb, up_vb_size, up_ib, up_ib_size;
    BOOL in_scene;
    UINT frames;
};

static ULONG obj_addref(Obj *o) { return InterlockedIncrement(&o->ref); }
static ULONG WINAPI com_AddRef(Obj *o) { return InterlockedIncrement(&o->ref); }

/* ---------- generic IUnknown / IDirect3DResource9 pieces ---------- */

static HRESULT WINAPI obj_QueryInterface(Obj *o, REFIID riid, void **out)
{
    *out = o; obj_addref(o); return S_OK;
}
static HRESULT WINAPI res_GetDevice(Obj *o, IDirect3DDevice9 **dev) { *dev = (IDirect3DDevice9 *)o->dev; obj_addref(&o->dev->o); return D3D_OK; }
static HRESULT WINAPI res_SetPrivateData(Obj *o, REFGUID g, const void *d, DWORD n, DWORD f) { return D3D_OK; }
static HRESULT WINAPI res_GetPrivateData(Obj *o, REFGUID g, void *d, DWORD *n) { return D3DERR_NOTFOUND; }
static HRESULT WINAPI res_FreePrivateData(Obj *o, REFGUID g) { return D3D_OK; }
static DWORD WINAPI res_SetPriority(Obj *o, DWORD p) { return 0; }
static DWORD WINAPI res_GetPriority(Obj *o) { return 0; }
static void WINAPI res_PreLoad(Obj *o) {}

#define F(x) ((void *)(x))

/* ---------- vertex and index buffers ---------- */

static ULONG WINAPI buf_Release(Buffer *b)
{
    ULONG r = InterlockedDecrement(&b->o.ref);
    if(!r)
    {
        EnterCriticalSection(&lock);
        surface_destroy(b->sid);
        LeaveCriticalSection(&lock);
        VirtualFree(b->mem, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, b);
    }
    return r;
}
static D3DRESOURCETYPE WINAPI buf_GetType(Buffer *b) { return b->type; }
static HRESULT WINAPI buf_Lock(Buffer *b, UINT offset, UINT size, void **data, DWORD flags)
{
    if(offset > b->size) offset = b->size;
    if(!size || offset + size > b->size) size = b->size - offset;
    *data = b->mem + offset;
    if(!(flags & D3DLOCK_READONLY))
    {
        if(!b->write_locked) { b->lock_lo = offset; b->lock_hi = offset + size; }
        else { if(offset < b->lock_lo) b->lock_lo = offset; if(offset + size > b->lock_hi) b->lock_hi = offset + size; }
        b->write_locked = 1;
    }
    b->locked++;
    return D3D_OK;
}
static HRESULT WINAPI buf_Unlock(Buffer *b)
{
    if(b->locked) b->locked--;
    if(!b->locked && b->write_locked)
    {
        EnterCriticalSection(&lock);
        dma(b->sid, 0, b->mem, 0, XFER_TO_HOST, b->lock_lo, 0, b->lock_hi - b->lock_lo, 1);
        flush();
        LeaveCriticalSection(&lock);
        b->write_locked = 0;
    }
    return D3D_OK;
}
static HRESULT WINAPI vb_GetDesc(Buffer *b, D3DVERTEXBUFFER_DESC *d)
{
    d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = b->usage; d->Pool = b->pool;
    d->Size = b->size; d->FVF = b->fvf; return D3D_OK;
}
static HRESULT WINAPI ib_GetDesc(Buffer *b, D3DINDEXBUFFER_DESC *d)
{
    d->Format = b->format; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->size;
    return D3D_OK;
}
static IDirect3DVertexBuffer9Vtbl vb_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(buf_Release), F(res_GetDevice), F(res_SetPrivateData),
    F(res_GetPrivateData), F(res_FreePrivateData), F(res_SetPriority), F(res_GetPriority), F(res_PreLoad),
    F(buf_GetType), F(buf_Lock), F(buf_Unlock), F(vb_GetDesc),
};
static IDirect3DIndexBuffer9Vtbl ib_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(buf_Release), F(res_GetDevice), F(res_SetPrivateData),
    F(res_GetPrivateData), F(res_FreePrivateData), F(res_SetPriority), F(res_GetPriority), F(res_PreLoad),
    F(buf_GetType), F(buf_Lock), F(buf_Unlock), F(ib_GetDesc),
};

static Buffer *buffer_new(Device *d, D3DRESOURCETYPE type, UINT size, DWORD usage, DWORD fvf, D3DFORMAT format, D3DPOOL pool)
{
    Buffer *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *b);
    b->o.vtbl = type == D3DRTYPE_VERTEXBUFFER ? (void *)&vb_vtbl : (void *)&ib_vtbl;
    b->o.ref = 1; b->o.dev = d; b->type = type; b->size = size; b->usage = usage; b->fvf = fvf; b->format = format;
    b->pool = pool;
    b->mem = VirtualAlloc(0, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    b->sid = new_id();
    EnterCriticalSection(&lock);
    DWORD *p = cmd(CMD_SURFACE_DEFINE, (9 + 3) * 4);
    p[0] = b->sid; p[1] = 0; p[2] = SF_BUFFER; p[3] = 1; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
    p[9] = size; p[10] = 1; p[11] = 1;
    LeaveCriticalSection(&lock);
    return b;
}

/* ---------- surfaces and textures ---------- */

static ULONG WINAPI tex_AddRef(Texture *t);
static ULONG WINAPI tex_Release(Texture *t);

static ULONG WINAPI surf_AddRef(Surface *s) { return s->tex ? tex_AddRef(s->tex) : obj_addref(&s->o); }
static ULONG WINAPI surf_Release(Surface *s)
{
    if(s->tex) return tex_Release(s->tex);
    ULONG r = InterlockedDecrement(&s->o.ref);
    if(!r)
    {
        EnterCriticalSection(&lock);
        if(s->sid) surface_destroy(s->sid);
        LeaveCriticalSection(&lock);
        if(s->mem) VirtualFree(s->mem, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return r;
}
static D3DRESOURCETYPE WINAPI surf_GetType(Surface *s) { return D3DRTYPE_SURFACE; }
static HRESULT WINAPI surf_GetContainer(Surface *s, REFIID riid, void **out)
{
    if(s->tex) { *out = s->tex; tex_AddRef(s->tex); return D3D_OK; }
    *out = s->o.dev; obj_addref(&s->o.dev->o); return D3D_OK;
}
static HRESULT WINAPI surf_GetDesc(Surface *s, D3DSURFACE_DESC *d)
{
    d->Format = s->format; d->Type = D3DRTYPE_SURFACE; d->Usage = s->usage; d->Pool = s->pool;
    d->MultiSampleType = D3DMULTISAMPLE_NONE; d->MultiSampleQuality = 0; d->Width = s->w; d->Height = s->h;
    return D3D_OK;
}
static HRESULT WINAPI surf_LockRect(Surface *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    if(!s->mem)
    {
        /* Render targets and default-pool surfaces: read back on demand. */
        s->mem = VirtualAlloc(0, s->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if(!s->mem) return D3DERR_INVALIDCALL;
    }
    if(s->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL) && !(flags & D3DLOCK_DISCARD) && !dxt_block(s->format))
    {
        EnterCriticalSection(&lock);
        dma(s->sid, s->level, s->mem, s->pitch, XFER_FROM_HOST, 0, 0, s->w, s->h);
        flush();
        LeaveCriticalSection(&lock);
    }
    RECT full = { 0, 0, (LONG)s->w, (LONG)s->h };
    s->lock_rect = r ? *r : full;
    int b = dxt_block(s->format);
    UINT x = s->lock_rect.left, y = s->lock_rect.top;
    lr->Pitch = s->pitch;
    lr->pBits = s->mem + (b ? (y / 4) * s->pitch + (x / 4) * b : y * s->pitch + x * bytes_pp(s->format));
    s->locked = (flags & D3DLOCK_READONLY) ? 1 : 2;
    return D3D_OK;
}
static void surf_upload(Surface *s, const RECT *r)
{
    int b = dxt_block(s->format);
    UINT x = r->left, y = r->top, w = r->right - r->left, h = r->bottom - r->top;
    if(b) { x &= ~3u; y &= ~3u; w = ((r->right + 3) & ~3) - x; h = ((r->bottom + 3) & ~3) - y; }
    if(x + w > s->w) w = s->w - x;
    if(y + h > s->h) h = s->h - y;
    EnterCriticalSection(&lock);
    dma(s->sid, s->level, s->mem, s->pitch, XFER_TO_HOST, x, y, w, h);
    flush();
    LeaveCriticalSection(&lock);
}
static HRESULT WINAPI surf_UnlockRect(Surface *s)
{
    if(s->locked == 2) surf_upload(s, &s->lock_rect);
    s->locked = 0;
    return D3D_OK;
}
static HRESULT WINAPI surf_GetDC(Surface *s, HDC *dc) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI surf_ReleaseDC(Surface *s, HDC dc) { return D3DERR_INVALIDCALL; }
static IDirect3DSurface9Vtbl surf_vtbl = {
    F(obj_QueryInterface), F(surf_AddRef), F(surf_Release), F(res_GetDevice), F(res_SetPrivateData),
    F(res_GetPrivateData), F(res_FreePrivateData), F(res_SetPriority), F(res_GetPriority), F(res_PreLoad),
    F(surf_GetType), F(surf_GetContainer), F(surf_GetDesc), F(surf_LockRect), F(surf_UnlockRect), F(surf_GetDC),
    F(surf_ReleaseDC),
};

static Surface *surface_new(Device *d, UINT w, UINT h, D3DFORMAT fmt, DWORD usage, D3DPOOL pool, int define)
{
    Surface *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    s->o.vtbl = &surf_vtbl; s->o.ref = 1; s->o.dev = d;
    s->w = w; s->h = h; s->format = fmt; s->usage = usage; s->pool = pool;
    level_layout(fmt, w, h, &s->pitch, &s->size);
    if(pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
        s->mem = VirtualAlloc(0, s->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(define)
    {
        s->sid = new_id();
        EnterCriticalSection(&lock);
        surface_define(s->sid, svga_format(fmt), w, h, 1);
        LeaveCriticalSection(&lock);
    }
    return s;
}

static ULONG WINAPI tex_AddRef(Texture *t) { return obj_addref(&t->o); }
static ULONG WINAPI tex_Release(Texture *t)
{
    ULONG r = InterlockedDecrement(&t->o.ref);
    if(!r)
    {
        EnterCriticalSection(&lock);
        surface_destroy(t->sid);
        LeaveCriticalSection(&lock);
        for(UINT i = 0; i < t->levels; i++)
        {
            if(t->mem[i]) VirtualFree(t->mem[i], 0, MEM_RELEASE);
            if(t->surf[i]) HeapFree(GetProcessHeap(), 0, t->surf[i]);
        }
        HeapFree(GetProcessHeap(), 0, t);
    }
    return r;
}
static D3DRESOURCETYPE WINAPI tex_GetType(Texture *t) { return D3DRTYPE_TEXTURE; }
static DWORD WINAPI tex_SetLOD(Texture *t, DWORD lod) { DWORD o = t->lod; t->lod = lod; return o; }
static DWORD WINAPI tex_GetLOD(Texture *t) { return t->lod; }
static DWORD WINAPI tex_GetLevelCount(Texture *t) { return t->levels; }
static HRESULT WINAPI tex_SetAutoGenFilterType(Texture *t, D3DTEXTUREFILTERTYPE f) { return D3D_OK; }
static D3DTEXTUREFILTERTYPE WINAPI tex_GetAutoGenFilterType(Texture *t) { return D3DTEXF_LINEAR; }
static void WINAPI tex_GenerateMipSubLevels(Texture *t)
{
    EnterCriticalSection(&lock);
    DWORD *p = cmd(CMD_GENERATE_MIPMAPS, 8); p[0] = t->sid; p[1] = 2;
    LeaveCriticalSection(&lock);
}
static Surface *tex_level(Texture *t, UINT level)
{
    if(level >= t->levels) return NULL;
    if(!t->surf[level])
    {
        Surface *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
        s->o.vtbl = &surf_vtbl; s->o.ref = 1; s->o.dev = t->o.dev; s->tex = t; s->level = level; s->sid = t->sid;
        s->usage = t->usage; s->format = t->format; s->pool = t->pool;
        s->w = t->w >> level; s->h = t->h >> level; if(!s->w) s->w = 1; if(!s->h) s->h = 1;
        s->pitch = t->pitch[level]; s->size = t->size[level]; s->mem = t->mem[level];
        t->surf[level] = s;
    }
    return t->surf[level];
}
static HRESULT WINAPI tex_GetLevelDesc(Texture *t, UINT level, D3DSURFACE_DESC *d)
{
    Surface *s = tex_level(t, level);
    return s ? surf_GetDesc(s, d) : D3DERR_INVALIDCALL;
}
static HRESULT WINAPI tex_GetSurfaceLevel(Texture *t, UINT level, IDirect3DSurface9 **out)
{
    Surface *s = tex_level(t, level);
    if(!s) return D3DERR_INVALIDCALL;
    tex_AddRef(t); *out = (IDirect3DSurface9 *)s; return D3D_OK;
}
static HRESULT WINAPI tex_LockRect(Texture *t, UINT level, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    Surface *s = tex_level(t, level);
    if(!s) return D3DERR_INVALIDCALL;
    HRESULT hr = surf_LockRect(s, lr, r, flags);
    t->mem[level] = s->mem;
    return hr;
}
static HRESULT WINAPI tex_UnlockRect(Texture *t, UINT level)
{
    Surface *s = tex_level(t, level);
    if(!s) return D3DERR_INVALIDCALL;
    HRESULT hr = surf_UnlockRect(s);
    if(level == 0 && t->usage & D3DUSAGE_AUTOGENMIPMAP) tex_GenerateMipSubLevels(t);
    return hr;
}
static HRESULT WINAPI tex_AddDirtyRect(Texture *t, const RECT *r) { return D3D_OK; }
static IDirect3DTexture9Vtbl tex_vtbl = {
    F(obj_QueryInterface), F(tex_AddRef), F(tex_Release), F(res_GetDevice), F(res_SetPrivateData),
    F(res_GetPrivateData), F(res_FreePrivateData), F(res_SetPriority), F(res_GetPriority), F(res_PreLoad),
    F(tex_GetType), F(tex_SetLOD), F(tex_GetLOD), F(tex_GetLevelCount), F(tex_SetAutoGenFilterType),
    F(tex_GetAutoGenFilterType), F(tex_GenerateMipSubLevels), F(tex_GetLevelDesc), F(tex_GetSurfaceLevel),
    F(tex_LockRect), F(tex_UnlockRect), F(tex_AddDirtyRect),
};

/* ---------- vertex declarations, shaders, queries, state blocks ---------- */

static ULONG WINAPI free_Release(Obj *o)
{
    ULONG r = InterlockedDecrement(&o->ref);
    if(!r) HeapFree(GetProcessHeap(), 0, o);
    return r;
}
static HRESULT WINAPI decl_GetDeclaration(Decl *d, D3DVERTEXELEMENT9 *el, UINT *n)
{
    if(el) memcpy(el, d->el, (d->n + 1) * sizeof *el);
    *n = d->n + 1; return D3D_OK;
}
static IDirect3DVertexDeclaration9Vtbl decl_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(free_Release), F(res_GetDevice), F(decl_GetDeclaration),
};

static ULONG WINAPI shader_Release(Shader *s)
{
    ULONG r = InterlockedDecrement(&s->o.ref);
    if(!r)
    {
        EnterCriticalSection(&lock);
        DWORD *p = cmd(CMD_SHADER_DESTROY, 12); p[0] = s->o.dev->cid; p[1] = s->shid; p[2] = s->type;
        LeaveCriticalSection(&lock);
        HeapFree(GetProcessHeap(), 0, s->tok);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return r;
}
static HRESULT WINAPI shader_GetFunction(Shader *s, void *data, UINT *size)
{
    if(data) memcpy(data, s->tok, s->ntok * 4);
    *size = s->ntok * 4; return D3D_OK;
}
static IDirect3DVertexShader9Vtbl vs_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(shader_Release), F(res_GetDevice), F(shader_GetFunction),
};
static IDirect3DPixelShader9Vtbl ps_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(shader_Release), F(res_GetDevice), F(shader_GetFunction),
};

static D3DQUERYTYPE WINAPI query_GetType(Query *q) { return q->type; }
static DWORD WINAPI query_GetDataSize(Query *q)
{
    return q->type == D3DQUERYTYPE_EVENT ? sizeof(BOOL) : q->type == D3DQUERYTYPE_OCCLUSION ? sizeof(DWORD) : 0;
}
static HRESULT WINAPI query_Issue(Query *q, DWORD flags) { return D3D_OK; }
static HRESULT WINAPI query_GetData(Query *q, void *data, DWORD size, DWORD flags)
{
    if(data && size >= 4) *(DWORD *)data = q->type == D3DQUERYTYPE_OCCLUSION ? 100000 : TRUE;
    return S_OK;
}
static IDirect3DQuery9Vtbl query_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(free_Release), F(res_GetDevice), F(query_GetType),
    F(query_GetDataSize), F(query_Issue), F(query_GetData),
};

static void sb_apply(StateBlock *sb);
static void sb_capture(StateBlock *sb);
static ULONG WINAPI sb_Release(StateBlock *sb)
{
    ULONG r = InterlockedDecrement(&sb->o.ref);
    if(!r) { HeapFree(GetProcessHeap(), 0, sb->e); HeapFree(GetProcessHeap(), 0, sb); }
    return r;
}
static HRESULT WINAPI sb_Capture(StateBlock *sb) { sb_capture(sb); return D3D_OK; }
static HRESULT WINAPI sb_Apply(StateBlock *sb) { sb_apply(sb); return D3D_OK; }
static IDirect3DStateBlock9Vtbl sb_vtbl = {
    F(obj_QueryInterface), F(com_AddRef), F(sb_Release), F(res_GetDevice), F(sb_Capture), F(sb_Apply),
};

/* ---------- device: state ---------- */

#define DEV(x) ((Device *)(x))

static DWORD map_blend_eq(DWORD v) { return v; }
static DWORD map_top(DWORD op)
{
    static const BYTE t[] = { 0, 1, 2, 3, 4, 12, 13, 5, 6, 17, 7, 18, 9, 8, 11, 16, 10, 19, 20, 21, 22, 23, 14, 24,
        15, 25, 26 };
    return op < sizeof t ? t[op] : 2;
}
static DWORD map_targ(DWORD a)
{
    static const BYTE src[] = { 3, 2, 4, 1, 5, 2, 6 };    /* diffuse, current, texture, tfactor, specular, temp, constant */
    DWORD s = a & 15, m = 0;
    if(a & D3DTA_COMPLEMENT) m |= 0x20;
    if(a & D3DTA_ALPHAREPLICATE) m |= 0x10;
    return (s < sizeof src ? src[s] : 2) | m;
}

static void send_rs(DWORD state, DWORD value)
{
    DWORD *p = cmd(CMD_SETRENDERSTATE, 12);
    p[0] = 0; p[1] = state; p[2] = value;
}

/* Send one D3D render state to the host (some map onto several SVGA3D states). */
static void apply_rs(Device *d, D3DRENDERSTATETYPE st)
{
    DWORD v = d->s.rs[st], *rs = d->s.rs;
#define RS(x) do { DWORD *p = cmd(CMD_SETRENDERSTATE, 12); p[0] = d->cid; p[1] = (x); p[2] = v; } while(0)
#define RSV(x, val) do { DWORD *p = cmd(CMD_SETRENDERSTATE, 12); p[0] = d->cid; p[1] = (x); p[2] = (val); } while(0)
    switch(st)
    {
        case D3DRS_ZENABLE: RSV(RS_ZENABLE, v ? 1 : 0); break;
        case D3DRS_FILLMODE: break;
        case D3DRS_SHADEMODE: RS(RS_SHADEMODE); break;
        case D3DRS_ZWRITEENABLE: RS(RS_ZWRITEENABLE); break;
        case D3DRS_ALPHATESTENABLE: RS(RS_ALPHATESTENABLE); break;
        case D3DRS_LASTPIXEL: RS(RS_LASTPIXEL); break;
        case D3DRS_SRCBLEND: case D3DRS_DESTBLEND:
        {
            DWORD s = rs[D3DRS_SRCBLEND], t = rs[D3DRS_DESTBLEND];
            if(s == D3DBLEND_BOTHSRCALPHA) { s = D3DBLEND_SRCALPHA; t = D3DBLEND_INVSRCALPHA; }
            else if(s == D3DBLEND_BOTHINVSRCALPHA) { s = D3DBLEND_INVSRCALPHA; t = D3DBLEND_SRCALPHA; }
            /* SVGA3dBlendOp has no BOTH* entries: the blend-factor pair is 12/13 there. */
            if(s >= D3DBLEND_BLENDFACTOR) s -= 2;
            if(t >= D3DBLEND_BLENDFACTOR) t -= 2;
            RSV(RS_SRCBLEND, s); RSV(RS_DSTBLEND, t);
            break;
        }
        case D3DRS_CULLMODE:
            RSV(RS_FRONTWINDING, 1);                     /* clockwise = front */
            RSV(RS_CULLMODE, v == D3DCULL_CW ? 2 : v == D3DCULL_CCW ? 3 : 1);
            break;
        case D3DRS_ZFUNC: RS(RS_ZFUNC); break;
        case D3DRS_ALPHAREF: RS(RS_ALPHAREF); break;
        case D3DRS_ALPHAFUNC: RS(RS_ALPHAFUNC); break;
        case D3DRS_DITHERENABLE: RS(RS_DITHERENABLE); break;
        case D3DRS_ALPHABLENDENABLE: RS(RS_BLENDENABLE); break;
        case D3DRS_FOGENABLE: RS(RS_FOGENABLE); break;
        case D3DRS_SPECULARENABLE: RS(RS_SPECULARENABLE); break;
        case D3DRS_FOGCOLOR: RS(RS_FOGCOLOR); break;
        case D3DRS_FOGTABLEMODE: case D3DRS_FOGVERTEXMODE: case D3DRS_RANGEFOGENABLE:
        {
            DWORD table = rs[D3DRS_FOGTABLEMODE], vert = rs[D3DRS_FOGVERTEXMODE];
            DWORD func = table ? table : vert ? vert : 4, type = table ? 2 : 1, base = rs[D3DRS_RANGEFOGENABLE] ? 2 : 1;
            RSV(RS_FOGMODE, func | type << 16 | base << 24);
            RSV(RS_RANGEFOGENABLE, rs[D3DRS_RANGEFOGENABLE]);
            break;
        }
        case D3DRS_FOGSTART: RS(RS_FOGSTART); break;
        case D3DRS_FOGEND: RS(RS_FOGEND); break;
        case D3DRS_FOGDENSITY: RS(RS_FOGDENSITY); break;
        case D3DRS_STENCILENABLE: RS(RS_STENCILENABLE); break;
        case D3DRS_STENCILFAIL: RS(RS_STENCILFAIL); break;
        case D3DRS_STENCILZFAIL: RS(RS_STENCILZFAIL); break;
        case D3DRS_STENCILPASS: RS(RS_STENCILPASS); break;
        case D3DRS_STENCILFUNC: RS(RS_STENCILFUNC); break;
        case D3DRS_STENCILREF: RS(RS_STENCILREF); break;
        case D3DRS_STENCILMASK: RS(RS_STENCILMASK); break;
        case D3DRS_STENCILWRITEMASK: RS(RS_STENCILWRITEMASK); break;
        case D3DRS_TEXTUREFACTOR: RS(RS_TEXTUREFACTOR); break;
        case D3DRS_CLIPPING: RS(RS_CLIPPING); break;
        case D3DRS_LIGHTING: RS(RS_LIGHTINGENABLE); break;
        case D3DRS_AMBIENT: RS(RS_AMBIENT); break;
        case D3DRS_COLORVERTEX: RS(RS_VERTEXMATERIALENABLE); break;
        case D3DRS_LOCALVIEWER: RS(RS_LOCALVIEWER); break;
        case D3DRS_NORMALIZENORMALS: RS(RS_NORMALIZENORMALS); break;
        case D3DRS_DIFFUSEMATERIALSOURCE: RS(RS_DIFFUSEMATERIALSOURCE); break;
        case D3DRS_SPECULARMATERIALSOURCE: RS(RS_SPECULARMATERIALSOURCE); break;
        case D3DRS_AMBIENTMATERIALSOURCE: RS(RS_AMBIENTMATERIALSOURCE); break;
        case D3DRS_EMISSIVEMATERIALSOURCE: RS(RS_EMISSIVEMATERIALSOURCE); break;
        case D3DRS_VERTEXBLEND: RS(RS_VERTEXBLEND); break;
        case D3DRS_CLIPPLANEENABLE: RS(RS_CLIPPLANEENABLE); break;
        case D3DRS_POINTSIZE: RS(RS_POINTSIZE); break;
        case D3DRS_POINTSIZE_MIN: RS(RS_POINTSIZEMIN); break;
        case D3DRS_POINTSIZE_MAX: RS(RS_POINTSIZEMAX); break;
        case D3DRS_POINTSPRITEENABLE: RS(RS_POINTSPRITEENABLE); break;
        case D3DRS_POINTSCALEENABLE: RS(RS_POINTSCALEENABLE); break;
        case D3DRS_POINTSCALE_A: RS(RS_POINTSCALE_A); break;
        case D3DRS_POINTSCALE_B: RS(RS_POINTSCALE_B); break;
        case D3DRS_POINTSCALE_C: RS(RS_POINTSCALE_C); break;
        case D3DRS_MULTISAMPLEANTIALIAS: RS(RS_MULTISAMPLEANTIALIAS); break;
        case D3DRS_MULTISAMPLEMASK: RS(RS_MULTISAMPLEMASK); break;
        case D3DRS_COLORWRITEENABLE: RS(RS_COLORWRITEENABLE); break;
        case D3DRS_BLENDOP: RSV(RS_BLENDEQUATION, map_blend_eq(v)); break;
        case D3DRS_SCISSORTESTENABLE: RS(RS_SCISSORTESTENABLE); break;
        case D3DRS_SLOPESCALEDEPTHBIAS: RS(RS_SLOPESCALEDEPTHBIAS); break;
        case D3DRS_ANTIALIASEDLINEENABLE: RS(RS_ANTIALIASEDLINEENABLE); break;
        case D3DRS_TWOSIDEDSTENCILMODE: RS(RS_STENCILENABLE2SIDED); break;
        case D3DRS_CCW_STENCILFAIL: RS(RS_CCWSTENCILFAIL); break;
        case D3DRS_CCW_STENCILZFAIL: RS(RS_CCWSTENCILZFAIL); break;
        case D3DRS_CCW_STENCILPASS: RS(RS_CCWSTENCILPASS); break;
        case D3DRS_CCW_STENCILFUNC: RS(RS_CCWSTENCILFUNC); break;
        case D3DRS_COLORWRITEENABLE1: RS(RS_COLORWRITEENABLE1); break;
        case D3DRS_COLORWRITEENABLE2: RS(RS_COLORWRITEENABLE2); break;
        case D3DRS_COLORWRITEENABLE3: RS(RS_COLORWRITEENABLE3); break;
        case D3DRS_BLENDFACTOR: RS(RS_BLENDCOLOR); break;
        case D3DRS_DEPTHBIAS: RS(RS_DEPTHBIAS); break;
        case D3DRS_SEPARATEALPHABLENDENABLE: RS(RS_SEPARATEALPHABLENDENABLE); break;
        case D3DRS_SRCBLENDALPHA: RSV(RS_SRCBLENDALPHA, v >= D3DBLEND_BLENDFACTOR ? v - 2 : v); break;
        case D3DRS_DESTBLENDALPHA: RSV(RS_DSTBLENDALPHA, v >= D3DBLEND_BLENDFACTOR ? v - 2 : v); break;
        case D3DRS_BLENDOPALPHA: RS(RS_BLENDEQUATIONALPHA); break;
        default: break;
    }
#undef RS
#undef RSV
}

static void send_ts(Device *d, DWORD stage, DWORD name, DWORD value)
{
    DWORD *p = cmd(CMD_SETTEXTURESTATE, 16);
    p[0] = d->cid; p[1] = stage; p[2] = name; p[3] = value;
}

static void apply_tss(Device *d, DWORD stage, D3DTEXTURESTAGESTATETYPE t)
{
    DWORD v = d->s.tss[stage][t];
    switch(t)
    {
        case D3DTSS_COLOROP: send_ts(d, stage, TS_COLOROP, map_top(v)); break;
        case D3DTSS_COLORARG1: send_ts(d, stage, TS_COLORARG1, map_targ(v)); break;
        case D3DTSS_COLORARG2: send_ts(d, stage, TS_COLORARG2, map_targ(v)); break;
        case D3DTSS_COLORARG0: send_ts(d, stage, TS_COLORARG0, map_targ(v)); break;
        case D3DTSS_ALPHAOP: send_ts(d, stage, TS_ALPHAOP, map_top(v)); break;
        case D3DTSS_ALPHAARG1: send_ts(d, stage, TS_ALPHAARG1, map_targ(v)); break;
        case D3DTSS_ALPHAARG2: send_ts(d, stage, TS_ALPHAARG2, map_targ(v)); break;
        case D3DTSS_ALPHAARG0: send_ts(d, stage, TS_ALPHAARG0, map_targ(v)); break;
        case D3DTSS_BUMPENVMAT00: case D3DTSS_BUMPENVMAT01: case D3DTSS_BUMPENVMAT10: case D3DTSS_BUMPENVMAT11:
            send_ts(d, stage, TS_BUMPENVMAT00 + (t - D3DTSS_BUMPENVMAT00), v); break;
        case D3DTSS_TEXCOORDINDEX:
        {
            static const BYTE gen[] = { 0, 2, 1, 3, 4 };
            DWORD g = v >> 16;
            send_ts(d, stage, TS_TEXCOORDINDEX, v & 0xFFFF);
            send_ts(d, stage, TS_TEXCOORDGEN, g < sizeof gen ? gen[g] : 0);
            break;
        }
        case D3DTSS_BUMPENVLSCALE: send_ts(d, stage, TS_BUMPENVLSCALE, v); break;
        case D3DTSS_BUMPENVLOFFSET: send_ts(d, stage, TS_BUMPENVLOFFSET, v); break;
        case D3DTSS_TEXTURETRANSFORMFLAGS:
        {
            DWORD n = v & 0xFF, bits = n >= 1 ? 1 : 0;
            if(n >= 2) bits |= 2;
            if(n >= 3) bits |= 4;
            if(n >= 4) bits |= 8;
            if(v & D3DTTFF_PROJECTED) bits |= 1 << 15;
            send_ts(d, stage, TS_TEXTURETRANSFORMFLAGS, bits);
            break;
        }
        default: break;
    }
}

static void apply_samp(Device *d, DWORD s, D3DSAMPLERSTATETYPE t)
{
    DWORD v = d->s.samp[s][t];
    if(s >= MAX_STAGES) return;
    switch(t)
    {
        case D3DSAMP_ADDRESSU: send_ts(d, s, TS_ADDRESSU, v); break;
        case D3DSAMP_ADDRESSV: send_ts(d, s, TS_ADDRESSV, v); break;
        case D3DSAMP_ADDRESSW: send_ts(d, s, TS_ADDRESSW, v); break;
        case D3DSAMP_BORDERCOLOR: send_ts(d, s, TS_BORDERCOLOR, v); break;
        case D3DSAMP_MAGFILTER: send_ts(d, s, TS_MAGFILTER, v); break;
        case D3DSAMP_MINFILTER: send_ts(d, s, TS_MINFILTER, v); break;
        case D3DSAMP_MIPFILTER: send_ts(d, s, TS_MIPFILTER, v); break;
        case D3DSAMP_MIPMAPLODBIAS: send_ts(d, s, TS_TEXTURE_LOD_BIAS, v); break;
        case D3DSAMP_MAXMIPLEVEL: send_ts(d, s, TS_TEXTURE_MIPMAP_LEVEL, v); break;
        case D3DSAMP_MAXANISOTROPY: send_ts(d, s, TS_TEXTURE_ANISOTROPIC_LEVEL, v); break;
        default: break;
    }
}

static DWORD tex_sid(IDirect3DBaseTexture9 *t)
{
    if(!t) return INVALID_ID;
    if(((Obj *)t)->vtbl == &tex_vtbl) return ((Texture *)t)->sid;
    return INVALID_ID;
}

static void apply_texture(Device *d, DWORD stage)
{
    if(stage < MAX_STAGES) send_ts(d, stage, TS_BIND_TEXTURE, tex_sid(d->s.texture[stage]));
}

static void apply_transform(Device *d, D3DTRANSFORMSTATETYPE t)
{
    DWORD type = t == D3DTS_WORLD ? 1 : t == D3DTS_VIEW ? 2 : t == D3DTS_PROJECTION ? 3 :
        t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7 ? 4 + (t - D3DTS_TEXTURE0) : 0;
    if(!type) return;
    DWORD *p = cmd(CMD_SETTRANSFORM, 18 * 4);
    p[0] = d->cid; p[1] = type; memcpy(p + 2, &d->s.transform[t], 64);
}

static void apply_light(Device *d, DWORD i)
{
    const D3DLIGHT9 *l = &d->s.light[i];
    DWORD *p = cmd(CMD_SETLIGHTDATA, 31 * 4);
    float *f = (float *)p;
    p[0] = d->cid; p[1] = i;
    p[2] = l->Type == D3DLIGHT_POINT ? 1 : l->Type == D3DLIGHT_SPOT ? 3 : 4;
    p[3] = 1;
    memcpy(f + 4, &l->Diffuse, 16); memcpy(f + 8, &l->Specular, 16); memcpy(f + 12, &l->Ambient, 16);
    f[16] = l->Position.x; f[17] = l->Position.y; f[18] = l->Position.z; f[19] = 1;
    f[20] = l->Direction.x; f[21] = l->Direction.y; f[22] = l->Direction.z; f[23] = 0;
    f[24] = l->Range; f[25] = l->Falloff; f[26] = l->Attenuation0; f[27] = l->Attenuation1; f[28] = l->Attenuation2;
    f[29] = l->Theta; f[30] = l->Phi;
}
static void apply_light_enable(Device *d, DWORD i)
{
    DWORD *p = cmd(CMD_SETLIGHTENABLED, 12);
    p[0] = d->cid; p[1] = i; p[2] = d->s.light_enable[i];
}
static void apply_material(Device *d)
{
    DWORD *p = cmd(CMD_SETMATERIAL, 19 * 4);
    p[0] = d->cid; p[1] = 4;                           /* front and back */
    memcpy(p + 2, &d->s.material, 17 * 4);
}
static void apply_viewport(Device *d)
{
    const D3DVIEWPORT9 *v = &d->s.viewport;
    DWORD *p = cmd(CMD_SETVIEWPORT, 20);
    p[0] = d->cid; p[1] = v->X; p[2] = v->Y; p[3] = v->Width; p[4] = v->Height;
    p = cmd(CMD_SETZRANGE, 12);
    p[0] = d->cid; p[1] = fbits(v->MinZ); p[2] = fbits(v->MaxZ);
}
static void apply_scissor(Device *d)
{
    const RECT *r = &d->s.scissor;
    DWORD *p = cmd(CMD_SETSCISSORRECT, 20);
    p[0] = d->cid; p[1] = r->left; p[2] = r->top; p[3] = r->right - r->left; p[4] = r->bottom - r->top;
}
static void apply_shader(Device *d, DWORD type)
{
    Shader *s = type == SHADER_VS ? d->s.vs : d->s.ps;
    DWORD *p = cmd(CMD_SET_SHADER, 12);
    p[0] = d->cid; p[1] = type; p[2] = s ? s->shid : INVALID_ID;
}
static void apply_const(Device *d, DWORD type, UINT reg, UINT count)
{
    float (*src)[4] = type == SHADER_VS ? d->s.vsc : d->s.psc;
    DWORD *p = cmd(CMD_SET_SHADER_CONST, 16 + count * 16);
    p[0] = d->cid; p[1] = reg; p[2] = type; p[3] = 0;
    memcpy(p + 4, src[reg], count * 16);
}

static void set_rendertarget(Device *d, DWORD type, Surface *s)
{
    DWORD *p = cmd(CMD_SETRENDERTARGET, 20);
    p[0] = d->cid; p[1] = type; p[2] = s ? s->sid : INVALID_ID; p[3] = 0; p[4] = s ? s->level : 0;
}

/* ---------- state blocks ---------- */

static void sb_add(StateBlock *sb, DWORD kind, DWORD a, DWORD b)
{
    for(UINT i = 0; i < sb->n; i++)
        if(sb->e[i].kind == kind && sb->e[i].a == a) { sb->e[i].b = b; return; }
    if(sb->n == sb->cap)
    {
        sb->cap = sb->cap ? sb->cap * 2 : 64;
        sb->e = sb->e ? HeapReAlloc(GetProcessHeap(), 0, sb->e, sb->cap * sizeof *sb->e) :
            HeapAlloc(GetProcessHeap(), 0, sb->cap * sizeof *sb->e);
    }
    sb->e[sb->n].kind = kind; sb->e[sb->n].a = a; sb->e[sb->n].b = b; sb->n++;
}
/* While recording, setters only note what they touched; values are captured at EndStateBlock. */
/* Recording applies the change as well (unlike Direct3D, which defers it); applications re-apply blocks anyway. */
#define RECORD(kind, a) do { if(d->recording) sb_add(d->recording, kind, a, 0); } while(0)

static HRESULT WINAPI dev_SetRenderState(Device *d, D3DRENDERSTATETYPE st, DWORD v);
static HRESULT WINAPI dev_SetTextureStageState(Device *d, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v);
static HRESULT WINAPI dev_SetSamplerState(Device *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v);
static HRESULT WINAPI dev_SetTransform(Device *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m);
static HRESULT WINAPI dev_SetLight(Device *d, DWORD i, const D3DLIGHT9 *l);
static HRESULT WINAPI dev_LightEnable(Device *d, DWORD i, BOOL on);
static HRESULT WINAPI dev_SetMaterial(Device *d, const D3DMATERIAL9 *m);
static HRESULT WINAPI dev_SetTexture(Device *d, DWORD stage, IDirect3DBaseTexture9 *t);
static HRESULT WINAPI dev_SetStreamSource(Device *d, UINT n, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride);
static HRESULT WINAPI dev_SetIndices(Device *d, IDirect3DIndexBuffer9 *ib);
static HRESULT WINAPI dev_SetFVF(Device *d, DWORD fvf);
static HRESULT WINAPI dev_SetVertexDeclaration(Device *d, IDirect3DVertexDeclaration9 *decl);
static HRESULT WINAPI dev_SetVertexShader(Device *d, IDirect3DVertexShader9 *s);
static HRESULT WINAPI dev_SetPixelShader(Device *d, IDirect3DPixelShader9 *s);
static HRESULT WINAPI dev_SetVertexShaderConstantF(Device *d, UINT reg, const float *f, UINT n);
static HRESULT WINAPI dev_SetPixelShaderConstantF(Device *d, UINT reg, const float *f, UINT n);
static HRESULT WINAPI dev_SetViewport(Device *d, const D3DVIEWPORT9 *v);
static HRESULT WINAPI dev_SetScissorRect(Device *d, const RECT *r);

/* Values live in a side table: for each entry, a copy of the state at capture time. */
typedef struct { D3DMATRIX m; D3DLIGHT9 l; D3DMATERIAL9 mat; float c[4]; D3DVIEWPORT9 vp; RECT r; } SBValue;

static void sb_capture(StateBlock *sb)
{
    Device *d = sb->o.dev;
    for(UINT i = 0; i < sb->n; i++)
    {
        SBEntry *e = &sb->e[i];
        DWORD a = e->a;
        switch(e->kind)
        {
            case SB_RS: e->b = d->s.rs[a]; break;
            case SB_TSS: e->b = d->s.tss[a >> 8][a & 255]; break;
            case SB_SAMP: e->b = d->s.samp[a >> 8][a & 255]; break;
            case SB_LIGHTENABLE: e->b = d->s.light_enable[a]; break;
            case SB_TEXTURE: e->b = (DWORD)d->s.texture[a]; break;
            case SB_STREAM: e->b = (DWORD)d->s.stream[a]; break;
            case SB_INDICES: e->b = (DWORD)d->s.indices; break;
            case SB_FVF: e->b = d->s.fvf; break;
            case SB_DECL: e->b = (DWORD)d->s.decl; break;
            case SB_VS: e->b = (DWORD)d->s.vs; break;
            case SB_PS: e->b = (DWORD)d->s.ps; break;
            default:
            {
                SBValue *v = (SBValue *)e->b;
                if(!v) { v = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *v); e->b = (DWORD)v; }
                if(e->kind == SB_TRANSFORM) v->m = d->s.transform[a];
                else if(e->kind == SB_LIGHT) v->l = d->s.light[a];
                else if(e->kind == SB_MATERIAL) v->mat = d->s.material;
                else if(e->kind == SB_VSCONST) memcpy(v->c, d->s.vsc[a], 16);
                else if(e->kind == SB_PSCONST) memcpy(v->c, d->s.psc[a], 16);
                else if(e->kind == SB_VIEWPORT) v->vp = d->s.viewport;
                else if(e->kind == SB_SCISSOR) v->r = d->s.scissor;
            }
        }
    }
}

static void sb_apply(StateBlock *sb)
{
    Device *d = sb->o.dev;
    UINT stride[MAX_STREAMS];
    for(UINT i = 0; i < MAX_STREAMS; i++) stride[i] = d->s.stream_stride[i];
    for(UINT i = 0; i < sb->n; i++)
    {
        SBEntry *e = &sb->e[i];
        DWORD a = e->a;
        SBValue *v = (SBValue *)e->b;
        switch(e->kind)
        {
            case SB_RS: dev_SetRenderState(d, a, e->b); break;
            case SB_TSS: dev_SetTextureStageState(d, a >> 8, a & 255, e->b); break;
            case SB_SAMP: dev_SetSamplerState(d, a >> 8, a & 255, e->b); break;
            case SB_LIGHTENABLE: dev_LightEnable(d, a, e->b); break;
            case SB_TEXTURE: dev_SetTexture(d, a, (IDirect3DBaseTexture9 *)e->b); break;
            case SB_STREAM: dev_SetStreamSource(d, a, (IDirect3DVertexBuffer9 *)e->b, d->s.stream_offset[a], stride[a]); break;
            case SB_INDICES: dev_SetIndices(d, (IDirect3DIndexBuffer9 *)e->b); break;
            case SB_FVF: dev_SetFVF(d, e->b); break;
            case SB_DECL: dev_SetVertexDeclaration(d, (IDirect3DVertexDeclaration9 *)e->b); break;
            case SB_VS: dev_SetVertexShader(d, (IDirect3DVertexShader9 *)e->b); break;
            case SB_PS: dev_SetPixelShader(d, (IDirect3DPixelShader9 *)e->b); break;
            case SB_TRANSFORM: if(v) dev_SetTransform(d, a, &v->m); break;
            case SB_LIGHT: if(v) dev_SetLight(d, a, &v->l); break;
            case SB_MATERIAL: if(v) dev_SetMaterial(d, &v->mat); break;
            case SB_VSCONST: if(v) dev_SetVertexShaderConstantF(d, a, v->c, 1); break;
            case SB_PSCONST: if(v) dev_SetPixelShaderConstantF(d, a, v->c, 1); break;
            case SB_VIEWPORT: if(v) dev_SetViewport(d, &v->vp); break;
            case SB_SCISSOR: if(v) dev_SetScissorRect(d, &v->r); break;
        }
    }
}

static StateBlock *sb_new(Device *d)
{
    StateBlock *sb = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *sb);
    sb->o.vtbl = &sb_vtbl; sb->o.ref = 1; sb->o.dev = d;
    return sb;
}

/* Everything a D3DSBT_ALL / PIXELSTATE / VERTEXSTATE block covers (approximately: all, or the listed groups). */
static void sb_fill(StateBlock *sb, D3DSTATEBLOCKTYPE type)
{
    int all = type == D3DSBT_ALL, pix = all || type == D3DSBT_PIXELSTATE, vert = all || type == D3DSBT_VERTEXSTATE;
    for(DWORD i = 1; i < 256; i++) sb_add(sb, SB_RS, i, 0);
    if(pix) for(DWORD s = 0; s < MAX_STAGES; s++) for(DWORD t = 1; t <= 32; t++) sb_add(sb, SB_TSS, s << 8 | t, 0);
    if(pix) for(DWORD s = 0; s < MAX_STAGES; s++) for(DWORD t = 1; t <= 13; t++) sb_add(sb, SB_SAMP, s << 8 | t, 0);
    if(vert) for(DWORD i = 0; i < MAX_LIGHTS; i++) if(sb->o.dev->s.light_set[i]) { sb_add(sb, SB_LIGHT, i, 0); sb_add(sb, SB_LIGHTENABLE, i, 0); }
    if(vert) { sb_add(sb, SB_FVF, 0, 0); sb_add(sb, SB_DECL, 0, 0); sb_add(sb, SB_VS, 0, 0); }
    if(pix) sb_add(sb, SB_PS, 0, 0);
    if(all)
    {
        sb_add(sb, SB_TRANSFORM, D3DTS_WORLD, 0); sb_add(sb, SB_TRANSFORM, D3DTS_VIEW, 0);
        sb_add(sb, SB_TRANSFORM, D3DTS_PROJECTION, 0);
        for(DWORD i = 0; i < 8; i++) sb_add(sb, SB_TRANSFORM, D3DTS_TEXTURE0 + i, 0);
        sb_add(sb, SB_MATERIAL, 0, 0); sb_add(sb, SB_VIEWPORT, 0, 0); sb_add(sb, SB_SCISSOR, 0, 0);
        for(DWORD i = 0; i < MAX_STAGES; i++) sb_add(sb, SB_TEXTURE, i, 0);
        for(DWORD i = 0; i < MAX_STREAMS; i++) sb_add(sb, SB_STREAM, i, 0);
        sb_add(sb, SB_INDICES, 0, 0);
    }
    sb_capture(sb);
}

/* ---------- device: IDirect3DDevice9 ---------- */

static HRESULT WINAPI dev_QueryInterface(Device *d, REFIID riid, void **out) { *out = d; obj_addref(&d->o); return S_OK; }
static ULONG WINAPI dev_AddRef(Device *d) { return obj_addref(&d->o); }
static ULONG WINAPI dev_Release(Device *d)
{
    ULONG r = InterlockedDecrement(&d->o.ref);
    if(!r)
    {
        EnterCriticalSection(&lock);
        *cmd(CMD_CONTEXT_DESTROY, 4) = d->cid;
        flush();
        LeaveCriticalSection(&lock);
    }
    return r;
}
static HRESULT WINAPI dev_TestCooperativeLevel(Device *d) { return D3D_OK; }
static UINT WINAPI dev_GetAvailableTextureMem(Device *d) { return 256 << 20; }
static HRESULT WINAPI dev_EvictManagedResources(Device *d) { return D3D_OK; }
static HRESULT WINAPI dev_GetDirect3D(Device *d, IDirect3D9 **out) { *out = d->d3d; IDirect3D9_AddRef(d->d3d); return D3D_OK; }
static void fill_caps(D3DCAPS9 *c);
static HRESULT WINAPI dev_GetDeviceCaps(Device *d, D3DCAPS9 *c) { fill_caps(c); return D3D_OK; }
static HRESULT WINAPI dev_GetDisplayMode(Device *d, UINT sc, D3DDISPLAYMODE *m)
{
    DEVMODEA dm = { .dmSize = sizeof dm };
    EnumDisplaySettingsA(0, ENUM_CURRENT_SETTINGS, &dm);
    m->Width = dm.dmPelsWidth; m->Height = dm.dmPelsHeight; m->RefreshRate = dm.dmDisplayFrequency;
    m->Format = dm.dmBitsPerPel == 16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetCreationParameters(Device *d, D3DDEVICE_CREATION_PARAMETERS *p)
{
    p->AdapterOrdinal = 0; p->DeviceType = D3DDEVTYPE_HAL; p->hFocusWindow = d->focus; p->BehaviorFlags = d->behavior;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetCursorProperties(Device *d, UINT x, UINT y, IDirect3DSurface9 *s) { return D3D_OK; }
static void WINAPI dev_SetCursorPosition(Device *d, int x, int y, DWORD f) {}
static BOOL WINAPI dev_ShowCursor(Device *d, BOOL show) { return TRUE; }
static HRESULT WINAPI dev_CreateAdditionalSwapChain(Device *d, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **sc) { return D3DERR_NOTAVAILABLE; }
static HRESULT WINAPI dev_GetSwapChain(Device *d, UINT i, IDirect3DSwapChain9 **sc)
{
    if(i) return D3DERR_INVALIDCALL;
    *sc = (IDirect3DSwapChain9 *)&d->swapchain; obj_addref(&d->o); return D3D_OK;
}
static UINT WINAPI dev_GetNumberOfSwapChains(Device *d) { return 1; }

static void create_targets(Device *d);
static void reset_state(Device *d);
static HRESULT WINAPI dev_Reset(Device *d, D3DPRESENT_PARAMETERS *pp)
{
    EnterCriticalSection(&lock);
    d->pp = *pp;
    create_targets(d);
    *pp = d->pp;
    reset_state(d);
    flush();
    LeaveCriticalSection(&lock);
    return D3D_OK;
}

static HRESULT WINAPI dev_Present(Device *d, const RECT *src, const RECT *dst, HWND override, const RGNDATA *dirty)
{
    HWND w = override ? override : d->pp.hDeviceWindow ? d->pp.hDeviceWindow : d->focus;
    RECT cr;
    POINT o = { 0, 0 };
    EnterCriticalSection(&lock);
    if(w && GetClientRect(w, &cr) && ClientToScreen(w, &o) && !IsIconic(w))
    {
        LONG sx = src ? src->left : 0, sy = src ? src->top : 0;
        LONG x = o.x + (dst ? dst->left : 0), y = o.y + (dst ? dst->top : 0);
        LONG cw = cr.right - (dst ? dst->left : 0), ch = cr.bottom - (dst ? dst->top : 0);
        LONG bw = (LONG)d->backbuffer->w - sx, bh = (LONG)d->backbuffer->h - sy;
        LONG pw = cw < bw ? cw : bw, ph = ch < bh ? ch : bh;
        if(x < 0) { sx -= x; pw += x; x = 0; }
        if(y < 0) { sy -= y; ph += y; y = 0; }
        if(pw > 0 && ph > 0)
        {
            DWORD *p = cmd(CMD_PRESENT, 28);
            p[0] = d->backbuffer->sid; p[1] = x; p[2] = y; p[3] = pw; p[4] = ph; p[5] = sx; p[6] = sy;
        }
    }
    flush();
    LeaveCriticalSection(&lock);
    d->frames++;
    return D3D_OK;
}

static HRESULT WINAPI dev_GetBackBuffer(Device *d, UINT sc, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **out)
{
    *out = (IDirect3DSurface9 *)d->backbuffer; surf_AddRef(d->backbuffer); return D3D_OK;
}
static HRESULT WINAPI dev_GetRasterStatus(Device *d, UINT sc, D3DRASTER_STATUS *r) { r->InVBlank = TRUE; r->ScanLine = 0; return D3D_OK; }
static HRESULT WINAPI dev_SetDialogBoxMode(Device *d, BOOL on) { return D3D_OK; }
static void WINAPI dev_SetGammaRamp(Device *d, UINT sc, DWORD f, const D3DGAMMARAMP *r) {}
static void WINAPI dev_GetGammaRamp(Device *d, UINT sc, D3DGAMMARAMP *r)
{
    for(int i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = i * 257;
}

static HRESULT WINAPI dev_CreateTexture(Device *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
    D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    DWORD sf = svga_format(fmt);
    if(!sf || !w || !h) return D3DERR_INVALIDCALL;
    UINT max = 1;
    while((w >> max) || (h >> max)) max++;
    if(!levels || levels > max) levels = max;
    if(usage & D3DUSAGE_AUTOGENMIPMAP) levels = max;
    if(levels > 16) levels = 16;
    Texture *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
    t->o.vtbl = &tex_vtbl; t->o.ref = 1; t->o.dev = d;
    t->w = w; t->h = h; t->levels = levels; t->usage = usage; t->format = fmt; t->pool = pool;
    for(UINT i = 0; i < levels; i++)
    {
        UINT lw = w >> i, lh = h >> i;
        level_layout(fmt, lw ? lw : 1, lh ? lh : 1, &t->pitch[i], &t->size[i]);
        if(!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
            t->mem[i] = VirtualAlloc(0, t->size[i], MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    t->sid = new_id();
    EnterCriticalSection(&lock);
    surface_define(t->sid, sf, w, h, levels);
    LeaveCriticalSection(&lock);
    for(UINT i = 0; i < levels; i++) tex_level(t, i);
    *out = (IDirect3DTexture9 *)t;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateVolumeTexture(Device *d, UINT w, UINT h, UINT dd, UINT l, DWORD u, D3DFORMAT f,
    D3DPOOL p, IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    hlog("CreateVolumeTexture: not supported");
    return D3DERR_NOTAVAILABLE;
}
static HRESULT WINAPI dev_CreateCubeTexture(Device *d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
    IDirect3DCubeTexture9 **out, HANDLE *sh)
{
    hlog("CreateCubeTexture: not supported");
    return D3DERR_NOTAVAILABLE;
}
static HRESULT WINAPI dev_CreateVertexBuffer(Device *d, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool,
    IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    *out = (IDirect3DVertexBuffer9 *)buffer_new(d, D3DRTYPE_VERTEXBUFFER, size, usage, fvf, D3DFMT_VERTEXDATA, pool);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateIndexBuffer(Device *d, UINT size, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
    IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    *out = (IDirect3DIndexBuffer9 *)buffer_new(d, D3DRTYPE_INDEXBUFFER, size, usage, 0, fmt, pool);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateRenderTarget(Device *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
    DWORD q, BOOL lockable, IDirect3DSurface9 **out, HANDLE *sh)
{
    if(!svga_format(fmt)) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface9 *)surface_new(d, w, h, fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, 1);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateDepthStencilSurface(Device *d, UINT w, UINT h, D3DFORMAT fmt,
    D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL discard, IDirect3DSurface9 **out, HANDLE *sh)
{
    if(!svga_format(fmt)) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface9 *)surface_new(d, w, h, fmt, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 1);
    return D3D_OK;
}
static HRESULT WINAPI dev_UpdateSurface(Device *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst,
    const POINT *dp)
{
    Surface *s = (Surface *)src, *t = (Surface *)dst;
    if(!s->mem || !t->sid) return D3DERR_INVALIDCALL;
    RECT r = { 0, 0, (LONG)s->w, (LONG)s->h };
    if(sr) r = *sr;
    LONG x = dp ? dp->x : r.left, y = dp ? dp->y : r.top;
    EnterCriticalSection(&lock);
    dma2(t->sid, t->level, s->mem, s->pitch, XFER_TO_HOST, x, y, r.right - r.left, r.bottom - r.top, r.left, r.top);
    flush();
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_UpdateTexture(Device *d, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst)
{
    Texture *s = (Texture *)src, *t = (Texture *)dst;
    if(s->o.vtbl != &tex_vtbl || t->o.vtbl != &tex_vtbl) return D3DERR_INVALIDCALL;
    for(UINT i = 0; i < s->levels && i < t->levels; i++)
        if(s->mem[i]) dev_UpdateSurface(d, (IDirect3DSurface9 *)tex_level(s, i), 0, (IDirect3DSurface9 *)tex_level(t, i), 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderTargetData(Device *d, IDirect3DSurface9 *rt, IDirect3DSurface9 *dst)
{
    Surface *s = (Surface *)rt, *t = (Surface *)dst;
    if(!t->mem) return D3DERR_INVALIDCALL;
    EnterCriticalSection(&lock);
    dma(s->sid, s->level, t->mem, t->pitch, XFER_FROM_HOST, 0, 0, s->w < t->w ? s->w : t->w, s->h < t->h ? s->h : t->h);
    flush();
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetFrontBufferData(Device *d, UINT sc, IDirect3DSurface9 *dst) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI dev_StretchRect(Device *d, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst,
    const RECT *dr, D3DTEXTUREFILTERTYPE filter)
{
    Surface *s = (Surface *)src, *t = (Surface *)dst;
    if(!s->sid || !t->sid) return D3DERR_INVALIDCALL;
    RECT a = { 0, 0, (LONG)s->w, (LONG)s->h }, b = { 0, 0, (LONG)t->w, (LONG)t->h };
    if(sr) a = *sr;
    if(dr) b = *dr;
    EnterCriticalSection(&lock);
    DWORD *p = cmd(CMD_SURFACE_STRETCHBLT, 19 * 4);
    p[0] = s->sid; p[1] = 0; p[2] = s->level; p[3] = t->sid; p[4] = 0; p[5] = t->level;
    p[6] = a.left; p[7] = a.top; p[8] = 0; p[9] = a.right - a.left; p[10] = a.bottom - a.top; p[11] = 1;
    p[12] = b.left; p[13] = b.top; p[14] = 0; p[15] = b.right - b.left; p[16] = b.bottom - b.top; p[17] = 1;
    p[18] = filter == D3DTEXF_LINEAR ? 2 : 1;
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_ColorFill(Device *d, IDirect3DSurface9 *surf, const RECT *r, D3DCOLOR c)
{
    Surface *s = (Surface *)surf;
    EnterCriticalSection(&lock);
    set_rendertarget(d, RT_COLOR0, s);
    DWORD *p = cmd(CMD_CLEAR, 9 * 4);
    p[0] = d->cid; p[1] = D3DCLEAR_TARGET; p[2] = c; p[3] = fbits(0); p[4] = 0;
    p[5] = r ? r->left : 0; p[6] = r ? r->top : 0;
    p[7] = r ? r->right - r->left : s->w; p[8] = r ? r->bottom - r->top : s->h;
    set_rendertarget(d, RT_COLOR0, d->rt0);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateOffscreenPlainSurface(Device *d, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
    IDirect3DSurface9 **out, HANDLE *sh)
{
    if(!svga_format(fmt)) return D3DERR_INVALIDCALL;
    Surface *s = surface_new(d, w, h, fmt, 0, pool, pool == D3DPOOL_DEFAULT);
    if(!s->mem) s->mem = VirtualAlloc(0, s->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *out = (IDirect3DSurface9 *)s;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetRenderTarget(Device *d, DWORD i, IDirect3DSurface9 *surf)
{
    Surface *s = (Surface *)surf;
    if(i) return i < 4 && !s ? D3D_OK : D3DERR_INVALIDCALL;
    if(!s) return D3DERR_INVALIDCALL;
    if(s != d->rt0) { surf_AddRef(s); if(d->rt0) surf_Release(d->rt0); d->rt0 = s; }
    EnterCriticalSection(&lock);
    set_rendertarget(d, RT_COLOR0, s);
    d->s.viewport.X = d->s.viewport.Y = 0; d->s.viewport.Width = s->w; d->s.viewport.Height = s->h;
    d->s.viewport.MinZ = 0; d->s.viewport.MaxZ = 1;
    apply_viewport(d);
    SetRect(&d->s.scissor, 0, 0, s->w, s->h);
    apply_scissor(d);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderTarget(Device *d, DWORD i, IDirect3DSurface9 **out)
{
    if(i || !d->rt0) { *out = 0; return D3DERR_NOTFOUND; }
    *out = (IDirect3DSurface9 *)d->rt0; surf_AddRef(d->rt0); return D3D_OK;
}
static HRESULT WINAPI dev_SetDepthStencilSurface(Device *d, IDirect3DSurface9 *surf)
{
    Surface *s = (Surface *)surf;
    if(s) surf_AddRef(s);
    if(d->ds) surf_Release(d->ds);
    d->ds = s;
    EnterCriticalSection(&lock);
    set_rendertarget(d, RT_DEPTH, s);
    set_rendertarget(d, RT_STENCIL, s);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetDepthStencilSurface(Device *d, IDirect3DSurface9 **out)
{
    if(!d->ds) { *out = 0; return D3DERR_NOTFOUND; }
    *out = (IDirect3DSurface9 *)d->ds; surf_AddRef(d->ds); return D3D_OK;
}
static HRESULT WINAPI dev_BeginScene(Device *d) { d->in_scene = TRUE; return D3D_OK; }
static HRESULT WINAPI dev_EndScene(Device *d) { d->in_scene = FALSE; return D3D_OK; }
static HRESULT WINAPI dev_Clear(Device *d, DWORD n, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD st)
{
    const D3DVIEWPORT9 *v = &d->s.viewport;
    EnterCriticalSection(&lock);
    if(!d->ds) flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if(!n || !rects)
    {
        DWORD *p = cmd(CMD_CLEAR, 9 * 4);
        p[0] = d->cid; p[1] = flags; p[2] = color; p[3] = fbits(z); p[4] = st;
        p[5] = v->X; p[6] = v->Y; p[7] = v->Width; p[8] = v->Height;
    }
    else
    {
        DWORD *p = cmd(CMD_CLEAR, (5 + 4 * n) * 4);
        p[0] = d->cid; p[1] = flags; p[2] = color; p[3] = fbits(z); p[4] = st;
        for(DWORD i = 0; i < n; i++)
        {
            p[5 + i * 4] = rects[i].x1; p[6 + i * 4] = rects[i].y1;
            p[7 + i * 4] = rects[i].x2 - rects[i].x1; p[8 + i * 4] = rects[i].y2 - rects[i].y1;
        }
    }
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetTransform(Device *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    if((DWORD)t >= 512) return D3DERR_INVALIDCALL;
    d->s.transform[t] = *m;
    RECORD(SB_TRANSFORM, t);
    EnterCriticalSection(&lock);
    apply_transform(d, t);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTransform(Device *d, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m)
{
    if((DWORD)t >= 512) return D3DERR_INVALIDCALL;
    *m = d->s.transform[t]; return D3D_OK;
}
static HRESULT WINAPI dev_MultiplyTransform(Device *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    if((DWORD)t >= 512) return D3DERR_INVALIDCALL;
    D3DMATRIX r, *a = &d->s.transform[t];
    for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++)
    {
        float s = 0;
        for(int k = 0; k < 4; k++) s += m->m[i][k] * a->m[k][j];
        r.m[i][j] = s;
    }
    return dev_SetTransform(d, t, &r);
}
static HRESULT WINAPI dev_SetViewport(Device *d, const D3DVIEWPORT9 *v)
{
    d->s.viewport = *v;
    RECORD(SB_VIEWPORT, 0);
    EnterCriticalSection(&lock);
    apply_viewport(d);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetViewport(Device *d, D3DVIEWPORT9 *v) { *v = d->s.viewport; return D3D_OK; }
static HRESULT WINAPI dev_SetMaterial(Device *d, const D3DMATERIAL9 *m)
{
    d->s.material = *m;
    RECORD(SB_MATERIAL, 0);
    EnterCriticalSection(&lock);
    apply_material(d);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetMaterial(Device *d, D3DMATERIAL9 *m) { *m = d->s.material; return D3D_OK; }
static HRESULT WINAPI dev_SetLight(Device *d, DWORD i, const D3DLIGHT9 *l)
{
    if(i >= MAX_LIGHTS) return D3DERR_INVALIDCALL;
    d->s.light[i] = *l; d->s.light_set[i] = TRUE;
    RECORD(SB_LIGHT, i);
    EnterCriticalSection(&lock);
    apply_light(d, i);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetLight(Device *d, DWORD i, D3DLIGHT9 *l)
{
    if(i >= MAX_LIGHTS || !d->s.light_set[i]) return D3DERR_INVALIDCALL;
    *l = d->s.light[i]; return D3D_OK;
}
static HRESULT WINAPI dev_LightEnable(Device *d, DWORD i, BOOL on)
{
    if(i >= MAX_LIGHTS) return D3DERR_INVALIDCALL;
    if(!d->s.light_set[i])
    {
        D3DLIGHT9 l = { 0 };
        l.Type = D3DLIGHT_DIRECTIONAL; l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = 1; l.Direction.z = 1;
        dev_SetLight(d, i, &l);
    }
    d->s.light_enable[i] = !!on;
    RECORD(SB_LIGHTENABLE, i);
    EnterCriticalSection(&lock);
    apply_light_enable(d, i);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetLightEnable(Device *d, DWORD i, BOOL *on)
{
    if(i >= MAX_LIGHTS || !d->s.light_set[i]) return D3DERR_INVALIDCALL;
    *on = d->s.light_enable[i] ? 128 : 0; return D3D_OK;
}
static HRESULT WINAPI dev_SetClipPlane(Device *d, DWORD i, const float *p) { return D3D_OK; }
static HRESULT WINAPI dev_GetClipPlane(Device *d, DWORD i, float *p) { p[0] = p[1] = p[2] = p[3] = 0; return D3D_OK; }
static HRESULT WINAPI dev_SetRenderState(Device *d, D3DRENDERSTATETYPE st, DWORD v)
{
    if((DWORD)st >= 256) return D3DERR_INVALIDCALL;
    d->s.rs[st] = v;
    RECORD(SB_RS, st);
    EnterCriticalSection(&lock);
    apply_rs(d, st);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderState(Device *d, D3DRENDERSTATETYPE st, DWORD *v)
{
    if((DWORD)st >= 256) return D3DERR_INVALIDCALL;
    *v = d->s.rs[st]; return D3D_OK;
}
static HRESULT WINAPI dev_CreateStateBlock(Device *d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **out)
{
    StateBlock *sb = sb_new(d);
    sb_fill(sb, t);
    *out = (IDirect3DStateBlock9 *)sb;
    return D3D_OK;
}
static HRESULT WINAPI dev_BeginStateBlock(Device *d)
{
    if(d->recording) return D3DERR_INVALIDCALL;
    d->recording = sb_new(d);
    return D3D_OK;
}
static HRESULT WINAPI dev_EndStateBlock(Device *d, IDirect3DStateBlock9 **out)
{
    if(!d->recording) return D3DERR_INVALIDCALL;
    StateBlock *sb = d->recording;
    d->recording = NULL;
    sb_capture(sb);
    *out = (IDirect3DStateBlock9 *)sb;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetClipStatus(Device *d, const D3DCLIPSTATUS9 *s) { return D3D_OK; }
static HRESULT WINAPI dev_GetClipStatus(Device *d, D3DCLIPSTATUS9 *s) { memset(s, 0, sizeof *s); return D3D_OK; }
static HRESULT WINAPI dev_GetTexture(Device *d, DWORD stage, IDirect3DBaseTexture9 **out)
{
    if(stage >= MAX_SAMPLERS) return D3DERR_INVALIDCALL;
    *out = d->s.texture[stage];
    if(*out) IDirect3DBaseTexture9_AddRef(*out);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetTexture(Device *d, DWORD stage, IDirect3DBaseTexture9 *t)
{
    if(stage >= D3DVERTEXTEXTURESAMPLER0) return D3D_OK;
    if(stage >= MAX_SAMPLERS) return D3DERR_INVALIDCALL;
    if(t) IDirect3DBaseTexture9_AddRef(t);
    if(d->s.texture[stage]) IDirect3DBaseTexture9_Release(d->s.texture[stage]);
    d->s.texture[stage] = t;
    RECORD(SB_TEXTURE, stage);
    EnterCriticalSection(&lock);
    apply_texture(d, stage);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTextureStageState(Device *d, DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
{
    if(s >= MAX_STAGES || (DWORD)t > 32) return D3DERR_INVALIDCALL;
    *v = d->s.tss[s][t]; return D3D_OK;
}
static HRESULT WINAPI dev_SetTextureStageState(Device *d, DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    if(s >= MAX_STAGES || (DWORD)t > 32) return D3DERR_INVALIDCALL;
    d->s.tss[s][t] = v;
    RECORD(SB_TSS, s << 8 | t);
    EnterCriticalSection(&lock);
    apply_tss(d, s, t);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetSamplerState(Device *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD *v)
{
    if(s >= MAX_SAMPLERS || (DWORD)t > 13) return D3DERR_INVALIDCALL;
    *v = d->s.samp[s][t]; return D3D_OK;
}
static HRESULT WINAPI dev_SetSamplerState(Device *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    if(s >= D3DVERTEXTEXTURESAMPLER0) return D3D_OK;
    if(s >= MAX_SAMPLERS || (DWORD)t > 13) return D3DERR_INVALIDCALL;
    d->s.samp[s][t] = v;
    RECORD(SB_SAMP, s << 8 | t);
    EnterCriticalSection(&lock);
    apply_samp(d, s, t);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_ValidateDevice(Device *d, DWORD *passes) { *passes = 1; return D3D_OK; }
static HRESULT WINAPI dev_SetPaletteEntries(Device *d, UINT n, const PALETTEENTRY *e) { return D3D_OK; }
static HRESULT WINAPI dev_GetPaletteEntries(Device *d, UINT n, PALETTEENTRY *e) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI dev_SetCurrentTexturePalette(Device *d, UINT n) { return D3D_OK; }
static HRESULT WINAPI dev_GetCurrentTexturePalette(Device *d, UINT *n) { *n = 0; return D3D_OK; }
static HRESULT WINAPI dev_SetScissorRect(Device *d, const RECT *r)
{
    d->s.scissor = *r;
    RECORD(SB_SCISSOR, 0);
    EnterCriticalSection(&lock);
    apply_scissor(d);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetScissorRect(Device *d, RECT *r) { *r = d->s.scissor; return D3D_OK; }
static HRESULT WINAPI dev_SetSoftwareVertexProcessing(Device *d, BOOL on) { return D3D_OK; }
static BOOL WINAPI dev_GetSoftwareVertexProcessing(Device *d) { return FALSE; }
static HRESULT WINAPI dev_SetNPatchMode(Device *d, float n) { return D3D_OK; }
static float WINAPI dev_GetNPatchMode(Device *d) { return 0; }

/* ---------- drawing ---------- */

static const Decl *current_decl(Device *d);

static DWORD prim_type(D3DPRIMITIVETYPE t)
{
    switch(t)
    {
        case D3DPT_POINTLIST: return 2;
        case D3DPT_LINELIST: return 3;
        case D3DPT_LINESTRIP: return 4;
        case D3DPT_TRIANGLELIST: return 1;
        case D3DPT_TRIANGLESTRIP: return 5;
        case D3DPT_TRIANGLEFAN: return 6;
        default: return 1;
    }
}

/* vb_sid/vb_off/vb_stride: per stream (UP draws pass their own). */
static void emit_draw(Device *d, D3DPRIMITIVETYPE pt, UINT count, const DWORD *vb_sid, const UINT *vb_off,
    const UINT *vb_stride, DWORD ib_sid, UINT ib_off, UINT iwidth, INT bias)
{
    const Decl *decl = current_decl(d);
    if(!decl || !decl->n) return;
    UINT n = decl->n;
    DWORD *p = cmd(CMD_DRAW_PRIMITIVES, (3 + n * 9 + 7) * 4);
    p[0] = d->cid; p[1] = n; p[2] = 1;
    DWORD *q = p + 3;
    for(UINT i = 0; i < n; i++, q += 9)
    {
        const D3DVERTEXELEMENT9 *e = &decl->el[i];
        q[0] = e->Type; q[1] = e->Method; q[2] = e->Usage; q[3] = e->UsageIndex;
        q[4] = vb_sid[e->Stream]; q[5] = vb_off[e->Stream] + e->Offset; q[6] = vb_stride[e->Stream];
        q[7] = 0; q[8] = 0;
    }
    q[0] = prim_type(pt); q[1] = count; q[2] = ib_sid; q[3] = ib_off; q[4] = iwidth; q[5] = iwidth; q[6] = bias;
}

static void stream_arrays(Device *d, DWORD *sid, UINT *off, UINT *stride, UINT start_vertex)
{
    for(UINT i = 0; i < MAX_STREAMS; i++)
    {
        Buffer *b = d->s.stream[i];
        sid[i] = b ? b->sid : INVALID_ID;
        stride[i] = d->s.stream_stride[i];
        off[i] = d->s.stream_offset[i] + start_vertex * stride[i];
    }
}

static HRESULT WINAPI dev_DrawPrimitive(Device *d, D3DPRIMITIVETYPE pt, UINT start, UINT count)
{
    DWORD sid[MAX_STREAMS]; UINT off[MAX_STREAMS], stride[MAX_STREAMS];
    EnterCriticalSection(&lock);
    stream_arrays(d, sid, off, stride, start);
    emit_draw(d, pt, count, sid, off, stride, INVALID_ID, 0, 0, 0);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitive(Device *d, D3DPRIMITIVETYPE pt, INT base, UINT minv, UINT nv,
    UINT start, UINT count)
{
    Buffer *ib = d->s.indices;
    if(!ib) return D3DERR_INVALIDCALL;
    DWORD sid[MAX_STREAMS]; UINT off[MAX_STREAMS], stride[MAX_STREAMS];
    UINT w = ib->format == D3DFMT_INDEX32 ? 4 : 2;
    EnterCriticalSection(&lock);
    stream_arrays(d, sid, off, stride, 0);
    if(base < 0)
    {
        /* The host takes non-negative biases only: move the streams instead. */
        for(UINT i = 0; i < MAX_STREAMS; i++) off[i] += base * (INT)stride[i];
        base = 0;
    }
    emit_draw(d, pt, count, sid, off, stride, ib->sid, start * w, w, base);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}

static UINT vertex_count(D3DPRIMITIVETYPE pt, UINT n)
{
    switch(pt)
    {
        case D3DPT_POINTLIST: return n;
        case D3DPT_LINELIST: return n * 2;
        case D3DPT_LINESTRIP: return n + 1;
        case D3DPT_TRIANGLELIST: return n * 3;
        default: return n + 2;
    }
}

/* Scratch buffers for the UP draws. */
static void up_buffer(DWORD *sid, DWORD *size, UINT need)
{
    if(*size >= need) return;
    if(*sid) surface_destroy(*sid);
    UINT s = 64 * 1024;
    while(s < need) s *= 2;
    *sid = new_id(); *size = s;
    DWORD *p = cmd(CMD_SURFACE_DEFINE, (9 + 3) * 4);
    p[0] = *sid; p[1] = SURFACE_STREAM; p[2] = SF_BUFFER; p[3] = 1; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
    p[9] = s; p[10] = 1; p[11] = 1;
}

static HRESULT WINAPI dev_DrawPrimitiveUP(Device *d, D3DPRIMITIVETYPE pt, UINT count, const void *data, UINT stride)
{
    UINT bytes = vertex_count(pt, count) * stride;
    DWORD sid[MAX_STREAMS]; UINT off[MAX_STREAMS] = { 0 }, str[MAX_STREAMS] = { 0 };
    EnterCriticalSection(&lock);
    up_buffer(&d->up_vb, &d->up_vb_size, bytes);
    dma(d->up_vb, 0, data, 0, XFER_TO_HOST, 0, 0, bytes, 1);
    for(UINT i = 0; i < MAX_STREAMS; i++) sid[i] = INVALID_ID;
    sid[0] = d->up_vb; str[0] = stride;
    emit_draw(d, pt, count, sid, off, str, INVALID_ID, 0, 0, 0);
    flush();
    LeaveCriticalSection(&lock);
    /* D3D9 resets stream 0 after a UP draw. */
    if(d->s.stream[0]) { buf_Release(d->s.stream[0]); d->s.stream[0] = NULL; }
    d->s.stream_offset[0] = d->s.stream_stride[0] = 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitiveUP(Device *d, D3DPRIMITIVETYPE pt, UINT minv, UINT nv, UINT count,
    const void *idx, D3DFORMAT ifmt, const void *data, UINT stride)
{
    UINT w = ifmt == D3DFMT_INDEX32 ? 4 : 2;
    UINT vbytes = (minv + nv) * stride, ibytes = vertex_count(pt, count) * w;
    DWORD sid[MAX_STREAMS]; UINT off[MAX_STREAMS] = { 0 }, str[MAX_STREAMS] = { 0 };
    EnterCriticalSection(&lock);
    up_buffer(&d->up_vb, &d->up_vb_size, vbytes);
    up_buffer(&d->up_ib, &d->up_ib_size, ibytes);
    dma(d->up_vb, 0, data, 0, XFER_TO_HOST, 0, 0, vbytes, 1);
    dma(d->up_ib, 0, idx, 0, XFER_TO_HOST, 0, 0, ibytes, 1);
    for(UINT i = 0; i < MAX_STREAMS; i++) sid[i] = INVALID_ID;
    sid[0] = d->up_vb; str[0] = stride;
    emit_draw(d, pt, count, sid, off, str, d->up_ib, 0, w, 0);
    flush();
    LeaveCriticalSection(&lock);
    if(d->s.stream[0]) { buf_Release(d->s.stream[0]); d->s.stream[0] = NULL; }
    d->s.stream_offset[0] = d->s.stream_stride[0] = 0;
    if(d->s.indices) { buf_Release(d->s.indices); d->s.indices = NULL; }
    return D3D_OK;
}
static HRESULT WINAPI dev_ProcessVertices(Device *d, UINT a, UINT b, UINT c, IDirect3DVertexBuffer9 *vb,
    IDirect3DVertexDeclaration9 *decl, DWORD f)
{
    hlog("ProcessVertices: not supported");
    return D3DERR_INVALIDCALL;
}

static Decl *decl_new(Device *d)
{
    Decl *x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *x);
    x->o.vtbl = &decl_vtbl; x->o.ref = 1; x->o.dev = d;
    return x;
}
static HRESULT WINAPI dev_CreateVertexDeclaration(Device *d, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **out)
{
    Decl *x = decl_new(d);
    while(el[x->n].Stream != 0xFF && x->n < MAXD3DDECLLENGTH) { x->el[x->n] = el[x->n]; x->n++; }
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    x->el[x->n] = end;
    *out = (IDirect3DVertexDeclaration9 *)x;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetVertexDeclaration(Device *d, IDirect3DVertexDeclaration9 *decl)
{
    Decl *x = (Decl *)decl;
    if(x) obj_addref(&x->o);
    if(d->s.decl) free_Release(&d->s.decl->o);
    d->s.decl = x;
    d->s.fvf = 0;
    RECORD(SB_DECL, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetVertexDeclaration(Device *d, IDirect3DVertexDeclaration9 **out)
{
    *out = (IDirect3DVertexDeclaration9 *)d->s.decl;
    if(d->s.decl) obj_addref(&d->s.decl->o);
    return D3D_OK;
}

/* FVF → declaration (stream 0). */
static void fvf_decl(DWORD fvf, Decl *x)
{
    UINT off = 0, n = 0;
#define EL(type, usage, idx, size) do { D3DVERTEXELEMENT9 e = { 0, (WORD)off, type, D3DDECLMETHOD_DEFAULT, usage, idx }; \
    x->el[n++] = e; off += size; } while(0)
    switch(fvf & D3DFVF_POSITION_MASK)
    {
        case D3DFVF_XYZ: EL(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12); break;
        case D3DFVF_XYZRHW: EL(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16); break;
        case D3DFVF_XYZW: EL(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16); break;
        case D3DFVF_XYZB1: case D3DFVF_XYZB2: case D3DFVF_XYZB3: case D3DFVF_XYZB4: case D3DFVF_XYZB5:
        {
            UINT nb = ((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2 + 1;
            EL(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
            off += nb * 4;                               /* blend weights: skipped */
            break;
        }
    }
    if(fvf & D3DFVF_NORMAL) EL(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if(fvf & D3DFVF_PSIZE) EL(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if(fvf & D3DFVF_DIFFUSE) EL(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if(fvf & D3DFVF_SPECULAR) EL(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    UINT ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for(UINT i = 0; i < ntex && i < 8; i++)
    {
        UINT f = (fvf >> (16 + i * 2)) & 3;
        static const BYTE types[] = { D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1 };
        static const BYTE sizes[] = { 8, 12, 16, 4 };
        EL(types[f], D3DDECLUSAGE_TEXCOORD, i, sizes[f]);
    }
#undef EL
    x->n = n; x->fvf = fvf;
    D3DVERTEXELEMENT9 end = D3DDECL_END();
    x->el[n] = end;
}

static const Decl *current_decl(Device *d)
{
    if(d->s.decl) return d->s.decl;
    if(!d->s.fvf) return NULL;
    if(!d->fvf_decl) d->fvf_decl = decl_new(d);
    if(d->fvf_decl_fvf != d->s.fvf || !d->fvf_decl->n) { fvf_decl(d->s.fvf, d->fvf_decl); d->fvf_decl_fvf = d->s.fvf; }
    return d->fvf_decl;
}

static HRESULT WINAPI dev_SetFVF(Device *d, DWORD fvf)
{
    d->s.fvf = fvf;
    if(fvf && d->s.decl) { free_Release(&d->s.decl->o); d->s.decl = NULL; }
    RECORD(SB_FVF, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetFVF(Device *d, DWORD *fvf) { *fvf = d->s.fvf; return D3D_OK; }

static Shader *shader_new(Device *d, DWORD type, const DWORD *tok)
{
    Shader *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    s->o.vtbl = type == SHADER_VS ? (void *)&vs_vtbl : (void *)&ps_vtbl; s->o.ref = 1; s->o.dev = d;
    s->type = type; s->shid = new_id();
    UINT n = 1;
    while(tok[n - 1] != 0x0000FFFF && n < 65536)
    {
        DWORD t = tok[n];
        if((t & 0xFFFF) == 0xFFFE) { n += 1 + (t >> 16); continue; }      /* comment */
        n++;
    }
    s->ntok = n;
    s->tok = HeapAlloc(GetProcessHeap(), 0, n * 4);
    memcpy(s->tok, tok, n * 4);
    EnterCriticalSection(&lock);
    DWORD *p = cmd(CMD_SHADER_DEFINE, (3 + n) * 4);
    p[0] = d->cid; p[1] = s->shid; p[2] = type;
    memcpy(p + 3, tok, n * 4);
    LeaveCriticalSection(&lock);
    return s;
}
static HRESULT WINAPI dev_CreateVertexShader(Device *d, const DWORD *tok, IDirect3DVertexShader9 **out)
{
    *out = (IDirect3DVertexShader9 *)shader_new(d, SHADER_VS, tok); return D3D_OK;
}
static HRESULT WINAPI dev_SetVertexShader(Device *d, IDirect3DVertexShader9 *s)
{
    Shader *x = (Shader *)s;
    if(x) obj_addref(&x->o);
    if(d->s.vs) shader_Release(d->s.vs);
    d->s.vs = x;
    RECORD(SB_VS, 0);
    EnterCriticalSection(&lock);
    apply_shader(d, SHADER_VS);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetVertexShader(Device *d, IDirect3DVertexShader9 **out)
{
    *out = (IDirect3DVertexShader9 *)d->s.vs;
    if(d->s.vs) obj_addref(&d->s.vs->o);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetVertexShaderConstantF(Device *d, UINT reg, const float *f, UINT n)
{
    if(reg + n > 256) return D3DERR_INVALIDCALL;
    memcpy(d->s.vsc[reg], f, n * 16);
    if(d->recording) for(UINT i = 0; i < n; i++) sb_add(d->recording, SB_VSCONST, reg + i, 0);
    EnterCriticalSection(&lock);
    apply_const(d, SHADER_VS, reg, n);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetVertexShaderConstantF(Device *d, UINT reg, float *f, UINT n)
{
    if(reg + n > 256) return D3DERR_INVALIDCALL;
    memcpy(f, d->s.vsc[reg], n * 16); return D3D_OK;
}
static HRESULT WINAPI dev_SetShaderConstIB(Device *d, DWORD type, DWORD ctype, UINT reg, const void *v, UINT n)
{
    EnterCriticalSection(&lock);
    for(UINT i = 0; i < n; i++)
    {
        DWORD *p = cmd(CMD_SET_SHADER_CONST, 32);
        p[0] = d->cid; p[1] = reg + i; p[2] = type; p[3] = ctype;
        if(ctype == 1) memcpy(p + 4, (const DWORD *)v + i * 4, 16);
        else { p[4] = ((const DWORD *)v)[i]; p[5] = p[6] = p[7] = 0; }
    }
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetVertexShaderConstantI(Device *d, UINT reg, const int *v, UINT n) { return dev_SetShaderConstIB(d, SHADER_VS, 1, reg, v, n); }
static HRESULT WINAPI dev_GetVertexShaderConstantI(Device *d, UINT reg, int *v, UINT n) { memset(v, 0, n * 16); return D3D_OK; }
static HRESULT WINAPI dev_SetVertexShaderConstantB(Device *d, UINT reg, const BOOL *v, UINT n) { return dev_SetShaderConstIB(d, SHADER_VS, 2, reg, v, n); }
static HRESULT WINAPI dev_GetVertexShaderConstantB(Device *d, UINT reg, BOOL *v, UINT n) { memset(v, 0, n * 4); return D3D_OK; }
static HRESULT WINAPI dev_SetStreamSource(Device *d, UINT n, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    if(n >= MAX_STREAMS) return D3DERR_INVALIDCALL;
    Buffer *b = (Buffer *)vb;
    if(b) obj_addref(&b->o);
    if(d->s.stream[n]) buf_Release(d->s.stream[n]);
    d->s.stream[n] = b; d->s.stream_offset[n] = off; d->s.stream_stride[n] = stride;
    RECORD(SB_STREAM, n);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetStreamSource(Device *d, UINT n, IDirect3DVertexBuffer9 **vb, UINT *off, UINT *stride)
{
    if(n >= MAX_STREAMS) return D3DERR_INVALIDCALL;
    *vb = (IDirect3DVertexBuffer9 *)d->s.stream[n];
    if(*vb) obj_addref(&d->s.stream[n]->o);
    *off = d->s.stream_offset[n]; *stride = d->s.stream_stride[n];
    return D3D_OK;
}
static HRESULT WINAPI dev_SetStreamSourceFreq(Device *d, UINT n, UINT div) { return D3D_OK; }
static HRESULT WINAPI dev_GetStreamSourceFreq(Device *d, UINT n, UINT *div) { *div = 1; return D3D_OK; }
static HRESULT WINAPI dev_SetIndices(Device *d, IDirect3DIndexBuffer9 *ib)
{
    Buffer *b = (Buffer *)ib;
    if(b) obj_addref(&b->o);
    if(d->s.indices) buf_Release(d->s.indices);
    d->s.indices = b;
    RECORD(SB_INDICES, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetIndices(Device *d, IDirect3DIndexBuffer9 **ib)
{
    *ib = (IDirect3DIndexBuffer9 *)d->s.indices;
    if(*ib) obj_addref(&d->s.indices->o);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreatePixelShader(Device *d, const DWORD *tok, IDirect3DPixelShader9 **out)
{
    *out = (IDirect3DPixelShader9 *)shader_new(d, SHADER_PS, tok); return D3D_OK;
}
static HRESULT WINAPI dev_SetPixelShader(Device *d, IDirect3DPixelShader9 *s)
{
    Shader *x = (Shader *)s;
    if(x) obj_addref(&x->o);
    if(d->s.ps) shader_Release(d->s.ps);
    d->s.ps = x;
    RECORD(SB_PS, 0);
    EnterCriticalSection(&lock);
    apply_shader(d, SHADER_PS);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetPixelShader(Device *d, IDirect3DPixelShader9 **out)
{
    *out = (IDirect3DPixelShader9 *)d->s.ps;
    if(d->s.ps) obj_addref(&d->s.ps->o);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetPixelShaderConstantF(Device *d, UINT reg, const float *f, UINT n)
{
    if(reg + n > 224) return D3DERR_INVALIDCALL;
    memcpy(d->s.psc[reg], f, n * 16);
    if(d->recording) for(UINT i = 0; i < n; i++) sb_add(d->recording, SB_PSCONST, reg + i, 0);
    EnterCriticalSection(&lock);
    apply_const(d, SHADER_PS, reg, n);
    LeaveCriticalSection(&lock);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetPixelShaderConstantF(Device *d, UINT reg, float *f, UINT n)
{
    if(reg + n > 224) return D3DERR_INVALIDCALL;
    memcpy(f, d->s.psc[reg], n * 16); return D3D_OK;
}
static HRESULT WINAPI dev_SetPixelShaderConstantI(Device *d, UINT reg, const int *v, UINT n) { return dev_SetShaderConstIB(d, SHADER_PS, 1, reg, v, n); }
static HRESULT WINAPI dev_GetPixelShaderConstantI(Device *d, UINT reg, int *v, UINT n) { memset(v, 0, n * 16); return D3D_OK; }
static HRESULT WINAPI dev_SetPixelShaderConstantB(Device *d, UINT reg, const BOOL *v, UINT n) { return dev_SetShaderConstIB(d, SHADER_PS, 2, reg, v, n); }
static HRESULT WINAPI dev_GetPixelShaderConstantB(Device *d, UINT reg, BOOL *v, UINT n) { memset(v, 0, n * 4); return D3D_OK; }
static HRESULT WINAPI dev_DrawRectPatch(Device *d, UINT h, const float *s, const D3DRECTPATCH_INFO *i) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI dev_DrawTriPatch(Device *d, UINT h, const float *s, const D3DTRIPATCH_INFO *i) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI dev_DeletePatch(Device *d, UINT h) { return D3D_OK; }
static HRESULT WINAPI dev_CreateQuery(Device *d, D3DQUERYTYPE t, IDirect3DQuery9 **out)
{
    if(t != D3DQUERYTYPE_EVENT && t != D3DQUERYTYPE_OCCLUSION) return D3DERR_NOTAVAILABLE;
    if(!out) return D3D_OK;
    Query *q = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *q);
    q->o.vtbl = &query_vtbl; q->o.ref = 1; q->o.dev = d; q->type = t;
    *out = (IDirect3DQuery9 *)q;
    return D3D_OK;
}

static IDirect3DDevice9Vtbl dev_vtbl = {
    .QueryInterface = F(dev_QueryInterface), .AddRef = F(dev_AddRef), .Release = F(dev_Release),
    .TestCooperativeLevel = F(dev_TestCooperativeLevel), .GetAvailableTextureMem = F(dev_GetAvailableTextureMem),
    .EvictManagedResources = F(dev_EvictManagedResources), .GetDirect3D = F(dev_GetDirect3D),
    .GetDeviceCaps = F(dev_GetDeviceCaps), .GetDisplayMode = F(dev_GetDisplayMode),
    .GetCreationParameters = F(dev_GetCreationParameters), .SetCursorProperties = F(dev_SetCursorProperties),
    .SetCursorPosition = F(dev_SetCursorPosition), .ShowCursor = F(dev_ShowCursor),
    .CreateAdditionalSwapChain = F(dev_CreateAdditionalSwapChain), .GetSwapChain = F(dev_GetSwapChain),
    .GetNumberOfSwapChains = F(dev_GetNumberOfSwapChains), .Reset = F(dev_Reset), .Present = F(dev_Present),
    .GetBackBuffer = F(dev_GetBackBuffer), .GetRasterStatus = F(dev_GetRasterStatus),
    .SetDialogBoxMode = F(dev_SetDialogBoxMode), .SetGammaRamp = F(dev_SetGammaRamp),
    .GetGammaRamp = F(dev_GetGammaRamp), .CreateTexture = F(dev_CreateTexture),
    .CreateVolumeTexture = F(dev_CreateVolumeTexture), .CreateCubeTexture = F(dev_CreateCubeTexture),
    .CreateVertexBuffer = F(dev_CreateVertexBuffer), .CreateIndexBuffer = F(dev_CreateIndexBuffer),
    .CreateRenderTarget = F(dev_CreateRenderTarget), .CreateDepthStencilSurface = F(dev_CreateDepthStencilSurface),
    .UpdateSurface = F(dev_UpdateSurface), .UpdateTexture = F(dev_UpdateTexture),
    .GetRenderTargetData = F(dev_GetRenderTargetData), .GetFrontBufferData = F(dev_GetFrontBufferData),
    .StretchRect = F(dev_StretchRect), .ColorFill = F(dev_ColorFill),
    .CreateOffscreenPlainSurface = F(dev_CreateOffscreenPlainSurface), .SetRenderTarget = F(dev_SetRenderTarget),
    .GetRenderTarget = F(dev_GetRenderTarget), .SetDepthStencilSurface = F(dev_SetDepthStencilSurface),
    .GetDepthStencilSurface = F(dev_GetDepthStencilSurface), .BeginScene = F(dev_BeginScene),
    .EndScene = F(dev_EndScene), .Clear = F(dev_Clear), .SetTransform = F(dev_SetTransform),
    .GetTransform = F(dev_GetTransform), .MultiplyTransform = F(dev_MultiplyTransform),
    .SetViewport = F(dev_SetViewport), .GetViewport = F(dev_GetViewport), .SetMaterial = F(dev_SetMaterial),
    .GetMaterial = F(dev_GetMaterial), .SetLight = F(dev_SetLight), .GetLight = F(dev_GetLight),
    .LightEnable = F(dev_LightEnable), .GetLightEnable = F(dev_GetLightEnable), .SetClipPlane = F(dev_SetClipPlane),
    .GetClipPlane = F(dev_GetClipPlane), .SetRenderState = F(dev_SetRenderState),
    .GetRenderState = F(dev_GetRenderState), .CreateStateBlock = F(dev_CreateStateBlock),
    .BeginStateBlock = F(dev_BeginStateBlock), .EndStateBlock = F(dev_EndStateBlock),
    .SetClipStatus = F(dev_SetClipStatus), .GetClipStatus = F(dev_GetClipStatus), .GetTexture = F(dev_GetTexture),
    .SetTexture = F(dev_SetTexture), .GetTextureStageState = F(dev_GetTextureStageState),
    .SetTextureStageState = F(dev_SetTextureStageState), .GetSamplerState = F(dev_GetSamplerState),
    .SetSamplerState = F(dev_SetSamplerState), .ValidateDevice = F(dev_ValidateDevice),
    .SetPaletteEntries = F(dev_SetPaletteEntries), .GetPaletteEntries = F(dev_GetPaletteEntries),
    .SetCurrentTexturePalette = F(dev_SetCurrentTexturePalette),
    .GetCurrentTexturePalette = F(dev_GetCurrentTexturePalette), .SetScissorRect = F(dev_SetScissorRect),
    .GetScissorRect = F(dev_GetScissorRect), .SetSoftwareVertexProcessing = F(dev_SetSoftwareVertexProcessing),
    .GetSoftwareVertexProcessing = F(dev_GetSoftwareVertexProcessing), .SetNPatchMode = F(dev_SetNPatchMode),
    .GetNPatchMode = F(dev_GetNPatchMode), .DrawPrimitive = F(dev_DrawPrimitive),
    .DrawIndexedPrimitive = F(dev_DrawIndexedPrimitive), .DrawPrimitiveUP = F(dev_DrawPrimitiveUP),
    .DrawIndexedPrimitiveUP = F(dev_DrawIndexedPrimitiveUP), .ProcessVertices = F(dev_ProcessVertices),
    .CreateVertexDeclaration = F(dev_CreateVertexDeclaration), .SetVertexDeclaration = F(dev_SetVertexDeclaration),
    .GetVertexDeclaration = F(dev_GetVertexDeclaration), .SetFVF = F(dev_SetFVF), .GetFVF = F(dev_GetFVF),
    .CreateVertexShader = F(dev_CreateVertexShader), .SetVertexShader = F(dev_SetVertexShader),
    .GetVertexShader = F(dev_GetVertexShader), .SetVertexShaderConstantF = F(dev_SetVertexShaderConstantF),
    .GetVertexShaderConstantF = F(dev_GetVertexShaderConstantF),
    .SetVertexShaderConstantI = F(dev_SetVertexShaderConstantI),
    .GetVertexShaderConstantI = F(dev_GetVertexShaderConstantI),
    .SetVertexShaderConstantB = F(dev_SetVertexShaderConstantB),
    .GetVertexShaderConstantB = F(dev_GetVertexShaderConstantB), .SetStreamSource = F(dev_SetStreamSource),
    .GetStreamSource = F(dev_GetStreamSource), .SetStreamSourceFreq = F(dev_SetStreamSourceFreq),
    .GetStreamSourceFreq = F(dev_GetStreamSourceFreq), .SetIndices = F(dev_SetIndices),
    .GetIndices = F(dev_GetIndices), .CreatePixelShader = F(dev_CreatePixelShader),
    .SetPixelShader = F(dev_SetPixelShader), .GetPixelShader = F(dev_GetPixelShader),
    .SetPixelShaderConstantF = F(dev_SetPixelShaderConstantF),
    .GetPixelShaderConstantF = F(dev_GetPixelShaderConstantF),
    .SetPixelShaderConstantI = F(dev_SetPixelShaderConstantI),
    .GetPixelShaderConstantI = F(dev_GetPixelShaderConstantI),
    .SetPixelShaderConstantB = F(dev_SetPixelShaderConstantB),
    .GetPixelShaderConstantB = F(dev_GetPixelShaderConstantB), .DrawRectPatch = F(dev_DrawRectPatch),
    .DrawTriPatch = F(dev_DrawTriPatch), .DeletePatch = F(dev_DeletePatch), .CreateQuery = F(dev_CreateQuery),
};

/* ---------- implicit swap chain ---------- */

#define SC_DEV(sc) ((Device *)((BYTE *)(sc) - offsetof(Device, swapchain)))
static HRESULT WINAPI sc_QueryInterface(SwapChain *sc, REFIID r, void **out) { *out = sc; obj_addref(&SC_DEV(sc)->o); return S_OK; }
static ULONG WINAPI sc_AddRef(SwapChain *sc) { return dev_AddRef(SC_DEV(sc)); }
static ULONG WINAPI sc_Release(SwapChain *sc) { return dev_Release(SC_DEV(sc)); }
static HRESULT WINAPI sc_Present(SwapChain *sc, const RECT *s, const RECT *d, HWND w, const RGNDATA *r, DWORD f) { return dev_Present(SC_DEV(sc), s, d, w, r); }
static HRESULT WINAPI sc_GetFrontBufferData(SwapChain *sc, IDirect3DSurface9 *s) { return D3DERR_INVALIDCALL; }
static HRESULT WINAPI sc_GetBackBuffer(SwapChain *sc, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **out) { return dev_GetBackBuffer(SC_DEV(sc), 0, i, t, out); }
static HRESULT WINAPI sc_GetRasterStatus(SwapChain *sc, D3DRASTER_STATUS *r) { return dev_GetRasterStatus(SC_DEV(sc), 0, r); }
static HRESULT WINAPI sc_GetDisplayMode(SwapChain *sc, D3DDISPLAYMODE *m) { return dev_GetDisplayMode(SC_DEV(sc), 0, m); }
static HRESULT WINAPI sc_GetDevice(SwapChain *sc, IDirect3DDevice9 **out) { *out = (IDirect3DDevice9 *)SC_DEV(sc); dev_AddRef(SC_DEV(sc)); return D3D_OK; }
static HRESULT WINAPI sc_GetPresentParameters(SwapChain *sc, D3DPRESENT_PARAMETERS *pp) { *pp = SC_DEV(sc)->pp; return D3D_OK; }
static IDirect3DSwapChain9Vtbl sc_vtbl = {
    F(sc_QueryInterface), F(sc_AddRef), F(sc_Release), F(sc_Present), F(sc_GetFrontBufferData), F(sc_GetBackBuffer),
    F(sc_GetRasterStatus), F(sc_GetDisplayMode), F(sc_GetDevice), F(sc_GetPresentParameters),
};

/* ---------- device creation ---------- */

static void create_targets(Device *d)
{
    D3DPRESENT_PARAMETERS *pp = &d->pp;
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : d->focus;
    d->wnd = w;
    if(!pp->BackBufferWidth || !pp->BackBufferHeight)
    {
        RECT r = { 0, 0, 640, 480 };
        if(w) GetClientRect(w, &r);
        if(!pp->BackBufferWidth) pp->BackBufferWidth = r.right > 0 ? r.right : 1;
        if(!pp->BackBufferHeight) pp->BackBufferHeight = r.bottom > 0 ? r.bottom : 1;
    }
    if(pp->BackBufferFormat == D3DFMT_UNKNOWN || !svga_format(pp->BackBufferFormat)) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if(!pp->BackBufferCount) pp->BackBufferCount = 1;

    if(d->rt0) { surf_Release(d->rt0); d->rt0 = NULL; }
    if(d->ds) { surf_Release(d->ds); d->ds = NULL; }
    if(d->backbuffer) surf_Release(d->backbuffer);
    if(d->auto_depth) { surf_Release(d->auto_depth); d->auto_depth = NULL; }
    LeaveCriticalSection(&lock);
    d->backbuffer = surface_new(d, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
        D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, 1);
    if(pp->EnableAutoDepthStencil)
        d->auto_depth = surface_new(d, pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat,
            D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 1);
    EnterCriticalSection(&lock);
    d->rt0 = d->backbuffer; surf_AddRef(d->rt0);
    set_rendertarget(d, RT_COLOR0, d->rt0);
    d->ds = d->auto_depth;
    if(d->ds) surf_AddRef(d->ds);
    set_rendertarget(d, RT_DEPTH, d->ds);
    set_rendertarget(d, RT_STENCIL, d->ds);
}

/* Direct3D 9's default state, sent to the host. */
static void reset_state(Device *d)
{
    State *s = &d->s;
    DWORD *rs = s->rs;
    memset(rs, 0, sizeof s->rs);
    rs[D3DRS_ZENABLE] = d->pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID; rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD; rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_LASTPIXEL] = TRUE; rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW; rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_FOGEND] = fbits(1); rs[D3DRS_FOGDENSITY] = fbits(1);
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS; rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF; rs[D3DRS_CLIPPING] = TRUE; rs[D3DRS_LIGHTING] = TRUE;
    rs[D3DRS_COLORVERTEX] = TRUE; rs[D3DRS_LOCALVIEWER] = TRUE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1; rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_POINTSIZE] = fbits(1); rs[D3DRS_POINTSIZE_MIN] = fbits(1); rs[D3DRS_POINTSCALE_A] = fbits(1);
    rs[D3DRS_POINTSIZE_MAX] = fbits(64); rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE; rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] =
        rs[D3DRS_COLORWRITEENABLE3] = 15;
    rs[D3DRS_BLENDOP] = rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD; rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS; rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    for(DWORD i = 1; i < 256; i++) apply_rs(d, i);
    for(DWORD st = 0; st < MAX_STAGES; st++)
    {
        DWORD *t = s->tss[st];
        memset(t, 0, sizeof s->tss[st]);
        t[D3DTSS_COLOROP] = st ? D3DTOP_DISABLE : D3DTOP_MODULATE; t[D3DTSS_ALPHAOP] = st ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_COLORARG0] = t[D3DTSS_ALPHAARG0] = t[D3DTSS_RESULTARG] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = st;
        for(DWORD k = 1; k <= 32; k++) apply_tss(d, st, k);
    }
    for(DWORD sm = 0; sm < MAX_SAMPLERS; sm++)
    {
        DWORD *t = s->samp[sm];
        memset(t, 0, sizeof s->samp[sm]);
        t[D3DSAMP_ADDRESSU] = t[D3DSAMP_ADDRESSV] = t[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        t[D3DSAMP_MAGFILTER] = t[D3DSAMP_MINFILTER] = D3DTEXF_POINT; t[D3DSAMP_MAXANISOTROPY] = 1;
        for(DWORD k = 1; k <= 13; k++) apply_samp(d, sm, k);
        if(sm < MAX_STAGES) apply_texture(d, sm);
    }
    D3DMATRIX id = { { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } } };
    for(DWORD i = 0; i < 512; i++) s->transform[i] = id;
    apply_transform(d, D3DTS_WORLD); apply_transform(d, D3DTS_VIEW); apply_transform(d, D3DTS_PROJECTION);
    memset(&s->material, 0, sizeof s->material);
    apply_material(d);
    s->viewport.X = s->viewport.Y = 0; s->viewport.Width = d->rt0->w; s->viewport.Height = d->rt0->h;
    s->viewport.MinZ = 0; s->viewport.MaxZ = 1;
    apply_viewport(d);
    SetRect(&s->scissor, 0, 0, d->rt0->w, d->rt0->h);
    apply_scissor(d);
    apply_shader(d, SHADER_VS); apply_shader(d, SHADER_PS);
}

static HRESULT create_device(IDirect3D9 *d3d, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    Device *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    d->o.vtbl = &dev_vtbl; d->o.ref = 1; d->o.dev = d;
    d->swapchain.o.vtbl = &sc_vtbl; d->swapchain.o.dev = d;
    d->d3d = d3d; IDirect3D9_AddRef(d3d);
    d->focus = focus; d->behavior = flags; d->pp = *pp;
    d->cid = new_id();
    EnterCriticalSection(&lock);
    *cmd(CMD_CONTEXT_DEFINE, 4) = d->cid;
    create_targets(d);
    *pp = d->pp;
    reset_state(d);
    hlog("CreateDevice: %ux%u fmt %u depth %u window %p", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->BackBufferFormat, pp->EnableAutoDepthStencil ? pp->AutoDepthStencilFormat : 0, d->wnd);
    flush();
    LeaveCriticalSection(&lock);
    *out = (IDirect3DDevice9 *)d;
    return D3D_OK;
}

/* ---------- IDirect3D9 ---------- */

typedef struct { Obj o; } D3D9;

static ULONG WINAPI d3d_Release(D3D9 *x) { return InterlockedDecrement(&x->o.ref); }
static HRESULT WINAPI d3d_RegisterSoftwareDevice(D3D9 *x, void *f) { return D3D_OK; }
static UINT WINAPI d3d_GetAdapterCount(D3D9 *x) { return 1; }
static HRESULT WINAPI d3d_GetAdapterIdentifier(D3D9 *x, UINT a, DWORD f, D3DADAPTER_IDENTIFIER9 *id)
{
    if(a) return D3DERR_INVALIDCALL;
    memset(id, 0, sizeof *id);
    strcpy(id->Driver, "d3d9fwd.dll");
    strcpy(id->Description, "Webulator Direct3D 9 forwarding (WebGL2)");
    strcpy(id->DeviceName, "\\\\.\\DISPLAY1");
    id->DriverVersion.QuadPart = 0x0006000E00010000LL;
    id->VendorId = 0x15AD; id->DeviceId = 0x0405; id->WHQLLevel = 1;
    return D3D_OK;
}
static int mode_list(D3DFORMAT f, D3DDISPLAYMODE *out, UINT want)
{
    UINT bpp = f == D3DFMT_R5G6B5 ? 16 : f == D3DFMT_X8R8G8B8 ? 32 : 0, n = 0;
    if(!bpp) return 0;
    DEVMODEA dm = { .dmSize = sizeof dm };
    for(DWORD i = 0; EnumDisplaySettingsA(0, i, &dm); i++)
    {
        if(dm.dmBitsPerPel != bpp) continue;
        if(out && n == want)
        {
            out->Width = dm.dmPelsWidth; out->Height = dm.dmPelsHeight; out->RefreshRate = dm.dmDisplayFrequency;
            out->Format = f; return 1;
        }
        n++;
    }
    return out ? 0 : n;
}
static UINT WINAPI d3d_GetAdapterModeCount(D3D9 *x, UINT a, D3DFORMAT f) { return mode_list(f, 0, 0); }
static HRESULT WINAPI d3d_EnumAdapterModes(D3D9 *x, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *m)
{
    return mode_list(f, m, i) ? D3D_OK : D3DERR_INVALIDCALL;
}
static HRESULT WINAPI d3d_GetAdapterDisplayMode(D3D9 *x, UINT a, D3DDISPLAYMODE *m) { return dev_GetDisplayMode(0, 0, m); }
static HRESULT WINAPI d3d_CheckDeviceType(D3D9 *x, UINT a, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf, BOOL win)
{
    if(t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return df == D3DFMT_X8R8G8B8 || df == D3DFMT_R5G6B5 ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT WINAPI d3d_CheckDeviceFormat(D3D9 *x, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f)
{
    if(rt == D3DRTYPE_CUBETEXTURE || rt == D3DRTYPE_VOLUMETEXTURE || rt == D3DRTYPE_VOLUME) return D3DERR_NOTAVAILABLE;
    if(f == D3DFMT_INDEX16 || f == D3DFMT_INDEX32 || f == D3DFMT_VERTEXDATA) return D3D_OK;
    if(!svga_format(f)) return D3DERR_NOTAVAILABLE;
    if(usage & D3DUSAGE_DEPTHSTENCIL) return is_depth(f) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if(is_depth(f)) return D3DERR_NOTAVAILABLE;
    if(usage & D3DUSAGE_RENDERTARGET)
        return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 || f == D3DFMT_R5G6B5 ? D3D_OK : D3DERR_NOTAVAILABLE;
    if(usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE | D3DUSAGE_QUERY_VERTEXTEXTURE |
        D3DUSAGE_QUERY_LEGACYBUMPMAP)) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}
static HRESULT WINAPI d3d_CheckDeviceMultiSampleType(D3D9 *x, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w,
    D3DMULTISAMPLE_TYPE ms, DWORD *q)
{
    if(q) *q = 1;
    return ms == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}
static HRESULT WINAPI d3d_CheckDepthStencilMatch(D3D9 *x, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) { return D3D_OK; }
static HRESULT WINAPI d3d_CheckDeviceFormatConversion(D3D9 *x, UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) { return D3D_OK; }

static void fill_caps(D3DCAPS9 *c)
{
    memset(c, 0, sizeof *c);
    c->DeviceType = D3DDEVTYPE_HAL;
    c->Caps2 = D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_DYNAMICTEXTURES;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c->CursorCaps = D3DCURSORCAPS_COLOR;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
        D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
        D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
        D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
        D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_CANBLTSYSTONONLOCAL;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
        D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_SEPARATEALPHABLEND;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
        D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS |
        D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_FOGRANGE;
    c->ZCmpCaps = c->AlphaCmpCaps = 0xFF;
    c->SrcBlendCaps = c->DestBlendCaps = 0x3FFF;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
        D3DPSHADECAPS_FOGGOURAUD;
    c->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP |
        D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;
    c->TextureFilterCaps = c->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
        D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
        D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
        D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c->MaxTextureWidth = c->MaxTextureHeight = 4096;
    c->MaxTextureRepeat = 8192; c->MaxTextureAspectRatio = 4096; c->MaxAnisotropy = 16; c->MaxVertexW = 1e10f;
    c->GuardBandLeft = c->GuardBandTop = -32768; c->GuardBandRight = c->GuardBandBottom = 32768;
    c->StencilCaps = 0x1FF | D3DSTENCILCAPS_TWOSIDED;
    c->FVFCaps = 8 | D3DFVFCAPS_PSIZE;
    c->TextureOpCaps = 0x03FFFFFF & ~(D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE);
    c->MaxTextureBlendStages = c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
        D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    c->MaxActiveLights = 8; c->MaxUserClipPlanes = 0; c->MaxVertexBlendMatrices = 0;
    c->MaxPointSize = 64; c->MaxPrimitiveCount = 0xFFFFF; c->MaxVertexIndex = 0xFFFFFF;
    c->MaxStreams = MAX_STREAMS; c->MaxStreamStride = 1024;
    c->VertexShaderVersion = D3DVS_VERSION(2, 0); c->MaxVertexShaderConst = 256;
    c->PixelShaderVersion = D3DPS_VERSION(2, 0); c->PixelShader1xMaxValue = 8;
    c->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET;
    c->NumberOfAdaptersInGroup = 1;
    c->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
        D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    c->NumSimultaneousRTs = 1;
    c->VS20Caps.NumTemps = 32; c->VS20Caps.StaticFlowControlDepth = 4; c->VS20Caps.DynamicFlowControlDepth = 0;
    c->PS20Caps.NumTemps = 32; c->PS20Caps.NumInstructionSlots = 512;
    c->MaxVShaderInstructionsExecuted = c->MaxPShaderInstructionsExecuted = 65535;
    c->MaxVertexShader30InstructionSlots = c->MaxPixelShader30InstructionSlots = 0;
}
static HRESULT WINAPI d3d_GetDeviceCaps(D3D9 *x, UINT a, D3DDEVTYPE t, D3DCAPS9 *c)
{
    if(t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    fill_caps(c); return D3D_OK;
}
static HMONITOR WINAPI d3d_GetAdapterMonitor(D3D9 *x, UINT a) { POINT p = { 0, 0 }; return MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY); }
static HRESULT WINAPI d3d_CreateDevice(D3D9 *x, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags,
    D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    if(t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    return create_device((IDirect3D9 *)x, focus, flags, pp, out);
}
static IDirect3D9Vtbl d3d_vtbl = {
    .QueryInterface = F(obj_QueryInterface), .AddRef = F(com_AddRef), .Release = F(d3d_Release),
    .RegisterSoftwareDevice = F(d3d_RegisterSoftwareDevice), .GetAdapterCount = F(d3d_GetAdapterCount),
    .GetAdapterIdentifier = F(d3d_GetAdapterIdentifier), .GetAdapterModeCount = F(d3d_GetAdapterModeCount),
    .EnumAdapterModes = F(d3d_EnumAdapterModes), .GetAdapterDisplayMode = F(d3d_GetAdapterDisplayMode),
    .CheckDeviceType = F(d3d_CheckDeviceType), .CheckDeviceFormat = F(d3d_CheckDeviceFormat),
    .CheckDeviceMultiSampleType = F(d3d_CheckDeviceMultiSampleType),
    .CheckDepthStencilMatch = F(d3d_CheckDepthStencilMatch),
    .CheckDeviceFormatConversion = F(d3d_CheckDeviceFormatConversion), .GetDeviceCaps = F(d3d_GetDeviceCaps),
    .GetAdapterMonitor = F(d3d_GetAdapterMonitor), .CreateDevice = F(d3d_CreateDevice),
};

static D3D9 the_d3d = { { &d3d_vtbl, 1, NULL } };

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    if(!hc_init()) return NULL;
    obj_addref(&the_d3d.o);
    return (IDirect3D9 *)&the_d3d;
}

/* D3DPERF stubs (d3d9.dll exports these; some applications import them). */
__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o) {}
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
