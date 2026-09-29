// speed.mem  - read, write and patch the game's memory
// speed.call - call a native game function
//
// This is the dangerous half of the SDK: a wrong address takes the game down.
// Every read goes through IsBadReadPtr, which catches the common case (null
// pointer, object not created yet) without costing much.
#include "Api.h"

#include "core/Hook.h"
#include "core/Log.h"

#include <string>
#include <vector>

namespace
{
    enum Conv { CONV_CDECL = 0, CONV_STDCALL = 1, CONV_THISCALL = 2 };

    bool Addr(JSContext* ctx, JSValueConst v, uintptr_t* out)
    {
        // Addresses arrive as numbers (0x8654A4) - they fit a double easily.
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) < 0) return false;
        if (d < 0 || d > 4294967295.0) return false;
        *out = (uintptr_t)d;
        return true;
    }

    bool Readable(uintptr_t addr, size_t size)
    {
        return addr && !IsBadReadPtr((const void*)addr, size);
    }

    bool Writable(uintptr_t addr, size_t size)
    {
        return addr && !IsBadWritePtr((void*)addr, size);
    }

    // --------------------------------------------------------------- reads
    template <typename T>
    JSValue ReadNumber(JSContext* ctx, JSValueConst* argv, bool asDouble)
    {
        uintptr_t addr;
        if (!Addr(ctx, argv[0], &addr)) return JS_ThrowTypeError(ctx, "address expected");
        if (!Readable(addr, sizeof(T))) return JS_NULL;

        T value = *(T*)addr;
        return asDouble ? JS_NewFloat64(ctx, (double)value)
                        : JS_NewInt64(ctx, (int64_t)value);
    }

    JSValue ReadU8 (JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<unsigned char>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadI8 (JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<signed char>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadU16(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<unsigned short>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadI16(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<short>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadU32(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<unsigned int>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadI32(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<int>(c, a, false) : JS_UNDEFINED; }
    JSValue ReadF32(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<float>(c, a, true) : JS_UNDEFINED; }
    JSValue ReadF64(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return n ? ReadNumber<double>(c, a, true) : JS_UNDEFINED; }

    JSValue ReadString(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        if (!argc || !Addr(ctx, argv[0], &addr)) return JS_ThrowTypeError(ctx, "address expected");

        int32_t max = 256;
        if (argc >= 2) JS_ToInt32(ctx, &max, argv[1]);
        if (!Readable(addr, 1)) return JS_NULL;

        std::string out;
        for (int i = 0; i < max; i++)
        {
            if (!Readable(addr + i, 1)) break;
            char c = *(char*)(addr + i);
            if (!c) break;
            out += c;
        }
        return JS_NewStringLen(ctx, out.c_str(), out.size());
    }

    JSValue ReadBytes(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        int32_t len = 0;
        if (argc < 2 || !Addr(ctx, argv[0], &addr))
            return JS_ThrowTypeError(ctx, "speed.mem.readBytes(address, length)");
        JS_ToInt32(ctx, &len, argv[1]);
        if (len <= 0 || !Readable(addr, len)) return JS_NULL;

        return JS_NewArrayBufferCopy(ctx, (const uint8_t*)addr, (size_t)len);
    }

    // -------------------------------------------------------------- writes
    template <typename T>
    JSValue WriteNumber(JSContext* ctx, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        if (argc < 2 || !Addr(ctx, argv[0], &addr))
            return JS_ThrowTypeError(ctx, "(address, value)");

        double value = 0;
        if (JS_ToFloat64(ctx, &value, argv[1]) < 0) return JS_EXCEPTION;
        if (!Writable(addr, sizeof(T))) return JS_FALSE;

        DWORD old;
        VirtualProtect((void*)addr, sizeof(T), PAGE_EXECUTE_READWRITE, &old);
        *(T*)addr = (T)value;
        VirtualProtect((void*)addr, sizeof(T), old, &old);
        return JS_TRUE;
    }

    JSValue WriteU8 (JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return WriteNumber<unsigned char>(c, n, a); }
    JSValue WriteU16(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return WriteNumber<unsigned short>(c, n, a); }
    JSValue WriteU32(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return WriteNumber<unsigned int>(c, n, a); }
    JSValue WriteF32(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return WriteNumber<float>(c, n, a); }
    JSValue WriteF64(JSContext* c, JSValueConst, int n, JSValueConst* a)
    { return WriteNumber<double>(c, n, a); }

    // Takes an array of numbers or a string "90 90 E9 ?? ..." (?? = leave it).
    bool ToByteList(JSContext* ctx, JSValueConst v, std::vector<int>* out)
    {
        if (JS_IsString(v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (!s) return false;
            for (const char* p = s; *p; )
            {
                while (*p == ' ') p++;
                if (!*p) break;
                if (p[0] == '?' ) { out->push_back(-1); p += (p[1] == '?') ? 2 : 1; }
                else
                {
                    char hex[3] = { p[0], p[1], 0 };
                    out->push_back((int)strtol(hex, 0, 16));
                    p += 2;
                }
            }
            JS_FreeCString(ctx, s);
            return true;
        }

        JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
        if (JS_IsUndefined(lenVal)) { JS_FreeValue(ctx, lenVal); return false; }

        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        for (uint32_t i = 0; i < len; i++)
        {
            JSValue item = JS_GetPropertyUint32(ctx, v, i);
            int32_t b = 0;
            JS_ToInt32(ctx, &b, item);
            JS_FreeValue(ctx, item);
            out->push_back(b & 0xFF);
        }
        return true;
    }

    JSValue Patch(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        std::vector<int> bytes;
        if (argc < 2 || !Addr(ctx, argv[0], &addr) ||
            !ToByteList(ctx, argv[1], &bytes))
            return JS_ThrowTypeError(ctx,
                "speed.mem.patch(address, [bytes] or \"90 90\")");

        if (bytes.empty() || !Writable(addr, bytes.size())) return JS_FALSE;

        DWORD old;
        VirtualProtect((void*)addr, bytes.size(), PAGE_EXECUTE_READWRITE, &old);
        for (size_t i = 0; i < bytes.size(); i++)
            if (bytes[i] >= 0) *(unsigned char*)(addr + i) = (unsigned char)bytes[i];
        VirtualProtect((void*)addr, bytes.size(), old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, bytes.size());
        return JS_TRUE;
    }

    JSValue Nop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        int32_t len = 0;
        if (argc < 2 || !Addr(ctx, argv[0], &addr))
            return JS_ThrowTypeError(ctx, "speed.mem.nop(address, length)");
        JS_ToInt32(ctx, &len, argv[1]);
        if (len <= 0 || !Writable(addr, len)) return JS_FALSE;

        DWORD old;
        VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
        memset((void*)addr, 0x90, len);
        VirtualProtect((void*)addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
        return JS_TRUE;
    }

    // Walks a pointer chain: chain(0x8900AC, [0x04, 0x34, 0x18]) reads the
    // pointer at 0x8900AC, adds 0x04, reads again, and so on. Returns the last
    // address, without reading the value there.
    JSValue Chain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        if (argc < 1 || !Addr(ctx, argv[0], &addr))
            return JS_ThrowTypeError(ctx, "speed.mem.chain(base, [offsets])");

        if (argc < 2) return JS_NewInt64(ctx, addr);

        JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        for (uint32_t i = 0; i < len; i++)
        {
            if (!Readable(addr, 4)) return JS_NULL;
            addr = *(uintptr_t*)addr;

            JSValue off = JS_GetPropertyUint32(ctx, argv[1], i);
            int32_t delta = 0;
            JS_ToInt32(ctx, &delta, off);
            JS_FreeValue(ctx, off);

            if (!addr) return JS_NULL;
            addr += delta;
        }
        return Readable(addr, 1) ? JS_NewInt64(ctx, addr) : JS_NULL;
    }

    JSValue Alloc(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int32_t size = 0;
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.mem.alloc(size)");
        JS_ToInt32(ctx, &size, argv[0]);
        if (size <= 0) return JS_NULL;

        void* p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        return p ? JS_NewInt64(ctx, (int64_t)(uintptr_t)p) : JS_NULL;
    }

    JSValue Free(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        if (argc < 1 || !Addr(ctx, argv[0], &addr)) return JS_FALSE;
        return VirtualFree((void*)addr, 0, MEM_RELEASE) ? JS_TRUE : JS_FALSE;
    }

    JSValue Valid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t addr;
        int32_t size = 1;
        if (argc < 1 || !Addr(ctx, argv[0], &addr)) return JS_FALSE;
        if (argc >= 2) JS_ToInt32(ctx, &size, argv[1]);
        return Readable(addr, size > 0 ? size : 1) ? JS_TRUE : JS_FALSE;
    }

    // ------------------------------------------------------- native calls
    // x86, three conventions. Arguments go on the stack right to left; under
    // thiscall the first one goes to ecx. Under cdecl the caller cleans up.
    // Two nearly identical blocks because only the float-returning one may
    // execute fstp: with an empty FPU stack that would raise an exception.
    uint32_t InvokeInt(uintptr_t fn, const uint32_t* args, int count, int conv)
    {
        uint32_t result = 0;
        uint32_t thisPtr = 0;
        const uint32_t* stackArgs = args;
        int pushCount = count;

        if (conv == CONV_THISCALL && count > 0)
        {
            thisPtr = args[0];
            stackArgs = args + 1;
            pushCount = count - 1;
        }

        __asm {
            push esi
            mov  esi, stackArgs
            mov  ecx, pushCount
            test ecx, ecx
            jz   noargs
            lea  esi, [esi + ecx * 4]
        pushloop:
            sub  esi, 4
            push dword ptr [esi]
            dec  ecx
            jnz  pushloop
        noargs:
            mov  ecx, thisPtr
            mov  eax, fn
            call eax
            mov  result, eax
            mov  edx, conv
            cmp  edx, CONV_CDECL
            jne  nocleanup
            mov  edx, pushCount
            shl  edx, 2
            add  esp, edx
        nocleanup:
            pop  esi
        }
        return result;
    }

    double InvokeFloat(uintptr_t fn, const uint32_t* args, int count, int conv)
    {
        double result = 0;
        uint32_t thisPtr = 0;
        const uint32_t* stackArgs = args;
        int pushCount = count;

        if (conv == CONV_THISCALL && count > 0)
        {
            thisPtr = args[0];
            stackArgs = args + 1;
            pushCount = count - 1;
        }

        __asm {
            push esi
            mov  esi, stackArgs
            mov  ecx, pushCount
            test ecx, ecx
            jz   noargs
            lea  esi, [esi + ecx * 4]
        pushloop:
            sub  esi, 4
            push dword ptr [esi]
            dec  ecx
            jnz  pushloop
        noargs:
            mov  ecx, thisPtr
            mov  eax, fn
            call eax
            fstp qword ptr [result]
            mov  edx, conv
            cmp  edx, CONV_CDECL
            jne  nocleanup
            mov  edx, pushCount
            shl  edx, 2
            add  esp, edx
        nocleanup:
            pop  esi
        }
        return result;
    }

    JSValue CallNative(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t fn;
        if (argc < 1 || !Addr(ctx, argv[0], &fn))
            return JS_ThrowTypeError(ctx,
                "speed.call(address, [args], { conv, ret })");

        // The string buffers have to outlive the call itself.
        std::vector<std::string> strings;
        std::vector<uint32_t> args;
        std::vector<JSValue> argVals;

        if (argc >= 2 && JS_IsObject(argv[1]))
        {
            JSValue lenVal = JS_GetPropertyStr(ctx, argv[1], "length");
            uint32_t len = 0;
            JS_ToUint32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);

            strings.reserve(len);
            for (uint32_t i = 0; i < len; i++)
            {
                JSValue item = JS_GetPropertyUint32(ctx, argv[1], i);
                if (JS_IsString(item))
                {
                    const char* s = JS_ToCString(ctx, item);
                    strings.push_back(s ? s : "");
                    if (s) JS_FreeCString(ctx, s);
                    args.push_back(0);   // filled in later: the vector may move
                }
                else if (JS_IsBool(item))
                {
                    args.push_back(JS_ToBool(ctx, item) ? 1u : 0u);
                    strings.push_back(std::string());
                }
                else
                {
                    double d = 0;
                    JS_ToFloat64(ctx, &d, item);
                    // A float goes in as bits: a native taking a float on the
                    // stack expects IEEE, not a truncated integer.
                    if (d != (double)(int64_t)d)
                    {
                        float f = (float)d;
                        uint32_t bits;
                        memcpy(&bits, &f, 4);
                        args.push_back(bits);
                    }
                    else
                    {
                        args.push_back((uint32_t)(int64_t)d);
                    }
                    strings.push_back(std::string());
                }
                JS_FreeValue(ctx, item);
            }

            for (size_t i = 0; i < strings.size(); i++)
                if (!strings[i].empty()) args[i] = (uint32_t)(uintptr_t)strings[i].c_str();
        }

        int conv = CONV_CDECL;
        std::string ret = "int";
        if (argc >= 3 && JS_IsObject(argv[2]))
        {
            JSValue c = JS_GetPropertyStr(ctx, argv[2], "conv");
            const char* cs = JS_IsString(c) ? JS_ToCString(ctx, c) : 0;
            if (cs)
            {
                if (!strcmp(cs, "stdcall"))  conv = CONV_STDCALL;
                if (!strcmp(cs, "thiscall")) conv = CONV_THISCALL;
                JS_FreeCString(ctx, cs);
            }
            JS_FreeValue(ctx, c);

            JSValue r = JS_GetPropertyStr(ctx, argv[2], "ret");
            const char* rs = JS_IsString(r) ? JS_ToCString(ctx, r) : 0;
            if (rs) { ret = rs; JS_FreeCString(ctx, rs); }
            JS_FreeValue(ctx, r);
        }

        const uint32_t* raw = args.empty() ? 0 : &args[0];
        if (ret == "float" || ret == "double")
            return JS_NewFloat64(ctx, InvokeFloat(fn, raw, (int)args.size(), conv));

        uint32_t r = InvokeInt(fn, raw, (int)args.size(), conv);
        if (ret == "void") return JS_UNDEFINED;
        if (ret == "bool") return r ? JS_TRUE : JS_FALSE;
        if (ret == "string")
        {
            if (!Readable(r, 1)) return JS_NULL;
            return JS_NewString(ctx, (const char*)r);
        }
        return JS_NewInt64(ctx, (int64_t)r);
    }

    // Repoints an existing CALL rel32 somewhere else. It is the cheap hook
    // Pops uses in the main loop; handy for mods with a stub of their own.
    JSValue RedirectCallJs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t site, target;
        if (argc < 2 || !Addr(ctx, argv[0], &site) || !Addr(ctx, argv[1], &target))
            return JS_ThrowTypeError(ctx, "speed.mem.redirectCall(site, target)");
        if (!Readable(site, 5) || *(unsigned char*)site != 0xE8)
            return JS_ThrowRangeError(ctx, "0x%08X is not a CALL rel32", site);

        uintptr_t prev = RedirectCall(site, (void*)target);
        return JS_NewInt64(ctx, (int64_t)prev);
    }
}

namespace
{
    // Memory scanning, for hunting down structures.
    //
    // The workflow is the classic one: search for a known value (money, for
    // example), change it in the game and narrow the list down. What is left
    // is the field. Without this, finding a save structure turns into
    // guesswork — and this project has already spent too many rounds guessing.
    //
    // The scan only walks committed, writable regions: the rest is code, and
    // a save does not live there.
    std::vector<uintptr_t> g_hits;

    // Protected read. It has to live in a function of its own with no C++
    // object anywhere near it: the compiler refuses __try where there is stack
    // unwinding (C2712), and a local std::vector is already enough for that.
    bool ReadInt(uintptr_t a, int32_t* out)
    {
        __try { *out = *(int32_t*)a; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Scans a region into a raw array. The region can vanish in the middle of
    // the scan — another thread freeing memory — and before this it took the
    // game down.
    size_t ScanRegion(uintptr_t base, size_t size, int32_t target,
                      uintptr_t* out, size_t maxOut)
    {
        size_t n = 0;
        __try
        {
            for (size_t off = 0; off + 4 <= size && n < maxOut; off += 4)
                if (*(int32_t*)(base + off) == target) out[n++] = base + off;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    size_t SnapshotRegion(uintptr_t base, uintptr_t end,
                          uintptr_t* addrs, int32_t* vals, size_t maxOut)
    {
        size_t n = 0;
        __try
        {
            for (uintptr_t a = base; a + 4 <= end && n < maxOut; a += 4)
            {
                addrs[n] = a;
                vals[n] = *(int32_t*)a;
                n++;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    bool InterestingRegion(const MEMORY_BASIC_INFORMATION& mbi)
    {
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & PAGE_GUARD) return false;

        DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                       | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (mbi.Protect & writable) != 0;
    }

    JSValue Scan(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.mem.scan(value, max?)");

        int32_t target = 0;
        if (JS_ToInt32(ctx, &target, argv[0])) return JS_EXCEPTION;

        int32_t maxHits = 4096;
        if (argc >= 2) JS_ToInt32(ctx, &maxHits, argv[1]);

        g_hits.clear();

        SYSTEM_INFO si;
        GetSystemInfo(&si);

        uintptr_t p = (uintptr_t)si.lpMinimumApplicationAddress;
        uintptr_t end = (uintptr_t)si.lpMaximumApplicationAddress;

        while (p < end && (int)g_hits.size() < maxHits)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)p, &mbi, sizeof(mbi))) break;

            if (InterestingRegion(mbi))
            {
                uintptr_t base = (uintptr_t)mbi.BaseAddress;
                size_t size = mbi.RegionSize;

                // Guarded by SEH: the scan reads live memory, and another
                // thread can free a region halfway through. Without this the
                // game crashed — and not at the bad read, but on the next
                // scan, which is worse to diagnose.
                //
                // Aligned to 4: an int is never misaligned, and scanning byte
                // by byte would multiply the cost by four.
                size_t room = (size_t)maxHits - g_hits.size();
                if (room)
                {
                    std::vector<uintptr_t> found(room);
                    size_t n = ScanRegion(base, size, target, found.data(), room);
                    g_hits.insert(g_hits.end(), found.begin(), found.begin() + n);
                }
            }

            p = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }

        return JS_NewInt32(ctx, (int32_t)g_hits.size());
    }

    // Second pass: keeps only the addresses that now hold the new number.
    JSValue ScanNext(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.mem.scanNext(value)");

        int32_t target = 0;
        if (JS_ToInt32(ctx, &target, argv[0])) return JS_EXCEPTION;

        std::vector<uintptr_t> remaining;
        for (uintptr_t a : g_hits)
        {
            if (IsBadReadPtr((void*)a, 4)) continue;
            int32_t v = 0;
            if (ReadInt(a, &v) && v == target) remaining.push_back(a);
        }
        g_hits.swap(remaining);
        return JS_NewInt32(ctx, (int32_t)g_hits.size());
    }

    // Search for an UNKNOWN value: take a snapshot, then filter by what
    // changed (or by what stayed the same).
    //
    // This is what works when the number you are after is unknown — for the
    // car index, nobody even knows whether it starts at zero. Just switch cars
    // between the snapshot and the filter.
    //
    // The snapshot only covers the module's globals range (a few MB), not the
    // whole memory: that is where the career state lives, and copying hundreds
    // of megabytes out of curiosity would be expensive for no reason.
    std::vector<uintptr_t> g_snapAddrs;
    std::vector<int32_t>   g_snapVals;

    JSValue ScanSnapshot(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        uintptr_t base = 0x400000, end = 0x900000;
        if (argc >= 1) { int32_t v = 0; JS_ToInt32(ctx, &v, argv[0]); base = (uintptr_t)v; }
        if (argc >= 2) { int32_t v = 0; JS_ToInt32(ctx, &v, argv[1]); end = base + (uintptr_t)v; }

        g_snapAddrs.clear();
        g_snapVals.clear();

        uintptr_t p = base;
        while (p < end)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)p, &mbi, sizeof(mbi))) break;

            uintptr_t regBase = (uintptr_t)mbi.BaseAddress;
            uintptr_t regEnd = regBase + mbi.RegionSize;
            if (InterestingRegion(mbi))
            {
                uintptr_t a = regBase < base ? base : regBase;
                uintptr_t b = regEnd > end ? end : regEnd;
                size_t room = (b > a) ? (size_t)((b - a) / 4) : 0;
                if (room)
                {
                    std::vector<uintptr_t> addrs(room);
                    std::vector<int32_t> vals(room);
                    size_t n = SnapshotRegion(a, b, addrs.data(), vals.data(), room);
                    g_snapAddrs.insert(g_snapAddrs.end(), addrs.begin(), addrs.begin() + n);
                    g_snapVals.insert(g_snapVals.end(), vals.begin(), vals.begin() + n);
                }
            }
            p = regEnd;
        }

        g_hits.clear();
        return JS_NewInt32(ctx, (int32_t)g_snapAddrs.size());
    }

    // changed = true  -> keeps whatever changed since the snapshot
    // changed = false -> keeps whatever stayed the same
    JSValue ScanDiff(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        bool changed = argc < 1 || JS_ToBool(ctx, argv[0]) == 1;

        std::vector<uintptr_t> addrs;
        std::vector<int32_t>   vals;

        for (size_t i = 0; i < g_snapAddrs.size(); i++)
        {
            uintptr_t a = g_snapAddrs[i];
            if (IsBadReadPtr((void*)a, 4)) continue;

            int32_t now = 0;
            if (!ReadInt(a, &now)) continue;
            if ((now != g_snapVals[i]) != changed) continue;

            addrs.push_back(a);
            vals.push_back(now);
        }

        g_snapAddrs.swap(addrs);
        g_snapVals.swap(vals);
        g_hits = g_snapAddrs;
        return JS_NewInt32(ctx, (int32_t)g_snapAddrs.size());
    }

    JSValue ScanResults(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        int32_t limit = 32;
        if (argc >= 1) JS_ToInt32(ctx, &limit, argv[0]);

        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < g_hits.size() && (int)i < limit; i++)
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                                 JS_NewInt64(ctx, (int64_t)g_hits[i]));
        return arr;
    }
}

