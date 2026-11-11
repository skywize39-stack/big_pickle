#include "ui/binds/KeyBinds.hpp"
#include "core/events/EventLog.hpp"
#include "features/combat/rage/Ragebot.hpp"
#include "features/combat/aimbot/Penetration.hpp"
#include "features/combat/aimbot/LagRecord.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include "features/combat/rage/AutoStop.hpp"
#include "features/combat/rage/LagCompensation.hpp"

#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <cstring>
#include <cstdio>

using namespace SDK;

// ------------------------------------------------------------------
//  Debug console output (autostop diagnostics)
// ------------------------------------------------------------------
// Rate-limits the console spam: returns true once every `everyN` calls.
static bool DebugTick(int everyN)
{
    static long s_debugTick = 0;
    return ((++s_debugTick) % everyN) == 0;
}

static float DebugSpeed(const Vec3& v)
{
    return sqrtf(v.x * v.x + v.y * v.y);
}

// Current-command AutoStop request. It is armed only for a valid Rage candidate
// and cleared before every command; a committed shot never receives a second
// braking command.
static bool  s_pendingAutoStop = false;
// Last command that armed the brake (carried over forced-release ticks).
static int   s_lastStopCmd = 0;
static float s_lastStopMaxSpeed = 0.f;
static bool s_lastStopFull = false;
static bool s_autoStopFull = false;
static float s_autoStopMaxSpeed = 0.f;

// True for the last executed Rage command that selected a final target.
static bool  s_hasTargetTick = false;

// A raw IN_ATTACK is not proof that rage has a valid shot: it can be the user
// holding fire while peek movement, scope, cooldown, HC or wallbang gates are
// unresolved. Keep one atomic commit for the current command; DT consumes its
// sequence token instead of guessing from a button bit.
struct RageFireCommit
{
    int  commandNumber = -1;
    int  targetIndex = 0;
    int  hitbox = -1;
    bool fromRecord = false;
    bool penetrated = false;
    float simtime = 0.f;
    float damage = 0.f;
    float hitchance = 0.f;
    // Current-command aimpoint and final aim. A DT follow-up produces a new
    // commit through this same pipeline, never an offset from an older shot.
    Vec3 pos{};
    Vec3 aim{};
};
static RageFireCommit s_rageFireCommit{};

// Engine time of the last committed rage shot (see GetLastRageFireTime).
static float s_lastRageFireTime = -1000.f;

// Current-command state of the active weapon profile's Min Damage Hold bind.
// It never mutates profile settings; each RunRage resolves it again from input.
static bool s_minDamageOverrideActive = false;
static bool s_minDamageOverrideShowIndicator = false;
static int  s_minDamageOverrideValue = 0;
static bool s_minDamageOverrideToggleActive = false;
static bool s_minDamageOverrideKeyWasDown = false;
static int  s_minDamageOverrideToggleProfile = -1;
static int  s_minDamageOverrideToggleKey = 0;

static float EffectiveRageMinDamage( const cfg::rage_weapon_profile_t& profile,
                                     bool penetrated, bool visible )
{
    if ( s_minDamageOverrideActive )
        return static_cast<float>( s_minDamageOverrideValue );
    return ( penetrated && !visible )
        ? profile.wall_min_dmg : static_cast<float>( profile.min_damage );
}
// for the selected rage target and consumed after Rage by counter-velocity move.
static constexpr float kSupremacyStopRatio = 0.34f;
static constexpr float kSupremacyLowBandInputRatio = 0.30f;
static constexpr float kSupremacyCounterInput = 450.f;

// ------------------------------------------------------------------
//  Math helpers
// ------------------------------------------------------------------
static float AngleFOV(float p1, float y1, float p2, float y2)
{
    float dp = p1 - p2;
    float dy = y1 - y2;
    while (dy > 180.f) dy -= 360.f;
    while (dy < -180.f) dy += 360.f;
    return sqrtf(dp * dp + dy * dy);
}

static void ClampAngles(Vec3& a)
{
    while (a.y > 180.f) a.y -= 360.f;
    while (a.y < -180.f) a.y += 360.f;
    if (a.x > 89.f) a.x = 89.f;
    if (a.x < -89.f) a.x = -89.f;
    a.z = 0.f;
}

static Vec3 CalcAngle(const Vec3& src, const Vec3& dst)
{
    Vec3 delta = dst - src;
    float hyp = sqrtf(delta.x * delta.x + delta.y * delta.y);
    Vec3 ang;
    ang.x = atan2f(-delta.z, hyp) * (180.f / 3.14159265f);
    ang.y = atan2f(delta.y, delta.x) * (180.f / 3.14159265f);
    ang.z = 0.f;
    return ang;
}

static Vec3 AngleVectors(const Vec3& angles)
{
    float sp = sinf(angles.x * 3.14159265f / 180.f);
    float cp = cosf(angles.x * 3.14159265f / 180.f);
    float sy = sinf(angles.y * 3.14159265f / 180.f);
    float cy = cosf(angles.y * 3.14159265f / 180.f);
    return Vec3{ cp * cy, cp * sy, -sp };
}

static void CorrectMovement(CUserCmd* cmd, const Vec3& oldAngles, const Vec3& newAngles)
{
    float deltaView = newAngles.y - oldAngles.y;

    float curForward = cmd->forwardmove.Decode();
    float curSide = cmd->sidemove.Decode();
    float moveSpeed = sqrtf(curForward * curForward + curSide * curSide);

    if (moveSpeed < 1.f)
        return;

    float moveYaw = atan2f(-curSide, curForward) * (180.f / 3.14159265f);
    float correctedYaw = moveYaw - deltaView;
    float correctedYawRad = correctedYaw * (3.14159265f / 180.f);

    cmd->forwardmove.Encode(cosf(correctedYawRad) * moveSpeed);
    cmd->sidemove.Encode(-sinf(correctedYawRad) * moveSpeed);
}

struct AimPoint
{
    Vec3 pos{};
    int  hitbox = -1;
};

struct HitboxArrays
{
    const int8_t* groups;
    const int8_t* studio_indices;
    const Vec3* centers;
    const Vec3* model_mins;
    const Vec3* model_maxs;
    const Vec3* world_mins;
    const Vec3* world_maxs;
    const float (*matrices)[12];
    const float* radii;
    int           num;

    HitboxInfo makeInfo(int hb) const
    {
        HitboxInfo info;
        info.index = studio_indices ? studio_indices[hb] : -1;
        info.group = groups[hb];
        info.center = centers[hb];
        info.model_min = model_mins[hb];
        info.model_max = model_maxs[hb];
        info.world_min = world_mins[hb];
        info.world_max = world_maxs[hb];
        info.radius = radii[hb];
        if (matrices)
            memcpy(info.matrix, matrices[hb], sizeof(info.matrix));
        return info;
    }
};

static bool SetupHitboxPoints(const HitboxInfo& hb, int arrayIndex,
    float scale, float bscale, bool inAir,
    const cfg::rage_weapon_profile_t& profile,
    std::vector<AimPoint>& out)
{
    out.clear();
    if (hb.index < 0)
        return false;

    if (inAir)
        scale = 0.7f;

    const Vec3& mins = hb.model_min;
    const Vec3& maxs = hb.model_max;

    auto transform = [&](const Vec3& p) -> Vec3
        {
            const float* m = hb.matrix;
            return { m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                     m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                     m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11] };
        };

    if (hb.radius <= 0.f)
    {
        const Vec3 center = (mins + maxs) * 0.5f;
        if (scale <= 0.001f && bscale <= 0.001f)
        {
            out.push_back({ center, arrayIndex });
            for (auto& p : out)
                p.pos = transform(p.pos);
            return true;
        }

        if (hb.index == HITBOX_R_FOOT || hb.index == HITBOX_L_FOOT)
        {

            // side is more optimal than the center; inverted for the left foot.
            float d1 = (mins.z - center.z) * 0.875f;
            if (hb.index == HITBOX_L_FOOT)
                d1 *= -1.f;
            out.push_back({ center + Vec3{ 0.f, 0.f, d1 }, arrayIndex });

            if (profile.multipoint_feet)
            {
                const float d2 = (mins.x - center.x) * scale;  // heel
                const float d3 = (maxs.x - center.x) * scale;  // toe
                out.push_back({ center + Vec3{ d2, 0.f, 0.f }, arrayIndex });
                out.push_back({ center + Vec3{ d3, 0.f, 0.f }, arrayIndex });
            }
        }

        if (out.empty())
            out.push_back({ center, arrayIndex });

        for (auto& p : out)
            p.pos = transform(p.pos);
        return true;
    }

    // Capsule hitboxes.
    const float r = hb.radius * scale;
    const float br = hb.radius * bscale;
    const Vec3  center = (mins + maxs) * 0.5f;

    if (scale <= 0.001f && bscale <= 0.001f)
    {
        out.push_back({ center, arrayIndex });
        for (auto& p : out)
            p.pos = transform(p.pos);
        return true;
    }

    if (hb.index == HITBOX_HEAD)
    {
        // center.
        out.push_back({ center, arrayIndex });

        if (profile.multipoint_head)
        {
            constexpr float rotation = 0.70710678f; // cos(45deg)

            // top/back 45 deg — the best spot to shoot at.
            out.push_back({ { maxs.x + rotation * r, maxs.y - rotation * r, maxs.z }, arrayIndex });
            // right.
            out.push_back({ { maxs.x, maxs.y, maxs.z + r }, arrayIndex });
            // left.
            out.push_back({ { maxs.x, maxs.y, maxs.z - r }, arrayIndex });
            // back.
            out.push_back({ { maxs.x, maxs.y - r, maxs.z }, arrayIndex });
            // bottom point requires the animstate min-pitch check (Pandora)
            // — not available from the snapshot, so it is skipped.
        }
    }
    else if (hb.index == HITBOX_BODY)
    {
        // center.
        out.push_back({ center, arrayIndex });

        // back.
        if (profile.multipoint_stomach)
            out.push_back({ { center.x, maxs.y - br, center.z }, arrayIndex });
    }
    else if (hb.index == HITBOX_PELVIS || hb.index == HITBOX_UPPER_CHEST)
    {
        // back point only.
        out.push_back({ { center.x, maxs.y - r, center.z }, arrayIndex });
    }
    else if (hb.index == HITBOX_THORAX || hb.index == HITBOX_CHEST)
    {
        // center.
        out.push_back({ center, arrayIndex });

        // extra point on the back.
        if (profile.multipoint_chest)
            out.push_back({ { center.x, maxs.y - r, center.z }, arrayIndex });
    }
    else if (hb.index == HITBOX_R_CALF || hb.index == HITBOX_L_CALF)
    {
        // center.
        out.push_back({ center, arrayIndex });

        // half bottom.
        if (profile.multipoint_legs)
            out.push_back({ { maxs.x - hb.radius * 0.5f, maxs.y, maxs.z }, arrayIndex });
    }
    else if (hb.index == HITBOX_R_THIGH || hb.index == HITBOX_L_THIGH)
    {
        // center only.
        out.push_back({ center, arrayIndex });
    }
    else if (hb.index == HITBOX_R_UPPER_ARM || hb.index == HITBOX_L_UPPER_ARM)
    {
        // elbow.
        out.push_back({ { maxs.x + hb.radius, center.y, center.z }, arrayIndex });
    }

    if (out.empty())
        return false;

    // Keep every capsule multipoint strictly INSIDE the capsule. Several
    // Pandora offsets (stomach/chest/pelvis back = maxs.y - r, elbow = maxs.x
    // + radius) assume the CSGO model axis layout; on a differently oriented
    // CSSO capsule they landed on or outside the surface, and even a correct
    // 100% point sits exactly on the edge where any client/server pose
    // difference is a clean miss. Pull points to <= 90% of the radius.
    {
        const Vec3 ab = maxs - mins;
        const float abab = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
        const float maxR = hb.radius * 0.9f;
        for (auto& p : out)
        {
            const Vec3 ap = p.pos - mins;
            float t = abab > 1e-6f ? (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / abab : 0.f;
            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
            const Vec3 q = mins + ab * t;
            const Vec3 d = p.pos - q;
            const float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
            if (dl > maxR && dl > 1e-4f)
                p.pos = q + d * (maxR / dl);
        }
    }

    for (auto& p : out)
        p.pos = transform(p.pos);

    return true;
}

static bool HitgroupEnabled(int g, int effMask)
{
    if (g != HITGROUP_HEAD && g != HITGROUP_CHEST && g != HITGROUP_STOMACH &&
        g != HITGROUP_LEFTARM && g != HITGROUP_RIGHTARM &&
        g != HITGROUP_LEFTLEG && g != HITGROUP_RIGHTLEG)
        return false;
    return (effMask & (1 << g)) != 0;
}

// ------------------------------------------------------------------
//  Point evaluation over a multipoint hitbox source
// ------------------------------------------------------------------
struct PointEval
{
    float damage = 0.f;
    float score = 0.f;
    int   shots = 999;   // выстрелов на килл этой точки (урон-приоритет)
    int   hitbox = -1;   // hitbox array index (record or snapshot)
    int   hitboxGroup = -1;
    float hitboxRadius = 0.f;
    HitboxInfo hb;             // full hitbox data of the winning point
    Vec3  pos{};
    bool  penetrate = false;
    bool  visible = false;
    bool  valid = false;
};

