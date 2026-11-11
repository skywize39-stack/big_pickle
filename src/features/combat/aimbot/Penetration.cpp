#include "features/combat/aimbot/Penetration.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include "sdk/SDK.hpp"
#include "sdk/build/CssoBuild.hpp"
#include "utils/Math.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>

using namespace SDK;

// server.dll penetration helper's primary ray uses exact MASK_SHOT (0x4600400B).
// This includes thin debris/prop contents that the previous local hull mask lost.
static constexpr unsigned int kBulletTraceMask = MASK_SHOT;

// ============================================================
//  Helpers
// ============================================================
static void InitRay( Ray_t& ray, const Vec3& start, const Vec3& end )
{
    memset( &ray, 0, sizeof( ray ) );
    ray.m_Start  = start;
    ray.m_Delta  = end - start;
    ray.m_IsRay  = true;
    ray.m_IsSwept = true;
}

static void TraceLine( const Vec3& start, const Vec3& end, int mask,
                       CGameTrace& trace, void* skip )
{
    memset( &trace, 0, sizeof( trace ) );
    Ray_t ray;
    InitRay( ray, start, end );
    CTraceFilterSimple filter( skip );
    auto* eng = g_pEngineTrace ? g_pEngineTrace : IEngineTrace::Get();
    if ( eng )
        eng->TraceRay( ray, mask, &filter, &trace );
    else
        trace.startsolid = trace.allsolid = true; // unavailable trace is not clear space
}

static float VecLen( const Vec3& v )
{
    return sqrtf( v.x * v.x + v.y * v.y + v.z * v.z );
}

static bool TraceReachesHitbox( const Vec3& start, const Vec3& end,
                                const HitboxInfo& hb, const CGameTrace& tr );

// ============================================================
//  ScaleDamage — hitgroup-based damage multipliers + armor
// ============================================================
static void ScaleDamage( void* player, const CGameTrace& enterTrace,
                         const WeaponData* wepInfo, float& currentDamage )
{
    if ( !wepInfo )
        return;

    int hitgroup = enterTrace.hitgroup;

    // hitgroup multiplier (applies regardless of victim pointer)
    switch ( hitgroup )
    {
    case HITGROUP_HEAD:
        currentDamage *= 4.0f;
        break;
    case HITGROUP_CHEST:
        currentDamage *= 1.0f;
        break;
    case HITGROUP_STOMACH:
        currentDamage *= 1.25f;
        break;
    case HITGROUP_LEFTARM:
    case HITGROUP_RIGHTARM:
        currentDamage *= 1.0f;
        break;
    case HITGROUP_LEFTLEG:
    case HITGROUP_RIGHTLEG:
        currentDamage *= 0.75f;
        break;
    default:
        break;
    }

    if ( !player )
        return;

    // armor scaling (no heavy armor in this game).
    int armor = *reinterpret_cast<const int*>(
        reinterpret_cast<uintptr_t>( player ) + nArmorOffset );
    if ( armor <= 0 )
        return;

    bool hasHelmet = *reinterpret_cast<const bool*>(
        reinterpret_cast<uintptr_t>( player ) + nHelmetOffset );

    bool armored = ( hitgroup == HITGROUP_HEAD && hasHelmet ) ||
                   ( hitgroup >= HITGROUP_CHEST && hitgroup <= HITGROUP_RIGHTARM );
    if ( !armored )
        return;

    float ratio      = wepInfo->armorRatio * 0.5f;
    float newDamage  = currentDamage * ratio;
    float bonusRatio = 0.5f;

    if ( ( currentDamage - newDamage ) * bonusRatio > armor )
        newDamage = currentDamage - ( armor / bonusRatio );

    currentDamage = floorf( newDamage );
}

// ============================================================
//  trace_to_exit — step-based content scan (reference autowall)
//  Steps 4 units at a time using GetPointContents.
//  When content changes, traces back 4 units for exact exit.
// ============================================================
static bool TraceToExit( const CGameTrace& enterTrace, CGameTrace& exitTrace,
                         const Vec3& startPos, const Vec3& direction )
{
    auto* eng = g_pEngineTrace ? g_pEngineTrace : IEngineTrace::Get();
    if ( !eng )
        return false;

    // server.dll uses MASK_SHOT for the primary contents query/backtrace, then
    // uses the separate MASK_SHOT_HULL only in its hitbox edge fallback below.
    const int exitMask = MASK_SHOT;

    float currentDistance = 0.0f;

    while ( currentDistance <= 90.0f )
    {
        currentDistance += 4.0f;
        Vec3 start = startPos + direction * currentDistance;

        int pointContents = eng->GetPointContents( start, exitMask, nullptr );
        if ( !( pointContents & exitMask ) )
        {
            Vec3 end = start - direction * 4.0f;
            TraceLine( start, end, exitMask, exitTrace, nullptr );

            if ( exitTrace.startsolid && ( exitTrace.surface.flags & SURF_HITBOX ) )
            {
                TraceLine( start, startPos, MASK_SHOT_HULL, exitTrace, (void*)1 );
                if ( exitTrace.fraction < 1.0f && !exitTrace.startsolid )
                    return true;
                continue;
            }

            // No valid backward trace means no confirmed back face. Even a
            // breakable entity must provide an engine exit; fabricating one
            // here was another zero-thickness false-positive path.
            if ( exitTrace.fraction >= 1.0f || exitTrace.startsolid )
                continue;

            if ( exitTrace.surface.flags & SURF_NODRAW )
            {
                if ( fnIsBreakableEntity && fnIsBreakableEntity( exitTrace.m_pEnt ) &&
                     fnIsBreakableEntity( enterTrace.m_pEnt ) )
                    return true;

                if ( !( enterTrace.surface.flags & SURF_NODRAW ) )
                    continue;
            }

            if ( ( exitTrace.plane.normal.x * direction.x +
                   exitTrace.plane.normal.y * direction.y +
                   exitTrace.plane.normal.z * direction.z ) <= 1.0f )
            {
                // server TraceToExit reconstructs exit from this probe point.
                // TraceRay already put endpos on the backward hit; subtracting
                // again from endpos double-offsets thin-wall exits inward.
                exitTrace.endpos = start - direction * ( exitTrace.fraction * 4.0f );
                return true;
            }
        }
    }

    return false;
}

