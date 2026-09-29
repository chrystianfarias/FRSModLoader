#include "ui/WorldDraw.h"

#include "core/Hook.h"
#include "core/Log.h"

#include <string.h>
#include <windows.h>

#include <math.h>
#include <vector>

// No D3DX: its SDK was discontinued and is not on this machine. The maths
// here is small, and avoiding the dependency keeps the build simple.
namespace
{
    struct Vec3
    {
        float x, y, z;
        Vec3() : x(0), y(0), z(0) {}
        Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
        Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
        Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
        Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
    };
}

namespace
{
    struct Particle
    {
        Vec3 pos;          // in the world, or on the car when attached
        Vec3 vel;          // in the same frame of reference as the position
        bool attached;
        float size;
        float life;       // remaining, in seconds
        float totalLife;
        DWORD colour;     // at birth
        DWORD endColour;  // at death; the colour moves from one to the other
    };

    // Blends two colours channel by channel. This is what lets the flame come
    // out bright and end up bluish, like the game's own backfire - hot burning
    // gas starts white and cools to blue, and a fixed colour never imitates that.
    DWORD Blend(DWORD a, DWORD b, float k)
    {
        if (k < 0) k = 0;
        if (k > 1) k = 1;

        DWORD out = 0;
        for (int i = 0; i < 3; i++)
        {
            int shift = i * 8;
            float ca = (float)((a >> shift) & 0xFF);
            float cb = (float)((b >> shift) & 0xFF);
            out |= ((DWORD)(ca + (cb - ca) * k) & 0xFF) << shift;
        }
        return out;
    }

    struct Vertex
    {
        float x, y, z;
        DWORD colour;
        float u, v;
    };
    const DWORD FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    std::vector<Particle> g_particles;
    IDirect3DTexture9* g_texture = 0;

    // The game's own texture, when the mod points at one. The procedural blob
    // stays around as a fallback: if the pointer is no longer valid (device
    // change, resource unloaded), we fall back to it instead of losing the
    // flame.
    IDirect3DTexture9* g_gameTexture = 0;

    // WHERE the pointer lives, not the pointer.
    //
    // Keeping the pointer works until you leave the event: the game unloads
    // its resources and reloads them later, and the old address ends up
    // pointing at freed memory - hence the "broken" texture on the way back.
    // Keeping the field that holds the pointer (info+0x18) means just
    // re-reading it: when the game swaps the texture, ours swaps with it, and
    // when it unloads, validation fails and we drop to our own blob instead of
    // drawing garbage.
    uintptr_t g_textureSource = 0;
    uintptr_t g_lastPointer = 0;

    // The game texture is a 2x2 sprite sheet: four frames of an animation, not
    // a single image. Drawing it whole shows all four at once; the right thing
    // is to step through the frames over the particle's life, which is how a
    // puff of fire behaves - it is born, grows, falls apart.
    int g_columns = 1;
    int g_rows = 1;

