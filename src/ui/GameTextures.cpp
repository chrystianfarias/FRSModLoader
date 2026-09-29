#include "GameTextures.h"

#include "core/Config.h"
#include "core/Log.h"

#include "LzmaDec.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>

// The packs are Black Box's chunk format: [id u32][size u32][payload], with
// the high bit of the id marking a chunk that holds more chunks. A texture
// pack (TPK) is one of those:
//
//   B3300000  TPK
//     B3310000  header
//       33310004  one 124-byte entry per texture: name, hash, size, offsets
//       33310005  one 32-byte entry per texture: its D3D format
//     B3320000  data
//       33320002  the pixels, starting at the next 128-byte boundary
//
// Entry layout, as far as this reads it (offsets inside the 124 bytes):
//
//   +0x0C  char[24] name, truncated to 23 characters
//   +0x24  u32  hash of the full name (bStringHash)
//   +0x30  u32  pixel data, from the data base
//   +0x34  u32  palette, from the data base (P8 only)
//   +0x44  u16  width, u16 height
//   +0x4A  u8   format: 0x22 DXT1, 0x24 DXT3, 0x26 DXT5, 0x20 A8R8G8B8, 0x08 P8
//
// The mip chain follows the first level and is ignored: a page scales the
// texture itself, and the first level is the only one it needs.
//
// A .LZC is the same file compressed with JDLZ, and is inflated whole.
namespace
{
    enum Format { FMT_DXT1 = 0x22, FMT_DXT3 = 0x24, FMT_DXT5 = 0x26,
                  FMT_ARGB = 0x20, FMT_P8 = 0x08 };

    // Where one pack's bytes come from. A compressed pack has to be inflated
    // to be read at all, so it stays in memory; an uncompressed one is read
    // from disk a texture at a time, which spares the 8 MB of GLOBALB in a
    // 32-bit process that already carries Chromium.
    struct Pack
    {
        std::string          path;
        std::vector<uint8_t> mem;      // empty when read from disk
        uint64_t             size = 0;

