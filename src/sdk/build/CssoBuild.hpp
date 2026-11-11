#pragma once
#include <cstdint>

// Addresses belong to the supplied CSSO x64 binaries, not to the donor SDK.
// A new game build requires new reverse evidence and native prediction tests.
namespace SDK::Build
{
    struct ModuleIdentity { uint32_t timestamp; uint32_t imageSize; };
    inline constexpr ModuleIdentity Client{0x6A415E32, 0x6D80000};
    inline constexpr ModuleIdentity Engine{0x6A3EC5E6, 0x6D58000};

    namespace Commands
    {
        inline constexpr uintptr_t Crc = 0x22B420;
        inline constexpr uintptr_t VerifiedCopy = 0x22B330;
    }
    namespace LagCompensation
    {
        // NET_Tick -> CClientState::ProcessTick (engine+0x21D380) ->
        // CClockDriftMgr::SetServerTick (engine+0x18B7D0).
        // engine+0x90F20 forwards stages 0..6 through [client vtable+0x118].
        inline constexpr uintptr_t ClientInterface = 0x6870B8;
        inline constexpr int FrameStageSlot = 35;
        inline constexpr uintptr_t ServerTick = 0x69B410;
        inline constexpr uintptr_t NetChannel = 0x69B280;
        inline constexpr uintptr_t NetChannelVtable = 0x47F108;
        inline constexpr uintptr_t Latency = 0x1F3930;
        inline constexpr uintptr_t Interpolation = 0xC6D70;
        // Defaults in the supplied server.dll. These are NOT advertised by
        // remote servers; a locally registered server ConVar overrides them.
        inline constexpr float DefaultMaxUnlag = 1.f;
        inline constexpr float DefaultTeleportDistance = 64.f;
        inline constexpr float ServerTolerance = .2f;
    }
    namespace Weapons
    {
        // client RecvProp registration +0x306695 and primary fire +0x30CB26.
        inline constexpr uintptr_t RecoilIndex = 0xE24;
    }
    namespace Prediction
    {
        inline constexpr uintptr_t Object = 0x93BAB8;
        inline constexpr uintptr_t Vtable = 0x6AFF28;
        inline constexpr uintptr_t Update = 0x208220;
        inline constexpr uintptr_t LastFrame = 0x10;
        inline constexpr uintptr_t PredictedCount = 0x14;
        inline constexpr uintptr_t EngineInterface = 0x6870D8;
        inline constexpr uintptr_t Signon = 0x69B3AC;
        inline constexpr uintptr_t DeltaTick = 0x69B418;
        inline constexpr uintptr_t LastOutgoing = 0x6A3F6C;
        inline constexpr uintptr_t Choked = 0x6A3F70;
        inline constexpr uintptr_t Acknowledged = 0x6A3F74;
        inline constexpr uintptr_t Input = 0x82EC90;
        inline constexpr uintptr_t Globals = 0x81CC50;
    }
}
