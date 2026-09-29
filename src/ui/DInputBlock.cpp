#include "ui/DInputBlock.h"

#include "core/Log.h"
#include "ui/InputRouter.h"

#include <windows.h>

namespace
{
    // Only the two vtable slots that matter, by index - including dinput.h just
    // for two function pointers would drag COM headers into the build for no
    // gain.
    const int VT_CREATE_DEVICE   = 3;    // IDirectInput8::CreateDevice
    const int VT_GET_DEVICE_STATE = 9;   // IDirectInputDevice8::GetDeviceState
    const int VT_GET_DEVICE_DATA  = 10;  // IDirectInputDevice8::GetDeviceData

    typedef HRESULT (STDMETHODCALLTYPE* tGetDeviceState)(void*, DWORD, void*);
    typedef HRESULT (STDMETHODCALLTYPE* tGetDeviceData)(void*, DWORD, void*,
                                                        DWORD*, DWORD);
    typedef HRESULT (STDMETHODCALLTYPE* tCreateDevice)(void*, const GUID&, void**, void*);
    typedef HRESULT (WINAPI* tDirectInput8Create)(HINSTANCE, DWORD, const IID&,
                                                  void**, void*);

    tGetDeviceState     g_origGetState = 0;
    tGetDeviceData      g_origGetData  = 0;
    tCreateDevice       g_origCreate   = 0;
    tDirectInput8Create g_origCreateDI = 0;

