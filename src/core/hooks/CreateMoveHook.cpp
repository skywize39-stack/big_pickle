#include "core/commands/CommandBuffer.hpp"
#include "sdk/build/CssoBuild.hpp"
#include "core/events/EventLog.hpp"
#include "core/hooks/CreateMoveHook.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "features/visuals/chams/Chams.hpp"
#include "features/combat/aimbot/Aimbot.hpp"
#include "features/combat/rage/LagCompensation.hpp"
#include "features/combat/triggerbot/Triggerbot.hpp"
#include "features/networking/fakelag/FakeLag.hpp"
#include "features/networking/fakeduck/FakeDuck.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/DefensiveDT.hpp"
#include "features/networking/doubletap/Exploits.hpp"
#include "features/combat/antiaim/AntiAim.hpp"
#include "features/visuals/camera/ThirdPerson.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "features/movement/autostrafe/AutoStrafe.hpp"
#include "features/misc/killsay/KillSay.hpp"
#include "features/misc/voicekill/VoiceKill.hpp"
#include "features/misc/clantag/ClanTag.hpp"
#include "features/visuals/esp/ESPManager.hpp"
#include "render/surface/SurfaceRenderer.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "core/runtime/FeatureDispatcher.hpp"
#include "features/visuals/smoke/NoSmoke.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include "third_party/MinHook/minhook.h"

#include <Windows.h>
#include <climits>
#include <memory>

// ============================================================
//  CreateMoveHook.cpp
//
//  Hooks CInput::CreateMove (vtable index 3) to build a
//  per-frame entity snapshot consumed by the ESP system.
//
//  Also hooks CInput::KeyButtons (vtable index 2) for bhop.
// ============================================================

using namespace SDK;

// Shared snapshot (written here, read by ProjectFrame and the PaintTraverse
// ESP passes).
std::mutex      g_snapshotMutex;
EntitySnapshot  g_snapshotData;
void*           g_pLocalPlayer = nullptr;

// ----------------------------------------------------------------
//  Native game functions for CRC + verified command sync
// ----------------------------------------------------------------
using KeyTranslationCopy_t = Commands::CopyFn;
using CUserCmdCRC_t        = Commands::CrcFn;

static KeyTranslationCopy_t oKeyTranslation = nullptr;
static CUserCmdCRC_t        oCRC            = nullptr;

// CPrediction::RunCommand, confirmed in this CSSO client.dll at RVA 0x2074E0.
// x64 ABI from the reversed prologue: (prediction, player, usercmd, moveHelper).
using RunCommandFn = void( __fastcall* )( void* prediction, void* player,
                                          void* usercmd, void* moveHelper );
static RunCommandFn oRunCommand = nullptr;

static void __fastcall RunCommandDetour( void* prediction, void* player,
                                         void* usercmd, void* moveHelper )
{
    // Observe the native fire-tick clock before RunCommand increments it.
    EnginePrediction::ObserveRunCommand( player, usercmd );
    // Reference tickbase bookkeeping: record m_nTickBase for the command
    // being predicted (the fire tick, not the incremented value).
    if ( player == g_pLocalPlayer )
        Exploits->RunCommand( reinterpret_cast<SDK::CUserCmd*>( usercmd ) );
    oRunCommand( prediction, player, usercmd, moveHelper );
}

// ----------------------------------------------------------------
//  Skeleton capture — done in the game thread (CreateMove), not the
//  render thread. SetupBones and the bone-matrix reads are only safe
//  here; calling them from EndScene silently crashes/no-ops.
// ----------------------------------------------------------------
using SetupBonesFn = bool( __fastcall* )( void* entity, float* boneMatrix,
                                          int maxBones, int boneMask, float curtime );

// RVA of C_BaseAnimating::SetupBones inside client.dll (from reversal).
static constexpr uintptr_t kSetupBonesRVA = 0x10C510;

