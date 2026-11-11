#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/menu/SkeetUI.hpp"
#include "resources/menu/SkeetAssets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace ImGui;

// Same helper as the original custom.hpp
#define to_vec4( r, g, b, a ) ImColor( ( r ) / 255.f, ( g ) / 255.f, ( b ) / 255.f, ( a ) )

namespace
{
    ImFont* FontOr( ImFont* f ) { return f ? f : GetFont(); }
    float   SizeOr( ImFont* f ) { return f ? f->LegacySize : GetFontSize(); }

    ImVec2 CalcTextSizeF( ImFont* f, const char* text, const char* end = nullptr )
    {
        return FontOr( f )->CalcTextSizeA( SizeOr( f ), FLT_MAX, 0.f, text, end );
    }

    // ImGui::RenderArrow with the skeet imgui_draw.cpp patch: the tip of
    // the up/down arrow sits at 0.20 r instead of 0.75 r (flat chevron).
    void RenderArrowSk( ImDrawList* draw_list, ImVec2 pos, ImU32 col, ImGuiDir dir, float scale )
    {
        const float h = GetFontSize();
        float r = h * 0.40f * scale;
        ImVec2 center = pos + ImVec2( h * 0.50f, h * 0.50f * scale );

        ImVec2 a, b, c;
        switch ( dir )
        {
        case ImGuiDir_Up:
        case ImGuiDir_Down:
            if ( dir == ImGuiDir_Up ) r = -r;
            a = ImVec2( +0.000f, +0.200f ) * r;
            b = ImVec2( -0.866f, -0.750f ) * r;
            c = ImVec2( +0.866f, -0.750f ) * r;
            break;
        case ImGuiDir_Left:
        case ImGuiDir_Right:
        default:
            if ( dir == ImGuiDir_Left ) r = -r;
            a = ImVec2( +0.750f, +0.000f ) * r;
            b = ImVec2( -0.750f, +0.866f ) * r;
            c = ImVec2( -0.750f, -0.866f ) * r;
            break;
        }
        draw_list->AddTriangleFilled( center + a, center + b, center + c, col );
    }

    // Keep the upper scroll hint; never darken controls at the bottom.
    void DrawChildFade()
    {
        ImGuiWindow* w = GetCurrentWindow();
        if ( !( w->Flags & ImGuiWindowFlags_ChildWindow ) || !w->ScrollbarY )
            return;
        w->DrawList->PushClipRect( w->Pos, w->Pos + w->Size, false );
        if ( w->Scroll.y > 0.f )
            w->DrawList->AddRectFilledMultiColor( w->Pos + ImVec2( 2, 0 ), w->Pos + ImVec2( w->Size.x - 2, 12 ),
                to_vec4( 23, 23, 23, GetStyle().Alpha ), to_vec4( 23, 23, 23, GetStyle().Alpha ),
                to_vec4( 23, 23, 23, 0.f ), to_vec4( 23, 23, 23, 0.f ) );
        w->DrawList->PopClipRect();
    }

    void OutlinedText( ImDrawList* dl, ImFont* f, ImVec2 pos, ImU32 col, const char* b, const char* e = nullptr )
    {
        for ( int i = -1; i < 2; ++i )
            for ( int j = -1; j < 2; ++j )
                dl->AddText( FontOr( f ), SizeOr( f ), pos + ImVec2( (float)i, (float)j ), GetColorU32( ImGuiCol_Border, 0.5f ), b, e );
        dl->AddText( FontOr( f ), SizeOr( f ), pos, col, b, e );
    }
}

namespace ui
{
    ImU32 Accent( float a )
    {
        return to_vec4( 141, 188, 0, a );
    }

    void LoadFonts()
    {
        ImGuiIO& io = GetIO();

        // Same font setup as the original main.cpp.
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.PixelSnapH = false;
        cfg.OversampleH = 5;
        cfg.OversampleV = 5;
        cfg.RasterizerMultiply = 1.2f;

        static const ImWchar ranges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
            0x2DE0, 0x2DFF, // Cyrillic Extended-A
            0xA640, 0xA69F, // Cyrillic Extended-B
            0xE000, 0xE226, // icons
            0,
        };

