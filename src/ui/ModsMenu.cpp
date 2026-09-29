#include "ModsMenu.h"

#include "D3D9Hook.h"
#include "Draw2D.h"
#include "ModsIconPng.h"
#include "core/Hook.h"
#include "core/Log.h"
#include "game/MenuSound.h"
#include "host/ModSettings.h"

#include "../../sdk/frsmodloader.h"

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

extern "C" const SL_Api* __cdecl FRSModLoader_GetApi(unsigned version);

namespace
{
    // ---- the engine -------------------------------------------------------
    const uintptr_t OPTIONS_SETUP    = 0x4B6230;
    const uintptr_t LANGUAGE_LOOKUP  = 0x4FF9D0;
    const uintptr_t GET_TEXTURE_INFO = 0x4901D0;
    const uintptr_t OPTIONS_PAGE     = 0x83AA04;   // which page Options opens
    const uintptr_t OPTIONS_NOTIFY   = 0x4B6080;   // the Options screen's messages
    const size_t    SCREEN_CURRENT   = 0x58;       // the option under the highlight
    const size_t    SCREEN_INPUT_ON  = 0x7B;       // cleared while it animates out

    const size_t    ICON_OPTION_SIZE = 0x48;
    const DWORD     OPTION_ACTIVATED = 0x0C407210;   // the hash a chosen option gets
    const uintptr_t ICON_OPTION_FREE = 0x4AEB70;     // slot 0 of an option's vtable
    const DWORD     AUDIO_ICON       = 0xF37AF144;   // the texture we copy from

    typedef void* (__cdecl* tOperatorNew)(size_t);
    typedef void  (__thiscall* tIconOptionCreate)(void* opt, DWORD textureHash,
                                                  DWORD stringHash, DWORD unk);
    typedef void  (__thiscall* tAddOption)(DWORD* screen, void* opt);
    typedef void  (__fastcall* tSetup)(DWORD* screen, void* edx);
    typedef void* (__cdecl* tGetTextureInfo)(DWORD hash, int a, int b);

    const tOperatorNew      OperatorNew      = (tOperatorNew)0x575620;
    const tIconOptionCreate IconOptionCreate = (tIconOptionCreate)0x51F670;

    tSetup          g_origSetup = 0;
    tGetTextureInfo g_origGetTexture = 0;
    void*           g_origLookup = 0;     // trampoline into 0x4FF9D0

    // The game's own bStringHash, so these read like the game's names.
    DWORD Hash(const char* s)
    {
        DWORD h = 0xFFFFFFFF;
        for (; *s; s++) h = h * 33 + (unsigned char)*s;
        return h;
    }

    DWORD       g_labelHash = 0;
    DWORD       g_iconHash = 0;
    const char* g_label = "Mods";

    // ---- the label --------------------------------------------------------
    // 0x4FF9D0 takes the hash in edx and answers in eax, which no C calling
    // convention describes: a naked stub checks for ours and otherwise lets
    // the original run from its trampoline, registers untouched.
    __declspec(naked) void LanguageLookupHook()
    {
        __asm
        {
            cmp edx, g_labelHash
            jne not_ours
            mov eax, g_label
            ret
        not_ours:
            jmp g_origLookup
        }
    }

    // ---- the icon ---------------------------------------------------------
    IDirect3DTexture9* g_iconTexture = 0;
    unsigned char      g_iconInfo[0x100];   // our TextureInfo, copied from Audio's
    unsigned char      g_iconPlat[0x80];    // and the platform part it points to
    bool               g_iconReady = false;
    bool               g_iconGaveUp = false;

