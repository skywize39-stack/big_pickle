#include "features/combat/rage/LagCompensation.hpp"
#include "features/combat/aimbot/LagRecord.hpp"
#include "sdk/SDK.hpp"
#include "sdk/build/CssoBuild.hpp"
#include <Windows.h>

namespace LagCompensation
{
    namespace
    {
        template<class T> T& At(uintptr_t b, uintptr_t o)
        { return *reinterpret_cast<T*>(b + o); }
        bool Image(uintptr_t b, SDK::Build::ModuleIdentity id)
        {
            if (!b || At<uint16_t>(b, 0) != 0x5A4D) return false;
            const auto p = At<uint32_t>(b, 0x3C);
            return p >= 0x40 && p <= 0x1000 && At<uint32_t>(b, p) == 0x4550 &&
                At<uint32_t>(b, p+8) == id.timestamp && At<uint32_t>(b, p+0x50) == id.imageSize;
        }
        bool ReadVar(const char* name, float& value)
        {
            auto* cvars = SDK::ICvar::Get();
            auto* var = cvars ? cvars->FindConVar(name) : nullptr;
            return var && var->TryGetFloat(value);
        }
    }
    bool NativeBuildReady()
    {
        __try
        {
            return Image(SDK::g_uClientBase, SDK::Build::Client) &&
                Image(SDK::g_uEngineBase, SDK::Build::Engine);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* FrameStageTarget()
    {
        __try
        {
            using namespace SDK;
            if (!NativeBuildReady()) return nullptr;
            const auto api = At<uintptr_t>(g_uEngineBase, Build::LagCompensation::ClientInterface);
            if (!api) return nullptr;
            const auto table = At<uintptr_t>(api, 0);
            if (table < g_uClientBase || table > g_uClientBase + Build::Client.imageSize - (Build::LagCompensation::FrameStageSlot + 1)*sizeof(uintptr_t))
                return nullptr;
            const auto target = At<uintptr_t>(table, Build::LagCompensation::FrameStageSlot*sizeof(uintptr_t));
            if (target < g_uClientBase || target >= g_uClientBase + Build::Client.imageSize)
                return nullptr;
            return reinterpret_cast<void*>(target);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    bool ReadRenderTime(float& time)
    {
        time = 0.f;
        __try
        {
            if (!NativeBuildReady()) return false;
            const auto globals = At<uintptr_t>(SDK::g_uClientBase, SDK::Build::Prediction::Globals);
            if (!globals) return false;
            const float value = At<float>(globals, 0xC);
            if (!std::isfinite(value) || value < 0.f) return false;
            time = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ReadTiming(Timing& out)
    {
        out = {};
        __try
        {
            using namespace SDK;
            if (!NativeBuildReady() || !g_pEngine || !g_pEngine->IsInGame() ||
                !g_pEngine->IsConnected() || At<int>(g_uEngineBase, Build::Prediction::Signon) != 6)
                return false;
            float enabled = 0.f, predict = 0.f, ratio = 0.f, interp = 0.f, rate = 0.f;
            if (!ReadVar("cl_lagcompensation", enabled) || enabled == 0.f ||
                !ReadVar("cl_predict", predict) || predict == 0.f ||
                !ReadVar("cl_interp_ratio", ratio) || ratio < 0.f ||
                !ReadVar("cl_interp", interp) || interp < 0.f ||
                !ReadVar("cl_updaterate", rate) || rate <= 0.f) return false;
            if (ReadVar("sv_unlag", enabled) && enabled == 0.f) return false;
            const auto globals = At<uintptr_t>(g_uClientBase, Build::Prediction::Globals);
            const auto channel = At<uintptr_t>(g_uEngineBase, Build::LagCompensation::NetChannel);
            if (!globals || !channel) return false;
            const auto table = At<uintptr_t>(channel, 0);
            if (table != g_uEngineBase + Build::LagCompensation::NetChannelVtable ||
                At<uintptr_t>(table, 9*sizeof(uintptr_t)) != g_uEngineBase + Build::LagCompensation::Latency)
                return false;
            out.interval = At<float>(globals, 0x1C);
            if (!std::isfinite(out.interval) || std::abs(out.interval-kTickInterval) > .00001f)
                return false;
            using LerpFn = float(__fastcall*)();
            using LatencyFn = float(__fastcall*)(void*, int);
            out.lerp = reinterpret_cast<LerpFn>(g_uEngineBase + Build::LagCompensation::Interpolation)();
            auto latency = reinterpret_cast<LatencyFn>(g_uEngineBase + Build::LagCompensation::Latency);
            out.outgoing = latency(reinterpret_cast<void*>(channel), 0);
            out.incoming = latency(reinterpret_cast<void*>(channel), 1);
            out.serverTick = At<int>(g_uEngineBase, Build::LagCompensation::ServerTick);
            out.choked = At<int>(g_uEngineBase, Build::Prediction::Choked);
            out.maxUnlag = Build::LagCompensation::DefaultMaxUnlag;
            out.teleportDistance = Build::LagCompensation::DefaultTeleportDistance;
            float maxUnlag = 0.f, teleport = 0.f;
            const bool hasMax = ReadVar("sv_maxunlag", maxUnlag);
            const bool hasTeleport = ReadVar("sv_lagcompensation_teleport_dist", teleport);
            if (hasMax) out.maxUnlag = maxUnlag;
            if (hasTeleport) out.teleportDistance = teleport;
            if (!std::isfinite(out.lerp) || out.lerp < 0.f || out.lerp > 1.f ||
                !std::isfinite(out.outgoing) || out.outgoing < 0.f || out.outgoing > 2.f ||
                !std::isfinite(out.incoming) || out.incoming < 0.f || out.incoming > 2.f ||
                out.maxUnlag <= 0.f || out.maxUnlag > 1.f || out.teleportDistance <= 0.f ||
                out.teleportDistance > 4096.f || out.serverTick <= 0 || out.choked < 0 || out.choked > 64)
                return false;
            out.serverLimitsKnown = hasMax && hasTeleport;
            out.available = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = {}; return false; }
    }
    bool ObserveNetworkState(int index, const PlayerSnapshot& sample, const Timing& t)
    {
        if (index < 1 || index > 64 || !t.available) return false;
        auto& owner = g_playerRecords[index];
        if (owner.entity != sample.entity || !sample.entity || !sample.alive || sample.dormant)
        { owner.records.clear(); owner.timeline = {}; }
        owner.entity = sample.entity;
        if (!sample.entity || !sample.alive || sample.dormant) return false;
        if (!sample.networkOriginValid || !std::isfinite(sample.simtime) || sample.simtime <= 0.f)
        { owner.records.clear(); return false; }
        const bool continuous = Observe(owner.timeline, reinterpret_cast<uintptr_t>(sample.entity),
            sample.modelIdentity ? sample.modelIdentity : owner.timeline.model,
            sample.hitboxSet >= 0 ? sample.hitboxSet : owner.timeline.hitboxSet,
            sample.simtime, sample.networkOrigin, t.interval, t.teleportDistance);
        if (!continuous) owner.records.clear();
        return owner.timeline.available;
    }
    bool CopyNetworkState(int index, uintptr_t entity, float simtime, PlayerSnapshot& out)
    {
        out.networkOriginValid = false;
        if (index < 1 || index > 64 || !entity || !std::isfinite(simtime)) return false;
        const auto& owner = g_playerRecords[index];
        const auto& line = owner.timeline;
        if (reinterpret_cast<uintptr_t>(owner.entity) != entity || !line.available || line.entity != entity ||
            std::abs(line.simtime-simtime) > SDK::kTickInterval*.25f) return false;
        out.networkOrigin = line.origin;
        out.networkOriginValid = true;
        out.modelIdentity = line.model;
        out.hitboxSet = line.hitboxSet;
        return true;
    }
    void Reset()
    {
        for (auto& store : g_playerRecords)
        { store.records.clear(); store.entity = nullptr; store.timeline = {}; }
    }
}
