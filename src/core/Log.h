#pragma once

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

// File log, next to SPEED2.EXE. Flushed on every line: the game crashes easily
// during reverse engineering, and the last line is exactly the one that
// matters. Inherited from Pops unchanged, except for the category prefix -
// there are several layers here (js, cef, d3d9) and without it the log turns
// into soup.
inline void LogTag(const char* tag, const char* fmt, ...)
{
    static FILE* f = 0;
    static DWORD t0 = 0;
    static CRITICAL_SECTION cs;
    static bool csReady = false;

    if (!csReady) { InitializeCriticalSection(&cs); csReady = true; }
    EnterCriticalSection(&cs);

    if (!f)
    {
        f = fopen("FRSModLoader.log", "w");
        t0 = GetTickCount();
    }
    if (f)
    {
        fprintf(f, "[%7.2f][%-4s] ", (GetTickCount() - t0) / 1000.0f, tag);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }

    LeaveCriticalSection(&cs);
}

#define Log(...)     LogTag("core", __VA_ARGS__)
#define LogJs(...)   LogTag("js",   __VA_ARGS__)
#define LogCef(...)  LogTag("cef",  __VA_ARGS__)
#define LogGfx(...)  LogTag("gfx",  __VA_ARGS__)
#define LogIn(...)   LogTag("in",   __VA_ARGS__)
