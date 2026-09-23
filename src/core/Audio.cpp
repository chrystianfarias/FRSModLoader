#include "core/Audio.h"

#include "core/Log.h"

#include <xaudio2.h>

#include <map>
#include <string>
#include <vector>

namespace
{
    const int VOICE_POOL = 16;      // simultaneous sounds

    struct Sound
    {
        std::vector<BYTE> data;
        WAVEFORMATEX      format;
        std::string       path;
    };

    IXAudio2*                g_xaudio = nullptr;
    IXAudio2MasteringVoice*  g_master = nullptr;

    struct Voice
    {
        IXAudio2SourceVoice* voice = nullptr;
        int                  sound = 0;     // which sound it was built for
        // A volume ramp, run by Tick(): from `from` to `to` over `ms`, then
        // stop the voice if `stopAtEnd`. `ms` 0 is no ramp.
        float from = 0, to = 0, volume = 1;
        DWORD start = 0, ms = 0;
        bool  stopAtEnd = false;
    };

    std::vector<Voice>       g_voices;
    std::map<int, Sound>     g_sounds;
    std::map<std::string, int> g_byPath;

    int   g_nextId = 1;
    bool  g_ready = false;
    bool  g_tried = false;
    float g_masterVolume = 1.0f;

    // Reads a RIFF/WAVE PCM file: walks the chunks looking for 'fmt ' and
    // 'data'. Deliberately small - the game's own .abk banks are another
    // matter entirely, and not what mods need.
    bool LoadWav(const std::string& path, Sound& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { LogTag("audio", "could not open %s", path.c_str()); return false; }

        fseek(f, 0, SEEK_END);
        long total = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (total < 44) { fclose(f); LogTag("audio", "%s is too short", path.c_str()); return false; }

        std::vector<BYTE> buf((size_t)total);
        size_t got = fread(buf.data(), 1, (size_t)total, f);
        fclose(f);
        if (got != (size_t)total) return false;

        if (memcmp(buf.data(), "RIFF", 4) != 0 || memcmp(buf.data() + 8, "WAVE", 4) != 0)
        {
            LogTag("audio", "%s is not RIFF/WAVE", path.c_str());
            return false;
        }

        bool haveFmt = false, haveData = false;
        size_t pos = 12;
        while (pos + 8 <= buf.size())
        {
            const char* id = (const char*)(buf.data() + pos);
            DWORD size = *(DWORD*)(buf.data() + pos + 4);
            size_t body = pos + 8;
            if (body + size > buf.size()) break;

            if (memcmp(id, "fmt ", 4) == 0 && size >= sizeof(PCMWAVEFORMAT))
            {
                memset(&out.format, 0, sizeof(out.format));
                memcpy(&out.format, buf.data() + body,
                       size < sizeof(WAVEFORMATEX) ? size : sizeof(WAVEFORMATEX));
                out.format.cbSize = 0;
                haveFmt = true;
            }
            else if (memcmp(id, "data", 4) == 0)
            {
                out.data.assign(buf.begin() + body, buf.begin() + body + size);
                haveData = true;
            }

            pos = body + size + (size & 1);   // chunks are word aligned
        }

        if (!haveFmt || !haveData)
        {
            LogTag("audio", "%s has no usable fmt/data chunk", path.c_str());
            return false;
        }

        // Some editors write a block align that does not match the format, and
        // XAudio2 then refuses the buffer. Recompute it for plain PCM rather
        // than trusting the header - this bit a real set of sounds.
        if (out.format.wFormatTag == WAVE_FORMAT_PCM)
        {
            WORD expected = (WORD)(out.format.nChannels * out.format.wBitsPerSample / 8);
            if (expected && out.format.nBlockAlign != expected)
            {
                LogTag("audio", "%s: block align %u is wrong, using %u",
                       path.c_str(), out.format.nBlockAlign, expected);
                out.format.nBlockAlign = expected;
            }
            out.format.nAvgBytesPerSec = out.format.nSamplesPerSec * out.format.nBlockAlign;
        }

        out.path = path;
        return true;
    }

    // A source voice is tied to the format it was created with, so the pool
    // keeps one per sound and rebuilds it when a slot is reused for another.
    IXAudio2SourceVoice* TakeVoice(int soundId, const WAVEFORMATEX& fmt)
    {
        for (auto& v : g_voices)
        {
            if (!v.voice || v.sound != soundId) continue;
            XAUDIO2_VOICE_STATE st;
            v.voice->GetState(&st);
            if (st.BuffersQueued == 0) return v.voice;
        }

        for (auto& v : g_voices)
        {
            if (v.voice)
            {
                XAUDIO2_VOICE_STATE st;
                v.voice->GetState(&st);
                if (st.BuffersQueued != 0) continue;
                v.voice->DestroyVoice();
                v.voice = nullptr;
            }

            IXAudio2SourceVoice* nv = nullptr;
            if (FAILED(g_xaudio->CreateSourceVoice(&nv, &fmt))) return nullptr;
            v.voice = nv;
            v.sound = soundId;
            return nv;
        }

        return nullptr;   // pool full: every voice is still playing
    }
}

namespace Audio
{
    bool Init()
    {
        if (g_ready) return true;
        if (g_tried) return false;
        g_tried = true;

        typedef HRESULT(__stdcall* tXAudio2Create)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

        const char* dlls[] = { "XAudio2_9.dll", "XAudio2_8.dll", "XAudio2_7.dll" };
        tXAudio2Create create = nullptr;
        for (int i = 0; i < 3 && !create; i++)
        {
            HMODULE h = LoadLibraryA(dlls[i]);
            if (h) create = (tXAudio2Create)GetProcAddress(h, "XAudio2Create");
        }
        if (!create) { LogTag("audio", "no XAudio2 runtime found"); return false; }

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
        { LogTag("audio", "XAudio2Create failed"); return false; }

        if (FAILED(g_xaudio->CreateMasteringVoice(&g_master)))
        { LogTag("audio", "CreateMasteringVoice failed"); return false; }

        g_voices.resize(VOICE_POOL);
        g_ready = true;
        LogTag("audio", "ready (%d voices)", VOICE_POOL);
        return true;
    }