// Capture world-space positions for the skeleton bones of one entity.
// Writes into ps.world_bones[] / ps.bone_parent[] / ps.bones_valid. Never throws.
// Screen projection happens on the render thread every frame.
//
// Reversed offsets (verified via BuildTransformations sub_180100110):
//   entity + 0xCC8         -> CStudioHdr*
//   CStudioHdr[0]          -> inner / studiohdr_t*
//   CStudioHdr[9]  (+0x48) -> per-bone flags DWORD* array
//   CStudioHdr[13] (+0x68) -> parent bone index int* array  (KEY: not inline!)
//   *(inner + 0x9C)        = numbones (loop bound in BuildTransformations)
//   SetupBones takes IClientRenderable = entity + 8
//   boneMask 0x100 = BONE_USED_BY_HITBOX
// Read network identity independently of SetupBones success. A failed bone
// build must not turn the same entity into a new/unknown network timeline.
static void CapturePlayerNetworkMetadata(void* entity, PlayerSnapshot& ps)
{
    ps.networkOriginValid = false;
    ps.modelIdentity = 0;
    ps.hitboxSet = -1;
    if (!entity) return;
    __try
    {
        const auto base = reinterpret_cast<uintptr_t>(entity);
        ps.networkOrigin = *reinterpret_cast<const Vec3*>(base + nOriginOffset);
        ps.networkOriginValid = std::isfinite(ps.networkOrigin.x) &&
            std::isfinite(ps.networkOrigin.y) && std::isfinite(ps.networkOrigin.z);
        const auto studio = *reinterpret_cast<const uintptr_t*>(base + 0xCC8);
        if (studio) ps.modelIdentity = *reinterpret_cast<const uintptr_t*>(studio);
        ps.hitboxSet = *reinterpret_cast<const int*>(base + 0x8B8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void CaptureSkeletonBones( void* entity, PlayerSnapshot& ps )
{
    ps.bones_valid = false;
    ps.num_bones   = 0;
    ps.hitboxes_valid = false;
    ps.num_hitboxes = 0;
    if ( !entity || !g_uClientBase )
        return;

    auto SetupBones = reinterpret_cast<SetupBonesFn>( g_uClientBase + kSetupBonesRVA );

    // matrix3x4_t = 12 floats per bone; max 128 bones.
    float boneMatrix[128 * 12] = {};

    // SetupBones takes engine simulation/render time, not platform uptime.
    // Let the native cache/animation pipeline own the pose: no invalidation,
    // simulation-time relabelling, or writes into another entity's bone state.
    float curtime = 0.f;
    if (!LagCompensation::ReadRenderTime(curtime)) return;
    const int   boneMask = 0x100; // BONE_USED_BY_HITBOX

    bool ok = false;
    __try
    {
        auto* renderable = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>( entity ) + 8 );
        ok = SetupBones( renderable, boneMatrix, 128, boneMask, curtime );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        return;
    }
    if ( !ok )
        return;

    auto studioHdr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>( entity ) + 0xCC8 );
    if ( !studioHdr )
        return;

    auto inner = *reinterpret_cast<uintptr_t*>( studioHdr );
    if ( !inner )
        return;

    ps.modelIdentity = inner;

    // numbones from *(inner + 0x9C), confirmed by BuildTransformations loop bound
    const int numbones = *reinterpret_cast<int*>( inner + 0x9C );
    if ( numbones <= 0 || numbones > kMaxBones )
        return;

    // Per-bone "used by" flags: CStudioHdr[9] = *(CStudioHdr + 0x48) -> DWORD[]
    auto flagsArray = *reinterpret_cast<uint32_t**>( studioHdr + 0x48 );

    // Parent bone indices: CStudioHdr[13] = *(CStudioHdr + 0x68) -> int*
    // BuildTransformations reads: *(parentArray + 4 * boneIndex)
    auto parentArray = *reinterpret_cast<int**>( studioHdr + 0x68 );

    for ( int i = 0; i < numbones; ++i )
    {
        const float* mat = &boneMatrix[i * 12];
        const Vec3 world{ mat[3], mat[7], mat[11] };

        ps.world_bones[i]  = world;

        bool isHitbox = flagsArray ? ( flagsArray[i] & 0x100 ) != 0 : false;
        ps.bone_hitbox[i] = isHitbox;

        int8_t parent = -1;
        if ( isHitbox && parentArray )
        {
            int rawParent = parentArray[i];
            if ( rawParent >= 0 && rawParent < numbones )
                parent = static_cast<int8_t>( rawParent );
        }
        ps.bone_parent[i] = parent;
    }

    ps.num_bones   = static_cast<uint8_t>( numbones );
    ps.bones_valid = true;

    // --------------------------------------------------------------
    //  Hitbox capture — same studiohdr + bone matrices.
    //
    //  Reversed from sub_1800FB2E0 / sub_1800F39E0 (hitbox select):
    //    entity + 0x8B8            = hitbox set index
    //    *(inner + 0xB0)           = hitboxset offset from studiohdr
    //    *(inner + 0xB4)           = num hitbox sets
    //    mstudiohitboxset_t (12B): +4 numhitboxes, +8 hitboxarray offset
    //    mstudiobbox_t (0x44):     +0 bone, +4 group, +8 bbmin, +0x14 bbmax,
    //                             +0x20 name, +0x24 angle, +0x30 radius
    //    world center = VectorTransform(boneMatrix[bone], (bbmin+bbmax)*0.5)
    // --------------------------------------------------------------
    ps.hitboxes_valid = false;
    ps.num_hitboxes   = 0;

    __try
    {
        const int hitboxSetIndex = *reinterpret_cast<int*>(
            reinterpret_cast<uintptr_t>( entity ) + 0x8B8 );
        const int numHitboxSets  = *reinterpret_cast<int*>( inner + 0xB4 );
        ps.hitboxSet = hitboxSetIndex;

        if ( hitboxSetIndex >= 0 && hitboxSetIndex < numHitboxSets )
        {
            const int hitboxSetOffset = *reinterpret_cast<int*>( inner + 0xB0 );
            const uintptr_t hitboxSet = inner + hitboxSetOffset
                                        + static_cast<uintptr_t>( hitboxSetIndex ) * 12u;

            const int numHitboxes = *reinterpret_cast<int*>( hitboxSet + 4 );
            const int hitboxArrOffset = *reinterpret_cast<int*>( hitboxSet + 8 );

            const int count = numHitboxes > kMaxHitboxes ? kMaxHitboxes : numHitboxes;
            const uintptr_t hitboxArr = hitboxSet + static_cast<uintptr_t>( hitboxArrOffset );

            int stored = 0;
            for ( int i = 0; i < count; ++i )
            {
                const uintptr_t hb = hitboxArr + static_cast<uintptr_t>( i ) * 0x44u;

                const int bone  = *reinterpret_cast<int*>( hb );
                const int group = *reinterpret_cast<int*>( hb + 4 );
                if ( bone < 0 || bone >= numbones )
                    continue;

                const Vec3 bbmin{ *reinterpret_cast<float*>( hb + 8 ),
                                  *reinterpret_cast<float*>( hb + 12 ),
                                  *reinterpret_cast<float*>( hb + 16 ) };
                const Vec3 bbmax{ *reinterpret_cast<float*>( hb + 20 ),
                                  *reinterpret_cast<float*>( hb + 24 ),
                                  *reinterpret_cast<float*>( hb + 28 ) };

                const float hitboxRadius=*reinterpret_cast<const float*>(hb+0x30);
                if (!std::isfinite(bbmin.x) || !std::isfinite(bbmin.y) || !std::isfinite(bbmin.z) ||
                    !std::isfinite(bbmax.x) || !std::isfinite(bbmax.y) || !std::isfinite(bbmax.z) ||
                    !std::isfinite(hitboxRadius)) continue;

                const float* mat = &boneMatrix[bone * 12];
                bool matrixFinite = true;
                for (int j=0;j<12;++j) matrixFinite = matrixFinite && std::isfinite(mat[j]);
                if (!matrixFinite) continue;
                const float basis = mat[0]*mat[0]+mat[1]*mat[1]+mat[2]*mat[2];
                if (basis < 0.25f || basis > 4.f) continue;

                // Model-space bounds (needed for the Pandora multipoint math).
                const Vec3 center = ( bbmin + bbmax ) * 0.5f;

                Vec3 world;
                world.x = mat[0] * center.x + mat[1] * center.y + mat[2] * center.z + mat[3];
                world.y = mat[4] * center.x + mat[5] * center.y + mat[6] * center.z + mat[7];
                world.z = mat[8] * center.x + mat[9] * center.y + mat[10] * center.z + mat[11];

                // World-space capsule endpoints / box corners (same transform).
                Vec3 wmin;
                wmin.x = mat[0] * bbmin.x + mat[1] * bbmin.y + mat[2] * bbmin.z + mat[3];
                wmin.y = mat[4] * bbmin.x + mat[5] * bbmin.y + mat[6] * bbmin.z + mat[7];
                wmin.z = mat[8] * bbmin.x + mat[9] * bbmin.y + mat[10] * bbmin.z + mat[11];

                Vec3 wmax;
                wmax.x = mat[0] * bbmax.x + mat[1] * bbmax.y + mat[2] * bbmax.z + mat[3];
                wmax.y = mat[4] * bbmax.x + mat[5] * bbmax.y + mat[6] * bbmax.z + mat[7];
                wmax.z = mat[8] * bbmax.x + mat[9] * bbmax.y + mat[10] * bbmax.z + mat[11];

                ps.hitbox_bone[stored]         = static_cast<int8_t>( bone );
                ps.hitbox_group[stored]        = static_cast<int8_t>( group );
                ps.hitbox_studio_index[stored] = static_cast<int8_t>( i );
                ps.hitbox_world_center[stored] = world;
                ps.hitbox_model_min[stored]    = bbmin;
                ps.hitbox_model_max[stored]    = bbmax;
                ps.hitbox_world_min[stored]    = wmin;
                ps.hitbox_world_max[stored]    = wmax;
                memcpy( ps.hitbox_matrix[stored], mat, 12 * sizeof( float ) );
                // m_flRadius at +0x30 (confirmed in sub_1800F39E0: reads
                // *(hitbox + 48), branches on <= 0 for box vs capsule).
                ps.hitbox_radius[stored]       = hitboxRadius;
                ++stored;
            }

            ps.num_hitboxes   = stored;
            ps.hitboxes_valid = stored > 0;
        }
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
    }
}

// Metadata only: never call SetupBones or modify animation/cache state here.
// Sampling after NET_UPDATE_END and before FRAME_RENDER_START interpolation
// prevents rendered movement from being misread as remote packet movement.
static void CaptureNetworkTimelines()
{
    const auto& cfg = g_config.settings.aimbot;
    if (!cfg.enabled || !cfg.rage) { LagCompensation::Reset(); return; }
    LagCompensation::Timing timing;
    if (!g_pEngine || !g_pEntityList || !LagCompensation::ReadTiming(timing))
    { LagCompensation::Reset(); return; }
    const int count = (std::clamp)(g_pEngine->GetMaxClients(), 0, 64);
    const int localIndex = g_pEngine->GetLocalPlayer();
    if (localIndex < 1 || localIndex > count) { LagCompensation::Reset(); return; }
    auto* local = reinterpret_cast<C_BaseEntity*>(g_pEntityList->GetClientEntity(localIndex));
    if (!local) { LagCompensation::Reset(); return; }
    const int team = local->GetTeamNum();
    static PlayerSnapshot sample;
    for (int i=1; i<=64; ++i)
    {
        sample = {};
        if (i <= count && i != localIndex)
        {
            __try
            {
                auto* entity = reinterpret_cast<C_BaseEntity*>(g_pEntityList->GetClientEntity(i));
                sample.entity = entity;
                if (entity)
                {
                    sample.alive = entity->GetHealth() > 0 && entity->GetLifeState() == 0 &&
                        entity->GetTeamNum() != team;
                    sample.dormant = *reinterpret_cast<const bool*>(reinterpret_cast<uintptr_t>(entity)+0x1F6);
                    if (sample.alive && !sample.dormant)
                    {
                        sample.simtime = entity->GetSimulationTime();
                        CapturePlayerNetworkMetadata(entity, sample);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { sample.alive = false; }
        }
        LagCompensation::ObserveNetworkState(i, sample, timing);
    }
}

// ----------------------------------------------------------------
//  CInput::CreateMove detour
//
//  Flow: original fills cmd + verified with original angles →
//  we modify cmd for aimbot → sync verified (server gets aimbot angles) →
//  RESTORE cmd to original angles (local rendering sees originals) →
//  return true so engine processes normally.
// ----------------------------------------------------------------
using CreateMoveFn = bool( __fastcall* )( void* thisptr, int sequence_number,
                                          float input_sample_frametime, bool active );
static CreateMoveFn oCreateMove = nullptr;
// Extra CL_Move calls execute synchronously without a network/animation frame
// between them. Reuse only enemy geometry, NEVER local prediction or shot state.
static EntitySnapshot s_moveSnapshot;
static void* s_snapshotLocal = nullptr;

static bool __fastcall CreateMoveDetour( void* thisptr, int sequence_number,
                                         float input_sample_frametime, bool active )
{
    const bool result = oCreateMove( thisptr, sequence_number, input_sample_frametime, active );

    // PaintTraverse only publishes atomic state; report it here, outside the
    // VGUI render lock and after the original command path has returned.
    SurfaceRenderer::PollStatus();

    if ( !g_pEngine || !g_pEntityList )
        return result;
    if ( !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
    {
        FakeDuck::Reset();
        DoubleTap::Reset();
        AutoPeek::Reset();
        DefensiveDT::Reset();
        PlayerList::ClearSession();
        LagCompensation::Reset();
        s_snapshotLocal = nullptr;
        g_pLocalPlayer = nullptr;
        EnginePrediction::ResetSession();
        return result;
    }

    const bool shifted = DoubleTap::IsShifting();
    // This snapshot is ~358 KiB. Never materialize a per-command value or
    // conditional temporary on the game's thread stack. Construct directly
    // in persistent storage on normal commands; synchronous DT extras reuse
    // enemy geometry and update the local command fields below.
    EntitySnapshot& snap = s_moveSnapshot;
    if (!shifted)
    {
        std::destroy_at(&s_moveSnapshot);
        std::construct_at(&s_moveSnapshot);
    }
    const int localIdx = g_pEngine->GetLocalPlayer();
    snap.localPlayerIndex = localIdx;
    snap.maxClients       = std::clamp(g_pEngine->GetMaxClients(), 0, 64);

    auto* local = localIdx >= 1 && localIdx <= snap.maxClients
        ? reinterpret_cast<C_BaseEntity*>(g_pEntityList->GetClientEntity(localIdx)) : nullptr;
    // InGame can become true before the local entity exists during signon.
    // Do not enter features/prediction with a missing or out-of-roster local.
    if (!local || localIdx < 1 || localIdx > snap.maxClients)
    {
        g_pLocalPlayer = nullptr;
        s_snapshotLocal = nullptr;
        DoubleTap::Reset();
        FakeDuck::Reset();
        AutoPeek::Reset();
        DefensiveDT::Reset();
        EnginePrediction::ResetSession();
        return result;
    }
    if (s_snapshotLocal && local != s_snapshotLocal)
        EnginePrediction::ResetSession();
    const int localTeam = local->GetTeamNum();
    g_pLocalPlayer = local;
    if (shifted && (!local || local != s_snapshotLocal))
    {
        DoubleTap::Reset();
        return result;
    }
    if ( local )
        snap.localOrigin = local->GetAbsOrigin();

    // Preserve the camera yaw before Triggerbot/Rage/AntiAim edit the command.
    // It belongs to the game-thread ESP snapshot, never to the render thread.
    auto** snapshotCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>( thisptr ) + CINPUT_OFFSET_COMMANDS );
    if ( snapshotCommands && *snapshotCommands )
    {
        auto* snapshotCmd = &( *snapshotCommands )[ sequence_number % MULTIPLAYER_BACKUP ];
        if ( snapshotCmd->command_number == sequence_number )
        {
            snap.cmdTickCount = snapshotCmd->tick_count;
            snap.inputButtons = snapshotCmd->buttons;
            snap.cameraYaw = GetYaw( snapshotCmd );
            snap.cameraPitch = GetPitch( snapshotCmd );
            FeatureDispatcher::OnCreateMove( snapshotCmd, local );
        }
    }

    // The donor runs prediction/aimbot on generated commands too. Avoid N
    // SetupBones/record captures, NOT N combat decisions. Local movement,
    // recoil, native spread, weapon readiness and traces stay per-command.
    if (!shifted)
    {
        // --------------------------------------------------------------
        //  Anti-flash: keep the local player's flash state cleared every
        //  tick so the white overlay never builds up.
        // --------------------------------------------------------------
        if ( g_config.settings.misc.no_flash && local )
        {
            *(float*)( (uintptr_t)local + nFlashDurationOffset ) = 0.f;
            *(float*)( (uintptr_t)local + nFlashMaxAlphaOffset  ) = 0.f;
        }

        ThirdPerson::Run();

        for ( int i = 1; i <= snap.maxClients; ++i )
        {
            auto& ps = snap.players[i];
            ps.health    = 0;
            ps.alive     = false;

            if ( i == localIdx )
                continue;

            auto* entity = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( i ) );
            if ( !entity )
                continue;

            ps.entity = reinterpret_cast<void*>( entity );
            ps.health  = entity->GetHealth();
            ps.team    = entity->GetTeamNum();
            ps.dormant = *(bool*)( (uintptr_t)entity + 0x1F6 );
            ps.alive   = ps.health > 0 && *(BYTE*)( (uintptr_t)entity + 0xCF ) == 0;

            if ( ps.alive )
            {
                ps.is_scoped   = entity->IsScoped();
                ps.is_defusing = entity->IsDefusing();
                ps.is_walking  = entity->IsWalking();
                ps.has_defuser = entity->HasDefuser();
                ps.has_helmet  = entity->HasHelmet();
                ps.armor       = entity->GetArmor();
                ps.flags       = entity->GetFlags();
                ps.flash_duration  = entity->GetFlashDuration();
                ps.flash_max_alpha = entity->GetFlashMaxAlpha();

                const char* wName = entity->GetWeaponName();
                if ( wName )
                {
                    // Strip "weapon_" prefix
                    if ( strncmp( wName, "weapon_", 7 ) == 0 )
                        strncpy_s( ps.weapon_name, wName + 7, 31 );
                    else
                        strncpy_s( ps.weapon_name, wName, 31 );
                }

                // ----------------------------------------------------------
                //  BUG FIX: is_planting
                //
                //  The original code used:
                //    entity->IsInBombZone() && entity->GetProgressBarDuration() > 0
                //
                //  GetProgressBarDuration() reads m_iProgressBarDuration which is
                //  only populated for the LOCAL player; on remote players it is
                //  always 0, so the flag never fired.
                //
                //  Combine classname check, m_bStartedArming, and m_iProgressBarDuration.
                //  Checking classname prevents reading garbage data at 0xEC8 from non-C4 weapons.
                // ----------------------------------------------------------
                constexpr int TEAM_T = 2;
                ps.is_planting = false;
                if ( ps.team == TEAM_T && entity->IsInBombZone() )
                {
                    void* pWeapon = entity->GetActiveWeapon();
                    if ( pWeapon )
                    {
                        const char* szClass = (const char*)( (uintptr_t)pWeapon + nWeaponClassnameOffset );
                        if ( szClass && strstr( szClass, "c4" ) )
                        {
                            // Now we are sure it's a C4, we can safely read m_bStartedArming (0xEC8)
                            // Removed nProgress check as it's often 0 for remote players in this engine
                            ps.is_planting = *(bool*)( (uintptr_t)pWeapon + nIsPlantingOffset );
                        }
                    }
                }
            }

            // Skip same-team and dead players from the visible set
            if ( !ps.alive || ps.team == localTeam )
            {
                ps.alive = false;
                // Names are still captured for teammates (health>0) so the
                // playerlist menu shows the full server roster.
                if ( ps.health > 0 )
                {
                    player_info_t info{};
                    if ( g_pEngine->GetPlayerInfo( i, &info ) )
                        strcpy_s( ps.name, info.name );
                }
                continue;
            }

            ps.origin = entity->GetAbsOrigin();

            // Skeleton bones are captured as world positions; the render thread
            // projects them (and the box feet/head) every frame. SetupBones is
            // only safe here on the game thread.
            if ( ps.alive && !ps.dormant )
            {
                ps.simtime = entity->GetSimulationTime();
                LagCompensation::CopyNetworkState(i, reinterpret_cast<uintptr_t>(entity), ps.simtime, ps);
                CaptureSkeletonBones( reinterpret_cast<void*>( entity ), ps );
            }

            player_info_t info{};
            if ( g_pEngine->GetPlayerInfo( i, &info ) )
                strcpy_s( ps.name, info.name );
            else
                ps.name[0] = '\0';
        }

        // Refresh the playerlist session roster (index -> nickname) before any
        // consumer (triggerbot, rage, visuals) evaluates per-player overrides.
        PlayerList::UpdateFromSnapshot( snap );
        s_snapshotLocal = local;
    }


    // ----------------------------------------------------------------
    //  Engine prediction window.
    //
    //  The server executes this command's movement BEFORE the weapon
    //  fires, so every fire-related feature below must evaluate against
    //  the one-tick-ahead state: predicted eye position for the aim
    //  angles, fire-tick curtime for the weapon cooldown gates, and the
    //  post-movement velocity for the inaccuracy cone. Native Update owns
    //  entity history restore/save and movement/weapon simulation. Preview
    //  attacks are suppressed; Finish invalidates the current preview before
    //  verified-command sync, allowing normal final execution on ordinary
    //  AND generated commands.
    // ----------------------------------------------------------------
    // Prepare return/air movement before predicting or selecting a shot.
    // On a NEW rage shot, begin return next command: moving back on the firing
    // command after HC would invalidate the eye used for that very shot.
    {
        auto** ppCommands = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uintptr_t>(thisptr) + CINPUT_OFFSET_COMMANDS);
        if (ppCommands && *ppCommands)
        {
            auto* cmd = &(*ppCommands)[sequence_number % MULTIPLAYER_BACKUP];
            if (cmd->command_number == sequence_number)
            {
                DoubleTap::PrepareCommand(cmd, local);
                if (g_config.settings.aimbot.enabled && g_config.settings.aimbot.rage)
                    AutoPeek::IgnoreAttack(cmd->command_number);
                AutoPeek::Run(cmd, local);
                AutoStrafe::Run(cmd, local);
                FakeDuck::Prepare(cmd, local);
            }
        }
    }

    EnginePrediction::Start( thisptr, sequence_number, local );
    {
        auto** commands = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uintptr_t>(thisptr) + CINPUT_OFFSET_COMMANDS);
        if (commands && *commands)
        {
            auto* cmd = &(*commands)[sequence_number % MULTIPLAYER_BACKUP];
            if (cmd->command_number == sequence_number)
                FakeDuck::AfterPrediction(cmd, local);
        }
    }

    // Network poses/lag records cannot change inside this synchronous burst.
    // Local prediction and every attack gate CAN change and must run anew.
    if (!shifted) Aimbot::UpdateRecords(snap, localTeam);
    if ( DoubleTap::CanRunAttackPipeline() && FakeLag::GameWindowUsable() )
    {
        Triggerbot::Run(thisptr, sequence_number, local, localTeam, snap);
        Aimbot::Run(thisptr, sequence_number, local, localTeam, snap);
    }

    // ----------------------------------------------------------------
    //  Final notification only. Existing return/air movement is already an
    //  input to prediction, and Rage applies/refreshes its stop BEFORE HC.
    //  New auto-peek return begins next command, not after validating this shot.
    // ----------------------------------------------------------------
    {
        auto** ppCommands = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uintptr_t>( thisptr ) + CINPUT_OFFSET_COMMANDS );
        if ( ppCommands && *ppCommands )
        {
            auto* cmd = &( *ppCommands )[ sequence_number % MULTIPLAYER_BACKUP ];
            if ( cmd->command_number == sequence_number )
            {
                // Notify only after rage passed its final eye/damage/HC gates.
                // R8 continues to trigger return from the real clip decrease.
                const bool manualAttack = (cmd->buttons & IN_ATTACK) != 0 &&
                    (snap.inputButtons & IN_ATTACK) != 0;
                if (Aimbot::HasCommittedRageShot(cmd->command_number) || manualAttack)
                {
                    void* weapon = local ? local->GetActiveWeapon() : nullptr;
                    const char* cls = weapon ? reinterpret_cast<const char*>(
                        reinterpret_cast<uintptr_t>(weapon) + nWeaponClassnameOffset) : nullptr;
                    const bool autoR8 = g_config.settings.aimbot.auto_revolver &&
                        cls && strstr(cls, "revolver");
                    if (!autoR8 && FakeLag::GameWindowUsable())
                        AutoPeek::OnShotCommitted(cmd->command_number);
                }
                // Only waiting/release commands may still have a pending brake.
                if ( DoubleTap::CanRunAttackPipeline() && FakeLag::GameWindowUsable() )
                    Aimbot::ApplyAutoStop(cmd, local);
            }
        }
    }

    // Double tap must observe the real rage command before anti-aim replaces
    // its yaw. It records the first shot and arms a command shift if the weapon
    // cycle can fit inside the configured amount.
    {
        auto** ppCommands = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uintptr_t>( thisptr ) + CINPUT_OFFSET_COMMANDS );
        if ( ppCommands && *ppCommands )
        {
            auto* cmd = &( *ppCommands )[ sequence_number % MULTIPLAYER_BACKUP ];
            if ( cmd->command_number == sequence_number )
            {
                DoubleTap::OnCreateMove( cmd, local );
                // Defensive DT: если включён и враг открыл нас — шифтим
                // (запрос будет проигнорирован если обычный DT уже занят burst'ом)
                DefensiveDT::OnCreateMove( cmd, local,
                    snap.localPlayerIndex > 0 ? local->GetTeamNum() : 0,
                    snap.maxClients );
            }
        }
    }

    // Anti-aim runs after the rage movement pipeline so its movement correction
    // preserves the auto-stop / auto-peek direction in the final command.
    if ( FakeLag::GameWindowUsable() )
        AntiAim::Run( thisptr, sequence_number, local );

    // Preserve only the original arming shot. Follow-ups keep their own
    // freshly predicted combat angle and lag-comp tick.
    {
        auto** ppCommands = reinterpret_cast<CUserCmd**>(
            reinterpret_cast<uintptr_t>( thisptr ) + CINPUT_OFFSET_COMMANDS );
        if ( ppCommands && *ppCommands )
        {
            auto* cmd = &( *ppCommands )[ sequence_number % MULTIPLAYER_BACKUP ];
            if ( cmd->command_number == sequence_number )
                DoubleTap::FinalizeCommand( cmd );
        }
    }

    // Reference tickbase reconciliation + server clock sampling run once per
    // executed CreateMove; both are internally guarded no-ops when their
    // slots are not due.
    Exploits->UpdateServerClock( Exploits->GetServerTick( 0 ) );
    Exploits->NetUpdate();

    // Invalidate the attack-free native preview before final command sync.
    // The engine must execute final movement/angles/attack normally.
    EnginePrediction::Finish();

    // ----------------------------------------------------------------
    //  Sync verified command after aimbot modifies angles.
    //  Server receives these modified angles for the shot.
    // ----------------------------------------------------------------
    Commands::CommandBuffer(thisptr).Synchronize(sequence_number, oCRC, oKeyTranslation);

    // ----------------------------------------------------------------
    //  Fake lag: choke or release the outgoing packet
    //  Must run AFTER aimbot so the engine processes the correct angles
    //  on the tick the packet is actually sent.
    // ----------------------------------------------------------------
    FakeLag::Run();

    if (!shifted)
    {
        std::lock_guard<std::mutex> lock( g_snapshotMutex );
        g_snapshotData = snap;
    }

    return result;
}

