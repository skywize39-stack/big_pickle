#include "features/visuals/esp/ESPText.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include <imgui.h>
#include <cstring>

// ============================================================
//  ESPText.cpp — name, flags, and centred text rendering
// ============================================================

namespace
{
    struct EspRect
    {
        int left, top, right, bottom;
    };

    EspRect ToRect( const PlayerBoxData& box )
    {
        return {
            (int)( box.top.x - box.width ),
            (int)  box.top.y,
            (int)( box.top.x + box.width ),
            (int)  box.bot.y,
        };
    }
}

void ESPText::DrawName( const PlayerBoxData& box, const char* name, float alpha )
{
    const EspRect r = ToRect( box );
    const int     a = (int)( alpha * 255.f );

    // Truncate name if too wide
    char buf[32];
    strcpy_s( buf, name );
    while ( strlen( buf ) > 0 && ImGui::CalcTextSize( buf ).x > 100.f )
        buf[strlen( buf ) - 1] = '\0';

    ImVec2 size = ImGui::CalcTextSize( buf );
    ESPDraw::DrawTextOutlined(
        box.top.x - size.x * 0.5f,
        (float)( r.top - 16 ),
        ESPDraw::Col( 255, 255, 255, a ), buf );
}

void ESPText::DrawFlagsRight( const PlayerBoxData& box, const char* flags, float alpha )
{
    const EspRect r     = ToRect( box );
    const int     a     = (int)( alpha * 255.f );
    const float   x     = (float)( r.right + 4 );
    const float   textH = ImGui::CalcTextSize( "A" ).y + 1.f;
    float         y     = (float)r.top;

    const int len   = (int)strlen( flags );
    int       start = 0;

    while ( start < len )
    {
        int end = start;
        while ( end < len && flags[end] != ' ' ) ++end;

        if ( end > start )
        {
            char line[64];
            strncpy_s( line, flags + start, end - start );
            line[end - start] = '\0';
            ESPDraw::DrawTextOutlined( x, y, ESPDraw::Col( 255, 255, 255, a ), line );
            y += textH;
        }
        start = end + 1;
    }
}

void ESPText::DrawTextCentered( const char* text, float centerX, float y, float alpha )
{
    const int     a    = (int)( alpha * 255.f );
    const ImVec2  size = ImGui::CalcTextSize( text );
    const float   x    = centerX - size.x * 0.5f;
    ESPDraw::DrawTextOutlined( x, y, ESPDraw::Col( 255, 255, 255, a ), text );
}
