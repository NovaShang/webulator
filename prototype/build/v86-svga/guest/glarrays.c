/* glarrays: one quad per cell, each drawn with a different vertex-array setup, to compare OpenGL runtimes.
 * Row 1: separate arrays + glDrawElements(GL_UNSIGNED_INT) + glLockArraysEXT | interleaved + GL_BGRA colours
 *        | constant colour (no colour array) | glDrawArrays
 * Row 2: GL_UNSIGNED_SHORT indices starting at vertex 4 | GL_UNSIGNED_BYTE indices | two texture units
 *        | glBegin/glEnd
 * Row 3 (buffer objects): interleaved RGBA | interleaved GL_BGRA | element buffer, 16-bit, indices from vertex 4
 *        | interleaved, two texture units, element buffer
 * Every cell should show a quad: red top-left corner, green top-right, blue bottom-right, white bottom-left. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>

typedef void (APIENTRY *LOCK)(GLint, GLsizei);
typedef void (APIENTRY *UNLOCK)(void);
typedef void (APIENTRY *ACTIVE)(GLenum);
typedef void (APIENTRY *GENB)(GLsizei, GLuint *);
typedef void (APIENTRY *BINDB)(GLenum, GLuint);
typedef void (APIENTRY *DATAB)(GLenum, GLsizeiptr, const void *, GLenum);
typedef struct { float x, y, z, nx, ny, nz; unsigned char c[4]; float u, v, u2, v2; } V2;   /* like S3DVertex2TCoords */

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, a, b);
}