// ============================================================
//  HandleBulletPenetration — damage reduction through walls
//  Ported from reference autowall logic
// ============================================================
static bool HandleBulletPenetration( const WeaponData* wepInfo,
                                     CGameTrace& enterTrace,
                                     Vec3& eyePosition,
                                     const Vec3& direction,
                                     int& possibleHitsRemaining,
                                     float& currentDamage,
                                     float penetrationPower )
{
    auto* props = ISurfacePropsClient::Get();
    if ( !props )
        return false;

    CGameTrace exitTrace{};

    auto* enterSurfaceData = props->GetSurfaceData( enterTrace.surface.surfaceProps );
    if ( !enterSurfaceData )
        return false;

    uint16_t enterMaterial = enterSurfaceData->gamematerial;

    const bool exitOk = TraceToExit( enterTrace, exitTrace, enterTrace.endpos, direction );

    if ( !wepInfo->penetration )
        return false;

    // A penetration is accepted only after the engine confirmed a real exit
    // surface. The previous zero-thickness fallback was the direct source of
    // shots into thick/unpenetrable walls: it invented an exit at entry.
    if ( !exitOk )
        return false;

    auto* exitSurfaceData = props->GetSurfaceData( exitTrace.surface.surfaceProps );
    if ( !exitSurfaceData )
        return false;

    uint16_t exitMaterial = exitSurfaceData->gamematerial;

    // Exact reference branch order: glass/grate have cheap loss; solid and
    // light surfaces use a neutral modifier; normal surfaces use the average
    // of entry/exit penetration modifiers; equal wood/cardboard and plastic
    // override that average. The old code multiplied a modifier into every
    // branch, so it could rate dense CSSO world geometry as penetrable.
    const float enterPenMod = enterSurfaceData->penetrationModifier;
    const float exitPenMod  = exitSurfaceData->penetrationModifier;
    if ( !std::isfinite( enterPenMod ) || !std::isfinite( exitPenMod ) )
        return false;

    const bool solidSurface = ( enterTrace.contents & CONTENTS_SOLID ) != 0;
    const bool lightSurface = ( enterTrace.surface.flags & SURF_LIGHT ) != 0;
    const bool glassOrGrate = enterMaterial == CHAR_TEX_GLASS ||
                              enterMaterial == CHAR_TEX_GRATE;
    // Fatality's material branch permits glass/grate before ordinary surface
    // modifier validation. All other zero-modifier materials are rejected.
    if ( !glassOrGrate && ( enterPenMod < 0.1f || exitPenMod < 0.1f ) )
        return false;

    float combinedPenMod = 0.f;
    float finalDamageMod = 0.16f;
    if ( glassOrGrate || ( enterTrace.surface.flags & SURF_NODRAW ) )
    {
        combinedPenMod = glassOrGrate ? 3.0f : 1.0f;
        if ( glassOrGrate )
            finalDamageMod = 0.05f;
    }
    else if ( solidSurface || lightSurface )
        combinedPenMod = 1.0f;
    else
        combinedPenMod = ( enterPenMod + exitPenMod ) * 0.5f;

    if ( enterMaterial == exitMaterial )
    {
        if ( exitMaterial == CHAR_TEX_CARDBOARD || exitMaterial == CHAR_TEX_WOOD )
            combinedPenMod = 3.0f;
        else if ( exitMaterial == CHAR_TEX_PLASTIC )
            combinedPenMod = 2.0f;
    }

    if ( combinedPenMod <= 0.0f || penetrationPower <= 0.0f )
        return false;

    Vec3 thickDelta = exitTrace.endpos - enterTrace.endpos;
    const float thicknessSq = thickDelta.x * thickDelta.x +
                              thickDelta.y * thickDelta.y +
                              thickDelta.z * thickDelta.z;
    const float modifierInv = 1.0f / combinedPenMod;
    const float thicknessLoss = thicknessSq * modifierInv / 24.0f;
    const float baseLoss = currentDamage * finalDamageMod +
        modifierInv * 3.0f * ( 3.0f / penetrationPower ) * 1.25f;
    float loss = baseLoss + thicknessLoss;
    if ( loss < 0.0f )
        loss = 0.0f;

    if ( currentDamage - loss < 1.0f )
    {
        return false;
    }

    currentDamage -= loss;

    eyePosition = exitTrace.endpos;
    --possibleHitsRemaining;

    return true;
}

