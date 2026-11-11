#pragma once

namespace SDK
{
    struct CUserCmd;
}

namespace ClanTag
{
    // Called from the game-thread CreateMove path. Advances at most one
    // animation frame after the configured number of command ticks.
    void Update( const SDK::CUserCmd* cmd );

    // Clears the locally applied tag while the engine is still available and
    // resets all module state before the cheat unloads.
    void Shutdown();
}
