#include "ui/binds/KeyBinds.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/Exploits.hpp"

#include "config/Config.hpp"
#include "features/networking/fakelag/FakeLag.hpp"
#include "features/networking/fakeduck/FakeDuck.hpp"
#include "features/combat/aimbot/Aimbot.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include "core/diagnostics/DebugLog.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>

using namespace SDK;

// Written every executed CreateMove in CreateMoveHook.cpp. Recharge runs in
// the CL_Move hook on frames whose own CreateMove was skipped, so it reads
// the local player from the last executed command.
extern void* g_pLocalPlayer;

namespace
{
    bool s_bindActive     = false;
    // Bind state as seen by the last EXECUTED CreateMove. The recharge path
    // re-evaluates the key itself because skipped moves never run CreateMove.
    bool s_prevBindActive = false;

    static bool UpdateBindState()
    {
        const auto& cfg = g_config.settings.misc.doubletap;
        s_bindActive = Binds::Active( "doubletap", cfg.key, cfg.key_mode, "Double tap" );
        return s_bindActive;
    }

    static C_BaseEntity* ResolveLocal()
    {
        const int localIndex = g_pEngine ? g_pEngine->GetLocalPlayer() : 0;
        if ( !g_pEntityList || localIndex < 1 || localIndex > 64 )
            return nullptr;
        auto* local = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( localIndex ) );
        return local && IsEntityAlive( local ) ? local : nullptr;
    }

    static bool IsWeaponTimerReady(void* weapon, float engineTime)
    {
        if (!weapon || !std::isfinite(engineTime)) return false;
        const float nextPrimary = GetWeaponNextPrimaryAttack(weapon);
        return std::isfinite(nextPrimary) && nextPrimary <= engineTime;
    }
}

bool DoubleTap::Init()
{
    Reset();
    return true;
}

void DoubleTap::Shutdown()
{
    Reset();
}

void DoubleTap::PrepareCommand(CUserCmd* cmd, void* player)
{
    // Shot-armed drains keep attack buttons so the follow-up bullet can fire
    // from the ordinary pipeline. Release/defensive drains are movement-only:
    // attack input is stripped so a held LMB cannot turn the teleport into
    // an un-aimed shot burst.
    if ( cmd && Exploits->shifting_tickbase && !Exploits->drain_fire )
        cmd->buttons &= ~( IN_ATTACK | IN_ATTACK2 );
    (void)player;
}

bool DoubleTap::CanRunAttackPipeline()
{
    // Movement-only drains (release teleport / defensive) skip the whole
    // combat pipeline on extras, including rage and its auto-stop brake —
    // braking 14 drained ticks is what reset velocity on bind release.
    return !Exploits->shifting_tickbase || Exploits->drain_fire;
}

void DoubleTap::OnCreateMove( CUserCmd* cmd, void* pLocalPlayer )
{
    const auto& cfg = g_config.settings.misc.doubletap;
    auto* local = reinterpret_cast<C_BaseEntity*>( pLocalPlayer );

    UpdateBindState();

    if ( !cmd || !local || !g_pEngine || !g_pEngine->IsInGame() || !g_pEngine->IsConnected() ||
         !FakeLag::GameWindowUsable() )
    {
        Reset();
        return;
    }

    // ctx.feed for the reference state machine.
    Exploits->send_packet = FakeLag::GetChokedCommands() == 0;
    Exploits->planting_bomb = local->IsInBombZone() && local->GetProgressBarDuration() > 0;

    // Bind-release teleport: an observed active -> inactive edge spends the
    // REAL withheld charge as a movement-only drain burst through the same
    // reference transport (teleport_next_tick -> HandleTeleport). Reference
    // parity: the release always spends the charge — no combat-state veto.
    const bool bindReleased = s_prevBindActive && !s_bindActive;
    s_prevBindActive = s_bindActive;
    if ( bindReleased && Exploits->tickbase_shift > 0 &&
         !Exploits->shifting_tickbase && !Exploits->teleport_next_tick &&
         !FakeDuck::WantsControl() && FakeLag::CanControlPackets() &&
         FakeLag::GetChokedCommands() == 0 )
    {
        Exploits->drain_fire = false; // movement-only release teleport
        Exploits->ForceTeleport();
        DBGLOG( "[doubletap] RELEASE-TP shift=%d\n", Exploits->tickbase_shift );
    }

    // Donor CExploits::Run — resolves exploit type and the charge target.
    Exploits->Run( cmd );

    if ( !cfg.enabled || FakeDuck::WantsControl() )
    {
        Exploits->defensive_this_tick = false;
        return;
    }

    // Donor CExploits::UpdateTickbase — per-command tickbase_info bookkeeping.
    Exploits->UpdateTickbase( cmd );

    // Donor shot arming: a committed rage shot (or a manual, weapon-ready
    // attack) on a charged tickbase arms the drain burst and, with the
    // defensive option, marks this command as the defensive shot.
    const bool rageShot = Aimbot::HasCommittedRageShot( cmd->command_number );
    bool manualAttack = false;
    if ( !rageShot && ( cmd->buttons & IN_ATTACK ) )
    {
        void* weapon = local->GetActiveWeapon();
        const float curtime = EnginePrediction::CurTime();
        manualAttack = EnginePrediction::Reliable() && weapon &&
            std::isfinite( curtime ) &&
            std::isfinite( GetWeaponNextPrimaryAttack( weapon ) ) &&
            GetWeaponNextPrimaryAttack( weapon ) <= curtime &&
            std::isfinite( local->GetNextAttack() ) && local->GetNextAttack() <= curtime;
    }

    if ( ( rageShot || manualAttack ) && Exploits->IsCharged() && Exploits->tickbase_shift > 0 )
    {
        Exploits->defensive_this_tick = cfg.defensive;
        if ( cfg.defensive )
            Exploits->allow_defensive = true;
        Exploits->drain_fire = true; // shot drain: follow-up bullets allowed
        Exploits->ForceTeleport();
        AutoPeek::OnShotCommitted( cmd->command_number );
        DBGLOG( "[doubletap] ARM cmd=%d shift=%d target=%d defensive=%d\n",
            cmd->command_number, Exploits->tickbase_shift,
            Exploits->target_tickbase_shift, cfg.defensive ? 1 : 0 );
    }
    else if ( !rageShot && !manualAttack )
    {
        Exploits->defensive_this_tick = false;
    }
}