// ============================================================
//  FireBullet — main penetration loop
//  Traces, applies range falloff, loops up to 4 wall passes
// ============================================================
static bool FireBullet( void* localPlayer, void* targetEntity,
                        const Vec3& eyePos, const Vec3& targetPos,
                        const WeaponData* wepInfo, const HitboxInfo* expectedHitbox,
                        int expectedHitgroup, float& outDamage,
                        int& outHitgroup, bool& outVisible )
{
    if ( !localPlayer || !wepInfo )
        return false;

    Vec3 direction = targetPos - eyePos;
    float totalLen = VecLen( direction );
    if ( !std::isfinite(totalLen) || totalLen <= 0.0f ||
         !std::isfinite(wepInfo->range) || wepInfo->range <= 0.0f )
        return false;

    direction.x /= totalLen;
    direction.y /= totalLen;
    direction.z /= totalLen;

    // Clip the ray, not the entire shot: range-sized spread rays can round
    // slightly past range, while the selected hitbox is well before it.
    // Geometry below still rejects hitboxes beyond the clipped segment.
    const float targetDist = (std::min)(totalLen, wepInfo->range);

    Vec3  eyePosition    = eyePos;
    float currentDamage  = (float)wepInfo->damage;
    float currentDistance = 0.0f;
    float penetrationPower = wepInfo->penetration;
    float rangeModifier   = wepInfo->rangeModifier;
    int   possibleHitsRemaining = 4;

    outVisible = true;
    outHitgroup = -1;

    while ( possibleHitsRemaining > 0 && currentDamage >= 1.0f )
    {
        // Trace ONLY up to the target. Anything behind it (another wall, the
        // ground, a player) must not count against us — the reference clips
        // the ray to the target for the same reason.
        float remainingToTarget = targetDist - currentDistance;
        if ( remainingToTarget <= 0.0f )
            break;

        Vec3 end = eyePosition + direction * remainingToTarget;
        if (expectedHitbox)
        {
            const float entry = Penetration::HitboxEntryFraction(eyePosition, end, *expectedHitbox);
            if (entry < 0.f) break;
            remainingToTarget = (std::min)(remainingToTarget, remainingToTarget * entry + 0.01f);
            end = eyePosition + direction * remainingToTarget;
        }

        CGameTrace enterTrace;
        TraceLine( eyePosition, end, kBulletTraceMask, enterTrace, localPlayer );

        if (enterTrace.startsolid || enterTrace.allsolid ||
            !std::isfinite(enterTrace.fraction) || enterTrace.fraction < 0.f || enterTrace.fraction > 1.f)
            break;

        if ( enterTrace.fraction >= 1.0f )
        {
            // A clear ray to an aimpoint is NOT proof of a hit: for a stored
            // lag-record the live entity may no longer be there, and the old
            // code treated that empty ray as a wallbang. Confirm the exact
            // recorded hitbox before accepting damage after every wall exit.
            if ( !expectedHitbox ||
                 !Penetration::CanHit( eyePosition, end, *expectedHitbox ) )
                break;

            currentDamage *= powf(rangeModifier, remainingToTarget / 500.f);
            CGameTrace expectedTrace{};
            expectedTrace.hitgroup = expectedHitgroup;
            ScaleDamage( targetEntity, expectedTrace, wepInfo, currentDamage );
            outDamage   = currentDamage;
            outHitgroup = expectedHitgroup;
            outVisible  = true;
            return true;
        }

        float traceDist = enterTrace.fraction * remainingToTarget;
        currentDistance += traceDist;

        // Range falloff: damage decays with distance of the current segment
        currentDamage *= powf( rangeModifier, traceDist / 500.0f );

        // No penetrationModifier gate on entry: the server's TraceAttack
        // (server.dll sub_18035F400) has none — in CS:S grate/glass carry
        // penMod 0.0 yet are the easiest surfaces to wallbang (lossMod 3.0,
        // lossTerm 0.05). The loss formula in HandleBulletPenetration is the
        // only stop, matching the server.

        // CSSO can expose the world collision proxy in m_pEnt even when this
        // ray terminates on a player capsule. The project triggerbot already
        // handles that engine behaviour without trusting m_pEnt. For rage,
        // accept the proxy only if the trace ends near the requested point AND
        // the exact selected capsule/AABB is geometrically crossed. This keeps
        // target identity strict without map-dependent entity-pointer rejects.
        const bool isPlayer = ( enterTrace.fraction < 1.0f && enterTrace.hitgroup > 0 );
        const bool crossesExpected = expectedHitbox &&
            TraceReachesHitbox( eyePosition, end, *expectedHitbox, enterTrace );
        const bool taggedTarget = isPlayer && enterTrace.m_pEnt == targetEntity &&
            ( expectedHitgroup < 0 || enterTrace.hitgroup == expectedHitgroup ) &&
            (!expectedHitbox || crossesExpected);

        if ( taggedTarget || crossesExpected )
        {
            CGameTrace hitTrace = enterTrace;
            if ( !taggedTarget )
            {
                hitTrace.hitgroup = expectedHitgroup;
                static unsigned int s_proxyAccepts = 0;
                if ( (++s_proxyAccepts & 31u) == 1u )
                    DBGLOG( "[rage] CSSO trace proxy accepted frac=%.3f hg=%d\n",
                        enterTrace.fraction, expectedHitgroup );
            }
            ScaleDamage( targetEntity, hitTrace, wepInfo, currentDamage );
            outDamage = currentDamage;
            outHitgroup = taggedTarget ? enterTrace.hitgroup : expectedHitgroup;
            outVisible = true;
            return true;
        }

        // A genuine player collision that does not geometrically cross the
        // requested target remains blocked; do not promote an arm/chest contact
        // into the selected head/stomach point.
        if ( isPlayer )
            break;

        auto* props = ISurfacePropsClient::Get();
        const auto* enterSurface = props
            ? props->GetSurfaceData( enterTrace.surface.surfaceProps ) : nullptr;
        // server.dll default sv_penetration_type=1 does not reject an entry
        // surface solely for penMod < 0.1 before material-specific branches.
        // Keep pointer/NaN protection here; HandleBulletPenetration still
        // requires a real exit, valid ordinary modifiers and enough damage.
        if ( !enterSurface || !std::isfinite( enterSurface->penetrationModifier ) )
            break;

        const Vec3 enterPos = enterTrace.endpos;
        if ( !HandleBulletPenetration( wepInfo, enterTrace, eyePosition, direction,
                                       possibleHitsRemaining, currentDamage,
                                       penetrationPower ) )
            break;

        // The previous pipeline omitted wall thickness from distance/range
        // accounting. Include it before the next trace; otherwise damage was
        // overestimated through dense or multi-layer walls.
        const float thickness = VecLen( eyePosition - enterPos );
        currentDistance += thickness;
        if ( currentDistance > wepInfo->range )
            break;
        currentDamage *= powf( rangeModifier, thickness / 500.0f );
        outVisible = false;
    }

    outDamage = currentDamage;
    return false;
}