// ----------------------------------------------------------------
//  CInput::KeyButtons detour — bunny-hop
//
//  The raw oKeyButtons result carries IN_JUMP for whichever key the
//  player bound to +jump (any key, or a wheel impulse). Auto-hop
//  records that raw state, strips the bit and re-applies it only on
//  the ground tick so the re-jump fires exactly on landing. No
//  physical-key polling here on purpose: it would act as a phantom
//  extra bind and would keep hopping while the player types in chat,
//  because GetAsyncKeyState ignores the game's input focus.
// ----------------------------------------------------------------
using KeyButtonsFn = int( __fastcall* )( void* thisptr, int a2 );
static KeyButtonsFn oKeyButtons = nullptr;

static bool s_bhopJumpIntent = false;

bool Hooks::BhopJumpIntent()
{
    return s_bhopJumpIntent;
}

static int __fastcall KeyButtonsDetour( void* thisptr, int a2 )
{
    int buttons = oKeyButtons( thisptr, a2 );

    // Slot 1 is the +jump query. Its unmodified result IS the jump intent:
    // every bound key lands here, and the engine reports nothing while the
    // player is chatting or the game window lacks keyboard focus.
    if ( a2 == 1 )
        s_bhopJumpIntent = ( buttons & IN_JUMP ) != 0;

    if ( a2 == 1 && g_config.settings.misc.bhop &&
         g_pEntityList && g_pEngine &&
         g_pEngine->IsInGame() && g_pEngine->IsConnected() )
    {
        const bool jumpHeld = s_bhopJumpIntent;

        buttons &= ~IN_JUMP;
        if ( jumpHeld )
        {
            const int localIdx = g_pEngine->GetLocalPlayer();
            auto* local = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( localIdx ) );
            if ( local && ( local->GetFlags() & FL_ONGROUND ) )
                buttons |= IN_JUMP;
        }
    }

    return buttons;
}

