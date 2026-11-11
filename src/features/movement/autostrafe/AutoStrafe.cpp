#include "features/movement/autostrafe/AutoStrafe.hpp"
#include "config/Config.hpp"
#include "sdk/SDK.hpp"
#include "core/hooks/CreateMoveHook.hpp"

#include <cmath>
#include <cstring>

using namespace SDK;

// ============================================================
//  AutoStrafe.cpp — predictive air strafe (port of the supplied
//  reference autostrafer).
//
//  The reference picks the strafe yaw by simulating CGameMovement::
//  AirMove for candidate yaws. CSSO's game movement functions are not
//  resolved in this SDK, but Source AirAccelerate is closed-form:
//  the wish speed clamps to 30 u/s and accelspeed reaches that cap for
//  a full 450 wishspeed, so the post-tick speed is |v + (30 - v·f)*f|,
//  maximized for the wish direction perpendicular to the velocity.
//  The reference's 0..110° sweep therefore converges to
//  velocity_angle ± 90°, and the rest of its logic — movement-key
//  bias, smoothing, wall avoidance, turn-side rules — is ported
//  verbatim below.
//
//  Movement write convention (matches CorrectMovement in this project):
//  for world wish yaw W and view yaw V:  forward = cos(W-V)*450,
//  sidemove = -sin(W-V)*450  (+sidemove = right = V-90).
// ============================================================

namespace
{
    constexpr float kStrafeSpeed      = 450.f;
    constexpr float kIdealStrafeAngle = 90.f;

    float NormalizeYaw( float yaw )
    {
        while ( yaw > 180.f )  yaw -= 360.f;
        while ( yaw < -180.f ) yaw += 360.f;
        return yaw;
    }

    void InitRay( Ray_t& ray, const Vec3& start, const Vec3& end )
    {
        memset( &ray, 0, sizeof( ray ) );
        ray.m_Start   = start;
        ray.m_Delta   = end - start;
        ray.m_IsRay   = true;
        ray.m_IsSwept = true;
    }

    bool TraceAhead( const Vec3& start, const Vec3& end, void* local, CGameTrace& pm )
    {
        memset( &pm, 0, sizeof( pm ) );
        Ray_t ray;
        InitRay( ray, start, end );
        CTraceFilterSimple filter( local );
        auto* eng = g_pEngineTrace ? g_pEngineTrace : IEngineTrace::Get();
        if ( !eng )
            return false;
        eng->TraceRay( ray, MASK_PLAYERSOLID, &filter, &pm );
        return true;
    }
}

