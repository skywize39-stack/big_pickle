#pragma once

// ============================================================
//  ESPText.hpp — player name, flags, and centered text rendering
// ============================================================

#include "features/visuals/esp/ESPTypes.hpp"

namespace ESPText
{
    // Draws the player name above the box.
    void DrawName( const PlayerBoxData& box, const char* name, float alpha );

    // Draws a space-separated flags string to the right of the box,
    // one flag per line.
    void DrawFlagsRight( const PlayerBoxData& box, const char* flags, float alpha );

    // Draws centred text at a given world-space Y position.
    void DrawTextCentered( const char* text, float centerX, float y, float alpha );
}
