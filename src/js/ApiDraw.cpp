// speed.draw - sprites inside the game's 3D scene.
//
// Unlike the interface: Chromium composites on top of everything, flat and
// with no depth. A flame at the exhaust has to stay where the car is, shrink
// with distance and disappear behind the bodywork when the camera turns - so it
// is drawn as geometry, in EndScene, with the camera the game already has set
// up.
#include "Api.h"

#include "ui/WorldDraw.h"

namespace
{
    double Num(JSContext* ctx, JSValueConst obj, const char* key, double fallback)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        double d = fallback;
        if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        return d;
    }

    // speed.draw.spark(x, y, z, { size, life, color })
    JSValue Spark(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 3)
            return JS_ThrowTypeError(ctx, "speed.draw.spark(x, y, z, options?)");

        double x = 0, y = 0, z = 0;
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
            JS_ToFloat64(ctx, &z, argv[2]))
            return JS_EXCEPTION;

        double size = 0.25, life = 0.25;
        uint32_t colour = 0xFF8020, endColour = 0;
        bool attached = false;
        double vx = 0, vy = 0, vz = 0;
        bool hasEndColour = false;

        if (argc >= 4 && JS_IsObject(argv[3]))
        {
            size = Num(ctx, argv[3], "size", size);
            life = Num(ctx, argv[3], "life", life);

            JSValue at = JS_GetPropertyStr(ctx, argv[3], "attached");
            attached = JS_ToBool(ctx, at) != 0;
            JS_FreeValue(ctx, at);

            JSValue vel = JS_GetPropertyStr(ctx, argv[3], "vel");
            if (JS_IsObject(vel))
                for (int i = 0; i < 3; i++)
                {
                    JSValue e = JS_GetPropertyUint32(ctx, vel, i);
                    double d = 0;
                    JS_ToFloat64(ctx, &d, e);
                    JS_FreeValue(ctx, e);
                    (i == 0 ? vx : i == 1 ? vy : vz) = d;
                }
            JS_FreeValue(ctx, vel);

            JSValue cf = JS_GetPropertyStr(ctx, argv[3], "color2");
            if (!JS_IsUndefined(cf))
            {
                int32_t n = 0;
                JS_ToInt32(ctx, &n, cf);
                endColour = (uint32_t)n;
                hasEndColour = true;
            }
            JS_FreeValue(ctx, cf);

            JSValue c = JS_GetPropertyStr(ctx, argv[3], "color");
            if (!JS_IsUndefined(c))
            {
                int32_t v = 0;
                JS_ToInt32(ctx, &v, c);
                colour = (uint32_t)v;
            }
            JS_FreeValue(ctx, c);
        }

        WorldDraw::Spawn((float)x, (float)y, (float)z, (float)size,
                         (float)life, colour, attached, (float)vx, (float)vy, (float)vz,
                         hasEndColour ? endColour : colour);
        return JS_UNDEFINED;
    }

    // speed.draw.camera({ view, fov, near, far, depth })
    //
    // The game does not hand its matrices to the fixed pipeline (it draws with
    // shaders), so the mod says where the view one is and which lens to draw
    // with. It lives in the mod, not hard-coded here, because the address is
    // knowledge the mod itself discovered and can adjust without rebuilding
    // the loader.
    JSValue Camera(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1 || !JS_IsObject(argv[0]))
            return JS_ThrowTypeError(ctx, "speed.draw.camera({ view, fov })");

        double view      = Num(ctx, argv[0], "view", 0);
        double fov       = Num(ctx, argv[0], "fov", 60);
        double nearPlane = Num(ctx, argv[0], "near", 0.1);
        double farPlane  = Num(ctx, argv[0], "far", 3000);

        JSValue d = JS_GetPropertyStr(ctx, argv[0], "depth");
        bool depth = JS_ToBool(ctx, d) != 0;
        JS_FreeValue(ctx, d);

        JSValue y = JS_GetPropertyStr(ctx, argv[0], "flipY");
        bool flipY = JS_ToBool(ctx, y) != 0;
        JS_FreeValue(ctx, y);

        JSValue a = JS_GetPropertyStr(ctx, argv[0], "auto");
        bool automatic = JS_IsUndefined(a) ? true : (JS_ToBool(ctx, a) != 0);
        JS_FreeValue(ctx, a);

        WorldDraw::Camera((uintptr_t)view, (float)fov, (float)nearPlane,
                          (float)farPlane, depth, flipY, automatic);
        return JS_UNDEFINED;
    }

    // speed.draw.texture(address) -> { width, height, format } or null
    JSValue Texture(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int64_t address = 0;
        if (argc >= 1 && JS_ToInt64(ctx, &address, argv[0])) return JS_EXCEPTION;

        unsigned width = 0, height = 0;
        int format = 0;
        if (!WorldDraw::UseTexture((uintptr_t)address, &width, &height, &format))
            return JS_NULL;

        if (!address) return JS_UNDEFINED;

        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, (int)width));
        JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, (int)height));
        JS_SetPropertyStr(ctx, o, "format", JS_NewInt32(ctx, format));
        return o;
    }

    // speed.draw.atlas(columns, rows)
    JSValue Atlas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int32_t columns = 1, rows = 1;
        if (argc >= 1) JS_ToInt32(ctx, &columns, argv[0]);
        if (argc >= 2) JS_ToInt32(ctx, &rows, argv[1]);
        WorldDraw::Atlas(columns, rows);
        return JS_UNDEFINED;
    }

    // speed.draw.anchor(x, y, z, [9 floats]) - the car's frame of reference
    JSValue Anchor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 4)
        {
            WorldDraw::Anchor(0, 0);
            return JS_UNDEFINED;
        }

        double x = 0, y = 0, z = 0;
        if (JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
            JS_ToFloat64(ctx, &z, argv[2]))
            return JS_EXCEPTION;

        float pos[3] = { (float)x, (float)y, (float)z };
        float rot[9];
        for (int i = 0; i < 9; i++)
        {
            JSValue v = JS_GetPropertyUint32(ctx, argv[3], i);
            double d = (i % 4 == 0) ? 1.0 : 0.0;
            JS_ToFloat64(ctx, &d, v);
            JS_FreeValue(ctx, v);
            rot[i] = (float)d;
        }

        WorldDraw::Anchor(pos, rot);
        return JS_UNDEFINED;
    }

    // speed.draw.textureFrom(address) - the field that holds the pointer
    JSValue TextureFrom(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int64_t address = 0;
        if (argc >= 1 && JS_ToInt64(ctx, &address, argv[0])) return JS_EXCEPTION;
        WorldDraw::TextureSource((uintptr_t)address);
        return JS_UNDEFINED;
    }

    JSValue TextureOk(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return JS_NewBool(ctx, WorldDraw::TextureValid()); }

    JSValue Clear(JSContext*, JSValueConst, int, JSValueConst*)
    { WorldDraw::Clear(); return JS_UNDEFINED; }

    JSValue Count(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return JS_NewInt32(ctx, WorldDraw::Count()); }
}