    bool DecodeIcon(std::vector<uint8_t>* bgra, UINT* w, UINT* h)
    {
        HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = 0;
        IWICStream* stream = 0;
        IWICBitmapDecoder* decoder = 0;
        IWICBitmapFrameDecode* frame = 0;
        IWICFormatConverter* converter = 0;
        bool ok = false;

        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory((BYTE*)kModsIconPng, sizeof(kModsIconPng))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream, NULL,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, NULL, 0.0,
                                            WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(w, h)))
        {
            bgra->resize((size_t)*w * *h * 4);
            ok = SUCCEEDED(converter->CopyPixels(NULL, *w * 4, (UINT)bgra->size(),
                                                 bgra->data()));
        }

        if (converter) converter->Release();
        if (frame)     frame->Release();
        if (decoder)   decoder->Release();
        if (stream)    stream->Release();
        if (factory)   factory->Release();
        if (SUCCEEDED(co)) CoUninitialize();
        return ok;
    }

    // Builds our TextureInfo the first time the icon is asked for. The Audio
    // icon's lives in the same pack as the Options screen, so it is loaded by
    // then.
    //
    // The Direct3D texture is not in the TextureInfo itself: the frontend's
    // bind (0x5BCC30) reads it as [[info + 0] + 0x18], through a platform
    // block the first field points at. Rather than trust those two offsets,
    // both levels are searched for the field whose object has the vtable of a
    // texture of ours, made on the same device. Both blocks are copied, and
    // the copies hold our hash and our texture.
    void BuildIcon(int a, int b)
    {
        unsigned char* donor = (unsigned char*)g_origGetTexture(AUDIO_ICON, a, b);
        IDirect3DDevice9* device = D3D9Hook::Device();
        if (!donor || !device) return;   // not yet: asked again next frame

        if (!g_iconTexture)
        {
            std::vector<uint8_t> bgra;
            UINT w = 0, h = 0;
            if (!DecodeIcon(&bgra, &w, &h))
            {
                LogGfx("mods menu: the icon PNG did not decode");
                g_iconGaveUp = true;
                return;
            }
            g_iconTexture = Draw2D::Texture(device, bgra.data(), (int)w, (int)h);
            if (!g_iconTexture)
            {
                LogGfx("mods menu: could not create the icon texture");
                g_iconGaveUp = true;
                return;
            }
        }

        const void* vtable = *(void**)g_iconTexture;
        auto isTexture = [vtable](void* p) {
            return p && !IsBadReadPtr(p, 4) && *(void**)p == vtable;
        };

        int platAt = -1, d3dAt = -1;   // platAt -1: the texture is in the info itself
        for (int off = 0; off < 0x80 && d3dAt < 0; off += 4)
            if (isTexture(*(void**)(donor + off))) d3dAt = off;
        for (int off = 0; off < 0x80 && d3dAt < 0; off += 4)
        {
            unsigned char* plat = *(unsigned char**)(donor + off);
            if (!plat || IsBadReadPtr(plat, sizeof(g_iconPlat))) continue;
            for (int in = 0; in < (int)sizeof(g_iconPlat); in += 4)
                if (isTexture(*(void**)(plat + in))) { platAt = off; d3dAt = in; break; }
        }
        if (d3dAt < 0)
        {
            LogGfx("mods menu: no Direct3D texture found behind the Audio icon's"
                   " TextureInfo - the entry keeps Audio's icon");
            g_iconGaveUp = true;
            return;
        }

        const DWORD donorHash = *(DWORD*)(donor + 0x24);
        memcpy(g_iconInfo, donor, sizeof(g_iconInfo));
        for (int off = 0; off < (int)sizeof(g_iconInfo); off += 4)
            if (*(DWORD*)(g_iconInfo + off) == donorHash)
                *(DWORD*)(g_iconInfo + off) = g_iconHash;

        if (platAt < 0)
        {
            *(IDirect3DTexture9**)(g_iconInfo + d3dAt) = g_iconTexture;
            LogGfx("mods menu: icon ready (texture at info+0x%X)", d3dAt);
        }
        else
        {
            memcpy(g_iconPlat, *(unsigned char**)(donor + platAt), sizeof(g_iconPlat));
            *(IDirect3DTexture9**)(g_iconPlat + d3dAt) = g_iconTexture;
            *(unsigned char**)(g_iconInfo + platAt) = g_iconPlat;
            LogGfx("mods menu: icon ready (texture at [info+0x%X]+0x%X)", platAt, d3dAt);
        }
        g_iconReady = true;
    }

    void* __cdecl GetTextureInfoHook(DWORD hash, int a, int b)
    {
        if (hash == g_iconHash)
        {
            if (!g_iconReady && !g_iconGaveUp) BuildIcon(a, b);
            if (g_iconReady) return g_iconInfo;
            hash = AUDIO_ICON;   // better Audio's picture than a hole
        }
        return g_origGetTexture(hash, a, b);
    }

    // ---- the entry --------------------------------------------------------
    //
    // Choosing an entry is a message to the screen (0x4B6080), and the
    // screen's answer is to leave: its base (0x543D40) marks the choice as
    // pending, the entry's React says which page, and the screen comes back
    // only when that page is closed - built anew, by its Setup. Every entry
    // the game has leaves this way, even the ones that "react in place"
    // (Profiles switches to UI_ProfileManager.fng). "Mods" does not leave:
    // the list opens over the screen. Let through, the choice left the screen
    // waiting for a page that never came, and no entry opened after it.
    //
    // So the choice of "Mods" never reaches the screen: it is taken here, and
    // the screen stays exactly as it was.
    typedef void (__fastcall* tNotify)(DWORD* screen, void* edx, unsigned int msg,
                                       unsigned int a, unsigned int b, unsigned int c);
    tNotify g_origNotify = 0;

    bool  g_openAsked = false;
    void* g_option = 0;          // our entry on the screen now on show
    DWORD g_optionVtable[2] = { ICON_OPTION_FREE, 0 };

    void Chosen()
    {
        MenuSound::Play("open");
        g_openAsked = true;   // the panel is opened from Tick, off the FE's stack
        LogGfx("mods menu: chosen");
    }

    void __fastcall OptionsNotifyHook(DWORD* screen, void* edx, unsigned int msg,
                                      unsigned int a, unsigned int b, unsigned int c)
    {
        unsigned char* s = (unsigned char*)screen;
        if (msg == OPTION_ACTIVATED && g_option &&
            *(void**)(s + SCREEN_CURRENT) == g_option && s[SCREEN_INPUT_ON])
        {
            Chosen();
            return;
        }
        g_origNotify(screen, edx, msg, a, b, c);
    }

    // Only reached if the choice came some other way than the screen's
    // message. Page index 6 is the one the screen stays put on - none of its
    // pages uses it.
    void __fastcall OptionReact(void*, void*, const char*, unsigned int data,
                                DWORD*, unsigned int, unsigned int)
    {
        if (data != OPTION_ACTIVATED) return;
        *(DWORD*)OPTIONS_PAGE = 6;
        Chosen();
    }

    void __fastcall OptionsSetupHook(DWORD* screen, void* edx)
    {
        void* opt = OperatorNew(ICON_OPTION_SIZE);
        if (opt)
        {
            IconOptionCreate(opt, g_iconHash, g_labelHash, 0);
            *(DWORD*)opt = (DWORD)g_optionVtable;
            // "Reacts in place", as the game marks Profiles or Save: should a
            // choice ever get past OptionsNotifyHook, the screen at least does
            // not switch its input off to animate out (0x52FBF0).
            *((BYTE*)opt + 0x45) = 1;
            g_option = opt;
            // AddOption is slot 6 of the screen's vtable.
            tAddOption addOption = (tAddOption)(*(DWORD**)screen)[6];
            addOption(screen, opt);
        }
        g_origSetup(screen, edx);
    }

    // ---- the panel --------------------------------------------------------
    const SL_Api* g_api = 0;
    SL_Panel*     g_panel = 0;

    // What the page says, as "<what>:<mod id>" with the data as JSON:
    //
    //   close            the list was closed: input back to the game
    //   enable:<id>      true / false
    //   set:<id>         the mod's whole settings object
    //   action:<id>      {"id": "<action row>"}
    void __cdecl OnPanelMessage(const char* channel, const char* json, void*)
    {
        const std::string c = channel;
        const size_t colon = c.find(':');
        const std::string what = c.substr(0, colon);
        const std::string id = colon == std::string::npos ? "" : c.substr(colon + 1);

        if (what == "close")       g_api->capture_input(0);
        else if (what == "enable") ModSettings::SetEnabled(id, !strcmp(json, "true"));
        else if (what == "set")    ModSettings::SetValues(id, json);
        else if (what == "action") ModSettings::Action(id, json);
    }

    // Hooks a function start. Gives up when its first instructions cannot be
    // moved - a jump there means another .asi got to it first.
    void* Detour(uintptr_t at, void* hook, const char* what)
    {
        size_t len = CopyLength((const uint8_t*)at, 5);
        if (!len)
        {
            LogGfx("mods menu: cannot detour %s at 0x%08X", what, at);
            return 0;
        }
        Trampoline* t = new Trampoline();
        return t->Install(at, hook, len);
    }
}

