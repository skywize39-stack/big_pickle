#pragma once

#include "sdk/SDK.hpp"

// ============================================================
//  DoubleTap.hpp — thin adapter over the ported reference
//  CExploits tickbase (see Exploits.hpp). The old burst/queue
//  machinery is fully replaced by the reference charge/shift/
//  HandleTeleport model; this namespace only keeps the call
//  sites (CreateMove hook, CL_Move hook, rage, anti-aim, HUD)
//  stable.
// ============================================================

namespace DoubleTap
{
    enum class IndicatorState
    {
        Off,
        BindInactive,
        Charging,
        Ready,
        Armed,
        Shifting,
        Teleport,
        WaitingForFire
    };

    // Donor parity: shifted (extra CL_Move) commands run the FULL normal
    // combat pipeline, including rage, so the follow-up bullet is selected
    // by the ordinary prediction/aim/HC pipeline for its own command.
    void PrepareCommand(SDK::CUserCmd* cmd, void* player);
    bool CanRunAttackPipeline();

    bool Init();
    void Shutdown();

    // Donor Run() + UpdateTickbase() + shot arming (ForceTeleport and the
    // defensive_this_tick marker on the shot command) + bind-release
    // teleport. Runs after Rage and before anti-aim.
    void OnCreateMove( SDK::CUserCmd* cmd, void* pLocalPlayer );

    // Runs after anti-aim: donor Shift() — ring pre-fill of break-LC /
    // defensive extra commands on sending ticks.
    void FinalizeCommand( SDK::CUserCmd* cmd );

    // True while the HandleTeleport drain runs its extra CL_Moves.
    bool IsShifting();
    bool IsBindActive();
    bool IsBurstPending();

    // Donor charge transport: a charge tick is a skipped CL_Move. Withheld
    // moves increment Exploits->tickbase_shift (plus the CSSO final
    // predicted tick fix so the prediction clock stays aligned).
    bool TrySkipMoveForRecharge();

    // Defensive DT: spends the charged ticks as a movement-only drain burst.
    bool RequestDefensiveShift();
    int  GetChargeTicks();

    bool IsRecharging();
    bool IsRechargeSuppressed();
    IndicatorState GetIndicatorState();
    const char* GetIndicatorText();
    void Reset();
}
