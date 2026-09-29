#include "CefHost.h"

#include "GameData.h"
#include "GameTextures.h"
#include "core/Config.h"
#include "core/Log.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"

#include <vector>

#define SL_MSG_TO_MOD "sl.toMod"
#define SL_MSG_TO_UI  "sl.toUi"

namespace
{
    // Overlay may ask for a frame before CEF is up (the game is drawing long
    // before that), so the lock is born with the DLL rather than in Init:
    // entering an uninitialised CRITICAL_SECTION takes the process down.
    struct FrameLock
    {
        CRITICAL_SECTION cs;
        FrameLock()  { InitializeCriticalSection(&cs); }
        ~FrameLock() { DeleteCriticalSection(&cs); }
    };
    FrameLock g_frameLockObj;
    CRITICAL_SECTION& g_frameLock = g_frameLockObj.cs;

    std::vector<unsigned char> g_frame;   // BGRA, premultiplied alpha
    int  g_frameW = 0, g_frameH = 0;
    bool g_frameDirty = false;

    int  g_viewW = 1280, g_viewH = 720;
    bool g_ready = false;
    CefHost::MessageFn g_onMessage;

    CefRefPtr<CefBrowser> g_browser;

    // ------------------------------------------------------------------
    class Client : public CefClient,
                   public CefRenderHandler,
                   public CefLifeSpanHandler,
                   public CefDisplayHandler,
                   public CefLoadHandler
    {
    public:
        CefRefPtr<CefRenderHandler>   GetRenderHandler() override   { return this; }
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefDisplayHandler>  GetDisplayHandler() override  { return this; }
        CefRefPtr<CefLoadHandler>     GetLoadHandler() override     { return this; }

        // ---- rendering (off-screen) ----
        void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
        {
            rect.Set(0, 0, g_viewW, g_viewH);
        }

        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                     const RectList&, const void* buffer,
                     int width, int height) override
        {
            if (type != PET_VIEW) return;   // popups (select, combo) are for v2

            static bool first = true;
            if (first)
            {
                first = false;
                LogCef("first OnPaint: %dx%d", width, height);
            }

            EnterCriticalSection(&g_frameLock);
            size_t bytes = (size_t)width * height * 4;
            if (g_frame.size() != bytes) g_frame.resize(bytes);
            memcpy(g_frame.data(), buffer, bytes);
            g_frameW = width;
            g_frameH = height;
            g_frameDirty = true;
            LeaveCriticalSection(&g_frameLock);
        }

