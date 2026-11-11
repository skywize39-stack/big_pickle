#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "utils/Math.hpp"

struct PlayerSnapshot;

namespace LagCompensation
{
    struct Timing
    {
        float interval = 0.f, lerp = 0.f, outgoing = 0.f, incoming = 0.f;
        float maxUnlag = 0.f, teleportDistance = 0.f;
        int serverTick = 0, choked = 0;
        bool available = false;
        bool serverLimitsKnown = false;
    };
    enum class Rejection { None, Unavailable, Invalid, Future, Expired, ServerFallback };
    struct Decision
    {
        int tick = 0;
        float targetTime = 0.f, delta = 0.f;
        Rejection reason = Rejection::Unavailable;
        explicit operator bool() const { return reason == Rejection::None; }
    };

    // Pure wire-time calculation. Never clamp a bad timestamp to a different
    // pose: reject it instead. Server StartLagCompensation subtracts the exact
    // lerp float from tick_count*interval, NOT a rounded lerp tick count.
    inline Decision Validate(const Timing& t, float simtime, int extraTicks = 0)
    {
        Decision d;
        if (!t.available) return d;
        d.reason = Rejection::Invalid;
        if (!std::isfinite(simtime) || simtime <= 0.f ||
            !std::isfinite(t.interval) || t.interval <= 0.f || t.interval > .1f ||
            !std::isfinite(t.lerp) || t.lerp < 0.f || t.lerp > 1.f ||
            !std::isfinite(t.outgoing) || t.outgoing < 0.f || t.outgoing > 2.f ||
            !std::isfinite(t.incoming) || t.incoming < 0.f || t.incoming > 2.f ||
            !std::isfinite(t.maxUnlag) || t.maxUnlag <= 0.f || t.maxUnlag > 1.f ||
            t.serverTick <= 0 || t.choked < 0 || t.choked > 64 ||
            extraTicks < 0 || extraTicks > 64) return d;
        const double wire = std::floor((double(simtime) + t.lerp) / t.interval + .5);
        if (wire <= 0 || wire > (std::numeric_limits<int>::max)()) return d;
        d.tick = static_cast<int>(wire);
        const double seen = double(t.serverTick) * t.interval;
        // Packet time is older than the next server command by the observed
        // round trip and queued commands. This is an estimate, not access to
        // a remote server's globals. Keep a one-tick safety margin below .2s.
        const double rtt = double(t.outgoing) + t.incoming;
        const double arrival = seen + rtt + (t.choked + extraTicks) * double(t.interval);
        d.targetTime = float(wire * t.interval - t.lerp);
        if (simtime > seen + t.interval || wire*t.interval > arrival + 8*t.interval)
        { d.reason = Rejection::Future; return d; }
        if (arrival - d.targetTime > t.maxUnlag)
        { d.reason = Rejection::Expired; return d; }
        const double correct = (std::clamp)(rtt + t.lerp, 0., double(t.maxUnlag));
        d.delta = float(correct - (arrival - d.targetTime));
        if (std::abs(d.delta) > .2 - t.interval)
        { d.reason = Rejection::ServerFallback; return d; }
        d.reason = Rejection::None;
        return d;
    }
    // A captured render pose belongs to its ORIGINAL command tick. Its
    // m_flSimulationTime identifies an update, not the time of its bones.
    // Reject interpolation changes instead of changing the pose's wire tick.
    inline Decision ValidateCaptured(const Timing& t, int captureTick,
        float captureLerp, int extraTicks = 0)
    {
        if (!t.available) return {};
        if (captureTick <= 0 || !std::isfinite(captureLerp) ||
            !std::isfinite(t.lerp) || std::abs(captureLerp-t.lerp) > .000001f)
        { Decision d; d.reason = Rejection::Invalid; return d; }
        auto d = Validate(t, float(double(captureTick)*t.interval-captureLerp), extraTicks);
        if (d && d.tick != captureTick) d.reason = Rejection::Invalid;
        return d;
    }
    // Observed network timeline, not an angle resolver or movement predictor.
    // Raw network origin is essential: interpolated abs-origin changes even
    // when no new remote command/state arrived.
    struct Timeline
    {
        uintptr_t entity = 0, model = 0;
        int hitboxSet = -1;
        float simtime = 0.f, brokenSince = 0.f;
        Vec3 origin{};
        int stepTicks = 0;
        bool available = false;
    };
    inline bool Observe(Timeline& line, uintptr_t entity, uintptr_t model,
        int hitboxSet, float simtime, const Vec3& rawOrigin,
        float interval, float teleportDistance)
    {
        if (!entity || !std::isfinite(simtime) || simtime <= 0.f ||
            !std::isfinite(rawOrigin.x) || !std::isfinite(rawOrigin.y) ||
            !std::isfinite(rawOrigin.z) || !std::isfinite(interval) || interval <= 0.f ||
            !std::isfinite(teleportDistance) || teleportDistance <= 0.f)
        { line = {}; return false; }
        const bool sameLife = line.available && line.entity == entity &&
            line.model == model && line.hitboxSet == hitboxSet &&
            simtime >= line.simtime-interval*.25f;
        if (!sameLife)
        {
            line = {};
            line.entity = entity; line.model = model; line.hitboxSet = hitboxSet;
            line.simtime = simtime; line.origin = rawOrigin; line.available = true;
            return false; // new identity/time epoch: discard any old pose chain
        }
        if (simtime-line.simtime < interval*.25f) return true;
        const double ticks = (double(simtime)-line.simtime)/interval;
        line.stepTicks = static_cast<int>((std::min)(64., std::floor(ticks+.5)));
        const Vec3 change = rawOrigin-line.origin;
        const bool continuous = double(change.x)*change.x + double(change.y)*change.y <=
            double(teleportDistance)*teleportDistance;
        if (!continuous) line.brokenSince = simtime;
        line.simtime = simtime; line.origin = rawOrigin;
        return continuous;
    }
    enum class Support { Supported, Unknown, BeyondUpdate, BrokenChain };
    inline Support CheckSupport(const Timing& t, double poseTime, float latestSimulation,
        float brokenSince = 0.f)
    {
        if (!t.available || !std::isfinite(t.interval) || t.interval <= 0.f ||
            !std::isfinite(poseTime) || poseTime <= 0. ||
            !std::isfinite(latestSimulation) || latestSimulation <= 0.f ||
            !std::isfinite(brokenSince) || brokenSince < 0.f)
            return Support::Unknown;
        // Only rounding uncertainty is allowed. A render pose newer than the
        // newest received simulation state is not certified by an empty ray/HC.
        const double rounding = t.interval*.5;
        if (poseTime > double(latestSimulation)+rounding)
            return Support::BeyondUpdate;
        if (brokenSince > 0.f && poseTime < double(brokenSince)-rounding)
            return Support::BrokenChain;
        return Support::Supported;
    }
    inline Support CheckCommandSupport(const Timing& t, int commandTick,
        float latestSimulation, float brokenSince = 0.f)
    {
        if (commandTick <= 0 || !std::isfinite(t.lerp) || t.lerp < 0.f)
            return Support::Unknown;
        return CheckSupport(t, double(commandTick)*t.interval-t.lerp,
            latestSimulation, brokenSince);
    }
    // Called only at NET_UPDATE_END. Copying the result later avoids treating
    // m_vecOrigin after client interpolation as a new raw network endpoint.
    bool ObserveNetworkState(int index, const PlayerSnapshot& sample, const Timing& timing);
    bool CopyNetworkState(int index, uintptr_t entity, float simtime, PlayerSnapshot& out);
    bool ReadRenderTime(float& time);
    bool ReadTiming(Timing& timing);
    bool NativeBuildReady();
    void* FrameStageTarget();
    // Unavailable timing disables historical shots only, never manual input.
    void Reset();
}