// ----------------------------------------------------------------
//  IBaseClientDLL::FrameStageNotify detour — mirrors the anti-aim
//  fake angles onto the local model so they are visible in
//  thirdperson. The game computes the local model's animation pose
//  INSIDE oFrameStageNotify at FRAME_RENDER_START (5), feeding the
//  eye angles (m_angEye, 0x85F0) into the animation state. The fake
//  angles therefore have to be written BEFORE that stage — at
//  FRAME_NET_UPDATE_START (1) — and restored after render (6).
// ----------------------------------------------------------------
using FrameStageNotifyFn = void( __fastcall* )( void* thisptr, int stage );
static FrameStageNotifyFn oFrameStageNotify = nullptr;
static void* s_frameStageTarget = nullptr;

static bool  g_aaMirrorApplied = false;
static float g_aaMirrorEye[2] = { 0.f, 0.f };
static float g_aaMirrorRot[2] = { 0.f, 0.f };

static bool AaMirrorActive()
{
    const auto& aa = g_config.settings.misc.antiaim;
    if ( !aa.enabled || !g_pLocalPlayer )
        return false;

    // ThirdPerson::Run() resolves Always/Hold/Toggle once on the game thread.
    // Mirroring must consume that same state; a raw key check would desync the
    // local model when the new toggle mode is used.
    if ( !ThirdPerson::IsActive() )
        return false;

    // Only overwrite the pose while alive; dead players keep their corpse pose
    return *(BYTE*)( (uintptr_t)g_pLocalPlayer + nLifeStateOffset ) == 0;
}

