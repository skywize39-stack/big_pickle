#include "ui/binds/KeyBinds.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"

#include "config/Config.hpp"
#include "sdk/SDK.hpp"

#include <Windows.h>
#include <cmath>
#include <mutex>

using namespace SDK;

namespace
{
    constexpr float kReturnCompleteDistance = 8.f;
    constexpr float kReturnMinInput = 90.f;
    constexpr float kReturnMaxInput = 450.f;
    constexpr ULONGLONG kReturnTimeoutMs = 1500;   // give up if we can't get back
    constexpr ULONGLONG kStuckCheckMs    = 250;    // progress check window
    constexpr float kStuckMinProgress    = 6.f;    // units per window

    bool s_bindWasActive = false;
    ULONGLONG s_returnStart = 0;
    ULONGLONG s_stuckCheck = 0;
    float s_stuckLastDist = 0.f;

    bool s_hasAnchor = false;
    bool s_returning = false;
    bool s_toggleActive = false;
    bool s_keyWasDown = false;
    Vec3 s_anchor{};
    Vec2 s_markerNdc{};
    bool s_markerVisible = false;
    std::mutex s_anchorMutex;

    void ClearAnchorLocked()
    {
        s_hasAnchor = false;
        s_returning = false;
        s_anchor.Clear();
        s_markerNdc = {};
        s_markerVisible = false;
        s_returnStart = 0;
        s_stuckCheck = 0;
        s_stuckLastDist = 0.f;
    }

    void ClearAnchor()
    {
        std::lock_guard<std::mutex> lock( s_anchorMutex );
        ClearAnchorLocked();
    }

    bool IsFiniteVec3( const Vec3& value )
    {
        return std::isfinite( value.x ) && std::isfinite( value.y ) &&
            std::isfinite( value.z );
    }

    float HorizontalDistance(const Vec3& a, const Vec3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return sqrtf(dx * dx + dy * dy);
    }

    float MaxReturnDistance()
    {
        float d = g_config.settings.misc.auto_peek_max_distance;
        return d < 50.f ? 50.f : ( d > 1000.f ? 1000.f : d );
    }

    // Returns true when the return must be dropped: too far (teleport,
    // respawn, walked off), timed out, or stuck against something.
    bool ShouldAbortReturn( float distance )
    {
        const ULONGLONG now = GetTickCount64();
        if ( distance > MaxReturnDistance() )
            return true;
        if ( s_returnStart == 0 )
        {
            s_returnStart = now;
            s_stuckCheck = now;
            s_stuckLastDist = distance;
            return false;
        }
        if ( now - s_returnStart > kReturnTimeoutMs )
            return true;
        if ( now - s_stuckCheck > kStuckCheckMs )
        {
            const bool stuck = ( s_stuckLastDist - distance ) < kStuckMinProgress;
            s_stuckCheck = now;
            s_stuckLastDist = distance;
            if ( stuck )
                return true;
        }
        return false;
    }

    void ResetReturnTimers()
    {
        s_returnStart = 0;
        s_stuckCheck = 0;
        s_stuckLastDist = 0.f;
    }
}

bool AutoPeek::GetAnchor(Vec3& outAnchor, bool& outReturning)
{
    // CreateMove updates the anchor while the DX9 render hook reads it for the
    // world marker.  Copy one coherent snapshot; never expose a partially
    // written Vec3 to WorldToScreen.
    std::lock_guard<std::mutex> lock( s_anchorMutex );
    if (!s_hasAnchor || !IsFiniteVec3( s_anchor ))
        return false;

    outAnchor = s_anchor;
    outReturning = s_returning;
    return true;
}

bool AutoPeek::IsReturning()
{
    std::lock_guard<std::mutex> lock( s_anchorMutex );
    return s_hasAnchor && s_returning;
}

bool AutoPeek::GetMarkerNdc(Vec2& outNdc, bool& outReturning)
{
    std::lock_guard<std::mutex> lock( s_anchorMutex );
    if ( !s_hasAnchor || !s_markerVisible ||
         !std::isfinite( s_markerNdc.x ) || !std::isfinite( s_markerNdc.y ) )
        return false;

    outNdc = s_markerNdc;
    outReturning = s_returning;
    return true;
}

void AutoPeek::Reset()
{
    ClearAnchor();
    s_toggleActive = false;
    s_keyWasDown = false;
    s_bindWasActive = false;
}

static int s_ignoreAttackCmd = 0;

void AutoPeek::IgnoreAttack(int commandNumber)
{
    s_ignoreAttackCmd = commandNumber;
}

void AutoPeek::OnShotCommitted(int commandNumber)
{
    if (commandNumber <= 0)
        return;

    std::lock_guard<std::mutex> lock(s_anchorMutex);
    if (s_hasAnchor && IsFiniteVec3(s_anchor) && !s_returning)
    {
        s_returning = true;
        s_returnStart = 0;
    }
}

