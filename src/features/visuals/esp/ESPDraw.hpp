#pragma once

// ============================================================
//  ESPDraw.hpp — low-level ImGui drawing helpers used by all
//                ESP sub-modules (box, bars, text, flags).
// ============================================================

#include <imgui.h>
#include <cstdint>

namespace ESPDraw
{
    // Returns the background ImDrawList
    ImDrawList* DL();

    // Colour helper
    ImU32 Col( uint8_t r, uint8_t g, uint8_t b, int alpha );

    // Filled rectangle
    void FillRect( int x, int y, int w, int h, ImU32 col );

    // Outlined rectangle
    void Rect( int x, int y, int w, int h, ImU32 col );

    // Line
    void Line( int x1, int y1, int x2, int y2, ImU32 col );

    // Text with 1-pixel black outline
    void DrawTextOutlined( float x, float y, ImU32 col, const char* text, float font_size = 0.f );

    // ---- Weapon icon font (CS2GunIcons.ttf) --------------------------
    // Glyphs live on printable ASCII letters; each weapon maps to one letter
    // in weapons::gunIcons. LoadWeaponIconFont is called once during ImGui
    // init; it returns false when the TTF is unavailable and callers must fall
    // back to text.
    bool LoadWeaponIconFont();
    ImFont* WeaponIconFont();
    uint16_t WeaponIconCodepoint( const char* weaponName );  // 0 = unknown
    // Draws one weapon icon centred at (centerX, y) with a black outline.
    void DrawIconCentered( float centerX, float y, uint16_t codepoint, ImU32 col, float size = 16.f );
}
