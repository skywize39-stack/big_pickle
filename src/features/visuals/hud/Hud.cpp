#include "features/visuals/hud/Hud.hpp"
#include "sdk/Entities.hpp"
#include "sdk/Trace.hpp"
#include "sdk/WorldEntity.hpp"
#include "sdk/Projection.hpp"
#include "config/Config.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/DefensiveDT.hpp"
#include "features/networking/fakelag/FakeLag.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "features/combat/aimbot/Aimbot.hpp"
#include "features/combat/antiaim/AntiAim.hpp"
#include "core/events/EventLog.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>
#include <imgui.h>

namespace
{
static void DrawDoubleTapIndicator()
{
    const auto& dt = g_config.settings.misc.doubletap;
    if ( !dt.enabled || !dt.indicator || !g_pLocalPlayer ||
         !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    const auto state = DoubleTap::GetIndicatorState();
    ImU32 color = IM_COL32( 225, 75, 75, 235 );
    if ( state == DoubleTap::IndicatorState::Ready )
        color = IM_COL32( 95, 220, 125, 235 );
    else if ( state == DoubleTap::IndicatorState::Charging )
        color = IM_COL32( 130, 165, 230, 235 );
    else if ( state == DoubleTap::IndicatorState::Armed )
        color = IM_COL32( 245, 205, 70, 235 );
    else if ( state == DoubleTap::IndicatorState::Shifting )
        color = IM_COL32( 90, 175, 255, 235 );
    else if ( state == DoubleTap::IndicatorState::Teleport )
        color = IM_COL32( 195, 105, 255, 235 );
    else if ( state == DoubleTap::IndicatorState::BindInactive ||
              state == DoubleTap::IndicatorState::WaitingForFire )
        color = IM_COL32( 170, 170, 170, 215 );

    const char* text = DoubleTap::GetIndicatorText();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 textSize = ImGui::CalcTextSize( text );
    // Left HUD stack: always visible without covering crosshair, scope or
    // target model. This intentionally does not follow the screen center.
    const ImVec2 pos( 18.f, display.y * 0.5f + 28.f );

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 10, 10, 14, 170 ), 3.f );
    dl->AddRect( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        color, 3.f, 0, 1.f );
    dl->AddText( pos, color, text );

    // Defensive DT state, one row below the DT indicator.
    if ( g_config.settings.misc.doubletap.defensive )
    {
        const char* dtext = DefensiveDT::GetIndicatorText();
        const ImVec2 dsize = ImGui::CalcTextSize( dtext );
        const ImVec2 dpos( 18.f, pos.y + textSize.y + 14.f );
        const ImU32 dcol = IM_COL32( 195, 105, 255, 235 );
        dl->AddRectFilled( ImVec2( dpos.x - 7.f, dpos.y - 4.f ),
            ImVec2( dpos.x + dsize.x + 7.f, dpos.y + dsize.y + 4.f ),
            IM_COL32( 10, 10, 14, 170 ), 3.f );
        dl->AddRect( ImVec2( dpos.x - 7.f, dpos.y - 4.f ),
            ImVec2( dpos.x + dsize.x + 7.f, dpos.y + dsize.y + 4.f ),
            dcol, 3.f, 0, 1.f );
        dl->AddText( dpos, dcol, dtext );
    }
}

[[maybe_unused]] static void DrawMinDamageOverrideIndicator()
{
    if ( !Aimbot::IsMinDamageOverrideIndicatorActive() || !g_pLocalPlayer ||
         !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    char text[48];
    sprintf_s( text, "MIN DMG | %d", Aimbot::GetMinDamageOverrideValue() );
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 textSize = ImGui::CalcTextSize( text );
    // One row above the existing Rage profile and Double Tap HUD stack.
    const ImVec2 pos( 18.f, display.y * 0.5f - 20.f );
    const ImU32 color = IM_COL32( 105, 220, 145, 235 );
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 10, 10, 14, 170 ), 3.f );
    dl->AddRect( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        color, 3.f, 0, 1.f );
    dl->AddText( pos, color, text );
}

static void DrawFakePingIndicator()
{
    const auto& fp = g_config.settings.misc.fake_ping;
    if ( !fp.enabled || !fp.indicator || !FakeLag::IsFakePingActive() ||
         !g_pLocalPlayer || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    char text[48]{};
    sprintf_s( text, "FAKE PING | +%dms", FakeLag::GetFakePingEffectiveDelayMs() );
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 textSize = ImGui::CalcTextSize( text );
    const ImVec2 pos( 18.f, display.y * 0.5f - 76.f );
    const ImU32 color = IM_COL32( 105, 185, 255, 240 );
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 8, 18, 34, 180 ), 3.f );
    dl->AddRect( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        color, 3.f, 0, 1.f );
    dl->AddText( pos, color, text );
}