    // Keyboard and mouse share one vtable, so the hooks see both; the buffered
    // reads only tell them apart by device.
    const GUID GUID_SysMouse_ = { 0x6F1D2B60, 0xD5A0, 0x11CF,
                                  { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    void* g_mouseDevice = 0;

    void* HookVTable(void* obj, int index, void* replacement)
    {
        void** vt = *(void***)obj;
        DWORD old;
        VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old);
        void* previous = vt[index];
        vt[index] = replacement;
        VirtualProtect(&vt[index], sizeof(void*), old, &old);
        return previous;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStateHook(void* self, DWORD size, void* buffer)
    {
        HRESULT hr = g_origGetState(self, size, buffer);

        // 256 bytes is the keyboard state, blocked while the UI has the
        // keyboard. The mouse (DIMOUSESTATE, 16 bytes, or DIMOUSESTATE2, 20)
        // is blocked while the page has the mouse - captured, or the pointer
        // over something it drew: the game takes the cursor position from
        // GetCursorPos and the clicks from here, so a button under a page
        // would still be clicked. Pads are never touched.
        if (SUCCEEDED(hr) && buffer)
        {
            if (size == 256 && InputRouter::IsCapturing())
                memset(buffer, 0, size);
            else if ((size == 16 || size == 20) && InputRouter::MouseOnUi())
                memset(buffer, 0, size);
        }

        return hr;
    }

    // What a blocked read still lets through: releases. The press that hands
    // the input to the UI - the Enter, or the click, on a menu entry that
    // opens a panel - reaches the game; its release comes a moment later,
    // with the UI holding the input. Swallowed, the game's menu would keep
    // that key down for good and ignore every press after it. Presses, and
    // the mouse's movement, go; releases of keys and buttons stay, compacted
    // to the front of the buffer. Returns how many are left.
    //
    // An item is a DIDEVICEOBJECTDATA: dwOfs (+0), dwData (+4, bit 0x80 =
    // down). On the keyboard dwOfs is the key; on the mouse 0/4/8 are the
    // axes and 12 on the buttons.
    DWORD KeepReleases(void* buffer, DWORD itemSize, DWORD count, bool mouse)
    {
        unsigned char* base = (unsigned char*)buffer;
        DWORD kept = 0;
        for (DWORD i = 0; i < count; i++)
        {
            unsigned char* item = base + (size_t)i * itemSize;
            const DWORD ofs = *(DWORD*)item;
            const DWORD data = *(DWORD*)(item + 4);
            const bool release = !(data & 0x80) && (!mouse || ofs >= 12);
            if (!release) continue;
            if (kept != i) memmove(base + (size_t)kept * itemSize, item, itemSize);
            kept++;
        }
        return kept;
    }

    // The other path, and the one NFSU2 actually uses: BUFFERED reads, where
    // key events come in a queue instead of a snapshot of the keyboard.
    // Zeroing the snapshot does nothing if the game never asks for it.
    //
    // While the UI has the input, the queue is drained and only the releases
    // stay (KeepReleases): the presses vanish for the game, without piling up
    // to arrive all at once when the UI gives the input back.
    HRESULT STDMETHODCALLTYPE GetDeviceDataHook(void* self, DWORD itemSize,
                                                void* buffer, DWORD* count,
                                                DWORD flags)
    {
        HRESULT hr = g_origGetData(self, itemSize, buffer, count, flags);

        if (SUCCEEDED(hr) && count)
        {
            const bool mouse = self == g_mouseDevice;
            const bool blocked = mouse ? InputRouter::MouseOnUi()
                                       : InputRouter::IsCapturing();
            if (blocked)
                *count = buffer && itemSize >= 8
                            ? KeepReleases(buffer, itemSize, *count, mouse)
                            : 0;
        }

        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateDeviceHook(void* self, const GUID& guid,
                                               void** device, void* agg)
    {
        HRESULT hr = g_origCreate(self, guid, device, agg);
        if (SUCCEEDED(hr) && device && *device &&
            memcmp(&guid, &GUID_SysMouse_, sizeof(GUID)) == 0)
        {
            g_mouseDevice = *device;
            LogTag("in  ", "mouse device is 0x%p", *device);
        }
        if (SUCCEEDED(hr) && device && *device && !g_origGetState)
        {
            // Keyboard and mouse share the same vtable, so one hook covers
            // both; the filtering is done by kind of read, not by
            // device.
            g_origGetState = (tGetDeviceState)HookVTable(*device,
                                                         VT_GET_DEVICE_STATE,
                                                         (void*)GetDeviceStateHook);
            g_origGetData = (tGetDeviceData)HookVTable(*device,
                                                       VT_GET_DEVICE_DATA,
                                                       (void*)GetDeviceDataHook);
            LogTag("in  ", "keyboard reads wrapped (state + buffered) on 0x%p",
                   *device);
        }
        return hr;
    }

    HRESULT WINAPI DirectInput8CreateHook(HINSTANCE inst, DWORD version,
                                          const IID& iid, void** out, void* agg)
    {
        HRESULT hr = g_origCreateDI(inst, version, iid, out, agg);
        if (SUCCEEDED(hr) && out && *out && !g_origCreate)
        {
            g_origCreate = (tCreateDevice)HookVTable(*out, VT_CREATE_DEVICE,
                                                     (void*)CreateDeviceHook);
            LogTag("in  ", "DirectInput wrapped (0x%p)", *out);
        }
        return hr;
    }
}

namespace DInputBlock
{
    void Install()
    {
        // Through the import table, so this works whether or not the game has
        // created its devices yet - the game calls DirectInput8Create itself,
        // and that is where we get in.
        HMODULE base = GetModuleHandleA(0);
        if (!base) return;

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);

        DWORD rva = nt->OptionalHeader
                      .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return;

        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + rva);

        for (; imp->Name; imp++)
        {
            const char* dll = (const char*)((BYTE*)base + imp->Name);
            if (_stricmp(dll, "dinput8.dll") != 0) continue;

            IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->OriginalFirstThunk);
            IMAGE_THUNK_DATA* addresses = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->FirstThunk);

            for (; names->u1.AddressOfData; names++, addresses++)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;

                IMAGE_IMPORT_BY_NAME* name =
                    (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + names->u1.AddressOfData);
                if (strcmp((const char*)name->Name, "DirectInput8Create") != 0) continue;

                DWORD old;
                VirtualProtect(&addresses->u1.Function, sizeof(void*),
                               PAGE_READWRITE, &old);
                g_origCreateDI = (tDirectInput8Create)addresses->u1.Function;
                addresses->u1.Function = (DWORD_PTR)DirectInput8CreateHook;
                VirtualProtect(&addresses->u1.Function, sizeof(void*), old, &old);

                LogTag("in  ", "DirectInput8Create wrapped in the import table");
                return;
            }
        }

        LogTag("in  ", "DirectInput8Create not found in the imports");
    }
}