// ============================================================
//  GetEstimatedDamage — main entry for penetration calc
// ============================================================
float Penetration::GetEstimatedDamage(
    void* localPlayer,
    void* targetEntity,
    const Vec3& eyePos,
    const Vec3& targetPos,
    float currentDamage,
    const WeaponData* wepInfo )
{
    if ( !localPlayer || !wepInfo )
        return 0.0f;

    bool  visible  = false;
    int   hitgroup = -1;
    float damage   = 0.0f;

    if ( FireBullet( localPlayer, targetEntity, eyePos, targetPos, wepInfo,
                     nullptr, -1, damage, hitgroup, visible ) )
        return damage;

    return 0.0f;
}

// ============================================================
//  DirectDamage — base damage with range falloff
// ============================================================
static float DirectDamage( const WeaponData* wepInfo, float distance )
{
    if ( !wepInfo )
        return 0.0f;

    if ( distance > wepInfo->range )
        return 0.0f; // out of weapon range — bullet falls short

    return (float)wepInfo->damage *
           powf( wepInfo->rangeModifier, distance / 500.0f );
}

// ============================================================
//  Calculate — full result: direct + through-walls
// ============================================================
PenetrationResult Penetration::Calculate(
    void* localPlayer,
    void* targetEntity,
    const Vec3& eyePos,
    const Vec3& targetPos,
    const WeaponData* wepInfo,
    const HitboxInfo* expectedHitbox,
    int expectedHitgroup )
{
    PenetrationResult result{};
    result.distance = VecLen( targetPos - eyePos );
    result.damage   = 0.0f;

    if ( !localPlayer || !targetEntity || !wepInfo )
        return result;
    if (expectedHitbox)
    {
        const float entry = HitboxEntryFraction(eyePos, targetPos, *expectedHitbox);
        if (entry < 0.f) return result;
        result.distance *= entry;
    }

    // Direct trace
    CGameTrace directTrace;
    TraceLine( eyePos, targetPos, kBulletTraceMask, directTrace, localPlayer );

    if (directTrace.startsolid || directTrace.allsolid ||
        !std::isfinite(directTrace.fraction) || directTrace.fraction < 0.f || directTrace.fraction > 1.f)
        return result;

    // CSSO TraceRay can return a world proxy in m_pEnt for a player impact.
    // Confirm target identity through the selected stored/live hitbox whenever
    // the trace ends close to its aimpoint. This mirrors the project triggerbot
    // and prevents whole-map rage silence when pointer identity changes with
    // the BSP/entity collision path.
    const bool crossesExpected = expectedHitbox &&
        TraceReachesHitbox( eyePos, targetPos, *expectedHitbox, directTrace );
    // A live entity pointer cannot validate an old/different selected pose.
    // Its collision must be at the selected shape, not merely the same group.
    const bool hitTarget = directTrace.fraction < 1.f && directTrace.hitgroup > 0 &&
        directTrace.m_pEnt == targetEntity &&
        ( expectedHitgroup < 0 || directTrace.hitgroup == expectedHitgroup ) &&
        (!expectedHitbox || crossesExpected);

    if ( hitTarget || crossesExpected )
    {
        result.visible   = true;
        result.penetrate = false;
        result.hitgroup  = hitTarget ? directTrace.hitgroup : expectedHitgroup;
        result.damage    = DirectDamage( wepInfo, result.distance );

        // Stored records are not currently present in the engine collision
        // tree. For those clear rays use the selected record hitgroup only
        // after its own capsule/AABB was crossed; never treat empty space as a
        // valid player hit.
        if ( hitTarget )
            ScaleDamage( targetEntity, directTrace, wepInfo, result.damage );
        else
        {
            CGameTrace expectedTrace{};
            expectedTrace.hitgroup = expectedHitgroup;
            ScaleDamage( targetEntity, expectedTrace, wepInfo, result.damage );
        }
        return result;
    }

    // Wall between us and target — calculate penetration.
    // A wall was already proven to block the direct line, so even when the
    // bullet exits cleanly this is a wallbang, not a direct (visible) hit.
    bool  penVisible = false;
    int   penHitgroup = -1;
    float penDamage   = 0.0f;

    if ( FireBullet( localPlayer, targetEntity, eyePos, targetPos, wepInfo,
                     expectedHitbox, expectedHitgroup, penDamage, penHitgroup,
                     penVisible ) )
    {
        result.visible   = false;
        result.penetrate = true;
        result.hitgroup  = penHitgroup;
        result.damage    = penDamage;
    }

    return result;
}

