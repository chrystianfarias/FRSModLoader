#pragma once

#include <windows.h>
#include <string>

// Audio for mods: load a WAV, fire it with volume and pitch.
//
// Why a native engine instead of letting the Chromium layer play the sounds:
// these are reactions to the physics, and they have to land on the frame that
// caused them. A pop fires from a gear change detected on the game thread; the
// UI layer is a whole process away, with JSON and a message queue in between.
// Chromium is the right place for interface sounds, not for engine noises.
//
// XAudio2 is loaded at run time (LoadLibrary), so the build does not depend on
// a particular SDK version being installed. XAudio2_9 ships with Windows 10+;
// the older ones are there as a fallback.
namespace Audio
{
    // Late, and never from DllMain: under the loader lock CreateSourceVoice
    // fails with 0x88960001 even though everything else succeeds. The first
    // call to Load() does this on its own.
    bool Init();
    void Shutdown();
    bool Ready();

    // Returns a sound id (>= 1), or 0 on failure. Loading the same path twice
    // returns the same id instead of a second copy.
    int Load(const std::string& path);
    bool Unload(int id);

    // volume: 0..1 (above that it clips), pitch: 0.5 = one octave down,
    // 2.0 = one octave up. Returns false if the sound does not exist or if
    // every voice in the pool is busy.
    bool Play(int id, float volume, float pitch);
    // The same, looping the whole sound until stopped when `loop`, and
    // rising from silence over `fadeInMs`.
    bool Play(int id, float volume, float pitch, bool loop, DWORD fadeInMs);

    // Stops what is playing: one sound, or everything.
    void Stop(int id);
    void StopAll();
    // Down to silence over `ms`, then stopped.
    void FadeOut(int id, DWORD ms);

    // Once a frame: moves the fades along.
    void Tick();

    // Applies to everything, on top of each Play's own volume.
    void SetMasterVolume(float volume);
    float MasterVolume();

    int  Count();
}
