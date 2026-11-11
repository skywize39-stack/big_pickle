#include "ui/binds/KeyBinds.hpp"
#include "features/combat/triggerbot/Triggerbot.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "core/prediction/EnginePrediction.hpp"

#include <cmath>
#include <Windows.h>

using namespace SDK;

static Vec3 AngleVectors( const Vec3& angles )
{
    float sp, sy, cp, cy;
    sp = sinf( angles.x * 3.14159265f / 180.f );
    cp = cosf( angles.x * 3.14159265f / 180.f );
    sy = sinf( angles.y * 3.14159265f / 180.f );
    cy = cosf( angles.y * 3.14159265f / 180.f );
    return Vec3{ cp * cy, cp * sy, -sp };
}

void Triggerbot::Run( void* pCInput, int sequenceNumber,
                      void* pLocalPlayer, int localTeam,
                      const EntitySnapshot& snap )
{
    if ( !g_config.settings.triggerbot.enabled || !pLocalPlayer || localTeam == 0 )
        return;

    auto& tb = g_config.settings.triggerbot;
    if ( tb.on_key && !Binds::Active( "triggerbot", tb.key, tb.key_mode, "Triggerbot" ) )
        return;

    auto* pEngineTrace = IEngineTrace::Get();
    if ( !pEngineTrace )
        return;

    auto** ppCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>( pCInput ) + CINPUT_OFFSET_COMMANDS );
    if ( !ppCommands || !*ppCommands )
        return;

    auto* commands = *ppCommands;
    int   idx      = sequenceNumber % MULTIPLAYER_BACKUP;
    auto* cmd      = &commands[idx];

    if ( cmd->command_number != sequenceNumber )
        return;

    auto* local = reinterpret_cast<C_BaseEntity*>( pLocalPlayer );
    // Predicted local eye — the server trace/fire happens from the
    // post-movement origin of this command. Enemy eyes stay live: they are
    // networked positions, lag compensation rewinds them server-side.
    Vec3 eyePos = EnginePrediction::EyePosition( local );

    float pitch = GetPitch( cmd );
    float yaw   = GetYaw( cmd );
    Vec3 forward = AngleVectors( Vec3{ pitch, yaw, 0.f } );

    Vec3 end = eyePos + forward * 8192.f;

    CTraceFilterSimple filter( local );

    // Crosshair direction trace — check if anything is in front
    __declspec(align(16)) Ray_t ray{};
    ray.m_Start    = eyePos;
    ray.m_Delta    = end - eyePos;
    ray.m_IsRay    = true;
    ray.m_IsSwept  = true;

    __declspec(align(16)) CGameTrace trace{};

    __try
    {
        pEngineTrace->TraceRay( ray, MASK_SHOT, &filter, &trace );
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        // printf_s( "[TRIG] EXCEPTION in TraceRay\n" );
        return;
    }

    if ( trace.fraction >= 1.0f )
    {
        // Nothing in front — nothing to shoot at
        return;
    }

    // We hit something.  Don't rely on trace.m_pEnt (CSSO returns a world
    // proxy, not a player pointer).  Instead iterate players and check if
    // any enemy is within a small angle of the crosshair.
    int    bestTarget = -1;
    float  bestDot    = 0.0f;

    for ( int i = 1; i <= snap.maxClients; ++i )
    {
        if ( !snap.players[i].alive || snap.players[i].team == localTeam ||
             snap.players[i].dormant )
            continue;

        // PlayerList: never trigger on a player with aiming disabled.
        if ( !PlayerList::AimAllowed( i ) )
            continue;

        auto* ent = reinterpret_cast<C_BaseEntity*>(
            g_pEntityList->GetClientEntity( i ) );
        if ( !ent )
            continue;

        Vec3 entEye = ent->EyePosition();
        Vec3 toEnt  = entEye - eyePos;

        // Manual length
        float dist = sqrtf( toEnt.x * toEnt.x + toEnt.y * toEnt.y + toEnt.z * toEnt.z );
        if ( dist < 1.0f )
            continue;

        // Normalize
        Vec3 dir = { toEnt.x / dist, toEnt.y / dist, toEnt.z / dist };

        // Manual dot product
        float dot = forward.x * dir.x + forward.y * dir.y + forward.z * dir.z;

        if ( dot > bestDot )
        {
            bestDot    = dot;
            bestTarget = i;
        }
    }

    if ( bestTarget < 0 || bestDot < 0.99f )
    {
        // printf_s( "[TRIG] no target frac=%.3f bestDot=%.4f\n",
        //           trace.fraction, bestDot );
        return;
    }

    // Best candidate found — trace directly to them to verify no wall
    auto* targetEnt = reinterpret_cast<C_BaseEntity*>(
        g_pEntityList->GetClientEntity( bestTarget ) );
    if ( !targetEnt )
        return;

    Vec3 targetEye = targetEnt->EyePosition();
    Vec3 delta     = targetEye - eyePos;
    float dist     = sqrtf( delta.x * delta.x + delta.y * delta.y + delta.z * delta.z );

    __declspec(align(16)) Ray_t toRay{};
    toRay.m_Start   = eyePos;
    toRay.m_Delta   = delta;
    toRay.m_IsRay   = true;
    toRay.m_IsSwept = true;

    __declspec(align(16)) CGameTrace toTrace{};

    __try
    {
        pEngineTrace->TraceRay( toRay, MASK_SHOT, &filter, &toTrace );
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        return;
    }

    // If trace reaches ≥95% of the distance to the target, path is clear
    if ( toTrace.fraction * dist < dist * 0.95f )
    {
        // printf_s( "[TRIG] blocked frac=%.3f to %d (dist=%.0f)\n",
        //           toTrace.fraction, bestTarget, dist );
        return;
    }

    if ( tb.hitgroup > 0 && toTrace.hitgroup != tb.hitgroup )
        return;

    cmd->buttons |= IN_ATTACK;
}
