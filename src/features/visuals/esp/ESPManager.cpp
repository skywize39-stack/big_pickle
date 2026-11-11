#include "features/visuals/esp/ESPManager.hpp"
#include "sdk/GameTypes.hpp"
#include "sdk/SDK.hpp"
#include "core/hooks/D3DHook.hpp"
#include "config/Config.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "features/visuals/camera/ThirdPerson.hpp"
#include "features/combat/players/PlayerList.hpp"
#include <mutex>
#include <cstring>
#include <cmath>

// ============================================================
//  ESPManager.cpp — game-thread projection cache
//
//  Threading contract:
//    - ProjectFrame: GAME thread (FrameStageNotify, FRAME_RENDER_END).
//      Converts the world snapshot to pixel coordinates with the engine
//      W2S while the view matrix is coherent. Runs every rendered frame —
//      including frames whose CreateMove was skipped by the DT recharge.
//
//  On this build visible ESP (boxes / skeletons / arrows) renders directly
//  through SurfaceRenderer (VGuiSurface033) from live entity data, so the
//  old ImGui Update/Draw cache and the g_players fading cache are gone.
// ============================================================

namespace
{
    // ------------------------------------------------------------------
    //  Game-thread projection output (written by ProjectFrame). Screen
    //  coordinates in pixels. On this build the visible ESP renders fresh
    //  each PaintTraverse pass (boxes / skeletons / arrows read live entity
    //  data), so this cache is only the historical off-screen-arrow source.
    // ------------------------------------------------------------------
    struct PlayerScreen
    {
        bool     valid = false;
        Vec2     feet{};
        Vec2     head{};

        // Direction in camera-space screen coordinates for players rejected by
        // coherent W2S (behind camera or outside the viewport).
        bool     offscreen = false;
        Vec2     offscreenDirection{};
        bool     bonesValid = false;
        uint8_t  numBones = 0;
        bool     boneVisible[kMaxBones]{};
        Vec2     bonesScreen[kMaxBones]{};
        int8_t   boneParent[kMaxBones]{};
        bool     boneHitbox[kMaxBones]{};
    };

    std::mutex   g_screenMutex;
    PlayerScreen g_screen[65];
    bool         g_anchorValid     = false;
    bool         g_anchorReturning = false;
    Vec2         g_anchorScreen{};
}