void AutoPeek::ApplyReturnMovement(void* pCmd, void* pLocalPlayer)
{
    if (!pCmd || !pLocalPlayer)
        return;

    auto* cmd = reinterpret_cast<CUserCmd*>(pCmd);
    auto* local = reinterpret_cast<C_BaseEntity*>(pLocalPlayer);
    if (local->GetHealth() <= 0)
        return;

    Vec3 anchor{};
    {
        std::lock_guard<std::mutex> lock(s_anchorMutex);
        if (!s_hasAnchor || !s_returning || !IsFiniteVec3(s_anchor))
            return;
        anchor = s_anchor;
    }

    const Vec3 currentOrigin = local->GetAbsOrigin();
    if (!IsFiniteVec3(currentOrigin))
        return;

    const float distance = HorizontalDistance(anchor, currentOrigin);
    if (ShouldAbortReturn(distance))
    {
        // Too far / timed out / stuck: drop this anchor completely instead
        // of dragging the player across the map.
        ClearAnchor();
        return;
    }
    if (distance <= kReturnCompleteDistance)
    {
        ResetReturnTimers();
        cmd->forwardmove.Encode(0.f);
        cmd->sidemove.Encode(0.f);
        std::lock_guard<std::mutex> lock(s_anchorMutex);
        s_returning = false;
        return;
    }

    const float desiredYaw = atan2f(anchor.y - currentOrigin.y,
        anchor.x - currentOrigin.x) * (180.f / 3.14159265f);
    const float viewYaw = GetYaw(cmd);
    const float relRad = (viewYaw - desiredYaw) * (3.14159265f / 180.f);
    float input = distance * 5.f;
    if (input < kReturnMinInput)
        input = kReturnMinInput;
    else if (input > kReturnMaxInput)
        input = kReturnMaxInput;

    cmd->forwardmove.Encode(cosf(relRad) * input);
    cmd->sidemove.Encode(sinf(relRad) * input);
}

void AutoPeek::Run(void* pCmd, void* pLocalPlayer)
{
    const auto& misc = g_config.settings.misc;
    if (!misc.auto_peek || !pCmd || !pLocalPlayer)
    {
        Reset();
        return;
    }

    const bool active = Binds::Active( "auto_peek", misc.auto_peek_key, misc.auto_peek_mode, "Auto peek" );

    // Bind off (hold released / toggled off) -> drop the anchor immediately,
    // even mid-return: the player takes control back right away.
    // Bind on again -> a NEW anchor is placed at the current position, the
    // old spot is never reused.
    const bool justActivated = active && !s_bindWasActive;
    s_bindWasActive = active;
    if (!active)
    {
        ClearAnchor();
        return;
    }
    if (justActivated)
        ClearAnchor();

    auto* cmd = reinterpret_cast<CUserCmd*>(pCmd);
    auto* local = reinterpret_cast<C_BaseEntity*>(pLocalPlayer);
    if (local->GetHealth() <= 0)
    {
        Reset();
        return;
    }
    if (!(local->GetFlags() & FL_ONGROUND))
        return;

    const Vec3 currentOrigin = local->GetAbsOrigin();
    if (!IsFiniteVec3( currentOrigin ))
    {
        ClearAnchor();
        return;
    }

    Vec3 anchor{};
    bool returning = false;
    {
        std::lock_guard<std::mutex> lock( s_anchorMutex );
        if (!s_hasAnchor)
        {
            s_anchor = currentOrigin;
            s_hasAnchor = true;
            s_returning = false;
        }

        // AutoPeek begins the return only after a shot has actually been
        // placed in the final command. This runs after rage in CreateMove.
        if (!s_returning && (cmd->buttons & IN_ATTACK) &&
            cmd->command_number != s_ignoreAttackCmd)
            s_returning = true;

        anchor = s_anchor;
        returning = s_returning;
    }

    // g_InternalW2S is used elsewhere exclusively from CreateMove to populate
    // ESP snapshots. Capture the auto-peek anchor by that proven game-thread
    // path, then let the render thread only consume the copied NDC result.
    Vec3 projected{};
    const bool markerVisible = g_InternalW2S &&
        !g_InternalW2S( nullptr, anchor, projected ) &&
        std::isfinite( projected.x ) && std::isfinite( projected.y );
    {
        std::lock_guard<std::mutex> lock( s_anchorMutex );
        s_markerVisible = markerVisible;
        if ( markerVisible )
            s_markerNdc = { projected.x, projected.y };
    }

    if (!returning || !IsFiniteVec3( anchor ))
        return;

    const float distance = HorizontalDistance(anchor, currentOrigin);
    if (ShouldAbortReturn(distance))
    {
        // Too far / timed out / stuck: drop this anchor completely instead
        // of dragging the player across the map.
        ClearAnchor();
        return;
    }
    if (distance <= kReturnCompleteDistance)
    {
        ResetReturnTimers();
        cmd->forwardmove.Encode(0.f);
        cmd->sidemove.Encode(0.f);
        {
            std::lock_guard<std::mutex> lock( s_anchorMutex );
            s_returning = false;
        }
        return;
    }

    const float desiredYaw = atan2f(anchor.y - currentOrigin.y,
        anchor.x - currentOrigin.x) * (180.f / 3.14159265f);
    const float viewYaw = GetYaw(cmd);
    const float relRad = (viewYaw - desiredYaw) * (3.14159265f / 180.f);

    float input = distance * 5.f;
    if (input < kReturnMinInput)
        input = kReturnMinInput;
    else if (input > kReturnMaxInput)
        input = kReturnMaxInput;

    cmd->forwardmove.Encode(cosf(relRad) * input);
    cmd->sidemove.Encode(sinf(relRad) * input);
}
