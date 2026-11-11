#pragma once

// ============================================================
//  DefensiveDT.hpp — Defensive Double Tap ("break LC on peek")
//
//  While DT is charged and we start peeking, the accumulated DT charge is
//  released as ONE movement-only teleport burst a few ticks BEFORE the
//  enemy can see us. On the server we jump N ticks ahead at once:
//    * the move is far more than 64 units of lag-comp history for one
//      update -> the enemy's lag compensation / backtrack cannot use our
//      previous records (LC broken),
//    * the enemy's client sees us appear late (bigger peeker's advantage).
//  Then DT recharges normally. One trigger per peek: it re-arms only after
//  we were out of sight for a while. It never fires on a shooting command
//  and never while rage already has a target (the charge is kept for DT).
// ============================================================

#include "sdk/SDK.hpp"

namespace DefensiveDT
{
    bool        OnCreateMove( SDK::CUserCmd* cmd, void* local, int localTeam, int maxClients );
    void        Reset();
    const char* GetIndicatorText();
}
