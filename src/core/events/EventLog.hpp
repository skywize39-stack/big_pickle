#pragma once

#include "sdk/GameTypes.hpp"

// Rage requests are matched best-effort to weapon_fire / impact / hurt.
// Endpoint-only miss labels are hypotheses, NOT authoritative resolver results.
// The CSV preserves unconfirmed requests instead of reporting them as misses.
namespace EventLog
{
    bool Init();
    void Shutdown();

    // Rage commit (CreateMove thread).
    void OnRageShot( int targetIndex, int hitgroup, float hitchance, float damage,
                     const Vec3& eye, const Vec3& aimPoint, bool fromRecord,
                     int commandNumber = 0, bool chargeCommand = false,
                     int backtrackTicks = 0 );

    // Legacy API name. The counter now genuinely tracks resolved endpoint
    // "possible spread" misses per player and feeds the rage adaptive
    // hitchance streak (internal_hvh get_final_hitchance).
    int GetSpreadMisses( int index );

    // Consecutive record-driven "unknown pose/timing" misses per player.
    // Rage pauses record shots on that player while this passes a threshold
    // (hit resets the streak, like the spread counter).
    int GetPoseMisses( int index );

    // Free-form line (any feature).
    void Push( const char* fmt, ... );

    // Resolve timed-out shots + draw (render thread, inside ImGui frame).
    void Render();

    // FRAME_RENDER_END: project tracers / hitmarkers (game thread).
    void ProjectFrame();

    // Game-thread authoritative planted C4 state (also works without plant event).
    void UpdateWorldState();
}
