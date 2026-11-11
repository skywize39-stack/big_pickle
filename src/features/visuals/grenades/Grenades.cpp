#include "features/visuals/grenades/Grenades.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include "sdk/Entities.hpp"
#include "sdk/Projection.hpp"
#include "sdk/WorldEntity.hpp"
#include "sdk/GameTypes.hpp"
#include "config/Config.hpp"
#include "render/surface/SurfaceRenderer.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cmath>
#include <imgui.h>

// ------------------------------------------------------------------
// Grenade producer runs in the verified CreateMove path, after its original.
// Thrown forecasts use paired NETWORK fields and cache unchanged samples;
// they do not depend on an unverified frame-stage callback or local clocks.
// Rendering consumes a copied cache and performs only world-to-screen draw.
// ------------------------------------------------------------------
namespace
{
    constexpr int kGrenadePredictionMaxPoints = 512;
    constexpr float kGrenadeStep = 1.f / 64.f;
    constexpr float kGrenadeDetonateCheckInterval = 0.2f;
    constexpr float kGrenadeGravity = 320.f;       // server-side 800 * 0.4 path
    constexpr float kGrenadeElasticity = 0.45f;    // projectile bounce coefficient

    struct GrenadePredictionCache
    {
        Vec3 points[kGrenadePredictionMaxPoints]{};
        Vec2 screen[kGrenadePredictionMaxPoints]{};
        bool projected[kGrenadePredictionMaxPoints]{};
        bool bounced[kGrenadePredictionMaxPoints]{};
        int count = 0;
        bool active = false;
        bool detonates = false;
        int expectedHeDamage = 0;
        int expectedHeTarget = 0;
    };

    std::mutex s_grenadePredictionMutex;
    GrenadePredictionCache s_grenadePredictionCache{};

    float DotProduct( const Vec3& a, const Vec3& b )
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Vec3 GrenadeForward( float pitch, float yaw )
    {
        constexpr float kDegToRad = 0.01745329251994329577f;
        const float pitchRad = pitch * kDegToRad;
        const float yawRad = yaw * kDegToRad;
        const float cp = cosf( pitchRad );
        return { cp * cosf( yawRad ), cp * sinf( yawRad ), -sinf( pitchRad ) };
    }

    bool IsHeGrenadeClass( const char* className )
    {
        return className && strstr( className, "hegrenade" );
    }

    bool IsThrowableGrenadeClass( const char* className )
    {
        return className && ( IsHeGrenadeClass( className ) ||
                              strstr( className, "flashbang" ) ||
                              strstr( className, "smokegrenade" ) ||
                              strstr( className, "decoy" ) ||
                              strstr( className, "molotov" ) ||
                              strstr( className, "incendiary" ) );
    }

    void ClearGrenadePredictionCache()
    {
        std::lock_guard<std::mutex> lock( s_grenadePredictionMutex );
        s_grenadePredictionCache = {};
    }

    // HE damage is only estimated for a target that is within the published GO
    // blast radius and has a direct solid-mask line of sight to the predicted
    // detonation point. Any wall/prop obstruction fails closed to zero damage.
    int EstimateVisibleHeDamage( const Vec3& explosion, SDK::C_BaseEntity* local,
                                 SDK::IEngineTrace* trace, int& outTargetIndex )
    {
        outTargetIndex = 0;
        if ( !local || !trace || !SDK::g_pEntityList || !SDK::g_pEngine )
            return 0;

        // Matching weapon_hegrenade.txt (supplied from this game install):
        // Damage=100, Range=350.0 and WeaponArmorRatio=1.475. Source
        // RadiusDamage uses linear falloff (damage - distance * damage/range),
        // not the foreign GO Gaussian fallback used by the earlier revision.
        constexpr float kHeScriptDamage = 100.f;
        constexpr float kHeScriptRange = 350.f;
        constexpr float kHeScriptArmorRatio = 1.475f;
        constexpr float kHeArmorBonusRatio = 0.5f;
        constexpr float kHeArmorDamageScale = kHeScriptArmorRatio * 0.5f;
        const float radiusSq = kHeScriptRange * kHeScriptRange;
        const int maxClients = SDK::g_pEngine->GetMaxClients() < 65
            ? SDK::g_pEngine->GetMaxClients() : 64;

        int bestDamage = 0;
        SDK::CTraceFilterSimple filter( local );
        for ( int index = 1; index <= maxClients; ++index )
        {
            auto* target = reinterpret_cast<SDK::C_BaseEntity*>(
                SDK::g_pEntityList->GetClientEntity( index ) );
            if ( !target || target == local || target->GetLifeState() != 0 ||
                 target->GetHealth() <= 0 || target->GetTeamNum() == local->GetTeamNum() )
                continue;

            // Mid-body point is intentionally used instead of a guessed bone
            // offset. The LOS trace must either reach the target entity or stay
            // almost completely unobstructed; otherwise do not display damage.
            const Vec3 targetPoint = target->GetAbsOrigin() + target->GetViewOffset() * 0.5f;
            const Vec3 delta = targetPoint - explosion;
            const float distanceSq = DotProduct( delta, delta );
            if ( distanceSq > radiusSq )
                continue;

            SDK::Ray_t losRay{};
            losRay.m_Start = explosion;
            losRay.m_Delta = delta;
            losRay.m_IsRay = true;
            losRay.m_IsSwept = distanceSq > 0.0001f;
            SDK::CGameTrace los{};
            trace->TraceRay( losRay, SDK::MASK_SOLID, &filter, &los );
            if ( los.m_pEnt != target && los.fraction < 0.97f )
                continue;

            const float distance = sqrtf( distanceSq );
            float damage = kHeScriptDamage -
                distance * ( kHeScriptDamage / kHeScriptRange );
            if ( damage <= 0.f )
                continue;

            // The normal Source armor path uses weapon armor ratio * 0.5 and
            // consumes armor at the 0.5 bonus ratio. This also handles partial
            // armor instead of blindly multiplying every armored target.
            const int armor = target->GetArmor();
            if ( armor > 0 )
            {
                float armoredDamage = damage * kHeArmorDamageScale;
                if ( ( damage - armoredDamage ) * kHeArmorBonusRatio > armor )
                    armoredDamage = damage - armor / kHeArmorBonusRatio;
                damage = armoredDamage;
            }

            const int roundedDamage = static_cast<int>( damage + 0.5f );
            if ( roundedDamage > bestDamage )
            {
                bestDamage = roundedDamage;
                outTargetIndex = index;
            }
        }
        return bestDamage;
    }

