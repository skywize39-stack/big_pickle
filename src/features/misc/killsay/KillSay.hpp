#pragma once

// ============================================================
//  KillSay.hpp — sends a chat message after the local player
//  gets a kill (player_death game event).
// ============================================================

namespace KillSay
{
    // Registers the game-event listener (needs the GameEventManager003
    // interface resolved). Returns true when the listener was added.
    bool Init();

    // Removes the listener (called on unload).
    void Shutdown();
}