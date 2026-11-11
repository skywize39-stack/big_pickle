#include "ui/binds/KeyBinds.hpp"
#include "features/combat/antiaim/AntiAim.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "features/networking/fakelag/FakeLag.hpp"
#include "features/networking/fakeduck/FakeDuck.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "render/surface/SurfaceRenderer.hpp"

#include <imgui.h>
#include <Windows.h>
#include <cmath>
#include <cstring>

using namespace SDK;

// ============================================================
//  AntiAim.cpp
//
//  Runs after Aimbot, before the CRC sync, so the server-side
//  verified cmd carries the anti-aim angles.
//
//  Real/fake split:
//    - the server only processes the LAST command of a sent packet
//    - FakeLag chokes in-between ticks and flushes them all on the
//      next send, so the angles the server sees are on the sent tick
//    - anti-aim therefore applies to the SENT (non-choked) ticks;
//      choked ticks are skipped so they don't grab the camera
//    - if FakeLag is disabled every tick is a send tick
//
//  Yaw (Menu::Combo zero-based): 0=backward, 1=left, 2=right,
//  3=jitter, 4=spin, 5=backward-jitter.
//  Pitch (Menu::Combo zero-based): 0=down 89, 1=up 89, 2=zero.
// ============================================================

namespace
{
    static bool s_freestandToggleActive = false;
    static bool s_freestandKeyWasDown = false;
    static int  s_freestandBoundKey = 0;
    static int  s_freestandBoundMode = -1;

    // Jitter phase counter — counts APPLIED send ticks only (choked ticks
    // never advance it), so the alternation the enemy server observes stays
    // strict regardless of the fakelag pattern.
    static unsigned s_jitterAppliedTicks = 0;

    // Deterministic per-phase hash -> uniform float in [-1, +1]. The angles
    // are sent to the server anyway, so determinism is only needed to keep
    // one phase stable across its whole duration.
    static float JitterRandomPhaseFloat( unsigned phase )
    {
        uint32_t h = phase * 2654435761u + 0x9E3779B9u;
        h ^= h >> 16; h *= 2246822519u;
        h ^= h >> 13; h *= 3266489917u;
        h ^= h >> 16;
        return ( static_cast<float>( h & 0xFFFFu ) / 65535.f ) * 2.f - 1.f;
    }

    static float NormalizeYaw( float yaw )
    {
        while ( yaw > 180.f )  yaw -= 360.f;
        while ( yaw < -180.f ) yaw += 360.f;
        return yaw;
    }

    static void InitTraceRay( Ray_t& ray, const Vec3& start, const Vec3& end )
    {
        memset( &ray, 0, sizeof( ray ) );
        ray.m_Start = start;
        ray.m_Delta = end - start;
        ray.m_IsRay = true;
        ray.m_IsSwept = true;
    }

    static float ShoulderTraceFraction( IEngineTrace* engine, void* skipEntity,
                                        const Vec3& start, const Vec3& end )
    {
        if ( !engine )
            return 1.f;
        Ray_t ray{};
        InitTraceRay( ray, start, end );
        CGameTrace trace{};
        CTraceFilterSimple filter( skipEntity );
        engine->TraceRay( ray, MASK_SHOT_HULL, &filter, &trace );
        return trace.fraction < 0.f ? 0.f : ( trace.fraction > 1.f ? 1.f : trace.fraction );
    }

    static bool IsFreestandBindActive( const cfg::antiaim_t& aa )
    {
        return aa.freestand_on_key &&
            Binds::Active( "freestand", aa.freestand_key, aa.freestand_key_mode, "Freestanding" );
    }

    static constexpr float kPi = 3.14159265f;
    static constexpr float kDeg2Rad = kPi / 180.f;

    static Vec3 YawDir( float yawDeg )
    {
        const float r = yawDeg * kDeg2Rad;
        return { cosf( r ), sinf( r ), 0.f };
    }