static void AaMirrorApply()
{
    auto* eye = reinterpret_cast<float*>( (uintptr_t)g_pLocalPlayer + nAnEyeOffset );
    auto* rot = reinterpret_cast<float*>( (uintptr_t)g_pLocalPlayer + nAngRotationOffset );

    if ( !g_aaMirrorApplied )
    {
        g_aaMirrorEye[0] = eye[0];
        g_aaMirrorEye[1] = eye[1];
        g_aaMirrorRot[0] = rot[0];
        g_aaMirrorRot[1] = rot[1];
        g_aaMirrorApplied = true;
    }

    eye[0] = AntiAim::lastFakePitch;
    eye[1] = AntiAim::lastFakeYaw;
    rot[0] = AntiAim::lastFakePitch;
    rot[1] = AntiAim::lastFakeYaw;
}

static void AaMirrorRestore()
{
    if ( !g_aaMirrorApplied || !g_pLocalPlayer )
        return;

    auto* eye = reinterpret_cast<float*>( (uintptr_t)g_pLocalPlayer + nAnEyeOffset );
    auto* rot = reinterpret_cast<float*>( (uintptr_t)g_pLocalPlayer + nAngRotationOffset );

    eye[0] = g_aaMirrorEye[0];
    eye[1] = g_aaMirrorEye[1];
    rot[0] = g_aaMirrorRot[0];
    rot[1] = g_aaMirrorRot[1];

    g_aaMirrorApplied = false;
}