    // The car's frame of reference, updated every frame by the mod.
    //
    // A particle loose in the world is left behind when the car moves - at 100
    // km/h, a quarter of a second of life turns into seven metres of trail.
    // Exhaust flame does not behave like that: it follows the car. So an
    // attached particle keeps its position IN CAR COORDINATES and only becomes
    // world space at draw time, with that frame's frame of reference.
    float g_anchorPos[3] = { 0, 0, 0 };
    float g_anchorRot[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    bool  g_hasAnchor = false;
    bool g_warnedFailure = false;

    // The camera, as told from outside. See WorldDraw::Camera.
    uintptr_t g_viewAddress = 0;
    float g_fovY  = 60.0f;
    float g_near = 0.1f;
    float g_far = 3000.0f;
    bool  g_depth = false;
    bool  g_flipY = false;
    bool  g_automatic = true;

    // The matrix the GAME uses, fished out of the vertex shader constants.
    //
    // Rebuilding the lens by hand does not add up: no field of view got it
    // right, a sign that the game's projection is not an ordinary symmetric
    // perspective (racing games often shift the centre to raise the horizon).
    // Guessing that is impossible - but it is not needed: drawing with
    // shaders, the game uploads the combined view+projection matrix as
    // constants, and it can be read from the device and reused whole. No field
    // of view, no flip, no clip planes: the same maths that draws the car
    // draws the flame.
    // Reading the constants in EndScene finds nothing, and the log proved it:
    // 64 read, 0 candidates. It makes sense - the last one to write in the
    // frame is the 2D HUD, which overwrites the scene matrix before we get
    // there. So we do not read at the end: we listen to the upload, hooking
    // SetVertexShaderConstantF, and keep the matrix at the moment the game
    // uses it to draw the world.
    float g_gameMatrix[16];
    bool  g_found = false;
    int   g_register = -1;
    bool  g_transposed = false;
    bool  g_listening = false;

    typedef HRESULT (STDMETHODCALLTYPE* tSetVSConst)(IDirect3DDevice9*, UINT,
                                                     const float*, UINT);
    tSetVSConst g_origSetVSConst = 0;

    // The matrices the game hands to the fixed pipeline.
    //
    // The premise that NFSU2 drew everything with vertex shaders was wrong,
    // and the log proved it two ways: no shader constant is ever uploaded, and
    // the matrices read in EndScene come back as identity. They come back like
    // that because the 2D HUD resets them at the end of the frame - reading
    // there is too late. So we listen to the upload, and keep the PERSPECTIVE
    // projection (the world one, which has the _44 corner zeroed; the HUD one
    // is orthographic and has a 1 there) together with the view that was in
    // effect with it.
    typedef HRESULT (STDMETHODCALLTYPE* tSetTransform)(IDirect3DDevice9*,
                                                       D3DTRANSFORMSTATETYPE,
                                                       const D3DMATRIX*);
    tSetTransform g_origSetTransform = 0;

    D3DMATRIX g_gameView;
    D3DMATRIX g_gameProj;
    bool g_hasGameView = false;
    bool g_hasGameProj = false;

    float g_reference[3];
    bool  g_hasReference = false;
    float g_bestError = 1e9f;

    // Telemetry for the listener. Without it there is no telling "the game
    // sends no matrix at all" from "it sends them, but none passed the test" -
    // and the log went quiet precisely because the ones rejected for a
    // degenerate shape always return the same value and never beat the
    // previous record.
    int g_uploads = 0;
    int g_largeUploads = 0;
    int g_regMin = 9999, g_regMax = -1, g_largestCount = 0;

    bool ReadMatrix(uintptr_t address, D3DMATRIX* out)
    {
        if (!address || IsBadReadPtr((const void*)address, sizeof(D3DMATRIX)))
            return false;
        memcpy(out, (const void*)address, sizeof(D3DMATRIX));

        // A view matrix has its last column (0,0,0,1) and an orthonormal
        // rotation part. Checking the first row is enough to avoid handing
        // garbage to the pipeline if the address changes between sessions: a
        // wrong matrix raises no error at all, it just makes the drawing vanish.
        float n = out->_11 * out->_11 + out->_12 * out->_12 +
                  out->_13 * out->_13;
        return n > 0.9f && n < 1.1f;
    }

    // Multiplies a point (x,y,z,1) by the matrix, in the chosen convention.
    void Project(const float* m, bool transposed, const float* p, float* out)
    {
        for (int i = 0; i < 4; i++)
        {
            const float* col = transposed ? &m[i * 4] : 0;
            out[i] = transposed
                ? (col[0] * p[0] + col[1] * p[1] + col[2] * p[2] + col[3])
                : (m[0 * 4 + i] * p[0] + m[1 * 4 + i] * p[1] +
                   m[2 * 4 + i] * p[2] + m[3 * 4 + i]);
        }
    }

    // Inverts the view matrix. Cheap because it is not just any matrix: the
    // rotation part is orthonormal, and for those the inverse is the transpose.
    void InvertView(const D3DMATRIX& v, float* out)
    {
        const float* m = &v._11;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                out[i * 4 + j] = m[j * 4 + i];

        for (int j = 0; j < 3; j++)
            out[12 + j] = -(m[12] * out[j] + m[13] * out[4 + j] +
                            m[14] * out[8 + j]);

        out[3] = out[7] = out[11] = 0.0f;
        out[15] = 1.0f;
    }

    void Multiply(const float* a, const float* b, float* out)
    {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                float sum = 0;
                for (int k = 0; k < 4; k++) sum += a[i * 4 + k] * b[k * 4 + j];
                out[i * 4 + j] = sum;
            }
    }