    // At-targets target selection.
    //
    //  * Every live, non-dormant enemy is a candidate — all 360 degrees.
    //    (The old ">90 deg behind -> skip" rule made the base yaw fall
    //    back to the camera whenever you turned away from the enemy, so
    //    the AA suddenly showed him your head; facing away from a target
    //    behind you is exactly what "at targets" must do.)
    //  * Score = yaw FOV from the crosshair; ties are broken by distance.
    //  * Hysteresis: the current target is kept until another enemy is
    //    better by more than kSwitchFov degrees, so two enemies near the
    //    crosshair do not flip the base yaw every tick.
    static int s_atTargetIndex = 0;

    static bool IsValidAtTarget( C_BaseEntity* local, C_BaseEntity* e, int localTeam )
    {
        return e && e != local && !SurfaceRenderer::IsEntityDormant( e ) &&
               e->GetTeamNum() != localTeam && e->GetTeamNum() > 1 &&
               e->GetHealth() > 0 && e->GetLifeState() == 0;
    }

    static C_BaseEntity* FindTarget( C_BaseEntity* local, float viewYaw, float viewPitch )
    {
        ( void )viewPitch;   // yaw-only selection: pitch sign is inverted in this build
        if ( !local || !g_pEngine || !g_pEntityList )
        {
            s_atTargetIndex = 0;
            return nullptr;
        }
        constexpr float kSwitchFov = 15.f;

        const int localTeam  = local->GetTeamNum();
        const int maxClients = g_pEngine->GetMaxClients() < 65 ? g_pEngine->GetMaxClients() : 64;
        const Vec3 org = local->GetAbsOrigin();

        auto Score = [&]( C_BaseEntity* e, float& fovOut, float& distOut )
        {
            const Vec3 d = e->GetAbsOrigin() - org;
            const float yaw = atan2f( d.y, d.x ) / kDeg2Rad;
            fovOut  = fabsf( NormalizeYaw( yaw - viewYaw ) );
            distOut = sqrtf( d.x * d.x + d.y * d.y + d.z * d.z );
        };

        int   bestIdx = 0;
        float bestFov = 1e9f, bestDist = 1e9f;
        float curFov = 1e9f, curDist = 1e9f;
        bool  curValid = false;

        for ( int i = 1; i <= maxClients; ++i )
        {
            auto* e = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( i ) );
            if ( !IsValidAtTarget( local, e, localTeam ) )
                continue;
            float fov, dist;
            Score( e, fov, dist );
            if ( i == s_atTargetIndex )
            {
                curValid = true;
                curFov = fov;
                curDist = dist;
            }
            if ( fov < bestFov - 1.f || ( fabsf( fov - bestFov ) <= 1.f && dist < bestDist ) )
            {
                bestFov = fov;
                bestDist = dist;
                bestIdx = i;
            }
        }

        if ( curValid && bestIdx != s_atTargetIndex && curFov <= bestFov + kSwitchFov )
            bestIdx = s_atTargetIndex;   // keep the current target (hysteresis)
        ( void )curDist;

