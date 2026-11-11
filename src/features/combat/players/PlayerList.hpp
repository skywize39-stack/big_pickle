#pragma once

// ============================================================
//  PlayerList.hpp — per-player overrides keyed by nickname
//
//  Online players are resolved through the CreateMove snapshot
//  (entity index -> nickname session map). Every entry persists
//  in the config so a marked player keeps his options across
//  sessions. Unmarked players behave as: aim allowed, normal
//  priority, visuals enabled, no highlight.
// ============================================================

#include <string>
#include <utility>
#include <vector>

#include "sdk/GameTypes.hpp"

namespace PlayerList
{
    // Persisted per-player settings (menu-editable, saved in configs).
    struct Entry
    {
        bool  aim       = true;   // aimbot / rage / triggerbot may target this player
        bool  prefer    = false;  // high-priority target (half effective FOV)
        bool  visuals   = true;   // ESP + chams are drawn for this player
        bool  highlight = false;  // draw the ESP box in the custom colour below
        int   hl_r = 255, hl_g = 80, hl_b = 80, hl_a = 255;
    };

    // Flattened visual decision for the render-side consumers.
    struct Override
    {
        bool hide      = false;
        bool highlight = false;
        int  hl_r = 255, hl_g = 80, hl_b = 80, hl_a = 255;
    };

    // Refresh the entity-index -> nickname session map (game thread).
    void UpdateFromSnapshot( const EntitySnapshot& snap );

    // Drop the session map (disconnect / map change).
    void ClearSession();

    // ---- hot-path queries (safe from any thread) ----
    bool     AimAllowed( int entityIndex );
    bool     IsPreferred( int entityIndex );
    Override GetVisualOverride( int entityIndex );

    // ---- menu editing (render thread; operates on copies) ----
    struct OnlineRow { int index; std::string name; };
    std::vector<OnlineRow> GetOnlineRows();

    bool TryGetCopy( const std::string& nickname, Entry& out );   // false when unmarked
    void Commit( const std::string& nickname, const Entry& in );  // create/update
    void Remove( const std::string& nickname );
    void ClearAll();

    // ---- config serialisation helpers ----
    std::vector<std::pair<std::string, Entry>> Export();          // lowercase-nickname keys
    void Import( const std::vector<std::pair<std::string, Entry>>& rows );

    // Nicknames may contain spaces/'='/'.' — percent-encode for the flat
    // key=value config format. Kept here so Config.cpp stays byte-agnostic.
    std::string EncodeName( const std::string& name );
    std::string DecodeName( const std::string& encoded );
}
