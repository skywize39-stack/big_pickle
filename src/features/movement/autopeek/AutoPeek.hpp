#pragma once

#include "sdk/SDK.hpp"

namespace AutoPeek
{
    // Called after rage/autostop in CreateMove. While the configured key is
    // held, captures the start position; after IN_ATTACK it drives movement
    // back to that anchor until the player reaches it or releases the key.
    void Run(void* pCmd, void* pLocalPlayer);

    // Called by DT immediately after a real shot has armed a burst. Starts the
    // return from an existing anchor even when Rage no longer leaves IN_ATTACK
    // set in the final command.
    void OnShotCommitted(int commandNumber);

    // Auto revolver holds IN_ATTACK while cocking the hammer — that press is
    // not a shot. The real R8 discharge is reported through OnShotCommitted.
    void IgnoreAttack(int commandNumber);

    // Writes the active anchor-return movement into an already generated DT
    // command. Used only inside the burst to begin retracting immediately.
    void ApplyReturnMovement(void* pCmd, void* pLocalPlayer);

    // Exposes the currently captured start point for the world HUD marker.
    // Returns false when auto-peek has no active anchor.
    bool GetAnchor(Vec3& outAnchor, bool& outReturning);

    // True only while auto-peek is actively writing a route back to its anchor.
    // Movement features use this to avoid fighting the return command after a
    // confirmed shot.
    bool IsReturning();

    // Projection is captured on the game thread using the same NDC convention
    // as ESP. Rendering only consumes this snapshot and never calls engine W2S.
    bool GetMarkerNdc(Vec2& outNdc, bool& outReturning);

    // Clears anchor and return state. Safe to call on level changes/unload.
    void Reset();
}