        f_normal = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\verdana.ttf",  12.f, &cfg, ranges );
        f_bold   = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\verdanab.ttf", 12.f, &cfg, ranges );
        f_icons  = io.Fonts->AddFontFromMemoryTTF( SkeetAssets::kIconsFont, (int)sizeof( SkeetAssets::kIconsFont ), 37.f, &cfg, ranges );
    }

    void ApplyStyle()
    {
        ImGuiStyle& s = GetStyle();

        // ImGui 1.89 defaults + skeet patches (imgui.cpp ImGuiStyle ctor)
        s.Alpha             = 1.f;
        s.WindowPadding     = { 8, 8 };
        s.WindowRounding    = 0.f;
        s.WindowBorderSize  = 1.f;
        s.WindowMinSize     = { 32, 32 };
        s.ChildRounding     = 0.f;
        s.ChildBorderSize   = 1.f;
        s.PopupRounding     = 0.f;
        s.PopupBorderSize   = 1.f;
        s.FramePadding      = { 4, 3 };
        s.FrameRounding     = 0.f;
        s.FrameBorderSize   = 0.f;
        s.ItemSpacing       = { 8, 4 };
        s.ItemInnerSpacing  = { 4, 4 };
        s.IndentSpacing     = 21.f;
        s.ScrollbarSize     = 8.f;
        s.ScrollbarRounding = 0.f;
        s.ScrollbarPadding  = 3.f;
        s.GrabMinSize       = 0.f;
        s.GrabRounding      = 0.f;
        s.TabRounding       = 0.f;

        StyleColorsDark( &s );
        ImVec4* c = s.Colors;
        // skeet imgui_draw.cpp StyleColorsDark patch
        c[ ImGuiCol_WindowBg ]             = ImColor( 34, 34, 34 );
        c[ ImGuiCol_Border ]               = ImVec4( 0.f, 0.f, 0.f, 1.f );
        c[ ImGuiCol_ScrollbarBg ]          = ImVec4( 1.f, 1.f, 1.f, 0.1f );
        c[ ImGuiCol_ScrollbarGrab ]        = ImVec4( 0.31f, 0.31f, 0.31f, 1.f );
        c[ ImGuiCol_ScrollbarGrabHovered ] = ImVec4( 0.36f, 0.36f, 0.36f, 1.f );
        c[ ImGuiCol_ScrollbarGrabActive ]  = ImVec4( 0.41f, 0.41f, 0.41f, 1.f );
        c[ ImGuiCol_TextSelectedBg ]       = ImColor( Accent() );
        // not part of the original, but stops the stock blue resize grip
        // from showing on the skeet window.
        c[ ImGuiCol_ResizeGrip ]           = ImVec4( 0.f, 0.f, 0.f, 0.f );
        c[ ImGuiCol_ResizeGripHovered ]    = ImVec4( 0.f, 0.f, 0.f, 0.f );
        c[ ImGuiCol_ResizeGripActive ]     = ImVec4( 0.f, 0.f, 0.f, 0.f );
    }

    // ---------------------------------------------------------------
    //  c_custom::tab
    // ---------------------------------------------------------------
    bool Tab( const char* name, bool cur )
    {
        ImGuiWindow* window = GetCurrentWindow();
        const ImGuiID id = window->GetID( name[ 0 ] ? name : "##cfgtab" );

        const ImVec2 pos = window->DC.CursorPos;
        ImDrawList* draw = window->DrawList;

        const ImRect bb( pos, pos + ImVec2( 68, 68 ) );
        ItemAdd( bb, id );
        ItemSize( bb, GetStyle().FramePadding.y );

        bool hovered, held;
        const bool pressed = ButtonBehavior( bb, id, &hovered, &held );

        if ( !cur )
        {
            draw->AddRectFilled( bb.Min, bb.Max, to_vec4( 12, 12, 12, GetStyle().Alpha ) );
            draw->AddLine( ImVec2( bb.Max.x, bb.Min.y ), bb.Max, GetColorU32( ImGuiCol_Border ) );
            draw->AddLine( ImVec2( bb.Max.x + 1, bb.Min.y ), bb.Max + ImVec2( 1, 0 ), ImColor( 1.f, 1.f, 1.f, 0.05f * GetStyle().Alpha ) );
        }
        else
        {
            draw->AddLine( bb.Min, ImVec2( bb.Max.x + 1, bb.Min.y ), GetColorU32( ImGuiCol_Border ) );
            draw->AddLine( bb.Min + ImVec2( 0, 1 ), ImVec2( bb.Max.x + 1, bb.Min.y + 1 ), ImColor( 1.f, 1.f, 1.f, 0.05f * GetStyle().Alpha ) );

            draw->AddLine( ImVec2( bb.Min.x, bb.Max.y - 1 ), bb.Max - ImVec2( -1, 1 ), GetColorU32( ImGuiCol_Border ) );
            draw->AddLine( ImVec2( bb.Min.x, bb.Max.y - 2 ), bb.Max - ImVec2( -1, 2 ), ImColor( 1.f, 1.f, 1.f, 0.05f * GetStyle().Alpha ) );
        }

        const ImU32 col = GetColorU32( ImGuiCol_Text, cur ? 0.9f : hovered ? 0.5f : 0.25f );

        if ( name[ 0 ] )
        {
            const ImVec2 name_size = CalcTextSizeF( f_icons, name );
            draw->AddText( FontOr( f_icons ), SizeOr( f_icons ), bb.GetCenter() - name_size / 2, col, name );
        }
        else
        {
            // The icon font has no config glyph: draw a floppy disk in
            // the same weight as the font icons.
            const ImVec2 c = bb.GetCenter();
            const ImVec2 mn = c - ImVec2( 14, 14 ), mx = c + ImVec2( 14, 14 );
            draw->PathLineTo( mn );
            draw->PathLineTo( ImVec2( mx.x - 6, mn.y ) );
            draw->PathLineTo( ImVec2( mx.x, mn.y + 6 ) );
            draw->PathLineTo( mx );
            draw->PathLineTo( ImVec2( mn.x, mx.y ) );
            draw->PathFillConvex( col );
            const ImU32 bg = cur ? GetColorU32( ImGuiCol_WindowBg ) : (ImU32)to_vec4( 12, 12, 12, GetStyle().Alpha );
            draw->AddRectFilled( mn + ImVec2( 6, 3 ), ImVec2( mx.x - 9, mn.y + 10 ), bg );
            draw->AddRectFilled( ImVec2( mn.x + 5, c.y + 2 ), ImVec2( mx.x - 5, mx.y - 3 ), bg );
        }

        return pressed;
    }

    // ---------------------------------------------------------------
    //  c_custom::group_box / end_group_box
    // ---------------------------------------------------------------
    static constexpr float kGroupTop = 9.f;  // inner child y (below caption)
    static constexpr float kGroupPad = 5.f;  // inner padding (9 + 5 = 14)

    void BeginGroupBox( const char* name, const ImVec2& size, bool resizing )
    {
        ImGuiWindow* window = GetCurrentWindow();
        const ImVec2 pos = window->DC.CursorPos;

        const char* name_end = FindRenderedTextEnd( name );
        const ImVec2 name_size = CalcTextSizeF( f_bold, name, name_end );

        // The menu layout owns height and resize state. A private widget size
        // here would make the drawn panel disagree with the neighbours' layout.
        const ImRect resize_grip(pos+size-ImVec2(8,8),pos+size-ImVec2(2,2));

        BeginChild( std::string( name ).append( ".main" ).c_str(), size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar );

        window->DrawList->AddRectFilled( pos, pos + size, to_vec4( 23, 23, 23, GetStyle().Alpha ) );
        window->DrawList->AddRect( pos, pos + size, GetColorU32( ImGuiCol_Border, 0.43f ) );
        window->DrawList->AddRect( pos + ImVec2( 1, 1 ), pos + size - ImVec2( 1, 1 ), resizing ? Accent( GetStyle().Alpha ) : (ImU32)ImColor( 1.f, 1.f, 1.f, 0.1f * GetStyle().Alpha ) );
        window->DrawList->AddLine( pos + ImVec2( 8, 0 ), pos + ImVec2( 16 + name_size.x, 0 ), to_vec4( 23, 23, 23, GetStyle().Alpha ), 2 );

        window->DrawList->AddText( FontOr( f_bold ), SizeOr( f_bold ), pos + ImVec2( 13, -( name_size.y / 2 ) + 2 ), ImColor( 0.f, 0.f, 0.f, 0.43f * GetStyle().Alpha ), name, name_end );
        window->DrawList->AddText( FontOr( f_bold ), SizeOr( f_bold ), pos + ImVec2( 12, -( name_size.y / 2 ) + 1 ), GetColorU32( ImGuiCol_Text ), name, name_end );

        window->DrawList->AddTriangleFilled( ImVec2( resize_grip.Min.x, resize_grip.Max.y ), ImVec2( resize_grip.Max.x, resize_grip.Min.y ), resize_grip.Max, resizing ? Accent( GetStyle().Alpha ) : (ImU32)ImColor( 0.2f, 0.2f, 0.2f, GetStyle().Alpha ) );

        // The scrolling region starts below the title (kGroupTop) so
        // scrolled items are clipped there instead of running under the
        // groupbox caption. First item still lands at y = 14 as before.
        PushStyleVar( ImGuiStyleVar_WindowPadding, { 0, kGroupPad } );

        SetCursorPosY( kGroupTop );
        BeginChild( name, { size.x, size.y - kGroupTop - 2 }, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None );
        SetCursorPosX( 17 );

        if ( GetCurrentWindow()->ScrollbarY )
        {
            if ( GetCurrentWindow()->Scroll.y <= GetCurrentWindow()->ScrollMax.y - 2 )
                RenderArrowSk( window->DrawList, pos + ImVec2( size.x - 22, size.y - 12 ), GetColorU32( ImGuiCol_Text ), ImGuiDir_Down, 0.65f );
            if ( GetCurrentWindow()->Scroll.y )
                RenderArrowSk( window->DrawList, pos + ImVec2( size.x - 22, 5 ), GetColorU32( ImGuiCol_Text ), ImGuiDir_Up, 0.65f );
        }

        BeginGroup();

        PushStyleVar( ImGuiStyleVar_ItemSpacing, { 8, 8 } );
    }

    static float s_lastGroupContentH = 0.f;
    float LastGroupBoxContentHeight() { return s_lastGroupContentH; }

    void EndGroupBox()
    {
        PopStyleVar( 2 );
        EndGroup();
        {
            // Height the outer box needs to show everything without
            // scrolling: inner content + inner bottom padding + the 4px
            // the inner child is shorter than the outer one.
            ImGuiWindow* w = GetCurrentWindow();
            s_lastGroupContentH = ( w->DC.CursorMaxPos.y - w->Pos.y + w->Scroll.y ) + w->WindowPadding.y + kGroupTop + 2.f + 5.f;
        }
        DrawChildFade();
        EndChild();
        EndChild();
    }

    float BoxH( float frac )
    {
        const ImGuiStyle& s = GetStyle();
        const float full = GetWindowHeight() - s.WindowPadding.y * 2;
        return frac * ( full + s.ItemSpacing.y ) - s.ItemSpacing.y;
    }

    float BoxW()
    {
        return GetWindowWidth() / 2 - GetStyle().ItemSpacing.x / 2;
    }

    void NextColumn()
    {
        SameLine();
        SetCursorPosY( GetStyle().WindowPadding.y );
    }

    // ---------------------------------------------------------------
    //  Checkbox
    // ---------------------------------------------------------------
    bool Checkbox( const char* label, bool* v, bool special )
    {
        ImGuiWindow* window = GetCurrentWindow();
        if ( window->SkipItems )
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID( label );
        const ImVec2 label_size = CalcTextSize( label, 0, 1 );

        const float square_sz = 9.f;
        const float ROUNDING = 0.f;
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect frame_bb( pos, pos + ImVec2( square_sz, square_sz ) );
        const ImRect total_bb( pos, pos + ImVec2( square_sz + label_size.x + 10, square_sz ) );
        ItemAdd( total_bb, id, &frame_bb );
        ItemSize( total_bb, style.FramePadding.y );

        bool hovered, held;
        const bool pressed = ButtonBehavior( total_bb, id, &hovered, &held );
        if ( pressed )
        {
            *v = !( *v );
            MarkItemEdited( id );
        }

        window->DrawList->AddRectFilledMultiColor( frame_bb.Min, frame_bb.Max,
            hovered ? to_vec4( 91, 91, 91, style.Alpha ) : to_vec4( 76, 76, 76, style.Alpha ),
            hovered ? to_vec4( 91, 91, 91, style.Alpha ) : to_vec4( 76, 76, 76, style.Alpha ),
            hovered ? to_vec4( 66, 66, 66, style.Alpha ) : to_vec4( 51, 51, 51, style.Alpha ),
            hovered ? to_vec4( 66, 66, 66, style.Alpha ) : to_vec4( 51, 51, 51, style.Alpha ) );
        if ( *v )
            window->DrawList->AddRectFilledMultiColor( frame_bb.Min, frame_bb.Max, Accent( style.Alpha ), Accent( style.Alpha ), Accent( 0.6f * style.Alpha ), Accent( 0.6f * style.Alpha ) );

        window->DrawList->AddRect( frame_bb.Min, frame_bb.Max, GetColorU32( ImGuiCol_Border ), ROUNDING );

        const ImVec2 label_pos = ImVec2( pos.x + 20, frame_bb.GetCenter().y - label_size.y / 2 );
        if ( special )
            PushStyleColor( ImGuiCol_Text, ImVec4( 182 / 255.f, 182 / 255.f, 101 / 255.f, 1.f ) );
        if ( label_size.x > 0.f )
            RenderText( label_pos, label );
        if ( special )
            PopStyleColor();

        return pressed;
    }

    // ---------------------------------------------------------------
    //  SliderScalar
    // ---------------------------------------------------------------
    static bool SliderScalarSk( const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max, const char* format )
    {
        ImGuiWindow* window = GetCurrentWindow();
        if ( window->SkipItems )
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID( label );
        const float w = GetWindowWidth() - 50;

        const ImVec2 label_size = CalcTextSize( label, 0, 1 );
        const ImRect frame_bb( window->DC.CursorPos + ImVec2( 20, label_size.y + 4 ), window->DC.CursorPos + ImVec2( w - 20, label_size.y + 10 ) );
        const ImRect total_bb( window->DC.CursorPos, frame_bb.Max );

        ItemAdd( total_bb, id, &frame_bb );
        ItemSize( total_bb, style.FramePadding.y );

        if ( format == NULL )
            format = DataTypeGetInfo( data_type )->PrintFmt;

        const bool hovered = ItemHoverable( frame_bb, id, 0 );
        const bool clicked = ( hovered && g.IO.MouseClicked[ 0 ] );
        if ( clicked )
        {
            SetActiveID( id, window );
            SetFocusID( id, window );
            FocusWindow( window );
            SetKeyOwner( ImGuiKey_MouseLeft, id );
            g.ActiveIdUsingNavDirMask |= ( 1 << ImGuiDir_Left ) | ( 1 << ImGuiDir_Right );
        }

        ImRect grab_bb;
        const bool value_changed = SliderBehavior( frame_bb, id, data_type, p_data, p_min, p_max, format, ImGuiSliderFlags_None, &grab_bb );
        if ( value_changed )
            MarkItemEdited( id );

        char value_buf[ 64 ];
        const char* value_buf_end = value_buf + DataTypeFormatString( value_buf, IM_ARRAYSIZE( value_buf ), data_type, p_data, format );

        const bool act = hovered || g.ActiveId == id;
        window->DrawList->AddRectFilledMultiColor( frame_bb.Min, frame_bb.Max,
            act ? to_vec4( 91, 91, 91, style.Alpha ) : to_vec4( 76, 76, 76, style.Alpha ),
            act ? to_vec4( 91, 91, 91, style.Alpha ) : to_vec4( 76, 76, 76, style.Alpha ),
            act ? to_vec4( 66, 66, 66, style.Alpha ) : to_vec4( 51, 51, 51, style.Alpha ),
            act ? to_vec4( 66, 66, 66, style.Alpha ) : to_vec4( 51, 51, 51, style.Alpha ) );
        window->DrawList->AddRectFilledMultiColor( frame_bb.Min, ImVec2( grab_bb.Max.x + 1, frame_bb.Max.y ), Accent( style.Alpha ), Accent( style.Alpha ), Accent( 0.6f * style.Alpha ), Accent( 0.6f * style.Alpha ) );
        window->DrawList->AddRect( frame_bb.Min, frame_bb.Max, GetColorU32( ImGuiCol_Border ) );

        RenderText( ImVec2( frame_bb.Min.x, total_bb.Min.y ), label );

        OutlinedText( window->DrawList, f_bold, grab_bb.Max - ImVec2( 6, 4 ), GetColorU32( ImGuiCol_Text ), value_buf, value_buf_end );

        return value_changed;
    }

    bool SliderInt( const char* label, int* v, int mn, int mx, const char* fmt )
    {
        return SliderScalarSk( label, ImGuiDataType_S32, v, &mn, &mx, fmt );
    }

    bool SliderFloat( const char* label, float* v, float mn, float mx, const char* fmt )
    {
        return SliderScalarSk( label, ImGuiDataType_Float, v, &mn, &mx, fmt );
    }

    // ---------------------------------------------------------------
    //  Combo
    // ---------------------------------------------------------------
    bool BeginCombo( const char* label, const char* preview_value, ImGuiComboFlags flags )
    {
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = GetCurrentWindow();

        g.NextWindowData.ClearFlags();
        if ( window->SkipItems )
            return false;

        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID( label );
        const float size = GetWindowWidth() - 50;

        const ImVec2 label_size = CalcTextSize( label, 0, 1 );
        const float labelHeight = label_size.x > 0.f ? label_size.y : 0.f;
        const ImRect clickable( window->DC.CursorPos + ImVec2( 20, labelHeight + 4 ), window->DC.CursorPos + ImVec2( size - 20, labelHeight + 25 ) );
        const ImRect total_bb( window->DC.CursorPos, clickable.Max );

        ItemAdd( total_bb, id, &clickable );
        ItemSize( total_bb, style.FramePadding.y );

        bool hovered, held;
        const bool pressed = ButtonBehavior( clickable, id, &hovered, &held );
        const ImGuiID popup_id = ImHashStr( "##ComboPopup", 0, id );
        bool popup_open = IsPopupOpen( popup_id, ImGuiPopupFlags_None );
        if ( pressed && !popup_open )
        {
            OpenPopupEx( popup_id, ImGuiPopupFlags_None );
            popup_open = true;
        }

        const bool hl = hovered || popup_open;
        window->DrawList->AddRectFilledMultiColor( clickable.Min, clickable.Max,
            hl ? to_vec4( 41, 41, 41, style.Alpha ) : to_vec4( 31, 31, 31, style.Alpha ),
            hl ? to_vec4( 41, 41, 41, style.Alpha ) : to_vec4( 31, 31, 31, style.Alpha ),
            hl ? to_vec4( 46, 46, 46, style.Alpha ) : to_vec4( 36, 36, 36, style.Alpha ),
            hl ? to_vec4( 46, 46, 46, style.Alpha ) : to_vec4( 36, 36, 36, style.Alpha ) );
        window->DrawList->AddRect( clickable.Min, clickable.Max, GetColorU32( ImGuiCol_Border ) );

        RenderText( ImVec2( clickable.Min.x, total_bb.Min.y ), label );

        PushStyleColor( ImGuiCol_Text, ImVec4( 0.64f, 0.64f, 0.64f, 1.f ) );
        RenderTextClipped( clickable.Min + ImVec2( 9, 4 ), clickable.Max - ImVec2( 16, 0 ), preview_value, NULL, NULL, ImVec2( 0.f, 0.f ) );
        PopStyleColor();

        RenderArrowSk( window->DrawList, ImVec2( clickable.Max.x - 14, clickable.Min.y + 7 ), GetColorU32( ImGuiCol_Text, 0.65f ), ImGuiDir_Down, 0.8f );

        if ( !popup_open )
            return false;

        // Popup: skeet pushes WindowPadding(3,4), PopupRounding 0,
        // PopupBorderSize 1 and PopupBg 35,35,35 around Begin.
        PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8, 4 ) );     // stays pushed for the popup body, popped in EndCombo
        PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 3, style.FramePadding.y ) ); // BeginComboPopup copies FramePadding.x into WindowPadding.x
        PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 3, 4 ) );
        PushStyleVar( ImGuiStyleVar_PopupRounding, 0.f );
        PushStyleVar( ImGuiStyleVar_PopupBorderSize, 1.f );
        PushStyleColor( ImGuiCol_PopupBg, ImVec4( ImColor( 35, 35, 35 ) ) );
        const bool ret = BeginComboPopup( popup_id, ImRect( clickable.Min, clickable.Max + ImVec2( 0, 3 ) ), flags );
        PopStyleColor();
        PopStyleVar( 4 );   // FramePadding, WindowPadding, PopupRounding, PopupBorderSize
        if ( !ret )
        {
            PopStyleVar();
            return false;
        }
        return true;
    }

    void EndCombo()
    {
        PopStyleVar();      // ItemSpacing from BeginCombo
        ImGui::EndCombo();
    }

    bool Combo( const char* label, int* v, const char* const items[], int count )
    {
        const char* preview = ( *v >= 0 && *v < count ) ? items[ *v ] : "";
        bool changed = false;
        if ( BeginCombo( label, preview ) )
        {
            for ( int i = 0; i < count; ++i )
            {
                PushID( i );
                const bool sel = ( *v == i );
                if ( Selectable( items[ i ], sel ) && !sel )
                {
                    *v = i;
                    changed = true;
                }
                if ( sel )
                    SetItemDefaultFocus();
                PopID();
            }
            EndCombo();
        }
        return changed;
    }

    bool MultiCombo( const char* label, bool combos[], const char* const items[], int items_count )
    {
        std::string preview = "-";
        for ( int i = 0, j = 0; i < items_count; i++ )
        {
            if ( combos[ i ] )
            {
                if ( j == 0 )
                    preview = items[ i ];
                else if ( j < 3 )
                    preview += std::string( ", " ) + items[ i ];
                else if ( j == 3 )
                    preview += ", ...";
                j++;
            }
        }

        bool changed = false;
        if ( BeginCombo( label, preview.c_str() ) )
        {
            for ( int i = 0; i < items_count; i++ )
            {
                PushID( i );
                if ( Selectable( items[ i ], combos[ i ], ImGuiSelectableFlags_NoAutoClosePopups ) )
                {
                    combos[ i ] = !combos[ i ];
                    changed = true;
                }
                PopID();
            }
            EndCombo();
        }
        return changed;
    }

    // ---------------------------------------------------------------
    //  Selectable (skeet: dark hover bg, bold font on hover/selected,
    //  accent text when selected, text at +10)
    // ---------------------------------------------------------------
    bool Selectable( const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size )
    {
        const ImVec4 bg = ImColor( to_vec4( 23, 22, 23, GetStyle().Alpha ) );
        PushStyleColor( ImGuiCol_Header, bg );
        PushStyleColor( ImGuiCol_HeaderHovered, bg );
        PushStyleColor( ImGuiCol_HeaderActive, bg );
        PushID( label );
        const bool pressed = ImGui::Selectable( "##sk_sel", selected, flags, size );
        PopID();
        PopStyleColor( 3 );

        const bool hovered = IsItemHovered();
        const ImRect bb( GetItemRectMin(), GetItemRectMax() );
        ImFont* f = ( hovered || selected ) ? f_bold : f_normal;
        const char* end = FindRenderedTextEnd( label );
        const ImVec2 ls = CalcTextSizeF( f, label, end );
        const ImU32 col = selected ? Accent( GetStyle().Alpha ) : GetColorU32( ImGuiCol_Text );
        GetWindowDrawList()->AddText( FontOr( f ), SizeOr( f ), ImVec2( bb.Min.x + 10, bb.GetCenter().y - ls.y / 2 ), col, label, end );
        return pressed;
    }

    // ---------------------------------------------------------------
    //  Button
    // ---------------------------------------------------------------
    bool Button( const char* label, const ImVec2& size_arg )
    {
        ImGuiWindow* window = GetCurrentWindow();
        if ( window->SkipItems )
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID( label );
        const ImVec2 label_size = CalcTextSize( label, NULL, true );

        const ImVec2 pos = window->DC.CursorPos + ImVec2( 20, 0 );
        const ImVec2 want( size_arg.x != 0.f ? size_arg.x : GetWindowWidth() - 90, size_arg.y != 0.f ? size_arg.y : 25.f );
        const ImVec2 size = CalcItemSize( want, label_size.x + style.FramePadding.x * 2.0f, label_size.y + style.FramePadding.y * 2.0f );

        const ImRect bb( pos, pos + size );
        ItemSize( size, style.FramePadding.y );
        if ( !ItemAdd( bb, id ) )
            return false;

        bool hovered, held;
        const bool pressed = ButtonBehavior( bb, id, &hovered, &held );

        const ImU32 top = ( hovered && held ) ? to_vec4( 26, 26, 26, style.Alpha ) : hovered ? to_vec4( 40, 40, 40, style.Alpha ) : to_vec4( 34, 34, 34, style.Alpha );
        const ImU32 bot = ( hovered && held ) ? to_vec4( 34, 34, 34, style.Alpha ) : hovered ? to_vec4( 32, 32, 32, style.Alpha ) : to_vec4( 26, 26, 26, style.Alpha );
        window->DrawList->AddRectFilledMultiColor( bb.Min, bb.Max, top, top, bot, bot );
        window->DrawList->AddRect( bb.Min, bb.Max, GetColorU32( ImGuiCol_Border ) );
        window->DrawList->AddRect( bb.Min + ImVec2( 1, 1 ), bb.Max - ImVec2( 1, 1 ), ImColor( 1.f, 1.f, 1.f, 0.1f * style.Alpha ) );

        PushStyleColor( ImGuiCol_Text, ImVec4( ImColor( 0.f, 0.f, 0.f, 0.43f ) ) );
        RenderText( bb.GetCenter() - label_size / 2 + ImVec2( 1, 1 ), label );
        PopStyleColor();

        RenderText( bb.GetCenter() - label_size / 2, label );

        return pressed;
    }

    // ---------------------------------------------------------------
    //  InputText: label above, frame at +20, width = window - 90.
    // ---------------------------------------------------------------
    bool InputText( const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags, const char* hint )
    {
        ImGuiWindow* window = GetCurrentWindow();
        if ( window->SkipItems )
            return false;

        const ImGuiStyle& style = GetStyle();
        const char* label_end = FindRenderedTextEnd( label );
        const bool has_label = label_end != label;
        const ImVec2 label_size = CalcTextSize( label, label_end );
        const ImVec2 start = window->DC.CursorPos;

        ImDrawList* dl = window->DrawList;
        if ( has_label )
            dl->AddText( ImVec2( start.x + 20, start.y ), GetColorU32( ImGuiCol_Text ), label, label_end );

        const ImVec2 fmin = start + ImVec2( 20, has_label ? label_size.y + 4 : 0 );
        const float  fw = GetWindowWidth() - 90;

        dl->ChannelsSplit( 2 );
        dl->ChannelsSetCurrent( 1 );

        SetCursorScreenPos( fmin );
        SetNextItemWidth( fw );
        PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0, 0, 0, 0 ) );
        PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0, 0, 0, 0 ) );
        PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0, 0, 0, 0 ) );
        PushStyleVar( ImGuiStyleVar_FrameBorderSize, 0.f );
        PushStyleVar( ImGuiStyleVar_FrameRounding, 0.f );
        PushID( label );
        const bool changed = hint ? InputTextWithHint( "##sk_in", hint, buf, buf_size, flags )
                                  : ImGui::InputText( "##sk_in", buf, buf_size, flags );
        PopID();
        PopStyleVar( 2 );
        PopStyleColor( 3 );

        const bool hl = IsItemHovered() || IsItemActive();
        const ImRect fb( GetItemRectMin(), GetItemRectMax() );

        dl->ChannelsSetCurrent( 0 );
        dl->AddRectFilledMultiColor( fb.Min, fb.Max,
            hl ? to_vec4( 41, 41, 41, style.Alpha ) : to_vec4( 31, 31, 31, style.Alpha ),
            hl ? to_vec4( 41, 41, 41, style.Alpha ) : to_vec4( 31, 31, 31, style.Alpha ),
            hl ? to_vec4( 46, 46, 46, style.Alpha ) : to_vec4( 36, 36, 36, style.Alpha ),
            hl ? to_vec4( 46, 46, 46, style.Alpha ) : to_vec4( 36, 36, 36, style.Alpha ) );
        dl->AddRect( fb.Min, fb.Max, GetColorU32( ImGuiCol_Border ) );
        dl->ChannelsMerge();

        return changed;
    }

    // ---------------------------------------------------------------
    //  ColorEdit4 (NoInputs | NoLabel) + skeet ColorPicker4
    // ---------------------------------------------------------------
    static bool PickerSk( float col[4], bool alpha_bar )
    {
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = GetCurrentWindow();
        ImDrawList* draw_list = window->DrawList;
        const ImGuiStyle& style = g.Style;
        ImGuiIO& io = g.IO;

        const float square_sz = GetFrameHeight();
        const float width = square_sz * 9.4f;
        const float bars_width = square_sz - 3;
        const float sv_picker_size = ImMax( bars_width * 1, width - ( alpha_bar ? 2 : 1 ) * ( bars_width + style.ItemInnerSpacing.x ) );
        const ImVec2 picker_pos = window->DC.CursorPos;
        const float bar0_pos_x = picker_pos.x + sv_picker_size + style.ItemInnerSpacing.x;
        const float bar1_pos_x = picker_pos.x;

        // Hue is kept across frames: RGB->HSV loses it at S==0 / V==0.
        static std::unordered_map< ImGuiID, float > s_hue;
        const ImGuiID hid = window->GetID( "##hue_keep" );

        float H, S, V;
        ColorConvertRGBtoHSV( col[ 0 ], col[ 1 ], col[ 2 ], H, S, V );
        auto it = s_hue.find( hid );
        if ( it != s_hue.end() && ( S == 0.f || V == 0.f ) )
            H = it->second;

        bool value_changed = false, value_changed_hsv = false;

        BeginGroup();

        SetCursorScreenPos( picker_pos );
        InvisibleButton( "sv", ImVec2( sv_picker_size, sv_picker_size ) );
        if ( IsItemActive() )
        {
            S = ImSaturate( ( io.MousePos.x - picker_pos.x ) / ( sv_picker_size - 1 ) );
            V = 1.0f - ImSaturate( ( io.MousePos.y - picker_pos.y ) / ( sv_picker_size - 1 ) );
            value_changed = value_changed_hsv = true;
        }

        SetCursorScreenPos( ImVec2( bar0_pos_x, picker_pos.y ) );
        InvisibleButton( "hue", ImVec2( bars_width, sv_picker_size ) );
        if ( IsItemActive() )
        {
            H = ImSaturate( ( io.MousePos.y - picker_pos.y ) / ( sv_picker_size - 1 ) );
            value_changed = value_changed_hsv = true;
        }

        if ( alpha_bar )
        {
            SetCursorScreenPos( ImVec2( bar1_pos_x, picker_pos.y + sv_picker_size + style.ItemInnerSpacing.y ) );
            InvisibleButton( "alpha", ImVec2( sv_picker_size, bars_width ) );
            if ( IsItemActive() )
            {
                col[ 3 ] = 1.0f - ImSaturate( ( io.MousePos.x - picker_pos.x ) / ( sv_picker_size - 1 ) );
                value_changed = true;
            }
        }

        if ( value_changed_hsv )
        {
            ColorConvertHSVtoRGB( H >= 1.f ? H - 10 * 1e-6f : H, S > 0.f ? S : 10 * 1e-6f, V > 0.f ? V : 1e-6f, col[ 0 ], col[ 1 ], col[ 2 ] );
            s_hue[ hid ] = H;
        }

        // Render
        const int style_alpha8 = IM_F32_TO_INT8_SAT( style.Alpha );
        const ImU32 col_black = IM_COL32( 0, 0, 0, style_alpha8 );
        const ImU32 col_white = IM_COL32( 255, 255, 255, style_alpha8 );
        const ImU32 col_hues[ 7 ] = { IM_COL32( 255, 0, 0, style_alpha8 ), IM_COL32( 255, 255, 0, style_alpha8 ), IM_COL32( 0, 255, 0, style_alpha8 ), IM_COL32( 0, 255, 255, style_alpha8 ), IM_COL32( 0, 0, 255, style_alpha8 ), IM_COL32( 255, 0, 255, style_alpha8 ), IM_COL32( 255, 0, 0, style_alpha8 ) };

        ImVec4 hue_color_f( 1, 1, 1, style.Alpha );
        ColorConvertHSVtoRGB( H, 1, 1, hue_color_f.x, hue_color_f.y, hue_color_f.z );
        const ImU32 hue_color32 = ColorConvertFloat4ToU32( hue_color_f );

        // SV square
        draw_list->AddRectFilledMultiColor( picker_pos, picker_pos + ImVec2( sv_picker_size, sv_picker_size ), col_white, hue_color32, hue_color32, col_white );
        draw_list->AddRectFilledMultiColor( picker_pos, picker_pos + ImVec2( sv_picker_size, sv_picker_size ), 0, 0, col_black, col_black );
        draw_list->AddRect( picker_pos, picker_pos + ImVec2( sv_picker_size, sv_picker_size ), GetColorU32( ImGuiCol_Border ) );

        ImVec2 sv_cursor_pos;
        sv_cursor_pos.x = ImClamp( IM_ROUND( picker_pos.x + ImSaturate( S ) * sv_picker_size ), picker_pos.x + 2, picker_pos.x + sv_picker_size - 2 );
        sv_cursor_pos.y = ImClamp( IM_ROUND( picker_pos.y + ImSaturate( 1 - V ) * sv_picker_size ), picker_pos.y + 2, picker_pos.y + sv_picker_size - 2 );

        // Hue bar
        for ( int i = 0; i < 6; ++i )
            draw_list->AddRectFilledMultiColor( ImVec2( bar0_pos_x, picker_pos.y + i * ( sv_picker_size / 6 ) ), ImVec2( bar0_pos_x + bars_width, picker_pos.y + ( i + 1 ) * ( sv_picker_size / 6 ) ), col_hues[ i ], col_hues[ i ], col_hues[ i + 1 ], col_hues[ i + 1 ] );
        const float bar0_line_y = IM_ROUND( picker_pos.y + H * sv_picker_size );
        draw_list->AddRect( ImVec2( bar0_pos_x, picker_pos.y ), ImVec2( bar0_pos_x + bars_width, picker_pos.y + sv_picker_size ), GetColorU32( ImGuiCol_Border ) );
        draw_list->AddRectFilled( ImVec2( bar0_pos_x, bar0_line_y ), ImVec2( bar0_pos_x + bars_width, bar0_line_y + 4 ), ImColor( 1.f, 1.f, 1.f, 0.4f * style.Alpha ) );
        draw_list->AddRect( ImVec2( bar0_pos_x, bar0_line_y ), ImVec2( bar0_pos_x + bars_width, bar0_line_y + 4 ), GetColorU32( ImGuiCol_Border ) );

        // SV cursor
        const float sv_cursor_rad = 4;
        draw_list->AddRectFilled( sv_cursor_pos - ImVec2( sv_cursor_rad / 2, sv_cursor_rad / 2 ), sv_cursor_pos + ImVec2( sv_cursor_rad / 2, sv_cursor_rad / 2 ), ImColor( 1.f, 1.f, 1.f, 0.4f * style.Alpha ) );
        draw_list->AddRect( sv_cursor_pos - ImVec2( sv_cursor_rad / 2, sv_cursor_rad / 2 ), sv_cursor_pos + ImVec2( sv_cursor_rad / 2, sv_cursor_rad / 2 ), GetColorU32( ImGuiCol_Border ) );

        // Alpha bar (horizontal, under the SV square)
        if ( alpha_bar )
        {
            const float alpha = ImSaturate( col[ 3 ] );
            const ImRect bar1_bb( picker_pos + ImVec2( 0, sv_picker_size + style.ItemInnerSpacing.y ), picker_pos + ImVec2( sv_picker_size, sv_picker_size + style.ItemInnerSpacing.y + bars_width ) );
            draw_list->AddRectFilled( bar1_bb.Min, bar1_bb.Max, ImColor( col[ 0 ], col[ 1 ], col[ 2 ], style.Alpha ) );
            draw_list->AddRect( bar1_bb.Min, bar1_bb.Max, GetColorU32( ImGuiCol_Border ) );
            const float bar1_line_x = IM_ROUND( picker_pos.x + ( 1.0f - alpha ) * sv_picker_size );
            // c_custom::render_arrows_for_horizontal_bar( pos, alpha, 4, bars_width )
            const ImVec2 ap( bar1_line_x - 1, bar1_bb.Min.y );
            draw_list->AddRectFilled( ap, ap + ImVec2( 4, bars_width ), ImColor( 1.f, 1.f, 1.f, 0.4f * ( style.Alpha * style.Alpha ) ) );
            draw_list->AddRect( ap, ap + ImVec2( 4, bars_width ), GetColorU32( ImGuiCol_Border ) );
        }

        EndGroup();

        return value_changed;
    }

    bool ColorEdit( const char* id_label, float col[4], bool alpha )
    {
        ImGuiWindow* window = GetCurrentWindow();
        if ( window->SkipItems )
            return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;

        PushID( id_label );
        const ImGuiID id = window->GetID( "##ColorButton" );
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect bb( pos, pos + ImVec2( 20, 10 ) );
        // skeet ColorButton: zero-size ItemSize so it sits on the
        // checkbox/label line without pushing the layout.
        ItemSize( ImRect( pos, pos ), style.FramePadding.y );

        bool value_changed = false;
        if ( ItemAdd( bb, id ) )
        {
            bool hovered, held;
            const bool pressed = ButtonBehavior( bb, id, &hovered, &held );

            const ImVec4 c( col[ 0 ], col[ 1 ], col[ 2 ], alpha ? col[ 3 ] : 1.f );
            if ( alpha && c.w < 1.f )
                RenderColorRectWithAlphaCheckerboard( window->DrawList, bb.Min, bb.Max, GetColorU32( c ), ImMin( bb.GetWidth(), bb.GetHeight() ) / 2.99f, ImVec2( 0.f, 0.f ), 0.f );
            else
                window->DrawList->AddRectFilled( bb.Min, bb.Max, GetColorU32( ImVec4( c.x, c.y, c.z, 1.f ) ) );
            window->DrawList->AddRect( bb.Min, bb.Max, GetColorU32( ImGuiCol_Border ) );

            if ( pressed )
            {
                OpenPopup( "picker" );
                SetNextWindowPos( bb.GetBL() + ImVec2( 0.0f, style.ItemSpacing.y / 2 ) );
            }
        }

        PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 4, 4 ) );
        PushStyleVar( ImGuiStyleVar_PopupRounding, 0.f );
        PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8, 4 ) );
        PushStyleColor( ImGuiCol_PopupBg, GetColorU32( ImGuiCol_WindowBg ) );

        if ( BeginPopup( "picker", ImGuiWindowFlags_NoMove ) )
        {
            value_changed |= PickerSk( col, alpha );
            EndPopup();
        }

        PopStyleColor();
        PopStyleVar( 3 );
        PopID();

        if ( value_changed )
            MarkItemEdited( id );
        return value_changed;
    }

    // ---------------------------------------------------------------
    //  Text
    // ---------------------------------------------------------------
    void Text( const char* fmt, ... )
    {
        SetCursorPosX( GetCursorPosX() + 20 );
        va_list args;
        va_start( args, fmt );
        TextV( fmt, args );
        va_end( args );
    }

    void TextColored( const ImVec4& col, const char* fmt, ... )
    {
        SetCursorPosX( GetCursorPosX() + 20 );
        va_list args;
        va_start( args, fmt );
        TextColoredV( col, fmt, args );
        va_end( args );
    }

    void TextDisabledWrapped( const char* text )
    {
        SetCursorPosX( GetCursorPosX() + 20 );
        PushTextWrapPos( GetWindowWidth() - 34 );
        TextDisabled( "%s", text );
        PopTextWrapPos();
    }

    // ---------------------------------------------------------------
    //  List area (combo-style frame + scrolling child)
    // ---------------------------------------------------------------
    bool BeginList( const char* id, float height )
    {
        const ImGuiStyle& style = GetStyle();
        SetCursorPosX( GetCursorPosX() + 20 );
        const ImVec2 pos = GetCursorScreenPos();
        const float  w = GetWindowWidth() - 90;

        ImDrawList* dl = GetWindowDrawList();
        dl->AddRectFilledMultiColor( pos, pos + ImVec2( w, height ),
            to_vec4( 31, 31, 31, style.Alpha ), to_vec4( 31, 31, 31, style.Alpha ),
            to_vec4( 36, 36, 36, style.Alpha ), to_vec4( 36, 36, 36, style.Alpha ) );
        dl->AddRect( pos, pos + ImVec2( w, height ), GetColorU32( ImGuiCol_Border ) );

        PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 1, 3 ) );
        PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0, 0, 0, 0 ) );
        const bool r = BeginChild( id, ImVec2( w, height ), ImGuiChildFlags_AlwaysUseWindowPadding );
        PopStyleColor();
        PopStyleVar();
        PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 2 ) );
        return r;
    }

    void EndList()
    {
        PopStyleVar();
        DrawChildFade();
        EndChild();
    }
}