static void DrawBacktrackOnlyTestIndicator()
{
    const auto& aim = g_config.settings.aimbot;
    if ( !aim.enabled || !aim.rage || !aim.backtrack ||
         !aim.backtrack_only_test || !g_pLocalPlayer ||
         !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    char text[64]{};
    sprintf_s( text, "BT ONLY TEST | %dT", aim.backtrack_ticks );
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 textSize = ImGui::CalcTextSize( text );
    const ImVec2 pos( 18.f, display.y * 0.5f - 48.f );
    const ImU32 color = IM_COL32( 255, 150, 65, 245 );
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 42, 20, 8, 185 ), 3.f );
    dl->AddRect( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        color, 3.f, 0, 1.f );
    dl->AddText( pos, color, text );
}

static void DrawRageProfileIndicator()
{
    const auto& aim = g_config.settings.aimbot;
    if ( !aim.enabled || !aim.rage || !aim.weapon_profiles.enabled ||
         !g_pLocalPlayer || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    char text[96];
    sprintf_s( text, "RAGE | %s", Aimbot::GetActiveRageProfileName() );
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 textSize = ImGui::CalcTextSize( text );
    const ImVec2 pos( 18.f, display.y * 0.5f + 4.f );
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 10, 10, 14, 170 ), 3.f );
    dl->AddRect( ImVec2( pos.x - 7.f, pos.y - 4.f ),
        ImVec2( pos.x + textSize.x + 7.f, pos.y + textSize.y + 4.f ),
        IM_COL32( 225, 145, 80, 235 ), 3.f, 0, 1.f );
    dl->AddText( pos, IM_COL32( 245, 190, 115, 235 ), text );
}

// ------------------------------------------------------------------
// Auto-peek anchor: soft ground glow with a short upward-fading wall.
// World geometry is fixed at the captured anchor. Every vertex uses the
// same immutable render-frame camera; no screen-space smoothing or pulse.
// ------------------------------------------------------------------
namespace
{
    constexpr int kPeekSegs = 64;
    struct PeekVertex { Vec2 screen; float opacity; };
    struct PeekTriangle { PeekVertex vertices[3]; };
    struct PeekCache { std::vector<PeekTriangle> triangles; };
    std::mutex s_peekMutex;
    PeekCache s_peek;

    // Clip before perspective division, including the near plane. A marker
    // partly under/behind the camera must not explode into screen-wide fans.
    static void PeekTriangleWorld(PeekCache& cache, const Vec3& a, float aa,
        const Vec3& b, float ab, const Vec3& c, float ac)
    {
        struct ClipVertex { SDK::ProjectionClip p; float opacity; };
        ClipVertex input[12]{}, output[12]{};
        input[0] = {SDK::ToProjectionClip(a), aa};
        input[1] = {SDK::ToProjectionClip(b), ab};
        input[2] = {SDK::ToProjectionClip(c), ac};
        for (int i=0; i<3; ++i)
            if (!std::isfinite(input[i].p.x) || !std::isfinite(input[i].p.y) ||
                !std::isfinite(input[i].p.w)) return;
        const auto distance = [](const SDK::ProjectionClip& p, int plane) {
            switch (plane) {
            case 0: return p.w - .001f;
            case 1: return p.w + p.x;
            case 2: return p.w - p.x;
            case 3: return p.w + p.y;
            default: return p.w - p.y;
            }
        };
        int count = 3;
        for (int plane=0; plane<5 && count; ++plane)
        {
            int written=0;
            ClipVertex previous=input[count-1];
            float pd=distance(previous.p, plane);
            for (int i=0; i<count; ++i)
            {
                const ClipVertex current=input[i];
                const float cd=distance(current.p, plane);
                if ((pd >= 0.f) != (cd >= 0.f))
                {
                    const float t=pd/(pd-cd);
                    output[written++] = {{
                        previous.p.x+(current.p.x-previous.p.x)*t,
                        previous.p.y+(current.p.y-previous.p.y)*t,
                        previous.p.w+(current.p.w-previous.p.w)*t},
                        previous.opacity+(current.opacity-previous.opacity)*t};
                }
                if (cd >= 0.f) output[written++]=current;
                previous=current; pd=cd;
            }
            count=written;
            for (int i=0; i<count; ++i) input[i]=output[i];
        }
        PeekVertex projected[12]{};
        const auto& projection=SDK::g_worldProjection;
        for (int i=0; i<count; ++i)
        {
            const auto& p=input[i].p;
            if (p.w < .00099f) return;
            projected[i]={{(1.f+p.x/p.w)*projection.width*.5f,
                (1.f-p.y/p.w)*projection.height*.5f},input[i].opacity};
        }
        for (int i=1; i+1<count; ++i)
            cache.triangles.push_back({{projected[0],projected[i],projected[i+1]}});
    }