    // How far a matrix strays from the shape of a projection.
    //
    // This is the test that replaced the previous one. The old one required
    // the flame to land on screen, and rejected any matrix the game sent with
    // the part's position already baked in - which is the normal case. This
    // one depends on no point at all: knowing the VIEW matrix (found in memory
    // and checked with /altura), one is divided by the other and we look at
    // what is left. If what is left is a real projection, it has an
    // unmistakable shape: almost everything zero off the diagonal, the 1 that
    // does the divide by w, and the corner zeroed. Even if the lens is
    // off-centre, which is exactly what no field of view managed to imitate.
    float ProjectionError(const float* p)
    {
        const int zeros[] = { 1, 2, 3, 4, 6, 7, 12, 13, 15 };
        float error = 0;
        for (int i = 0; i < 9; i++) error += fabsf(p[zeros[i]]);
        error += fabsf(fabsf(p[11]) - 1.0f);

        // And it has to be a plausible lens, not a degenerate matrix that
        // passes the test by being all zeros.
        if (fabsf(p[0]) < 0.2f || fabsf(p[0]) > 10.0f) return 1e9f;
        if (fabsf(p[5]) < 0.2f || fabsf(p[5]) > 10.0f) return 1e9f;
        if (fabsf(p[14]) < 1e-4f) return 1e9f;
        return error;
    }

    bool OnScreen(const float* c)
    {
        if (!(c[3] > 0.05f) || !(c[3] < 1e6f)) return false;
        float x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
        return x > -1.3f && x < 1.3f && y > -1.3f && y < 1.3f &&
               z > 0.0f && z < 1.0f;
    }

