#pragma once

// ============================================================
//  CreateMoveHook.hpp — CInput::CreateMove hook declaration
// ============================================================

#include "sdk/GameTypes.hpp"

namespace Hooks
{
    bool Init();
    void Shutdown();

    // True while the player holds ANY key bound to +jump (raw engine state
    // captured in the KeyButtons detour, before the bhop strip). AutoStrafe
    // uses it as its jump-cadence signal; it stays clear while chatting.
    bool BhopJumpIntent();
}