static void __fastcall FrameStageNotifyDetour( void* thisptr, int stage )
{
    // Never branch on the detour body before the original is known: a
    // trampoline that was never created (MH_CreateHook failed, or the hook
    // was torn down by Hooks::Shutdown) would be a null call inside the
    // engine's own frame stage.
    if ( !oFrameStageNotify )
        return;

    // Restore the real angles before the game finishes the frame
    if ( stage == FRAME_RENDER_END )
        AaMirrorRestore();

    oFrameStageNotify( thisptr, stage );

    if (stage == FRAME_NET_UPDATE_END)
        CaptureNetworkTimelines();

    // Apply the fake angles as early as possible (and re-apply at the end of
    // the net update) so the pose computed at FRAME_RENDER_START — inside
    // oFrameStageNotify — reads them from the animation state.
    if ( ( stage == FRAME_NET_UPDATE_START || stage == FRAME_NET_UPDATE_END ) && AaMirrorActive() )
        AaMirrorApply();

    // Projection is deliberately kept in the established frame-stage path.
    // PaintTraverse is a narrow liveness callback only; it must not run D3D,
    // ImGui, or World-to-Screen work under the VGUI paint lock.
    if ( stage == FRAME_RENDER_END )
    {
        Visuals::ProjectFrame();
    }
}

