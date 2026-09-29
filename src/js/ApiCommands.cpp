// speed.command and speed.print - the game's console.
//
// Two halves of the same thing: mods write lines with speed.print and the
// player answers with /commands. The bar and the history live in the UI shell;
// what lives here is the command registry and the path to it.
//
// Lines accept colour through markup: "burned {orange}12.4 L{/} of
// {yellow}ethanol{/}". The colour is picked when the line is written, by whoever
// knows what matters in it — a log all in one colour turns into a wall of
// text, and numbers are what you look for in a console during a race.
//
// speed.command - slash commands, typed over the game.
//
// A mod registers a name and a handler; the player presses "/" in the game, a
// bar opens over the screen, and whatever is typed is routed to the mod that
// owns the first word.
//
// The registry lives here rather than in each mod because commands are shared
// ground: the bar has to list what exists, and two mods must not both answer to
// "/fuel" without anyone noticing.
#include "Api.h"

#include "core/Log.h"
#include "ui/CefHost.h"
#include "ui/InputRouter.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    struct Command
    {
        Js::Mod* mod;
        JSValue  fn;        // owned: freed when the runtime tears down
        std::string help;
    };

    // Lower-case name -> command. One owner per name, first registration wins,
    // so a mod cannot quietly take over another's command.
    std::map<std::string, Command> g_commands;

    std::string Lower(const std::string& s)
    {
        std::string out = s;
        for (char& c : out) c = (char)tolower((unsigned char)c);
        return out;
    }

    std::vector<std::string> Split(const std::string& line)
    {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && isspace((unsigned char)line[i])) i++;
            size_t start = i;
            while (i < line.size() && !isspace((unsigned char)line[i])) i++;
            if (i > start) out.push_back(line.substr(start, i - start));
        }
        return out;
    }

    // speed.print("text {green}coloured{/}")
    JSValue Print(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_UNDEFINED;

        const char* text = JS_ToCString(ctx, argv[0]);
        if (!text) return JS_EXCEPTION;

        Js::Mod* mod = Js::Owner(ctx);

        // Wrapped as JSON so quotes and accents make it across intact.
        JSValue payload = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, payload, "mod",
                          JS_NewString(ctx, mod ? mod->id.c_str() : "sl"));
        JS_SetPropertyStr(ctx, payload, "text", JS_NewString(ctx, text));
        JS_FreeCString(ctx, text);

        JSValue json = JS_JSONStringify(ctx, payload, JS_UNDEFINED, JS_UNDEFINED);
        const char* enc = JS_ToCString(ctx, json);
        if (enc) { CefHost::SendToUi("sl:log", enc); JS_FreeCString(ctx, enc); }
        JS_FreeValue(ctx, json);
        JS_FreeValue(ctx, payload);
        return JS_UNDEFINED;
    }

    JSValue Register(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "speed.command(name, handler, help?)");

        const char* name = JS_ToCString(ctx, argv[0]);
        if (!name) return JS_EXCEPTION;

        std::string key = Lower(name);
        JS_FreeCString(ctx, name);

        auto existing = g_commands.find(key);
        if (existing != g_commands.end() && existing->second.mod != Js::Owner(ctx))
        {
            LogJs("command /%s already belongs to %s - ignoring",
                  key.c_str(), existing->second.mod->id.c_str());
            return JS_NewBool(ctx, false);
        }

        if (existing != g_commands.end())
            JS_FreeValue(ctx, existing->second.fn);   // same mod re-registering

        Command cmd;
        cmd.mod = Js::Owner(ctx);
        cmd.fn = JS_DupValue(ctx, argv[1]);

        if (argc >= 3)
        {
            const char* help = JS_ToCString(ctx, argv[2]);
            if (help) { cmd.help = help; JS_FreeCString(ctx, help); }
        }

        g_commands[key] = cmd;
        return JS_NewBool(ctx, true);
    }
}

namespace Js
{
    void RegisterCommands(JSContext* ctx, JSValue speed, Mod*)
    {
        JS_SetPropertyStr(ctx, speed, "command",
                          JS_NewCFunction(ctx, Register, "command", 3));
        JS_SetPropertyStr(ctx, speed, "print",
                          JS_NewCFunction(ctx, Print, "print", 1));
    }

    // Runs a typed line. The answer (a string, or nothing) goes back to the bar
    // so the player sees what happened without opening a log file.
    void RunCommand(const std::string& line)
    {
        std::vector<std::string> parts = Split(line);
        if (parts.empty()) return;

        std::string name = Lower(parts[0]);
        if (name == "help" || name == "ajuda" || name == "?")
        {
            std::string out = "commands: ";
            bool first = true;
            for (const auto& kv : g_commands)
            {
                if (!first) out += ", ";
                first = false;
                out += "/" + kv.first;
            }
            if (first) out = "no commands registered";
            CefHost::SendToUi("sl:command-reply", "\"" + out + "\"");
            return;
        }

        auto it = g_commands.find(name);
        if (it == g_commands.end())
        {
            CefHost::SendToUi("sl:command-reply",
                              "\"unknown command: /" + name + "\"");
            return;
        }

        JSContext* ctx = it->second.mod->ctx;

        JSValue args = JS_NewArray(ctx);
        for (size_t i = 1; i < parts.size(); i++)
            JS_SetPropertyUint32(ctx, args, (uint32_t)(i - 1),
                                 JS_NewString(ctx, parts[i].c_str()));

        JSValue argv[1] = { args };
        JSValue r = JS_Call(ctx, it->second.fn, JS_UNDEFINED, 1, argv);
        JS_FreeValue(ctx, args);

        if (JS_IsException(r))
        {
            ReportException(ctx, "command");
            CefHost::SendToUi("sl:command-reply", "\"/" + name + " failed\"");
        }
        else if (!JS_IsUndefined(r) && !JS_IsNull(r))
        {
            const char* text = JS_ToCString(ctx, r);
            if (text)
            {
                // The reply is JSON-encoded so quotes in it do not break the
                // message; JSONStringify on a string does exactly that.
                JSValue s = JS_NewString(ctx, text);
                JSValue j = JS_JSONStringify(ctx, s, JS_UNDEFINED, JS_UNDEFINED);
                const char* enc = JS_ToCString(ctx, j);
                CefHost::SendToUi("sl:command-reply", enc ? enc : "\"ok\"");
                if (enc) JS_FreeCString(ctx, enc);
                JS_FreeValue(ctx, j);
                JS_FreeValue(ctx, s);
                JS_FreeCString(ctx, text);
            }
        }

        JS_FreeValue(ctx, r);
    }

    void ClearCommands(JSContext* ctx)
    {
        for (auto it = g_commands.begin(); it != g_commands.end(); )
        {
            if (it->second.mod && it->second.mod->ctx == ctx)
            {
                JS_FreeValue(ctx, it->second.fn);
                it = g_commands.erase(it);
            }
            else ++it;
        }
    }
}
