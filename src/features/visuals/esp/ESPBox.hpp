#pragma once

// ============================================================
//  ESPBox.hpp — player bounding-box calculation and rendering
// ============================================================

#include "features/visuals/esp/ESPTypes.hpp"

namespace cfg { struct box_color_t; }

namespace ESPBox
{
    // Calculates a screen-space box from head and feet NDC positions.
    PlayerBoxData Calculate( const Vec2& headScreen, const Vec2& feetScreen );

    // Draws the box (full or corner style, with optional fill and outline).
    // colorOverride replaces the configured box colour (playerlist highlight).
    void Draw( const PlayerBoxData& box, float alpha,
               const cfg::box_color_t* colorOverride = nullptr );
}