// ----------------------------------------------------------------
//  CCSPlayerAnimState::Update detour (client.dll + 0x2A6E20).
//  The game feeds the player's eye yaw/pitch into the animation
//  state through this function (arg order: yaw, pitch); the pose is
//  then built from those values. For the LOCAL player the eye angles
//  are taken from internal animstate fields (not m_angEye, 0x85F0),
//  so the only reliable point to inject the fake angles is here.
// ----------------------------------------------------------------
using AnimStateUpdateFn = void( __fastcall* )( void* animstate, float eyeYaw, float eyePitch, char force );
static AnimStateUpdateFn oAnimStateUpdate = nullptr;

static void __fastcall AnimStateUpdateDetour( void* animstate, float eyeYaw, float eyePitch, char force )
{
    const bool isLocal = g_pLocalPlayer && animstate &&
                         *(void**)( (uintptr_t)animstate + nAnimStateOwnerOffset ) == g_pLocalPlayer;

    if ( isLocal && AaMirrorActive() )
    {
        oAnimStateUpdate( animstate, AntiAim::lastFakeYaw, AntiAim::lastFakePitch, force );
        return;
    }

    oAnimStateUpdate( animstate, eyeYaw, eyePitch, force );
}

// ----------------------------------------------------------------
//  CHudScope::Paint detour (client.dll + 0x2C9C80). When scoped, the
//  HUD draws the black scope overlay (scope_lens fullscreen texture
//  with a circular hole + scope_arc rim + crosshair lines + black
//  corner bars). Returning early removes the overlay entirely while
//  keeping the zoom FOV applied by the view renderer. The crosshair
//  lines are then drawn by our own renderer (see FeatureDispatcher::OnRender) so
//  the crosshair survives without the black frame.
// ----------------------------------------------------------------
using HudScopePaintFn = void( __fastcall* )( void* hudScope );
static HudScopePaintFn oHudScopePaint = nullptr;

static void __fastcall HudScopePaintDetour( void* hudScope )
{
    if ( !g_config.settings.misc.remove_scope_overlay )
        oHudScopePaint( hudScope );
}

// ----------------------------------------------------------------
//  C_BasePlayer::GetFOV detour (client.dll + 0x12AD60). This is the
//  function that computes the FOV used for the 3D view: convar "fov"
//  → weapon FOV (GetWeaponForFOV) → GetDefaultFOV (networked m_iFOV
//  at 0x16A4, which the server sets to the zoom value e.g. 20/10/55
//  while scoped), with zoom smoothing. Overriding its result while
//  scoped lets us change the scope zoom level client-side.
// ----------------------------------------------------------------
using GetFOVFn = float( __fastcall* )( void* player );
static GetFOVFn oGetFOV = nullptr;

static float __fastcall GetFOVDetour( void* player )
{
    const float result = oGetFOV( player );

    const float scopeFov = g_config.settings.misc.scope_fov;
    if ( scopeFov > 0.f && player == g_pLocalPlayer )
    {
        auto* local = reinterpret_cast<SDK::C_BaseEntity*>( player );
        if ( local && local->IsScoped() )
            return scopeFov;
    }

    return result;
}