    void SimulateGrenadeTrajectory( SDK::CUserCmd* cmd, SDK::C_BaseEntity* local )
    {
        if ( !cmd || !local || !g_config.settings.misc.grenade_prediction.enabled )
        {
            ClearGrenadePredictionCache();
            return;
        }

        void* weapon = local->GetActiveWeapon();
        const char* className = weapon ? reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>( weapon ) + SDK::nWeaponClassnameOffset ) : nullptr;
        const bool primary = ( cmd->buttons & SDK::IN_ATTACK ) != 0;
        const bool secondary = ( cmd->buttons & SDK::IN_ATTACK2 ) != 0;
        if ( !IsThrowableGrenadeClass( className ) || !( primary || secondary ) )
        {
            ClearGrenadePredictionCache();
            return;
        }

        // m_flThrowStrength is a confirmed CBaseCSGrenade field in this client.
        // Using it captures the engine's actual full/lob/drop state instead of
        // inferring force from buttons. Keep a conservative input fallback only
        // while the weapon's replicated field is not valid yet.
        float throwStrength = primary && secondary ? 0.25f : ( secondary ? 0.50f : 1.0f );
        const float replicatedStrength = *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>( weapon ) + SDK::nGrenadeThrowStrengthOffset );
        if ( std::isfinite( replicatedStrength ) &&
             replicatedStrength >= 0.f && replicatedStrength <= 1.f )
            throwStrength = replicatedStrength;

        // CSSO/Source throw setup: normalise pitch, derive throw velocity,
        // offset the source by strength, trace 22 units forward and pull it 6
        // units back. The source trace prevents starting the simulation inside
        // a nearby wall or prop.
        float throwPitch = SDK::GetPitch( cmd );
        throwPitch -= ( 90.f - fabsf( throwPitch ) ) * ( 10.f / 90.f );
        const float throwVelocity = 750.f * 0.9f * ( throwStrength * 0.7f + 0.3f );
        const Vec3 forward = GrenadeForward( throwPitch, SDK::GetYaw( cmd ) );
        Vec3 position = local->EyePosition();
        position.z += throwStrength * 12.f - 12.f;
        Vec3 velocity = local->GetVelocity() * 1.25f + forward * throwVelocity;

        auto* trace = SDK::IEngineTrace::Get();
        if ( !trace )
        {
            ClearGrenadePredictionCache();
            return;
        }

        SDK::CTraceFilterSimple filter( local );
        auto TraceGrenadeHull = [&]( const Vec3& start, const Vec3& end, SDK::CGameTrace& result )
        {
            SDK::Ray_t ray{};
            ray.m_Start = start;
            ray.m_Delta = end - start;
            ray.m_Extents = { 2.f, 2.f, 2.f };
            ray.m_IsRay = false;
            ray.m_IsSwept = DotProduct( ray.m_Delta, ray.m_Delta ) > 0.0001f;
            // Matching server projectile paths use MASK_SOLID (0x200400B), not
            // MASK_SHOT, which is a bullet mask.
            trace->TraceRay( ray, SDK::MASK_SOLID, &filter, &result );
        };

        SDK::CGameTrace setupTrace{};
        TraceGrenadeHull( position, position + forward * 22.f, setupTrace );
        position = setupTrace.endpos - forward * 6.f;

        GrenadePredictionCache next{};
        next.active = true;
        next.points[0] = position;
        next.count = 1;

        const bool heGrenade = IsHeGrenadeClass( className );
        const bool flashGrenade = strstr( className, "flashbang" ) != nullptr;
        const bool fireGrenade = strstr( className, "molotov" ) ||
                                 strstr( className, "incendiary" );
        const bool smokeOrDecoy = strstr( className, "smokegrenade" ) ||
                                  strstr( className, "decoy" );
        const int detonateTickInterval = static_cast<int>(
            kGrenadeDetonateCheckInterval / kGrenadeStep );
        const int detonateTickMod = detonateTickInterval > 0 ? detonateTickInterval : 1;

        // Regular Source projectile loop. Gravity is integrated using the mean
        // vertical speed over a tick, and detonation is sampled on the engine's
        // 0.2-second cadence instead of enforcing a hard visual cutoff.
        for ( int tick = 1; tick < kGrenadePredictionMaxPoints; ++tick )
        {
            const float newVelocityZ = velocity.z - kGrenadeGravity * kGrenadeStep;
            const Vec3 move{ velocity.x * kGrenadeStep,
                             velocity.y * kGrenadeStep,
                             ( velocity.z + newVelocityZ ) * 0.5f * kGrenadeStep };
            velocity.z = newVelocityZ;

            SDK::CGameTrace collision{};
            bool hit = false;
            if ( DotProduct( move, move ) > 0.0001f )
            {
                TraceGrenadeHull( position, position + move, collision );
                hit = collision.fraction < 1.f && !collision.allsolid;
                position = collision.endpos;
            }
            else
            {
                collision.endpos = position;
                collision.fraction = 1.f;
            }

            if ( hit )
            {
                const float normalVelocity = DotProduct( velocity, collision.plane.normal );
                velocity -= collision.plane.normal * ( 2.f * normalVelocity );
                velocity = velocity * kGrenadeElasticity;

                // Source's sleep test is on the reflected/damped velocity.
                if ( DotProduct(velocity,velocity) < 400.f )
                    velocity = {};
                else if ( collision.plane.normal.z > 0.7f )
                {
                    // Move through the unconsumed fraction of the same tick,
                    // exactly where the old coarse loop created post-bounce drift.
                    SDK::CGameTrace remainder{};
                    const float remainingTime = ( 1.f - collision.fraction ) * kGrenadeStep;
                    TraceGrenadeHull( position, position + velocity * remainingTime, remainder );
                    position = remainder.endpos;
                }
            }

            next.points[next.count] = position;
            next.bounced[next.count] = hit;
            ++next.count;

            const float elapsed = static_cast<float>( tick ) * kGrenadeStep;
            const bool detonateSample = ( tick % detonateTickMod ) == 0;
            bool detonate = false;
            if ( heGrenade || flashGrenade )
                detonate = elapsed > 1.5f && detonateSample;
            else if ( fireGrenade )
                detonate = ( hit && collision.plane.normal.z > 0.7f ) ||
                           ( elapsed > 2.f && detonateSample );
            else if ( smokeOrDecoy )
                detonate = detonateSample &&
                    ( velocity.x * velocity.x + velocity.y * velocity.y ) < 0.01f;

            if ( detonate )
            {
                next.detonates = true;
                if ( heGrenade && g_config.settings.misc.grenade_prediction.damage_indicator )
                    next.expectedHeDamage = EstimateVisibleHeDamage(
                        position, local, trace, next.expectedHeTarget );
                break;
            }

            // Only non-timed projectiles may end merely because they came to
            // rest. HE/flash must remain at rest until their fuse check occurs.
            if ( !heGrenade && !flashGrenade && !fireGrenade && !smokeOrDecoy &&
                 DotProduct( velocity, velocity ) < 0.0001f )
                break;
        }

        // Publish world data only; Surface projects against its current HUD camera.
        std::lock_guard<std::mutex> lock( s_grenadePredictionMutex );
        s_grenadePredictionCache = next;
    }

    void ProjectGrenadeTrajectory()
    {
        std::lock_guard<std::mutex> lock( s_grenadePredictionMutex );
        auto& c = s_grenadePredictionCache;
        for ( int i = 0; i < c.count; ++i )
            c.projected[i] = SDK::WorldToScreen( c.points[i], c.screen[i] );
    }

    void DrawGrenadeTrajectory()
    {
        if ( !g_config.settings.misc.grenade_prediction.enabled ||
             !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
            return;

        GrenadePredictionCache cache{};
        {
            std::lock_guard<std::mutex> lock( s_grenadePredictionMutex );
            cache = s_grenadePredictionCache;
        }
        if ( !cache.active || cache.count < 2 )
            return;

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImU32 pathColor = IM_COL32( 255, 255, 255, 220 );
        const ImU32 bounceColor = IM_COL32( 255, 255, 255, 255 );
        for ( int i = 1; i < cache.count; ++i )
        {
            const Vec2& a = cache.screen[i - 1];
            const Vec2& b = cache.screen[i];
            if ( cache.projected[i - 1] && cache.projected[i] )
                draw->AddLine( ImVec2( a.x, a.y ), ImVec2( b.x, b.y ), pathColor, 1.f );

            if ( cache.bounced[i] && cache.projected[i] )
                draw->AddCircleFilled( ImVec2( b.x, b.y ), 2.5f, bounceColor );
        }

        const Vec2& endpoint = cache.screen[cache.count - 1];
        if ( cache.projected[cache.count - 1] )
        {
            const ImU32 endpointColor = cache.detonates
                ? IM_COL32( 255, 135, 70, 245 ) : pathColor;
            char endpointText[48]{};
            if ( cache.detonates && cache.expectedHeDamage > 0 )
                sprintf_s( endpointText, "HE | %d DMG", cache.expectedHeDamage );
            else
                strcpy_s( endpointText, cache.detonates ? "DETONATE" : "LAND" );
            draw->AddCircle( ImVec2( endpoint.x, endpoint.y ), 5.f, endpointColor, 16, 1.f );
            draw->AddText( ImVec2( endpoint.x + 8.f, endpoint.y - 8.f ), endpointColor, endpointText );
        }
    }

    // ------------------------------------------------------------------
    //  In-flight grenade warning: finds thrown projectiles, estimates their
    //  velocity from position deltas and simulates the rest of the path.
    // ------------------------------------------------------------------
    constexpr int kWarnMaxGrenades = 8;
    constexpr int kWarnMaxPoints = 256;

    enum class WarnKind : unsigned char { He, Flash, Smoke, Fire, Decoy, Unknown };

    struct WarnPath
    {
        unsigned long long identity = 0; // lifetime identity, never a render-thread entity dereference
        WarnKind kind = WarnKind::He;
        Vec3 points[kWarnMaxPoints]{};
        Vec2 screen[kWarnMaxPoints]{};
        bool projected[kWarnMaxPoints]{};
        int count = 0;
        float timeLeft = -1.f; // seconds to detonation (if known)
        int damage = 0;         // HE estimate on local player
        bool danger = false;    // local within effect radius
        Vec2 edgeDirection{0.f,-1.f}; // game-thread camera-relative bearing
    };

    struct WarnTrack
    {
        void* ent = nullptr;
        unsigned long long identity = 0;
        Vec3 lastPos{};
        float lastTime = 0.f;
        float firstSeen = 0.f;
        Vec3 vel{};
        bool hasVel = false;
        WarnPath forecast{};
        bool hasForecast = false;
    };

    // CSSO client: DT_BaseEntity m_vecOrigin RecvProp -> m_vecNetworkOrigin.
    // Confirmed at client+0x114420 (r8d=0x520); simulation time at +0x1143C9.
    constexpr uintptr_t kWarningNetworkOrigin = 0x520;

    class WarningTraceFilter : public SDK::CTraceFilterSimple
    {
        void* local_;
    public:
        WarningTraceFilter(void* projectile,void* local)
            : SDK::CTraceFilterSimple(projectile),local_(local) {}
        bool ShouldHitEntity(void* entity,int mask) override
        { return entity!=local_ && SDK::CTraceFilterSimple::ShouldHitEntity(entity,mask); }
    };

    std::mutex s_warnMutex;
    WarnPath s_warnPaths[kWarnMaxGrenades];
    int s_warnCount = 0;
    WarnTrack s_warnTracks[2049];
    unsigned long long s_nextWarnIdentity = 0;

    bool ValidWarningVelocity(const Vec3& velocity)
    {
        return std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z) &&
            DotProduct(velocity,velocity)<25000000.f;
    }

    bool ClassifyThrown( const char* path, WarnKind& kind, const char* networkClass = nullptr )
    {
        const bool projectile = networkClass &&
            (strstr(networkClass,"GrenadeProjectile") || strstr(networkClass,"DecoyProjectile") ||
             strstr(networkClass,"MolotovProjectile"));
        if (!projectile && (!path || !(strstr(path,"thrown") || strstr(path,"projectile")))) return false;
        // Confirmed network projectile classes are authoritative. Their model
        // need not contain "thrown"; dropped inventory grenades stay excluded.
        const char* cls = networkClass ? networkClass : "";
        if (strstr(cls,"Smoke")) {kind=WarnKind::Smoke;return true;}
        if (strstr(cls,"Molotov") || strstr(cls,"Incendiary")) {kind=WarnKind::Fire;return true;}
        if (strstr(cls,"Decoy")) {kind=WarnKind::Decoy;return true;}
        if (!path) {kind=WarnKind::Unknown;return projectile;}
        if ( strstr( path, "flash" ) ) kind = WarnKind::Flash;
        else if ( strstr( path, "smoke" ) ) kind = WarnKind::Smoke;
        else if ( strstr( path, "molotov" ) || strstr( path, "incend" ) ) kind = WarnKind::Fire;
        else if ( strstr( path, "decoy" ) ) kind = WarnKind::Decoy;
        else if (strstr(path,"frag") || strstr(path,"hegrenade")) kind = WarnKind::He;
        else kind = WarnKind::Unknown;
        return true;
    }

    void UpdateGrenadeWarning( SDK::C_BaseEntity* local, float cameraYaw = 0.f )
    {
        const auto& gp = g_config.settings.misc.grenade_prediction;
        WarnPath out[kWarnMaxGrenades];
        int outCount = 0;
        auto* trace = SDK::g_pEngineTrace ? SDK::g_pEngineTrace : SDK::IEngineTrace::Get();
        if ( !gp.warning || !local || !trace || !SDK::g_pEntityList || !SDK::g_pEngine )
        {
            std::lock_guard<std::mutex> lock( s_warnMutex );
            s_warnCount = 0;
            return;
        }

        const Vec3 me = local->GetAbsOrigin() + local->GetViewOffset() * 0.5f;

        for ( int i = 1; i <= 2048 && outCount < kWarnMaxGrenades; ++i )
        {
            void* ent = SDK::g_pEntityList->GetClientEntity( i );
            WarnTrack& tr = s_warnTracks[i];
            if ( !ent || SurfaceRenderer::IsEntityDormant( ent ) )
            {
                tr = {};
                continue;
            }
            WarnKind kind;
            const char* networkClass = SDK::WorldEntity::ClassName(ent);
            // Avoid probing every world model once a non-projectile class is known.
            if (networkClass && !strstr(networkClass,"GrenadeProjectile") &&
                !strstr(networkClass,"DecoyProjectile") && !strstr(networkClass,"MolotovProjectile"))
            {tr={};continue;}
            if ( !ClassifyThrown( SurfaceRenderer::GetWorldModelPath( ent ), kind,
                    networkClass) )
            {
                tr = {};
                continue;
            }

            Vec3 pos{};
            float now=0.f;
            if(!SDK::WorldEntity::Read(ent,kWarningNetworkOrigin,pos) ||
               !SDK::WorldEntity::Read(ent,SDK::nSimulationTimeOffset,now) ||
               !std::isfinite(now) || now<=0.f ||
               !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
                continue; // Never mix render origin with an unrelated local clock.
            // Network fields are independent of the local prediction/render clock.
            Vec3 nativeVelocity{};
            if(!SDK::WorldEntity::Read(ent,SDK::nVelocityOffset,nativeVelocity) ||
               !ValidWarningVelocity(nativeVelocity))continue;
            const bool newSample=tr.ent!=ent || now!=tr.lastTime ||
                DotProduct(pos-tr.lastPos,pos-tr.lastPos)>0.0001f ||
                DotProduct(nativeVelocity-tr.vel,nativeVelocity-tr.vel)>0.0001f;
            if (kind == WarnKind::Smoke)
            {
                bool deployed=false;
                // CWeaponSmokeGrenadeProjectile m_bDidSmokeEffect, client+0x2E05E.
                SDK::WorldEntity::Read(ent,0xD28,deployed);
                if (deployed) {tr={};continue;}
            }
            if ( tr.ent != ent || now < tr.lastTime )
            {
                tr = {};
                tr.ent = ent;
                tr.identity = ++s_nextWarnIdentity;
                tr.firstSeen = now;
                tr.lastPos = pos;
                tr.lastTime = now;
            }
            // DT_BaseGrenade m_vecVelocity -> RecvProxy_LocalVelocity.
            // CSSO client+0xA269 declares offset 0x148; proxy +0x11DDB0
            // writes all three CURRENT components. It is not initial velocity.
            tr.vel=nativeVelocity;
            tr.hasVel=true;
            tr.lastPos=pos;
            tr.lastTime=now;

            WarnPath& w = out[outCount];
            w = {};
            w.identity = tr.identity;
            w.kind = kind;
            w.points[0] = pos;
            w.count = 1;

            if(!newSample && tr.hasForecast)
                w=tr.forecast;
            else
            {
                WarningTraceFilter filter(ent,local);
                Vec3 p = pos, v = tr.vel;
                const float age = now - tr.firstSeen;
                float fuse = -1.f;
                if ( kind == WarnKind::He || kind == WarnKind::Flash ) fuse = 1.5f - age;
                else if ( kind == WarnKind::Fire )  fuse = 2.0f - age;
                else if ( kind == WarnKind::Smoke )  fuse = 3.0f - age;  // ~3 s to deploy
                else if ( kind == WarnKind::Decoy )  fuse = 3.0f - age;  // ~3 s to first beep

                if (fuse < 0.f && (kind==WarnKind::He || kind==WarnKind::Flash || kind==WarnKind::Fire))
                    fuse=0.f;
                for ( int t = 1; t < kWarnMaxPoints; ++t )
                {
                    const float nz = v.z - kGrenadeGravity * kGrenadeStep;
                    const Vec3 move{ v.x * kGrenadeStep, v.y * kGrenadeStep, ( v.z + nz ) * 0.5f * kGrenadeStep };
                    v.z = nz;
                    SDK::Ray_t ray{};
                    ray.m_Start = p;
                    ray.m_Delta = move;
                    ray.m_Extents = { 2.f, 2.f, 2.f };
                    ray.m_IsRay = false;
                    ray.m_IsSwept = DotProduct( move, move ) > 0.0001f;
                    SDK::CGameTrace col{};
                    bool hit = false;
                    if ( ray.m_IsSwept )
                    {
                        trace->TraceRay( ray, SDK::MASK_SOLID, &filter, &col );
                        hit = col.fraction < 1.f && !col.allsolid;
                        p = col.endpos;
                    }
                    if ( hit )
                    {
                        const float nv = DotProduct( v, col.plane.normal );
                        v -= col.plane.normal * ( 2.f * nv );
                        v = v * kGrenadeElasticity;
                        if (DotProduct(v,v)<400.f)v={};
                        if ( kind == WarnKind::Fire && col.plane.normal.z > 0.7f )
                        {
                            w.points[w.count++] = p;
                            break;
                        }
                        // Consume the remaining tick after a floor bounce, as
                        // PhysicsPushEntity does. Do not throw away flight time.
                        if(col.plane.normal.z>0.7f && DotProduct(v,v)>0.f)
                        {
                            SDK::Ray_t remainder{};
                            remainder.m_Start=p;
                            remainder.m_Delta=v*((1.f-col.fraction)*kGrenadeStep);
                            remainder.m_Extents={2.f,2.f,2.f};
                            remainder.m_IsRay=false;
                            remainder.m_IsSwept=DotProduct(remainder.m_Delta,remainder.m_Delta)>0.0001f;
                            if(remainder.m_IsSwept)
                            {
                                SDK::CGameTrace result{};
                                trace->TraceRay(remainder,SDK::MASK_SOLID,&filter,&result);
                                if(!result.allsolid)p=result.endpos;
                            }
                        }
                    }
                    w.points[w.count++] = p;
                    const float el = t * kGrenadeStep;
                    if ( fuse >= 0.f && el >= fuse ) break;
                    if ( fuse < 0.f && DotProduct( v, v ) < 0.01f ) break; // smoke/decoy rest
                }
                w.timeLeft = fuse >= 0.f ? fuse : -1.f;
                tr.forecast=w;
                tr.hasForecast=true;
            }
            // Camera bearing and distance/damage may change on every command;
            // the world-space trajectory only changes on a new network sample.
            w.damage=0;
            w.danger=false;

            const Vec3 end = w.points[w.count - 1];
            const float bearing=atan2f(end.y-me.y,end.x-me.x)-cameraYaw*0.01745329252f;
            w.edgeDirection={-sinf(bearing),-cosf(bearing)};
            const Vec3 d = me - end;
            const float dist = sqrtf( DotProduct( d, d ) );
            if ( kind == WarnKind::He )
            {
                w.danger = dist < 350.f;
                if ( w.danger )
                {
                    float dmg = 100.f - dist * ( 100.f / 350.f );
                    if ( local->GetArmor() > 0 ) dmg *= 1.475f * 0.5f;
                    w.damage = static_cast<int>( dmg + 0.5f );
                }
            }
            else if ( kind == WarnKind::Fire ) w.danger = dist < 150.f;
            else if ( kind == WarnKind::Flash ) w.danger = dist < 600.f;

            ++outCount;
        }

        std::lock_guard<std::mutex> lock( s_warnMutex );
        for ( int k = 0; k < outCount; ++k )
            s_warnPaths[k] = out[k];
        s_warnCount = outCount;
    }

    Vec2 WarningFrameDirection(const Vec3& point,Vec2 fallback)
    {
        // Off-screen bearing must use the same render camera as the path,
        // not the CreateMove camera/eye captured one or more ticks earlier.
        if (!SDK::g_worldProjection.active || !SDK::g_worldProjection.valid) return fallback;
        const auto clip=SDK::ToProjectionClip(point);
        const float x=clip.x*SDK::g_worldProjection.width;
        const float y=-clip.y*SDK::g_worldProjection.height;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(clip.w)) return fallback;
        const float length=sqrtf(x*x+y*y);
        if (!std::isfinite(length)) return fallback;
        if (length<.0001f) return clip.w<0.f ? Vec2{0.f,1.f} : fallback;
        return {x/length,y/length};
    }

    void ProjectGrenadeWarning()
    {
        std::lock_guard<std::mutex> lock( s_warnMutex );
        for ( int g = 0; g < s_warnCount; ++g )
        {
            auto& w = s_warnPaths[g];
            for ( int k = 0; k < w.count; ++k )
                w.projected[k] = SDK::WorldToScreen( w.points[k], w.screen[k] );
            if (w.count>0)
                w.edgeDirection=WarningFrameDirection(w.points[w.count-1],w.edgeDirection);
        }
    }

    struct WarningPlacement { ImVec2 center; bool offscreen; };

    WarningPlacement PlaceWarning(const WarnPath& w, ImVec2 display,
        bool previousOffscreen=false,bool hasPrevious=false)
    {
        const float margin=hasPrevious ? (previousOffscreen ? 68.f : 44.f) : 54.f;
        const int last=w.count-1;
        if (last>=0 && w.projected[last] &&
            w.screen[last].x>=margin && w.screen[last].x<=display.x-margin &&
            w.screen[last].y>=margin && w.screen[last].y<=display.y-margin)
            return {{w.screen[last].x,w.screen[last].y},false};
        const float hx=(std::max)(1.f,display.x*.5f-54.f);
        const float hy=(std::max)(1.f,display.y*.5f-54.f);
        const float dx=w.edgeDirection.x,dy=w.edgeDirection.y;
        const float sx=fabsf(dx)>.0001f ? hx/fabsf(dx) : 1e8f;
        const float sy=fabsf(dy)>.0001f ? hy/fabsf(dy) : 1e8f;
        const float scale=(std::min)(sx,sy);
        return {{display.x*.5f+dx*scale,display.y*.5f+dy*scale},true};
    }

    void WarningText(ImDrawList* draw,ImVec2 center,float y,const char* text,ImU32 color,float size)
    {
        ImFont* font=ImGui::GetFont();
        const ImVec2 bounds=font->CalcTextSizeA(size,FLT_MAX,0.f,text);
        const ImVec2 p{center.x-bounds.x*.5f,y};
        draw->AddText(font,size,{p.x+1.f,p.y+1.f},IM_COL32(0,0,0,210),text);
        draw->AddText(font,size,p,color,text);
    }

    void DrawWarningBadge(ImDrawList* draw,const WarnPath& w,ImVec2 center,bool timers,float displaySeconds)
    {
        constexpr float radius=20.f;
        const ImU32 accent=w.danger ? IM_COL32(239,83,90,255) :
            (w.kind==WarnKind::Fire ? IM_COL32(231,174,92,255) : IM_COL32(213,216,215,245));
        draw->AddCircleFilled({center.x,center.y+2.f},radius+2.f,IM_COL32(0,0,0,75),48);
        draw->AddCircleFilled(center,radius,IM_COL32(20,21,23,235),48);
        draw->AddCircle(center,radius,IM_COL32(7,8,10,230),48,1.f);
        draw->AddCircle(center,radius-2.f,
            (timers && displaySeconds>=0.f) ? IM_COL32(63,65,69,170) : accent,48,2.f);
        const float total=(w.kind==WarnKind::Smoke || w.kind==WarnKind::Decoy) ? 3.f :
            (w.kind==WarnKind::Fire ? 2.f : 1.5f);
        if (timers && displaySeconds>=0.f)
        {
            const float frac=(std::clamp)(displaySeconds/total,0.f,1.f);
            if (frac>.001f)
            {
                draw->PathArcTo(center,radius-2.f,-1.570796327f,-1.570796327f+6.283185307f*frac,48);
                draw->PathStroke(accent,0,2.f);
            }
        }
        const char* weapon="weapon_hegrenade";
        switch(w.kind)
        {
        case WarnKind::Flash:weapon="weapon_flashbang";break;
        case WarnKind::Smoke:weapon="weapon_smokegrenade";break;
        case WarnKind::Fire:weapon="weapon_molotov";break;
        case WarnKind::Decoy:weapon="weapon_decoy";break;
        default:break;
        }
        ImFont* iconFont=ESPDraw::WeaponIconFont();
        const uint16_t glyph=w.kind==WarnKind::Unknown ? 0 : ESPDraw::WeaponIconCodepoint(weapon);
        const bool hasTimer=timers && displaySeconds>=0.f;
        const float iconSize=hasTimer ? 21.f : 25.f;
        if(iconFont && glyph>0 && glyph<128 && iconFont->IsGlyphInFont(glyph))
        {
            const char icon[2]={static_cast<char>(glyph),0};
            const ImVec2 bounds=iconFont->CalcTextSizeA(iconSize,FLT_MAX,0.f,icon);
            draw->AddText(iconFont,iconSize,{center.x-bounds.x*.5f,
                center.y-bounds.y*.5f-(hasTimer?4.f:0.f)},IM_COL32(237,238,239,255),icon);
        }
        else
            WarningText(draw,center,center.y-(hasTimer?12.f:7.f),"!",IM_COL32(237,238,239,255),16.f);
        if(hasTimer)
        {
            char text[16];sprintf_s(text,"~%.1f",displaySeconds);
            WarningText(draw,center,center.y+6.f,text,IM_COL32(206,208,210,245),10.f);
        }
        if(w.kind==WarnKind::He && w.damage>0)
        {
            char text[24];sprintf_s(text,"-%d HP",w.damage);
            WarningText(draw,center,center.y+25.f,text,accent,11.f);
        }
    }

    ImVec2 SeparateWarnings(ImVec2 anchor,ImVec2 tangent,ImVec2 display,const ImVec2* placed,int count)
    {
        const auto free=[&](ImVec2 p)
        {
            for(int i=0;i<count;++i)
            {const float dx=p.x-placed[i].x,dy=p.y-placed[i].y;if(dx*dx+dy*dy<58.f*58.f)return false;}
            return true;
        };
        if(free(anchor))return anchor;
        for(int attempt=0;attempt<16;++attempt)
        {
            const float shift=64.f*(attempt/2+1)*(attempt%2 ? -1.f : 1.f);
            const ImVec2 p{(std::clamp)(anchor.x+tangent.x*shift,54.f,display.x-54.f),
                (std::clamp)(anchor.y+tangent.y*shift,54.f,display.y-54.f)};
            if(free(p))return p;
        }
        // Packed edge: choose the closest available padded grid location.
        // Never put eight badges on top of each other at the same bearing.
        ImVec2 best{-1.f,-1.f};float distance=FLT_MAX;
        for(float y=54.f;y<=display.y-54.f;y+=64.f)
            for(float x=54.f;x<=display.x-54.f;x+=64.f)
            {
                const ImVec2 p{x,y};if(!free(p))continue;
                const float dx=x-anchor.x,dy=y-anchor.y,d=dx*dx+dy*dy;
                if(d<distance){distance=d;best=p;}
            }
        return best;
    }

    struct WarningVisual
    {
        unsigned long long identity=0;
        int frame=-1;
        bool ready=false,offscreen=false;
        ImVec2 center{},display{},direction{0.f,-1.f};
        float seconds=-1.f;
    };
    WarningVisual s_warningVisuals[16];

    WarningVisual& WarningVisualFor(unsigned long long identity,int frame)
    {
        WarningVisual* oldest=&s_warningVisuals[0];
        for(auto& state:s_warningVisuals)
        {
            if(state.identity==identity)return state;
            if(!state.identity || state.frame<oldest->frame)oldest=&state;
        }
        *oldest={};oldest->identity=identity;oldest->frame=frame;return *oldest;
    }

    ImVec2 SmoothWarningCenter(WarningVisual& state,ImVec2 target,ImVec2 display,float dt,int frame,
                              bool worldAnchored=false)
    {
        const bool resized=state.display.x!=display.x || state.display.y!=display.y;
        const float dx=target.x-state.center.x,dy=target.y-state.center.y;
        const float snapDistance=(std::max)(display.x,display.y)*.6f;
        // An on-screen world marker must follow the CURRENT camera exactly.
        // Pixel interpolation mixes projections from different camera positions
        // and makes a stationary grenade swim whenever the player moves.
        if(worldAnchored || !state.ready || resized || frame-state.frame>2 || dx*dx+dy*dy>snapDistance*snapDistance)
            state.center=target;
        else
        {
            // Exponential response is equivalent across 30/60/144+ FPS.
            const float alpha=1.f-expf(-(std::clamp)(dt,0.f,.1f)/.07f);
            state.center.x+=dx*alpha;state.center.y+=dy*alpha;
        }
        state.ready=true;state.display=display;state.frame=frame;
        return state.center;
    }

    void DrawGrenadeWarning()
    {
        const auto& gp=g_config.settings.misc.grenade_prediction;
        if(!gp.warning || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame())
        {for(auto& state:s_warningVisuals)state={};return;}
        std::lock_guard<std::mutex> lock(s_warnMutex);
        ImDrawList* draw=ImGui::GetForegroundDrawList();
        const ImVec2 display=ImGui::GetIO().DisplaySize;
        if(display.x<128.f || display.y<128.f)return;
        ImVec2 placed[kWarnMaxGrenades]{};int placedCount=0;
        for(int g=0;g<s_warnCount;++g)
        {
            const auto& w=s_warnPaths[g];if(w.count<2)continue;
            // Fine, muted trajectory. The warning badge, not the path, owns emphasis.
            for(int k=1;k<w.count;++k)
                if(w.projected[k-1] && w.projected[k])
                    draw->AddLine({w.screen[k-1].x,w.screen[k-1].y},{w.screen[k].x,w.screen[k].y},
                        w.danger ? IM_COL32(208,114,119,115) : IM_COL32(198,200,202,100),1.f);
            const int frame=ImGui::GetFrameCount();
            const float dt=ImGui::GetIO().DeltaTime;
            auto& visual=WarningVisualFor(w.identity ? w.identity : static_cast<unsigned long long>(g+1),frame);
            auto placement=PlaceWarning(w,display,visual.offscreen,visual.ready);
            const float directionAlpha=1.f-expf(-(std::clamp)(dt,0.f,.1f)/.055f);
            if(!visual.ready)visual.direction={w.edgeDirection.x,w.edgeDirection.y};
            else
            {
                const float oldAngle=atan2f(visual.direction.y,visual.direction.x);
                float change=atan2f(w.edgeDirection.y,w.edgeDirection.x)-oldAngle;
                while(change>3.141592654f)change-=6.283185307f;
                while(change<-3.141592654f)change+=6.283185307f;
                const float angle=oldAngle+change*directionAlpha;
                visual.direction={cosf(angle),sinf(angle)};
            }
            if(placement.offscreen)
            {
                const float sx=fabsf(visual.direction.x)>.0001f ? (display.x*.5f-54.f)/fabsf(visual.direction.x) : 1e8f;
                const float sy=fabsf(visual.direction.y)>.0001f ? (display.y*.5f-54.f)/fabsf(visual.direction.y) : 1e8f;
                const float scale=(std::min)(sx,sy);
                placement.center={display.x*.5f+visual.direction.x*scale,display.y*.5f+visual.direction.y*scale};
            }
            const ImVec2 anchor=placement.center;
            const ImVec2 tangent=placement.offscreen ? ImVec2(-w.edgeDirection.y,w.edgeDirection.x) : ImVec2(1.f,0.f);
            placement.center=SeparateWarnings(anchor,tangent,display,placed,placedCount);
            if(placement.center.x<0.f)continue; // too small a viewport for another badge
            placement.center=SmoothWarningCenter(visual,placement.center,display,dt,frame,!placement.offscreen);
            // Recheck spacing after interpolation; close warnings must not overlap.
            const ImVec2 spaced=SeparateWarnings(placement.center,tangent,display,placed,placedCount);
            if(spaced.x<0.f)continue;
            placement.center=spaced;visual.center=spaced;visual.offscreen=placement.offscreen;
            placed[placedCount++]=placement.center;
            if(placement.offscreen)
            {
                const ImVec2 d=visual.direction,p(-d.y,d.x),c=placement.center;
                const ImU32 color=w.danger ? IM_COL32(239,83,90,235) : IM_COL32(211,214,213,210);
                draw->AddTriangleFilled({c.x+d.x*31.f,c.y+d.y*31.f},
                    {c.x+d.x*24.f+p.x*4.f,c.y+d.y*24.f+p.y*4.f},
                    {c.x+d.x*24.f-p.x*4.f,c.y+d.y*24.f-p.y*4.f},color);
            }
            else if(fabsf(placement.center.x-anchor.x)>1.f || fabsf(placement.center.y-anchor.y)>1.f)
                draw->AddLine(anchor,placement.center,IM_COL32(160,162,165,100),1.f);
            if(w.timeLeft<0.f || visual.seconds<0.f || w.timeLeft<=0.f)visual.seconds=w.timeLeft;
            else visual.seconds+=(w.timeLeft-visual.seconds)*(1.f-expf(-(std::clamp)(dt,0.f,.1f)/.04f));
            DrawWarningBadge(draw,w,placement.center,gp.timers,visual.seconds);
        }
    }

}