        bool Read(uint64_t off, void* out, size_t n) const
        {
            if (off + n > size) return false;
            if (!mem.empty()) { memcpy(out, mem.data() + off, n); return true; }
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) return false;
            bool ok = _fseeki64(f, (long long)off, SEEK_SET) == 0 &&
                      fread(out, 1, n, f) == n;
            fclose(f);
            return ok;
        }
    };

    struct Tex
    {
        uint32_t hash;
        char     name[24];
        int      w, h, format;
        uint64_t data, palette;   // absolute, inside the pack
        int      pack;
    };

    // Born with the DLL, like CefHost's frame lock: the first request comes
    // from CEF's IO thread and must not be the one that initialises it.
    struct Lock
    {
        CRITICAL_SECTION cs;
        Lock()  { InitializeCriticalSection(&cs); }
        ~Lock() { DeleteCriticalSection(&cs); }
    };
    Lock               g_lockObj;
    CRITICAL_SECTION&  g_lock = g_lockObj.cs;
    bool               g_loaded = false;
    char               g_gameDir[MAX_PATH];
    std::vector<Pack>  g_packs;
    std::vector<Tex>   g_texs;
    std::map<uint32_t, size_t>     g_byHash;   // first pack wins
    std::map<std::string, std::string> g_cache; // request -> PNG
    // The interface's textures are small and kept for good. A map is not: a
    // 2048x2048 one is a 16 MB PNG, and a 32-bit process that already carries
    // Chromium cannot keep one per race. Only the latest big one stays.
    const size_t BIG = 4u << 20;
    std::string  g_bigKey;

    uint32_t StringHash(const char* s)   // the game's bStringHash
    {
        uint32_t h = 0xFFFFFFFF;
        for (; *s; s++) h = h * 33 + (uint8_t)*s;
        return h;
    }

    // ------------------------------------------------------------ JDLZ
    bool Jdlz(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
    {
        if (in.size() < 16 || memcmp(in.data(), "JDLZ", 4) != 0) return false;
        uint32_t ulen = *(const uint32_t*)&in[8];
        out.assign(ulen, 0);

        size_t i = 16, o = 0;
        unsigned f1 = 1, f2 = 1;
        while (i < in.size() && o < ulen)
        {
            if (f1 == 1) { f1 = in[i++] | 0x100; if (i >= in.size()) break; }
            if (f2 == 1) { f2 = in[i++] | 0x100; if (i >= in.size()) break; }
            if (f1 & 1)
            {
                if (i + 1 >= in.size()) break;
                size_t len, back;
                if (f2 & 1)
                {
                    len  = (in[i + 1] | ((in[i] & 0xF0) << 4)) + 3;
                    back = (in[i] & 0x0F) + 1;
                }
                else
                {
                    back = (in[i + 1] | ((in[i] & 0xE0) << 3)) + 17;
                    len  = (in[i] & 0x1F) + 3;
                }
                i += 2;
                if (back > o) return false;
                for (size_t k = 0; k < len && o < ulen; k++, o++) out[o] = out[o - back];
                f2 >>= 1;
            }
            else
            {
                out[o++] = in[i++];
            }
            f1 >>= 1;
        }
        return o == ulen;
    }

    // ------------------------------------------------------------ index
    void IndexTpk(int packIndex, uint64_t start, uint64_t end)
    {
        const Pack& p = g_packs[packIndex];
        uint64_t entries = 0, entriesSize = 0, formats = 0, formatsSize = 0, base = 0;

        for (uint64_t off = start; off + 8 <= end;)
        {
            uint32_t hdr[2];
            if (!p.Read(off, hdr, 8)) return;
            uint64_t body = off + 8, bodyEnd = body + hdr[1];
            if (hdr[0] == 0xB3310000 || hdr[0] == 0xB3320000)
            {
                for (uint64_t o2 = body; o2 + 8 <= bodyEnd;)
                {
                    uint32_t h2[2];
                    if (!p.Read(o2, h2, 8)) return;
                    if (h2[0] == 0x33310004) { entries = o2 + 8; entriesSize = h2[1]; }
                    if (h2[0] == 0x33310005) { formats = o2 + 8; formatsSize = h2[1]; }
                    if (h2[0] == 0x33320002) base = (o2 + 8 + 0x7F) & ~(uint64_t)0x7F;
                    o2 += 8 + (uint64_t)h2[1];
                }
            }
            off = bodyEnd;
        }
        if (!entries || !base) return;

        size_t count = (size_t)(entriesSize / 124);
        std::vector<uint8_t> e((size_t)entriesSize), fmt((size_t)formatsSize);
        if (!p.Read(entries, e.data(), e.size())) return;
        if (formatsSize && !p.Read(formats, fmt.data(), fmt.size())) fmt.clear();

        for (size_t k = 0; k < count; k++)
        {
            const uint8_t* r = &e[k * 124];
            Tex t = {};
            memcpy(t.name, r + 0x0C, 23);
            t.hash    = *(const uint32_t*)(r + 0x24);
            t.data    = base + *(const uint32_t*)(r + 0x30);
            t.palette = base + *(const uint32_t*)(r + 0x34);
            t.w       = *(const uint16_t*)(r + 0x44);
            t.h       = *(const uint16_t*)(r + 0x46);
            t.format  = r[0x4A];
            t.pack    = packIndex;

            // The format table says the same thing in D3D terms; where the
            // two disagree the FourCC is the one D3D would have been given.
            if (fmt.size() >= (k + 1) * 32)
            {
                uint32_t cc = *(const uint32_t*)&fmt[k * 32 + 20];
                if (cc == 0x31545844)      t.format = FMT_DXT1;   // "DXT1"
                else if (cc == 0x33545844) t.format = FMT_DXT3;
                else if (cc == 0x35545844) t.format = FMT_DXT5;
                else if (cc == 21)     t.format = FMT_ARGB;   // D3DFMT_A8R8G8B8
                else if (cc == 41)     t.format = FMT_P8;     // D3DFMT_P8
            }
            if (!t.w || !t.h) continue;

            g_texs.push_back(t);
            g_byHash.emplace(t.hash, g_texs.size() - 1);
        }
    }

    void LoadPack(const char* gameDir, const char* const* candidates)
    {
        for (; *candidates; candidates++)
        {
            char path[MAX_PATH];
            _snprintf(path, sizeof(path), "%s\\%s", gameDir, *candidates);
            FILE* f = fopen(path, "rb");
            if (!f) continue;

            Pack p;
            p.path = path;
            char magic[4] = {};
            fread(magic, 1, 4, f);
            _fseeki64(f, 0, SEEK_END);
            p.size = (uint64_t)_ftelli64(f);

            if (memcmp(magic, "JDLZ", 4) == 0)
            {
                std::vector<uint8_t> packed((size_t)p.size);
                _fseeki64(f, 0, SEEK_SET);
                bool read = fread(packed.data(), 1, packed.size(), f) == packed.size();
                fclose(f);
                if (!read || !Jdlz(packed, p.mem))
                {
                    LogCef("textures: %s does not inflate", path);
                    return;
                }
                p.size = p.mem.size();
            }
            else
            {
                fclose(f);
            }

            g_packs.push_back(std::move(p));
            int index = (int)g_packs.size() - 1;
            size_t before = g_texs.size();

            for (uint64_t off = 0; off + 8 <= g_packs[index].size;)
            {
                uint32_t hdr[2];
                if (!g_packs[index].Read(off, hdr, 8)) break;
                if (hdr[0] == 0xB3300000) IndexTpk(index, off + 8, off + 8 + hdr[1]);
                off += 8 + (uint64_t)hdr[1];
            }
            LogCef("textures: %s, %u textures%s", *candidates,
                   (unsigned)(g_texs.size() - before),
                   g_packs[index].mem.empty() ? "" : " (inflated)");
            return;   // the first candidate that exists is the one the game reads
        }
    }

    void LoadAll()
    {
        g_loaded = true;

        GetModuleFileNameA(NULL, g_gameDir, sizeof(g_gameDir));
        if (char* slash = strrchr(g_gameDir, '\\')) *slash = 0;
        const char* gameDir = g_gameDir;

        // The packs the interface is drawn from, in the order a hash is looked
        // up. Each is a list of names the game may have it under.
        static const char* const front[]  = { "FRONTEND\\FRONTB.LZC", "FRONTEND\\FRONTB.BUN", 0 };
        static const char* const global[] = { "GLOBAL\\GLOBALB.LZC", "GLOBAL\\GLOBALB.BUN", 0 };
        static const char* const ingame[] = { "GLOBAL\\INGAMECOMMON.LZC", "GLOBAL\\INGAMECOMMON.BUN", 0 };
        LoadPack(gameDir, front);
        LoadPack(gameDir, global);
        LoadPack(gameDir, ingame);
    }

    // ------------------------------------------------------------ decode
    // Everything comes out as straight-alpha RGBA, which is what PNG wants.
    void Rgb565(uint16_t c, uint8_t* out)
    {
        out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
        out[2] = (uint8_t)((c & 31) * 255 / 31);
    }

    void DecodeDxt(const uint8_t* src, int w, int h, int format, uint8_t* rgba)
    {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            const uint8_t* alpha = format == FMT_DXT1 ? 0 : src;
            const uint8_t* color = format == FMT_DXT1 ? src : src + 8;
            src += format == FMT_DXT1 ? 8 : 16;

            uint16_t c0 = *(const uint16_t*)color, c1 = *(const uint16_t*)(color + 2);
            uint8_t pal[4][4];
            Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 255;
            if (c0 > c1 || format != FMT_DXT1)
            {
                for (int k = 0; k < 3; k++)
                {
                    pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
                    pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
                }
                pal[2][3] = pal[3][3] = 255;
            }
            else
            {
                for (int k = 0; k < 3; k++) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
                pal[2][3] = 255;
                pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
            }

            uint8_t a5[8] = {};
            if (format == FMT_DXT5)
            {
                a5[0] = alpha[0]; a5[1] = alpha[1];
                if (a5[0] > a5[1])
                    for (int k = 1; k < 7; k++) a5[k + 1] = (uint8_t)(((7 - k) * a5[0] + k * a5[1]) / 7);
                else
                {
                    for (int k = 1; k < 5; k++) a5[k + 1] = (uint8_t)(((5 - k) * a5[0] + k * a5[1]) / 5);
                    a5[6] = 0; a5[7] = 255;
                }
            }

            uint32_t bits = *(const uint32_t*)(color + 4);
            uint64_t abits = 0;
            if (format == FMT_DXT5) for (int k = 0; k < 6; k++) abits |= (uint64_t)alpha[2 + k] << (8 * k);

            for (int py = 0; py < 4; py++)
            for (int px = 0; px < 4; px++)
            {
                int x = bx * 4 + px, y = by * 4 + py, i = py * 4 + px;
                if (x >= w || y >= h) continue;
                uint8_t* d = rgba + ((size_t)y * w + x) * 4;
                const uint8_t* c = pal[(bits >> (2 * i)) & 3];
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];
                if (format == FMT_DXT3)
                {
                    uint8_t nib = (alpha[i / 2] >> ((i & 1) * 4)) & 15;
                    d[3] = (uint8_t)(nib * 17);
                }
                else if (format == FMT_DXT5)
                {
                    d[3] = a5[(abits >> (3 * i)) & 7];
                }
            }
        }
    }

    bool Decode(const Tex& t, std::vector<uint8_t>& rgba)
    {
        const Pack& p = g_packs[t.pack];
        size_t pixels = (size_t)t.w * t.h;
        rgba.assign(pixels * 4, 0);

        if (t.format == FMT_DXT1 || t.format == FMT_DXT3 || t.format == FMT_DXT5)
        {
            size_t blocks = (size_t)((t.w + 3) / 4) * ((t.h + 3) / 4);
            std::vector<uint8_t> src(blocks * (t.format == FMT_DXT1 ? 8 : 16));
            if (!p.Read(t.data, src.data(), src.size())) return false;
            DecodeDxt(src.data(), t.w, t.h, t.format, rgba.data());

            // The TRACKMAPs are DXT3 with every alpha nibble at zero: the game
            // draws them opaque, so an alpha block that holds nothing does not
            // mean a texture that shows nothing.
            if (t.format == FMT_DXT3)
            {
                bool any = false;
                for (size_t i = 3; i < rgba.size() && !any; i += 4) any = rgba[i] != 0;
                if (!any) for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            }
            return true;
        }
        if (t.format == FMT_ARGB)
        {
            std::vector<uint8_t> src(pixels * 4);
            if (!p.Read(t.data, src.data(), src.size())) return false;
            for (size_t i = 0; i < pixels; i++)
            {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = src[i * 4 + 3];
            }
            return true;
        }
        if (t.format == FMT_P8)
        {
            std::vector<uint8_t> src(pixels), pal(1024);
            if (!p.Read(t.data, src.data(), src.size())) return false;
            if (!p.Read(t.palette, pal.data(), pal.size())) return false;
            for (size_t i = 0; i < pixels; i++)
            {
                const uint8_t* c = &pal[src[i] * 4];   // BGRA
                rgba[i * 4 + 0] = c[2];
                rgba[i * 4 + 1] = c[1];
                rgba[i * 4 + 2] = c[0];
                rgba[i * 4 + 3] = c[3];
            }
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------ PNG
    // Stored (uncompressed) deflate: the textures are small, they never leave
    // the process, and it keeps zlib out of the build.
    uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFF)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t v = i;
                for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320 ^ (v >> 1) : v >> 1;
                table[i] = v;
            }
            ready = true;
        }
        for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c;
    }

    void Be32(std::string& s, uint32_t v)
    {
        s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
    }

    void Chunk(std::string& png, const char* type, const std::string& data)
    {
        Be32(png, (uint32_t)data.size());
        std::string body = std::string(type, 4) + data;
        png += body;
        Be32(png, Crc((const uint8_t*)body.data(), body.size()) ^ 0xFFFFFFFF);
    }

    std::string Png(const std::vector<uint8_t>& rgba, int w, int h)
    {
        std::string raw;
        raw.reserve(((size_t)w * 4 + 1) * h);
        for (int y = 0; y < h; y++)
        {
            raw += '\0';   // filter: none
            raw.append((const char*)&rgba[(size_t)y * w * 4], (size_t)w * 4);
        }

        std::string z = "\x78\x01";
        uint32_t a = 1, b = 0;
        for (size_t off = 0; off < raw.size() || off == 0;)
        {
            size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
            bool last = off + n == raw.size();
            z += (char)(last ? 1 : 0);
            z += (char)(n & 0xFF); z += (char)(n >> 8);
            z += (char)(~n & 0xFF); z += (char)((~n >> 8) & 0xFF);
            z.append(raw, off, n);
            off += n;
            if (last) break;
        }
        for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
        Be32(z, (b << 16) | a);

        std::string ihdr;
        Be32(ihdr, (uint32_t)w); Be32(ihdr, (uint32_t)h);
        ihdr += "\x08\x06\x00\x00\x00";   // 8-bit RGBA
        ihdr.resize(13);

        std::string png = "\x89PNG\r\n\x1a\n";
        Chunk(png, "IHDR", ihdr);
        Chunk(png, "IDAT", z);
        Chunk(png, "IEND", "");
        return png;
    }

    // ------------------------------------------------------------ requests
    const Tex* Find(std::string name)
    {
        for (char& c : name) c = (char)toupper((unsigned char)c);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".PNG") == 0)
            name.resize(name.size() - 4);
        if (name.empty()) return 0;

        // A race's minimap is a pack of its own, TRACKS\TRACKMAP<id>.BIN,
        // holding one texture of the same name. There are 150 of them and a
        // page wants one or two, so a pack is read the first time it is asked
        // for rather than at startup. TRACKMAP4000 is the free-roam city.
        if (name.size() == 12 && name.compare(0, 8, "TRACKMAP") == 0 &&
            name.find_first_not_of("0123456789", 8) == std::string::npos &&
            g_byHash.find(StringHash(name.c_str())) == g_byHash.end())
        {
            std::string file = "TRACKS\\" + name + ".BIN";
            const char* const candidates[] = { file.c_str(), 0 };
            LoadPack(g_gameDir, candidates);
        }

        uint32_t hash;
        if (name.size() > 2 && name[0] == '0' && name[1] == 'X')
            hash = strtoul(name.c_str() + 2, 0, 16);
        else
            hash = StringHash(name.c_str());

        auto it = g_byHash.find(hash);
        if (it != g_byHash.end()) return &g_texs[it->second];

        // The name as the pack stores it: cut at 23 characters, which is what
        // index.json lists. Several can share one, and the first is taken.
        for (const Tex& t : g_texs)
            if (_stricmp(t.name, name.c_str()) == 0) return &t;
        return 0;
    }

    std::string Param(const std::string& query, const char* key)
    {
        size_t klen = strlen(key);
        for (size_t pos = 0; pos < query.size();)
        {
            size_t amp = query.find('&', pos);
            if (amp == std::string::npos) amp = query.size();
            std::string pair = query.substr(pos, amp - pos);
            if (pair.compare(0, klen, key) == 0 &&
                (pair.size() == klen || pair[klen] == '='))
                return pair.size() > klen ? pair.substr(klen + 1) : std::string("1");
            pos = amp + 1;
        }
        return std::string();
    }

    // What FEng does to every quad it draws: texture times colour. The white
    // pieces of the interface are meant to be tinted, and the game tints them.
    void Tint(std::vector<uint8_t>& rgba, const std::string& hex)
    {
        std::string h = hex;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() != 6 && h.size() != 8) return;
        uint32_t v = strtoul(h.c_str(), 0, 16);
        uint32_t r = h.size() == 8 ? (v >> 24) & 255 : (v >> 16) & 255;
        uint32_t g = h.size() == 8 ? (v >> 16) & 255 : (v >> 8) & 255;
        uint32_t b = h.size() == 8 ? (v >> 8) & 255 : v & 255;
        uint32_t a = h.size() == 8 ? v & 255 : 255;
        for (size_t i = 0; i < rgba.size(); i += 4)
        {
            rgba[i + 0] = (uint8_t)(rgba[i + 0] * r / 255);
            rgba[i + 1] = (uint8_t)(rgba[i + 1] * g / 255);
            rgba[i + 2] = (uint8_t)(rgba[i + 2] * b / 255);
            rgba[i + 3] = (uint8_t)(rgba[i + 3] * a / 255);
        }
    }

    // The same quarter corner or arrow is drawn mirrored all over the menus;
    // the game flips UVs, a page asks for ?flip=h, v or hv.
    void Flip(std::vector<uint8_t>& rgba, int w, int h, const std::string& how)
    {
        bool fh = how.find('h') != std::string::npos;
        bool fv = how.find('v') != std::string::npos;
        std::vector<uint8_t> out(rgba.size());
        for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int sx = fh ? w - 1 - x : x, sy = fv ? h - 1 - y : y;
            memcpy(&out[((size_t)y * w + x) * 4], &rgba[((size_t)sy * w + sx) * 4], 4);
        }
        rgba.swap(out);
    }

    // The menus' rounded boxes are built from one quarter: a 16x16 corner that
    // FEng draws four times, rotated, around strips of a flat box. ?mirror
    // puts the four together - the texture as the bottom-right quarter, the
    // others its reflections - which is a whole box a page can nine-slice.
    void Mirror(std::vector<uint8_t>& rgba, int& w, int& h)
    {
        int W = w * 2, H = h * 2;
        std::vector<uint8_t> out((size_t)W * H * 4);
        for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            int sx = x < w ? w - 1 - x : x - w;
            int sy = y < h ? h - 1 - y : y - h;
            memcpy(&out[((size_t)y * W + x) * 4], &rgba[((size_t)sy * w + sx) * 4], 4);
        }
        rgba.swap(out);
        w = W;
        h = H;
    }

    // The menus get their depth of black by drawing the same translucent box
    // two or three times over itself. ?stack=N is that, done once: N copies
    // of a colour over itself keep the colour and take alpha 1 - (1 - a)^N.
    void Stack(std::vector<uint8_t>& rgba, int n)
    {
        if (n < 2) return;
        if (n > 8) n = 8;
        for (size_t i = 3; i < rgba.size(); i += 4)
        {
            double clear = 1.0 - rgba[i] / 255.0, left = 1.0;
            for (int k = 0; k < n; k++) left *= clear;
            rgba[i] = (uint8_t)((1.0 - left) * 255.0 + 0.5);
        }
    }

    // ------------------------------------------------------------ mod zip
    // A .dds a mod asks for that is not a loose file may be inside a zip in the
    // mod's own root - a texture pack left as downloaded, "NFSU2 Detailed Map"
    // for one. The entry is found by file name, anywhere in the zip; where the
    // pack has variants, one under a "Default" folder wins and one under "Beta"
    // is never taken. Entries stored (0) or LZMA (14, what 7-Zip writes) are
    // read; the central directory of each zip is read once and kept.
    //
    //   end of central dir  50 4B 05 06: +10 u16 entries, +12 u32 size, +16 u32 offset
    //   central entry       50 4B 01 02: +10 method, +20 packed, +24 size,
    //                       +28 name len, +30 extra, +32 comment, +42 local
    //   local header        50 4B 03 04: +26 name len, +28 extra, data after
    //   LZMA entry data     u8 ver, u8 ver, u16 props size (5), props, stream
    struct ZipEntry { std::string name; uint16_t method; uint32_t packed, size, local; };
    struct ZipIndex { std::string path; std::vector<ZipEntry> entries; };
    std::map<std::string, std::vector<ZipIndex>> g_zips;   // mod dir -> its zips

    uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
    uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

    bool ReadAt(FILE* f, uint64_t at, void* out, size_t n)
    {
        return _fseeki64(f, (long long)at, SEEK_SET) == 0 && fread(out, 1, n, f) == n;
    }

    ZipIndex IndexZip(const std::string& path)
    {
        ZipIndex z;
        z.path = path;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return z;
        _fseeki64(f, 0, SEEK_END);
        uint64_t size = (uint64_t)_ftelli64(f);
        size_t tail = (size_t)(size < 0x10000 + 22 ? size : 0x10000 + 22);
        std::vector<uint8_t> t(tail);
        if (tail >= 22 && ReadAt(f, size - tail, t.data(), tail))
        {
            // the end record is the last 50 4B 05 06, behind an optional comment
            for (size_t i = tail - 22 + 1; i-- > 0;)
            {
                if (Le32(&t[i]) != 0x06054B50) continue;
                uint16_t count = Le16(&t[i + 10]);
                uint32_t dirSize = Le32(&t[i + 12]), dirAt = Le32(&t[i + 16]);
                std::vector<uint8_t> dir(dirSize);
                if (ReadAt(f, dirAt, dir.data(), dirSize))
                    for (size_t p = 0, n = 0; n < count && p + 46 <= dir.size(); n++)
                    {
                        if (Le32(&dir[p]) != 0x02014B50) break;
                        uint16_t nameLen = Le16(&dir[p + 28]);
                        if (p + 46 + nameLen > dir.size()) break;
                        ZipEntry e;
                        e.method = Le16(&dir[p + 10]);
                        e.packed = Le32(&dir[p + 20]);
                        e.size   = Le32(&dir[p + 24]);
                        e.local  = Le32(&dir[p + 42]);
                        e.name.assign((const char*)&dir[p + 46], nameLen);
                        z.entries.push_back(e);
                        p += 46 + nameLen + Le16(&dir[p + 30]) + Le16(&dir[p + 32]);
                    }
                break;
            }
        }
        fclose(f);
        LogCef("textures: zip %s, %u entries", path.c_str(), (unsigned)z.entries.size());
        return z;
    }

    void* LzmaAlloc(ISzAllocPtr, size_t n) { return malloc(n); }
    void  LzmaFree(ISzAllocPtr, void* p) { free(p); }

    bool ReadEntry(const ZipIndex& z, const ZipEntry& e, std::vector<uint8_t>& out)
    {
        FILE* f = fopen(z.path.c_str(), "rb");
        if (!f) return false;
        uint8_t lh[30];
        bool ok = ReadAt(f, e.local, lh, 30) && Le32(lh) == 0x04034B50;
        std::vector<uint8_t> packed(e.packed);
        if (ok) ok = ReadAt(f, (uint64_t)e.local + 30 + Le16(&lh[26]) + Le16(&lh[28]), packed.data(), packed.size());
        fclose(f);
        if (!ok) return false;

        if (e.method == 0) { out.swap(packed); return out.size() == e.size; }
        if (e.method != 14 || packed.size() < 4)
        {
            LogCef("textures: %s in %s uses zip method %u, not read", e.name.c_str(), z.path.c_str(), e.method);
            return false;
        }
        uint16_t propsSize = Le16(&packed[2]);
        if (propsSize != LZMA_PROPS_SIZE || packed.size() < 4u + propsSize) return false;
        out.resize(e.size);
        SizeT outLen = e.size, inLen = packed.size() - 4 - propsSize;
        ELzmaStatus status;
        ISzAlloc alloc = { LzmaAlloc, LzmaFree };
        SRes r = LzmaDecode(out.data(), &outLen, &packed[4 + propsSize], &inLen,
                            &packed[4], propsSize, LZMA_FINISH_ANY, &status, &alloc);
        return r == SZ_OK && outLen == e.size;
    }

    bool FromModZip(const std::string& modDir, const std::string& file, std::vector<uint8_t>& out)
    {
        auto found = g_zips.find(modDir);
        if (found == g_zips.end())
        {
            std::vector<ZipIndex> zips;
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((modDir + "\\*.zip").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do zips.push_back(IndexZip(modDir + "\\" + fd.cFileName));
                while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            found = g_zips.emplace(modDir, std::move(zips)).first;
        }
        auto lower = [](std::string v) { for (char& c : v) c = (char)tolower((unsigned char)c); return v; };
        const std::string want = lower(file);
        for (const ZipIndex& z : found->second)
        {
            const ZipEntry* best = 0;
            for (const ZipEntry& e : z.entries)
            {
                std::string n = lower(e.name);
                size_t cut = n.find_last_of('/');
                if ((cut == std::string::npos ? n : n.substr(cut + 1)) != want) continue;
                if (n.find("/beta/") != std::string::npos) continue;
                if (!best || n.find("/default/") != std::string::npos) best = &e;
            }
            if (best && ReadEntry(z, *best, out)) return true;
        }
        return false;
    }

    // ------------------------------------------------------------ mod DDS
    // "mod/<id>/<path>.dds": a texture a mod ships in its own folder, decoded
    // with the same code as the packs. For art that is not the game's but is
    // meant to sit where the game's does - a redrawn TRACKMAP, say - without
    // touching the game's files. Only .dds, only under the mods folder.
    bool ModDds(const std::string& rel, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        if (rel.size() < 5 || _stricmp(rel.c_str() + rel.size() - 4, ".dds") != 0) return false;
        if (rel.find("..") != std::string::npos || rel.find(':') != std::string::npos ||
            rel.find('\\') != std::string::npos || rel[0] == '/')
            return false;

        char dir[MAX_PATH], abs[MAX_PATH];
        Config::GetString("Mods", "Dir", "FRSModLoader\\mods", dir, sizeof(dir));
        Config::Resolve(dir, abs, sizeof(abs));
        std::string path = std::string(abs) + "\\" + rel;
        for (char& c : path) if (c == '/') c = '\\';

        std::vector<uint8_t> d;
        bool read = false;
        if (FILE* f = fopen(path.c_str(), "rb"))
        {
            _fseeki64(f, 0, SEEK_END);
            d.resize((size_t)_ftelli64(f));
            _fseeki64(f, 0, SEEK_SET);
            read = fread(d.data(), 1, d.size(), f) == d.size();
            fclose(f);
        }
        else
        {
            // Not a loose file: maybe inside a zip the mod keeps in its root.
            size_t slash = rel.find('/');
            std::string modDir = std::string(abs) + "\\" + rel.substr(0, slash);
            size_t last = rel.find_last_of('/');
            read = FromModZip(modDir, rel.substr(last == std::string::npos ? 0 : last + 1), d);
            if (!read)
            {
                LogCef("textures: no %s, loose or in a zip of the mod", rel.c_str());
                return false;
            }
        }

        // DDS: "DDS ", then the 124-byte header - height +12, width +16,
        // pixel format flags +80, FourCC +84, bits per pixel +88. The first
        // level starts at 128; the mips after it are not needed.
        if (!read || d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0)
        {
            LogCef("textures: %s is not a DDS", rel.c_str());
            return false;
        }
        h = (int)*(const uint32_t*)&d[12];
        w = (int)*(const uint32_t*)&d[16];
        uint32_t flags = *(const uint32_t*)&d[80];
        uint32_t bits  = *(const uint32_t*)&d[88];
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;

        int format = 0;
        if (flags & 4)   // DDPF_FOURCC
        {
            if (!memcmp(&d[84], "DXT1", 4)) format = FMT_DXT1;
            if (!memcmp(&d[84], "DXT3", 4)) format = FMT_DXT3;
            if (!memcmp(&d[84], "DXT5", 4)) format = FMT_DXT5;
        }
        else if ((flags & 0x40) && bits == 32)   // DDPF_RGB, assumed A8R8G8B8
        {
            format = FMT_ARGB;
        }

        size_t pixels = (size_t)w * h;
        rgba.assign(pixels * 4, 0);
        if (format == FMT_DXT1 || format == FMT_DXT3 || format == FMT_DXT5)
        {
            size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * (format == FMT_DXT1 ? 8 : 16);
            if (d.size() < 128 + need) return false;
            DecodeDxt(&d[128], w, h, format, rgba.data());
            return true;
        }
        if (format == FMT_ARGB && d.size() >= 128 + pixels * 4)
        {
            for (size_t i = 0; i < pixels; i++)
            {
                rgba[i * 4 + 0] = d[128 + i * 4 + 2];
                rgba[i * 4 + 1] = d[128 + i * 4 + 1];
                rgba[i * 4 + 2] = d[128 + i * 4 + 0];
                rgba[i * 4 + 3] = d[128 + i * 4 + 3];
            }
            return true;
        }
        LogCef("textures: %s, a DDS format this does not read", rel.c_str());
        return false;
    }

    std::string IndexJson()
    {
        static const char* formats[] = { "DXT1", "DXT3", "DXT5", "A8R8G8B8", "P8" };
        std::string j = "[";
        char line[256];
        for (size_t i = 0; i < g_texs.size(); i++)
        {
            const Tex& t = g_texs[i];
            const char* f = t.format == FMT_DXT1 ? formats[0] : t.format == FMT_DXT3 ? formats[1]
                          : t.format == FMT_DXT5 ? formats[2] : t.format == FMT_ARGB ? formats[3]
                          : t.format == FMT_P8 ? formats[4] : "?";
            const char* pack = strrchr(g_packs[t.pack].path.c_str(), '\\');
            _snprintf(line, sizeof(line),
                      "%s\n{\"name\":\"%s\",\"hash\":\"0x%08X\",\"width\":%d,\"height\":%d,"
                      "\"format\":\"%s\",\"pack\":\"%s\"}",
                      i ? "," : "", t.name, t.hash, t.w, t.h, f, pack ? pack + 1 : "");
            j += line;
        }
        return j + "\n]\n";
    }
}

