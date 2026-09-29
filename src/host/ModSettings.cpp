#include "ModSettings.h"

#include "core/Config.h"
#include "core/Log.h"
#include "core/Version.h"
#include "js/JsRuntime.h"

#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    struct Native
    {
        std::string  id;
        std::string  manifest;   // as the plugin gave it
        std::string  values;     // kept for mod_settings, which returns a pointer
        SL_MessageFn fn = 0;
        void*        user = 0;
    };

    std::string                        g_modsDir;
    std::set<std::string>              g_on, g_off;   // choices made in the menu
    std::map<std::string, std::string> g_extra;     // id -> items, comma-joined
    std::vector<Native*>               g_natives;

    std::string DataDir(const char* sub = 0)
    {
        char dir[MAX_PATH];
        Config::Resolve("FRSModLoader\\data", dir, sizeof(dir));
        _mkdir(dir);
        std::string out = dir;
        if (sub)
        {
            out += "\\";
            out += sub;
            _mkdir(out.c_str());
        }
        return out;
    }

    bool ReadText(const std::string& path, std::string* out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = size >= 0 && size < 4 * 1024 * 1024;
        if (ok)
        {
            out->assign((size_t)size, '\0');
            ok = size == 0 || fread(&(*out)[0], 1, (size_t)size, f) == (size_t)size;
        }
        fclose(f);
        return ok;
    }

    // Written whole, through a temporary: a crash mid-write leaves the old
    // file, not half of the new one.
    bool WriteText(const std::string& path, const std::string& text)
    {
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        fclose(f);
        return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    std::string Quote(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if ((unsigned char)c < 0x20) out += ' ';
            else out += c;
        }
        return out + "\"";
    }

    std::string FileUrl(const std::string& path)
    {
        std::string url = "file:///";
        for (char c : path)
        {
            if (c == '\\')     url += '/';
            else if (c == ' ') url += "%20";
            else if (c == '#') url += "%23";
            else if (c == '?') url += "%3F";
            else               url += c;
        }
        return url;
    }

    // A mod id names a file here, so it stays a name.
    bool SafeId(const std::string& id)
    {
        if (id.empty() || id.size() > 64) return false;
        for (char c : id)
            if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
        return id != "." && id != "..";
    }

    // mods.json holds two lists of ids and nothing else, which is all this
    // reads back out of it: the quoted strings in the array after `key`.
    void ReadList(const std::string& text, const char* key, std::set<std::string>* out)
    {
        size_t at = text.find(std::string("\"") + key + "\"");
        if (at == std::string::npos) return;
        at = text.find('[', at);
        size_t end = text.find(']', at);
        while (at != std::string::npos && at < end)
        {
            size_t open = text.find('"', at + 1);
            if (open == std::string::npos || open > end) break;
            size_t close = text.find('"', open + 1);
            if (close == std::string::npos || close > end) break;
            out->insert(text.substr(open + 1, close - open - 1));
            at = close;
        }
    }

    std::string WriteList(const std::set<std::string>& ids)
    {
        std::string text = "[";
        for (const std::string& id : ids)
            text += (text.size() > 1 ? ", " : "") + Quote(id);
        return text + "]";
    }

    void LoadChoices()
    {
        std::string text;
        if (!ReadText(DataDir() + "\\mods.json", &text)) return;
        ReadList(text, "on", &g_on);
        ReadList(text, "off", &g_off);
    }

    void SaveChoices()
    {
        std::string text = "{ \"on\": " + WriteList(g_on) +
                           ", \"off\": " + WriteList(g_off) + " }\n";
        if (!WriteText(DataDir() + "\\mods.json", text))
            Log("mods: could not write %s\\mods.json", DataDir().c_str());
    }

    Native* FindNative(const std::string& id)
    {
        for (Native* n : g_natives)
            if (n->id == id) return n;
        return 0;
    }

    std::string Entry(const std::string& id, const std::string& manifest,
                      const std::string& base, bool native, bool running)
    {
        auto extra = g_extra.find(id);
        return "{\"id\":" + Quote(id) +
               ",\"native\":" + (native ? "true" : "false") +
               ",\"running\":" + (running ? "true" : "false") +
               ",\"choice\":" + (g_on.count(id) ? "true" : g_off.count(id) ? "false" : "null") +
               ",\"base\":" + Quote(base) +
               ",\"manifest\":" + manifest +
               ",\"values\":" + ModSettings::Values(id) +
               ",\"extra\":[" + (extra != g_extra.end() ? extra->second : "") + "]}";
    }
}

namespace ModSettings
{
    void Init(const char* modsDir)
    {
        g_modsDir = modsDir ? modsDir : "";
        LoadChoices();
        if (!g_off.empty())
            Log("mods: %d switched off in the Mods menu", (int)g_off.size());
    }