namespace ModsMenu
{
    void Install()
    {
        g_labelHash = Hash("FRSMODLOADER_MODS");
        g_iconHash  = Hash("FRSMODLOADER_MODS_ICON");
        g_optionVtable[1] = (DWORD)OptionReact;

        g_origLookup = Detour(LANGUAGE_LOOKUP, (void*)LanguageLookupHook, "the language lookup");
        g_origGetTexture = (tGetTextureInfo)Detour(GET_TEXTURE_INFO, (void*)GetTextureInfoHook,
                                                   "GetTextureInfo");
        // Without its label and icon the entry would be a blank; better none.
        if (!g_origLookup || !g_origGetTexture) return;
        // The choice has to be caught before the screen sees it; without that
        // the entry would lock the menu, so no hook, no entry.
        g_origNotify = (tNotify)Detour(OPTIONS_NOTIFY, (void*)OptionsNotifyHook,
                                       "the Options messages");
        if (!g_origNotify) return;
        g_origSetup = (tSetup)Detour(OPTIONS_SETUP, (void*)OptionsSetupHook, "the Options Setup");
        LogGfx("mods menu: %s", g_origSetup ? "installed in Options" : "not installed");
    }

    void Tick()
    {
        if (!g_api)
        {
            g_api = FRSModLoader_GetApi(SL_API_VERSION);
            if (!g_api) return;
        }
        if (!g_panel)
        {
            // Mounted up front, hidden: the list opens at once when chosen.
            g_panel = g_api->panel_open("frsmodloader-mods", "FRSModLoader\\ui\\mods.html");
            if (g_panel) g_api->panel_on(g_panel, OnPanelMessage, 0);
            return;
        }
        if (g_openAsked && g_api->panel_ready(g_panel))
        {
            g_openAsked = false;
            // Built at the moment it opens: a plugin may have registered, or a
            // mod defined settings, since the last time.
            g_api->panel_send(g_panel, "open", ModSettings::PanelJson().c_str());
            g_api->capture_input(1);   // Esc and the mouse go to the list
        }
    }
}