// Damage-based best aimpoint over a hitbox source (a lag record or the live
// snapshot). Priority: fewest shots to kill, then visibility; the hit-chance
// weighting (multipoint boxRel) only ranks candidates inside the same
// shots/visibility tier via the internal_hvh rules (different hitbox must
// beat by +30, the same hitbox needs +10).
static PointEval EvalHitboxSource(void* local, const PlayerSnapshot& ps,
    const HitboxArrays& hbArr,
    const Vec3& eyePos,
    const WeaponData* wepInfo,
    const cfg::rage_weapon_profile_t& profile, int effMask,
    bool inAir)
{
    PointEval best;

    if (hbArr.num <= 0)
        return best;

    // Shots-to-kill prior defined here, since the run-level lambda lives in RunRage.
    auto shotsToKill = []( float damage, float rawHealth ) -> int
        {
            if ( damage <= 0.f || rawHealth <= 0.f )
                return 999;
            const int shots = static_cast<int>( ceilf( rawHealth / damage ) );
            return shots > 0 ? shots : 1;
        };

    // ------------------------------------------------------------------
    //  Hit-chance-aware aimpoint selection (умный выбор хитбокса).
    //  Вероятность попадания (конус рассеивания против углового размера
    //  хитбокса) взвешивает кандидатов ТОЛЬКО внутри одного приоритета
    //  по урону. Раньше boxRel применялся к score напрямую, и крупный
    //  хитбокс (грудь/ноги) переигрывал хитбоксы, убивающие меньшим числом
    //  выстрелов, — чит регулярно выбивал меньше урона, чем мог.
    //  Теперь сначала сравнивается число выстрелов до килла, затем
    //  видимость, и только потом взвешенный по HC score (+30/+10).
    // ------------------------------------------------------------------
    void* weapon = nullptr;
    if ( local )
        weapon = reinterpret_cast<C_BaseEntity*>( local )->GetActiveWeapon();
    float coneRadius = 0.f;
    if ( weapon )
        coneRadius = Penetration::GetInaccuracy( weapon )
                   + Penetration::GetSpread( weapon );
    const bool hcAware = coneRadius > 0.0001f;

    // Evaluate per hitbox (Pandora GetBestAimPosition structure): SetupHitboxPoints
    // resets its point vector, so each hitbox's points must be processed before
    // moving to the next one — otherwise hitboxes without multipoints (neck,
    // hands, forearms) would wipe the accumulated points of the earlier ones.
    std::vector<AimPoint> points;
    for (int hb = 0; hb < hbArr.num; ++hb)
    {
        if (!HitgroupEnabled(hbArr.groups[hb], effMask))
            continue;

        HitboxInfo info = hbArr.makeInfo(hb);
        if (info.index < 0)
            continue;

        // Вероятность попадания в ЭТОТ хитбокс (0..1), одинаковая для всех его
        // точек — поэтому не меняет выбор лучшей точки внутри хитбокса, только
        // ранжирование между разными хитбоксами. Для капсулы берём её радиус;
        // для бокса (feet) — четверть средней стороны как радиус сферы.
        // Распределение рассеивания равномерно по радиусу конуса, поэтому
        // P(hit) ~ (угловой размер / конус)^2.
        float boxRel = 1.f;
        if ( hcAware )
        {
            float effR = info.radius > 0.f ? info.radius : 0.f;
            if ( effR <= 0.001f )
            {
                const float bxw = info.world_max.x - info.world_min.x;
                const float bxl = info.world_max.y - info.world_min.y;
                const float bxh = info.world_max.z - info.world_min.z;
                effR = 0.25f * sqrtf( bxw * bxw + bxl * bxl + bxh * bxh );
            }
            const float dxt = info.center.x - eyePos.x;
            const float dyt = info.center.y - eyePos.y;
            const float dzt = info.center.z - eyePos.z;
            const float dist = sqrtf( dxt * dxt + dyt * dyt + dzt * dzt );
            const float angR = dist > 0.001f ? atan2f( effR, dist )
                                             : 3.14159265f;
            const float ratio = angR / coneRadius;
            float rel = ratio * ratio;
            if ( rel > 1.f ) rel = 1.f;
            // Смесь не обесценивает урон полностью даже у маленькой мишени
            // (0.2 floor), но сильно штрафует хитбокс, в который не попасть.
            boxRel = 0.20f + 0.80f * rel;
        }

        // Auto Wall must not disable multipoint: an edge point can be the
        // only geometrically valid ray around cover. Each point still passes
        // the full penetration, damage and hitchance gates below. Only safe
        // point mode explicitly requests center-only scans.
        const bool centerOnly = profile.safe_point_mode == 2;
        const float pointScale = centerOnly ? 0.f : profile.multipoint_scale / 100.f;
        const float stomachScale = centerOnly ? 0.f : profile.stomach_scale / 100.f;
        if (!SetupHitboxPoints(info, hb, pointScale,
            stomachScale, inAir, profile, points))
            continue;

        for (const auto& pt : points)
        {
            PenetrationResult pen = Penetration::Calculate(local, ps.entity, eyePos,
                pt.pos, wepInfo, &info, hbArr.groups[pt.hitbox]);

            if (!pen.visible && !pen.penetrate)
                continue;
            if (!profile.auto_wall && !pen.visible)
                continue;

            float need = EffectiveRageMinDamage( profile, pen.penetrate, pen.visible );
            // A configured minimum above the target's remaining HP must not
            // reject an otherwise lethal distant or pistol shot.
            if (need > (float)ps.health)
                need = (float)ps.health;
            if (need < 1.f)
                need = 1.f;
            if (pen.damage + 0.1f < need)
                continue;

            // Policy never authorises an invalid point. It only ranks points
            // that already passed geometry, autowall and min-damage checks.
            const int group = hbArr.groups[pt.hitbox];
            const bool isBody = group == HITGROUP_CHEST || group == HITGROUP_STOMACH ||
                group == HITGROUP_LEFTARM || group == HITGROUP_RIGHTARM ||
                group == HITGROUP_LEFTLEG || group == HITGROUP_RIGHTLEG;
            const float dx = pt.pos.x - info.center.x;
            const float dy = pt.pos.y - info.center.y;
            const float dz = pt.pos.z - info.center.z;
            const bool isCenter = (dx * dx + dy * dy + dz * dz) < 1.0f;

            // RageBot.cpp ComputePointsPriority ranks lethal body points above
            // raw-damage alternatives, gives context-sensitive head preference,
            // then applies user-forced body/head and safe-point policy. CSSO has
            // no resolver velocity matrices, so use only live snapshot flags and
            // actual penetration damage rather than fabricated resolver state.
            const bool lethal = pen.damage + 0.1f >= static_cast<float>(ps.health);
            float score = pen.damage;
            if (lethal)
                score += isBody ? 26.f : 18.f;
            // CSSO exposes the same Source player flag layout through the
            // snapshot integer, but does not export the FL_DUCKING symbol.
            constexpr int kCssoDuckingFlag = 0x2;
            if (group == HITGROUP_HEAD && (ps.flags & kCssoDuckingFlag))
                score += 5.f;
            if (profile.hitbox_preference == 1 && group == HITGROUP_HEAD)
                score += 20.f;
            else if (profile.hitbox_preference == 2 && isBody)
                score += 20.f;
            if (profile.safe_point_mode == 1 && isCenter)
                score += 6.f;

            bool better = false;
            const float effScore = score * boxRel;
            const int stk = shotsToKill( pen.damage, static_cast<float>( ps.health ) );
            if ( best.hitbox < 0 )
                better = true;
            else if ( stk != best.shots )
                better = stk < best.shots;
            else if ( pen.visible && !best.visible )
                better = true;
            else if ( !pen.visible && best.visible )
                better = false;
            else if ( best.hitbox != pt.hitbox )
                better = effScore > best.score + 30.f;   // internal_hvh hitbox-switch rule
            else
                better = effScore > best.score + 10.f;

            if ( better )
            {
                best.damage = pen.damage;
                best.score = effScore; // взвешенный score (raw урон в best.damage)
                best.shots = stk;
                best.hitbox = pt.hitbox;
                best.hitboxGroup = hbArr.groups[pt.hitbox];
                best.hitboxRadius = hbArr.radii[pt.hitbox];
                best.hb = hbArr.makeInfo(pt.hitbox);
                best.pos = pt.pos;
                best.penetrate = pen.penetrate;
                best.visible = pen.visible;
                best.valid = true;

                // Оптимизация: если точка видима и наносит летальный урон,
                // прекращаем перебор точек для этого хитбокса.
                if (best.visible && best.damage >= (float)ps.health)
                    break;
            }
        }
    }

    return best;
}

// ------------------------------------------------------------------
//  Can-shoot gate (internal_hvh port)
// ------------------------------------------------------------------
static bool IsGrenadeOrC4(void* weapon)
{
    const char* cls = (const char*)((uintptr_t)weapon + nWeaponClassnameOffset);
    if (!cls)
        return false;
    if (strstr(cls, "c4"))        return true;
    if (strstr(cls, "grenade"))   return true;
    if (strstr(cls, "flashbang")) return true;
    if (strstr(cls, "molotov"))   return true;
    if (strstr(cls, "decoy"))     return true;
    return false;
}

// Press-edge weapons fire once per button PRESS: pistols AND bolt-action
// snipers (AWP/scout) never re-fire on a continuously held IN_ATTACK. Rage
// owns the button for them completely — the raw attack is stripped on every
// command and re-pressed only on the commit, so each shot is a fresh edge
// with the verified rage angles.
static bool IsSemiAutoWeapon(void* weapon)
{
    const char* cls = (const char*)((uintptr_t)weapon + nWeaponClassnameOffset);
    if (!cls)
        return false;
    return strstr(cls, "deagle") || strstr(cls, "revolver") ||
           strstr(cls, "glock") || strstr(cls, "hkp2000") ||
           strstr(cls, "usp")   || strstr(cls, "p250") ||
           strstr(cls, "fiveseven") || strstr(cls, "tec9") ||
           strstr(cls, "cz75") || strstr(cls, "elite") ||
           strstr(cls, "awp")   || strstr(cls, "ssg08");
}

// IN_ATTACK2 is zoom only for these scoped rifle classes. On pistols it is a
// secondary action and must never replace a rage attack while waiting for HC.
static bool IsZoomableWeapon(void* weapon)
{
    const char* cls = (const char*)((uintptr_t)weapon + nWeaponClassnameOffset);
    if (!cls)
        return false;

    return strstr(cls, "awp") || strstr(cls, "ssg08") ||
        strstr(cls, "scar20") || strstr(cls, "g3sg1") ||
        strstr(cls, "aug") || strstr(cls, "sg556");
}

static bool IsRevolverWeapon(void* weapon)
{
    const char* cls = (const char*)((uintptr_t)weapon + nWeaponClassnameOffset);
    if (!cls)
        return false;
    return strstr(cls, "revolver") != nullptr;
}

// ---------------------------------------------------------------------------
//  Auto revolver (R8) controller.
//  server.dll CWeaponRevolver: while IN_ATTACK is held and curtime >=
//  m_flNextPrimaryAttack the weapon plays HAULBACK, sets
//  m_flNextSecondaryAttack = curtime + 0.25 every tick, and FIRES as soon as
//  m_flPostponeFireReadyTime (set ~0.234 s into the haulback) <= curtime.
//  Holding IN_ATTACK2 alone while curtime < m_flNextSecondaryAttack keeps the
//  hammer cocked without firing; releasing both buttons cancels the wind-up.
//  So: cock with IN_ATTACK until the postpone time is reached, then NEVER hold
//  IN_ATTACK except on an aimed rage shot; pause with IN_ATTACK2 while it is
//  safe, otherwise release one command and re-cock.
// ---------------------------------------------------------------------------
namespace RvrCtl
{
    // client.dll RecvProp + prediction field m_flPostponeFireReadyTime.
    constexpr int   kPostponeOffset = 0xEC8;
    constexpr float kCockTime       = 0.234375f;   // 0.03448 prone + 0.2 postpone
    constexpr float kSecondaryHold  = 0.25f;       // CanFire: nextSecondary = t + 0.25
    static void* s_weapon    = nullptr;
    static float s_cockStart = 0.f;   // first IN_ATTACK tick of the current wind-up
    static float s_lastAtk   = -1.f;  // last IN_ATTACK tick past next-primary
    static int   s_lastClip  = -1;

    static float Postpone(void* weapon)
    {
        const float v = *reinterpret_cast<float*>((uintptr_t)weapon + kPostponeOffset);
        return std::isfinite(v) ? v : 0.f;
    }

    static void Reset()
    {
        s_cockStart = 0.f;
        s_lastAtk = -1.f;
    }

    static void Sync(void* weapon)
    {
        const int clip = GetWeaponClip1(weapon);
        if (s_weapon != weapon)
        {
            s_weapon = weapon;
            Reset();
        }
        else if (s_lastClip >= 0 && clip < s_lastClip)
        {
            DBGLOG("[rvr] shot clip %d->%d\n", s_lastClip, clip);
            Reset();
            // Real discharge: this is the moment auto-peek may go back.
            AutoPeek::OnShotCommitted(1);
        }
        s_lastClip = clip;
    }