// ============================================================
//  CanHit / CheckHitchance — Pandora port (record-based hit chance)
// ============================================================

namespace
{
    using RandomSeedFn  = void  ( __cdecl* )( int );
    using RandomFloatFn = float ( __cdecl* )( float, float );
    using GetInaccuracyFn = float ( __fastcall* )( void* );
    using GetSpreadFn     = float ( __fastcall* )( void* );
    using UpdateAccuracyFn= void  ( __fastcall* )( void* );

    RandomSeedFn      fnRandomSeed   = nullptr;
    RandomFloatFn     fnRandomFloat  = nullptr;
    GetInaccuracyFn   fnGetInaccuracy = nullptr;
    GetSpreadFn       fnGetSpread    = nullptr;
    UpdateAccuracyFn  fnUpdateAccuracy = nullptr;
    bool              g_randomResolved = false;

    void ResolveRandom()
    {
        if ( g_randomResolved )
            return;
        g_randomResolved = true;

        HMODULE hVstd = GetModuleHandleA( "vstdlib.dll" );
        if ( hVstd )
        {
            fnRandomSeed  = reinterpret_cast<RandomSeedFn>( GetProcAddress( hVstd, "RandomSeed" ) );
            fnRandomFloat = reinterpret_cast<RandomFloatFn>( GetProcAddress( hVstd, "RandomFloat" ) );
        }

        HMODULE hClient = GetModuleHandleA( "client.dll" );
        if ( hClient )
        {
            // C_WeaponCSBaseGun real accuracy functions — all take the weapon
            // entity (the game calls them with rcx = weapon, e.g. in the fire
            // path sub_18030C7B0 / sub_180307660).
            fnGetInaccuracy  = reinterpret_cast<GetInaccuracyFn>(
                reinterpret_cast<uintptr_t>( hClient ) + 0x30A950 );
            fnGetSpread      = reinterpret_cast<GetSpreadFn>(
                reinterpret_cast<uintptr_t>( hClient ) + 0x30B010 );
            fnUpdateAccuracy = reinterpret_cast<UpdateAccuracyFn>(
                reinterpret_cast<uintptr_t>( hClient ) + 0x30DD10 );
        }
    }