    void Shutdown()
    {
        for (auto& v : g_voices) if (v.voice) v.voice->DestroyVoice();
        g_voices.clear();
        if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
        if (g_xaudio) { g_xaudio->Release(); g_xaudio = nullptr; }
        g_sounds.clear();
        g_byPath.clear();
        g_ready = false;
        g_tried = false;
    }

    bool Ready() { return g_ready; }
    int  Count() { return (int)g_sounds.size(); }

    int Load(const std::string& path)
    {
        if (!Init()) return 0;

        auto known = g_byPath.find(path);
        if (known != g_byPath.end()) return known->second;

        Sound s;
        if (!LoadWav(path, s)) return 0;

        int id = g_nextId++;
        g_sounds[id] = std::move(s);
        g_byPath[path] = id;

        const Sound& kept = g_sounds[id];
        LogTag("audio", "loaded %s (%u bytes, %u Hz, %u ch, %u bits) as #%d",
               path.c_str(), (unsigned)kept.data.size(),
               kept.format.nSamplesPerSec, kept.format.nChannels,
               kept.format.wBitsPerSample, id);
        return id;
    }

    bool Unload(int id)
    {
        auto it = g_sounds.find(id);
        if (it == g_sounds.end()) return false;

        Stop(id);
        for (auto& v : g_voices)
        {
            if (v.voice && v.sound == id) { v.voice->DestroyVoice(); v.voice = nullptr; v.sound = 0; }
        }
        g_byPath.erase(it->second.path);
        g_sounds.erase(it);
        return true;
    }

    bool Play(int id, float volume, float pitch)
    {
        return Play(id, volume, pitch, false, 0);
    }

    bool Play(int id, float volume, float pitch, bool loop, DWORD fadeInMs)
    {
        if (!g_ready) return false;

        auto it = g_sounds.find(id);
        if (it == g_sounds.end()) return false;
        const Sound& s = it->second;

        IXAudio2SourceVoice* voice = TakeVoice(id, s.format);
        if (!voice) return false;

        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = (UINT32)s.data.size();
        buf.pAudioData = s.data.data();
        buf.Flags = XAUDIO2_END_OF_STREAM;
        // The whole buffer again and again, sample-exact, until stopped.
        if (loop) buf.LoopCount = XAUDIO2_LOOP_INFINITE;

        voice->Stop(0);
        voice->FlushSourceBuffers();
        if (FAILED(voice->SubmitSourceBuffer(&buf))) return false;

        if (volume < 0.0f) volume = 0.0f;
        if (pitch < 0.03125f) pitch = 0.03125f;     // XAudio2's floor
        if (pitch > 4.0f) pitch = 4.0f;

        for (auto& v : g_voices)
        {
            if (v.voice != voice) continue;
            v.volume = volume;
            v.from = 0;
            v.to = volume;
            v.start = GetTickCount();
            v.ms = fadeInMs;
            v.stopAtEnd = false;
        }
        voice->SetVolume((fadeInMs ? 0.0f : volume) * g_masterVolume);
        voice->SetFrequencyRatio(pitch);
        return SUCCEEDED(voice->Start(0));
    }

    void Stop(int id)
    {
        for (auto& v : g_voices)
            if (v.voice && v.sound == id) { v.voice->Stop(0); v.voice->FlushSourceBuffers(); v.ms = 0; }
    }

    void FadeOut(int id, DWORD ms)
    {
        if (!ms) { Stop(id); return; }
        for (auto& v : g_voices)
        {
            if (!v.voice || v.sound != id) continue;
            XAUDIO2_VOICE_STATE st;
            v.voice->GetState(&st);
            if (st.BuffersQueued == 0) continue;
            // From wherever it is now: a fade that was still coming in goes
            // back down from its current level, not from the top.
            float now = v.volume;
            if (v.ms)
            {
                float t = (float)(GetTickCount() - v.start) / (float)v.ms;
                if (t > 1.0f) t = 1.0f;
                now = v.from + (v.to - v.from) * t;
            }
            v.from = now;
            v.to = 0;
            v.start = GetTickCount();
            v.ms = ms;
            v.stopAtEnd = true;
        }
    }

    void Tick()
    {
        const DWORD now = GetTickCount();
        for (auto& v : g_voices)
        {
            if (!v.voice || !v.ms) continue;
            float t = (float)(now - v.start) / (float)v.ms;
            if (t > 1.0f) t = 1.0f;
            v.voice->SetVolume((v.from + (v.to - v.from) * t) * g_masterVolume);
            if (t < 1.0f) continue;
            v.ms = 0;
            v.volume = v.to;
            if (v.stopAtEnd) { v.voice->Stop(0); v.voice->FlushSourceBuffers(); v.stopAtEnd = false; }
        }
    }

    void StopAll()
    {
        for (auto& v : g_voices)
            if (v.voice) { v.voice->Stop(0); v.voice->FlushSourceBuffers(); }
    }

    void SetMasterVolume(float volume)
    {
        if (volume < 0.0f) volume = 0.0f;
        g_masterVolume = volume;
        if (g_master) g_master->SetVolume(volume);
    }

    float MasterVolume() { return g_masterVolume; }
}