    static void BuildPeekGlow(PeekCache& cache, const Vec3& anchor)
    {
        const auto& matrix=SDK::g_worldProjection.matrix;
        const float depthScale=sqrtf(matrix[3][0]*matrix[3][0]+
            matrix[3][1]*matrix[3][1]+matrix[3][2]*matrix[3][2]);
        // Fade close to/inside the camera instead of tinting the whole screen.
        // Normalize by the matrix row length so this is measured in world units.
        const float fade=depthScale > .000001f ? std::clamp(
            SDK::ToProjectionClip(anchor).w/(depthScale*24.f),0.f,1.f) : 1.f;
        if (fade <= 0.f || !std::isfinite(fade)) return;
        const size_t firstTriangle=cache.triangles.size();
        const auto point = [&](float radius, float angle, float height) {
            return Vec3{anchor.x+cosf(angle)*radius,
                anchor.y+sinf(angle)*radius, anchor.z+height};
        };
        // Broad soft footprint, not a bright white outline. Alpha falls to
        // zero at the outside edge and at the top of the cylindrical glow.
        constexpr float radii[]={0.f,10.f,18.f,28.f};
        constexpr float alpha[]={.70f,.60f,.22f,0.f};
        cache.triangles.reserve(960);
        for (int i=0; i<kPeekSegs; ++i)
        {
            const float a=6.2831853f*i/kPeekSegs;
            const float b=6.2831853f*(i+1)/kPeekSegs;
            for (int band=0; band<3; ++band)
            {
                const Vec3 p=point(radii[band],a,1.f), q=point(radii[band],b,1.f);
                const Vec3 r=point(radii[band+1],a,1.f), t=point(radii[band+1],b,1.f);
                PeekTriangleWorld(cache,p,alpha[band],r,alpha[band+1],t,alpha[band+1]);
                if (band) PeekTriangleWorld(cache,p,alpha[band],t,alpha[band+1],q,alpha[band]);
            }
        }
        // Several low-opacity shells soften the silhouette as well as the
        // height. A single opaque shell leaves a hard-edged cylinder.
        constexpr float wallRadii[]={6.f,10.f,14.f,18.f,22.f};
        constexpr float wallAlpha[]={.09f,.08f,.055f,.035f,.015f};
        for (int layer=4; layer>=0; --layer)
            for (int i=0; i<kPeekSegs; ++i)
            {
                const float a=6.2831853f*i/kPeekSegs, b=6.2831853f*(i+1)/kPeekSegs;
                const float radius=wallRadii[layer], alpha=wallAlpha[layer];
                const Vec3 p=point(radius,a,1.f), q=point(radius,b,1.f);
                const Vec3 r=point(radius,a,17.f), t=point(radius,b,17.f);
                PeekTriangleWorld(cache,p,alpha,q,alpha,t,0.f);
                PeekTriangleWorld(cache,p,alpha,t,0.f,r,0.f);
            }
        for (size_t i=firstTriangle; i<cache.triangles.size(); ++i)
            for (auto& vertex:cache.triangles[i].vertices) vertex.opacity*=fade;
    }
}

static void ProjectAutoPeek()
{
    PeekCache next;
    Vec3 anchor{}; bool returning=false;
    if (g_config.settings.misc.auto_peek && g_pLocalPlayer && SDK::g_pEngine &&
        SDK::g_pEngine->IsInGame() && SDK::g_worldProjection.active &&
        SDK::g_worldProjection.valid && AutoPeek::GetAnchor(anchor,returning))
        BuildPeekGlow(next,anchor);
    std::lock_guard<std::mutex> lock(s_peekMutex);
    s_peek=std::move(next);
}

// ------------------------------------------------------------------
//  Utility timers — smoke countdown + molotov fire zone.
//
//  Smoke clouds are the C_ParticleSmokeGrenade entities NoSmoke already
//  knows (client vtable RVA 0x6F8ED8, cloud radius at +0x1584). Inferno
//  entities are learned once by network class name ("Inferno") and then
//  tracked by vtable. Every zone renders a soft ground disc (the same
//  clipped projection pipeline as the auto-peek anchor) plus a
//  remaining-time label above its center. Lifetime is measured from
//  the first frame the entity is observed (client curtime), so no
//  per-gamemode offsets are needed.
// ------------------------------------------------------------------
namespace
{
    constexpr float kSmokeDuration = 18.f; // CS smoke lifetime
    constexpr float kFireDuration  = 7.f;  // molotov/inferno burn time
    constexpr int   kUtilitySegs   = 48;
    constexpr uintptr_t kSmokeCloudVtableRVA = 0x6F8ED8;