        s_atTargetIndex = bestIdx;
        return bestIdx ? reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( bestIdx ) )
                       : nullptr;
    }

    static float AtTargetYaw( C_BaseEntity* local, C_BaseEntity* target )
    {
        const Vec3 d = target->GetAbsOrigin() - local->GetAbsOrigin();
        return atan2f( d.y, d.x ) / kDeg2Rad;
    }

    // ------------------------------------------------------------------
    //  Freestanding — "auto direction" as done in public rage bases
    //  (supremacy / onetap-style):
    //
    //   1. candidates: back (at+180), left (at+90), right (at-90)
    //   2. for each, estimate our head position: eye + dir * 32
    //   3. walk the line enemy-eye -> head and measure how many units of
    //      solid (MASK_SHOT_HULL) a bullet would have to go through.
    //      Solid close to OUR head counts more (x1.25 / x1.25 / x2),
    //      because that is the wall that actually covers the head.
    //   4. pick the candidate with the most "penetrated distance".
    //      No solid anywhere -> back.
    //
    //  The engine has no exposed GetPointContents here, so the walk is
    //  done with TraceRay: jump to the next wall with one trace, then
    //  step through it in 4u point tests (startsolid) to get thickness.
    //  ~10-30 traces per candidate instead of hundreds.
    //
    //  Hysteresis: a new side must beat the current one clearly and the
    //  choice is held >= 150 ms, so the head doesn't flicker sides.
    // ------------------------------------------------------------------
    static constexpr float kFsRange = 32.f;
    static constexpr float kFsStep  = 4.f;

    static bool PointInSolid( IEngineTrace* engine, void* skip, const Vec3& p, const Vec3& dir )
    {
        Ray_t ray{};
        InitTraceRay( ray, p, p + dir * 0.5f );
        CGameTrace tr{};
        CTraceFilterSimple filter( skip );
        engine->TraceRay( ray, MASK_SHOT_HULL, &filter, &tr );
        return tr.startsolid || tr.allsolid;
    }

    static float PenetratedDistance( IEngineTrace* engine, void* skipEnemy,
                                     const Vec3& start, const Vec3& end )
    {
        Vec3 delta = end - start;
        const float len = sqrtf( delta.x * delta.x + delta.y * delta.y + delta.z * delta.z );
        if ( len <= 1.f )
            return 0.f;
        const Vec3 dir = delta * ( 1.f / len );

        float score = 0.f;
        float pos = 0.f;
        for ( int walls = 0; walls < 8 && pos < len; ++walls )
        {
            // Jump to the next surface along the line.
            Ray_t ray{};
            InitTraceRay( ray, start + dir * pos, end );
            CGameTrace tr{};
            CTraceFilterSimple filter( skipEnemy );
            engine->TraceRay( ray, MASK_SHOT_HULL, &filter, &tr );
            if ( tr.fraction >= 1.f && !tr.startsolid )
                break;
            pos += ( len - pos ) * ( tr.fraction < 0.f ? 0.f : tr.fraction );

            // Step through the solid.
            int steps = 0;
            while ( pos < len && steps < 64 &&
                    PointInSolid( engine, skipEnemy, start + dir * pos, dir ) )
            {
                float mult = 1.f;
                if ( pos > len * 0.5f )  mult = 1.25f;
                if ( pos > len * 0.75f ) mult = 1.25f;
                if ( pos > len * 0.9f )  mult = 2.f;
                score += kFsStep * mult;
                pos += kFsStep;
                ++steps;
            }
            pos += 0.5f; // leave the exit surface
        }
        return score;
    }

    static bool ResolveFreestandYaw( C_BaseEntity* local, C_BaseEntity* target,
                                     float atTarget, float& outYaw, int& outSide )
    {
        static int       s_choice = 0;      // 0 back, 1 left, 2 right
        static ULONGLONG s_lastSwitch = 0;

        if ( !local || !target )
            return false;
        auto* engineTrace = g_pEngineTrace ? g_pEngineTrace : IEngineTrace::Get();
        if ( !engineTrace )
            return false;

        const Vec3 eye      = local->EyePosition();
        const Vec3 enemyEye = target->EyePosition();

        const float yaws[3] = { atTarget + 180.f, atTarget + 90.f, atTarget - 90.f };
        float dist[3] = { 0.f, 0.f, 0.f };
        bool  any = false;
        for ( int i = 0; i < 3; ++i )
        {
            const Vec3 head = eye + YawDir( yaws[i] ) * kFsRange;
            dist[i] = PenetratedDistance( engineTrace, target, enemyEye, head );
            if ( dist[i] > 0.f )
                any = true;
        }

        int best = 0;
        if ( any )
        {
            for ( int i = 1; i < 3; ++i )
                if ( dist[i] > dist[best] )
                    best = i;
        }

        // Hysteresis
        const ULONGLONG now = GetTickCount64();
        if ( best != s_choice )
        {
            const bool clearlyBetter = dist[best] > dist[s_choice] * 1.15f + kFsStep;
            if ( clearlyBetter && now - s_lastSwitch >= 150 )
            {
                s_choice = best;
                s_lastSwitch = now;
            }
        }

        // s_choice = the side where the HEAD must end up (most cover).
        // On this model, with the anti-aim pitch, the head ends up on the
        // side OPPOSITE to the yaw direction (the original shoulder code
        // did the same), so we face away from the covered side.
        // "Flip FS Side" in the menu restores the direct mapping.
        const bool flip = g_config.settings.misc.antiaim.freestand_flip;
        if ( s_choice == 0 )
            outYaw = yaws[0];
        else
            outYaw = flip ? yaws[s_choice] : yaws[s_choice] + 180.f;
        outSide = s_choice == 1 ? 1 : ( s_choice == 2 ? 2 : 3 );
        return true;
    }

    // Manual keys: edge-triggered. Same key again releases the manual.
    static void UpdateManualKeys( const cfg::antiaim_t& aa )
    {
        static bool s_wasLeft = false, s_wasRight = false, s_wasBack = false;
        if ( !aa.manual_enabled )
        {
            AntiAim::manualSide = 0;
            s_wasLeft = s_wasRight = s_wasBack = false;
            return;
        }
        // Don't react while the game window is not focused (alt-tab, typing elsewhere).
        DWORD fgPid = 0;
        GetWindowThreadProcessId( GetForegroundWindow(), &fgPid );
        const bool focused = fgPid == GetCurrentProcessId();
        const bool l = focused && Binds::KeyDown( aa.manual_left_key );
        const bool r = focused && Binds::KeyDown( aa.manual_right_key );
        const bool b = focused && Binds::KeyDown( aa.manual_back_key );

        auto press = [&]( bool now, bool& was, int side )
        {
            if ( now && !was )
                AntiAim::manualSide = ( AntiAim::manualSide == side ) ? 0 : side;
            was = now;
        };
        press( l, s_wasLeft, 1 );
        press( r, s_wasRight, 2 );
        press( b, s_wasBack, 3 );
    }
}

