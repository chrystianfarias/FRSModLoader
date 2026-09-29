// The native plugin API: HTML panels for .asi mods written in C or C++.
//
// The design decision worth writing down is that a native panel is not a
// second kind of UI. It goes through the same shell, on the same
// "<id>:<channel>" convention a JavaScript mod's page uses, so the page is
// identical either way and the shell needs one extra message, not a second
// mounting path.
//
// The other one is the threading. A plugin is somebody else's DLL: it can call
// us from its own hook, from a worker thread, from wherever. So nothing here
// touches CEF or the plugin's callback directly - calls land in queues under a
// lock, and Tick drains them on the game thread, the same place Bridge already
// delivers everything else.
#include "HostApi.h"

#include "core/Config.h"
#include "core/Log.h"
#include "core/Version.h"
#include "game/MenuSound.h"
#include "host/ModSettings.h"
#include "ui/CefHost.h"
#include "ui/InputRouter.h"

#include "../../sdk/frsmodloader.h"

#include <windows.h>
#include <deque>
#include <string>
#include <vector>

namespace
{
    struct Panel
    {
        std::string  id;
        std::string  url;       // file:/// URL of the page
        SL_MessageFn onMessage;
        void*        user;
        bool         visible;
        bool         mounted;   // the shell has it on screen
        bool         wantsMount;
        unsigned     gen;       // bumped by panel_reload: the shell remounts
        bool         closing;
    };

    struct Outbound
    {
        std::string channel;
        std::string json;
    };

    CRITICAL_SECTION    g_lock;
    std::vector<Panel*> g_panels;
    std::deque<Outbound> g_out;
    bool                g_installed = false;

    // A plugin in a bad loop should not grow this without bound; the UI is
    // 60 Hz and nothing it misses at that rate is worth the memory.
    const size_t kMaxOut = 512;

    struct Guard
    {
        Guard()  { EnterCriticalSection(&g_lock); }
        ~Guard() { LeaveCriticalSection(&g_lock); }
    };

