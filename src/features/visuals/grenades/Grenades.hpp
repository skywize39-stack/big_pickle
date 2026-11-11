#pragma once
#include "sdk/UserCommand.hpp"
#include "sdk/Entities.hpp"

namespace Grenades
{
    // Immutable world-space data for the main-HUD Surface pass. No entity
    // pointers or ImGui coordinates cross the producer/render boundary.
    constexpr int kSurfaceWarningMax = 8, kSurfaceWarningPoints = 256;
    constexpr int kSurfaceTrajectoryPoints = 512;
    struct SurfaceGrenadeWarning
    {
        unsigned long long identity = 0;
        Vec3 points[kSurfaceWarningPoints]{};
        int count = 0, kind = 0, damage = 0; // kind: HE, flash, smoke, fire, decoy, unknown
        float seconds = -1.f;
        bool danger = false;
        Vec2 edgeDirection{0.f,-1.f};
    };
    struct SurfaceGrenadeFrame
    {
        SurfaceGrenadeWarning warnings[kSurfaceWarningMax]{};
        int warningCount = 0;
        bool warningsEnabled = false, timers = false;
        Vec3 trajectory[kSurfaceTrajectoryPoints]{};
        bool bounced[kSurfaceTrajectoryPoints]{};
        int trajectoryCount = 0, trajectoryDamage = 0;
        bool trajectoryEnabled = false, detonates = false;
    };
    void CopySurfaceGrenadeFrame(SurfaceGrenadeFrame& frame);

    void OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local);
    void UpdateThrownGrenades();
}