typedef struct { float x, y, z; unsigned char bgra[4]; float u, v; } IV;

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = { CS_OWNDC, proc, 0, 0, inst, 0, LoadCursor(0, IDC_ARROW), 0, 0, "glarrays" };
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("glarrays", "glarrays", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 528, 430, 0, 0, inst, 0);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0,0,0,0,0,0, 0,0, 0,0,0,0,0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0,0,0 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
    LOCK lock = (LOCK)wglGetProcAddress("glLockArraysEXT");
    UNLOCK unlock = (UNLOCK)wglGetProcAddress("glUnlockArraysEXT");
    ACTIVE client = (ACTIVE)wglGetProcAddress("glClientActiveTextureARB");
    ACTIVE active = (ACTIVE)wglGetProcAddress("glActiveTextureARB");
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    int bgra = ext && strstr(ext, "GL_EXT_vertex_array_bgra") != NULL;
    GENB genb = (GENB)wglGetProcAddress("glGenBuffersARB");
    BINDB bindb = (BINDB)wglGetProcAddress("glBindBufferARB");
    DATAB datab = (DATAB)wglGetProcAddress("glBufferDataARB");

    /* white 2×2 texture */
    static unsigned char white[16] = { 255,255,255,255, 255,255,255,255, 255,255,255,255, 255,255,255,255 };
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    /* corners: top-left, top-right, bottom-right, bottom-left (y up) */
    static const float P[4][3] = { {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}, {-1, -1, 0} };
    static const unsigned char RGBA[4][4] = { {255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255} };
    static const float UV[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    float xyz[8][4]; unsigned char col[8][4]; float uv[8][2];
    for (int i = 0; i < 8; i++) { int k = i & 3; xyz[i][0] = P[k][0]; xyz[i][1] = P[k][1]; xyz[i][2] = 0; xyz[i][3] = 1;
        memcpy(col[i], RGBA[k], 4); uv[i][0] = UV[k][0]; uv[i][1] = UV[k][1]; }
    IV iv[4];
    for (int k = 0; k < 4; k++) { iv[k].x = P[k][0]; iv[k].y = P[k][1]; iv[k].z = 0;
        iv[k].bgra[0] = RGBA[k][2]; iv[k].bgra[1] = RGBA[k][1]; iv[k].bgra[2] = RGBA[k][0]; iv[k].bgra[3] = 255;
        iv[k].u = UV[k][0]; iv[k].v = UV[k][1]; }
    static const GLuint i32[6] = { 0, 1, 2, 0, 2, 3 };
    /* buffer objects: 8 vertices (second four are the ones drawn by the offset case), RGBA and BGRA copies */
    V2 v2[2][8];
    for (int b = 0; b < 2; b++) for (int i = 0; i < 8; i++) { int k = i & 3; V2 *q = &v2[b][i];
        q->x = P[k][0]; q->y = P[k][1]; q->z = 0; q->nx = q->ny = 0; q->nz = 1;
        if (b) { q->c[0] = RGBA[k][2]; q->c[1] = RGBA[k][1]; q->c[2] = RGBA[k][0]; q->c[3] = 255; }
        else memcpy(q->c, RGBA[k], 4);
        q->u = q->u2 = UV[k][0]; q->v = q->v2 = UV[k][1]; }
    GLuint vbo[3] = { 0, 0, 0 };
    static const GLushort e16[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    if (genb && bindb && datab) {
        genb(3, vbo);
        bindb(GL_ARRAY_BUFFER_ARB, vbo[0]); datab(GL_ARRAY_BUFFER_ARB, sizeof v2[0], v2[0], GL_STATIC_DRAW_ARB);
        bindb(GL_ARRAY_BUFFER_ARB, vbo[1]); datab(GL_ARRAY_BUFFER_ARB, sizeof v2[1], v2[1], GL_STATIC_DRAW_ARB);
        bindb(GL_ELEMENT_ARRAY_BUFFER_ARB, vbo[2]); datab(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof e16, e16, GL_STATIC_DRAW_ARB);
        bindb(GL_ARRAY_BUFFER_ARB, 0); bindb(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
    }
    static const GLushort i16[6] = { 4, 5, 6, 4, 6, 7 };
    static const GLubyte i8[6] = { 0, 1, 2, 0, 2, 3 };

    MSG msg;
    DWORD start = GetTickCount();
    for (;;) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return 0; DispatchMessageA(&msg); }
        if (GetTickCount() - start > 120000) return 0;
        glClearColor(0.2f, 0.2f, 0.2f, 1); glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 3, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glEnable(GL_TEXTURE_2D);
        for (int cell = 0; cell < 12; cell++) {
            glLoadIdentity(); glTranslatef(cell % 4 + 0.5f, 2.5f - cell / 4, 0); glScalef(0.4f, 0.4f, 1);
            glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glColor4f(1, 1, 1, 1);
            switch (cell) {
            case 0:
                glVertexPointer(3, GL_FLOAT, 16, xyz); glEnableClientState(GL_VERTEX_ARRAY);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, col); glEnableClientState(GL_COLOR_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, 0, uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                if (lock) lock(0, 4);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, i32);
                if (unlock) unlock();
                break;
            case 1:
                glVertexPointer(3, GL_FLOAT, sizeof(IV), &iv[0].x);
                if (bgra) glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, sizeof(IV), iv[0].bgra);
                else glColorPointer(4, GL_UNSIGNED_BYTE, 0, col);
                glEnableClientState(GL_COLOR_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, sizeof(IV), &iv[0].u); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const GLushort[]){ 0, 1, 2, 0, 2, 3 });
                break;
            case 2:
                glVertexPointer(3, GL_FLOAT, 16, xyz);
                glColor4f(1, 0.5f, 0, 1);
                glTexCoordPointer(2, GL_FLOAT, 0, uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, i32);
                break;
            case 3:
                glVertexPointer(3, GL_FLOAT, 16, xyz);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, col); glEnableClientState(GL_COLOR_ARRAY);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                break;
            case 4:
                glVertexPointer(3, GL_FLOAT, 16, xyz);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, col); glEnableClientState(GL_COLOR_ARRAY);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, i16);
                break;
            case 5:
                glVertexPointer(3, GL_FLOAT, 16, xyz);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, col); glEnableClientState(GL_COLOR_ARRAY);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, i8);
                break;
            case 6:
                glVertexPointer(3, GL_FLOAT, 16, xyz);
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, col); glEnableClientState(GL_COLOR_ARRAY);
                if (client && active) {
                    active(GL_TEXTURE1); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, t);
                    client(GL_TEXTURE1); glTexCoordPointer(2, GL_FLOAT, 0, uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    client(GL_TEXTURE0); active(GL_TEXTURE0);
                }
                glTexCoordPointer(2, GL_FLOAT, 0, uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, i32);
                if (client && active) {
                    active(GL_TEXTURE1); glDisable(GL_TEXTURE_2D); client(GL_TEXTURE1);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY); client(GL_TEXTURE0); active(GL_TEXTURE0);
                }
                break;
            case 8: case 9: case 10: case 11:
            {
                if (!vbo[0]) break;
                int b = cell == 9 && bgra;
                bindb(GL_ARRAY_BUFFER_ARB, vbo[b]);
                glVertexPointer(3, GL_FLOAT, sizeof(V2), (void *)0);
                glColorPointer(b ? GL_BGRA : 4, GL_UNSIGNED_BYTE, sizeof(V2), (void *)24); glEnableClientState(GL_COLOR_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, sizeof(V2), (void *)28); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                if (cell == 11 && client && active) {
                    active(GL_TEXTURE1); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, t);
                    client(GL_TEXTURE1); glTexCoordPointer(2, GL_FLOAT, sizeof(V2), (void *)36); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    client(GL_TEXTURE0); active(GL_TEXTURE0);
                }
                if (cell >= 10) {
                    bindb(GL_ELEMENT_ARRAY_BUFFER_ARB, vbo[2]);
                    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)(cell == 10 ? 12 : 0));
                    bindb(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
                } else glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
                if (cell == 11 && client && active) {
                    active(GL_TEXTURE1); glDisable(GL_TEXTURE_2D); client(GL_TEXTURE1);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY); client(GL_TEXTURE0); active(GL_TEXTURE0);
                }
                bindb(GL_ARRAY_BUFFER_ARB, 0);
                break;
            }
            case 7:
                glBegin(GL_QUADS);
                for (int k = 0; k < 4; k++) { glColor4ubv(RGBA[k]); glTexCoord2fv(UV[k]); glVertex3fv(P[k]); }
                glEnd();
                break;
            }
        }
        SwapBuffers(dc);
        Sleep(10);
    }
}
