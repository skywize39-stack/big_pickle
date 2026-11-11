#pragma once

// ============================================================
//  Triggerbot.hpp — auto-fire when crosshair is on enemy
// ============================================================

#include "sdk/GameTypes.hpp"

namespace Triggerbot
{
    void Run( void* pCInput, int sequenceNumber,
              void* pLocalPlayer, int localTeam,
              const EntitySnapshot& snap );
}