namespace Js
{
    void RegisterMemory(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue mem = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, mem, "readU8",  JS_NewCFunction(ctx, ReadU8,  "readU8", 1));
        JS_SetPropertyStr(ctx, mem, "readI8",  JS_NewCFunction(ctx, ReadI8,  "readI8", 1));
        JS_SetPropertyStr(ctx, mem, "readU16", JS_NewCFunction(ctx, ReadU16, "readU16", 1));
        JS_SetPropertyStr(ctx, mem, "readI16", JS_NewCFunction(ctx, ReadI16, "readI16", 1));
        JS_SetPropertyStr(ctx, mem, "readU32", JS_NewCFunction(ctx, ReadU32, "readU32", 1));
        JS_SetPropertyStr(ctx, mem, "readI32", JS_NewCFunction(ctx, ReadI32, "readI32", 1));
        JS_SetPropertyStr(ctx, mem, "readPtr", JS_NewCFunction(ctx, ReadU32, "readPtr", 1));
        JS_SetPropertyStr(ctx, mem, "readF32", JS_NewCFunction(ctx, ReadF32, "readF32", 1));
        JS_SetPropertyStr(ctx, mem, "readF64", JS_NewCFunction(ctx, ReadF64, "readF64", 1));
        JS_SetPropertyStr(ctx, mem, "readString",
                          JS_NewCFunction(ctx, ReadString, "readString", 2));
        JS_SetPropertyStr(ctx, mem, "readBytes",
                          JS_NewCFunction(ctx, ReadBytes, "readBytes", 2));