void Grenades::CopySurfaceGrenadeFrame(SurfaceGrenadeFrame& frame)
{
    static_assert(kWarnMaxGrenades==kSurfaceWarningMax && kWarnMaxPoints==kSurfaceWarningPoints);
    static_assert(kGrenadePredictionMaxPoints==kSurfaceTrajectoryPoints);
    frame = {};
    const auto& config = g_config.settings.misc.grenade_prediction;
    frame.warningsEnabled = config.warning;
    frame.timers = config.timers;
    frame.trajectoryEnabled = config.enabled;
    if (frame.warningsEnabled)
    {
        std::lock_guard<std::mutex> lock(s_warnMutex);
        frame.warningCount = (std::clamp)(s_warnCount,0,kSurfaceWarningMax);
        for (int i=0;i<frame.warningCount;++i)
        {
            const auto& source = s_warnPaths[i];
            auto& dest = frame.warnings[i];
            dest.identity = source.identity;
            dest.count = (std::clamp)(source.count,0,kSurfaceWarningPoints);
            dest.kind = static_cast<int>(source.kind);
            dest.damage = source.damage; dest.seconds = source.timeLeft;
            dest.danger = source.danger; dest.edgeDirection = source.edgeDirection;
            for (int k=0;k<dest.count;++k) dest.points[k]=source.points[k];
        }
    }
    if (frame.trajectoryEnabled)
    {
        std::lock_guard<std::mutex> lock(s_grenadePredictionMutex);
        const auto& source=s_grenadePredictionCache;
        if (source.active)
        {
            frame.trajectoryCount=(std::clamp)(source.count,0,kSurfaceTrajectoryPoints);
            frame.detonates=source.detonates;
            frame.trajectoryDamage=source.expectedHeDamage;
            for (int k=0;k<frame.trajectoryCount;++k)
            {frame.trajectory[k]=source.points[k];frame.bounced[k]=source.bounced[k];}
        }
    }
}

void Grenades::UpdateThrownGrenades()
{
    // Called by the verified CreateMove detour after its original. The old
    // FrameStageNotify visual cache is not live on this CSSO build, so it must
    // never be the sole producer. All forecast samples are network-clocked.
    if(!SDK::g_pEngine || !SDK::g_pEntityList ||
       !SDK::g_pEngine->IsInGame() || !SDK::g_pEngine->IsConnected())
    {
        for(auto& track:s_warnTracks)track={};
        std::lock_guard<std::mutex> lock(s_warnMutex);
        s_warnCount=0;
        return;
    }
    auto* local=reinterpret_cast<SDK::C_BaseEntity*>(SDK::g_pEntityList->GetClientEntity(
        SDK::g_pEngine->GetLocalPlayer()));
    UpdateGrenadeWarning(local);
}


void Grenades::OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
{
    SimulateGrenadeTrajectory(cmd,local);
    UpdateThrownGrenades();
}
