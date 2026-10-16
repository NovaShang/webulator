/* hc.h: the hypercall transport and SVGA3D command helpers shared by the forwarding runtimes (d3d9fwd.c, glfwd.c).
 * Each runtime is a single translation unit that includes this once. */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ---------- SVGA3D protocol ---------- */

enum {
    CMD_SURFACE_DEFINE = 1040, CMD_SURFACE_DESTROY = 1041, CMD_SURFACE_COPY = 1042, CMD_SURFACE_STRETCHBLT = 1043,
    CMD_SURFACE_DMA = 1044, CMD_CONTEXT_DEFINE = 1045, CMD_CONTEXT_DESTROY = 1046, CMD_SETTRANSFORM = 1047,
    CMD_SETZRANGE = 1048, CMD_SETRENDERSTATE = 1049, CMD_SETRENDERTARGET = 1050, CMD_SETTEXTURESTATE = 1051,
    CMD_SETMATERIAL = 1052, CMD_SETLIGHTDATA = 1053, CMD_SETLIGHTENABLED = 1054, CMD_SETVIEWPORT = 1055,
    CMD_CLEAR = 1057, CMD_PRESENT = 1058, CMD_SHADER_DEFINE = 1059, CMD_SHADER_DESTROY = 1060, CMD_SET_SHADER = 1061,
    CMD_SET_SHADER_CONST = 1062, CMD_DRAW_PRIMITIVES = 1063, CMD_SETSCISSORRECT = 1064, CMD_GENERATE_MIPMAPS = 1071,
    CMD_LOG = 0x7F000001, CMD_SURFACE_DMA_FORMAT = 0x7F000002,
};
enum { GMR_LINEAR = 0xFFFFFFFD, INVALID_ID = 0xFFFFFFFF };
/* Surface flag: scratch buffer written in small pieces, each used right after it is written (see the renderer). */
#define SURFACE_STREAM 0x80000000u
enum { XFER_TO_HOST = 1, XFER_FROM_HOST = 2 };
enum { RT_DEPTH = 0, RT_STENCIL = 1, RT_COLOR0 = 2 };
enum { SHADER_VS = 1, SHADER_PS = 2 };
enum {   /* SVGA3dSurfaceFormat */
    SF_X8R8G8B8 = 1, SF_A8R8G8B8 = 2, SF_R5G6B5 = 3, SF_X1R5G5B5 = 4, SF_A1R5G5B5 = 5, SF_A4R4G4B4 = 6, SF_Z_D32 = 7,
    SF_Z_D16 = 8, SF_Z_D24S8 = 9, SF_Z_D15S1 = 10, SF_LUMINANCE8 = 11, SF_LUMINANCE4_ALPHA4 = 12, SF_LUMINANCE16 = 13,
    SF_LUMINANCE8_ALPHA8 = 14, SF_DXT1 = 15, SF_DXT2 = 16, SF_DXT3 = 17, SF_DXT4 = 18, SF_DXT5 = 19, SF_ALPHA8 = 32,
    SF_BUFFER = 37, SF_Z_D24X8 = 38,
};
/* SVGA3dRenderStateName / SVGA3dTextureStateName (only the ones we send) */
enum {
    RS_ZENABLE = 1, RS_ZWRITEENABLE = 2, RS_ALPHATESTENABLE = 3, RS_DITHERENABLE = 4, RS_BLENDENABLE = 5,
    RS_FOGENABLE = 6, RS_SPECULARENABLE = 7, RS_STENCILENABLE = 8, RS_LIGHTINGENABLE = 9, RS_NORMALIZENORMALS = 10,
    RS_POINTSPRITEENABLE = 11, RS_POINTSCALEENABLE = 12, RS_STENCILREF = 13, RS_STENCILMASK = 14,
    RS_STENCILWRITEMASK = 15, RS_FOGSTART = 16, RS_FOGEND = 17, RS_FOGDENSITY = 18, RS_POINTSIZE = 19,
    RS_POINTSIZEMIN = 20, RS_POINTSIZEMAX = 21, RS_POINTSCALE_A = 22, RS_POINTSCALE_B = 23, RS_POINTSCALE_C = 24,
    RS_FOGCOLOR = 25, RS_AMBIENT = 26, RS_CLIPPLANEENABLE = 27, RS_FOGMODE = 28, RS_FILLMODE = 29, RS_SHADEMODE = 30,
    RS_SRCBLEND = 32, RS_DSTBLEND = 33, RS_BLENDEQUATION = 34, RS_CULLMODE = 35, RS_ZFUNC = 36, RS_ALPHAFUNC = 37,
    RS_STENCILFUNC = 38, RS_STENCILFAIL = 39, RS_STENCILZFAIL = 40, RS_STENCILPASS = 41, RS_ALPHAREF = 42,
    RS_FRONTWINDING = 43, RS_RANGEFOGENABLE = 46, RS_COLORWRITEENABLE = 47, RS_VERTEXMATERIALENABLE = 48,
    RS_DIFFUSEMATERIALSOURCE = 49, RS_SPECULARMATERIALSOURCE = 50, RS_AMBIENTMATERIALSOURCE = 51,
    RS_EMISSIVEMATERIALSOURCE = 52, RS_TEXTUREFACTOR = 53, RS_LOCALVIEWER = 54, RS_SCISSORTESTENABLE = 55,
    RS_BLENDCOLOR = 56, RS_STENCILENABLE2SIDED = 57, RS_CCWSTENCILFUNC = 58, RS_CCWSTENCILFAIL = 59,
    RS_CCWSTENCILZFAIL = 60, RS_CCWSTENCILPASS = 61, RS_VERTEXBLEND = 62, RS_SLOPESCALEDEPTHBIAS = 63,
    RS_DEPTHBIAS = 64, RS_LASTPIXEL = 67, RS_CLIPPING = 68, RS_MULTISAMPLEANTIALIAS = 85, RS_MULTISAMPLEMASK = 86,
    RS_ANTIALIASEDLINEENABLE = 89, RS_COLORWRITEENABLE1 = 90, RS_COLORWRITEENABLE2 = 91, RS_COLORWRITEENABLE3 = 92,
    RS_SEPARATEALPHABLENDENABLE = 93, RS_SRCBLENDALPHA = 94, RS_DSTBLENDALPHA = 95, RS_BLENDEQUATIONALPHA = 96,
};
enum {
    TS_BIND_TEXTURE = 1, TS_COLOROP = 2, TS_COLORARG1 = 3, TS_COLORARG2 = 4, TS_ALPHAOP = 5, TS_ALPHAARG1 = 6,
    TS_ALPHAARG2 = 7, TS_ADDRESSU = 8, TS_ADDRESSV = 9, TS_MIPFILTER = 10, TS_MAGFILTER = 11, TS_MINFILTER = 12,
    TS_BORDERCOLOR = 13, TS_TEXCOORDINDEX = 14, TS_TEXTURETRANSFORMFLAGS = 15, TS_TEXCOORDGEN = 16,
    TS_BUMPENVMAT00 = 17, TS_TEXTURE_MIPMAP_LEVEL = 21, TS_TEXTURE_LOD_BIAS = 22, TS_TEXTURE_ANISOTROPIC_LEVEL = 23,
    TS_ADDRESSW = 24, TS_BUMPENVLSCALE = 26, TS_BUMPENVLOFFSET = 27, TS_COLORARG0 = 28, TS_ALPHAARG0 = 29,
};