void AutoStrafe::Run( CUserCmd* cmd, void* pLocalPlayer )
{
    const auto& cfg = g_config.settings.misc;
    if ( !cfg.autostrafe || !cmd || !pLocalPlayer )
        return;
    // Ladder/noclip are "airborne" without FL_ONGROUND: strafing there
    // overwrote the climb input.
    if ( SDK::IsOnLadderOrNoclip( pLocalPlayer ) )
        return;

    // ---- logical movement-key tracking (reference check_button) ----
    static int lastPressed  = 0;
    static int lastButtons  = 0;

    auto checkButton = [ & ]( const int button )
    {
        if ( cmd->buttons & button && ( !( lastButtons & button )
            || ( button & IN_MOVELEFT  && !( lastPressed & IN_MOVERIGHT ) )
            || ( button & IN_MOVERIGHT && !( lastPressed & IN_MOVELEFT  ) )
            || ( button & IN_FORWARD   && !( lastPressed & IN_BACK      ) )
            || ( button & IN_BACK      && !( lastPressed & IN_FORWARD   ) ) ) )
        {
            // easy strafe is always on: opposite keys clear each other so
            // W+D reports one clean direction
            if ( button & IN_MOVELEFT )       lastPressed &= ~IN_MOVERIGHT;
            else if ( button & IN_MOVERIGHT ) lastPressed &= ~IN_MOVELEFT;
            else if ( button & IN_FORWARD )   lastPressed &= ~IN_BACK;
            else if ( button & IN_BACK )      lastPressed &= ~IN_FORWARD;

            lastPressed |= button;
        }
        else if ( !( cmd->buttons & button ) )
            lastPressed &= ~button;
    };

    checkButton( IN_MOVELEFT );
    checkButton( IN_MOVERIGHT );
    checkButton( IN_FORWARD );
    checkButton( IN_BACK );

    lastButtons = cmd->buttons;

    auto* local = reinterpret_cast<C_BaseEntity*>( pLocalPlayer );
    if ( !IsEntityAlive( local ) )
        return;

    // Strafe while the jump intent is live (bhop cadence). The bhop
    // KeyButtons detour strips IN_JUMP from the command while airborne (it
    // re-adds it only on the ground tick for the re-jump), so the raw
    // +jump state captured there is the reliable signal — and it covers
    // ANY jump bind, not just space.
    const bool jumpHeld = ( cmd->buttons & IN_JUMP ) ||
                          Hooks::BhopJumpIntent();
    if ( !jumpHeld )
        return;

    // Air strafing is weapon-independent, including primed/released grenades.
    // Only forward/sidemove are changed below; view angles and attack buttons
    // stay untouched. The grenade inherits the resulting movement normally.

    if ( local->GetFlags() & FL_ONGROUND )
        return;

    const Vec3  velocity = local->GetVelocity();
    const float speed = sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
    if ( speed < 1.f )
        return;

    float velocityAngle = NormalizeYaw(
        atan2f( velocity.y, velocity.x ) * ( 180.f / 3.14159265f ) );

    float yaw = NormalizeYaw( GetYaw( cmd ) );

    // ---- movement-key bias (reference movement_keys mode) ----
    float offset = 0.f;
    if ( lastPressed & IN_MOVELEFT )  offset += 90.f;
    if ( lastPressed & IN_MOVERIGHT ) offset -= 90.f;
    if ( lastPressed & IN_FORWARD )   offset *= 0.5f;
    else if ( lastPressed & IN_BACK ) offset = ( -offset * 0.5f ) + 180.f;
    yaw = NormalizeYaw( yaw + offset );

    // ---- wall avoidance (reference avoid_walls) ----
    bool didAvoid = false;
    if ( cfg.autostrafe_avoid_walls )
    {
        const Vec3 pos = local->GetAbsOrigin();
        const float rad = yaw * ( 3.14159265f / 180.f );
        const Vec3 forward{ cosf( rad ), sinf( rad ), 0.f };

        CGameTrace pm{};
        const Vec3 traceTo = pos + forward * speed * kTickInterval * 5.f;
        if ( TraceAhead( pos, traceTo, pLocalPlayer, pm ) )
        {
            if ( pm.fraction < 1.f && fabsf( pm.plane.normal.z ) < 0.0001f )
            {
                // Clip (pos + forward) against the wall plane — the slide
                // direction reveals where the wall pushes the movement.
                const Vec3 in = pos + forward;
                const float backoff = in.x * pm.plane.normal.x +
                                      in.y * pm.plane.normal.y;
                const Vec3 out{
                    in.x - pm.plane.normal.x * backoff,
                    in.y - pm.plane.normal.y * backoff,
                    0.f };

                float outYaw = NormalizeYaw(
                    atan2f( out.y, out.x ) * ( 180.f / 3.14159265f ) );
                const float wallHitAngle = fabsf( NormalizeYaw( outYaw - yaw ) );

                if ( fabsf( wallHitAngle - 90.f ) > 15.f )
                {
                    if ( wallHitAngle > 90.f )
                        outYaw += 180.f;

                    // Step-up headroom: only avoid when there is room to
                    // slide up over the obstruction.
                    CGameTrace pm2{};
                    const float reach = 250.f * kTickInterval;
                    const Vec3 from{ pm.endpos.x + forward.x * reach,
                                     pm.endpos.y + forward.y * reach,
                                     pm.endpos.z };
                    const Vec3 to{ pm.endpos.x + forward.x * reach,
                                   pm.endpos.y + forward.y * reach,
                                   pm.endpos.z + 18.f + 0.03125f };
                    if ( TraceAhead( from, to, pLocalPlayer, pm2 ) )
                    {
                        if ( !pm2.startsolid && !pm2.allsolid )
                        {
                            didAvoid = true;
                            yaw = NormalizeYaw( NormalizeYaw( outYaw ) +
                                NormalizeYaw( outYaw - velocityAngle ) * 0.5f );
                        }
                    }
                }
            }
        }
    }

    // ---- ideal strafe angle + smoothing (reference calc sweep) ----
    const float velocityDelta = NormalizeYaw( yaw - velocityAngle );

    const float smoothing = cfg.autostrafe_smoothing;
    const float correct = kIdealStrafeAngle +
        fabsf( velocityDelta ) * 0.3f * ( ( 100.f - smoothing ) * 0.01f );

    if ( ( fabsf( velocityDelta ) > 170.f && speed > 80.f && !didAvoid ) ||
         velocityDelta > 0.f )
        yaw = NormalizeYaw( velocityAngle + correct );
    else if ( velocityDelta <= 0.f )
        yaw = NormalizeYaw( velocityAngle - correct );

    // ---- write the wish direction into the command ----
    const float relRad = NormalizeYaw( yaw - NormalizeYaw( GetYaw( cmd ) ) ) *
        ( 3.14159265f / 180.f );
    cmd->forwardmove.Encode( cosf( relRad ) * kStrafeSpeed );
    cmd->sidemove.Encode( -sinf( relRad ) * kStrafeSpeed );
}