    // Ready = the next IN_ATTACK command fires. Deliberately biased EARLY
    // (2 ticks): an early "ready" only delays the shot by a tick, a late one
    // makes the held IN_ATTACK discharge into nowhere.
    static bool Ready(void* weapon, float t)
    {
        if (s_cockStart <= 0.f)
            return false;
        if (t >= s_cockStart + kCockTime - 2.f * SDK::kTickInterval)
            return true;
        const float p = Postpone(weapon);
        return p > 0.f && p <= t + SDK::kTickInterval;
    }

    // Runs last on every rage command while Auto Revolver is on.
    static void Finalize(CUserCmd* cmd, void* weapon, float t)
    {
        Sync(weapon);
        // Every R8 IN_ATTACK (cocking, early commit, manual hold) is hidden
        // from auto-peek; only the clip drop above counts as a shot.
        AutoPeek::IgnoreAttack(cmd->command_number);
        const float npa = GetWeaponNextPrimaryAttack(weapon);
        const bool cooldown = t < npa;

        // Aimed rage shot or the player's own held trigger.
        if (cmd->buttons & IN_ATTACK)
        {
            cmd->buttons &= ~IN_ATTACK2;
            if (!cooldown)
            {
                if (s_cockStart <= 0.f)
                    s_cockStart = t;
                s_lastAtk = t;
            }
            return;
        }
        // Player's own secondary (fan fire) passes untouched.
        if (cmd->buttons & IN_ATTACK2)
        {
            Reset();
            return;
        }
        if (cooldown || GetWeaponClip1(weapon) < 1 || IsWeaponReloading(weapon))
        {
            Reset();
            return;
        }
        if (!Ready(weapon, t))
        {
            // Winding up: postpone time not reached yet, IN_ATTACK cannot fire.
            if (s_cockStart <= 0.f)
                s_cockStart = t;
            s_lastAtk = t;
            cmd->buttons |= IN_ATTACK;
            return;
        }
        // Cocked: hold the hammer with IN_ATTACK2 while the secondary timer
        // keeps that button inert.
        if (s_lastAtk > 0.f && t < s_lastAtk + kSecondaryHold - 2.f * SDK::kTickInterval)
        {
            cmd->buttons |= IN_ATTACK2;
            return;
        }
        // Pause window over: one empty command cancels the wind-up (no shot),
        // the next command starts a fresh one.
        Reset();
    }
}

// Matches internal_hvh aimbot::can_shoot(). When this returns false the whole
// rage routine (including autostop) is skipped, so movement stays free during
// reload, empty clip and fire cooldowns.
static bool CanShoot(C_BaseEntity* local, void* weapon, float time)
{
    if (!local || !weapon || !std::isfinite(time) || IsGrenadeOrC4(weapon) ||
        GetWeaponClip1(weapon) < 1 || IsWeaponReloading(weapon)) return false;
    const float nextPrimary = GetWeaponNextPrimaryAttack(weapon);
    const float nextAttack = local->GetNextAttack();
    // These are native player-clock timers. A future or invalid value is
    // NOT a ready weapon. Never rewrite deploy/cooldown state or authorize
    // an early commit merely because a render clock disagreed with it.
    return std::isfinite(nextPrimary) && std::isfinite(nextAttack) &&
        nextPrimary <= time && nextAttack <= time;
}

// ------------------------------------------------------------------
//  Backtracking helpers (internal_hvh lag_compensation port)
// ------------------------------------------------------------------

// Backtrack tick: records keep the engine tick_count of the command their
// interpolated pose was captured on (LagRecord::cmd_tick); the former
// simtime+0.1 lerp guess (FixTickCount) was removed — see RunRage.

// Hit chance against a chosen hitbox uses Pandora's CanHit math on the
// record's stored capsule endpoints — no live-entity traces involved.

// Observe each native snapshot. Store its render pose only while its command
// time remains supported by the received network timeline.
void Ragebot::UpdateRecords(const EntitySnapshot& snap, int localTeam)
{
    if (!g_config.settings.aimbot.enabled || !g_config.settings.aimbot.rage)
    { LagCompensation::Reset(); return; }
    // Timeline observation is required even with historical Backtrack off.
    // Keep the native render pose and its command timestamp as one unit.
    // Do not pretend SetupBones(simtime) reconstructs server animation.
    if (snap.cmdTickCount <= 0) return;
    LagCompensation::Timing timing;
    if (!LagCompensation::ReadTiming(timing))
    { LagCompensation::Reset(); return; }
    static int previousServerTick = 0;
    if (previousServerTick > timing.serverTick) LagCompensation::Reset();
    previousServerTick = timing.serverTick;
    const int maxClients = (std::clamp)(snap.maxClients, 0, 64);
    for (int i=maxClients+1; i<65; ++i)
    { g_playerRecords[i].records.clear(); g_playerRecords[i].entity = nullptr;
      g_playerRecords[i].timeline = {}; }
    for (int i = 1; i <= maxClients; ++i)
    {
        const auto& ps = snap.players[i];
        auto& owner = g_playerRecords[i];
        if (owner.entity != ps.entity || !ps.entity || !ps.alive ||
            ps.team == localTeam || ps.dormant)
        { owner.records.clear(); owner.timeline = {}; }
        owner.entity = ps.entity;
        if (!ps.entity || !ps.alive || ps.team == localTeam || ps.dormant) continue;
        if (!ps.networkOriginValid || !std::isfinite(ps.simtime) || ps.simtime <= 0.f)
        { owner.records.clear(); continue; } // preserve continuity across a failed sample
        auto& store = owner.records;
        if (!LagCompensation::Observe(owner.timeline,
            reinterpret_cast<uintptr_t>(ps.entity),
            ps.modelIdentity ? ps.modelIdentity : owner.timeline.model,
            ps.hitboxSet >= 0 ? ps.hitboxSet : owner.timeline.hitboxSet,
            ps.simtime, ps.networkOrigin, timing.interval, timing.teleportDistance))
            store.clear();
        // A failed bone sample cannot erase a detected network discontinuity.
        if (!ps.hitboxes_valid || ps.num_hitboxes <= 0 || ps.num_hitboxes > kMaxHitboxes)
        { store.clear(); continue; }
        if (!store.empty() && (snap.cmdTickCount < store.back().cmd_tick ||
            std::abs(store.back().capture_lerp-timing.lerp) > .000001f)) store.clear();
        if (!g_config.settings.aimbot.backtrack)
        { store.clear(); continue; } // keep observing the remote timeline
        const int maximumAge = (std::min)(kMaxLagRecords,
            static_cast<int>(timing.maxUnlag/timing.interval));
        while (!store.empty() && snap.cmdTickCount-store.front().cmd_tick > maximumAge)
            store.pop_front();
        if (!store.empty() && snap.cmdTickCount == store.back().cmd_tick) continue;
        if (LagCompensation::CheckCommandSupport(timing, snap.cmdTickCount,
            ps.simtime, owner.timeline.brokenSince) != LagCompensation::Support::Supported)
            continue; // no invented/extrapolated history while packets are absent
        const auto decision = LagCompensation::ValidateCaptured(timing, snap.cmdTickCount, timing.lerp);
        if (!decision) continue;
        LagRecord rec;
        rec.simtime = ps.simtime;
        rec.cmd_tick = snap.cmdTickCount;
        rec.capture_lerp = timing.lerp;
        rec.origin = ps.origin;
        rec.modelIdentity = ps.modelIdentity;
        rec.hitboxSet = ps.hitboxSet;
        rec.flags = ps.flags;
        rec.num_hitboxes = ps.num_hitboxes;
        for (int hb = 0; hb < ps.num_hitboxes; ++hb)
        {
            rec.hitbox_group[hb] = ps.hitbox_group[hb];
            rec.hitbox_studio_index[hb] = ps.hitbox_studio_index[hb];
            rec.hitbox_world_center[hb] = ps.hitbox_world_center[hb];
            rec.hitbox_model_min[hb] = ps.hitbox_model_min[hb];
            rec.hitbox_model_max[hb] = ps.hitbox_model_max[hb];
            rec.hitbox_world_min[hb] = ps.hitbox_world_min[hb];
            rec.hitbox_world_max[hb] = ps.hitbox_world_max[hb];
            memcpy(rec.hitbox_matrix[hb], ps.hitbox_matrix[hb],
                sizeof(rec.hitbox_matrix[hb]));
            rec.hitbox_radius[hb] = ps.hitbox_radius[hb];
        }

        store.push_back(rec);
        while (store.size() > kMaxLagRecords)
            store.pop_front();
    }
}

// ------------------------------------------------------------------
//  Bone targeting
// ------------------------------------------------------------------
// ------------------------------------------------------------------
//  Hitbox targeting
// ------------------------------------------------------------------

// World-space center of the first hitbox whose group matches `hitgroup`.
static bool FindHitboxGroup(const PlayerSnapshot& ps, int hitgroup, Vec3& out)
{
    if (!ps.hitboxes_valid || ps.num_hitboxes <= 0)
        return false;

    for (int i = 0; i < ps.num_hitboxes; ++i)
    {
        if (ps.hitbox_group[i] == hitgroup)
        {
            out = ps.hitbox_world_center[i];
            return true;
        }
    }
    return false;
}

static bool FindHeadBone(const PlayerSnapshot& ps, int manualBone, Vec3& out)
{
    if (!ps.bones_valid || ps.num_bones == 0)
        return false;

    if (manualBone > 0 && manualBone < ps.num_bones)
    {
        out = ps.world_bones[manualBone];
        return true;
    }

    // Real head hitbox center (much more accurate than origin + 60).
    if (FindHitboxGroup(ps, HITGROUP_HEAD, out))
        return true;

    Vec3 headGuess = ps.origin + Vec3{ 0.f, 0.f, 60.f };
    float bestDist = 999999.f;
    int   bestBone = -1;

    for (int i = 1; i < ps.num_bones; ++i)
    {
        const Vec3& pos = ps.world_bones[i];
        if (pos.x == 0.f && pos.y == 0.f && pos.z == 0.f)
            continue;
        if (pos.z < ps.origin.z + 40.f)
            continue;

        float dx = pos.x - headGuess.x;
        float dy = pos.y - headGuess.y;
        float dz = pos.z - headGuess.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < bestDist)
        {
            bestDist = dist;
            bestBone = i;
        }
    }

    if (bestBone < 0)
        return false;

    out = ps.world_bones[bestBone];
    return true;
}

// ------------------------------------------------------------------
//  Legit aimbot (original logic)
// ------------------------------------------------------------------
static void RunLegit(void* pCInput, int seq,
    void* pLocalPlayer, int localTeam,
    const EntitySnapshot& snap)
{
    auto& aim = g_config.settings.aimbot;
    if (aim.on_key && !Binds::Active("aimbot", aim.key, aim.key_mode, "Aimbot"))
        return;

    auto** ppCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>(pCInput) + CINPUT_OFFSET_COMMANDS);
    if (!ppCommands || !*ppCommands)
        return;

    auto* commands = *ppCommands;
    int   idx = seq % MULTIPLAYER_BACKUP;
    auto* cmd = &commands[idx];

    if (cmd->command_number != seq)
        return;

    auto* local = reinterpret_cast<C_BaseEntity*>(pLocalPlayer);
    // Predicted eye — same fire-tick contract as RunRage.
    Vec3  eyePos = EnginePrediction::EyePosition(local);

    Vec3 oldAngles;
    oldAngles.x = GetPitch(cmd);
    oldAngles.y = GetYaw(cmd);
    oldAngles.z = 0.f;

    float bestFov = aim.fov > 0.f ? aim.fov : 180.f;
    Vec3  bestAngle{};
    bool  found = false;

    for (int i = 1; i <= snap.maxClients; ++i)
    {
        auto& ps = snap.players[i];
        if (!ps.alive || ps.team == localTeam || ps.dormant)
            continue;

        // PlayerList: a player with aiming disabled is invisible to legit aim.
        if (!PlayerList::AimAllowed(i))
            continue;

        Vec3 target{};
        if (!FindHeadBone(ps, aim.bone_index, target))
            continue;

        Vec3  ang = CalcAngle(eyePos, target);
        float fov = AngleFOV(oldAngles.x, oldAngles.y, ang.x, ang.y);
        if (fov < bestFov)
        {
            bestFov = fov;
            bestAngle = ang;
            found = true;
        }
    }

    if (!found)
        return;

    ClampAngles(bestAngle);
    Vec3 newAngles = { bestAngle.x, bestAngle.y, 0.f };

    if (aim.silent && !(cmd->buttons & IN_ATTACK))
        return;

    CorrectMovement(cmd, oldAngles, newAngles);
    SetYaw(cmd, bestAngle.y);
    SetPitch(cmd, bestAngle.x);
}

