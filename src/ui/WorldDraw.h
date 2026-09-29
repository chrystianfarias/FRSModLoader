#pragma once

#include <d3d9.h>
#include <stdint.h>

// Sprites drawn in the world, inside the game's own 3D scene.
//
// Not the UI layer: Chromium composites on top of everything, flat, with no
// depth and no perspective. A flame at the exhaust has to sit where the car is,
// shrink with distance and hide behind the bodywork when the camera swings
// around - so it is drawn as geometry, in EndScene, with the view and
// projection the game is already using.
//
// The particles are additive and depth-tested but not depth-written, which is
// how flames and sparks are normally drawn: they light up what is behind them
// without occluding each other.
namespace WorldDraw
{
    void Init();

    // Points the particles at one of the game's own textures (0 goes back to
    // the procedural blob). The address is validated through D3D before it is
    // accepted, because it comes from a memory scan and could be anything.
    bool UseTexture(uintptr_t address, unsigned* width, unsigned* height,
                    int* format);

    // The texture is a sprite sheet: how many frames in each direction. The
    // sequence plays once over the life of each particle. 1x1 turns it off.
    void Atlas(int columns, int rows);

    // The address of the FIELD that holds the texture pointer (info+0x18), so
    // that it is re-read every frame. Survives resources being unloaded and
    // reloaded when leaving and returning to an event.
    void TextureSource(uintptr_t address);

    // Is the game texture still valid? False once the game has unloaded the
    // resource - the signal for the mod to look it up by name again.
    bool TextureValid();

    // Where the game's view matrix is, and which lens to draw with.
    //
    // MAIN PATH (automatic): the view+projection matrix is fished out of the
    // constants the game sends to the vertex shader, and recognised by its
    // SHAPE - divided by the known view matrix, what is left has to look like
    // a projection. That way there is no field of view to guess: it is the same
    // maths that draws the car, including the game's off-centre lens (fovY ~69,
    // aspect 1.62, Y already flipped), which no hand-built fov could imitate.
    //
    // Two details cost hours and are recorded here:
    //
    //   - the hook HAS to be in the d3d9.dll code, not in the vtable: another
    //     ASI (WidescreenFix/HDReflections) restores the device vtable, and a
    //     hook there dies silently before the game reaches the track;
    //   - reading the matrices in EndScene never works: the 2D HUD sets view
    //     and projection to identity before that.
    //
    // The manual path below (view address + fov + flip) was kept as a
    // fallback. It is off by a little and in a misleading way: it gets the
    // point the camera pivots on right and everything around it wrong, which
    // looks like a position problem.
    //
    // NFSU2 draws with vertex shaders and never feeds the fixed pipeline, so
    // the matrices have to come from here. The view one is read from the
    // game's memory every frame (address found by the mod's /camera command);
    // the projection one is built from the field of view, since the game does
    // not keep one.
    //
    // depth=false draws over the scene: useful while the depth range is not
    // calibrated against the game's.
    void Camera(uintptr_t viewMatrixAddress, float fovYDegrees,
                float nearPlane, float farPlane, bool depth, bool flipY,
                bool automatic);
    void OnDeviceLost();
    void OnDeviceReset();

    // Spawns one particle. Position is world space; size is in world units;
    // life in seconds. Colour is 0xRRGGBB and fades to nothing over the life.
    // `attached` reads the position in the CAR's coordinates: the particle
    // follows the vehicle instead of being left behind in the world.
    // `vx,vy,vz` is velocity in the same frame as the position (m/s): on an
    // attached particle, velocity relative to the car. `endColour` is the
    // colour at the end of the life; the colour moves from the birth one to it.
    void Spawn(float x, float y, float z, float size, float life, DWORD colour,
               bool attached = false, float vx = 0, float vy = 0, float vz = 0,
               DWORD endColour = 0);

    // The car's frame of reference for this frame: position and the three rows
    // of the rotation matrix (the car's axes in the world), as in the pose.
    void Anchor(const float* position, const float* rotation3x3);

    // Called once a frame from the render hook, before the UI is composited.
    void Render(IDirect3DDevice9* device, float dt);

    void Clear();
    int  Count();
}