// ----------------------------------------------------------------
//  Hook lifecycle
// ----------------------------------------------------------------
bool Hooks::Init()
{
    const uintptr_t clientBase = (uintptr_t)GetModuleHandleA( "client.dll" );

    // Init native CRC / key-translation function pointers (verified via IDA reverse)
    oCRC            = reinterpret_cast<CUserCmdCRC_t>( clientBase + SDK::Build::Commands::Crc );
    oKeyTranslation = reinterpret_cast<KeyTranslationCopy_t>( clientBase + SDK::Build::Commands::VerifiedCopy );

    // Hook CInput::CreateMove at 0x1D02B0 and the reversed
    // CPrediction::RunCommand at 0x2074E0 for DT prediction reconciliation.
    void* createMove  = (void*)( clientBase + 0x1D02B0 );
    void* keyButtons  = (void*)( clientBase + 0x1D0C30 );
    void* runCommand  = (void*)( clientBase + 0x2074E0 );

    if ( MH_CreateHook( createMove, &CreateMoveDetour, (void**)&oCreateMove ) != MH_OK )
        return false;
    if ( MH_EnableHook( createMove ) != MH_OK )
        return false;

    if ( MH_CreateHook( keyButtons, &KeyButtonsDetour, (void**)&oKeyButtons ) != MH_OK )
        return false;
    if ( MH_EnableHook( keyButtons ) != MH_OK )
        return false;

    if ( MH_CreateHook( runCommand, &RunCommandDetour, (void**)&oRunCommand ) != MH_OK )
        return false;
    if ( MH_EnableHook( runCommand ) != MH_OK )
        return false;

    // Verified engine+0x90F20 uses slot 35, not the old slot 36. Resolve
    // the same interface instance the engine invokes, not a donor layout.
    s_frameStageTarget = LagCompensation::FrameStageTarget();
    if (s_frameStageTarget)
    {
        if (MH_CreateHook(s_frameStageTarget, &FrameStageNotifyDetour,
                         (void**)&oFrameStageNotify) == MH_OK)
        {
            if (MH_EnableHook(s_frameStageTarget) != MH_OK)
            {
                MH_RemoveHook(s_frameStageTarget);
                oFrameStageNotify = nullptr;
                s_frameStageTarget = nullptr;
            }
        }
        else s_frameStageTarget = nullptr;
    }

    // Hook CCSPlayerAnimState::Update so the local model's pose in
    // thirdperson is driven by the anti-aim fake angles.
    void* animStateUpdate = (void*)( clientBase + 0x2A6E20 );
    if ( MH_CreateHook( animStateUpdate, &AnimStateUpdateDetour, (void**)&oAnimStateUpdate ) == MH_OK )
        MH_EnableHook( animStateUpdate );

    // Hook CHudScope::Paint so the black scope overlay can be removed.
    void* hudScopePaint = (void*)( clientBase + 0x2C9C80 );
    if ( MH_CreateHook( hudScopePaint, &HudScopePaintDetour, (void**)&oHudScopePaint ) == MH_OK )
        MH_EnableHook( hudScopePaint );

    // Hook C_BasePlayer::GetFOV so the scope zoom level can be changed.
    void* getFOV = (void*)( clientBase + 0x12AD60 );
    if ( MH_CreateHook( getFOV, &GetFOVDetour, (void**)&oGetFOV ) == MH_OK )
        MH_EnableHook( getFOV );

    Chams::Init();
    FakeLag::HookClMove();
    DoubleTap::Init();

    // Patch the sv_cheats gate in CInput::CAM_Think so thirdperson
    // survives on online servers (sv_cheats is 0 there, bots use 1).
    ThirdPerson::Init();
    KillSay::Init();
    EventLog::Init();
    VoiceKill::Init();
    NoSmoke::Init();

    // Native VGUI test hook. A failure is non-fatal: all existing hooks stay
    // active and the console explains whether the verified panel was found.
    SurfaceRenderer::Init();
    return true;
}

void Hooks::Shutdown()
{
    // Remove the vgui2 hook before the global MinHook teardown in EntryPoint.
    SurfaceRenderer::Shutdown();
    FakeLag::UnhookClMove();
    DoubleTap::Shutdown();
    ThirdPerson::Shutdown();
    ClanTag::Shutdown();
    VoiceKill::Shutdown();
    NoSmoke::Shutdown();
    KillSay::Shutdown();
    EventLog::Shutdown();
    Chams::Shutdown();

    const uintptr_t clientBase = (uintptr_t)GetModuleHandleA( "client.dll" );

    if (s_frameStageTarget)
    {
        MH_DisableHook(s_frameStageTarget);
        MH_RemoveHook(s_frameStageTarget);
        s_frameStageTarget = nullptr;
        oFrameStageNotify = nullptr;
    }
    LagCompensation::Reset();

    if ( oAnimStateUpdate )
    {
        MH_DisableHook( (void*)( clientBase + 0x2A6E20 ) );
        MH_RemoveHook ( (void*)( clientBase + 0x2A6E20 ) );
        oAnimStateUpdate = nullptr;
    }

    if ( oHudScopePaint )
    {
        MH_DisableHook( (void*)( clientBase + 0x2C9C80 ) );
        MH_RemoveHook ( (void*)( clientBase + 0x2C9C80 ) );
        oHudScopePaint = nullptr;
    }

    if ( oGetFOV )
    {
        MH_DisableHook( (void*)( clientBase + 0x12AD60 ) );
        MH_RemoveHook ( (void*)( clientBase + 0x12AD60 ) );
        oGetFOV = nullptr;
    }

    MH_DisableHook( (void*)( clientBase + 0x1D02B0 ) );
    MH_RemoveHook ( (void*)( clientBase + 0x1D02B0 ) );
    oCreateMove = nullptr;

    MH_DisableHook( (void*)( clientBase + 0x1D0C30 ) );
    MH_RemoveHook ( (void*)( clientBase + 0x1D0C30 ) );
    oKeyButtons = nullptr;

    MH_DisableHook( (void*)( clientBase + 0x2074E0 ) );
    MH_RemoveHook ( (void*)( clientBase + 0x2074E0 ) );
    oRunCommand = nullptr;
}