void DoubleTap::FinalizeCommand( CUserCmd* cmd )
{
    if ( !cmd || FakeDuck::WantsControl() )
        return;

    // Donor CExploits::Shift — ring pre-fill for break-LC / defensive
    // ticks on sending commands. The drain burst (HandleTeleport) and this
    // path never run for the same charge: LC_TickbaseShift returns 0 while
    // a teleport is armed.
    Exploits->Shift( cmd );
}

bool DoubleTap::IsShifting()
{
    return Exploits->shifting_tickbase;
}

bool DoubleTap::IsBindActive()
{
    return s_bindActive;
}

bool DoubleTap::IsBurstPending()
{
    return Exploits->shifting_tickbase || Exploits->teleport_next_tick;
}

bool DoubleTap::TrySkipMoveForRecharge()
{
    // Donor charge transport: a charge tick is not a modified command,
    // original CL_Move is simply not called. Re-evaluate the key here
    // because skipped moves do not enter CreateMove.
    const auto& cfg = g_config.settings.misc.doubletap;
    const bool bindActive = UpdateBindState();
    if ( FakeDuck::WantsControl() || !cfg.enabled || !bindActive || !g_pEngine ||
         !g_pEngine->IsInGame() || !g_pEngine->IsConnected() ||
         Exploits->shifting_tickbase || Exploits->teleport_next_tick ||
         !FakeLag::GameWindowUsable() )
        return false;

    // CL_Move runs before this frame's CreateMove refreshes g_pLocalPlayer.
    // Resolve the current roster entry first; never dereference yesterday's
    // pointer during a disconnect/map transition.
    const int localIndex = g_pEngine->GetLocalPlayer();
    void* currentLocal = g_pEntityList && localIndex >= 1 && localIndex <= 64
        ? g_pEntityList->GetClientEntity(localIndex) : nullptr;
    if (!currentLocal || currentLocal != g_pLocalPlayer)
    {
        Reset();
        return false;
    }
    auto* local = reinterpret_cast<C_BaseEntity*>(currentLocal);
    if (!IsEntityAlive(local))
    {
        Exploits->tickbase_shift = 0;
        Exploits->target_tickbase_shift = 0;
        Exploits->teleport_next_tick = false;
        return false;
    }
    // Donor send_packet gate: never withhold movement while commands are
    // still pending release.
    if (FakeLag::GetChokedCommands() != 0)
        return false;
    // Charge tick = skipped CL_Move, not a shot. Only a missing weapon
    // entity (nothing to read next tick) blocks it.
    if (!local->GetActiveWeapon())
        return false;

    if (!Exploits->ShouldCharge())
        return false;

    // Withholding a move freezes the client tickbase for that frame (no
    // CreateMove/prediction runs), so a cooldown that is still counting
    // would NEVER complete while charging — every follow-up shot waited for
    // the full charge before firing again. Recharge may only withhold moves
    // the weapon could not use anyway: while the cooldown runs, the normal
    // move executes, the tickbase advances and plain follow-up shots fire
    // immediately.
    void* weapon = local->GetActiveWeapon();
    const float playerTime = static_cast<float>( local->TickBase() ) * kTickInterval;
    const float nextPrimary = GetWeaponNextPrimaryAttack( weapon );
    if ( !std::isfinite( playerTime ) ||
         ( std::isfinite( nextPrimary ) && nextPrimary > playerTime ) )
        return false; // cooldown active — keep simulating, do not charge

    // Shooting priority: a ready shot beats charging. While frozen at the
    // ready point the weapon IS ready, so a click (or an auto-fire target)
    // always vetoes the skip and fires a plain shot right away — the banked
    // charge stays for the next engagement.
    const float nextAttack = local->GetNextAttack();
    const bool canFireNow = std::isfinite( nextAttack ) &&
        nextAttack <= playerTime + kTickInterval;
    if ( canFireNow &&
         ( ( GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) || Aimbot::HasTargetThisTick() ) )
        return false;

    ++Exploits->tickbase_shift;

    // CSSO fix — GetLocalPlayer()->GetFinalPredictedTick()++. A charge tick
    // skips CL_Move entirely: the engine never runs its prediction path for
    // this frame, so its own `inc [player+0x17E0]` never executes and the
    // client prediction clock falls behind the real frame clock. Advance the
    // same field once per skipped move.
    if ( g_pLocalPlayer )
    {
        ++(*reinterpret_cast<int*>(
            reinterpret_cast<uintptr_t>( g_pLocalPlayer ) +
            nFinalPredictedTickOffset ));
    }

    return true;
}

