#pragma once

// ============================================================
//  GameTypes.hpp — per-frame entity snapshot passed from
//  CreateMoveHook to the ESP rendering system.
// ============================================================

#include "utils/Math.hpp"
#include <mutex>

// ------------------------------------------------------------------
//  PlayerSnapshot — data for a single enemy captured each game tick.
//
//  The snapshot stores WORLD-space data only. Screen projection happens on
//  the render thread every frame (SDK::WorldToScreen via the live engine
//  matrix), so ESP keeps tracking the camera even while CreateMove is not
//  running (DT recharge skips whole CL_Move calls).
// ------------------------------------------------------------------
// Maximum bones we support iterating from CStudioHdr.
inline constexpr int kMaxBones = 128;

// Maximum hitboxes we store per player (CS:S player models use ~18).
inline constexpr int kMaxHitboxes = 32;

struct PlayerSnapshot
{
    void* entity        = nullptr;  // client entity pointer (index-based)
    Vec3  origin{};
    Vec3 networkOrigin{}; // un-interpolated m_vecOrigin, for timeline continuity only
    bool networkOriginValid = false;
    int   health       = 0;
    int   team         = 0;
    bool  dormant      = false;
    bool  alive        = false;
    float simtime      = 0.f;   // m_flSimulationTime (server state timestamp)
    uintptr_t modelIdentity = 0; // CStudioHdr + selected hitbox set checked at capture
    int hitboxSet = -1;

    // Skeleton: per-bone data computed in game thread (CreateMove).
    // Arrays are indexed by raw bone index (0..num_bones-1).
    bool  bones_valid         = false;
    uint8_t num_bones         = 0;
    Vec3  world_bones[kMaxBones]{};
    int8_t bone_parent[kMaxBones]{}; // -1 = root; >= 0 = parent bone index
    bool  bone_hitbox[kMaxBones]{};  // true if BONE_USED_BY_HITBOX

    // Hitboxes: filled in the game thread from the studio hitbox set +
    // SetupBones bone matrices. Index 0..num_hitboxes-1.
    bool    hitboxes_valid         = false;
    int     num_hitboxes           = 0;
    int8_t  hitbox_bone[kMaxHitboxes]{};     // bone index into world_bones[] / bone matrices
    int8_t  hitbox_group[kMaxHitboxes]{};    // HITGROUP_* (1=head, 2=chest, 3=stomach, ...)
    int8_t  hitbox_studio_index[kMaxHitboxes]{}; // studio hitbox index (0=head, 11=right foot, 12=left foot)
    Vec3    hitbox_world_center[kMaxHitboxes]{};
    Vec3    hitbox_model_min[kMaxHitboxes]{};    // mstudiobbox m_mins (model space)
    Vec3    hitbox_model_max[kMaxHitboxes]{};    // mstudiobbox m_maxs (model space)
    Vec3    hitbox_world_min[kMaxHitboxes]{};    // capsule endpoint A / box corner A (world space)
    Vec3    hitbox_world_max[kMaxHitboxes]{};    // capsule endpoint B / box corner B (world space)
    float   hitbox_matrix[kMaxHitboxes][12]{};   // 3x4 bone matrix of the hitbox bone
    float   hitbox_radius[kMaxHitboxes]{};   // mstudiobbox.m_flRadius (+0x40); -1 for box hitboxes

    // m_fFlags of the player (FL_ONGROUND etc.) at capture time
    int     flags = 0;

    // Flags
    bool  is_scoped    = false;
    bool  is_defusing  = false;
    bool  is_walking   = false;
    bool  is_planting  = false;   // T-side player standing in bomb zone
    bool  has_defuser  = false;
    bool  has_helmet   = false;

    float flash_duration  = 0.f;
    float flash_max_alpha = 0.f;
    int   armor           = 0;
    char  name[32]{};
    char  weapon_name[32]{};
};

// ------------------------------------------------------------------
//  EntitySnapshot — full world state for one rendered frame
// ------------------------------------------------------------------
struct EntitySnapshot
{
    PlayerSnapshot players[65]; // 1-indexed; index 0 unused
    int localPlayerIndex = 0;
    int maxClients       = 0;

    // Captured before aim/anti-aim changes the current command. ProjectFrame
    // uses this camera-space reference to form off-screen arrow directions
    // without querying entities or a view matrix on the render thread.
    Vec3  localOrigin{};
    float cameraYaw = 0.f;
    float cameraPitch = 0.f;
    // Original command time for rendering/logging. It is NOT the simulation
    // timestamp of each enemy. Render-pose history must keep this exact tick.
    int   cmdTickCount = 0;
    int   inputButtons = 0; // original engine input, before trigger/rage edits
};

// ------------------------------------------------------------------
//  Shared snapshot globals (written by CreateMoveHook, read by ESP)
// ------------------------------------------------------------------
extern std::mutex      g_snapshotMutex;
extern EntitySnapshot  g_snapshotData;
extern void*           g_pLocalPlayer;