        // The pointer over the page is the game's, drawn by Cursor. Left to
        // itself, CEF would SetCursor Windows' arrow (or a hand, or an
        // I-beam) on top of it.
        bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                            cef_cursor_type_t, const CefCursorInfo&) override
        {
            return true;
        }

        // ---- lifetime ----
        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
        {
            g_browser = browser;
            g_ready = true;
            LogCef("browser created (%dx%d)", g_viewW, g_viewH);
        }

        void OnBeforeClose(CefRefPtr<CefBrowser>) override
        {
            g_ready = false;
            g_browser = nullptr;
        }

        // ---- diagnostics ----
        bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t,
                              const CefString& message, const CefString& source,
                              int line) override
        {
            LogCef("console: %s (%s:%d)", message.ToString().c_str(),
                   source.ToString().c_str(), line);
            return false;
        }

        void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                         ErrorCode errorCode, const CefString& errorText,
                         const CefString& failedUrl) override
        {
            LogCef("FAILED to load %s: %s (%d)", failedUrl.ToString().c_str(),
                   errorText.ToString().c_str(), errorCode);
        }

        // ---- bridge ----
        bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                      CefProcessId,
                                      CefRefPtr<CefProcessMessage> msg) override
        {
            if (msg->GetName() != SL_MSG_TO_MOD) return false;
            if (g_onMessage)
            {
                CefRefPtr<CefListValue> a = msg->GetArgumentList();
                g_onMessage(a->GetString(0).ToString(), a->GetString(1).ToString());
            }
            return true;
        }

        IMPLEMENT_REFCOUNTING(Client);
    };

    CefRefPtr<Client> g_client;

    // ------------------------------------------------------------------
    // http://nfsu2.tex/<NAME> - the game's own textures, read out of its packs
    // by GameTextures. A page that wants the pill button asks for it by the
    // name the game uses, and gets the pixels the game draws.
    //
    // The response owns its bytes, so nothing outside has to outlive it.
    class TextureResponse : public CefResourceHandler
    {
    public:
        TextureResponse(bool found, std::string body, std::string mime)
            : found_(found), body_(std::move(body)), mime_(std::move(mime)) {}

        bool Open(CefRefPtr<CefRequest>, bool& handleRequest,
                  CefRefPtr<CefCallback>) override
        {
            handleRequest = true;
            return true;
        }

        void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& length,
                                CefString&) override
        {
            response->SetStatus(found_ ? 200 : 404);
            response->SetStatusText(found_ ? "OK" : "Not Found");
            response->SetMimeType(mime_);
            // The page is file://, so every fetch here is cross-origin, and
            // mask-image and canvas both insist on CORS for that.
            response->SetHeaderByName("Access-Control-Allow-Origin", "*", true);
            length = (int64_t)body_.size();
        }

        bool Read(void* out, int bytesToRead, int& bytesRead,
                  CefRefPtr<CefResourceReadCallback>) override
        {
            size_t n = body_.size() - offset_;
            if (n > (size_t)bytesToRead) n = (size_t)bytesToRead;
            memcpy(out, body_.data() + offset_, n);
            offset_ += n;
            bytesRead = (int)n;
            return n > 0;
        }

        void Cancel() override {}

    private:
        bool        found_;
        std::string body_, mime_;
        size_t      offset_ = 0;
        IMPLEMENT_REFCOUNTING(TextureResponse);
    };

    // http://nfsu2.tex/ serves textures and http://nfsu2.data/ the map data
    // (GameData): same response, a different reader behind each host.
    class TextureScheme : public CefSchemeHandlerFactory
    {
    public:
        typedef bool (*Getter)(const std::string&, std::string*, std::string*);
        explicit TextureScheme(Getter get) : get_(get) {}

        CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                             const CefString&,
                                             CefRefPtr<CefRequest> request) override
        {
            CefURLParts parts;
            if (!CefParseURL(request->GetURL(), parts)) return nullptr;
            std::string path = CefString(&parts.path).ToString();
            std::string query = CefString(&parts.query).ToString();
            if (!query.empty()) path += "?" + query;

            std::string body, mime = "text/plain";
            bool found = get_(path, &body, &mime);
            if (!found) { body = "not found"; mime = "text/plain"; }
            return new TextureResponse(found, std::move(body), std::move(mime));
        }

    private:
        Getter get_;
        IMPLEMENT_REFCOUNTING(TextureScheme);
    };

    // ------------------------------------------------------------------
    class App : public CefApp, public CefBrowserProcessHandler
    {
    public:
        CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
        {
            return this;
        }

        void OnBeforeCommandLineProcessing(const CefString& processType,
                                           CefRefPtr<CefCommandLine> cmd) override
        {
            if (!processType.empty()) return;

            // The game is a 2004 D3D9 title in the same process: letting
            // Chromium open a GPU context here only invites conflict. With
            // software compositing, OnPaint hands us BGRA directly, which is
            // exactly what Overlay wants.
            cmd->AppendSwitch("disable-gpu");
            cmd->AppendSwitch("disable-gpu-compositing");
            cmd->AppendSwitch("disable-gpu-vsync");
            cmd->AppendSwitch("no-sandbox");
            // shell.html is local and fetches each mod's UI fragment from its
            // own folder. Without this, Chromium treats every file as its own
            // origin and blocks the lot.
            cmd->AppendSwitch("allow-file-access-from-files");
            // Without these Chromium throttles itself whenever it believes it is
            // in the background - and with no window of its own, it always does.
            cmd->AppendSwitch("disable-renderer-backgrounding");
            cmd->AppendSwitch("disable-backgrounding-occluded-windows");
        }

        IMPLEMENT_REFCOUNTING(App);
    };
}