    // Every upload of 4 or more vectors is a candidate matrix. The test is the
    // same: with the right matrix, the flame lands on screen. An object matrix
    // (which already carries the part's position) fails, because our reference
    // point is in world coordinates.
    HRESULT STDMETHODCALLTYPE SetVSConstHook(IDirect3DDevice9* device, UINT reg,
                                             const float* data, UINT count)
    {
        HRESULT hr = g_origSetVSConst(device, reg, data, count);

        g_uploads++;
        if (count >= 4)
        {
            g_largeUploads++;
            if ((int)reg < g_regMin) g_regMin = (int)reg;
            if ((int)reg > g_regMax) g_regMax = (int)reg;
            if ((int)count > g_largestCount) g_largestCount = (int)count;
        }

        if (g_uploads == 3000 && !g_found)
            LogGfx("worlddraw: %d shader uploads, %d with 4+ vectors, "
                   "registers c%d..c%d, largest upload %d vectors, best error %.4f",
                   g_uploads, g_largeUploads, g_regMin, g_regMax, g_largestCount,
                   g_bestError >= 1e8f ? -1.0f : g_bestError);

        if (!data || count < 4) return hr;

        D3DMATRIX view;
        if (!ReadMatrix(g_viewAddress, &view)) return hr;

        float inverse[16];
        InvertView(view, inverse);

        for (UINT i = 0; i + 4 <= count; i++)
        {
            for (int t = 0; t < 2; t++)
            {
                float m[16];
                if (t)
                    for (int l = 0; l < 4; l++)
                        for (int col = 0; col < 4; col++)
                            m[l * 4 + col] = data[i * 4 + col * 4 + l];
                else
                    memcpy(m, &data[i * 4], sizeof(m));

                float projection[16];
                Multiply(inverse, m, projection);

                // The threshold was 0.02 and rejected the right matrix by a
                // hair: the log recorded c0 with error 0.0200, stubbornly
                // stable. The leftover is not noise - it comes from the view
                // matrix read from memory being from a slightly different
                // instant than the one the game used, which is exactly the
                // lag we are trying to eliminate. Demanding a perfect shape
                // from an imperfect reference is contradictory; 0.06 accepts
                // the matrix and stays far from the rejected ones, which are
                // off by whole units.
                float error = ProjectionError(projection);
                if (error > 0.06f)
                {
                    if (error < g_bestError && error < 1e8f)
                    {
                        g_bestError = error;
                        LogGfx("worlddraw: close at c%d%s, error %.4f "
                               "[%.3f %.3f | %.3f %.3f | %.3f %.3f]",
                               (int)(reg + i), t ? " transposed" : "", error,
                               projection[0], projection[5], projection[8], projection[9],
                               projection[10], projection[14]);
                    }
                    continue;
                }

                memcpy(g_gameMatrix, m, sizeof(m));
                g_transposed = false;      // already undone above

                if (!g_found)
                {
                    g_found = true;
                    g_register = (int)(reg + i);
                    float fovY = 2.0f * atanf(1.0f / projection[5]) * 57.2957795f;
                    LogGfx("worlddraw: game matrix at c%d%s — fovY %.1f, "
                           "centre (%.3f, %.3f), error %.4f",
                           g_register, t ? " transposed" : "", fovY,
                           projection[8], projection[9], error);
                }
                return hr;
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetTransformHook(IDirect3DDevice9* device,
                                               D3DTRANSFORMSTATETYPE state,
                                               const D3DMATRIX* m)
    {
        // The first uploads, raw: this is what settles once and for all what
        // the game uses and what it does not.
        static int seen = 0;
        if (m && seen < 12)
        {
            seen++;
            LogGfx("worlddraw: SetTransform state %d [%.3f %.3f | %.3f %.3f | "
                   "%.3f %.3f | %.1f %.1f %.1f %.3f]",
                   (int)state, m->_11, m->_22, m->_31, m->_32, m->_33, m->_34,
                   m->_41, m->_42, m->_43, m->_44);
        }

        if (m)
        {
            if (state == D3DTS_VIEW)
            {
                g_gameView = *m;
                g_hasGameView = true;
            }
            else if (state == D3DTS_PROJECTION && m->_44 == 0.0f && m->_34 != 0.0f)
            {
                if (!g_hasGameProj)
                {
                    float fovY = m->_22 != 0 ? 2.0f * atanf(1.0f / m->_22) * 57.2957795f : 0;
                    LogGfx("worlddraw: game projection — fovY %.1f, aspect %.2f, "
                           "centre (%.3f, %.3f), z %.4f/%.2f",
                           fovY, m->_22 / (m->_11 ? m->_11 : 1),
                           m->_31, m->_32, m->_33, m->_43);
                }
                g_gameProj = *m;
                g_hasGameProj = true;
            }
        }
        return g_origSetTransform(device, state, m);
    }

    // The hook has to be in the CODE, inside d3d9.dll, not in the vtable.
    //
    // The device vtable is contested: another ASI (WidescreenFix or
    // HDReflections) restores it a few seconds after us - D3D9Hook already
    // documents this for EndScene. A vtable hook here dies before the game
    // reaches the track, and the log goes quiet: not because the game stops
    // sending the matrices, but because there is no hook left when they go
    // by. Nobody restores the function body.
    bool DetourInCode(void* target, void* hook, Trampoline* tramp,
                      void** original, const char* name)
    {
        unsigned char* p = (unsigned char*)target;
        if (!target || IsBadReadPtr(p, 16)) return false;

        size_t len = CopyLength(p, 5);
        if (len == 0 || len > 16)
        {
            LogGfx("worlddraw: %s at 0x%p has a prologue I cannot copy"
                   " (%02X %02X %02X %02X %02X)",
                   name, target, p[0], p[1], p[2], p[3], p[4]);
            return false;
        }

        *original = tramp->Install((uintptr_t)target, hook, len);
        return *original != 0;
    }

    void Listen(IDirect3DDevice9* device)
    {
        if (g_listening) return;
        g_listening = true;

        void** vt = *(void***)device;
        const int VT_SET_VS_CONST  = 94;  // IDirect3DDevice9::SetVertexShaderConstantF
        const int VT_SET_TRANSFORM = 44;  // IDirect3DDevice9::SetTransform

        static Trampoline tramp1, tramp2;
        bool a = DetourInCode(vt[VT_SET_TRANSFORM], (void*)SetTransformHook,
                              &tramp1, (void**)&g_origSetTransform,
                              "SetTransform");
        bool b = DetourInCode(vt[VT_SET_VS_CONST], (void*)SetVSConstHook,
                              &tramp2, (void**)&g_origSetVSConst,
                              "SetVertexShaderConstantF");

        LogGfx("worlddraw: listening in code — SetTransform %s, constants %s",
               a ? "ok" : "failed", b ? "ok" : "failed");
    }


    // A soft radial blob, built here rather than shipped as a file: it is a few
    // lines of code, it always matches the blend mode, and it saves the mod
    // from carrying an asset just to draw a dot of light.
    IDirect3DTexture9* BuildTexture(IDirect3DDevice9* device)
    {
        const UINT N = 64;
        IDirect3DTexture9* tex = 0;
        if (FAILED(device->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &tex, 0)))
            return 0;

        IDirect3DTexture9* staging = 0;
        if (FAILED(device->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_SYSTEMMEM, &staging, 0)))
        { tex->Release(); return 0; }

        D3DLOCKED_RECT lr;
        if (SUCCEEDED(staging->LockRect(0, &lr, 0, 0)))
        {
            for (UINT y = 0; y < N; y++)
            {
                DWORD* row = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (UINT x = 0; x < N; x++)
                {
                    float dx = (x + 0.5f) / N * 2.0f - 1.0f;
                    float dy = (y + 0.5f) / N * 2.0f - 1.0f;
                    float d = sqrtf(dx * dx + dy * dy);

                    // Bright core, soft edge: squaring the falloff keeps the
                    // middle hot instead of washing the whole quad out.
                    float a = d >= 1.0f ? 0.0f : (1.0f - d);
                    a = a * a;

                    BYTE v = (BYTE)(a * 255.0f);
                    row[x] = (v << 24) | 0x00FFFFFF;
                }
            }
            staging->UnlockRect(0);
            device->UpdateTexture(staging, tex);
        }

        staging->Release();
        return tex;
    }
}

// Where d3d9.dll lives. Everything that gets called has to be inside it.
static bool D3D9Range(uintptr_t* start, uintptr_t* end)
{
    HMODULE m = GetModuleHandleA("d3d9.dll");
    if (!m) return false;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);

