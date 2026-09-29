#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Configuration read from FRSModLoader.ini, next to the .asi (the game's
// scripts\ folder).
namespace Config
{
    // `inline`, not `static`: in a header, static would give every .cpp its own
    // copy and only dllmain.cpp's would ever be filled in - the others would
    // resolve paths starting from an empty string.
    inline char g_iniPath[MAX_PATH] = { 0 };
    inline char g_ownDir[MAX_PATH]  = { 0 };

    inline void Init(HMODULE self)
    {
        GetModuleFileNameA(self, g_iniPath, MAX_PATH);
        strcpy(g_ownDir, g_iniPath);
        char* slash = strrchr(g_ownDir, '\\');
        if (slash) *slash = 0;

        char* dot = strrchr(g_iniPath, '.');
        if (dot) strcpy(dot, ".ini");
    }

    inline int GetInt(const char* section, const char* key, int def)
    {
        return (int)GetPrivateProfileIntA(section, key, def, g_iniPath);
    }

    inline bool GetBool(const char* section, const char* key, bool def)
    {
        return GetInt(section, key, def ? 1 : 0) != 0;
    }

    inline float GetFloat(const char* section, const char* key, float def)
    {
        char buf[64], defbuf[64];
        sprintf(defbuf, "%g", def);
        GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf), g_iniPath);
        return (float)atof(buf);
    }

    // GetPrivateProfileInt only understands decimal; addresses and virtual-keys
    // read far better in hex, so these go through strtol instead.
    inline unsigned int GetHex(const char* section, const char* key,
                               unsigned int def)
    {
        char buf[64], defbuf[64];
        sprintf(defbuf, "0x%X", def);
        GetPrivateProfileStringA(section, key, defbuf, buf, sizeof(buf), g_iniPath);
        return (unsigned int)strtoul(buf, 0, 0);
    }

    inline void GetString(const char* section, const char* key, const char* def,
                          char* out, DWORD outSize)
    {
        GetPrivateProfileStringA(section, key, def, out, outSize, g_iniPath);
    }

    // The folder the .asi lives in, for resolving paths relative to it.
    inline const char* OwnDir() { return g_ownDir; }

    // Absolute path of something next to the .asi: Resolve("mods") gives
    // "<game>\scripts\FRSModLoader\mods" when `sub` is relative.
    inline void Resolve(const char* sub, char* out, DWORD outSize)
    {
        if (sub && (sub[1] == ':' || sub[0] == '\\'))
        {
            strncpy(out, sub, outSize - 1);
            out[outSize - 1] = 0;
            return;
        }
        _snprintf(out, outSize, "%s\\%s", g_ownDir, sub ? sub : "");
        out[outSize - 1] = 0;
    }
}
