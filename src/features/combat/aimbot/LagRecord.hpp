#pragma once

// Per-player immutable render-pose history. Simulation time identifies the
// network update only; it does NOT certify a reconstructed server pose.
// Preserve the capture command tick and interpolation together with its bones.

#include "sdk/SDK.hpp"
#include "sdk/GameTypes.hpp"
#include "features/combat/rage/LagCompensation.hpp"

#include <deque>
#include <cstring>

// A 1s default server history at 64 tick, with a hard memory bound.
// The .2s delta tolerance is not the size of the server's history.
inline constexpr int kMaxLagRecords = 64;

// Studio hitbox indices (Pandora Hitboxes_t — matches the game's hitbox set
// order: head 0, neck 1, pelvis 2, body 3, thorax 4, chest 5, ...).
enum HitboxIndex : int
{
    HITBOX_HEAD = 0,
    HITBOX_NECK,
    HITBOX_PELVIS,
    HITBOX_BODY,
    HITBOX_THORAX,
    HITBOX_CHEST,
    HITBOX_UPPER_CHEST,
    HITBOX_R_THIGH,
    HITBOX_L_THIGH,
    HITBOX_R_CALF,
    HITBOX_L_CALF,
    HITBOX_R_FOOT,
    HITBOX_L_FOOT,
    HITBOX_R_HAND,
    HITBOX_L_HAND,
    HITBOX_R_UPPER_ARM,
    HITBOX_R_FOREARM,
    HITBOX_L_UPPER_ARM,
    HITBOX_L_FOREARM,
    HITBOX_MAX
};

// Everything the aimbot needs to reason about one hitbox of a record or the
// live snapshot: model-space bounds, world-space capsule endpoints and the
// bone matrix for transforming multipoint points.
struct HitboxInfo
{
    int   index      = -1;        // HitboxIndex (studio index)
    int   group      = 0;         // HITGROUP_*
    Vec3  center{};
    Vec3  model_min{};
    Vec3  model_max{};
    Vec3  world_min{};            // capsule endpoint A / box corner A
    Vec3  world_max{};            // capsule endpoint B / box corner B
    float radius     = -1.f;      // capsule radius; <= 0 = box hitbox
    float matrix[12]{};           // 3x4 bone matrix of the hitbox bone
};

struct LagRecord
{
    float simtime       = 0.f;
    int   cmd_tick      = 0; // original command tick, never re-derived from simtime
    float capture_lerp  = 0.f;
    Vec3 origin{};
    uintptr_t modelIdentity = 0;
    int hitboxSet = -1;
    int   flags         = 0;      // m_fFlags at capture time (FL_ONGROUND)
    int   num_hitboxes  = 0;
    int8_t hitbox_group[kMaxHitboxes]{};
    int8_t hitbox_studio_index[kMaxHitboxes]{};
    Vec3   hitbox_world_center[kMaxHitboxes]{};
    Vec3   hitbox_model_min[kMaxHitboxes]{};
    Vec3   hitbox_model_max[kMaxHitboxes]{};
    Vec3   hitbox_world_min[kMaxHitboxes]{};
    Vec3   hitbox_world_max[kMaxHitboxes]{};
    float  hitbox_matrix[kMaxHitboxes][12]{};
    float  hitbox_radius[kMaxHitboxes]{};
    bool   valid        = true;
};

// Convenience accessor — pulls one hitbox of a record into a HitboxInfo.
inline HitboxInfo RecordHitbox( const LagRecord& rec, int hb )
{
    HitboxInfo info;
    info.index      = rec.hitbox_studio_index[hb];
    info.group      = rec.hitbox_group[hb];
    info.center     = rec.hitbox_world_center[hb];
    info.model_min  = rec.hitbox_model_min[hb];
    info.model_max  = rec.hitbox_model_max[hb];
    info.world_min  = rec.hitbox_world_min[hb];
    info.world_max  = rec.hitbox_world_max[hb];
    info.radius     = rec.hitbox_radius[hb];
    memcpy( info.matrix, rec.hitbox_matrix[hb], sizeof( info.matrix ) );
    return info;
}

// Same accessor for the live snapshot.
inline HitboxInfo SnapshotHitbox( const PlayerSnapshot& ps, int hb )
{
    HitboxInfo info;
    info.index      = ps.hitbox_studio_index[hb];
    info.group      = ps.hitbox_group[hb];
    info.center     = ps.hitbox_world_center[hb];
    info.model_min  = ps.hitbox_model_min[hb];
    info.model_max  = ps.hitbox_model_max[hb];
    info.world_min  = ps.hitbox_world_min[hb];
    info.world_max  = ps.hitbox_world_max[hb];
    info.radius     = ps.hitbox_radius[hb];
    memcpy( info.matrix, ps.hitbox_matrix[hb], sizeof( info.matrix ) );
    return info;
}

struct PlayerRecordStore
{
    void* entity = nullptr;
    LagCompensation::Timeline timeline{};
    std::deque<LagRecord> records;
};

// Per-player history. Indexed by entity index like EntitySnapshot (0 unused).
inline PlayerRecordStore g_playerRecords[65];

// Fallback radius (units) used for hit-chance ray-vs-hitbox tests when a
// hitbox is a box (mstudiobbox radius <= 0). Approximates CS:S box half-sizes.
inline float RecordHitboxRadius( int group, float storedRadius )
{
    if ( storedRadius > 0.5f )
        return storedRadius;
    switch ( group )
    {
    case SDK::HITGROUP_HEAD:    return 5.f;
    case SDK::HITGROUP_CHEST:   return 14.f;
    case SDK::HITGROUP_STOMACH: return 12.f;
    case SDK::HITGROUP_LEFTARM:
    case SDK::HITGROUP_RIGHTARM:return 6.f;
    default:                    return 7.f; // legs / pelvis / etc
    }
}
