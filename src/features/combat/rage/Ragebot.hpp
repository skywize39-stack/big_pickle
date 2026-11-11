#pragma once

#include "sdk/GameTypes.hpp"

namespace Ragebot
{
    void Run(void* pCInput, int seq, void* pLocalPlayer, int localTeam,
        const EntitySnapshot& snap);
    void ApplyAutoStop(void* pCmd, void* pLocalPlayer);
    void UpdateRecords(const EntitySnapshot& snap, int localTeam);
    bool HasCommittedRageShot(int commandNumber);

    // True when the last executed Rage command had a final target selected
    // (valid point that passed min-damage). DT uses it to keep its recharge
    // from stealing movement frames mid-duel on long-cycle weapons.
    bool HasTargetThisTick();

    // Engine time of the last committed rage shot (-1000 before the first
    // one). DT gates its recharge combat window on this instead of a bare
    // target check so the charge is full before a peek.
    float GetLastRageFireTime();

    // Current-command target state. A shifted follow-up is independently
    // selected and validated, with its own command tick and geometry.
    float GetCommittedRageShotSimtime();
    bool  GetCommittedRageShotFromRecord();
    // World-space aimpoint of the current committed shot.
    const Vec3& GetCommittedRageShotPoint();
    const char* GetActiveRageProfileName();

    bool IsMinDamageOverrideIndicatorActive();
    int GetMinDamageOverrideValue();
}
