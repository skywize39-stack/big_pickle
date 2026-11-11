#pragma once

// ============================================================
//  AutoStrafe.hpp — air strafe assist (rage movement)
// ============================================================

#include "sdk/SDK.hpp"

namespace AutoStrafe
{
    // Called from the CreateMove rage movement pipeline (after auto-stop,
    // before anti-aim). While airborne and the view is turning, replaces
    // the move with a pure side strafe INTO the turn — the classic Source
    // air-acceleration speed gain used for bhop movement.
    void Run( SDK::CUserCmd* cmd, void* pLocalPlayer );
}
