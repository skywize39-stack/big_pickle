#pragma once

#include "sdk/SDK.hpp"
#include "features/combat/aimbot/LagRecord.hpp"

struct PenetrationResult
{
    bool   visible;
    bool   penetrate;
    float  damage;
    int    hitgroup;
    float  distance;
};

namespace Penetration
{
    PenetrationResult Calculate(
        void* localPlayer,
        void* targetEntity,
        const Vec3& eyePos,
        const Vec3& targetPos,
        const SDK::WeaponData* wepInfo,
        const HitboxInfo* expectedHitbox = nullptr,
        int expectedHitgroup = -1 );

    float GetEstimatedDamage(
        void* localPlayer,
        void* targetEntity,
        const Vec3& eyePos,
        const Vec3& targetPos,
        float currentDamage,
        const SDK::WeaponData* wepInfo );

    // Pure segment-vs-capsule/oriented-box geometry, sharing the exact entry
    // calculation below. Supports recorded shapes without entity rewinds.
    bool CanHit( const Vec3& start, const Vec3& end, const HitboxInfo& hb );

    // Fraction (0..1) along start->end where the segment first enters the
    // hitbox (capsule or oriented box), -1 when it misses.
    float HitboxEntryFraction( const Vec3& start, const Vec3& end, const HitboxInfo& hb );

    // Hitchance uses the exact selected rage aimpoint for its ray direction.
    // Clear-line seeds use target hitbox geometry; wallbang seeds additionally
    // run the actual penetration path to the selected point, so an angular
    // offset that is blocked by a different wall thickness cannot be counted
    // as a hit locally and become a server miss.
    float CheckHitchance(
        void* weapon,
        void* localPlayer,
        void* targetEntity,
        const Vec3& eyePos,
        const Vec3& aimAngle,
        const Vec3& aimPoint,
        const SDK::WeaponData* wepInfo,
        float hitChanceCfg,
        const HitboxInfo* targetHitboxes,
        int numTargetHitboxes,
        const HitboxInfo* selectedHitbox,
        int selectedHitgroup,
        bool requirePenetration,
        float inaccuracyOverride = -1.f,
        float minimumDamage = 1.f );

    // Expose the seed-based spread simulation so other features (record-based
    // hit chance) can reuse the same per-seed offset without duplicating the
    // vstdlib RandomSeed/RandomFloat plumbing.
    bool SpreadOffset( int seed, float inaccuracy, float spread, Vec2& out, void* weapon = nullptr );

    // Real weapon functions (C_WeaponCSBaseGun), all take the WEAPON entity:
    //  GetInaccuracy  = client.dll+0x30A950  (penalty + move/air/jump penalties)
    //  GetSpread      = client.dll+0x30B010  (per-subtype spread table)
    //  UpdateAccuracy = client.dll+0x30DD10  (decays the fire penalty over time)
    float GetInaccuracy( void* weapon );
    // Hypothetical native rest cone; only decides whether braking can help.
    // Negative means unavailable. Never use this as the live shot cone.
    float GetRestingInaccuracy(void* weapon);
    float GetSpread( void* weapon );
    void  UpdateAccuracy( void* weapon );
}
