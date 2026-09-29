#include "GameData.h"

#include "core/Log.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include <map>
#include <vector>

// Same chunk format as the texture packs: [id u32][size u32][payload], the
// high bit of the id marking a chunk made of chunks.
//
// Road graph, TRACKS\ROUTES<region>\RoutesFreeRoam.bin, chunk 0x34121: one
// record per street, back to back, filling the chunk exactly (855 in L4RA):
//
//   +0x00  u32 0x0B, 0x0B
//   +0x10  char[32] name   "TrackRoutesA10"
//   +0x34  u16 point count (twice)
//   +0x60  f32 x4          plan box: min x, min y, max x, max y
//   +0x84  u8 1 = highway  (the ring and its approaches, 55 in L4RA)
//   +0x85  u8 1 = avenue   (the main streets)
//   +0x8C  points, 56 bytes each: f32 x, y; u8 +0x0E is 252 on a street and
//          248/253 on the short connectors beside them (read as alleys)
//
// It is the graph the GPS routes on: 0x5DEB80 loads this file's 0x34122 as
// the route nodes the planner walks ([0x883DB0], 32 bytes each). Unlike the
// race routes it has the alleys, and each street once.
//
// Career, GLOBAL\GLOBALB.BUN, the FIRST 0x80034A10 (the full career):
//
//   0x34A1D  string table
//   0x34A11  races, 136 bytes: u32[0] name offset into the strings,
//            u32[6] low 16 bits the track id, u32[10] the trigger's hash
//
// Trigger, a zone in 0x3414A of TRACKS\ROUTESL4RA\PathsFreeRoam.bin, found
// by its hash, which sits at +0x30 of the record:
//
//   +0x00  u32 zone type, 0x0D for a race trigger
//   +0x04  f32 x, y        the centre - where the start icon stands
//   +0x14  f32 z           height, following the ground
//   +0x20  f32 x4          plan box: min x, min y, max x, max y
namespace
{
    struct Lock
    {
        CRITICAL_SECTION cs;
        Lock()  { InitializeCriticalSection(&cs); }
        ~Lock() { DeleteCriticalSection(&cs); }
    };
    Lock g_lockObj;
    CRITICAL_SECTION& g_lock = g_lockObj.cs;
    std::map<std::string, std::string> g_cache;   // path -> JSON, never evicted

    std::string GameDir()
    {
        char dir[MAX_PATH];
        GetModuleFileNameA(NULL, dir, sizeof(dir));
        if (char* slash = strrchr(dir, '\\')) *slash = 0;
        return dir;
    }

    bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        _fseeki64(f, 0, SEEK_END);
        long long size = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        out.resize((size_t)size);
        bool ok = fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
    }

    uint32_t U32(const std::vector<uint8_t>& b, size_t at) { uint32_t v; memcpy(&v, &b[at], 4); return v; }
    uint16_t U16(const std::vector<uint8_t>& b, size_t at) { uint16_t v; memcpy(&v, &b[at], 2); return v; }
    float    F32(const std::vector<uint8_t>& b, size_t at) { float v;    memcpy(&v, &b[at], 4); return v; }

    struct Chunk { uint32_t id; size_t data, size; };

    // The chunks between `off` and `end`; one that runs past the end stops it.
    std::vector<Chunk> Chunks(const std::vector<uint8_t>& b, size_t off, size_t end)
    {
        std::vector<Chunk> out;
        while (off + 8 <= end)
        {
            uint32_t id = U32(b, off), size = U32(b, off + 4);
            if (off + 8 + (size_t)size > end) break;
            out.push_back({ id, off + 8, size });
            off += 8 + (size_t)size;
        }
        return out;
    }

    // The first chunk `id` anywhere under `off..end`, descending into containers.
    bool Find(const std::vector<uint8_t>& b, size_t off, size_t end, uint32_t id, Chunk* out)
    {
        for (const Chunk& c : Chunks(b, off, end))
        {
            if (c.id == id) { *out = c; return true; }
            if ((c.id & 0x80000000) && Find(b, c.data, c.data + c.size, id, out)) return true;
        }
        return false;
    }

    bool ValidRegion(const std::string& r)
    {
        if (r.size() != 4) return false;
        for (char c : r) if (!isupper((unsigned char)c) && !isdigit((unsigned char)c)) return false;
        return true;
    }

    void Num(std::string& j, float v)
    {
        char s[32];
        _snprintf(s, sizeof(s), "%.1f", v);
        j += s;
    }

    // ------------------------------------------------------------ roads
    std::string Roads(const std::string& region)
    {
        std::vector<uint8_t> b;
        std::string file = GameDir() + "\\TRACKS\\ROUTES" + region + "\\RoutesFreeRoam.bin";
        Chunk c;
        if (!ReadFile(file, b) || !Find(b, 0, b.size(), 0x34121, &c))
        {
            LogCef("data: no road graph in ROUTES%s\\RoutesFreeRoam.bin", region.c_str());
            return std::string();
        }

        std::string j = "{\"region\":\"" + region + "\",\"lines\":[";
        int lines = 0, points = 0;
        std::map<std::string, int> classes;
        size_t at = c.data, end = c.data + c.size;
        while (at + 0x8C <= end && U32(b, at) == 0x0B && U32(b, at + 4) == 0x0B)
        {
            size_t count = U16(b, at + 0x34);
            if (at + 0x8C + count * 56 > end) break;

            // The point byte +0x0E is 252 on nearly every street; the ones
            // where most points say something else are the short connectors
            // beside the streets that the TRACKMAP does not draw.
            int other = 0;
            for (size_t n = 0; n < count; n++) other += b[at + 0x8C + n * 56 + 0x0E] != 252;
            const char* cls = b[at + 0x84] ? "highway"
                            : other * 2 > (int)count ? "alley"
                            : b[at + 0x85] ? "avenue"
                            : "street";
            classes[cls]++;

            char head[48];
            _snprintf(head, sizeof(head), "%s{\"id\":%u,\"c\":\"", lines++ ? "," : "",
                      (unsigned)U16(b, at + 0x0A));
            j += head;
            j += cls;
            j += "\",\"p\":[";
            for (size_t n = 0; n < count; n++)
            {
                size_t p = at + 0x8C + n * 56;
                if (n) j += ",";
                Num(j, F32(b, p)); j += ","; Num(j, F32(b, p + 4));
            }
            j += "]}";
            points += (int)count;
            at += 0x8C + count * 56;
        }
        if (at != end)
            LogCef("data: %s road graph stopped 0x%X bytes before its end", region.c_str(), (unsigned)(end - at));

        LogCef("data: %s roads, %d streets (%d highway, %d avenue, %d street, %d alley), %d points",
               region.c_str(), lines, classes["highway"], classes["avenue"], classes["street"],
               classes["alley"], points);
        return j + "]}\n";
    }

    // ------------------------------------------------------------ graph
    // The GPS's nodes, 0x34122 (after a 4-byte lead, 32 bytes each): a node
    // is the passage from one street into another - street id +0x08 left at
    // its point +0x04, street id +0x0A entered at its point +0x06; the two
    // points are ~2 units apart. A route (a controller's +0x428) is a list of
    // these node indices, so drawing one is: along each street, from the
    // point it was entered to the point the next node leaves it.
    std::string Graph(const std::string& region)
    {
        std::vector<uint8_t> b;
        Chunk c;
        if (!ReadFile(GameDir() + "\\TRACKS\\ROUTES" + region + "\\RoutesFreeRoam.bin", b) ||
            !Find(b, 0, b.size(), 0x34122, &c) || c.size < 4)
            return std::string();
        std::string j = "{\"region\":\"" + region + "\",\"nodes\":[";
        size_t count = (c.size - 4) / 32;
        for (size_t i = 0; i < count; i++)
        {
            size_t at = c.data + 4 + i * 32;
            char n[64];
            _snprintf(n, sizeof(n), "%s[%u,%u,%u,%u]", i ? "," : "",
                      (unsigned)U16(b, at + 0x08), (unsigned)U16(b, at + 0x04),
                      (unsigned)U16(b, at + 0x0A), (unsigned)U16(b, at + 0x06));
            j += n;
        }
        LogCef("data: %s graph, %u nodes", region.c_str(), (unsigned)count);
        return j + "]}\n";
    }

    // ------------------------------------------------------------ events
    const char* const KINDS[] = { "CIRCUIT", "SPRINT", "DRIFT", "DRAG", "STREET", "URL", "SUV" };

    // The kind is in the race's name, as a whole word: S3_H_DRAG_6, STAGE_1_SPRINT_1.
    const char* KindOf(const std::string& name)
    {
        for (const char* k : KINDS)
        {
            size_t len = strlen(k);
            for (size_t p = name.find(k); p != std::string::npos; p = name.find(k, p + 1))
            {
                bool left  = p == 0 || name[p - 1] == '_';
                bool right = p + len == name.size() || name[p + len] == '_';
                if (left && right) return k;
            }
        }
        return 0;
    }

    uint32_t StringHash(const char* s)   // bStringHash
    {
        uint32_t h = 0xFFFFFFFF;
        for (; *s; s++) h = h * 33 + (uint8_t)*s;
        return h;
    }

    struct Race { std::string name; const char* kind; int track; uint32_t trigger; };

    std::string Events(const std::string& region)
    {
        // The career's race starts all live in the city's free roam.
        if (region != "L4RA") return std::string();
        std::string game = GameDir();

        std::vector<uint8_t> g, fr;
        if (!ReadFile(game + "\\GLOBAL\\GLOBALB.BUN", g))
        {
            LogCef("data: no GLOBAL\\GLOBALB.BUN");
            return std::string();
        }
        if (!ReadFile(game + "\\TRACKS\\ROUTESL4RA\\PathsFreeRoam.bin", fr))
        {
            LogCef("data: no PathsFreeRoam.bin");
            return std::string();
        }

        Chunk career = {}, strings = {}, list = {};
        bool found = false;
        for (const Chunk& c : Chunks(g, 0, g.size()))
            if (c.id == 0x80034A10) { career = c; found = true; break; }
        if (!found ||
            !Find(g, career.data, career.data + career.size, 0x34A1D, &strings) ||
            !Find(g, career.data, career.data + career.size, 0x34A11, &list))
        {
            LogCef("data: no career in GLOBALB.BUN");
            return std::string();
        }

        auto text = [&](uint32_t off) -> std::string {
            if (off >= strings.size) return std::string();
            const char* s = (const char*)&g[strings.data + off];
            return std::string(s, strnlen(s, strings.size - off));
        };

        // Trigger names by hash, for reading (EV_14, EVENT_LOCATOR_02).
        std::map<uint32_t, std::string> names;
        for (size_t p = 0; p < strings.size;)
        {
            std::string s = text((uint32_t)p);
            if (!s.empty()) names[StringHash(s.c_str())] = s;
            p += s.size() + 1;
        }

        std::vector<Race> races;
        std::map<int, const char*> byTrack;
        for (size_t i = 0; i < list.size / 136; i++)
        {
            size_t at = list.data + i * 136;
            Race r;
            r.name    = text(U32(g, at));
            r.track   = (int)(U32(g, at + 6 * 4) & 0xFFFF);
            r.trigger = U32(g, at + 10 * 4);
            r.kind    = KindOf(r.name);
            if (r.kind) byTrack[r.track] = r.kind;
            races.push_back(r);
        }

        // One start per trigger, in the career's order; unnamed races take the
        // kind of another race on the same track.
        std::vector<uint32_t> order;
        std::map<uint32_t, std::vector<const Race*>> starts;
        for (Race& r : races)
        {
            if (!r.kind) r.kind = byTrack.count(r.track) ? byTrack[r.track] : "CIRCUIT";
            if (!starts.count(r.trigger)) order.push_back(r.trigger);
            starts[r.trigger].push_back(&r);
        }

        std::string j = "{\"region\":\"L4RA\",\"events\":[";
        int count = 0, placed = 0;
        for (uint32_t hash : order)
        {
            // the hash sits at +0x30 of its zone; the first hit that reads as
            // a race trigger whose centre is inside its own box is the zone
            size_t zone = 0;
            for (size_t at = 0x30; at + 4 <= fr.size(); at++)
            {
                if (U32(fr, at) != hash) continue;
                size_t z = at - 0x30;
                float x = F32(fr, z + 4), y = F32(fr, z + 8);
                if (U32(fr, z) == 0x0D && F32(fr, z + 0x20) <= x && x <= F32(fr, z + 0x28) &&
                    F32(fr, z + 0x24) <= y && y <= F32(fr, z + 0x2C))
                {
                    zone = z;
                    break;
                }
            }
            if (!zone) continue;

            char head[160];
            auto name = names.find(hash);
            _snprintf(head, sizeof(head), "%s\n{\"trigger\":\"%s\",\"hash\":\"0x%08X\",\"x\":",
                      count++ ? "," : "",
                      name != names.end() ? name->second.c_str() : "", hash);
            j += head;
            Num(j, F32(fr, zone + 4));  j += ",\"y\":";
            Num(j, F32(fr, zone + 8));  j += ",\"z\":";
            Num(j, F32(fr, zone + 0x14));
            j += ",\"races\":[";
            bool first = true;
            for (const Race* r : starts[hash])
            {
                char line[160];
                _snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"track\":%d,\"kind\":\"%s\"}",
                          first ? "" : ",", r->name.c_str(), r->track, r->kind);
                j += line;
                first = false;
                placed++;
            }
            j += "]}";
        }
        LogCef("data: %d races, %d starts", placed, count);
        return j + "\n]}\n";
    }
    // ------------------------------------------------------------ shops
    // The career's shop list, 0x34A12, 160 bytes each: +0x00 the name
    // (UC_BODYSHOP), +0x3C the hash of its zone, +0x40 the front-end scene,
    // +0x50 the kind. The zone is a type 0x0E record in 0x3414A of
    // PathsFreeRoam.bin, hash at +0x30, centre at +0x04: where the beacon is.
    const char* ShopKind(uint32_t k)
    {
        switch (k)
        {
            case 0x101: return "PAINT";         // graphics
            case 0x102: return "BODY";          // body shop
            case 0x103: return "PERFORMANCE";
            case 0x105: return "SPECIALTY";     // car specialties: audio, neon...
            case 0x004: return "CARLOT";
            case 0x000: return "GARAGE";
            default:    return "OTHER";         // CC_MEGALOW (3), the Uniques (6)
        }
    }

    std::string Shops(const std::string& region)
    {
        if (region != "L4RA") return std::string();
        std::string game = GameDir();
        std::vector<uint8_t> g, fr;
        if (!ReadFile(game + "\\GLOBAL\\GLOBALB.BUN", g) ||
            !ReadFile(game + "\\TRACKS\\ROUTESL4RA\\PathsFreeRoam.bin", fr))
        {
            LogCef("data: shops need GLOBALB.BUN and PathsFreeRoam.bin");
            return std::string();
        }
        Chunk career = {}, list = {};
        bool found = false;
        for (const Chunk& c : Chunks(g, 0, g.size()))
            if (c.id == 0x80034A10) { career = c; found = true; break; }
        if (!found || !Find(g, career.data, career.data + career.size, 0x34A12, &list))
        {
            LogCef("data: no shop list in GLOBALB.BUN");
            return std::string();
        }

        std::string j = "{\"region\":\"L4RA\",\"shops\":[";
        int count = 0;
        for (size_t i = 0; i < list.size / 160; i++)
        {
            size_t at = list.data + i * 160;
            char name[57] = {};
            memcpy(name, &g[at], 56);
            uint32_t zone = U32(g, at + 0x3C), kind = U32(g, at + 0x50);

            size_t z = 0;
            for (size_t p = 0x30; p + 4 <= fr.size(); p++)
                if (U32(fr, p) == zone && U32(fr, p - 0x30) == 0x0E) { z = p - 0x30; break; }
            if (!z) continue;   // the Uniques are reached from a menu, not a zone

            char head[160];
            _snprintf(head, sizeof(head), "%s\n{\"name\":\"%s\",\"kind\":\"%s\",\"zone\":\"0x%08X\",\"x\":",
                      count++ ? "," : "", name, ShopKind(kind), zone);
            j += head;
            Num(j, F32(fr, z + 4));  j += ",\"y\":";
            Num(j, F32(fr, z + 8));  j += ",\"z\":";
            Num(j, F32(fr, z + 0x14));
            j += "}";
        }
        LogCef("data: %d shops", count);
        return j + "\n]}\n";
    }
}


namespace GameData
{
    bool Get(const std::string& path, std::string* body, std::string* mime)
    {
        // "/<REGION>/<what>.json"
        std::string p = path.substr(0, path.find('?'));
        while (!p.empty() && p[0] == '/') p.erase(0, 1);
        size_t slash = p.find('/');
        if (slash == std::string::npos) return false;
        std::string region = p.substr(0, slash), what = p.substr(slash + 1);
        for (char& c : region) c = (char)toupper((unsigned char)c);
        if (!ValidRegion(region)) return false;

        EnterCriticalSection(&g_lock);
        std::string key = region + "/" + what;
        auto cached = g_cache.find(key);
        bool ok = cached != g_cache.end();
        if (ok)
        {
            *body = cached->second;
        }
        else
        {
            std::string j = what == "roads.json"  ? Roads(region)
                          : what == "events.json" ? Events(region)
                          : what == "shops.json"  ? Shops(region)
                          : what == "graph.json"  ? Graph(region)
                          : std::string();
            ok = !j.empty();
            if (ok) *body = g_cache[key] = j;
            else    LogCef("data: no %s", path.c_str());
        }
        LeaveCriticalSection(&g_lock);
        *mime = "application/json";
        return ok;
    }
}