    // ------------------------------------------------------------- utilities
    std::string JsonString(const char* text)
    {
        std::string out = "\"";
        for (const char* p = text ? text : ""; *p; p++)
        {
            unsigned char c = (unsigned char)*p;
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char esc[8];
                    _snprintf(esc, sizeof(esc), "\\u%04X", c);
                    out += esc;
                }
                else out += (char)c;
            }
        }
        out += "\"";
        return out;
    }

    std::string ToFileUrl(const std::string& path)
    {
        std::string url = "file:///";
        for (size_t i = 0; i < path.size(); i++)
        {
            char c = path[i];
            if (c == '\\')      url += '/';
            else if (c == ' ')  url += "%20";
            else if (c == '#')  url += "%23";
            else if (c == '?')  url += "%3F";
            else                url += c;
        }
        return url;
    }

    // Ids name channels and show up in the console, so they stay to what reads
    // as a name. "sl" is the loader's own prefix and cannot be taken.
    bool ValidId(const char* id)
    {
        if (!id || !*id) return false;
        size_t len = strlen(id);
        if (len > 32) return false;
        if (!strcmp(id, "sl")) return false;

        for (size_t i = 0; i < len; i++)
        {
            char c = id[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    // Called with the lock held.
    void Queue(const std::string& channel, const std::string& json)
    {
        if (g_out.size() >= kMaxOut) g_out.pop_front();
        Outbound m;
        m.channel = channel;
        m.json = json;
        g_out.push_back(m);
    }

    std::string MountJson(const Panel* p)
    {
        return "{\"id\":" + JsonString(p->id.c_str()) +
               ",\"name\":" + JsonString(p->id.c_str()) +
               ",\"url\":" + JsonString(p->url.c_str()) +
               ",\"gen\":" + std::to_string(p->gen) +
               ",\"native\":true}";
    }

    Panel* Find(const std::string& id)
    {
        for (size_t i = 0; i < g_panels.size(); i++)
            if (g_panels[i]->id == id) return g_panels[i];
        return 0;
    }

    bool Alive(SL_Panel* handle)
    {
        Panel* p = (Panel*)handle;
        if (!p) return false;
        for (size_t i = 0; i < g_panels.size(); i++)
            if (g_panels[i] == p) return !p->closing;
        return false;
    }

    // ------------------------------------------------------------- the table
    const char* __cdecl ApiLoaderVersion(void)
    {
        return FRSMODLOADER_VERSION;
    }

    SL_Panel* __cdecl ApiPanelOpen(const char* id, const char* html)
    {
        if (!g_installed) return 0;
        if (!ValidId(id))
        {
            Log("plugin: \"%s\" is not a usable panel id", id ? id : "(null)");
            return 0;
        }
        if (!html || !*html)
        {
            Log("plugin %s: no page given", id);
            return 0;
        }

        char abs[MAX_PATH];
        Config::Resolve(html, abs, sizeof(abs));
        if (GetFileAttributesA(abs) == INVALID_FILE_ATTRIBUTES)
        {
            // Not fatal: the panel is created anyway and the shell reports the
            // fetch failure on screen, which is where the author is looking.
            Log("plugin %s: %s not found", id, abs);
        }

        Guard guard;
        if (Find(id))
        {
            Log("plugin: panel id \"%s\" is already taken", id);
            return 0;
        }

        Panel* p = new Panel();
        p->id = id;
        p->url = ToFileUrl(abs);
        p->onMessage = 0;
        p->user = 0;
        p->visible = true;
        p->mounted = false;
        p->wantsMount = true;
        p->closing = false;
        p->gen = 0;
        g_panels.push_back(p);

        Log("plugin panel \"%s\" -> %s", id, abs);
        return (SL_Panel*)p;
    }

    void __cdecl ApiPanelClose(SL_Panel* handle)
    {
        Guard guard;
        if (!Alive(handle)) return;

        Panel* p = (Panel*)handle;
        p->closing = true;
        p->onMessage = 0;
        Queue("sl:unmount", "{\"id\":" + JsonString(p->id.c_str()) + "}");
    }

    void __cdecl ApiPanelOn(SL_Panel* handle, SL_MessageFn fn, void* user)
    {
        Guard guard;
        if (!Alive(handle)) return;
        ((Panel*)handle)->onMessage = fn;
        ((Panel*)handle)->user = user;
    }

    void __cdecl ApiPanelSend(SL_Panel* handle, const char* channel, const char* json)
    {
        Guard guard;
        if (!Alive(handle) || !channel || !*channel) return;
        Queue(((Panel*)handle)->id + ":" + channel, json && *json ? json : "null");
    }

    void __cdecl ApiPanelSendText(SL_Panel* handle, const char* channel, const char* text)
    {
        Guard guard;
        if (!Alive(handle) || !channel || !*channel) return;
        Queue(((Panel*)handle)->id + ":" + channel, JsonString(text));
    }

    void __cdecl ApiPanelSendNumber(SL_Panel* handle, const char* channel, double value)
    {
        Guard guard;
        if (!Alive(handle) || !channel || !*channel) return;

        char buf[64];
        // %.17g round-trips a double; JSON has no other number type anyway.
        _snprintf(buf, sizeof(buf), "%.17g", value);
        Queue(((Panel*)handle)->id + ":" + channel, buf);
    }

    void __cdecl ApiPanelReload(SL_Panel* handle)
    {
        Guard guard;
        if (!Alive(handle)) return;

        // Mounting again is the reload: the shell drops the old shadow root
        // and fetches the file afresh, so edited HTML shows up without a
        // restart. A new generation is what tells it apart from a mount the
        // shell already has.
        Panel* p = (Panel*)handle;
        p->mounted = false;
        p->wantsMount = true;
        p->gen++;
    }

    void __cdecl ApiPanelShow(SL_Panel* handle, int show)
    {
        Guard guard;
        if (!Alive(handle)) return;

        Panel* p = (Panel*)handle;
        p->visible = show != 0;
        Queue("sl:panel-show", "{\"id\":" + JsonString(p->id.c_str()) +
                               ",\"show\":" + (p->visible ? "true" : "false") + "}");
    }

    int __cdecl ApiPanelReady(SL_Panel* handle)
    {
        Guard guard;
        return Alive(handle) && ((Panel*)handle)->mounted ? 1 : 0;
    }

    void __cdecl ApiPrint(const char* tag, const char* text)
    {
        if (!text) return;
        Guard guard;
        Queue("sl:log", "{\"mod\":" + JsonString(tag ? tag : "sl") +
                        ",\"text\":" + JsonString(text) + "}");
    }

    void __cdecl ApiCaptureInput(int on)
    {
        InputRouter::SetCapturing(on != 0);
    }

    int __cdecl ApiCapturingInput(void)
    {
        return InputRouter::IsCapturing() ? 1 : 0;
    }

    SL_Mod* __cdecl ApiModRegister(const char* manifest)
    {
        return ModSettings::RegisterNative(manifest);
    }

    const char* __cdecl ApiModSettings(SL_Mod* mod)
    {
        return ModSettings::NativeValues(mod);
    }

    void __cdecl ApiModOnSettings(SL_Mod* mod, SL_MessageFn fn, void* user)
    {
        ModSettings::NativeOnSettings(mod, fn, user);
    }

    int __cdecl ApiModEnabled(SL_Mod* mod)
    {
        return ModSettings::NativeEnabled(mod);
    }

    int __cdecl ApiMenuSound(const char* name)
    {
        return MenuSound::Play(name) ? 1 : 0;
    }

    const SL_Api g_api = {
        SL_API_VERSION,
        ApiLoaderVersion,
        ApiPanelOpen,
        ApiPanelClose,
        ApiPanelOn,
        ApiPanelSend,
        ApiPanelSendText,
        ApiPanelSendNumber,
        ApiPanelReload,
        ApiPanelShow,
        ApiPanelReady,
        ApiPrint,
        ApiCaptureInput,
        ApiCapturingInput,
        ApiModRegister,
        ApiModSettings,
        ApiModOnSettings,
        ApiModEnabled,
        ApiMenuSound,
    };
}

// The only export. A plugin asks for the version it was built against, and a
// newer loader is free to answer with an older table - which is why the
// version is a parameter and not something the caller reads off the struct.
extern "C" __declspec(dllexport) const SL_Api* __cdecl FRSModLoader_GetApi(unsigned version)
{
    if (version > SL_API_VERSION)
    {
        Log("plugin asked for host API v%u, this loader has v%u",
            version, (unsigned)SL_API_VERSION);
        return 0;
    }
    return &g_api;
}

namespace Host
{
    void Install()
    {
        InitializeCriticalSection(&g_lock);
        g_installed = true;
    }

    void Tick()
    {
        if (!g_installed || !CefHost::IsReady()) return;

        std::deque<Outbound> pending;

        {
            Guard guard;

            // Mounts first, so a panel opened and fed in the same frame does
            // not send into a page that is not there yet.
            for (size_t i = 0; i < g_panels.size(); i++)
            {
                Panel* p = g_panels[i];
                if (!p->closing && p->wantsMount)
                {
                    p->wantsMount = false;
                    p->mounted = true;
                    Queue("sl:mount", MountJson(p));
                    if (!p->visible)
                        Queue("sl:panel-show", "{\"id\":" + JsonString(p->id.c_str()) +
                                               ",\"show\":false}");
                }
            }

            pending.swap(g_out);

            // Closed panels are freed only here, on the game thread, after
            // their unmount has been queued: a callback must not be freed
            // while the queue still holds messages for it.
            for (size_t i = g_panels.size(); i-- > 0; )
            {
                if (g_panels[i]->closing)
                {
                    delete g_panels[i];
                    g_panels.erase(g_panels.begin() + i);
                }
            }
        }

        while (!pending.empty())
        {
            CefHost::SendToUi(pending.front().channel, pending.front().json);
            pending.pop_front();
        }
    }

    bool HandleUiMessage(const std::string& channel, const std::string& json)
    {
        if (!g_installed) return false;

        size_t colon = channel.find(':');
        if (colon == std::string::npos) return false;

        std::string id = channel.substr(0, colon);
        std::string name = channel.substr(colon + 1);

        // The page reloads on its own (F5, or a mod calling reload), and the
        // shell asks for the panels back when it does. Native panels are not
        // in mods.json - they exist only in this process - so this is the only
        // way they survive a reload.
        // frsmodloader.sound(name), from any page.
        if (id == "sl" && name == "sound")
        {
            // A JSON string: the name between its quotes.
            if (json.size() > 1 && json[0] == '"')
                MenuSound::Play(json.substr(1, json.size() - 2).c_str());
            return true;
        }

        if (id == "sl" && name == "panels")
        {
            Guard guard;
            for (size_t i = 0; i < g_panels.size(); i++)
            {
                if (g_panels[i]->closing) continue;
                g_panels[i]->mounted = false;
                g_panels[i]->wantsMount = true;
            }
            return true;
        }

        SL_MessageFn fn = 0;
        void* user = 0;

        {
            Guard guard;
            Panel* p = Find(id);
            if (!p || p->closing) return false;
            fn = p->onMessage;
            user = p->user;
        }

        // Outside the lock: the plugin is free to call back into us from here,
        // and it runs on the game thread, where its own hooks run.
        if (fn) fn(name.c_str(), json.c_str(), user);
        return true;
    }

    void Shutdown()
    {
        if (!g_installed) return;
        g_installed = false;

        EnterCriticalSection(&g_lock);
        for (size_t i = 0; i < g_panels.size(); i++) delete g_panels[i];
        g_panels.clear();
        g_out.clear();
        LeaveCriticalSection(&g_lock);
    }
}