    struct UtilVertex { Vec2 screen; float opacity; };
    struct UtilTriangle { UtilVertex vertices[3]; ImU32 color; };
    struct UtilLabel { ImVec2 pos; char text[32]; ImU32 color; };
    struct UtilCache
    {
        std::vector<UtilTriangle> triangles;
        std::vector<UtilLabel> labels;
    };
    std::mutex s_utilMutex;
    UtilCache  s_util;

    struct UtilBirth { void* ent; float bornAt; Vec3 origin{}; float groundZ = 0.f; bool fire = false; bool seen = false; };
    std::vector<UtilBirth> s_utilBirths;
    std::vector<float> s_smokeLives;   // last observed lifetimes (self-calibrating)
    std::vector<float> s_fireLives;
    void* s_infernoVtable = nullptr;
    int   s_utilScan = 0;

    // Countdown duration = median of the last observed lifetimes, so the
    // timer matches the ACTUAL server durations (round-restart outliers are
    // damped by the median). Falls back to the defaults for the first uses.
    static float UtilDuration( bool fire )
    {
        const std::vector<float>& v = fire ? s_fireLives : s_smokeLives;
        if ( v.empty() )
            return fire ? kFireDuration : kSmokeDuration;
        std::vector<float> sorted( v.begin(), v.end() );
        std::sort( sorted.begin(), sorted.end() );
        const float median = sorted[ sorted.size() / 2 ];
        return std::clamp( median, fire ? 4.f : 12.f, fire ? 12.f : 25.f );
    }

    // One short downward trace per zone, cached at birth: puts the disc on
    // the floor instead of floating at the detonation/cloud height.
    static float UtilGroundZ( const Vec3& origin )
    {
        auto* trace = SDK::g_pEngineTrace ? SDK::g_pEngineTrace : SDK::IEngineTrace::Get();
        if ( !trace )
            return origin.z + 2.f;
        SDK::Ray_t ray{};
        std::memset( &ray, 0, sizeof( ray ) );
        ray.m_Start   = origin;
        ray.m_Delta   = { 0.f, 0.f, -160.f };
        ray.m_IsRay   = true;
        ray.m_IsSwept = true;
        SDK::CGameTrace tr{};
        SDK::CTraceFilterSimple filter( g_pLocalPlayer );
        trace->TraceRay( ray, SDK::MASK_SHOT, &filter, &tr );
        if ( tr.fraction > 0.f && tr.fraction < 1.f )
            return origin.z + ray.m_Delta.z * tr.fraction + 2.f;
        return origin.z + 2.f;
    }

    static float UtilBirthTime( void* ent, const Vec3& origin, float now,
                                bool fire, float& groundZ, bool& fresh )
    {
        fresh = false;
        for ( auto& b : s_utilBirths )
            if ( b.ent == ent )
            {
                b.seen = true;
                // The same entity slot reused by a new grenade lands
                // somewhere else: restart the countdown instead of
                // inheriting the previous record.
                const float dx = origin.x - b.origin.x, dy = origin.y - b.origin.y;
                if ( dx * dx + dy * dy > 300.f * 300.f ||
                     std::abs( origin.z - b.origin.z ) > 200.f )
                {
                    b.bornAt = now;
                    b.origin = origin;
                    b.fire = fire;
                    b.groundZ = UtilGroundZ( origin );
                    fresh = true;
                }
                groundZ = b.groundZ;
                return b.bornAt;
            }
        UtilBirth rec{};
        rec.ent = ent;
        rec.bornAt = now;
        rec.origin = origin;
        rec.fire = fire;
        rec.seen = true;
        rec.groundZ = UtilGroundZ( origin );
        groundZ = rec.groundZ;
        s_utilBirths.push_back( rec );
        fresh = true;
        return now;
    }

