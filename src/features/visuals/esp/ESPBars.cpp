#include "features/visuals/esp/ESPBars.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include <imgui.h>
#include <algorithm>

namespace
{
    struct EspRect
    {
        int left, top, right, bottom;
        int Width()  const { return right  - left; }
        int Height() const { return bottom - top;  }
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

void ESPBars::DrawHealthBar( const PlayerBoxData& box, int health, float alpha )
{
    const EspRect r  = ToRect( box );
    const int     a  = (int)( alpha * 255.f );
    const int     hp = (std::clamp)( health, 0, 100 );
    const int     green  = (int)( hp * 2.55f );
    const int     red    = 255 - green;
    const int     fillH  = ( r.Height() * hp ) / 100;
    const int     barPos = r.left - 4;

    ESPDraw::FillRect( barPos-1, r.top-1, 3, r.Height()+2, ESPDraw::Col(1,1,1,a) );
    ESPDraw::FillRect( barPos, r.bottom-fillH, 1, fillH,
        ESPDraw::Col( (uint8_t)red, (uint8_t)green, 0, a ) );

    if ( health < 99 && health >= 1 )
    {
        char buf[16];
        sprintf_s( buf, "%d", health );
        ImVec2 textSize = ImGui::CalcTextSize( buf );
        ESPDraw::DrawTextOutlined(
            (float)barPos - textSize.x * 0.5f + 0.5f,
            (float)( r.bottom - fillH ) - textSize.y,
            ESPDraw::Col( 255, 255, 255, a ), buf );
    }
}

void ESPBars::DrawArmorBar( const PlayerBoxData& box, int armor, float alpha )
{
    const EspRect r     = ToRect( box );
    const int     a     = (int)( alpha * 255.f );
    const int     arm   = (std::clamp)( armor, 0, 100 );
    const int     fillW = ( r.Width() * arm ) / 100;
    const int     barY  = r.bottom + 3;

    ESPDraw::FillRect( r.left-1, barY-1, r.Width()+2, 5, ESPDraw::Col(1,1,1,a) );
    ESPDraw::FillRect( r.left,   barY+1, fillW,       1, ESPDraw::Col(0,128,255,a) );
}