/* ---------- hypercall transport ---------- */

#define HC_PORT 0x5680
#define HC_MAGIC 0x44334657
#define CMDBUF_WORDS (1 << 20)

typedef struct { volatile DWORD magic, addr, len, status, fault, done; } HC;
static HC hc __attribute__((aligned(64)));
static DWORD *cmdbuf;
static DWORD cmdpos;                 /* in words */
static CRITICAL_SECTION lock;
static int host_ok = -1;
static FILE *logfile;

static void out_port(DWORD v)
{
    __asm__ volatile("outl %0, %w1" : : "a"(v), "Nd"(HC_PORT) : "memory");
}

static void flush(void)
{
    DWORD off = 0, used = cmdpos * 4, last_fault = 0, writes = 0;
    while(off < used)
    {
        hc.magic = HC_MAGIC; hc.addr = (DWORD)cmdbuf + off; hc.len = used - off; hc.status = 0xFFFFFFFF;
        hc.fault = 0; hc.done = 0;
        out_port((DWORD)&hc);
        if(hc.status == 0) break;
        if(hc.status != 1) { host_ok = 0; break; }          /* nobody answered */
        /* A page the emulator could not read or write: fault it in and resubmit the rest. */
        volatile BYTE *p = (volatile BYTE *)hc.fault;
        if(hc.fault == last_fault) { if(++writes > 2) break; BYTE v = *p; *p = v; }
        else { BYTE v = *p; (void)v; writes = 0; }
        last_fault = hc.fault;
        off += hc.done;
    }
    cmdpos = 0;
}

/* Reserve a command of `bytes` body bytes; returns the body. */
static DWORD *cmd(DWORD id, DWORD bytes)
{
    DWORD words = 2 + (bytes + 3) / 4;
    if(cmdpos + words > CMDBUF_WORDS) flush();
    DWORD *p = cmdbuf + cmdpos;
    p[0] = id; p[1] = (bytes + 3) & ~3;
    cmdpos += words;
    return p + 2;
}