bool DoubleTap::RequestDefensiveShift()
{
    // Defensive DT spends the REAL charged ticks as a movement-only drain
    // burst. Never while a burst is already pending or while fake duck
    // owns the packets.
    if ( Exploits->tickbase_shift <= 0 ||
         FakeDuck::WantsControl() ||
         Exploits->shifting_tickbase || Exploits->teleport_next_tick )
        return false;

    Exploits->drain_fire = false; // movement-only defensive drain
    Exploits->ForceTeleport();
    DBGLOG( "[doubletap] DEFENSIVE shift=%d\n", Exploits->tickbase_shift );
    return true;
}

int DoubleTap::GetChargeTicks()
{
    return Exploits->tickbase_shift;
}

bool DoubleTap::IsRechargeSuppressed()
{
    // Missing weapon entity parks the recharge entirely. An actively
    // counting cooldown parks it too (withheld moves freeze the tickbase,
    // so the cooldown could never finish) — the HUD reports WAIT FIRE and
    // plain follow-up shots keep firing meanwhile.
    auto* local = ResolveLocal();
    if ( !local || !local->GetActiveWeapon() )
        return true;
    void* weapon = local->GetActiveWeapon();
    const float playerTime = static_cast<float>( local->TickBase() ) * kTickInterval;
    const float nextPrimary = GetWeaponNextPrimaryAttack( weapon );
    return std::isfinite( nextPrimary ) && nextPrimary > playerTime;
}

bool DoubleTap::IsRecharging()
{
    const auto& cfg = g_config.settings.misc.doubletap;
    if ( !cfg.enabled || !s_bindActive || Exploits->shifting_tickbase ||
         Exploits->teleport_next_tick || Exploits->target_tickbase_shift <= 0 )
        return false;

    return Exploits->tickbase_shift < Exploits->target_tickbase_shift;
}

DoubleTap::IndicatorState DoubleTap::GetIndicatorState()
{
    if ( !g_config.settings.misc.doubletap.enabled )
        return IndicatorState::Off;
    if ( Exploits->shifting_tickbase )
        return IndicatorState::Teleport;
    if ( !s_bindActive )
        return IndicatorState::BindInactive;
    if ( Exploits->teleport_next_tick )
        return IndicatorState::Armed;
    if ( Exploits->target_tickbase_shift <= 0 )
        return IndicatorState::Charging;
    if ( Exploits->tickbase_shift >= Exploits->target_tickbase_shift )
        return IndicatorState::Ready;
    if ( IsRechargeSuppressed() )
        return IndicatorState::WaitingForFire;
    return IndicatorState::Charging;
}

const char* DoubleTap::GetIndicatorText()
{
    switch ( GetIndicatorState() )
    {
    case IndicatorState::BindInactive:   return g_config.settings.misc.doubletap.key_mode == 1
                                             ? "DT | TOGGLE OFF" : "DT | HOLD";
    case IndicatorState::Charging:       return "DT | CHARGING";
    case IndicatorState::Ready:          return "DT | READY";
    case IndicatorState::Armed:          return "DT | ARMED";
    case IndicatorState::Shifting:       return "DT | SHIFT";
    case IndicatorState::Teleport:       return "DT | TELEPORT";
    case IndicatorState::WaitingForFire: return "DT | WAIT FIRE";
    default:                             return "DT | OFF";
    }
}

void DoubleTap::Reset()
{
    // Transient reference state only. tickbase_info/server_clock history is
    // donor-persistent across rounds; the withheld charge itself must not
    // survive death/disconnect (it is re-gated in TrySkipMoveForRecharge).
    Exploits->teleport_next_tick = false;
    Exploits->shifting_tickbase = false;
    Exploits->drain_fire = false;
    Exploits->target_tickbase_shift = 0;
    Exploits->block_charge = false;
    Exploits->fix_recharge = false;
    Exploits->fixed_recharge = false;
    Exploits->charged_command = 0;
    Exploits->charged_tickbase = 0;
    Exploits->defensive_this_tick = false;
    Exploits->allow_defensive = false;
    Exploits->force_charge = false;
    Exploits->shot_cmd = 0;
    Exploits->ResetDefensive();
    s_bindActive = false;
    s_prevBindActive = false;
}
