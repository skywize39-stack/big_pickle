#include "features/visuals/esp/ESPBox.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include "config/Config.hpp"
#include <cmath>
#include "features/visuals/esp/BoxGeometry.hpp"

// ============================================================
//  ESPBox.cpp — bounding-box calculation and rendering
// ============================================================

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

PlayerBoxData ESPBox::Calculate( const Vec2& headScreen, const Vec2& feetScreen )
{
    PlayerBoxData box{};
    box.bot    = feetScreen;
    box.top    = { feetScreen.x, headScreen.y };
    box.height = fabsf( feetScreen.y - headScreen.y );
    box.width  = box.height / 3.8f;
    return box;
}

void ESPBox::Draw( const PlayerBoxData& box, float alpha,
                   const cfg::box_color_t* colorOverride )
{
    const EspRect r = ToRect( box );
    const int     a = (int)( alpha * 255.f );
    const auto&   bc = colorOverride ? *colorOverride
                                     : g_config.settings.visuals.esp.box_color;
    const ImU32   col = ESPDraw::Col(
        (uint8_t)bc.r, (uint8_t)bc.g, (uint8_t)bc.b, (int)( bc.a * alpha ) );

    if ( g_config.settings.visuals.esp.fill_box )
    {
        const auto& fc = g_config.settings.visuals.esp.fill_color;
        const ImU32 fillCol = ESPDraw::Col(
            (uint8_t)fc.r, (uint8_t)fc.g, (uint8_t)fc.b, (int)( fc.a * alpha ) );
        ESPDraw::FillRect( r.left, r.top, r.Width(), r.Height(), fillCol );
    }
    const auto& esp = g_config.settings.visuals.esp;
    auto segments = [&](auto emit) {
        BoxGeometry::Segments(r.left, r.top, r.right, r.bottom, esp.box_type == 1, emit);
    };
    if (esp.outline_box)
        segments([&](int l, int t, int right, int bottom) {
            ESPDraw::FillRect(l-1, t-1, right-l+2, bottom-t+2, ESPDraw::Col(1,1,1,a));
        });
    segments([&](int l, int t, int right, int bottom) {
        ESPDraw::FillRect(l, t, right-l, bottom-t, col);
    });

}
