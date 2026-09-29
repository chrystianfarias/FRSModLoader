#include "D3D9Hook.h"

#include "core/Hook.h"
#include "ui/WorldDraw.h"
#include "core/Log.h"

namespace
{
    // D3D9 vtable indices, counted from d3d9.h; unchanged since 2004.
    const int VT_D3D9_CREATE_DEVICE   = 16;
    const int VT_DEVICE_RESET         = 16;
    const int VT_DEVICE_END_SCENE     = 42;

    typedef IDirect3D9*  (WINAPI* tDirect3DCreate9)(UINT);
    typedef HRESULT (STDMETHODCALLTYPE* tCreateDevice)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
        D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    typedef HRESULT (STDMETHODCALLTYPE* tEndScene)(IDirect3DDevice9*);
    typedef HRESULT (STDMETHODCALLTYPE* tReset)(IDirect3DDevice9*,
                                                D3DPRESENT_PARAMETERS*);

    tDirect3DCreate9 g_origCreate9   = 0;
    tCreateDevice    g_origCreateDev = 0;
    tEndScene        g_origEndScene  = 0;
    tReset           g_origReset     = 0;

    IDirect3DDevice9* g_device = 0;
    D3D9Hook::FrameFn  g_onFrame = 0;
    D3D9Hook::DeviceFn g_onLost = 0;
    D3D9Hook::DeviceFn g_onReset = 0;

    bool g_deviceHooked = false;

    HRESULT STDMETHODCALLTYPE EndSceneHook(IDirect3DDevice9* device)
    {
        // With the detour sitting in d3d9.dll's code, this runs for every
        // device in the process - including ones other mods create for
        // themselves. We only draw into the game's device, captured in
        // CreateDevice.
        if (g_device && device != g_device) return g_origEndScene(device);
        g_device = device;

        // Heartbeat: if this log stops, we lost the detour or the game swapped
        // devices without going through Reset.
        static DWORD last = 0;
        static unsigned long long frames = 0;
        frames++;
        DWORD now = GetTickCount();
        if (now - last > 5000)
        {
            last = now;
            LogGfx("EndScene alive: %llu frames", frames);
        }

        // The world's particles go BEFORE the interface: they are part of the
        // scene, with depth and perspective, while the overlay is flat and
        // covers everything. Drawing them afterwards would put the flame over
        // the HUD.
        {
            static DWORD previous = 0;
            DWORD current = GetTickCount();
            float dt = previous ? (current - previous) / 1000.0f : 0.0f;
            previous = current;
            if (dt > 0.25f) dt = 0.25f;   // a long pause does not kill the particles
            WorldDraw::Render(device, dt);
        }

        if (g_onFrame) g_onFrame(device);
        return g_origEndScene(device);
    }

    HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device,
                                        D3DPRESENT_PARAMETERS* pp)
    {
        LogGfx("Reset: %ux%u %s", pp ? pp->BackBufferWidth : 0,
               pp ? pp->BackBufferHeight : 0,
               (pp && pp->Windowed) ? "windowed" : "fullscreen");

        WorldDraw::OnDeviceLost();
        if (g_onLost) g_onLost(device);
        HRESULT hr = g_origReset(device, pp);
        if (SUCCEEDED(hr)) WorldDraw::OnDeviceReset();
        if (SUCCEEDED(hr) && g_onReset) g_onReset(device);
        else if (FAILED(hr)) LogGfx("Reset FAILED (0x%08X)", hr);
        return hr;
    }

    // Detour in the function's code, inside d3d9.dll, rather than in the vtable.
    //
    // The device vtable is contested: WidescreenFix (or HDReflections) restores
    // the EndScene slot to d3d9.dll's original a few seconds after we start,
    // and the UI simply stopped being drawn. Nobody restores the function body.
    //
    // How many bytes to copy comes from CopyLength (core/Hook.h), which sums
    // whole instructions until it covers the 5 bytes of the JMP. The Windows 11
    // d3d9.dll starts with `push 0x14; mov eax, imm32` - 7 bytes, no relative
    // jump inside. If the prologue holds anything the measurer does not know,
    // we do not gamble: the vtable hook stays as the fallback.
    Trampoline g_endSceneTramp;
    Trampoline g_resetTramp;
    void* g_endSceneCode = 0;   // the function we detoured in d3d9.dll, if any

    bool CodeDetour(void* target, void* hook, Trampoline* tramp, void** original,
                    const char* name)
    {
        unsigned char* p = (unsigned char*)target;
        if (!target || IsBadReadPtr(p, 16)) return false;

        size_t len = CopyLength(p, 5);
        if (len == 0 || len > 16)
        {
            LogGfx("%s at 0x%p: prologue I cannot copy"
                   " (%02X %02X %02X %02X %02X) - falling back to the vtable",
                   name, target, p[0], p[1], p[2], p[3], p[4]);
            return false;
        }

        *original = tramp->Install((uintptr_t)target, hook, len);
        LogGfx("%s detoured in code (0x%p, %u bytes), immune to vtable swaps",
               name, target, (unsigned)len);
        return true;
    }

    void HookDevice(IDirect3DDevice9* device)
    {
        if (g_deviceHooked) return;
        g_deviceHooked = true;
        g_device = device;

        // Take the originals from the vtable, which is where they are now.
        void** vtbl = *(void***)device;
        void* endScene = vtbl[VT_DEVICE_END_SCENE];
        void* reset    = vtbl[VT_DEVICE_RESET];

        if (CodeDetour(endScene, (void*)EndSceneHook, &g_endSceneTramp,
                       (void**)&g_origEndScene, "EndScene"))
        {
            g_endSceneCode = endScene;
            // With the code detour in place, touching the vtable would only
            // cause a double call: the entry point is still the original.
            CodeDetour(reset, (void*)ResetHook, &g_resetTramp,
                       (void**)&g_origReset, "Reset");
            return;
        }

        g_origEndScene = (tEndScene)HookVTable(device, VT_DEVICE_END_SCENE,
                                               (void*)EndSceneHook);
        g_origReset    = (tReset)HookVTable(device, VT_DEVICE_RESET,
                                            (void*)ResetHook);
        LogGfx("device 0x%p hooked through the vtable (EndScene/Reset)", device);
    }

    HRESULT STDMETHODCALLTYPE CreateDeviceHook(
        IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focusWindow,
        DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
    {
        HRESULT hr = g_origCreateDev(self, adapter, type, focusWindow, behavior,
                                     pp, out);
        if (SUCCEEDED(hr) && out && *out)
        {
            LogGfx("CreateDevice: %ux%u %s", pp ? pp->BackBufferWidth : 0,
                   pp ? pp->BackBufferHeight : 0,
                   (pp && pp->Windowed) ? "windowed" : "fullscreen");
            HookDevice(*out);
        }
        return hr;
    }

    IDirect3D9* WINAPI Direct3DCreate9Hook(UINT sdkVersion)
    {
        IDirect3D9* d3d = g_origCreate9(sdkVersion);
        if (d3d && !g_origCreateDev)
        {
            g_origCreateDev = (tCreateDevice)HookVTable(d3d,
                VT_D3D9_CREATE_DEVICE, (void*)CreateDeviceHook);
            LogGfx("IDirect3D9 0x%p hooked (CreateDevice)", d3d);
        }
        return d3d;
    }
}

namespace D3D9Hook
{
    void Tick()
    {
        if (!g_device) return;

        static DWORD last = 0;
        DWORD now = GetTickCount();
        if (now - last < 5000) return;
        last = now;

        if (g_endSceneCode)
        {
            // With a code detour the vtable can say whatever the game likes:
            // what matters is the JMP still sitting at the top of the function.
            if (*(unsigned char*)g_endSceneCode == 0xE9) return;
            LogGfx("our EndScene code detour was removed from 0x%p",
                   g_endSceneCode);
            return;
        }

        void** vtbl = *(void***)g_device;
        void* current = vtbl[VT_DEVICE_END_SCENE];
        if (current == (void*)EndSceneHook) return;   // still ours

        // Someone rewrote the slot after us. Knowing which module it came from
        // settles the question between "another mod took the hook" and "the
        // game abandoned this device and is using another one".
        char name[MAX_PATH] = "(unknown)";
        HMODULE mod = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)current, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof(name));

        LogGfx("vtable EndScene is now 0x%p (%s), not ours 0x%p",
               current, name, (void*)EndSceneHook);
    }

    void OnFrame(FrameFn fn)        { g_onFrame = fn; }
    void OnDeviceLost(DeviceFn fn)  { g_onLost = fn; }
    void OnDeviceReset(DeviceFn fn) { g_onReset = fn; }

    IDirect3DDevice9* Device() { return g_device; }

    bool Install()
    {
        g_origCreate9 = (tDirect3DCreate9)HookIat("d3d9.dll", "Direct3DCreate9",
                                                  (void*)Direct3DCreate9Hook);
        if (!g_origCreate9)
        {
            LogGfx("FAILED to hook Direct3DCreate9 in the import table");
            return false;
        }
        LogGfx("Direct3DCreate9 hooked (original 0x%p)", g_origCreate9);
        return true;
    }

    bool BackBufferSize(int* width, int* height)
    {
        if (!g_device) return false;

        IDirect3DSurface9* back = 0;
        if (FAILED(g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))
            || !back)
            return false;

        D3DSURFACE_DESC desc;
        HRESULT hr = back->GetDesc(&desc);
        back->Release();
        if (FAILED(hr)) return false;

        *width = (int)desc.Width;
        *height = (int)desc.Height;
        return true;
    }
}
