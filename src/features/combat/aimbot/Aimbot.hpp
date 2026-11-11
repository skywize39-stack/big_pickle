#pragma once

// ============================================================
//  Aimbot.hpp — silent aimbot, runs in game thread (CreateMove)
// ============================================================

#include "sdk/GameTypes.hpp"

namespace Aimbot
{
    // Called from CHLClientCreateMoveDetour after the snapshot is built.
    // pCInput = CHLClient* thisptr, seq = frame sequence number.
    void Run( void* pCInput, int seq,
              void* pLocalPlayer, int localTeam,
              const EntitySnapshot& snap );

    // Applies the current-command Supremacy pending-stop movement after Rage
    // and before anti-aim's final movement correction.
    void ApplyAutoStop( void* pCmd, void* pLocalPlayer );

    // Called from the snapshot loop to append a per-player lag record
    // whenever the enemy got a fresh server state (simtime changed).
    void UpdateRecords( const EntitySnapshot& snap, int localTeam );

    // True only for the current command when Rage has committed one concrete
    // shot after target, penetration, hitchance and weapon-readiness gates.
    // DT consumes this token instead of treating any raw IN_ATTACK as a valid
    // shift trigger while peeking.
    bool HasCommittedRageShot( int commandNumber );

    // True when the last executed Rage command had a final target selected.
    // DT gates its recharge skips on it so long-cycle weapons do not freeze
    // mid-duel.
    bool HasTargetThisTick();

    // Last class selected in CreateMove. Safe for HUD display; it never reads
    // the active weapon entity from the render thread.
    const char* GetActiveRageProfileName();

    // Current-command target timestamp/record flag. Shifted follow-ups use
    // their own fresh combat decision rather than inheriting this commit.
    float GetCommittedRageShotSimtime();
    bool  GetCommittedRageShotFromRecord();

    // World-space aimpoint of the current committed shot.
    const Vec3& GetCommittedRageShotPoint();

    // Current-command Min Damage Hold override state for the active profile.
    bool IsMinDamageOverrideIndicatorActive();
    int GetMinDamageOverrideValue();
}