    *start = (uintptr_t)m;
    *end = *start + nt->OptionalHeader.SizeOfImage;
    return true;
}

// Validates an arbitrary pointer as a texture, trusting nothing.
//
// The address comes from a memory scan, so it could be anything - and here is
// the lesson that cost a crash: __try is NOT ENOUGH. It guards against invalid
// reads, but not against the worse case, which is the vtable being READABLE
// garbage: the `call` jumps to an arbitrary address and the process executes
// what it should not, with no exception at all to catch.
//
// That is why validation happens in two stages, and the first executes
// nothing: the vtable of a real texture lives inside d3d9.dll, and so does its
// first method. Only after that do we ask D3D for the type and description.
static bool CheckTexture(void* p, unsigned* width, unsigned* height, int* fmt)
{
    uintptr_t start = 0, end = 0;
    if (!D3D9Range(&start, &end)) return false;

    if (IsBadReadPtr(p, sizeof(void*))) return false;
    uintptr_t vtable = *(uintptr_t*)p;
    if (vtable < start || vtable >= end) return false;
    if (IsBadReadPtr((void*)vtable, sizeof(void*) * 12)) return false;

    // The methods the vtable points to also have to be in the dll: a plausible
    // vtable that points outside is exactly what brings the game down.
    for (int i = 0; i < 12; i++)
    {
        uintptr_t method = ((uintptr_t*)vtable)[i];
        if (method < start || method >= end) return false;
    }

    __try
    {
        IDirect3DTexture9* t = (IDirect3DTexture9*)p;
        if (t->GetType() != D3DRTYPE_TEXTURE) return false;

        D3DSURFACE_DESC d;
        if (FAILED(t->GetLevelDesc(0, &d))) return false;
        if (!d.Width || !d.Height || d.Width > 8192 || d.Height > 8192) return false;

        *width = d.Width; *height = d.Height; *fmt = (int)d.Format;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

namespace WorldDraw
{
    void Init() {}

    bool TextureValid() { return g_gameTexture != 0; }

    void TextureSource(uintptr_t address)
    {
        g_textureSource = address;
        g_lastPointer = 0;
    }

    bool UseTexture(uintptr_t address, unsigned* width, unsigned* height,
                    int* format)
    {
        if (!address)
        {
            g_gameTexture = 0;
            g_textureSource = 0;
            return true;
        }

        if (IsBadReadPtr((const void*)address, sizeof(void*))) return false;
        if (!CheckTexture((void*)address, width, height, format)) return false;

        g_gameTexture = (IDirect3DTexture9*)address;
        LogGfx("worlddraw: using the game texture at 0x%p (%ux%u, format %d)",
               (void*)address, *width, *height, *format);
        return true;
    }

    void OnDeviceLost()
    {
        if (g_texture) { g_texture->Release(); g_texture = 0; }

        // The game's texture is not ours to release, but it does not survive
        // the reset either: drop the reference and let the source re-read it
        // on the way back.
        g_gameTexture = 0;
        g_lastPointer = 0;
        g_particles.clear();
    }

    void OnDeviceReset() { /* the texture is rebuilt on the next frame */ }

    void Anchor(const float* position, const float* rotation)
    {
        if (!position || !rotation) { g_hasAnchor = false; return; }
        for (int i = 0; i < 3; i++) g_anchorPos[i] = position[i];
        for (int i = 0; i < 9; i++) g_anchorRot[i] = rotation[i];
        g_hasAnchor = true;
    }

    void Atlas(int columns, int rows)
    {
        g_columns = (columns > 0 && columns <= 16) ? columns : 1;
        g_rows    = (rows    > 0 && rows    <= 16) ? rows    : 1;
    }

    void Camera(uintptr_t viewAddress, float fovYDegrees, float nearPlane,
                float farPlane, bool depth, bool flipY, bool automatic)
    {
        g_flipY = flipY;
        // Turning automatic mode off and on again restarts the search from
        // scratch: that is what makes /vista auto useful as a test, and not
        // just as a switch.
        if (automatic != g_automatic) { g_found = false; g_register = -1; }
        g_automatic = automatic;
        g_viewAddress = viewAddress;
        if (fovYDegrees > 1.0f && fovYDegrees < 179.0f) g_fovY = fovYDegrees;
        if (nearPlane > 0.0f) g_near = nearPlane;
        if (farPlane > nearPlane) g_far = farPlane;
        g_depth = depth;
    }

    void Spawn(float x, float y, float z, float size, float life, DWORD colour,
               bool attached, float vx, float vy, float vz, DWORD endColour)
    {
        // A cap, not a queue: a runaway mod spawning every frame should cost a
        // constant amount of work, not grow until the frame time collapses.
        if (g_particles.size() >= 256) return;

        // Serves as the reference point for identifying the game's matrix: a
        // particle is, by definition, where the flame should appear.
        if (!attached)
        {
            g_reference[0] = x; g_reference[1] = y; g_reference[2] = z;
            g_hasReference = true;
        }

        Particle p;
        p.attached = attached;
        p.pos = Vec3(x, y, z);
        p.vel = Vec3(vx, vy, vz);
        p.endColour = endColour;
        p.size = size;
        p.life = p.totalLife = life;
        p.colour = colour;
        g_particles.push_back(p);
    }

    void Clear() { g_particles.clear(); }
    int  Count() { return (int)g_particles.size(); }

    // Re-checks the game texture every frame by re-reading the source. Cheap:
    // one read, and the full validation only when the pointer changes.
    void RefreshTexture()
    {
        if (!g_textureSource) return;
        if (IsBadReadPtr((const void*)g_textureSource, sizeof(void*))) return;

        uintptr_t current = *(uintptr_t*)g_textureSource;
        if (current == g_lastPointer) return;
        g_lastPointer = current;

        unsigned width = 0, height = 0;
        int format = 0;
        if (current && !IsBadReadPtr((const void*)current, sizeof(void*)) &&
            CheckTexture((void*)current, &width, &height, &format))
        {
            g_gameTexture = (IDirect3DTexture9*)current;
            LogGfx("worlddraw: game texture reloaded (0x%p, %ux%u)",
                   (void*)current, width, height);
        }
        else
        {
            // Vanish instead of drawing garbage: our own blob takes over.
            g_gameTexture = 0;
            LogGfx("worlddraw: game texture is gone, using the blob");
        }
    }

    void Render(IDirect3DDevice9* device, float dt)
    {
        if (!device) return;
        RefreshTexture();
        if (g_automatic) Listen(device);
        if (g_particles.empty()) return;

        if (!g_texture)
        {
            g_texture = BuildTexture(device);
            if (!g_texture)
            {
                if (!g_warnedFailure)
                {
                    g_warnedFailure = true;
                    LogGfx("worlddraw: could not create the particle texture");
                }
                return;
            }
        }

        // The camera. Asking the device is no use: NFSU2 draws everything
        // with vertex shaders and never sets the fixed pipeline matrices, so
        // they come back as identity - the quads would end up off screen, with
        // no error at all, which was exactly the symptom.
        //
        // The VIEW one comes from the game's memory (0x8734A0, found with the
        // /camera command: it is the only one whose rotation is orthonormal
        // and that puts the car a few metres in front of the camera). The
        // layout is a standard D3DMATRIX, so it is copied raw.
        //
        // The PROJECTION one does not exist anywhere: being shader-based, the
        // game keeps the constants already combined. It is built here - it is
        // an ordinary perspective, and the only missing data are the field of
        // view (adjustable by the mod) and the screen aspect, which comes from
        // the viewport itself.
        D3DMATRIX view;
        if (!ReadMatrix(g_viewAddress, &view))
            device->GetTransform(D3DTS_VIEW, &view);

        // One-off diagnostic: if the game draws with vertex shaders, it never
        // sets the fixed pipeline matrices and these come back as identity -
        // in which case the quads would go off screen and nothing would show
        // up, with no error at all. It is the first thing to rule out.
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            D3DMATRIX proj;
            device->GetTransform(D3DTS_PROJECTION, &proj);
            LogGfx("worlddraw: view [%.2f %.2f %.2f | %.2f %.2f %.2f | %.1f %.1f %.1f]",
                   view._11, view._12, view._13,
                   view._21, view._22, view._23,
                   view._41, view._42, view._43);
            LogGfx("worlddraw: proj [%.2f %.2f %.2f %.2f]",
                   proj._11, proj._22, proj._33, proj._43);

            IDirect3DVertexShader9* vs = 0;
            device->GetVertexShader(&vs);
            LogGfx("worlddraw: vertex shader active = %s", vs ? "yes" : "no");
            if (vs) vs->Release();
        }

        // The camera axes come from the COLUMNS of the view matrix, without
        // inverting anything: its rotational part is orthonormal, and for a
        // matrix like that the inverse is the transpose. That is what lets us
        // do without D3DX.
        Vec3 right(view._11, view._21, view._31);
        Vec3 up(view._12, view._22, view._32);

        std::vector<Vertex> vertices;
        vertices.reserve(g_particles.size() * 6);

        for (size_t i = 0; i < g_particles.size(); )
        {
            Particle& p = g_particles[i];
            p.life -= dt;
            if (p.life <= 0.0f)
            {
                p = g_particles.back();
                g_particles.pop_back();
                continue;
            }

            // Move. The velocity is in the same frame of reference as the
            // position, so on an attached particle it is velocity RELATIVE TO
            // THE CAR - which is what makes the jet come out of the pipe
            // instead of out of the world.
            p.pos = p.pos + p.vel * dt;

            // An attached particle only becomes world space now, with this
            // frame's frame of reference - that is what keeps it stuck to the
            // car instead of being left behind.
            Vec3 position = p.pos;
            if (p.attached && g_hasAnchor)
            {
                const float* m = g_anchorRot;
                position = Vec3(
                    g_anchorPos[0] + p.pos.x * m[0] + p.pos.y * m[3] + p.pos.z * m[6],
                    g_anchorPos[1] + p.pos.x * m[1] + p.pos.y * m[4] + p.pos.z * m[7],
                    g_anchorPos[2] + p.pos.x * m[2] + p.pos.y * m[5] + p.pos.z * m[8]);
            }

            float t = p.life / p.totalLife;          // 1 -> 0 over its life

            // The animation frame, in sheet order: left to right, top to
            // bottom. The sequence plays once over the life.
            float u0 = 0.0f, v0 = 0.0f, du = 1.0f, dv = 1.0f;
            const int frames = g_columns * g_rows;
            if (frames > 1)
            {
                int frame = (int)((1.0f - t) * frames);
                if (frame < 0) frame = 0;
                if (frame >= frames) frame = frames - 1;

                du = 1.0f / g_columns;
                dv = 1.0f / g_rows;
                u0 = (frame % g_columns) * du;
                v0 = (frame / g_columns) * dv;
            }
            BYTE alpha = (BYTE)(t * 255.0f);

            // Grows a little as it fades, like a puff of burning gas.
            float s = p.size * (1.0f + (1.0f - t) * 0.8f);
            DWORD colour = (alpha << 24) |
                           (Blend(p.colour, p.endColour, 1.0f - t) & 0x00FFFFFF);

            Vec3 r = right * s;
            Vec3 u = up * s;

            Vec3 a = position - r - u;
            Vec3 b = position + r - u;
            Vec3 c = position + r + u;
            Vec3 d = position - r + u;

            const float u1 = u0 + du, v1 = v0 + dv;
            Vertex quad[6] = {
                { a.x, a.y, a.z, colour, u0, v1 },
                { d.x, d.y, d.z, colour, u0, v0 },
                { b.x, b.y, b.z, colour, u1, v1 },
                { b.x, b.y, b.z, colour, u1, v1 },
                { d.x, d.y, d.z, colour, u0, v0 },
                { c.x, c.y, c.z, colour, u1, v0 },
            };
            for (int k = 0; k < 6; k++) vertices.push_back(quad[k]);
            i++;
        }

        if (vertices.empty()) return;

        IDirect3DStateBlock9* state = 0;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state))) return;
        state->Capture();

        D3DMATRIX identity = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };
        device->SetTransform(D3DTS_WORLD, &identity);

        // The good attempt first: the game's own matrix. It only searches once,
        // and only when there is a particle to draw - the reference point is
        // the particle itself, which by definition is where the flame should
        // appear.
        // The best source, when there is one: the matrices the game handed
        // over while drawing the world this frame. No hand-built field of view
        // and no flip - it is the same maths that drew the car.
        if (g_automatic && g_hasGameProj && g_hasGameView)
        {
            device->SetTransform(D3DTS_VIEW, &g_gameView);
            device->SetTransform(D3DTS_PROJECTION, &g_gameProj);
        }
        else if (g_automatic && g_found)
        {
            D3DMATRIX vp;
            float* d = &vp._11;

            // The fixed pipeline multiplies with the point on the LEFT (p * M),
            // and the shader gets the constants to multiply on the right. When
            // that is the game's convention, the matrix goes in transposed.
            if (g_transposed)
                for (int l = 0; l < 4; l++)
                    for (int col = 0; col < 4; col++)
                        d[l * 4 + col] = g_gameMatrix[col * 4 + l];
            else
                memcpy(d, g_gameMatrix, sizeof(g_gameMatrix));

            device->SetTransform(D3DTS_VIEW, &identity);
            device->SetTransform(D3DTS_PROJECTION, &vp);
        }
        else if (g_viewAddress)
        {
            device->SetTransform(D3DTS_VIEW, &view);

            D3DVIEWPORT9 vp;
            float aspect = 4.0f / 3.0f;
            if (SUCCEEDED(device->GetViewport(&vp)) && vp.Height)
                aspect = (float)vp.Width / (float)vp.Height;

            float h = 1.0f / tanf(g_fovY * 3.14159265f / 360.0f);
            float w = h / aspect;
            float q = g_far / (g_far - g_near);

            // The sign of the vertical axis: if the game's matrix has "up"
            // inverted relative to what the fixed pipeline expects, everything
            // we draw shows up mirrored around the camera axis - right
            // horizontally and in distance, wrong only in height. Which is
            // exactly the symptom.
            if (g_flipY) h = -h;

            D3DMATRIX proj = {
                w, 0, 0, 0,
                0, h, 0, 0,
                0, 0, q, 1,
                0, 0, -q * g_near, 0,
            };
            device->SetTransform(D3DTS_PROJECTION, &proj);
        }

        device->SetFVF(FVF);
        device->SetTexture(0, g_gameTexture ? g_gameTexture : g_texture);
        device->SetPixelShader(0);
        device->SetVertexShader(0);

        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);    // additive
        device->SetRenderState(D3DRS_ZENABLE, g_depth ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(vertices.size() / 3),
                                vertices.data(), sizeof(Vertex));

        state->Apply();
        state->Release();
    }
}