    // Same near-plane-aware clip->project path as the peek glow, tinted per
    // triangle so smoke and fire zones can carry different colors.
    static void UtilTriangleWorld( UtilCache& cache, const Vec3& a, float aa,
        const Vec3& b, float ab, const Vec3& c, float ac, ImU32 color )
    {
        struct ClipVertex { SDK::ProjectionClip p; float opacity; };
        ClipVertex input[12]{}, output[12]{};
        input[0] = {SDK::ToProjectionClip(a), aa};
        input[1] = {SDK::ToProjectionClip(b), ab};
        input[2] = {SDK::ToProjectionClip(c), ac};
        for (int i=0;i<3;++i)
            if (!std::isfinite(input[i].p.x) || !std::isfinite(input[i].p.y) ||
                !std::isfinite(input[i].p.w)) return;
        const auto distance = [](const SDK::ProjectionClip& p, int plane) {
            switch (plane) {
            case 0: return p.w - .001f;
            case 1: return p.w + p.x;
            case 2: return p.w - p.x;
            case 3: return p.w + p.y;
            default: return p.w - p.y;
            }
        };
        int count = 3;
        for (int plane=0; plane<5 && count; ++plane)
        {
            int written=0;
            ClipVertex previous=input[count-1];
            float pd=distance(previous.p, plane);
            for (int i=0;i<count; ++i)
            {
                const ClipVertex current=input[i];
                const float cd=distance(current.p, plane);
                if ((pd >= 0.f) != (cd >= 0.f))
                {
                    const float t=pd/(pd-cd);
                    output[written++] = {{
                        previous.p.x+(current.p.x-previous.p.x)*t,
                        previous.p.y+(current.p.y-previous.p.y)*t,
                        previous.p.w+(current.p.w-previous.p.w)*t},
                        previous.opacity+(current.opacity-previous.opacity)*t};
                }
                if (cd >= 0.f) output[written++]=current;
                previous=current; pd=cd;
            }
            count=written;
            for (int i=0;i<count; ++i) input[i]=output[i];
        }
        UtilVertex projected[12]{};
        const auto& projection=SDK::g_worldProjection;
        for (int i=0;i<count; ++i)
        {
            const auto& p=input[i].p;
            if (p.w < .00099f) return;
            projected[i]={{(1.f+p.x/p.w)*projection.width*.5f,
                (1.f-p.y/p.w)*projection.height*.5f},input[i].opacity};
        }
        for (int i=1;i+1<count; ++i)
            cache.triangles.push_back({{projected[0],projected[i],projected[i+1]},color});
    }

    static bool UtilProjectPoint( const Vec3& w, ImVec2& out )
    {
        const auto p = SDK::ToProjectionClip( w );
        if ( !std::isfinite( p.x ) || !std::isfinite( p.y ) ||
             !std::isfinite( p.w ) || p.w < 1.f )
            return false;
        const auto& pr = SDK::g_worldProjection;
        out = { ( 1.f + p.x / p.w ) * pr.width * .5f,
                ( 1.f - p.y / p.w ) * pr.height * .5f };
        return std::isfinite( out.x ) && std::isfinite( out.y );
    }

    static ImU32 UtilColor( const cfg::box_color_t& c )
    {
        return IM_COL32( std::clamp( c.r, 0, 255 ), std::clamp( c.g, 0, 255 ),
            std::clamp( c.b, 0, 255 ), std::clamp( c.a, 0, 255 ) );
    }

    static void BuildUtilityZone( UtilCache& cache, const Vec3& origin, float groundZ,
        float radius, float remaining, float duration, bool fire,
        ImU32 ground, ImU32 labelCol )
    {
        // Soft radial fill: bright core fading to zero at the rim, plus a
        // brighter edge ring that reads as the zone boundary. The countdown
        // tint fades with the remaining time so a dying zone dims out.
        const float life = duration > 0.f ? std::clamp( remaining / duration, 0.f, 1.f ) : 1.f;
        const auto point = [&]( float r, float angle ) {
            return Vec3{ origin.x + cosf( angle ) * r,
                         origin.y + sinf( angle ) * r, groundZ + 1.f };
        };
        constexpr float radii[]  = { 0.f, .45f, .8f, 1.f };
        const float    alpha[]   = { .55f, .38f, .16f, 0.f };
        for ( int i = 0; i < kUtilitySegs; ++i )
        {
            const float a = 6.2831853f * i / kUtilitySegs;
            const float b = 6.2831853f * ( i + 1 ) / kUtilitySegs;
            for ( int band = 0; band < 3; ++band )
            {
                const Vec3 p = point( radii[band]*radius, a ), q = point( radii[band]*radius, b );
                const Vec3 r = point( radii[band+1]*radius, a ), t = point( radii[band+1]*radius, b );
                UtilTriangleWorld( cache, p, alpha[band]*life, r, alpha[band+1]*life,
                    t, alpha[band+1]*life, ground );
                if ( band ) UtilTriangleWorld( cache, p, alpha[band]*life, t, alpha[band+1]*life,
                    q, alpha[band]*life, ground );
            }
            // Boundary ring: thin band just inside the rim.
            const Vec3 p1 = point( radius*.9f, a ), q1 = point( radius*.9f, b );
            const Vec3 r1 = point( radius, a ),     t1 = point( radius, b );
            UtilTriangleWorld( cache, p1, .45f*life, r1, 0.f, t1, 0.f, ground );
            UtilTriangleWorld( cache, p1, .45f*life, t1, 0.f, q1, .45f*life, ground );
        }

        ImVec2 screen;
        if ( UtilProjectPoint( { origin.x, origin.y, groundZ + 46.f }, screen ) )
        {
            UtilLabel label{};
            sprintf_s( label.text, "%s %.1f", fire ? "FIRE" : "SMOKE", remaining );
            label.pos = screen;
            label.color = labelCol;
            cache.labels.push_back( label );
        }
    }
}