float AntiAim::lastFakeYaw   = 0.f;
float AntiAim::lastFakePitch = 0.f;
int   AntiAim::manualSide    = 0;
int   AntiAim::freestandSide = 0;

void AntiAim::Run( void* pCInput, int seq, void* pLocalPlayer )
{
    const auto& aa = g_config.settings.misc.antiaim;
    UpdateManualKeys( aa );
    if ( !aa.enabled || !pCInput || !pLocalPlayer )
    {
        freestandSide = 0;
        return;
    }

    auto** ppCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>( pCInput ) + CINPUT_OFFSET_COMMANDS );
    if ( !ppCommands || !*ppCommands )
        return;

    auto* cmd = &( *ppCommands )[ seq % MULTIPLAYER_BACKUP ];
    if ( cmd->command_number != seq )
        return;

    // Grenades compute their throw arc from the command's viewangles. The
    // IN_ATTACK early-out below only protects the windup commands — the
    // release tick carries no attack button, took the fake pitch/yaw and
    // dumped every nade at the player's feet. Hold throwables out of
    // anti-aim entirely while they are the active weapon.
    auto* local = reinterpret_cast<C_BaseEntity*>( pLocalPlayer );

    // E (use): doors, buttons, hostages, defuse and ladders need the REAL
    // view angles — the server traces +use along them. Anti-aim stands down
    // for every command that carries IN_USE, and on ladders where the fake
    // pitch/yaw inverted climbing direction.
    if ( ( cmd->buttons & IN_USE ) || IsOnLadderOrNoclip( local ) )
        return;

    if ( void* weapon = local->GetActiveWeapon() )
    {
        const char* cls = reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>( weapon ) + nWeaponClassnameOffset );
        if ( cls && ( strstr( cls, "grenade" ) || strstr( cls, "flashbang" ) ||
             strstr( cls, "smoke" ) || strstr( cls, "molotov" ) ||
             strstr( cls, "incendiary" ) || strstr( cls, "decoy" ) ) )
            return;
    }

    // Never alter the aimbot command while firing. Still calculate and retain
    // the fake pose below: a revolver keeps IN_ATTACK down for its entire
    // cocking cycle, and returning here left thirdperson stuck on stale angles.
    // Shifted commands follow the same rule.
    const bool preserveShotCommand = ( cmd->buttons & ( IN_ATTACK | IN_ATTACK2 ) ) ||
                                     DoubleTap::IsShifting();

    const bool fakelagOn = FakeLag::IsActive() || FakeDuck::OwnsPackets();
    // FakeLag chokes broken ticks and sends the accumulated ones on the
    // next real tick. The server only ever sees the LAST command of the
    // packet, so anti-aim must go on the non-choked (sent) ticks, not on
    // the choked ones — otherwise the real angles win.
    const bool isSendTick = !fakelagOn || !FakeLag::s_shouldChoke;

    const float realYaw = GetYaw( cmd );
    float yaw   = realYaw;
    float pitch = GetPitch( cmd );

    if ( isSendTick )
    {
        switch ( aa.pitch_mode )
        {
        case 0: pitch = -89.f; break;  // Down (negative pitch = down in Source)
        case 1: pitch =  89.f; break;  // Up
        case 2: pitch =   0.f; break;  // Zero
        default: break;
        }

        // ---- Base yaw: camera or at-targets ----
        C_BaseEntity* target = FindTarget( local, realYaw, GetPitch( cmd ) );
        const float baseYaw = ( aa.base_yaw == 1 && target ) ? AtTargetYaw( local, target ) : realYaw;
        yaw = baseYaw;

        switch ( aa.yaw_mode )
        {
        case 0: yaw = baseYaw + 180.f; break;                                // Backward
        case 1: yaw = baseYaw +  90.f; break;                                // Left
        case 2: yaw = baseYaw -  90.f; break;                                // Right
        case 3: yaw = baseYaw + ( ( cmd->tick_count & 1 ) ?  90.f : -90.f ); break; // Jitter L/R
        case 4: yaw = baseYaw + fmodf( (float)cmd->tick_count * aa.spin_speed, 360.f ); break; // Spin
        case 5: yaw = baseYaw + ( ( cmd->tick_count & 1 ) ? 180.f :   0.f ); break; // Backward jitter
        default: break;
        }

        // ---- Priority: manual > freestand > yaw mode ----
        bool allowJitter = true;
        freestandSide = 0;
        if ( manualSide != 0 )
        {
            // Manuals are relative to the camera so the key always means
            // "screen left / right / back", regardless of targets.
            if ( manualSide == 1 )      yaw = realYaw + 90.f;
            else if ( manualSide == 2 ) yaw = realYaw - 90.f;
            else                        yaw = realYaw + 180.f;
            if ( aa.manual_disable_jitter )
                allowJitter = false;
        }
        else if ( IsFreestandBindActive( aa ) && target )
        {
            float fsYaw = yaw;
            int   side  = 0;
            if ( ResolveFreestandYaw( local, target, AtTargetYaw( local, target ), fsYaw, side ) )
            {
                yaw = fsYaw;
                freestandSide = side;
                if ( aa.freestand_disable_jitter )
                    allowJitter = false;
            }
        }

        // ----------------------------------------------------------
        //  Jitter — configurable yaw oscillation on top of the final
        //  base angle (yaw mode or freestand shoulder).
        //
        //    Offset : flips +range / -range every phase
        //    Random : uniform value in [-range, +range], re-rolled
        //             once per phase
        //    Sway   : triangle wave sweeping -range..+range over two
        //             phases (smooth swing)
        //
        //  The phase counter counts applied send ticks, so a phase is
        //  exactly jitter_speed observed ticks long even when fakelag
        //  chokes commands in between.
        // ----------------------------------------------------------
        if ( allowJitter && aa.jitter_enabled && aa.jitter_range >= 0.5f )
        {
            const int ticksPerPhase = aa.jitter_speed < 1 ? 1 : aa.jitter_speed;
            const unsigned tick = s_jitterAppliedTicks;
            ++s_jitterAppliedTicks;

            switch ( aa.jitter_mode )
            {
            case 1: // Random
                yaw += JitterRandomPhaseFloat( tick / (unsigned)ticksPerPhase )
                     * aa.jitter_range;
                break;
            case 2: // Sway — triangle over a 2-phase window
            {
                const unsigned step = tick % (unsigned)( ticksPerPhase * 2 );
                const float t = static_cast<float>( step )
                              / static_cast<float>( ticksPerPhase ); // 0..2
                const float tri = t <= 1.f ? ( t * 2.f - 1.f )
                                           : ( 3.f - t * 2.f );       // -1..1
                yaw += tri * aa.jitter_range;
                break;
            }
            default: // 0 = Offset flip
                yaw += ( ( tick / (unsigned)ticksPerPhase ) & 1u )
                     ? aa.jitter_range : -aa.jitter_range;
                break;
            }
        }
    }

    yaw = NormalizeYaw( yaw );
    pitch = pitch > 89.f ? 89.f : ( pitch < -89.f ? -89.f : pitch );

    if ( isSendTick )
    {
        // Preserve world movement only when this module is allowed to modify
        // the command itself. The thirdperson mirror below is safe either way.
        if ( !preserveShotCommand )
        {
            const float delta = ( yaw - realYaw ) * ( 3.14159265f / 180.f );
            if ( fabsf( delta ) > 0.001f )
            {
                const float fwd  = cmd->forwardmove.Decode();
                const float side = cmd->sidemove.Decode();
                cmd->forwardmove.Encode( fwd * cosf( delta ) - side * sinf( delta ) );
                cmd->sidemove.Encode(  fwd * sinf( delta ) + side * cosf( delta ) );
            }
        }

        // Store every send-tick pose for the FrameStageNotify thirdperson
        // mirror, including R8 cocking and DT commands.
        lastFakeYaw   = yaw;
        lastFakePitch = pitch;
    }

    if ( preserveShotCommand )
        return;

    SetYaw( cmd, yaw );
    SetPitch( cmd, pitch );
}

