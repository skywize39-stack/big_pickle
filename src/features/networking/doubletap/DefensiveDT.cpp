// ============================================================
//  DefensiveDT.cpp — Defensive Double Tap (break LC on peek)
//
//  Ported onto the reference CExploits tickbase:
//    * Exploits->DefensiveDoubletap() runs every command and owns the
//      donor defensive window (defensive_this_tick -> in_defensive ->
//      defensive_ticks, bounded by the withheld tickbase_shift).
//    * Exploits->allow_defensive enables the donor ShouldBreakLC option,
//      so a DT shot during the window pre-fills break-LC movement
//      commands (Shift()).
//    * On a predicted peek (enemy about to see us, not seeing us yet)
//      the REAL charged ticks are spent as a movement-only drain burst
//      (ForceTeleport -> HandleTeleport): on the server we jump N ticks
//      ahead at once, which breaks lag compensation and gives peeker's
//      advantage. One trigger per peek, re-armed after kRearmTicks out
//      of sight. Never fires on IN_ATTACK and never while rage already
//      has a target — the charge belongs to the DT shot.
// ============================================================

#include "features/networking/doubletap/DefensiveDT.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/Exploits.hpp"
#include "features/combat/aimbot/Aimbot.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "render/surface/SurfaceRenderer.hpp"
#include "config/Config.hpp"

#include <cstring>
#include <cmath>

using namespace SDK;

namespace
{
    constexpr int   kRearmTicks     = 16;    // out of sight this long -> next peek may trigger
    constexpr int   kCooldownTicks  = 32;    // min ticks between two defensive shifts
    constexpr int   kMinLookahead   = 4;     // ticks of extrapolation at least
    constexpr float kMinPeekSpeed   = 40.f;  // u/s: slower is not a peek

    bool s_armed        = true;   // may trigger on the next peek
    int  s_hiddenTicks  = 0;      // consecutive ticks no enemy sees us
    int  s_cooldown     = 0;
    bool s_seenNow      = false;
    bool s_firedThisPeek = false;

    // Can the enemy eye see `point`? The enemy is skipped in the filter
    // (the ray starts inside his own head); reaching us (or the point) = visible.
    bool CanSee( void* localPtr, void* enemy, const Vec3& from, const Vec3& point )
    {
        auto* trace = g_pEngineTrace ? g_pEngineTrace : IEngineTrace::Get();
        if ( !trace )
            return false;

        Ray_t ray{};
        std::memset( &ray, 0, sizeof( ray ) );
        ray.m_Start   = from;
        ray.m_Delta   = point - from;
        ray.m_IsRay   = true;
        ray.m_IsSwept = true;

        CGameTrace tr{};
        CTraceFilterSimple filter( enemy );
        trace->TraceRay( ray, MASK_SHOT, &filter, &tr );

        return tr.fraction >= 0.97f || ( tr.m_pEnt && tr.m_pEnt == localPtr );
    }
}

bool DefensiveDT::OnCreateMove( CUserCmd* cmd, void* localPtr, int localTeam, int maxClients )
{
    const auto& cfg = g_config.settings.misc.doubletap;
    if ( !cfg.enabled || !cfg.defensive )
    {
        Exploits->allow_defensive = false;
        Reset();
        return false;
    }
    if ( !cmd || !localPtr || !g_pEntityList || !g_pEngine )
        return false;

    auto* local = reinterpret_cast<C_BaseEntity*>( localPtr );
    if ( !IsEntityAlive( local ) )
    {
        Reset();
        return false;
    }

    // Donor: the defensive window tracker runs on every command while the
    // defensive option is on. It consumes defensive_this_tick, which
    // DoubleTap::OnCreateMove sets on the DT shot command.
    Exploits->DefensiveDoubletap();
    Exploits->allow_defensive = Exploits->GetExploitType() == CExploits::E_DoubleTap;

    if ( s_cooldown > 0 )
        --s_cooldown;

    const int charge = DoubleTap::GetChargeTicks();
    const Vec3 eye = local->EyePosition();
    const Vec3 vel = local->GetVelocity();
    const float speed2D = sqrtf( vel.x * vel.x + vel.y * vel.y );

    // Where our head will be after the teleport (or a few ticks ahead).
    const int lookTicks = charge > kMinLookahead ? charge : kMinLookahead;
    const Vec3 futureEye = {
        eye.x + vel.x * kTickInterval * lookTicks,
        eye.y + vel.y * kTickInterval * lookTicks,
        eye.z };

    bool seenNow = false, seenSoon = false;
    const bool wantFuture = speed2D >= kMinPeekSpeed;
    for ( int i = 1; i <= maxClients && !seenNow; ++i )
    {
        auto* e = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( i ) );
        if ( !e || e == local || !IsEntityAlive( e ) ||
             SurfaceRenderer::IsEntityDormant( e ) ||
             e->GetTeamNum() == localTeam || e->GetTeamNum() <= 1 )
            continue;

        const Vec3 eEye = e->GetAbsOrigin() + e->GetViewOffset();
        if ( CanSee( localPtr, e, eEye, eye ) )
        {
            seenNow = true;
            break;
        }
        if ( wantFuture && !seenSoon && CanSee( localPtr, e, eEye, futureEye ) )
            seenSoon = true;
    }
    s_seenNow = seenNow;

    // Re-arm once we have been hidden long enough (peek finished).
    if ( seenNow )
        s_hiddenTicks = 0;
    else if ( s_hiddenTicks < 1000 )
        ++s_hiddenTicks;
    if ( !s_armed && s_hiddenTicks >= kRearmTicks )
    {
        s_armed = true;
        s_firedThisPeek = false;
    }

    // Trigger: about to be seen, not seen yet.
    if ( !seenSoon || seenNow || !s_armed || s_cooldown > 0 )
        return false;

    // Shooting / targets have priority: the charge belongs to the DT shot.
    if ( ( cmd->buttons & IN_ATTACK ) || Aimbot::HasTargetThisTick() )
        return false;
    if ( !DoubleTap::IsBindActive() || DoubleTap::IsBurstPending() ||
         Exploits->shifting_tickbase || AutoPeek::IsReturning() )
        return false;

    // The jump only breaks lag compensation with REAL withheld ticks.
    if ( !Exploits->IsCharged() || Exploits->tickbase_shift <= 0 ||
         Exploits->target_tickbase_shift <= 0 )
        return false;

    if ( !DoubleTap::RequestDefensiveShift() )
        return false;

    s_armed = false;
    s_firedThisPeek = true;
    s_cooldown = kCooldownTicks;
    return true;
}

void DefensiveDT::Reset()
{
    s_armed = true;
    s_hiddenTicks = 0;
    s_cooldown = 0;
    s_seenNow = false;
    s_firedThisPeek = false;
}

const char* DefensiveDT::GetIndicatorText()
{
    if ( s_firedThisPeek )
        return "DDT | BROKEN LC";
    if ( !s_armed )
        return "DDT | WAIT";
    if ( Exploits->tickbase_shift < 2 )
        return "DDT | CHARGING";
    return s_seenNow ? "DDT | VISIBLE" : "DDT | READY";
}