static void ProjectUtilityTimers()
{
    UtilCache next;

    const bool ready = SDK::g_pEntityList && SDK::g_pEngine &&
        SDK::g_pEngine->IsInGame() &&
        SDK::g_worldProjection.active && SDK::g_worldProjection.valid;
    const float now = ready ? SDK::WorldEntity::CurTime() : -1.f;

    // Retire entities that vanished last frame and feed their observed
    // lifetime into the calibration, so future countdowns match the real
    // server durations (median damps round-restart outliers).
    if ( now >= 0.f )
    {
        for ( auto& b : s_utilBirths )
        {
            if ( b.seen )
                continue;
            const float life = std::clamp( now - b.bornAt, 1.f, 30.f );
            auto& list = b.fire ? s_fireLives : s_smokeLives;
            list.push_back( life );
            if ( list.size() > 5 )
                list.erase( list.begin() );
        }
    }
    s_utilBirths.erase( std::remove_if( s_utilBirths.begin(), s_utilBirths.end(),
        []( const UtilBirth& b ) { return !b.seen; } ), s_utilBirths.end() );
    for ( auto& b : s_utilBirths )
        b.seen = false;

    const auto& o = g_config.settings.visuals.others;
    if ( ( !o.molotov_zone && !o.smoke_timer ) || !ready || now < 0.f )
    {
        std::lock_guard<std::mutex> lock( s_utilMutex );
        s_util = std::move( next );
        return;
    }

    const uintptr_t client = SDK::g_uClientBase;
    const uintptr_t smokeVt = client ? client + kSmokeCloudVtableRVA : 0;
    ++s_utilScan;

    const ImU32 fireGround = UtilColor( o.molotov_color );
    const ImU32 fireLabel  = IM_COL32( 255, 170, 90, 235 );
    const ImU32 smokeGround = UtilColor( o.smoke_timer_color );
    const ImU32 smokeLabel  = IM_COL32( 225, 225, 235, 235 );

    for ( int i = 1; i < 2048; ++i )
    {
        void* ent = SDK::g_pEntityList->GetClientEntity( i );
        if ( !ent )
            continue;
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>( ent );

        bool smoke = smokeVt && vt == smokeVt;
        bool fire  = s_infernoVtable && vt == reinterpret_cast<uintptr_t>( s_infernoVtable );

        // Learn the inferno vtable from the network class name; after the
        // first hit this is a plain pointer compare every frame.
        if ( !smoke && !fire && !s_infernoVtable && ( s_utilScan % 64 ) == 0 )
        {
            const char* name = SDK::WorldEntity::ClassName( ent );
            if ( name && strstr( name, "Inferno" ) )
            {
                s_infernoVtable = reinterpret_cast<void*>( vt );
                fire = o.molotov_zone;
            }
        }

        if ( ( smoke && !o.smoke_timer ) || ( fire && !o.molotov_zone ) )
            continue;
        if ( !smoke && !fire )
            continue;

        const Vec3 origin = *reinterpret_cast<const Vec3*>(
            reinterpret_cast<uintptr_t>( ent ) + SDK::nOriginOffset );
        if ( !std::isfinite( origin.x ) || !std::isfinite( origin.y ) ||
             !std::isfinite( origin.z ) )
            continue;

        float radius = fire ? 150.f : 144.f;
        if ( smoke )
        {
            const float r = *reinterpret_cast<const float*>(
                reinterpret_cast<uintptr_t>( ent ) + 0x1584 );
            if ( std::isfinite( r ) && r > 40.f && r < 400.f )
                radius = r;
        }

        const float duration = UtilDuration( fire );
        bool fresh = false;
        float groundZ = origin.z;
        const float born = UtilBirthTime( ent, origin, now, fire, groundZ, fresh );
        (void)fresh;
        const float remaining = std::clamp( duration - ( now - born ), 0.f, duration );

        BuildUtilityZone( next, origin, groundZ, radius, remaining, duration, fire,
            fire ? fireGround : smokeGround, fire ? fireLabel : smokeLabel );
    }

    std::lock_guard<std::mutex> lock( s_utilMutex );
    s_util = std::move( next );
}