// ============================================================
//  Indicator: skeet-style arrows around the crosshair.
//  Active manual side is bright, others dim. "FS" under the
//  crosshair while freestand is actually turning the head.
// ============================================================
void AntiAim::DrawIndicator()
{
    const auto& aa = g_config.settings.misc.antiaim;
    if ( !aa.enabled || !g_pEngine || !g_pEngine->IsInGame() )
        return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c( ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f );
    const ImU32 on  = IM_COL32( 150, 200, 60, 255 );
    const ImU32 off = IM_COL32( 30, 30, 30, 140 );
    const ImU32 fs  = IM_COL32( 90, 175, 255, 255 );
    const float gap = 45.f, sz = 9.f;
    const int side = manualSide;

    if ( aa.manual_enabled && aa.manual_indicator )
    {
        // left
        dl->AddTriangleFilled( { c.x - gap - sz, c.y }, { c.x - gap, c.y - sz }, { c.x - gap, c.y + sz },
                               side == 1 ? on : ( freestandSide == 1 ? fs : off ) );
        // right
        dl->AddTriangleFilled( { c.x + gap + sz, c.y }, { c.x + gap, c.y + sz }, { c.x + gap, c.y - sz },
                               side == 2 ? on : ( freestandSide == 2 ? fs : off ) );
        // back
        dl->AddTriangleFilled( { c.x, c.y + gap + sz }, { c.x + sz, c.y + gap }, { c.x - sz, c.y + gap },
                               side == 3 ? on : off );
    }

    if ( side == 0 && freestandSide != 0 )
    {
        const char* t = "FS";
        const ImVec2 ts = ImGui::CalcTextSize( t );
        dl->AddText( { c.x - ts.x * 0.5f, c.y + 22.f }, fs, t );
    }
}