namespace Js
{
    void RegisterDraw(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue draw = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, draw, "spark", JS_NewCFunction(ctx, Spark, "spark", 4));
        JS_SetPropertyStr(ctx, draw, "camera", JS_NewCFunction(ctx, Camera, "camera", 1));
        JS_SetPropertyStr(ctx, draw, "texture", JS_NewCFunction(ctx, Texture, "texture", 1));
        JS_SetPropertyStr(ctx, draw, "textureOk",
                          JS_NewCFunction(ctx, TextureOk, "textureOk", 0));
        JS_SetPropertyStr(ctx, draw, "textureFrom",
                          JS_NewCFunction(ctx, TextureFrom, "textureFrom", 1));
        JS_SetPropertyStr(ctx, draw, "anchor", JS_NewCFunction(ctx, Anchor, "anchor", 4));
        JS_SetPropertyStr(ctx, draw, "atlas", JS_NewCFunction(ctx, Atlas, "atlas", 2));
        JS_SetPropertyStr(ctx, draw, "clear", JS_NewCFunction(ctx, Clear, "clear", 0));
        JS_SetPropertyStr(ctx, draw, "count", JS_NewCFunction(ctx, Count, "count", 0));

        JS_SetPropertyStr(ctx, speed, "draw", draw);
    }
}