    bool ReadNegevRecoil(void* weapon,float& recoil)
    {
        recoil=3.f; // ordinary distribution once Negev's recoil index >= 3
        if (!weapon) return true;
        __try
        {
            const auto base=reinterpret_cast<uintptr_t>(weapon);
            const auto* name=reinterpret_cast<const char*>(base+nWeaponClassnameOffset);
            if (std::strcmp(name,"weapon_negev")==0)
            {
                recoil=*reinterpret_cast<const float*>(base+Build::Weapons::RecoilIndex);
                if (!std::isfinite(recoil) || recoil<0.f) return false;
            }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // CSSO primary fire consumes two independent radius/angle pairs.
    // Negev additionally remaps BOTH radii until recoilIndex reaches 3:
    // client+0x2B0492/+0x2B05A6, server+0x375530/+0x375636.
    // Revolver's inverted radius is secondary-mode only; primary rage must
    // NOT apply it just because its active weapon happens to be an R8.
    Vec2 CalculateSpreadCSSO(int seed,float inaccuracy,float spread,float negevRecoil=3.f)
    {
        if (!fnRandomSeed || !fnRandomFloat) return {};
        fnRandomSeed((seed&0xFF)+1);
        float inaccuracyRadius=fnRandomFloat(0.f,1.f);
        const float inaccuracyAngle=fnRandomFloat(0.f,6.2831855f);
        float spreadRadius=fnRandomFloat(0.f,1.f);
        const float spreadAngle=fnRandomFloat(0.f,6.2831855f);
        if (negevRecoil<3.f)
        {
            for (int power=3;power>negevRecoil;--power)
            { inaccuracyRadius*=inaccuracyRadius;spreadRadius*=spreadRadius; }
            inaccuracyRadius=1.f-inaccuracyRadius;
            spreadRadius=1.f-spreadRadius;
        }
        inaccuracyRadius*=inaccuracy;spreadRadius*=spread;
        return {cosf(inaccuracyAngle)*inaccuracyRadius+cosf(spreadAngle)*spreadRadius,
                sinf(inaccuracyAngle)*inaccuracyRadius+sinf(spreadAngle)*spreadRadius};
    }


}

// ============================================================
//  CanHit / CheckHitchance — Fatality calc_hc port
// ============================================================

// Use CSSO's actual forward/right/up spread basis. Fatality's reflected
// angle approximation belongs to its own trace/animation pipeline and must
// not be layered onto this game's native four-draw bullet spread.

namespace
{
    // Perform intersection arithmetic in double. At long ranges float's
    // quadratic discriminant rounded near-misses into capsule tangencies.
    struct HitVec
    {
        double x, y, z;
        HitVec operator-(const HitVec& v) const { return {x-v.x,y-v.y,z-v.z}; }
        HitVec operator+(const HitVec& v) const { return {x+v.x,y+v.y,z+v.z}; }
        HitVec operator*(double f) const { return {x*f,y*f,z*f}; }
    };
    static HitVec Precise(const Vec3& v) { return {v.x,v.y,v.z}; }
    static double Dot(const HitVec& a,const HitVec& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    static bool FinitePoint(const Vec3& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

    static float CapsuleEntry(const Vec3& start,const Vec3& end,const HitboxInfo& hb)
    {
        if (!FinitePoint(hb.world_min) || !FinitePoint(hb.world_max)) return -1.f;
        const auto origin=Precise(start), delta=Precise(end)-origin;
        const auto pa=Precise(hb.world_min), ba=Precise(hb.world_max)-pa, oa=origin-pa;
        const double len=std::sqrt(Dot(delta,delta)), baba=Dot(ba,ba), r=hb.radius;
        const double baoa=Dot(ba,oa);
        const double closest=baba>1e-16 ? (std::clamp)(baoa/baba,0.,1.) : 0.;
        const auto separation=oa-ba*closest;
        if (Dot(separation,separation)<=r*r) return 0.f;
        if (len<=1e-12) return -1.f;
        const auto rd=delta*(1./len);
        const double bard=Dot(ba,rd), rdoa=Dot(rd,oa), oaoa=Dot(oa,oa);
        double best=std::numeric_limits<double>::infinity();
        const double a=baba-bard*bard;
        if (a>1e-14*baba)
        {
            const double b=baba*rdoa-baoa*bard;
            const double c=baba*oaoa-baoa*baoa-r*r*baba;
            const double disc=b*b-a*c;
            if (disc>=0.)
            {
                const double distance=(-b-std::sqrt(disc))/a;
                const double y=baoa+distance*bard;
                if (distance>=0. && y>=0. && y<=baba) best=distance;
            }
        }
        // A capsule is the union of its finite cylinder and endpoint spheres.
        // This also handles coincident endpoints (a sphere) and axial rays.
        for (int cap=0;cap<2;++cap)
        {
            const auto oc=cap==0 ? oa : oa-ba;
            const double b=Dot(rd,oc), c=Dot(oc,oc)-r*r, disc=b*b-c;
            if (disc<0.) continue;
            const double distance=-b-std::sqrt(disc);
            if (distance>=0.) best=(std::min)(best,distance);
        }
        return best<=len ? static_cast<float>(best/len) : -1.f;
    }

    static bool ToBoxSpace(const float* m,const Vec3& p,HitVec& out)
    {
        for (int i=0;i<12;++i) if (!std::isfinite(m[i])) return false;
        // Use the actual affine inverse, not rotation-transpose. Transpose
        // falsely enlarged/shrank boxes for a scaled bone/model transform.
        const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
        const double determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
        if (!std::isfinite(determinant) || std::abs(determinant)<1e-12) return false;
        const double x=double(p.x)-m[3],y=double(p.y)-m[7],z=double(p.z)-m[11];
        out={((e*i-f*h)*x+(c*h-b*i)*y+(b*f-c*e)*z)/determinant,
             ((f*g-d*i)*x+(a*i-c*g)*y+(c*d-a*f)*z)/determinant,
             ((d*h-e*g)*x+(b*g-a*h)*y+(a*e-b*d)*z)/determinant};
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }
}

float Penetration::HitboxEntryFraction(const Vec3& start,const Vec3& end,const HitboxInfo& hb)
{
    if (!FinitePoint(start) || !FinitePoint(end) || !std::isfinite(hb.radius)) return -1.f;
    if (hb.radius>0.f) return CapsuleEntry(start,end,hb);
    if (!FinitePoint(hb.model_min) || !FinitePoint(hb.model_max)) return -1.f;
    HitVec p0{},p1{};
    if (!ToBoxSpace(hb.matrix,start,p0) || !ToBoxSpace(hb.matrix,end,p1)) return -1.f;
    const auto delta=p1-p0;
    const double origin[3]={p0.x,p0.y,p0.z},direction[3]={delta.x,delta.y,delta.z};
    const float lo[3]={hb.model_min.x,hb.model_min.y,hb.model_min.z};
    const float hi[3]={hb.model_max.x,hb.model_max.y,hb.model_max.z};
    double enter=0.,exit=1.;
    for (int axis=0;axis<3;++axis)
    {
        const double low=(std::min)(lo[axis],hi[axis]), high=(std::max)(lo[axis],hi[axis]);
        if (std::abs(direction[axis])<1e-14)
        { if (origin[axis]<low || origin[axis]>high) return -1.f; continue; }
        double first=(low-origin[axis])/direction[axis],last=(high-origin[axis])/direction[axis];
        if (first>last) std::swap(first,last);
        enter=(std::max)(enter,first);exit=(std::min)(exit,last);
        if (enter>exit) return -1.f;
    }
    return static_cast<float>(enter);
}

bool Penetration::CanHit(const Vec3& start,const Vec3& end,const HitboxInfo& hb)
{
    // Entry and boolean tests must agree for degenerate capsules, tangencies
    // and near-boundary rays; two independent float algorithms did not.
    return HitboxEntryFraction(start,end,hb)>=0.f;
}

// True when a trace that stopped at `hitFrac` along start->end can still be a
// hit on the expected hitbox: it reached (or nearly reached) the hitbox entry.
// The previous rule accepted ANY stop past 95% of the distance, so a crate
// edge / wall corner right in front of the enemy counted as "visible" and
// the bullet went into the cover.
static bool TraceReachesHitbox( const Vec3& start, const Vec3& end,
                                const HitboxInfo& hb, const CGameTrace& tr )
{
    const float entry = Penetration::HitboxEntryFraction( start, end, hb );
    if ( entry < 0.f )
        return false;
    if (tr.startsolid || tr.allsolid || !std::isfinite(tr.fraction) ||
        tr.fraction < 0.f || tr.fraction > 1.f) return false;
    const float len = VecLen(end - start);
    const float hitDist = tr.fraction * len;
    const float entryDist = entry * len;
    if (tr.hitgroup == 0) return hitDist + 0.03125f >= entryDist;
    // Some CSSO traces expose a proxy pointer for a player. Require matching
    // studio index/group AND contact at this exact shape's entry surface.
    return tr.hitgroup == hb.group && tr.hitboxIndex == static_cast<unsigned int>(hb.index) &&
        fabsf(hitDist-entryDist) <= 0.25f;
}

float Penetration::CheckHitchance(
    void* weapon,
    void* localPlayer,
    void* targetEntity,
    const Vec3& eyePos,
    const Vec3& aimAngle,
    const Vec3& aimPoint,
    const WeaponData* wepInfo,
    float hitChanceCfg,
    const HitboxInfo* targetHitboxes,
    int numTargetHitboxes,
    const HitboxInfo* selectedHitbox,
    int selectedHitgroup,
    bool requirePenetration,
    float inaccuracyOverride, float minimumDamage )
{
    if ( !weapon || !wepInfo || !targetHitboxes || numTargetHitboxes <= 0 )
        return 0.f;

    // A wallbang cannot use the clear-line capsule-only approximation. Each
    // angular seed may enter a different material, fail TraceToExit or lose too
    // much damage. Reject the check rather than authorising a blind wall shot.
    if ( requirePenetration && ( !localPlayer || !targetEntity || !selectedHitbox ) )
        return 0.f;

    ResolveRandom();
    if ( !fnRandomSeed || !fnRandomFloat || !fnGetInaccuracy || !fnGetSpread )
        return 0.f; // cannot evaluate — block the shot instead of firing blind

    if ( hitChanceCfg <= 0.f )
        return 100.f;

    // Accuracy is updated once by Ragebot before it evaluates the
    // primary point and the optional center fallback. Updating it here would
    // advance the weapon state twice in the same command when both checks run,
    // producing a moving HC result and a sticky WAIT-ACC loop.
    // A non-negative override replaces the live cone with a hypothetical one
    // (e.g. standing-still inaccuracy), letting callers ask "would this shot
    // pass HC once stopped?" without touching weapon state.
    const float inaccuracy = ( inaccuracyOverride >= 0.f )
        ? inaccuracyOverride
        : GetInaccuracy( weapon );
    const float spread     = GetSpread( weapon );
    if (!std::isfinite(inaccuracy) || inaccuracy < 0.f ||
        !std::isfinite(spread) || spread < 0.f ||
        !std::isfinite(wepInfo->range) || wepInfo->range <= 0.f)
        return 0.f;
    float negevRecoil=3.f;
    if (!ReadNegevRecoil(weapon,negevRecoil)) return 0.f;
    minimumDamage = (std::max)(1.f, minimumDamage);

    // Reference calc_penhc derives angles from target->point.point. Use the
    // exact multipoint selected by rage, not the larger hitbox-center proxy.
    const float pr = aimAngle.x * 3.14159265f / 180.f;
    const float yr = aimAngle.y * 3.14159265f / 180.f;
    const float sp = sinf( pr ), cp = cosf( pr );
    const float sy = sinf( yr ), cy = cosf( yr );

    const Vec3 fwd{ cp * cy, cp * sy, -sp };
    // Source AngleVectors (roll 0): right = (sy, -cy, 0). The old (-sy, cy)
    // was the LEFT vector, i.e. every seed was mirrored horizontally, so HC
    // was computed for a different set of 256 bullet directions than the
    // server actually fires.
    const Vec3 right{ sy, -cy, 0.f };
    const Vec3 up{ sp * cy, sp * sy, cp };

    // CSSO's bullet seed has 256 possible low-byte values. Evaluating only
    // the first 64 biases wallbang HC towards that subset. Use the same full
    // population for visible and penetrating shots, with safe decision exits.
    constexpr int seedCount = 256;
    int hits = 0;

    for ( int i = 0; i < seedCount; ++i )
    {
        const Vec2 vSpread=CalculateSpreadCSSO(i,inaccuracy,spread,negevRecoil);

        Vec3 dir = fwd + right * vSpread.x + up * vSpread.y;
        const float dlen = sqrtf( dir.x * dir.x + dir.y * dir.y + dir.z * dir.z );
        if ( dlen > 0.0001f )
        {
            dir.x /= dlen;
            dir.y /= dlen;
            dir.z /= dlen;
        }

        // Working Supremacy CSSO contract: the seeded spread direction is
        // already expressed in the aim basis. Trace that normalized direction
        // through the selected point; do not reflect the cone a second time.
        // The small extension allows a ray to pass fully through the selected
        // capsule instead of ending at its centre.
        const Vec3 traceEnd = eyePos + dir * wepInfo->range;

        bool seedHit = false;
        if ( requirePenetration )
        {
            // The direct aimpoint was already screened by min-damage. For HC
            // we only count a seed if this exact ray still reaches the selected
            // hitbox through the actual CSSO materials. This is intentionally
            // strict: a possible false negative waits one tick; a false positive
            // fires into a wall and is perceived as a rage miss.
            if (HitboxEntryFraction(eyePos, traceEnd, *selectedHitbox)>=0.f)
            {
                const PenetrationResult seedPen = Calculate(localPlayer, targetEntity,
                    eyePos, traceEnd, wepInfo, selectedHitbox, selectedHitgroup);
                seedHit = (seedPen.visible || seedPen.penetrate) && seedPen.damage + 0.1f >= minimumDamage;
            }
        }
        else
        {
            // For a visible target, only count a hit if the ray actually reaches
            // the *selected* hitbox — the one rage will aim at and the server
            // will validate. Counting any hitbox (legs, arms, …) produced
            // false-positive HC: the spread ray would clip a leg capsule, HC
            // passed, rage fired at the head with those angles, and the server
            // traced the bullet past the head → real miss.
            // selectedHitbox is the winning HitboxInfo from EvalHitboxSource.
            if ( selectedHitbox )
            {
                const float entry = HitboxEntryFraction( eyePos, traceEnd, *selectedHitbox );
                seedHit = entry >= 0.f;
                if (seedHit)
                {
                    const float reach = entry * wepInfo->range;
                    CGameTrace cover{};
                    TraceLine(eyePos, eyePos + dir * reach, kBulletTraceMask, cover, localPlayer);
                    const bool knownTarget = cover.m_pEnt == targetEntity &&
                        cover.hitgroup == selectedHitgroup &&
                        TraceReachesHitbox(eyePos, eyePos + dir * reach, *selectedHitbox, cover);
                    const bool clear = !cover.startsolid && !cover.allsolid &&
                        cover.hitgroup == 0 && cover.fraction * reach + 0.03125f >= reach;
                    // An unknown player 12 units in front is NOT our target.
                    seedHit = !cover.startsolid && !cover.allsolid && (knownTarget || clear);
                }
            }
            else
            {
                // Fallback: no specific hitbox known — scan all (old behaviour).
                for ( int k = 0; k < numTargetHitboxes && !seedHit; ++k )
                    seedHit = CanHit( eyePos, traceEnd, targetHitboxes[ k ] );
            }
        }

        if ( seedHit )
            ++hits;

        if (requirePenetration)
        {
            // Return only when the full-population decision is already proven.
            // Partial successes are a lower bound, never rescaled to samples run.
            const double needed=double(hitChanceCfg)*seedCount/100.;
            if (hits>=needed || hits+(seedCount-i-1)<needed) break;
        }
    }

    const float hc = 100.f * static_cast<float>(hits) / static_cast<float>(seedCount);

    return hc;
}

bool Penetration::SpreadOffset( int seed, float inaccuracy, float spread, Vec2& out, void* weapon )
{
    ResolveRandom();
    if ( !fnRandomSeed || !fnRandomFloat )
        return false;
    float negevRecoil=3.f;
    if (!std::isfinite(inaccuracy) || inaccuracy<0.f || !std::isfinite(spread) || spread<0.f ||
        !ReadNegevRecoil(weapon,negevRecoil)) return false;
    out = CalculateSpreadCSSO(seed,inaccuracy,spread,negevRecoil);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

float Penetration::GetInaccuracy( void* weapon )
{
    ResolveRandom();
    if ( !fnGetInaccuracy || !weapon )
        return 0.f;
    EnginePrediction::ScopedAccuracyState state;
    return fnGetInaccuracy( weapon );
}

float Penetration::GetRestingInaccuracy(void* weapon)
{
    ResolveRandom();
    if (!fnGetInaccuracy || !weapon || !EnginePrediction::Reliable())
        return -1.f;
    const Vec3 stopped{};
    EnginePrediction::ScopedAccuracyState state(&stopped);
    const float inaccuracy = fnGetInaccuracy(weapon);
    return std::isfinite(inaccuracy) && inaccuracy >= 0.f ? inaccuracy : -1.f;
}

float Penetration::GetSpread( void* weapon )
{
    ResolveRandom();
    if ( !fnGetSpread || !weapon )
        return 0.f;
    EnginePrediction::ScopedAccuracyState state;
    return fnGetSpread( weapon );
}

void Penetration::UpdateAccuracy( void* weapon )
{
    ResolveRandom();
    if ( !fnUpdateAccuracy || !weapon )
        return;
    EnginePrediction::ScopedAccuracyState state;
    fnUpdateAccuracy( weapon );
}