namespace CefHost
{
    bool Init(const char* url, int width, int height, bool devtoolsOnStart)
    {
        g_viewW = width;
        g_viewH = height;

        // libcef.dll lives in the FRSModLoader folder, not next to SPEED2.EXE.
        // It is delay-loaded (see CMakeLists) precisely so it can be loaded
        // here, by full path, before the first CEF call.
        char path[MAX_PATH];
        Config::Resolve("FRSModLoader\\libcef.dll", path, sizeof(path));

        // LOAD_WITH_ALTERED_SEARCH_PATH makes the DLL's own folder the search
        // root. Without it, Windows would look for chrome_elf.dll next to
        // SPEED2.EXE, where it is not, and the load fails with error 126.
        if (!LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH))
        {
            LogCef("FAILED to load %s (error %lu)", path, GetLastError());
            return false;
        }

        CefMainArgs args(GetModuleHandleA(NULL));

        CefSettings settings;
        settings.no_sandbox = true;
        settings.windowless_rendering_enabled = true;
        // The game has its own loop and will never pump a message for CEF, so
        // CEF runs its own message loop on another thread.
        settings.multi_threaded_message_loop = true;
        settings.log_severity = LOGSEVERITY_WARNING;

        char buf[MAX_PATH];
        Config::Resolve("FRSModLoader\\FRSModLoaderHelper.exe", buf, sizeof(buf));
        CefString(&settings.browser_subprocess_path) = buf;
        Config::Resolve("FRSModLoader", buf, sizeof(buf));
        CefString(&settings.resources_dir_path) = buf;
        Config::Resolve("FRSModLoader\\locales", buf, sizeof(buf));
        CefString(&settings.locales_dir_path) = buf;
        Config::Resolve("FRSModLoader\\cef_cache", buf, sizeof(buf));
        CefString(&settings.root_cache_path) = buf;
        CefString(&settings.cache_path) = buf;
        Config::Resolve("FRSModLoaderCef.log", buf, sizeof(buf));
        CefString(&settings.log_file) = buf;

        if (!CefInitialize(args, settings, new App(), nullptr))
        {
            LogCef("CefInitialize FAILED");
            return false;
        }
        LogCef("CEF initialised");

        // http is a standard scheme, so a factory for one host is all it takes:
        // no custom scheme to register in every subprocess.
        CefRegisterSchemeHandlerFactory("http", "nfsu2.tex", new TextureScheme(GameTextures::Get));
        CefRegisterSchemeHandlerFactory("http", "nfsu2.data", new TextureScheme(GameData::Get));

        CefWindowInfo windowInfo;
        windowInfo.SetAsWindowless(NULL);
        windowInfo.shared_texture_enabled = false;
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

        CefBrowserSettings browserSettings;
        browserSettings.windowless_frame_rate = 60;
        browserSettings.background_color = 0;   // transparent over the game

        g_client = new Client();
        if (!CefBrowserHost::CreateBrowser(windowInfo, g_client, url,
                                           browserSettings, nullptr, nullptr))
        {
            LogCef("CreateBrowser FAILED");
            return false;
        }

