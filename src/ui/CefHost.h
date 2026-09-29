#pragma once

#include <windows.h>
#include <functional>
#include <string>

// FRSModLoader's Chromium layer: an off-screen browser inside the game process.
// CEF paints into memory (OnPaint) and Overlay is what puts that on screen, on
// the game thread, inside EndScene.
//
// All of CEF hides behind this interface. No other file in the project includes
// a CEF header, which keeps the rest compiling fast and makes swapping the
// layer later a contained job.
namespace CefHost
{
    // Called when the UI sends something to the native side
    // (frsmodloader.send). It arrives on the CEF thread; dispatching it to mod
    // JS is the Bridge queue's job, on the game thread.
    typedef std::function<void(const std::string& channel,
                               const std::string& json)> MessageFn;

    bool Init(const char* url, int width, int height, bool devtoolsOnStart);
    void Shutdown();

    void Resize(int width, int height);
    void LoadUrl(const char* url);
    void Reload(bool ignoreCache);
    void ShowDevTools();

    void SetMessageHandler(MessageFn fn);
    void SendToUi(const std::string& channel, const std::string& json);

    // The latest frame, BGRA with premultiplied alpha. Returns false if nothing
    // changed since the last call. CEF cannot paint between Lock and Unlock.
    bool LockFrame(const void** pixels, int* width, int* height);
    void UnlockFrame();

    // How opaque the latest frame is at (x, y), 0-255: whether the page has
    // something there or the game shows through. Any thread.
    int AlphaAt(int x, int y);

    // Input, forwarded by InputRouter. Coordinates in window pixels.
    void MouseMove(int x, int y, bool leftDown);
    void MouseButton(int x, int y, int button, bool down, int clickCount);
    void MouseWheel(int x, int y, int delta);
    void MouseLeave();
    void Key(UINT msg, WPARAM wParam, LPARAM lParam);
    void SetFocus(bool focused);

    bool IsReady();
}