static void hlog(const char *fmt, ...)
{
    char t[512];
    va_list a; va_start(a, fmt); int n = vsnprintf(t, sizeof t, fmt, a); va_end(a);
    if(n < 0) return;
    if(n >= (int)sizeof t) n = sizeof t - 1;
    EnterCriticalSection(&lock);
    memcpy(cmd(CMD_LOG, n + 1), t, n + 1);
    flush();                                 /* now: the process may be about to crash */
    LeaveCriticalSection(&lock);
    if(logfile) { fputs(t, logfile); fputc('\n', logfile); fflush(logfile); }
}

/* Log crashes (code, address, module, a few stack words) through the hypercall before Windows' dialog. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e);

static int hc_init(void)
{
    if(host_ok >= 0) return host_ok;
    InitializeCriticalSection(&lock);
    cmdbuf = VirtualAlloc(0, CMDBUF_WORDS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(getenv("D3DFWD_LOG")) logfile = fopen(getenv("D3DFWD_LOG"), "w");
    host_ok = 1;
    SetUnhandledExceptionFilter(crash_filter);
    hlog("d3d9fwd: hello from pid %lu", GetCurrentProcessId());
    flush();
    return host_ok;
}

static DWORD next_id = 0x40000000;
static DWORD new_id(void) { return InterlockedIncrement((LONG *)&next_id); }

static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

/* Host-only texel formats (byte orders of OpenGL client data). */
enum { SF_RGBA8 = 0x1001, SF_RGB8 = 0x1002, SF_BGR8 = 0x1003, SF_INTENSITY8 = 0x1004 };

static void surface_define(DWORD sid, DWORD format, UINT w, UINT h, UINT levels)
{
    DWORD *p = cmd(CMD_SURFACE_DEFINE, (9 + levels * 3) * 4);
    p[0] = sid; p[1] = 0; p[2] = format;
    p[3] = levels; p[4] = p[5] = p[6] = p[7] = p[8] = 0;
    for(UINT i = 0; i < levels; i++)
    {
        UINT lw = w >> i, lh = h >> i;
        p[9 + i * 3] = lw ? lw : 1; p[10 + i * 3] = lh ? lh : 1; p[11 + i * 3] = 1;
    }
}
static void surface_destroy(DWORD sid) { *cmd(CMD_SURFACE_DESTROY, 4) = sid; }

/* DMA between guest memory (image at `addr`, rows `pitch` apart) and (sid, mip). The box is at (x, y) on the host
 * surface and (sx, sy) in the guest image; pixels, or bytes for buffers (h = 1). */
static void dma2(DWORD sid, UINT mip, const void *addr, UINT pitch, DWORD transfer, UINT x, UINT y, UINT w, UINT h,
    UINT sx, UINT sy)
{
    DWORD *p = cmd(CMD_SURFACE_DMA, (7 + 9) * 4);
    p[0] = GMR_LINEAR; p[1] = (DWORD)addr; p[2] = pitch; p[3] = sid; p[4] = 0; p[5] = mip; p[6] = transfer;
    p[7] = x; p[8] = y; p[9] = 0; p[10] = w; p[11] = h; p[12] = 1; p[13] = sx; p[14] = sy; p[15] = 0;
}
static void dma(DWORD sid, UINT mip, const void *addr, UINT pitch, DWORD transfer, UINT x, UINT y, UINT w, UINT h)
{
    dma2(sid, mip, addr, pitch, transfer, x, y, w, h, x, y);
}

/* DMA of a guest image whose texel format differs from the surface's. */
static void dma_format(DWORD fmt, DWORD sid, UINT mip, const void *addr, UINT pitch, UINT x, UINT y, UINT w, UINT h,
    UINT sx, UINT sy)
{
    DWORD *p = cmd(CMD_SURFACE_DMA_FORMAT, (1 + 7 + 9) * 4);
    p[0] = fmt;
    p[1] = GMR_LINEAR; p[2] = (DWORD)addr; p[3] = pitch; p[4] = sid; p[5] = 0; p[6] = mip; p[7] = XFER_TO_HOST;
    p[8] = x; p[9] = y; p[10] = 0; p[11] = w; p[12] = h; p[13] = 1; p[14] = sx; p[15] = sy; p[16] = 0;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *x = e->ContextRecord;
    char mod[MAX_PATH] = "?";
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)r->ExceptionAddress, &m);
    if(m) GetModuleFileNameA(m, mod, sizeof mod);
    hlog("crash: code %08lx at %p (%s+0x%lx) access %lu %p", r->ExceptionCode, r->ExceptionAddress, mod,
        m ? (DWORD)r->ExceptionAddress - (DWORD)m : 0, r->NumberParameters > 1 ? r->ExceptionInformation[0] : 0,
        r->NumberParameters > 1 ? (void *)r->ExceptionInformation[1] : NULL);
    DWORD *sp = (DWORD *)x->Esp;
    hlog("crash: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx stack %08lx %08lx %08lx %08lx %08lx %08lx",
        x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5]);
    return EXCEPTION_CONTINUE_SEARCH;
}