    bool Enabled(const std::string& id, bool byDefault)
    {
        if (g_on.count(id)) return true;
        if (g_off.count(id)) return false;
        return byDefault;
    }

    void SetEnabled(const std::string& id, bool enabled)
    {
        if (!SafeId(id)) return;
        (enabled ? g_on : g_off).insert(id);
        (enabled ? g_off : g_on).erase(id);
        SaveChoices();
        Log("mods: %s %s (from the next start)", id.c_str(), enabled ? "on" : "off");

        if (Native* n = FindNative(id))
            if (n->fn) n->fn("enabled", enabled ? "true" : "false", n->user);
    }

    std::string Values(const std::string& id)
    {
        std::string text;
        if (!SafeId(id) || !ReadText(DataDir("settings") + "\\" + id + ".json", &text))
            return "{}";
        size_t first = text.find_first_not_of(" \t\r\n");
        return first != std::string::npos && text[first] == '{' ? text : "{}";
    }

    void SetValues(const std::string& id, const std::string& json, bool notify)
    {
        if (!SafeId(id) || json.empty() || json[0] != '{') return;
        if (!WriteText(DataDir("settings") + "\\" + id + ".json", json))
        {
            Log("mods: could not save the settings of %s", id.c_str());
            return;
        }

        Native* n = FindNative(id);
        if (n) n->values = json;
        if (!notify) return;

        if (n) { if (n->fn) n->fn("settings", json.c_str(), n->user); }
        else   Js::SettingsChanged(id, json);
    }

    void Action(const std::string& id, const std::string& actionJson)
    {
        if (Native* n = FindNative(id))
        {
            if (n->fn) n->fn("action", actionJson.c_str(), n->user);
        }
        else
        {
            Js::SettingsAction(id, actionJson);
        }
    }

    void Define(const std::string& id, const std::string& schemaJson)
    {
        // An array: its items join the ones defined before.
        size_t open = schemaJson.find('['), close = schemaJson.rfind(']');
        if (open == std::string::npos || close == std::string::npos || close <= open) return;
        std::string items = schemaJson.substr(open + 1, close - open - 1);
        if (items.find_first_not_of(" \t\r\n") == std::string::npos) return;

        std::string& all = g_extra[id];
        all += (all.empty() ? "" : ",") + items;
    }

    std::string PanelJson()
    {
        std::string out = "{\"version\":\"" FRSMODLOADER_VERSION "\",\"mods\":[";
        bool first = true;

        // Every folder, not only what loaded: a mod switched off has to be
        // there to be switched back on.
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA((g_modsDir + "\\*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;

                std::string dir = g_modsDir + "\\" + fd.cFileName;
                std::string manifest, id;
                if (!ReadText(dir + "\\mod.json", &manifest)) continue;
                if (!Js::ParseManifest(manifest, fd.cFileName, &id)) continue;

                out += (first ? "" : ",") +
                       Entry(id, manifest, FileUrl(dir + "\\"), false, Js::IsRunning(id));
                first = false;
            }
            while (FindNextFileA(find, &fd));
            FindClose(find);
        }

        // A plugin's thumb is relative to scripts\, where its .asi lives.
        const std::string scripts = FileUrl(std::string(Config::OwnDir()) + "\\");
        for (Native* n : g_natives)
        {
            out += (first ? "" : ",") + Entry(n->id, n->manifest, scripts, true, true);
            first = false;
        }
        return out + "]}";
    }

    SL_Mod* RegisterNative(const char* manifestJson)
    {
        std::string manifest = manifestJson ? manifestJson : "";
        std::string id;
        if (!Js::ParseManifest(manifest, "", &id) || !SafeId(id))
        {
            Log("plugin: a manifest without a usable \"id\" was not registered");
            return 0;
        }

        Native* n = FindNative(id);
        if (!n)
        {
            n = new Native();
            n->id = id;
            g_natives.push_back(n);
        }
        n->manifest = manifest;
        n->values = Values(id);
        Log("plugin %s is in the Mods menu", id.c_str());
        return (SL_Mod*)n;
    }

    const char* NativeValues(SL_Mod* mod)
    {
        return mod ? ((Native*)mod)->values.c_str() : "{}";
    }

    void NativeOnSettings(SL_Mod* mod, SL_MessageFn fn, void* user)
    {
        if (!mod) return;
        ((Native*)mod)->fn = fn;
        ((Native*)mod)->user = user;
    }

    int NativeEnabled(SL_Mod* mod)
    {
        return mod && Enabled(((Native*)mod)->id, true) ? 1 : 0;
    }
}
