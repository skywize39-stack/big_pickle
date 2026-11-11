#pragma once

// ============================================================
//  ESPBars.hpp — health bar and armor bar rendering
// ============================================================

#include "features/visuals/esp/ESPTypes.hpp"

namespace ESPBars
{
    void DrawHealthBar( const PlayerBoxData& box, int health, float alpha );
    void DrawArmorBar ( const PlayerBoxData& box, int armor,  float alpha );
}
