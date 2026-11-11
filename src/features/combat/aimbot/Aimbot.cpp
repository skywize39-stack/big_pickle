#include "features/combat/aimbot/Aimbot.hpp"
#include "features/combat/rage/Ragebot.hpp"

namespace Aimbot
{
    void Run(void* pCInput, int seq, void* pLocalPlayer, int localTeam,
        const EntitySnapshot& snap)
    {
        Ragebot::Run(pCInput, seq, pLocalPlayer, localTeam, snap);
    }

    void ApplyAutoStop(void* pCmd, void* pLocalPlayer)
    {
        Ragebot::ApplyAutoStop(pCmd, pLocalPlayer);
    }

    void UpdateRecords(const EntitySnapshot& snap, int localTeam)
    {
        Ragebot::UpdateRecords(snap, localTeam);
    }

    bool HasCommittedRageShot(int commandNumber)
    {
        return Ragebot::HasCommittedRageShot(commandNumber);
    }

    bool HasTargetThisTick()
    {
        return Ragebot::HasTargetThisTick();
    }

    const char* GetActiveRageProfileName()
    {
        return Ragebot::GetActiveRageProfileName();
    }

    float GetCommittedRageShotSimtime()
    {
        return Ragebot::GetCommittedRageShotSimtime();
    }

    bool GetCommittedRageShotFromRecord()
    {
        return Ragebot::GetCommittedRageShotFromRecord();
    }

    const Vec3& GetCommittedRageShotPoint()
    {
        return Ragebot::GetCommittedRageShotPoint();
    }

    bool IsMinDamageOverrideIndicatorActive()
    {
        return Ragebot::IsMinDamageOverrideIndicatorActive();
    }

    int GetMinDamageOverrideValue()
    {
        return Ragebot::GetMinDamageOverrideValue();
    }
}