namespace GameTextures
{
    bool Get(const std::string& path, std::string* body, std::string* mime)
    {
        EnterCriticalSection(&g_lock);
        if (!g_loaded) LoadAll();

        bool ok = false;
        auto cached = g_cache.find(path);
        if (cached != g_cache.end())
        {
            *body = cached->second;
            ok = true;
        }
        else
        {
            size_t q = path.find('?');
            std::string name  = path.substr(0, q);
            std::string query = q == std::string::npos ? std::string() : path.substr(q + 1);
            while (!name.empty() && name[0] == '/') name.erase(0, 1);

            if (name == "index.json")
            {
                *body = IndexJson();
                *mime = "application/json";
                LeaveCriticalSection(&g_lock);
                return true;
            }

            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            bool have = false;
            if (name.compare(0, 4, "mod/") == 0)
            {
                have = ModDds(name.substr(4), rgba, w, h);
            }
            else if (const Tex* t = Find(name))
            {
                have = Decode(*t, rgba);
                w = t->w;
                h = t->h;
            }
            if (have)
            {
                std::string tint = Param(query, "tint"), flip = Param(query, "flip");
                if (!tint.empty()) Tint(rgba, tint);
                std::string stack = Param(query, "stack");
                if (!stack.empty()) Stack(rgba, atoi(stack.c_str()));
                if (!flip.empty()) Flip(rgba, w, h, flip);
                if (!Param(query, "mirror").empty()) Mirror(rgba, w, h);
                *body = Png(rgba, w, h);
                if (body->size() > BIG)
                {
                    if (!g_bigKey.empty()) g_cache.erase(g_bigKey);
                    g_bigKey = path;
                }
                g_cache[path] = *body;
                ok = true;
            }
            else
            {
                LogCef("textures: no %s", name.c_str());
            }
        }
        *mime = "image/png";
        LeaveCriticalSection(&g_lock);
        return ok;
    }

    bool Rgba(const char* name, std::vector<uint8_t>* rgba, int* width, int* height)
    {
        EnterCriticalSection(&g_lock);
        if (!g_loaded) LoadAll();

        bool ok = false;
        if (const Tex* t = Find(name ? name : ""))
        {
            ok = Decode(*t, *rgba);
            *width = t->w;
            *height = t->h;
        }
        LeaveCriticalSection(&g_lock);
        return ok;
    }
}
