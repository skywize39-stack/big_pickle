#include "features/visuals/esp/ESPDraw.hpp"
#include "resources/fonts/WeaponIconFontData.hpp"
#include "features/visuals/esp/WeaponIcons.hpp"
#include <cstdio>
#include <cstring>
#include <windows.h>

// ============================================================
//  ESPDraw.cpp — low-level ImGui drawing primitives
// ============================================================

namespace
{
    ImFont* g_weaponIconFont = nullptr;
}


ImDrawList* ESPDraw::DL()
{
    // The background draw list does not survive this engine's EndScene state
    // (skeleton/anchor/ESP geometry silently vanished), while the foreground
    // list renders reliably — the watermark already uses it. VGUI PaintTraverse
    // ESP geometry happens earlier in the frame, so foreground overlay order
    // is preserved.
    return ImGui::GetForegroundDrawList();
}

ImU32 ESPDraw::Col( uint8_t r, uint8_t g, uint8_t b, int alpha )
{
    return IM_COL32( r, g, b, alpha );
}

void ESPDraw::FillRect( int x, int y, int w, int h, ImU32 col )
{
    DL()->AddRectFilled(
        { (float)x,     (float)y     },
        { (float)(x+w), (float)(y+h) }, col );
}

void ESPDraw::Rect( int x, int y, int w, int h, ImU32 col )
{
    DL()->AddRect(
        { (float)x,     (float)y     },
        { (float)(x+w), (float)(y+h) }, col );
}

void ESPDraw::Line( int x1, int y1, int x2, int y2, ImU32 col )
{
    DL()->AddLine(
        { (float)x1, (float)y1 },
        { (float)x2, (float)y2 }, col );
}

bool ESPDraw::LoadWeaponIconFont()
{
    if ( g_weaponIconFont )
        return true;

    ImGuiIO& io = ImGui::GetIO();

    // The TTF is embedded into the binary, so no external file layout or
    // working-directory assumptions can silently disable the icons.
    // Buffer is static embedded storage, not an ImGui allocation.
    // CS2GunIcons.ttf places every weapon icon on a printable ASCII letter,
    // so only that range needs to be rasterised.
    static const ImWchar iconRanges[] = { 0x21, 0x7E, 0 };

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;

    g_weaponIconFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<void*>( static_cast<const void*>( ESPDrawData::kFontData ) ),
        static_cast<int>( ESPDrawData::kFontDataSize ),
        16.f, &cfg, iconRanges );
    return g_weaponIconFont != nullptr;
}

ImFont* ESPDraw::WeaponIconFont()
{
    return g_weaponIconFont;
}

uint16_t ESPDraw::WeaponIconCodepoint( const char* weaponName )
{
    // The glyph comes from the embedded CS2GunIcons.ttf via the weapons::gunIcons
    // map and is an ASCII letter; weapons::get returns 0 for weapons without a
    // dedicated glyph so callers fall back to the literal classname text.
    return static_cast<uint16_t>( weapons::get( weaponName ) );
}

void ESPDraw::DrawIconCentered( float centerX, float y, uint16_t codepoint, ImU32 col, float size )
{
    if ( !g_weaponIconFont || !codepoint )
        return;

    char utf8[ 5 ]{};
    size_t n = 0;
    if ( codepoint < 0x80 )
    {
        // ASCII weapon glyph (CS2GunIcons.ttf): one byte.
        utf8[ n++ ] = static_cast<char>( codepoint );
    }
    else if ( codepoint < 0x800 )
    {
        utf8[ n++ ] = (char)( 0xC0 | ( ( codepoint >> 6 ) & 0x1F ) );
        utf8[ n++ ] = (char)( 0x80 | ( codepoint & 0x3F ) );
    }
    else
    {
        utf8[ n++ ] = (char)( 0xE0 | ( ( codepoint >> 12 ) & 0x0F ) );
        utf8[ n++ ] = (char)( 0x80 | ( ( codepoint >> 6 )  & 0x3F ) );
        utf8[ n++ ] = (char)( 0x80 | ( codepoint & 0x3F ) );
    }
    utf8[ n ] = '\0';

    const ImVec2 sz = g_weaponIconFont->CalcTextSizeA( size, FLT_MAX, 0.f, utf8 );
    const float  x  = centerX - sz.x * 0.5f;

    const int     a       = ( col >> 24 ) & 0xFF;
    const ImU32   outline = Col( 1, 1, 1, a );

    DL()->AddText( g_weaponIconFont, size, { x - 1, y - 1 }, outline, utf8 );
    DL()->AddText( g_weaponIconFont, size, { x + 1, y - 1 }, outline, utf8 );
    DL()->AddText( g_weaponIconFont, size, { x - 1, y + 1 }, outline, utf8 );
    DL()->AddText( g_weaponIconFont, size, { x + 1, y + 1 }, outline, utf8 );
    DL()->AddText( g_weaponIconFont, size, { x, y },         col,     utf8 );
}

void ESPDraw::DrawTextOutlined( float x, float y, ImU32 col, const char* text, float font_size )
{
    ImFont* font = ImGui::GetFont();
    const int a = ( col >> 24 ) & 0xFF;
    const ImU32 outline = Col( 1, 1, 1, a );

    DL()->AddText( font, font_size, { x-1, y-1 }, outline, text );
    DL()->AddText( font, font_size, { x+1, y-1 }, outline, text );
    DL()->AddText( font, font_size, { x-1, y+1 }, outline, text );
    DL()->AddText( font, font_size, { x+1, y+1 }, outline, text );
    DL()->AddText( font, font_size, { x,   y-1 }, outline, text );
    DL()->AddText( font, font_size, { x,   y+1 }, outline, text );
    DL()->AddText( font, font_size, { x-1, y   }, outline, text );
    DL()->AddText( font, font_size, { x+1, y   }, outline, text );
    DL()->AddText( font, font_size, { x,   y   }, col,     text );
}