void Visuals::ProjectFrame()
{
    using namespace SDK;

    // At most one projection pass per 0.5 ms — FrameStageNotify fires for
    // many stages per frame and repeats would only waste the snapshot copy.
    static INT64 s_lastQpc = 0;
    static INT64 s_freq    = 0;
    if ( s_freq == 0 )
        ::QueryPerformanceFrequency( reinterpret_cast<LARGE_INTEGER*>( &s_freq ) );
    INT64 now = 0;
    ::QueryPerformanceCounter( reinterpret_cast<LARGE_INTEGER*>( &now ) );

    if ( s_lastQpc != 0 && ( now - s_lastQpc ) * 2000 < s_freq )
        return;
    s_lastQpc = now;

    if ( !g_pEngine || !g_InternalW2S )
        return;
    if ( !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
        return;

    EntitySnapshot snap;
    {
        std::lock_guard<std::mutex> lock( g_snapshotMutex );
        snap = g_snapshotData;
    }

    // ProjectFrame must continue populating the VGUI snapshot even when the
    // EndScene device path is unavailable. Prefer its exact viewport, then
    // fall back to the game window client size without invoking any renderer.
    UINT screenWidth = 0;
    UINT screenHeight = 0;
    D3DVIEWPORT9 vp{};
    if ( g_pD3Device && SUCCEEDED( g_pD3Device->GetViewport( &vp ) ) && vp.Width && vp.Height )
    {
        screenWidth = vp.Width;
        screenHeight = vp.Height;
    }
    else
    {
        RECT client{};
        if ( !g_hGameWnd || !::GetClientRect( g_hGameWnd, &client ) ||
             client.right <= client.left || client.bottom <= client.top )
            return;
        screenWidth = static_cast<UINT>( client.right - client.left );
        screenHeight = static_cast<UINT>( client.bottom - client.top );
    }

    auto W2S = [ screenWidth, screenHeight ]( const Vec3& world, Vec2& out ) -> bool
    {
        Vec3 ndc;
        if ( g_InternalW2S( nullptr, world, ndc ) )
            return false;
        if ( !std::isfinite( ndc.x ) || !std::isfinite( ndc.y ) )
            return false;
        out.x = ( 1.0f + ndc.x ) * static_cast<float>( screenWidth ) * 0.5f;
        out.y = ( 1.0f - ndc.y ) * static_cast<float>( screenHeight ) * 0.5f;
        return std::isfinite( out.x ) && std::isfinite( out.y );
    };

    // Same projection, but it also hands back the raw NDC so callers can tell
    // "in front of the camera" from "behind it". The magnitude is checked as
    // well as the return code: near-plane points can be reported as a success
    // with coordinates scaled far outside the viewport.
    auto W2SNdc = [ screenWidth, screenHeight ]( const Vec3& world, Vec2& out,
                                                 Vec3& ndc ) -> bool
    {
        if ( g_InternalW2S( nullptr, world, ndc ) )
            return false;
        if ( !std::isfinite( ndc.x ) || !std::isfinite( ndc.y ) )
            return false;
        out.x = ( 1.0f + ndc.x ) * static_cast<float>( screenWidth ) * 0.5f;
        out.y = ( 1.0f - ndc.y ) * static_cast<float>( screenHeight ) * 0.5f;
        return std::isfinite( out.x ) && std::isfinite( out.y );
    };

    // NDC radius within which a projection is still drawn as a (possibly
    // edge-clipped) box. Normal and near-edge players stay well inside it;
    // the engine's behind-camera coordinates are orders of magnitude outside.
    constexpr float kBoxNdcLimit = 2.0f;

    PlayerScreen screen[65]{};
    const float yawRad   = snap.cameraYaw * ( 3.14159265f / 180.f );
    const float pitchRad = snap.cameraPitch * ( 3.14159265f / 180.f );
    // Source's AngleVectors basis for zero roll, with pitch:
    //   forward = ( cos(yaw)*cos(pitch), sin(yaw)*cos(pitch), -sin(pitch) )
    //   right   = ( sin(yaw),           -cos(yaw),            0           )
    //   up      = ( sin(pitch)*cos(yaw), sin(pitch)*sin(yaw),  cos(pitch) )
    // The z-component of `up` carries the vertical (dz) term so the bearing is
    // correct when the camera pitches or the target is above / below the view
    // direction. right/up here are the un-mirrored basis.
    const float cosYaw   = cosf( yawRad );
    const float sinYaw   = sinf( yawRad );
    const float cosPitch = cosf( pitchRad );
    const float sinPitch = sinf( pitchRad );
    const float cameraRightX =  sinYaw;
    const float cameraRightY = -cosYaw;
    const float cameraUpX    = sinPitch * cosYaw;
    const float cameraUpY    = sinPitch * sinYaw;
    const float cameraUpZ    = cosPitch;
    const bool showLocal = g_config.settings.visuals.esp.local && ThirdPerson::IsActive();

    for ( int i = 1; i <= snap.maxClients && i < 65; ++i )
    {
        auto& scr = screen[i];
        scr.valid      = false;
        scr.offscreen  = false;
        scr.bonesValid = false;
        scr.numBones   = 0;

        if ( i == snap.localPlayerIndex && !showLocal )
            continue;

        const auto& ps = snap.players[i];
        if ( !ps.alive )
            continue;

        // PlayerList: a hidden player must not be projected at all, otherwise
        // the offscreen-arrow pass still draws a direction marker for them.
        // The PaintTraverse passes (boxes / skeletons / arrows) filter hidden
        // players separately.
        if ( PlayerList::GetVisualOverride( i ).hide )
            continue;

        const Vec3 headWorld{ ps.origin.x, ps.origin.y, ps.origin.z + 72.f };

        Vec3 feetNdc{}, headNdc{};
        const bool gotFeet = W2SNdc( ps.origin, scr.feet, feetNdc );
        const bool gotHead = W2SNdc( headWorld, scr.head, headNdc );

        // Draw a box while the projection stays near the viewport — the wide
        // window keeps the familiar edge-clipped box. Anything outside it,
        // above all the scaled-up behind-camera coordinates, becomes an
        // off-screen marker instead of a garbage full-screen box.
        const bool drawable =
            gotFeet && gotHead &&
            fabsf( feetNdc.x ) <= kBoxNdcLimit && fabsf( feetNdc.y ) <= kBoxNdcLimit &&
            fabsf( headNdc.x ) <= kBoxNdcLimit && fabsf( headNdc.y ) <= kBoxNdcLimit;

        if ( !drawable )
        {
            // Derive a stable arrow direction from the 3D camera basis.
            // Project the world-space direction onto the camera's right and up
            // vectors (the screen plane), then normalize. This correctly
            // handles pitch so arrows point up when the target is above the
            // camera's view direction.
            const float dx = ps.origin.x - snap.localOrigin.x;
            const float dy = ps.origin.y - snap.localOrigin.y;
            const float dz = ps.origin.z - snap.localOrigin.z;
            const float right = dx * cameraRightX + dy * cameraRightY;
            const float up    = dx * cameraUpX + dy * cameraUpY + dz * cameraUpZ;
            const float length = sqrtf( right * right + up * up );
            if ( std::isfinite( length ) && length > 1.f )
            {
                // Screen-space Y grows downward (positive ndc.y is "up"), so
                // the up axis is negated for the stored screen direction.
                scr.offscreenDirection.x =  right / length;
                scr.offscreenDirection.y = -up / length;
                scr.offscreen = std::isfinite( scr.offscreenDirection.x ) &&
                                std::isfinite( scr.offscreenDirection.y );
            }
            continue;
        }
        scr.valid = true;

        scr.bonesValid = ps.bones_valid;
        scr.numBones   = ps.num_bones;
        if ( ps.bones_valid )
        {
            for ( int b = 0; b < ps.num_bones; ++b )
            {
                scr.boneVisible[b] = W2S( ps.world_bones[b], scr.bonesScreen[b] );
                scr.boneParent[b]  = ps.bone_parent[b];
                scr.boneHitbox[b]  = ps.bone_hitbox[b];
            }
        }
    }

    // The AutoPeek world anchor marker joins the same coherent pass.
    bool anchorValid     = false;
    bool anchorReturning = false;
    Vec2 anchorScreen{};
    if ( g_config.settings.misc.auto_peek )
    {
        Vec3 anchor{};
        if ( AutoPeek::GetAnchor( anchor, anchorReturning ) )
            anchorValid = W2S( anchor, anchorScreen );
    }

    {
        std::lock_guard<std::mutex> lock( g_screenMutex );
        memcpy( g_screen, screen, sizeof( screen ) );
        g_anchorValid     = anchorValid;
        g_anchorReturning = anchorReturning;
        g_anchorScreen    = anchorScreen;
    }
}

bool Visuals::GetAnchorScreen( Vec2& outPos, bool& outReturning )
{
    std::lock_guard<std::mutex> lock( g_screenMutex );
    if ( !g_anchorValid )
        return false;
    outPos       = g_anchorScreen;
    outReturning = g_anchorReturning;
    return true;
}

bool Visuals::GetOffscreenArrowDirections( Vec2 outDirections[65] )
{
    if ( !outDirections )
        return false;
    std::lock_guard<std::mutex> lock( g_screenMutex );
    bool any = false;
    for ( int i = 0; i < 65; ++i )
    {
        outDirections[i] = {};
        if ( g_screen[i].offscreen )
        {
            outDirections[i] = g_screen[i].offscreenDirection;
            any = true;
        }
    }
    return any;
}
