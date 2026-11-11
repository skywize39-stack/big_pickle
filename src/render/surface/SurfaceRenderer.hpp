#pragma once

namespace SurfaceRenderer
{
    // Installs the verified CSSO IPanel::PaintTraverse hook.
    bool Init();
    void Shutdown();

    // True once the hook has seen at least one paint call (the per-frame ESP
    // projection runs from it; callers use this to detect the fallback path).
    bool IsLive();

    // Call from normal game-command flow, never from PaintTraverse itself.
    // Prints the one-time test result after the native paint callback returned.
    void PollStatus();

    // Validated world model path (IClientRenderable) / dormancy, SEH-guarded.
    const char* GetWorldModelPath( void* entity );
    bool IsEntityDormant( void* entity );
}