        JS_SetPropertyStr(ctx, mem, "writeU8",  JS_NewCFunction(ctx, WriteU8,  "writeU8", 2));
        JS_SetPropertyStr(ctx, mem, "writeU16", JS_NewCFunction(ctx, WriteU16, "writeU16", 2));
        JS_SetPropertyStr(ctx, mem, "writeU32", JS_NewCFunction(ctx, WriteU32, "writeU32", 2));
        JS_SetPropertyStr(ctx, mem, "writePtr", JS_NewCFunction(ctx, WriteU32, "writePtr", 2));
        JS_SetPropertyStr(ctx, mem, "writeF32", JS_NewCFunction(ctx, WriteF32, "writeF32", 2));
        JS_SetPropertyStr(ctx, mem, "writeF64", JS_NewCFunction(ctx, WriteF64, "writeF64", 2));

        JS_SetPropertyStr(ctx, mem, "patch", JS_NewCFunction(ctx, Patch, "patch", 2));
        JS_SetPropertyStr(ctx, mem, "nop",   JS_NewCFunction(ctx, Nop, "nop", 2));
        JS_SetPropertyStr(ctx, mem, "chain", JS_NewCFunction(ctx, Chain, "chain", 2));
        JS_SetPropertyStr(ctx, mem, "alloc", JS_NewCFunction(ctx, Alloc, "alloc", 1));
        JS_SetPropertyStr(ctx, mem, "free",  JS_NewCFunction(ctx, Free, "free", 1));
        JS_SetPropertyStr(ctx, mem, "valid", JS_NewCFunction(ctx, Valid, "valid", 2));
        JS_SetPropertyStr(ctx, mem, "scan",
                          JS_NewCFunction(ctx, Scan, "scan", 2));
        JS_SetPropertyStr(ctx, mem, "scanNext",
                          JS_NewCFunction(ctx, ScanNext, "scanNext", 1));
        JS_SetPropertyStr(ctx, mem, "scanSnapshot",
                          JS_NewCFunction(ctx, ScanSnapshot, "scanSnapshot", 2));
        JS_SetPropertyStr(ctx, mem, "scanDiff",
                          JS_NewCFunction(ctx, ScanDiff, "scanDiff", 1));
        JS_SetPropertyStr(ctx, mem, "scanResults",
                          JS_NewCFunction(ctx, ScanResults, "scanResults", 1));

        JS_SetPropertyStr(ctx, mem, "redirectCall",
                          JS_NewCFunction(ctx, RedirectCallJs, "redirectCall", 2));

        JS_SetPropertyStr(ctx, speed, "mem", mem);
        JS_SetPropertyStr(ctx, speed, "call",
                          JS_NewCFunction(ctx, CallNative, "call", 3));
    }
}