        if (devtoolsOnStart) ShowDevTools();
        return true;
    }

    void Shutdown()
    {
        if (g_browser) g_browser->GetHost()->CloseBrowser(true);
        g_browser = nullptr;
        g_client = nullptr;
        CefShutdown();
    }

    bool IsReady() { return g_ready && g_browser != nullptr; }

    void Resize(int width, int height)
    {
        if (width == g_viewW && height == g_viewH) return;
        g_viewW = width;
        g_viewH = height;
        if (g_browser) g_browser->GetHost()->WasResized();
        LogCef("resize %dx%d", width, height);
    }

    void LoadUrl(const char* url)
    {
        if (g_browser) g_browser->GetMainFrame()->LoadURL(url);
    }

    void Reload(bool ignoreCache)
    {
        if (!g_browser) return;
        if (ignoreCache) g_browser->ReloadIgnoreCache();
        else             g_browser->Reload();
    }

    void ShowDevTools()
    {
        if (!g_browser) return;
        CefWindowInfo info;
        info.SetAsPopup(NULL, "FRSModLoader DevTools");
        CefBrowserSettings settings;
        g_browser->GetHost()->ShowDevTools(info, nullptr, settings, CefPoint());
    }

    void SetMessageHandler(MessageFn fn) { g_onMessage = fn; }

    void SendToUi(const std::string& channel, const std::string& json)
    {
        if (!g_browser) return;
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(SL_MSG_TO_UI);
        msg->GetArgumentList()->SetString(0, channel);
        msg->GetArgumentList()->SetString(1, json);
        g_browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
    }

    bool LockFrame(const void** pixels, int* width, int* height)
    {
        EnterCriticalSection(&g_frameLock);
        if (!g_frameDirty || g_frame.empty())
        {
            LeaveCriticalSection(&g_frameLock);
            return false;
        }
        *pixels = g_frame.data();
        *width = g_frameW;
        *height = g_frameH;
        return true;   // stays locked until UnlockFrame
    }

    int AlphaAt(int x, int y)
    {
        int alpha = 0;
        EnterCriticalSection(&g_frameLock);
        if (x >= 0 && y >= 0 && x < g_frameW && y < g_frameH &&
            g_frame.size() >= (size_t)g_frameW * g_frameH * 4)
            alpha = g_frame[((size_t)y * g_frameW + x) * 4 + 3];
        LeaveCriticalSection(&g_frameLock);
        return alpha;
    }

    void UnlockFrame()
    {
        g_frameDirty = false;
        LeaveCriticalSection(&g_frameLock);
    }

    // ------------------------------------------------------------------ input
    static void FillModifiers(CefMouseEvent& e, bool leftDown)
    {
        e.modifiers = 0;
        if (leftDown)                         e.modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        if (GetKeyState(VK_SHIFT)   & 0x8000) e.modifiers |= EVENTFLAG_SHIFT_DOWN;
        if (GetKeyState(VK_CONTROL) & 0x8000) e.modifiers |= EVENTFLAG_CONTROL_DOWN;
        if (GetKeyState(VK_MENU)    & 0x8000) e.modifiers |= EVENTFLAG_ALT_DOWN;
    }

    void MouseMove(int x, int y, bool leftDown)
    {
        if (!g_browser) return;
        CefMouseEvent e;
        e.x = x; e.y = y;
        FillModifiers(e, leftDown);
        g_browser->GetHost()->SendMouseMoveEvent(e, false);
    }

    void MouseButton(int x, int y, int button, bool down, int clickCount)
    {
        if (!g_browser) return;
        CefMouseEvent e;
        e.x = x; e.y = y;
        FillModifiers(e, down && button == 0);
        cef_mouse_button_type_t type = button == 1 ? MBT_RIGHT
                                     : button == 2 ? MBT_MIDDLE : MBT_LEFT;
        g_browser->GetHost()->SendMouseClickEvent(e, type, !down, clickCount);
    }

    void MouseWheel(int x, int y, int delta)
    {
        if (!g_browser) return;
        CefMouseEvent e;
        e.x = x; e.y = y;
        FillModifiers(e, false);
        g_browser->GetHost()->SendMouseWheelEvent(e, 0, delta);
    }

    void MouseLeave()
    {
        if (!g_browser) return;
        CefMouseEvent e;
        e.x = -1; e.y = -1;
        e.modifiers = 0;
        g_browser->GetHost()->SendMouseMoveEvent(e, true);
    }

    void Key(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_browser) return;

        CefKeyEvent e;
        e.windows_key_code = (int)wParam;
        e.native_key_code = (int)lParam;
        e.is_system_key = (msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN ||
                           msg == WM_SYSKEYUP);
        e.modifiers = 0;
        if (GetKeyState(VK_SHIFT)   & 0x8000) e.modifiers |= EVENTFLAG_SHIFT_DOWN;
        if (GetKeyState(VK_CONTROL) & 0x8000) e.modifiers |= EVENTFLAG_CONTROL_DOWN;
        if (GetKeyState(VK_MENU)    & 0x8000) e.modifiers |= EVENTFLAG_ALT_DOWN;
        if (lParam & (1 << 24))               e.modifiers |= EVENTFLAG_IS_KEY_PAD;

        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)  e.type = KEYEVENT_RAWKEYDOWN;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) e.type = KEYEVENT_KEYUP;
        else                                            e.type = KEYEVENT_CHAR;

        g_browser->GetHost()->SendKeyEvent(e);
    }

    void SetFocus(bool focused)
    {
        if (g_browser) g_browser->GetHost()->SetFocus(focused);
    }
}