static void DrawUtilityTimers()
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    std::lock_guard<std::mutex> lock( s_utilMutex );
    for ( const auto& triangle : s_util.triangles )
    {
        dl->PrimReserve( 3, 3 );
        const ImDrawIdx base = static_cast<ImDrawIdx>( dl->_VtxCurrentIdx );
        for ( const auto& v : triangle.vertices )
            dl->PrimWriteVtx( { v.screen.x, v.screen.y }, uv,
                ( triangle.color & 0x00FFFFFF ) |
                static_cast<ImU32>( static_cast<float>( ( triangle.color >> 24 ) & 0xFF ) * v.opacity ) << 24 );
        dl->PrimWriteIdx( base ); dl->PrimWriteIdx( base + 1 ); dl->PrimWriteIdx( base + 2 );
    }
    for ( const auto& label : s_util.labels )
    {
        const ImVec2 size = ImGui::CalcTextSize( label.text );
        dl->AddRectFilled( ImVec2( label.pos.x - size.x * .5f - 5.f, label.pos.y - 3.f ),
            ImVec2( label.pos.x + size.x * .5f + 5.f, label.pos.y + size.y + 3.f ),
            IM_COL32( 10, 10, 14, 170 ), 3.f );
        dl->AddText( ImVec2( label.pos.x - size.x * .5f, label.pos.y ), label.color, label.text );
    }
}

static void DrawAutoPeekAnchor()
{
    if (!g_config.settings.misc.auto_peek) return;
    const auto& color=g_config.settings.misc.auto_peek_color;
    const int r=std::clamp(color.r,0,255), g=std::clamp(color.g,0,255);
    const int b=std::clamp(color.b,0,255), a=std::clamp(color.a,0,255);
    if (!a) return;
    ImDrawList* dl=ImGui::GetForegroundDrawList();
    const ImVec2 uv=ImGui::GetFontTexUvWhitePixel();
    std::lock_guard<std::mutex> lock(s_peekMutex);
    for (const auto& triangle:s_peek.triangles)
    {
        dl->PrimReserve(3,3);
        const ImDrawIdx base=static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        for (const auto& v:triangle.vertices)
            dl->PrimWriteVtx({v.screen.x,v.screen.y},uv,
                IM_COL32(r,g,b,static_cast<int>(a*v.opacity)));
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(base+1); dl->PrimWriteIdx(base+2);
    }
}

