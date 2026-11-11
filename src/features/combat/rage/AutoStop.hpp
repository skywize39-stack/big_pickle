#pragma once

namespace SDK { struct WeaponData; }

namespace AutoStop
{
    float ResolveMaxSpeed(const SDK::WeaponData* weaponData, bool scoped);

    // Applies one current-command stop result. The caller owns target/fire gating;
    // forceAccuracy selects Full (true) or weapon-speed Slide (false).
    void ApplyMovement(void* pCmd, void* pLocalPlayer, float viewYaw,
        float maxSpeed, bool forceAccuracy);
}