// ------------------------------------------------------------------
//  Rage weapon profile resolution
// ------------------------------------------------------------------
static int ClassifyRageWeaponProfile( void* weapon )
{
    if ( !weapon )
        return static_cast<int>( cfg::rage_weapon_profile_id::Default );

    const char* name = reinterpret_cast<const char*>(
        reinterpret_cast<uintptr_t>( weapon ) + nWeaponClassnameOffset );
    if ( !name || !name[0] )
        return static_cast<int>( cfg::rage_weapon_profile_id::Default );

    const auto has = [ name ]( const char* token ) { return std::strstr( name, token ) != nullptr; };
    if ( has( "knife" ) || has( "bayonet" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::Knife );
    if ( has( "deagle" ) || has( "revolver" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::HeavyPistol );
    if ( has( "ssg08" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::Scout );
    if ( has( "awp" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::AWP );
    if ( has( "g3sg1" ) || has( "scar20" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::AutoSniper );
    if ( has( "nova" ) || has( "xm1014" ) || has( "mag7" ) || has( "sawedoff" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::Shotgun );
    if ( has( "m249" ) || has( "negev" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::LMG );
    if ( has( "mac10" ) || has( "mp9" ) || has( "mp7" ) || has( "mp5" ) ||
         has( "ump45" ) || has( "p90" ) || has( "bizon" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::SMG );
    if ( has( "glock" ) || has( "hkp2000" ) || has( "usp" ) || has( "p250" ) ||
         has( "fiveseven" ) || has( "tec9" ) || has( "cz75" ) || has( "elite" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::Pistol );
    if ( has( "ak47" ) || has( "m4a" ) || has( "aug" ) || has( "sg556" ) ||
         has( "galilar" ) || has( "famas" ) )
        return static_cast<int>( cfg::rage_weapon_profile_id::Rifle );
    return static_cast<int>( cfg::rage_weapon_profile_id::Default );
}

static cfg::rage_weapon_profile_t LegacyRageProfile( const cfg::aimbot_t& aim )
{
    cfg::rage_weapon_profile_t p;
    p.auto_fire = aim.auto_fire;
    p.auto_wall = aim.auto_wall;
    p.auto_scope = aim.auto_scope;
    p.min_damage = aim.min_damage;
    p.wall_min_dmg = aim.wall_min_dmg;
    p.hit_chance = aim.hit_chance;
    p.fov = aim.fov;
    p.hitgroup_mask = aim.hitgroup_mask;
    p.multipoint_scale = aim.multipoint_scale;
    p.stomach_scale = aim.stomach_scale;
    p.multipoint_head = aim.multipoint_head;
    p.multipoint_chest = aim.multipoint_chest;
    p.multipoint_stomach = aim.multipoint_stomach;
    p.multipoint_legs = aim.multipoint_legs;
    p.multipoint_feet = aim.multipoint_feet;
    p.auto_stop = aim.auto_stop;
    p.auto_stop_on_key = aim.auto_stop_on_key;
    p.auto_stop_key = aim.auto_stop_key;
    p.auto_stop_key_mode = aim.auto_stop_key_mode;
    p.auto_stop_slide_speed = aim.auto_stop_slide_speed;
    return p;
}

static cfg::rage_weapon_profile_t ResolveRageProfile( void* weapon, int& outIndex )
{
    const auto& profiles = g_config.settings.aimbot.weapon_profiles;
    outIndex = ClassifyRageWeaponProfile( weapon );
    if ( !profiles.enabled )
    {
        outIndex = static_cast<int>( cfg::rage_weapon_profile_id::Default );
        return LegacyRageProfile( g_config.settings.aimbot );
    }

    if ( outIndex < 0 || outIndex >= cfg::RAGE_WEAPON_PROFILE_COUNT )
        outIndex = static_cast<int>( cfg::rage_weapon_profile_id::Default );

    const auto& selected = profiles.profile[outIndex];
    if ( selected.enabled )
        return selected;

    // A disabled class intentionally falls back to Default so unknown/custom
    // CSSO weapon classnames never make the rage pipeline silently disappear.
    outIndex = static_cast<int>( cfg::rage_weapon_profile_id::Default );
    return profiles.profile[outIndex];
}

static int s_activeRageProfile = static_cast<int>( cfg::rage_weapon_profile_id::Default );

// ------------------------------------------------------------------
//  Rage aimbot
// ------------------------------------------------------------------
static void RunRage(void* pCInput, int seq,
    void* pLocalPlayer, int localTeam,
    const EntitySnapshot& snap)
{
    auto& aim = g_config.settings.aimbot;

    // A Supremacy stop request is current-command only. Clear it before every
    // possible early exit so cooldown/reload/no-target commands can never
    // replay movement from a preceding target.
    s_pendingAutoStop = false;
    s_autoStopMaxSpeed = 0.f;

    if (aim.on_key && !Binds::Active("aimbot", aim.key, aim.key_mode, "Aimbot"))
        return;

    auto* local = reinterpret_cast<C_BaseEntity*>(pLocalPlayer);
    if (!local)
        return;

    // Get weapon data from active weapon
    void* weapon = local->GetActiveWeapon();
    if (!weapon)
        return;

    // Predicted fire-tick curtime: m_nTickBase * interval — the native
    // clock domain the weapon timers (m_flNextPrimaryAttack, m_flNextAttack)
    // live in (RunCommand computes curtime from player+0x17DC). The old
    // IVEngineClient::GetTime read runs in the client render domain and was
    // the source of the "clock domain mismatch" stalls and premature /
    // late fire gates. Falls back to GetTime outside the prediction window.
    const float curtime = EnginePrediction::CurTime();
    LagCompensation::Timing lagTiming;
    LagCompensation::ReadTiming(lagTiming); // also protects live-only rage against stale updates

    const WeaponData* wepInfo = GetWeaponDataFromEntity(weapon);
    if (!wepInfo || wepInfo->damage <= 0)
    {
        // TEMP diagnostic: the silent early return when weapon data is not
        // resolved is the prime suspect for "rage won't fire until I pick up
        // one specific weapon". Prints once per weapon pointer so one run
        // names exactly which weapon/class fails and which SDK fns are null.
        static void* s_lastBadWeapon = nullptr;
        if ( s_lastBadWeapon != weapon )
        {
            s_lastBadWeapon = weapon;
            const char* cls = (const char*)( (uintptr_t)weapon + nWeaponClassnameOffset );
            DBGLOG( "[rage] EARLY-WEP wep=%p cls='%s' info=%p dmg=%d name2idx=%p idx2ptr=%p invalid=%p\n",
                weapon, cls ? cls : "?", (const void*)wepInfo,
                wepInfo ? wepInfo->damage : 0,
                (void*)SDK::fnWeaponDataNameToIndex,
                (void*)SDK::fnWeaponDataIndexToPtr,
                SDK::fnWeaponDataInvalidIdx
                    ? (void*)SDK::fnWeaponDataInvalidIdx() : nullptr );
        }
        return;
    }

    int activeProfile = 0;
    cfg::rage_weapon_profile_t profile = ResolveRageProfile( weapon, activeProfile );
    s_activeRageProfile = activeProfile;
    if ( !profile.enabled )
    {
        // TEMP diagnostic: profile disabled -> rage exits before any shot.
        static void* s_lastDisabledWeapon = nullptr;
        if ( s_lastDisabledWeapon != weapon )
        {
            s_lastDisabledWeapon = weapon;
            const char* cls = (const char*)( (uintptr_t)weapon + nWeaponClassnameOffset );
            DBGLOG( "[rage] EARLY-PROF wep=%p cls='%s' profile=%d/(%s) enabled=0\n",
                weapon, cls ? cls : "?", activeProfile,
                cfg::RageWeaponProfileName( activeProfile ) );
        }
        return;
    }

    // Min damage override goes through the shared bind system (hold /
    // toggle / always / off-hotkey) and shows in the keybind list.
    // One bind state per weapon profile: with a single shared id, swapping
    // to a weapon whose profile has a different key/mode reset the toggle,
    // so the override was off again when you switched back.
    static const char* const kMinDmgBindIds[] = {
        "min_damage_p0", "min_damage_p1", "min_damage_p2", "min_damage_p3",
        "min_damage_p4", "min_damage_p5", "min_damage_p6", "min_damage_p7",
        "min_damage_p8", "min_damage_p9", "min_damage_p10", "min_damage_p11",
        "min_damage_p12", "min_damage_p13", "min_damage_p14", "min_damage_p15" };
    static_assert( cfg::RAGE_WEAPON_PROFILE_COUNT <= 16, "extend kMinDmgBindIds" );
    const int mdIdx = ( activeProfile >= 0 && activeProfile < cfg::RAGE_WEAPON_PROFILE_COUNT ) ? activeProfile : 0;
    s_minDamageOverrideActive = profile.min_damage_override_on_key &&
        Binds::Active( kMinDmgBindIds[ mdIdx ], profile.min_damage_override_key,
                       profile.min_damage_override_key_mode, "Min damage override" );
    s_minDamageOverrideShowIndicator = s_minDamageOverrideActive &&
        profile.min_damage_override_indicator;
    s_minDamageOverrideValue = profile.min_damage_override;
    if ( s_minDamageOverrideValue < 1 ) s_minDamageOverrideValue = 1;
    if ( s_minDamageOverrideValue > 100 ) s_minDamageOverrideValue = 100;

    auto** ppCommands = reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>(pCInput) + CINPUT_OFFSET_COMMANDS);
    if (!ppCommands || !*ppCommands)
        return;

    auto* commands = *ppCommands;
    int   idx = seq % MULTIPLAYER_BACKUP;
    auto* cmd = &commands[idx];

    if (cmd->command_number != seq)
        return;

    // Never authorize a shot or consume manual input from an approximate or
    // missing prediction frame (loading/reconnect/history wrap/build mismatch).
    if (!EnginePrediction::Reliable())
    {
        if (DebugTick(32)) DBGLOG("[rage] BLOCK native-prediction cmd=%d reason=%s\n",
            cmd->command_number, EnginePrediction::FailureReason());
        return;
    }

    // Physical engine input ALWAYS retains the user's own primary attack.
    // Read the input snapshot taken BEFORE Triggerbot/Rage, not GetAsyncKeyState
    // or auto-fire's synthetic button. Gates own only automatically added shots.
    struct ManualAttackGuard
    {
        CUserCmd* c; bool held; bool secondaryHeld;
        void* rvrWeapon = nullptr;
        float rvrTime = 0.f;
        ~ManualAttackGuard()
        {
            if (held)
            {
                // Auto-scope must not swallow a real LMB click. Preserve a
                // secondary action only when the user actually requested it.
                if (!secondaryHeld) c->buttons &= ~IN_ATTACK2;
                c->buttons |= IN_ATTACK;
            }
            // Auto revolver owns the final button state of every command.
            if ( rvrWeapon )
                RvrCtl::Finalize( c, rvrWeapon, rvrTime );
        }
    } manualAttackGuard{cmd, (snap.inputButtons & IN_ATTACK) != 0,
                            (snap.inputButtons & IN_ATTACK2) != 0};

    // Evaluate automatic shots without provisional attack buttons. Original
    // human input is restored by the scope guard regardless of these gates.
    const bool semiAuto = IsSemiAutoWeapon(weapon);
    const bool autoRevolver = g_config.settings.aimbot.auto_revolver && IsRevolverWeapon(weapon);
    const bool rvrRechargeBlocked = autoRevolver && DoubleTap::IsRecharging();
    if (autoRevolver && !rvrRechargeBlocked)
    {
        manualAttackGuard.rvrWeapon = weapon;
        manualAttackGuard.rvrTime = curtime;
        RvrCtl::Sync(weapon);
    }

    const bool userAttackHeld = (cmd->buttons & IN_ATTACK) != 0;
    // Strip provisional automatic buttons during evaluation. The scope
    // guard restores ORIGINAL manual input even when auto-fire is enabled.
    if (profile.auto_fire || semiAuto || autoRevolver)
        cmd->buttons &= ~IN_ATTACK;

    // Match the source routine's `tickbase::to_recharge` guard. During the
    // local command recharge phase do not start or extend an R8 primary cycle.
    if (rvrRechargeBlocked)
        return;

    // Manual fire edge: a fresh physical click this command. Rage must never
    // lock the player's own gun while it waits for its gates — every click
    // goes through with the player's angles; only the held/auto intent is
    // suppressed so shots cannot leak continuously un-aimed.
    static bool s_lastUserAttackHeld = false;
    const bool userAttackEdge = userAttackHeld && !s_lastUserAttackHeld;
    s_lastUserAttackHeld = userAttackHeld;

    // Press-edge spacing: a commit command must be followed by at least one
    // release command. Without it, whenever the gates pass continuously
    // (which is exactly what happens when the engine silently refused the
    // first shot — no cooldown starts), EVERY command commits, the button is
    // held without interruption, no new press edge ever appears and the
    // weapon stays silent — the standing "fires only after a jump" stall.
    // The forced release tick breaks that loop within one command.
    static int   s_edgeLastCommit = 0;
    static void* s_edgeWeapon     = nullptr;
    if ( s_edgeWeapon != weapon )
    {
        s_edgeWeapon     = weapon;
        s_edgeLastCommit = 0;
    }
    // The revolver charge session needs a continuous button; the forced
    // release tick between pistol commits would abort every second charge.
    if ( semiAuto && !autoRevolver && s_edgeLastCommit > 0 &&
         cmd->command_number <= s_edgeLastCommit + 1 )
    {
        cmd->buttons &= ~IN_ATTACK;
        // This forced-release tick returns before target selection, which
        // used to drop the brake every other tick on pistols. Keep it.
        if ( s_lastStopCmd == cmd->command_number - 1 && s_lastStopMaxSpeed > 1.f )
        {
            s_pendingAutoStop = true;
            s_autoStopMaxSpeed = s_lastStopMaxSpeed;
            s_autoStopFull=s_lastStopFull;
            s_lastStopCmd = cmd->command_number;
        }
        return;
    }

    // Predicted eye position: the server fires from the post-movement origin
    // of this command, so the aim angle must be computed from the one-tick-
    // ahead eye. The old live EyePosition() was up to ~4 units stale per tick
    // while moving — a head miss at range.
    Vec3 eyePos = EnginePrediction::EyePosition(local);

    Vec3 oldAngles;
    oldAngles.x = GetPitch(cmd);
    oldAngles.y = GetYaw(cmd);
    oldAngles.z = 0.f;

    // Persistent target index — survives across ticks to implement hysteresis
    // and FOV bias toward the current target.
    static int s_targetIndex = 0;

    // In the reference pipeline target evaluation runs independently from
    // weapon readiness. Keep that separation: normal mode can maintain its
    // slowwalk band between shots, while this flag is checked before scope/fire.
    // R8 cock state is NOT part of readiness: the HC / wait-brake gates must
    // run on every command, otherwise the brake toggled with the hammer cycle
    // (cocking ticks braked, cocked ticks released) — jerky walking. The
    // cock state gates only the final commit below.
    const bool weaponReady = CanShoot(local, weapon, curtime);
    const bool weaponUsable = !IsGrenadeOrC4(weapon) &&
        GetWeaponClip1(weapon) > 0 && !IsWeaponReloading(weapon);

    if (!weaponReady && profile.auto_stop > 0 && DebugTick(4))
    {
        const float npa = GetWeaponNextPrimaryAttack(weapon);
        const float nextAtk = local->GetNextAttack();
        DBGLOG("[autostop] COOLDOWN clip=%d reload=%d npa=%.2f>%.2f nextAtk=%.2f>%.2f\n",
            GetWeaponClip1(weapon), IsWeaponReloading(weapon) ? 1 : 0,
            npa, curtime, nextAtk, curtime);
    }

    // Per-profile hitbox policy. Force binds are true hold binds and only
    // replace the mask for the current command; they never mutate the saved
    // profile or affect a different weapon after a swap.
    constexpr int kBodyMask = cfg::HITGROUP_BIT_CHEST | cfg::HITGROUP_BIT_STOMACH |
        cfg::HITGROUP_BIT_LEFTARM | cfg::HITGROUP_BIT_RIGHTARM |
        cfg::HITGROUP_BIT_LEFTLEG | cfg::HITGROUP_BIT_RIGHTLEG;
    const bool forceBodyBind = profile.force_body_on_key &&
        Binds::Active( "force_body", profile.force_body_key, profile.force_body_key_mode, "Force body aim" );
    const bool forceHeadBind = profile.force_head_on_key &&
        Binds::Active( "force_head", profile.force_head_key, profile.force_head_key_mode, "Force head aim" );
    const bool forceBody = forceBodyBind;
    const bool forceHead = !forceBody && forceHeadBind;
    const int configuredMask = forceBody ? kBodyMask :
        ( forceHead ? cfg::HITGROUP_BIT_HEAD : profile.hitgroup_mask );
    const int effMask = configuredMask ? configuredMask
        : ( cfg::HITGROUP_BIT_HEAD | cfg::HITGROUP_BIT_CHEST | cfg::HITGROUP_BIT_STOMACH );

    const float maxFov = profile.fov > 0.f ? profile.fov : 180.f;
    const bool  inair = !(local->GetFlags() & FL_ONGROUND);

    // Damage-based target selection (internal_hvh port): every enemy is scanned
    // over its lag history (newest→oldest within the server compensation
    // window), multipoint aimpoints per record, the best-damage point wins, and
    // the best-damage enemy is chosen. If no usable record exists for a player
    // (fresh round, first frames, simtime read failed) we fall back to the live
    // snapshot so the aimbot keeps working instead of going silent.

    struct PlayerBest
    {
        int    index = 0;
        int    hitbox = -1;
        int    hitboxGroup = -1;
        void* entity = nullptr;
        float  simtime = 0.f;   // 0 = live snapshot (no tick correction)
        int    cmdTick = 0;     // validated original command tick of the captured pose
        HitboxInfo hb;              // winning hitbox (center/endpoints/matrix)
        Vec3   pos{};               // multipoint aimpoint (aim angle goes here)
        Vec3   angle{};
        float  damage = 0.f;
        float  health = 0.f;
        float  fov = 0.f;
        int    shotsToKill = 999;
        bool   fromRecord = false;
        bool   penetrate = false;
        bool   visible = false;
        bool   valid = false;
        // Winning hitbox set for the hit-chance trace (ClipRayToEntity
        // semantics — the whole record/snapshot, not just the winning
        // hitbox). Pointers are stable for the duration of RunRage: the
        // records deque is not mutated here and snap is a reference param.
        const LagRecord* record = nullptr;
        const PlayerSnapshot* snap = nullptr;
    };

    auto MeetsMinDamage = [&](const PlayerBest& cand) -> bool
        {
            if (!cand.valid)
                return false;

            float minDmg = EffectiveRageMinDamage( profile, cand.penetrate,
                !cand.penetrate );
            if (cand.health > 0.f && minDmg > cand.health)
                minDmg = cand.health;
            if (minDmg < 1.f)
                minDmg = 1.f;
            return cand.damage + 0.1f >= minDmg;
        };

    // Rage priority adapted from the reference scan order: first prefer a
    // target that dies in fewer shots; use visibility, damage, FOV and record
    // freshness only as deterministic tie breakers. It avoids dumping a
    // nearly-dead visible enemy just because another target has a larger raw
    // damage estimate through a wall.
    auto ShotsToKill = [](float damage, float health) -> int
        {
            if (damage <= 0.f || health <= 0.f)
                return 999;
            const int shots = static_cast<int>(ceilf(health / damage));
            return shots > 0 ? shots : 1;
        };

    auto CandidateBeats = [&](float damage, float health, bool visible,
        float fov, float simtime, bool fromRecord,
        const PlayerBest& current) -> bool
        {
            if (!current.valid)
                return true;

            // Legendware can prefer a historical record because it also owns
            // full prediction, latency and server tickbase state. This CSSO
            // project does not expose that complete pipeline; favour the live
            // snapshot whenever it is valid and retain records as fallback.
            // It prevents choosing a past capsule that local math accepts but
            // the server cannot rewind to on the outgoing command.
            if (fromRecord != current.fromRecord)
                return !fromRecord;

            const int shots = ShotsToKill(damage, health);
            if (shots != current.shotsToKill)
                return shots < current.shotsToKill;
            if (visible != current.visible)
                return visible;
            if (damage > current.damage + 0.5f)
                return true;
            if (current.damage > damage + 0.5f)
                return false;
            if (fov + 0.1f < current.fov)
                return true;
            if (current.fov + 0.1f < fov)
                return false;
            return simtime > current.simtime;
        };

    PlayerBest playerBest[65];

    // Validate s_targetIndex before the scan: if the previous target is now
    // dead, dormant or on our team, reset immediately so the FOV-bias (* 0.5)
    // is not applied to a dead slot — which would make every other enemy look
    // twice as far away and block target switching after a kill.
    if (s_targetIndex > 0 && s_targetIndex <= snap.maxClients)
    {
        const auto& prevPs = snap.players[s_targetIndex];
        if (!prevPs.alive || prevPs.team == localTeam || prevPs.dormant)
            s_targetIndex = 0;
    }
    else
    {
        s_targetIndex = 0;
    }

    for (int i = 1; i <= snap.maxClients; ++i)
    {
        const auto& ps = snap.players[i];
        if (!ps.alive || ps.team == localTeam || ps.dormant)
            continue;

        // PlayerList: a player with aiming disabled is invisible to rage.
        if (!PlayerList::AimAllowed(i))
            continue;

        // PlayerList: preferred players use the same half-FOV bias as the
        // currently locked target so they win the scan from every angle.
        const bool preferredTarget = PlayerList::IsPreferred(i);

        auto& owner = g_playerRecords[i];
        auto& store = owner.records;
        if (owner.entity != ps.entity || (!store.empty() &&
            (store.back().modelIdentity != ps.modelIdentity ||
             store.back().hitboxSet != ps.hitboxSet)))
        { store.clear(); owner.entity = ps.entity; }

        // Missing updates are not proof of desync. Respect the configured
        // hitgroups; do not replace them with a forced body mask or guess yaw.
        const float breakTime = owner.timeline.available ? owner.timeline.brokenSince : 0.f;
        const bool liveSupported = !lagTiming.available ||
            (ps.networkOriginValid && LagCompensation::CheckCommandSupport(lagTiming,
                snap.cmdTickCount, ps.simtime, breakTime) == LagCompensation::Support::Supported);

        // Record-pose pause: after two record shots on this player resolved as
        // "unknown pose/timing", the captured rewind capsule clearly stopped
        // matching the server's. Skip the lag-history scan for now (a landed
        // shot resets the stutter) — the live snapshot below is still scanned,
        // so aiming never goes silent for this player.
        const bool recordPaused = EventLog::GetPoseMisses( i ) >= 2 &&
            !g_config.settings.aimbot.backtrack_only_test;

        // ---- 1) lag history scan ----
        // Backtrack is deliberately optional. With it off the code below skips
        // every historical record and continues to the live snapshot fallback.
        if (g_config.settings.aimbot.backtrack && !recordPaused)
        {
        int recordsScanned = 0;
        for (auto it = store.rbegin(); it != store.rend(); ++it)
        {
            const LagRecord& rec = *it;

            // Оптимизация: не сканируем слишком много записей, чтобы не просаживать FPS.
            if (++recordsScanned > 12)
                break;

            const int rewindTicks = (std::clamp)(aim.backtrack_ticks, 1, 12);
            const int age = snap.cmdTickCount-rec.cmd_tick;
            if (!rec.valid || age < 0) continue;
            if (age > rewindTicks) break;
            const auto decision = LagCompensation::ValidateCaptured(lagTiming,
                rec.cmd_tick, rec.capture_lerp);
            if (!decision || LagCompensation::CheckSupport(lagTiming,
                decision.targetTime, ps.simtime, breakTime) != LagCompensation::Support::Supported)
                continue;

            if (rec.num_hitboxes <= 0)
                continue;

            // Cheap FOV gate on the first enabled hitbox center of this record
            // before spending penetration traces on it.
            int probeHitbox = -1;
            for (int hb = 0; hb < rec.num_hitboxes; ++hb)
            {
                if (!HitgroupEnabled(rec.hitbox_group[hb], effMask))
                    continue;
                probeHitbox = hb;
                break;
            }
            if (probeHitbox < 0)
                continue;

            Vec3  probeAng = CalcAngle(eyePos, rec.hitbox_world_center[probeHitbox]);
            float fov = AngleFOV(oldAngles.x, oldAngles.y, probeAng.x, probeAng.y);
            if (i == s_targetIndex || preferredTarget)
                fov *= 0.5f;
            if (fov > maxFov)
                continue;

            HitboxArrays arr{ rec.hitbox_group, rec.hitbox_studio_index,
                              rec.hitbox_world_center, rec.hitbox_model_min,
                              rec.hitbox_model_max, rec.hitbox_world_min,
                              rec.hitbox_world_max, rec.hitbox_matrix,
                              rec.hitbox_radius, rec.num_hitboxes };
            PointEval ev = EvalHitboxSource(local, ps, arr, eyePos, wepInfo, profile,
                effMask, !(rec.flags & FL_ONGROUND));
            if (!ev.valid)
                continue;

            Vec3  bestAng = CalcAngle(eyePos, ev.pos);
            float bestFov = AngleFOV(oldAngles.x, oldAngles.y, bestAng.x, bestAng.y);
            if (i == s_targetIndex || preferredTarget)
                bestFov *= 0.5f;

            // Keep the record/point with the best rage priority instead of
            // raw damage alone (fewest shots to kill, then visibility etc.).
            if (CandidateBeats(ev.damage, static_cast<float>(ps.health),
                ev.visible, bestFov, rec.simtime, true, playerBest[i]))
            {
                playerBest[i].index = i;
                playerBest[i].hitbox = ev.hitbox;
                playerBest[i].hitboxGroup = ev.hitboxGroup;
                playerBest[i].entity = ps.entity;
                playerBest[i].simtime = rec.simtime;
                playerBest[i].cmdTick = decision.tick;
                playerBest[i].hb = ev.hb;
                playerBest[i].pos = ev.pos;
                playerBest[i].angle = bestAng;
                playerBest[i].damage = ev.damage;
                playerBest[i].health = (float)ps.health;
                playerBest[i].fov = bestFov;
                playerBest[i].shotsToKill = ShotsToKill(ev.damage, static_cast<float>(ps.health));
                playerBest[i].fromRecord = true;
                playerBest[i].penetrate = ev.penetrate;
                playerBest[i].visible = ev.visible;
                playerBest[i].record = &rec;
                playerBest[i].snap = nullptr;
                playerBest[i].valid = true;

                // Оптимизация: если нашли летальный урон в этой записи, нет смысла смотреть более старые.
                if (playerBest[i].damage >= playerBest[i].health)
                    break;
            }
        }
        }

        // ---- 2) live snapshot fallback ----
        // Normal Backtrack always scans the live snapshot. The dedicated test
        // mode deliberately disables this branch so every selected shot proves
        // that a historical captured pose won selection.
        if (!g_config.settings.aimbot.backtrack_only_test && liveSupported)
        {
            if (!ps.hitboxes_valid || ps.num_hitboxes <= 0)
                continue;

            int probeHitbox = -1;
            for (int hb = 0; hb < ps.num_hitboxes; ++hb)
            {
                if (!HitgroupEnabled(ps.hitbox_group[hb], effMask))
                    continue;
                probeHitbox = hb;
                break;
            }
            if (probeHitbox < 0)
                continue;

            Vec3  probeAng = CalcAngle(eyePos, ps.hitbox_world_center[probeHitbox]);
            float fov = AngleFOV(oldAngles.x, oldAngles.y, probeAng.x, probeAng.y);
            if (i == s_targetIndex || preferredTarget)
                fov *= 0.5f;
            if (fov > maxFov)
                continue;

            HitboxArrays arr{ ps.hitbox_group, ps.hitbox_studio_index,
                              ps.hitbox_world_center, ps.hitbox_model_min,
                              ps.hitbox_model_max, ps.hitbox_world_min,
                              ps.hitbox_world_max, ps.hitbox_matrix,
                              ps.hitbox_radius, ps.num_hitboxes };
            PointEval ev = EvalHitboxSource(local, ps, arr, eyePos, wepInfo, profile,
                effMask, !(ps.flags & FL_ONGROUND));
            if (!ev.valid)
                continue;

            Vec3  bestAng = CalcAngle(eyePos, ev.pos);
            float bestFov = AngleFOV(oldAngles.x, oldAngles.y, bestAng.x, bestAng.y);
            if (i == s_targetIndex || preferredTarget)
                bestFov *= 0.5f;

            if (!CandidateBeats(ev.damage, static_cast<float>(ps.health),
                ev.visible, bestFov, ps.simtime, false, playerBest[i]))
                continue;

            playerBest[i].index = i;
            playerBest[i].hitbox = ev.hitbox;
            playerBest[i].hitboxGroup = ev.hitboxGroup;
            playerBest[i].entity = ps.entity;
            playerBest[i].simtime = ps.simtime;
            playerBest[i].cmdTick = 0;
            playerBest[i].hb = ev.hb;
            playerBest[i].pos = ev.pos;
            playerBest[i].angle = bestAng;
            playerBest[i].damage = ev.damage;
            playerBest[i].health = (float)ps.health;
            playerBest[i].fov = bestFov;
            playerBest[i].shotsToKill = ShotsToKill(ev.damage, static_cast<float>(ps.health));
            playerBest[i].fromRecord = false;
            playerBest[i].penetrate = ev.penetrate;
            playerBest[i].visible = ev.visible;
            playerBest[i].record = nullptr;
            playerBest[i].snap = &ps;
            playerBest[i].valid = true;
        }
    }

    // Choose the best player using the same priority already used for the
    // per-record scan. This is the current project's CSSO equivalent of the
    // reference final-target comparison.
    // PlayerList: two-pass selection — preferred players win outright when
    // any of them produced a valid candidate this tick.
    PlayerBest best;
    bool hasBest = false;
    for (int pass = 0; pass < 2 && !hasBest; ++pass)
    {
        for (int i = 1; i <= snap.maxClients; ++i)
        {
            if (!playerBest[i].valid || !MeetsMinDamage(playerBest[i]))
                continue;

            if (pass == 0 && !PlayerList::IsPreferred(i))
                continue;

            const auto& cand = playerBest[i];
            if (!hasBest || CandidateBeats(cand.damage, cand.health, cand.visible,
                cand.fov, cand.simtime, cand.fromRecord, best))
            {
                best = cand;
                hasBest = true;
            }
        }
    }

    // DT recharge gate: this command really has an engaged enemy.
    s_hasTargetTick = hasBest;

    // Preserve a currently locked target only when it is still competitive.
    // A target that needs more shots or loses visibility yields immediately to
    // the better candidate; small raw-damage differences alone do not thrash
    // the selection between enemies.
    if (hasBest && s_targetIndex > 0 && s_targetIndex != best.index &&
        playerBest[s_targetIndex].valid &&
        MeetsMinDamage(playerBest[s_targetIndex]))
    {
        const auto& locked = playerBest[s_targetIndex];
        const bool lockedCompetitive =
            locked.shotsToKill <= best.shotsToKill &&
            (locked.visible || !best.visible) &&
            locked.damage >= best.damage * 0.85f;
        if (lockedCompetitive)
            best = locked;
    }

    if (!best.valid)
    {
        s_targetIndex = 0;

        // CUserCmd already contains the player's movement. If no target is
        // available, leave that movement untouched.
        return;
    }

    s_targetIndex = best.index;

    // ---- Corner hitpoint refinement (fatality check_corner_hitpoint) ----
    // An edge multipoint wins selection by damage, but it is the point most
    // sensitive to pose/interp error. Walk it towards the hitbox center and
    // keep the most central point that is still shootable with the same
    // shots-to-kill, then settle halfway between that and the original.
    {
        const Vec3 orig = best.pos;
        const Vec3 center = best.hb.center;
        const Vec3 off = orig - center;
        if (off.x * off.x + off.y * off.y + off.z * off.z > 1.f)
        {
            const int baseStk = ShotsToKill(best.damage, best.health);
            auto tryPoint = [&](const Vec3& pt, PenetrationResult& out) -> bool
                {
                    out = Penetration::Calculate(local, best.entity, eyePos, pt,
                        wepInfo, &best.hb, best.hitboxGroup);
                    if (!out.visible && !out.penetrate)
                        return false;
                    if (!profile.auto_wall && !out.visible)
                        return false;
                    // Never turn a visible shot into a wallbang.
                    if (best.visible && !out.visible)
                        return false;
                    float need = EffectiveRageMinDamage(profile, out.penetrate, out.visible);
                    if (need > best.health) need = best.health;
                    if (need < 1.f) need = 1.f;
                    if (out.damage + 0.1f < need)
                        return false;
                    return ShotsToKill(out.damage, best.health) <= baseStk;
                };

            Vec3 chosen = orig;
            PenetrationResult chosenPen{};
            bool moved = false;
            for (int k = 1; k <= 5; ++k)
            {
                const float f = k / 6.f;
                const Vec3 pt = orig + (center - orig) * f;
                PenetrationResult pr{};
                if (tryPoint(pt, pr))
                {
                    chosen = pt;
                    chosenPen = pr;
                    moved = true;
                }
            }
            if (moved)
            {
                const Vec3 mid = chosen + (orig - chosen) * 0.5f;
                PenetrationResult pr{};
                Vec3 finalPt = chosen;
                PenetrationResult finalPen = chosenPen;
                if (tryPoint(mid, pr))
                {
                    finalPt = mid;
                    finalPen = pr;
                }
                best.pos = finalPt;
                best.angle = CalcAngle(eyePos, finalPt);
                best.damage = finalPen.damage;
                best.visible = finalPen.visible;
                best.penetrate = finalPen.penetrate && !finalPen.visible;
            }
        }
    }

    // One enabled/disabled AutoStop request per valid
    // target. Existing profile key semantics remain, but legacy slide/full
    // modes now preserve their meaning: Slide accurate speed / Full zero.
    const bool autoStopEnabled = profile.auto_stop > 0 &&
        (!profile.auto_stop_on_key || Binds::Active("auto_stop", profile.auto_stop_key, profile.auto_stop_key_mode, "Auto stop"));
    const float maxSpeed = AutoStop::ResolveMaxSpeed(wepInfo, local->IsScoped());

    const bool fireIntent = profile.auto_fire || userAttackHeld;
    // The stop request is NOT armed on plain fire intent. Reference rage
    // brakes only in the tick that actually fires (armed in the commit block
    // below) or, while waiting for accuracy, only when a fully stopped shot
    // would clear the hitchance gate on a visible point. Arming on intent
    // alone froze the player permanently against wallbang/range-limited
    // targets that stopping could never make shootable.
    // Retain stop requests even at low speed: Full suppresses held movement;
    // Slide limits its predicted speed to the weapon accuracy band. The wait
    // branch still releases the brake if even resting HC cannot pass.
    Vec3 curVel = local->GetVelocity();
    const float curSpeed2D = sqrtf(curVel.x * curVel.x + curVel.y * curVel.y);
    s_pendingAutoStop = false;
    s_autoStopMaxSpeed = 0.f;

    const float preStopForward = cmd->forwardmove.Decode();
    const float preStopSide = cmd->sidemove.Decode();
    bool stopAppliedHere = false;

    // Early auto-stop (skeet-style). Previously the brake was armed ONLY in
    // the wait-for-accuracy branch, so it silently did nothing when:
    //  - hitchance already passed while moving -> shot left at full speed;
    //  - the weapon was cycling between shots -> player kept running and the
    //    next shot again started from full speed;
    // which is exactly the "sometimes it just doesn't stop" behaviour.
    // Now every valid target arms the brake when the weapon is ready or will
    // be ready shortly; the wait branch below can still cancel it when even
    // a fully stopped shot could never pass hitchance (no permanent freeze).
    {
        const float readyAt = (std::max)(GetWeaponNextPrimaryAttack(weapon),
                                         local->GetNextAttack());
        const bool readySoon = weaponReady || (readyAt - curtime) < 0.25f;
        if (autoStopEnabled && !inair && weaponUsable && fireIntent &&
            maxSpeed > 1.f && !(cmd->buttons & IN_JUMP) &&
            readySoon && !IsWeaponReloading(weapon))
        {
            s_pendingAutoStop = true;
            s_autoStopMaxSpeed = maxSpeed;
            s_autoStopFull=profile.auto_stop==2 || autoRevolver;
        }
    }

    // ---- Adaptive hitchance (internal_hvh get_final_hitchance) ----
    // After more than 2 consecutive spread misses on this player, raise the
    // required hitchance by max(5, (misses-2) * (100-hc)/6). The
    // Configured HC remains the actual release threshold.
    if (profile.hit_chance >= 1.f)
    {
        constexpr int kShotLimit = 2;
        const int spreadMisses = EventLog::GetSpreadMisses(best.index);
        if (spreadMisses > kShotLimit)
        {
            const float diff = 100.f - profile.hit_chance;
            float add = (spreadMisses - kShotLimit) * (diff / 6.f);
            if (add < 5.f) add = 5.f;
            profile.hit_chance = profile.hit_chance + add > 100.f ? 100.f : profile.hit_chance + add;
        }
    }

    Vec3 hcAngle{};
    HitboxInfo hcBoxes[kMaxHitboxes];
    int numHcBoxes = 0;
    float hitchance = 100.f;
    auto evaluateFireMovement = [&]() -> bool
    {
        // Movement must be final BEFORE computing the shot eye, damage and HC.
        // Fatality re-predicts/fixes its shot after movement changes; previously
        // this project applied the brake after committing angles from the W-held eye.
        if (s_pendingAutoStop && !AutoPeek::IsReturning())
        {
            Ragebot::ApplyAutoStop(cmd, local);
            stopAppliedHere = true;
        }
        EnginePrediction::Refresh(cmd, local);
        eyePos = EnginePrediction::EyePosition(local);
        if (!EnginePrediction::Reliable())
        {
            DBGLOG("[rage] BLOCK local-move/duck prediction uncertain cmd=%d\n", cmd->command_number);
            return false;
        }
        const PenetrationResult firePen = Penetration::Calculate(local, best.entity,
            eyePos, best.pos, wepInfo, &best.hb, best.hitboxGroup);
        float fireNeed = EffectiveRageMinDamage(profile, firePen.penetrate, firePen.visible);
        fireNeed = (std::max)(1.f, (std::min)(fireNeed, best.health));
        if ((!firePen.visible && !firePen.penetrate) ||
            (!profile.auto_wall && !firePen.visible) || firePen.damage + 0.1f < fireNeed)
        {
            DBGLOG("[rage] BLOCK changed-eye penetration cmd=%d dmg=%.1f need=%.1f\n",
                cmd->command_number, firePen.damage, fireNeed);
            return false;
        }
        best.damage = firePen.damage;
        best.visible = firePen.visible;
        best.penetrate = firePen.penetrate && !firePen.visible;
        best.angle = CalcAngle(eyePos, best.pos);

        // Recoil compensation: server fires along EyeAngles + aimPunch * 2.
        // The bullet itself travels along the UNcompensated direction (eye →
        // point), so hitchance must be evaluated on that ray — not on the
        // compensated command angle, which is offset by -punch*2 and made HC
        // look at empty space while spraying / right after a shot.
        Vec3 recoilOffset{ 0.f, 0.f, 0.f };
        if (SDK::nPunchAngleOffset > 0 && local)
        {
            const Vec3 punch = *reinterpret_cast<const Vec3*>(
                reinterpret_cast<uintptr_t>(local) + SDK::nPunchAngleOffset);
            if (std::isfinite(punch.x) && std::isfinite(punch.y))
            {
                recoilOffset.x = punch.x * SDK::flRecoilScale;
                recoilOffset.y = punch.y * SDK::flRecoilScale;
            }
        }
        best.angle.x -= recoilOffset.x;
        best.angle.y -= recoilOffset.y;
        ClampAngles(best.angle);
        // HC must follow the ACTUAL command+punch ray. At steep angles the
        // compensated command can clamp at +/-89; the desired target angle then
        // no longer equals the direction the server will fire.
        hcAngle = {best.angle.x + recoilOffset.x, best.angle.y + recoilOffset.y, 0.f};
        const Vec3 actualRayEnd = eyePos + AngleVectors(hcAngle) * wepInfo->range;
        const PenetrationResult actualRayPen = Penetration::Calculate(local, best.entity,
            eyePos, actualRayEnd, wepInfo, &best.hb, best.hitboxGroup);
        const float actualNeed = (std::max)(1.f, (std::min)(
            EffectiveRageMinDamage(profile, actualRayPen.penetrate, actualRayPen.visible), best.health));
        if ((!actualRayPen.visible && !actualRayPen.penetrate) ||
            (!profile.auto_wall && !actualRayPen.visible) || actualRayPen.damage + 0.1f < actualNeed)
            return false;
        best.damage = actualRayPen.damage;
        best.visible = actualRayPen.visible;
        best.penetrate = actualRayPen.penetrate && !actualRayPen.visible;

        // ------------------------------------------------------------------
        //  Hitchance — evaluate the exact point/hitbox that rage will fire at.
        //  The former center + whole-player proxy and standing 100% override could
        //  authorise a capsule-edge multipoint which the real bullet then missed.
        // ------------------------------------------------------------------
        numHcBoxes = 0;
        if (best.record)
        {
            for (int hb = 0; hb < best.record->num_hitboxes && numHcBoxes < kMaxHitboxes; ++hb)
            {
                if (best.record->hitbox_studio_index[hb] >= 0)
                    hcBoxes[numHcBoxes++] = RecordHitbox(*best.record, hb);
            }
        }
        else if (best.snap)
        {
            for (int hb = 0; hb < best.snap->num_hitboxes && numHcBoxes < kMaxHitboxes; ++hb)
            {
                if (best.snap->hitbox_studio_index[hb] >= 0)
                    hcBoxes[numHcBoxes++] = SnapshotHitbox(*best.snap, hb);
            }
        }

        hitchance = 100.f;
        if (profile.hit_chance > 0.f)
        {
            // Native RunCommand has already advanced weapon accuracy for this
            // movement frame. Calling UpdateAccuracyPenalty again would decay
            // the penalty twice and authorize shots from an artificial cone.
            hitchance = numHcBoxes > 0
                ? Penetration::CheckHitchance(weapon, local, best.entity, eyePos,
                    hcAngle, best.pos, wepInfo, profile.hit_chance, hcBoxes,
                    numHcBoxes, &best.hb, best.hitboxGroup, best.penetrate, -1.f,
                    (std::max)(1.f, (std::min)(EffectiveRageMinDamage(profile, best.penetrate, best.visible), best.health)))
                : 0.f;

            // Reference rage scans more than one point before it commits a wait.
            // The old target selected a high-damage edge multipoint once and then
            // waited forever at 69-73% even though the same hitbox center was
            // shootable. Retry the center ONLY in that stalled state.
            //
            // The retry must never run while the selected point already passes
            // HC: the center almost always has the higher raw HC (deepest point
            // of the capsule, shortest ray), so an unconditional retry replaced
            // every multipoint with the center before the commit and the feature
            // effectively never fired.
            //
            // Autowall is permission to penetrate, not a center-only scan.
            // A stalled edge can have a usable center even with autowall on.
            const Vec3 centerDelta=best.pos-best.hb.center;
            const bool pointStalled = hitchance + 0.01f < profile.hit_chance;
            if (pointStalled && centerDelta.x*centerDelta.x+
                centerDelta.y*centerDelta.y+centerDelta.z*centerDelta.z>0.0001f)
            {
                const Vec3 center = best.hb.center;
                const Vec3 centerAngle = CalcAngle(eyePos, center);
                const PenetrationResult centerPen = Penetration::Calculate(local, best.entity,
                    eyePos, center, wepInfo, &best.hb, best.hitboxGroup);
                float centerNeed = EffectiveRageMinDamage( profile,
                    centerPen.penetrate, centerPen.visible );
                if (centerNeed > best.health)
                    centerNeed = best.health;
                if (centerNeed < 1.f)
                    centerNeed = 1.f;
                const bool centerUsable = (centerPen.visible || centerPen.penetrate) &&
                    (profile.auto_wall || centerPen.visible) && centerPen.damage + 0.1f >= centerNeed;
                if (centerUsable && numHcBoxes > 0)
                {
                    Vec3 centerCommand{centerAngle.x-recoilOffset.x,
                        centerAngle.y-recoilOffset.y, 0.f};
                    ClampAngles(centerCommand);
                    const Vec3 centerBullet{centerCommand.x+recoilOffset.x,
                        centerCommand.y+recoilOffset.y, 0.f};
                    const float centerHc = Penetration::CheckHitchance(weapon, local,
                        best.entity, eyePos, centerBullet, center, wepInfo, profile.hit_chance,
                        hcBoxes, numHcBoxes, &best.hb, best.hitboxGroup,
                        centerPen.penetrate && !centerPen.visible, -1.f, centerNeed);
                    // Switch only when the center would commit NOW — this is a
                    // stall recovery, not a better-point search. While both
                    // points wait, the multipoint keeps its aim so the user's
                    // configured point layout is the one that fires once the
                    // cone settles.
                    if (centerHc + 0.01f >= profile.hit_chance &&
                        centerHc > hitchance + 0.1f)
                    {
                        best.pos = center;
                        hcAngle = centerBullet;
                        best.angle = centerCommand;
                        best.damage = centerPen.damage;
                        best.visible = centerPen.visible;
                        best.penetrate = centerPen.penetrate && !centerPen.visible;
                        hitchance = centerHc;
                    }
                }
            }
        }
        return true;
    };
    if (!evaluateFireMovement()) return;

    // Chance below threshold — do not fire this tick. Braking below is not
    // unconditional: it happens only when stopping would actually raise the
    // shot over the threshold (see the wait-brake comment inside).
    //
    // The hitchance gate runs regardless of auto_fire: even in manual-fire
    // mode rage strips the raw attack so the shot cannot leave with the
    // player's old viewangles before a shootable command exists.
    const Vec3  dbgVel = local->GetVelocity();
    const float speed = DebugSpeed(dbgVel);
    // Optional per-profile fire delay: minimum ticks between committed rage
    // shots. Paces rapid weapons whose cycle timer re-arms long before the
    // fire cone recovers (the max-RPM spray of misses). 0 = off — the commit
    // cadence is then exactly the weapon's own cycle + hitchance, nothing
    // forced. This replaces the removed hard DT/cone holds that throttled
    // the autopeek + doubletap peek rhythm.
    static int s_lastRageFireCommand = 0;

    // Semi-auto pacing. The weapon cooldown re-arms far sooner than the
    // spread cone recovers: CanShoot clears the moment the cycle elapses
    // while the fire penalty is still open, so auto-fire re-commits every
    // cooldown tick and the pistol machine-guns an unrecovered cone instead
    // of evaluating whether a bullet can actually land. When the profile has
    // no explicit fire_delay, pace semi weapons at one full cycle plus a
    // recovery margin so hitchance gets a settled cone to evaluate.
    static void* s_paceWeapon   = nullptr;
    static int   s_pacedDelay   = 0;
    if ( s_paceWeapon != weapon )
    {
        s_paceWeapon = weapon;
        const float paceCycle = local->IsScoped() &&
            wepInfo->cycleTimeAlt > 0.001f ? wepInfo->cycleTimeAlt : wepInfo->cycleTime;
        const int paceTicks = static_cast<int>( ceilf( paceCycle * 64.f ) );
        s_pacedDelay = paceTicks + 10; // ~156 ms cone-decay margin over the cycle
    }
    int effectiveFireDelay = profile.fire_delay;
    // Revolvers pace themselves: the cycle timer plus the charge session
    // already gate the next commit, an extra pistol margin would only stall.
    // The forced "cycle + 10 ticks" pistol/sniper pacing was removed: it made
    // every follow-up shot wait ~150 ms AFTER the weapon was already ready
    // (and longer, since it was computed once per weapon). The hitchance gate
    // already blocks firing into an unrecovered cone, so the next shot now
    // goes out the moment the weapon cycle ends and HC passes — like other
    // rage cheats. profile.fire_delay still works as an explicit manual cap.
    (void)s_pacedDelay;

    const bool fireDelayHold = effectiveFireDelay > 0 && s_lastRageFireCommand > 0 &&
        ( cmd->command_number - s_lastRageFireCommand ) < effectiveFireDelay;
    // Borderline epsilon: a hitchance within 0.25 of the configured threshold
    // counts as passing. The strict < comparison hung forever on targets
    // whose best point peaks exactly at the threshold (74.98 vs 75.0 printed
    // as 75.0/75.0) — STATE spam, no shot, no brake release.
    //
    // Scoped weapons must take the shot scoped: the unscoped cone is so wide
    // that hitchance "passes" against it (75% of a huge cone still counts
    // 75%) and every committed bullet sprays. Force the scope path (zoom +
    // held fire) before any commit can happen — auto_scope previously only
    // ran inside this wait block and never triggered when the fat-cone HC
    // already passed.
    const bool needsScope = profile.auto_scope && IsZoomableWeapon( weapon ) &&
        !local->IsScoped();
    // A weapon at resting accuracy still has spread and can be occluded.
    // Do not silently turn a configured 95/100% shot into an 83% release.
    const float hcCfg = profile.hit_chance;
    const bool hcPasses = hitchance + 0.01f >= hcCfg;

    const bool waitForAccuracy = !hcPasses ||
        fireDelayHold || needsScope;

    // Query a native hypothetical rest cone without advancing accuracy or
    // changing this command's prediction. Never cache GetInaccuracy() based
    // on the pre-command speed: held movement may already have accelerated
    // the predicted velocity, poisoning a supposed "resting" sample.

    // === DEBUG: full fire-gate state, once every 8 ticks. When a standing
    // stall happens this line names the exact gate holding the shot:
    //   ready=0 npa>0   — weapon next-attack timer (delta visible)
    //   ready=0 atk>0   — player m_flNextAttack (delta visible)
    //   wait=1          — hitchance below threshold (real value printed)
    //   ready=1 wait=0 with no FIRE line — button/silent path (semi/af/hold)
    if (DebugTick(8))
    {
        const float npaDelta = GetWeaponNextPrimaryAttack(weapon) - curtime;
        const float atkDelta = *(float*)((uintptr_t)local + nNextAttackOffset) - curtime;
        DBGLOG("[rage] GATE tick=%d tgt=%d hc=%.2f/<%.1f ready=%d npa=%+.3f "
            "atk=%+.3f wait=%d semi=%d af=%d hold=%d clip=%d rl=%d inacc=%.4f spd=%.1f pred=%.1f brake=%d full=%d fwd=%.1f side=%.1f\n",
            seq, s_targetIndex, hitchance, profile.hit_chance,
            weaponReady ? 1 : 0, npaDelta, atkDelta, waitForAccuracy ? 1 : 0,
            semiAuto ? 1 : 0, profile.auto_fire ? 1 : 0, userAttackHeld ? 1 : 0,
            GetWeaponClip1(weapon), IsWeaponReloading(weapon) ? 1 : 0,
            Penetration::GetInaccuracy(weapon), speed,
            DebugSpeed(EnginePrediction::PredictedVelocity()), stopAppliedHere ? 1 : 0,
            s_autoStopFull ? 1 : 0,
            cmd->forwardmove.Decode(), cmd->sidemove.Decode());
    }

    // A normal weapon cooldown does not fire, scope or alter movement. R8 is
    // handled by its independent active-charge branch immediately above this
    // gate, because this project exposes stale next-primary values while R8 is
    // being cocked locally.
    if (!weaponReady)
        return;

    if (waitForAccuracy)
    {
        // HC controls automatic commits; the guard preserves original LMB.
        if ( userAttackEdge && !profile.auto_fire )
            cmd->buttons |= IN_ATTACK;
        else
            cmd->buttons &= ~IN_ATTACK;

        if (profile.auto_scope && IsZoomableWeapon(weapon) && !local->IsScoped())
        {
            cmd->buttons &= ~IN_ATTACK;
            cmd->buttons |= IN_ATTACK2;
        }

        // Wait-brake: brake only while a fully stopped shot would clear the
        // gate that is holding the fire. For visible points that is the
        // hitchance threshold evaluated with the standing cone; for wallbang
        // points the same prediction runs the full-seed penetration path
        // — its HC is also movement-limited through the cone, and without the
        // brake a moving player could never shoot through a wall at all.
        // When even a stopped shot cannot pass, braking could never create
        // the shot and would only freeze the player, so movement stays free.
        if (autoStopEnabled && !inair && weaponUsable && fireIntent &&
            maxSpeed > 1.f && !(cmd->buttons & IN_JUMP))
        {
            const float standInacc = Penetration::GetRestingInaccuracy(weapon);

            bool stopEnablesShot;
            if (standInacc >= 0.f && numHcBoxes > 0)
            {
                // Same seeds/geometry as the live pass, with the cone the
                // weapon would have once fully stopped. CheckHitchance never
                // advances accuracy state, so this is a pure prediction.
                const float stoppedHc = Penetration::CheckHitchance(weapon, local,
                    best.entity, eyePos, hcAngle, best.pos, wepInfo,
                    profile.hit_chance, hcBoxes, numHcBoxes, &best.hb,
                    best.hitboxGroup, best.penetrate, standInacc,
                    (std::max)(1.f, (std::min)(EffectiveRageMinDamage(profile, best.penetrate, best.visible), best.health)));
                stopEnablesShot = stoppedHc + 0.01f >= profile.hit_chance;
            }
            else
            {
                // Native rest query unavailable: conservatively fall back to
                // "the current cone is movement-dominated".
                stopEnablesShot = Penetration::GetInaccuracy(weapon) > 0.05f;
            }

            if (stopEnablesShot)
            {
                s_pendingAutoStop = true;
                s_autoStopMaxSpeed = maxSpeed;
                s_autoStopFull=profile.auto_stop==2 || autoRevolver;
            }
            else
            {
                // Stopping can never make this point shootable: keep moving.
                s_pendingAutoStop = false;
                s_autoStopMaxSpeed = 0.f;
                if (stopAppliedHere)
                {
                    cmd->forwardmove.Encode(preStopForward);
                    cmd->sidemove.Encode(preStopSide);
                    EnginePrediction::Refresh(cmd, local);
                }
            }
        }

        if (profile.auto_stop > 0 && DebugTick(8))
            DBGLOG("[autostop] WAIT-ACC hc=%.1f/<%.1f pending=%d speed=%.1f\n",
                hitchance, profile.hit_chance, s_pendingAutoStop ? 1 : 0, speed);
        return;
    }

    // Any shot-tick counter-strafe was applied and refreshed before HC.
    // No downstream movement module may replace that validated movement.

    // A render history pose keeps its capture tick. Never synthesize a new
    // rewind tick from the entity's network timestamp. Revalidate at commit.
    // R8: everything passed but the hammer is still winding up — keep the
    // fire-tick brake armed above and let RvrCtl keep cocking. The shot is
    // committed on the first cocked command.
    if (autoRevolver && !RvrCtl::Ready(weapon, curtime))
        return;

    LagCompensation::Timing commitTiming;
    const bool hasCommitTiming = LagCompensation::ReadTiming(commitTiming);
    if (lagTiming.available && !hasCommitTiming) return;
    const auto& targetTimeline = g_playerRecords[best.index].timeline;
    const float breakTime = targetTimeline.available ? targetTimeline.brokenSince : 0.f;
    const float latestSim = snap.players[best.index].simtime;
    const int poseTick = best.fromRecord && best.record
        ? best.record->cmd_tick : snap.cmdTickCount;
    if (hasCommitTiming && LagCompensation::CheckCommandSupport(commitTiming,
        poseTick, latestSim, breakTime) != LagCompensation::Support::Supported) return;
    if (best.fromRecord)
    {
        const auto decision = best.record && hasCommitTiming
            ? LagCompensation::ValidateCaptured(commitTiming, best.record->cmd_tick,
                best.record->capture_lerp) : LagCompensation::Decision{};
        if (!decision) return;
        cmd->tick_count = decision.tick;
        DBGLOG("[lagcomp] FIRE cmd=%d target=%d updateSim=%.4f poseTime=%.4f tick=%d server=%d "
            "lerp=%.4f out=%.4f in=%.4f delta=%.4f limits=%s\n",
            cmd->command_number, best.index, best.simtime, decision.targetTime, decision.tick,
            commitTiming.serverTick, commitTiming.lerp, commitTiming.outgoing,
            commitTiming.incoming, decision.delta,
            commitTiming.serverLimitsKnown ? "cvars" : "server-defaults");
    }

    // Semi-auto weapons must never bypass the profile's Auto Fire setting.
    // With Auto Fire off, a pistol creates exactly one rage pulse from a fresh
    // physical click; the raw held button was stripped above so it cannot
    // machine-gun commands with stale target/angle state. With Auto Fire on,
    // CanShoot plus the forced release tick still supplies a real press edge
    // only after the weapon's own next-primary timer has elapsed.
    if (profile.auto_fire || (semiAuto && userAttackEdge))
        cmd->buttons |= IN_ATTACK;

    // A candidate is not a shot. Do not apply angles, fire commits, DT tokens
    // or fire-tick autostop unless this exact command actually carries attack.
    // This is unconditional: non-silent manual/semi paths need the same guard.
    const bool isShooting = (cmd->buttons & IN_ATTACK) != 0;
    if (!isShooting)
    {
        if (profile.auto_stop > 0 && DebugTick(8))
            DBGLOG("[autostop] RELEASE(no-shot) hc=%.1f pending=%d\n",
                hitchance, s_pendingAutoStop ? 1 : 0);
        return;
    }



    // Auto-stop is a current-command request. Once this command fires, the
    // next command begins from the user's original movement unless accuracy
    // again requires the configured slide/full band.

    if (isShooting && best.penetrate)
    {
        DBGLOG("[autowall] FIRE tgt=%d hb=%d dmg=%.0f/hp=%.0f recAge=%.3f strict=1\n",
            best.index, best.hitbox, best.damage, best.health,
            (best.fromRecord && best.simtime > 0.f) ? (curtime - best.simtime) : 0.f);
    }

    Vec3 newAngles = { best.angle.x, best.angle.y, 0.f };
    CorrectMovement(cmd, oldAngles, newAngles);
    SetYaw(cmd, best.angle.y);
    SetPitch(cmd, best.angle.x);

    // ====== TEMP DIAGNOSTIC ======
    // Prints exactly where the bullet is aimed on the firing command, so a real
    // miss can be matched to the aim point / rewind tick instead of guessing.
    static int s_fireDbgN = 0;
    if ((++s_fireDbgN & 15u) == 1u)
    {
        static const char* const kHitNames[] = {
            "generic", "head", "chest", "stomach", "larm", "rarm", "lleg", "rleg" };
        const int hg = best.hitboxGroup;
        DBGLOG("[aim] cmd=%d tgt=%d %s src=(%.1f,%.1f,%.1f) aim=(%.1f,%.1f,%.1f) "
            "ang=(%.1f,%.1f) hb=%d rec=%d tck=%d recAge=%.3f pen=%d vis=%d spd=%.1f\n",
            cmd->command_number, best.index,
            (hg >= 0 && hg <= 7) ? kHitNames[hg] : "?",
            eyePos.x, eyePos.y, eyePos.z,
            best.pos.x, best.pos.y, best.pos.z,
            best.angle.x, best.angle.y, best.hitbox,
            best.fromRecord ? 1 : 0, cmd->tick_count,
            (best.fromRecord && best.simtime > 0.f) ? (curtime - best.simtime) : 0.f,
            best.penetrate ? 1 : 0, best.visible ? 1 : 0,
            DebugSpeed(dbgVel));
    }
    // ====== /TEMP DIAGNOSTIC ======

    // DT runs after Rage in CreateMove. Mark this exact command only after all
    // target, min-damage, penetration, HC, scope and readiness gates passed and
    // the final real rage angles were written.
    // Already applied before HC. Never change the firing movement after
    // the shot eye and cone were validated.
    s_pendingAutoStop = false;
    s_autoStopMaxSpeed = 0.f;
    s_lastRageFireCommand = cmd->command_number;
    if ( semiAuto )
        s_edgeLastCommit = cmd->command_number;
    s_lastRageFireTime = curtime;
    s_rageFireCommit.commandNumber = cmd->command_number;
    s_rageFireCommit.targetIndex = best.index;
    s_rageFireCommit.hitbox = best.hitbox;
    s_rageFireCommit.fromRecord = best.fromRecord;
    s_rageFireCommit.penetrated = best.penetrate;
    s_rageFireCommit.simtime = best.simtime;
    s_rageFireCommit.damage = best.damage;
    s_rageFireCommit.hitchance = hitchance;
    s_rageFireCommit.pos = best.pos;
    s_rageFireCommit.aim = newAngles;

    // Original command tick was captured before any rage rewind.
    const int backtrackTicks = best.fromRecord
        ? (std::max)(0, snap.cmdTickCount - cmd->tick_count) : 0;
    EventLog::OnRageShot(best.index, best.hitboxGroup, hitchance, best.damage,
        eyePos, best.pos, best.fromRecord, cmd->command_number, autoRevolver,
        backtrackTicks);

    // === DEBUG (autostop): a shot is actually going out this tick ===
    if (profile.auto_stop > 0 && DebugTick(8))
    {
        Vec3 dbgVel = local->GetVelocity();
        DBGLOG("[autostop] FIRE hc=%.1f inacc=%.4f speed=%.1f "
            "band=%.1f dmg=%.0f/hp=%.0f stk=%d recAge=%s%.3f\n",
            hitchance, Penetration::GetInaccuracy(weapon),
            DebugSpeed(dbgVel), maxSpeed * kSupremacyStopRatio, best.damage, best.health,
            best.shotsToKill,
            (best.fromRecord && best.simtime > 0.f) ? "" : "-",
            (best.fromRecord && best.simtime > 0.f)
            ? (curtime - best.simtime)
            : 0.f);
    }
}

// ------------------------------------------------------------------
//  ApplyAutoStop — consumes the single current-command request created by
//  the rage scan. It runs before anti-aim; anti-aim corrects movement for its
//  final yaw afterwards.
// ------------------------------------------------------------------
void Ragebot::ApplyAutoStop(void* pCmd, void* pLocalPlayer)
{
    if (!s_pendingAutoStop || !pCmd || !pLocalPlayer || !EnginePrediction::Reliable())
        return;

    auto* cmd = reinterpret_cast<CUserCmd*>(pCmd);
    auto* local = reinterpret_cast<C_BaseEntity*>(pLocalPlayer);

    // AutoPeek return and airborne movement remain authoritative in the Rage
    // layer. A committed shot no longer skips the brake: under the stop-on-
    // the-shot-tick contract the fire command is exactly where the
    // counter-strafe belongs. Slide/Full movement and friction-aware speed
    // limiting are owned exclusively by AutoStop.
    if (AutoPeek::IsReturning() || !(local->GetFlags() & FL_ONGROUND) ||
        IsOnLadderOrNoclip(local) || (cmd->buttons & IN_USE))
        return;

    AutoStop::ApplyMovement(pCmd, pLocalPlayer, GetYaw(cmd),
        s_autoStopMaxSpeed, s_autoStopFull);
    s_lastStopCmd = cmd->command_number;
    s_lastStopMaxSpeed = s_autoStopMaxSpeed;
    s_lastStopFull=s_autoStopFull;

    // Consume immediately — one movement correction per arming. The next
    // command independently re-evaluates target/readiness and selected mode.
    // RunRage() will re-arm on the next tick if targets remain.
    s_pendingAutoStop = false;
    s_autoStopMaxSpeed = 0.f;
}

// ------------------------------------------------------------------
//  Main entry point
// ------------------------------------------------------------------
void Ragebot::Run(void* pCInput, int seq,
    void* pLocalPlayer, int localTeam,
    const EntitySnapshot& snap)
{
    s_pendingAutoStop = false;
    s_autoStopMaxSpeed = 0.f;
    s_hasTargetTick = false;
    s_rageFireCommit = {};
    s_minDamageOverrideActive = false;
    s_minDamageOverrideShowIndicator = false;
    s_minDamageOverrideValue = 0;
    if ( !g_config.settings.aimbot.rage )
    {
        s_minDamageOverrideToggleActive = false;
        s_minDamageOverrideKeyWasDown = false;
        s_minDamageOverrideToggleProfile = -1;
        s_minDamageOverrideToggleKey = 0;
    }

    if (!g_config.settings.aimbot.enabled || !pLocalPlayer || localTeam == 0)
        return;

    if (!g_pEngine || !g_pEngine->IsInGame() || !g_pEngine->IsConnected())
        return;

    if (g_config.settings.aimbot.rage)
        RunRage(pCInput, seq, pLocalPlayer, localTeam, snap);
    else
        RunLegit(pCInput, seq, pLocalPlayer, localTeam, snap);
}

bool Ragebot::HasCommittedRageShot( int commandNumber )
{
    return commandNumber > 0 && commandNumber == s_rageFireCommit.commandNumber;
}

bool Ragebot::HasTargetThisTick()
{
    return s_hasTargetTick;
}

float Ragebot::GetLastRageFireTime()
{
    return s_lastRageFireTime;
}

float Ragebot::GetCommittedRageShotSimtime()
{
    return s_rageFireCommit.simtime;
}

bool Ragebot::GetCommittedRageShotFromRecord()
{
    return s_rageFireCommit.fromRecord;
}

// World-space aimpoint of the current committed shot.
const Vec3& Ragebot::GetCommittedRageShotPoint()
{
    return s_rageFireCommit.pos;
}

const char* Ragebot::GetActiveRageProfileName()
{
    return cfg::RageWeaponProfileName( s_activeRageProfile );
}

bool Ragebot::IsMinDamageOverrideIndicatorActive()
{
    return s_minDamageOverrideShowIndicator;
}

int Ragebot::GetMinDamageOverrideValue()
{
    return s_minDamageOverrideValue;
}