static void DrawWatermark()
{
    if ( !g_config.settings.visuals.others.watermark )
        return;
    if ( !g_pLocalPlayer || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    const int localIdx = SDK::g_pEngine->GetLocalPlayer();
    SDK::player_info_t info{};
    const char* name = SDK::g_pEngine->GetPlayerInfo( localIdx, &info ) &&
        info.name[ 0 ] ? info.name : "player";
    const int fps = static_cast<int>( ImGui::GetIO().Framerate );
    SYSTEMTIME st{};
    GetLocalTime( &st );

    // One compact title avoids the artificial game/sense spacing visible in
    // the previous layout. The separators below are the only intentional gaps.
    const char* titleText = "big pickle";
    char tailText[ 96 ]{};
    sprintf_s( tailText, " | %s | %d fps | %02d:%02d", name, fps, st.wHour, st.wMinute );

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 titleSize = ImGui::CalcTextSize( titleText );
    const ImVec2 tailSize = ImGui::CalcTextSize( tailText );
    const float width = titleSize.x + tailSize.x + 8.f;
    const float height = 18.f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float x = display.x - width - 12.f;
    const float y = 12.f;

    // Same nested dark frame/inset proportions as the supplied renderer.
    dl->AddRectFilled( ImVec2( x - 6.f, y - 6.f ),
        ImVec2( x + width + 7.f, y + height + 8.f ), IM_COL32( 0, 0, 0, 255 ) );
    dl->AddRectFilled( ImVec2( x - 5.f, y - 5.f ),
        ImVec2( x + width + 6.f, y + height + 7.f ), IM_COL32( 34, 34, 34, 255 ) );
    dl->AddRectFilled( ImVec2( x + 1.f, y ),
        ImVec2( x + width + 1.f, y + height + 1.f ), IM_COL32( 0, 0, 0, 255 ) );
    dl->AddRect( ImVec2( x - 1.f, y - 1.f ),
        ImVec2( x + width + 2.f, y + height + 2.f ), IM_COL32( 56, 56, 56, 255 ) );
    dl->AddRect( ImVec2( x - 5.f, y - 5.f ),
        ImVec2( x + width + 6.f, y + height + 7.f ), IM_COL32( 56, 56, 56, 255 ) );

    // The top accent is split in two exactly as the provided watermark:
    // cyan -> purple -> lime, followed by a softened second row.
    const float split = width * 0.5f;
    const ImU32 cyan = IM_COL32( 59, 175, 222, 255 );
    const ImU32 purple = IM_COL32( 202, 70, 205, 255 );
    const ImU32 lime = IM_COL32( 201, 227, 58, 255 );
    const ImU32 cyanSoft = IM_COL32( 59, 175, 222, 130 );
    const ImU32 purpleSoft = IM_COL32( 202, 70, 205, 130 );
    const ImU32 limeSoft = IM_COL32( 201, 227, 58, 130 );
    dl->AddRectFilledMultiColor( ImVec2( x + 2.f, y + 1.f ), ImVec2( x + 1.f + split, y + 2.f ),
        cyan, purple, purple, cyan );
    dl->AddRectFilledMultiColor( ImVec2( x + 1.f + split, y + 1.f ), ImVec2( x + width, y + 2.f ),
        purple, lime, lime, purple );
    dl->AddRectFilledMultiColor( ImVec2( x + 2.f, y + 2.f ), ImVec2( x + 1.f + split, y + 3.f ),
        cyanSoft, purpleSoft, purpleSoft, cyanSoft );
    dl->AddRectFilledMultiColor( ImVec2( x + 1.f + split, y + 2.f ), ImVec2( x + width, y + 3.f ),
        purpleSoft, limeSoft, limeSoft, purpleSoft );

    const float textY = y + ( height - titleSize.y ) * 0.5f + 1.f;
    const float titleX = x + 5.f;
    const float tailX = titleX + titleSize.x;
    const ImU32 shadow = IM_COL32( 0, 0, 0, 180 );
    dl->AddText( ImVec2( titleX, textY + 1.f ), shadow, titleText );
    dl->AddText( ImVec2( tailX, textY + 1.f ), shadow, tailText );
    dl->AddText( ImVec2( titleX, textY ), IM_COL32( 255, 255, 255, 255 ), titleText );
    dl->AddText( ImVec2( tailX, textY ), IM_COL32( 255, 255, 255, 255 ), tailText );
}

static void DrawScopeLines()
{
    if ( !g_config.settings.misc.remove_scope_overlay )
        return;
    if ( !g_pLocalPlayer || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;

    auto* local = reinterpret_cast<SDK::C_BaseEntity*>( g_pLocalPlayer );
    if ( !local || !local->IsScoped() )
        return;

    const auto& ch = g_config.settings.misc.crosshair;
    const ImVec2 c = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const ImU32 col = IM_COL32(
        std::clamp( ch.scope_color.r, 0, 255 ), std::clamp( ch.scope_color.g, 0, 255 ),
        std::clamp( ch.scope_color.b, 0, 255 ), std::clamp( ch.scope_color.a, 0, 255 ) );
    const ImU32 out = IM_COL32( 0, 0, 0, (std::min)( ch.scope_color.a, 220 ) );
    const float th = (std::max)( ch.scope_thickness, 1.f );
    const float cx = c.x * 0.5f;
    const float cy = c.y * 0.5f;

    const auto line = [&]( float x1, float y1, float x2, float y2 )
    {
        if ( ch.scope_outline )
            dl->AddRectFilled( ImVec2( x1 - 1.f, y1 - 1.f ),
                               ImVec2( x2 + 1.f, y2 + 1.f ), out );
        dl->AddRectFilled( ImVec2( x1, y1 ), ImVec2( x2, y2 ), col );
    };

    const float len = (std::max)( ch.scope_length, 0.f );
    const float gap = (std::max)( ch.scope_gap, 0.f );
    if ( len <= 0.f )
    {
        // Classic full-screen scope lines.
        line( cx - th * 0.5f, 0.f, cx + th * 0.5f, c.y );
        line( 0.f, cy - th * 0.5f, c.x, cy + th * 0.5f );
    }
    else
    {
        // Centered scope crosshair.
        if ( !ch.scope_t_style )
            line( cx - th * 0.5f, cy - gap - len, cx + th * 0.5f, cy - gap );
        line( cx - th * 0.5f, cy + gap, cx + th * 0.5f, cy + gap + len );
        line( cx - gap - len, cy - th * 0.5f, cx - gap, cy + th * 0.5f );
        line( cx + gap, cy - th * 0.5f, cx + gap + len, cy + th * 0.5f );
    }

    if ( ch.scope_dot )
    {
        const float ds = (std::max)( ch.scope_dot_size, 1.f ) * 0.5f;
        if ( ch.scope_outline )
            dl->AddRectFilled( ImVec2( cx - ds - 1.f, cy - ds - 1.f ),
                               ImVec2( cx + ds + 1.f, cy + ds + 1.f ), out );
        dl->AddRectFilled( ImVec2( cx - ds, cy - ds ), ImVec2( cx + ds, cy + ds ), col );
    }
}

}

void Hud::ProjectFrame()
{
    ProjectAutoPeek();
    ProjectUtilityTimers();
    EventLog::ProjectFrame();
}

void Hud::Render()
{
    DrawAutoPeekAnchor();
    DrawUtilityTimers();
    DrawFakePingIndicator();
    DrawBacktrackOnlyTestIndicator();
    AntiAim::DrawIndicator();
    DrawScopeLines();
    DrawWatermark();
}
