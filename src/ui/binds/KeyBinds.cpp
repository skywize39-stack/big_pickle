#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/binds/KeyBinds.hpp"
#include "config/Config.hpp"
#include "ui/menu/Menu.hpp"

#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include "ui/menu/SkeetUI.hpp"
#include "ui/binds/PixelFont.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct BindState
    {
        int    key = 0;
        int    mode = 0;
        bool   wasDown = false;
        bool   toggled = false;
        bool   active = false;
        const char* display = nullptr;
        ULONGLONG lastSeen = 0;
    };

    std::mutex g_mtx;
    std::unordered_map<const char*, BindState> g_binds; // keyed by literal ptr
    std::vector<const char*> g_order;                    // stable list order

    // Widget capture state: label ptr currently waiting for a key.
    const void* g_capturing = nullptr;
    bool g_captureArmed = false; // wait until the opening click is released
}

namespace Binds
{
    bool IsCapturing() { return g_capturing != nullptr; }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_binds.clear();
        g_order.clear();
        g_capturing = nullptr;
        g_captureArmed = false;
    }

    bool KeyDown( int vk )
    {
        if ( vk <= 0 || vk >= 256 )
            return false;
        return ( ::GetAsyncKeyState( vk ) & 0x8000 ) != 0;
    }

    bool Active( const char* id, int key, int mode, const char* display )
    {
        std::lock_guard<std::mutex> lock( g_mtx );
        auto it = g_binds.find( id );
        if ( it == g_binds.end() )
        {
            it = g_binds.emplace( id, BindState{} ).first;
            g_order.push_back( id );
        }
        BindState& s = it->second;

        const bool down = KeyDown( key );
        if ( s.key != key || s.mode != mode )
        {
            // A rebind never inherits an armed toggle; a key already held at
            // rebind time must be released and pressed again.
            s.toggled = false;
            s.wasDown = down;
            s.key = key;
            s.mode = mode;
        }

        bool active = false;
        switch ( mode )
        {
        case kToggle:
            if ( down && !s.wasDown )
                s.toggled = !s.toggled;
            active = key > 0 && s.toggled;
            break;
        case kAlways:
            active = true;
            break;
        case kOffHotkey:
            active = !down;
            break;
        case kHold:
        default:
            active = down;
            break;
        }

        s.wasDown = down;
        s.active = active;
        s.display = display;
        s.lastSeen = GetTickCount64();
        return active;
    }

    const char* ModeName( int mode )
    {
        switch ( mode )
        {
        case kToggle:    return "toggled";
        case kAlways:    return "always on";
        case kOffHotkey: return "off hotkey";
        default:         return "holding";
        }
    }

    const char* KeyName( int vk )
    {
        static char buf[ 32 ];
        switch ( vk )
        {
        case 0:            return "-";
        case VK_LBUTTON:   return "M1";
        case VK_RBUTTON:   return "M2";
        case VK_MBUTTON:   return "M3";
        case VK_XBUTTON1:  return "M4";
        case VK_XBUTTON2:  return "M5";
        case VK_SHIFT:     return "SHIFT";
        case VK_LSHIFT:    return "LSHIFT";
        case VK_RSHIFT:    return "RSHIFT";
        case VK_CONTROL:   return "CTRL";
        case VK_LCONTROL:  return "LCTRL";
        case VK_RCONTROL:  return "RCTRL";
        case VK_MENU:      return "ALT";
        case VK_LMENU:     return "LALT";
        case VK_RMENU:     return "RALT";
        case VK_SPACE:     return "SPACE";
        case VK_TAB:       return "TAB";
        case VK_CAPITAL:   return "CAPS";
        case VK_RETURN:    return "ENTER";
        case VK_INSERT:    return "INS";
        case VK_DELETE:    return "DEL";
        case VK_HOME:      return "HOME";
        case VK_END:       return "END";
        case VK_PRIOR:     return "PGUP";
        case VK_NEXT:      return "PGDN";
        case VK_UP:        return "UP";
        case VK_DOWN:      return "DOWN";
        case VK_LEFT:      return "LEFT";
        case VK_RIGHT:     return "RIGHT";
        default: break;
        }
        if ( ( vk >= '0' && vk <= '9' ) || ( vk >= 'A' && vk <= 'Z' ) )
        {
            buf[ 0 ] = static_cast<char>( vk );
            buf[ 1 ] = 0;
            return buf;
        }
        if ( vk >= VK_F1 && vk <= VK_F24 )
        {
            sprintf_s( buf, "F%d", vk - VK_F1 + 1 );
            return buf;
        }
        if ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 )
        {
            sprintf_s( buf, "NUM%d", vk - VK_NUMPAD0 );
            return buf;
        }
        sprintf_s( buf, "0x%02X", vk );
        return buf;
    }

    bool Widget( const char* label, int& key, int& mode )
    {
        // skeet ImGui::Hotkey look: label at +20, "[KEY]" right-aligned at
        // window width - 34 with a dark outline; red while capturing,
        // brighter on hover. RMB opens the bind mode popup.
        bool changed = false;
        ImGui::PushID( label );

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiStyle& style = ImGui::GetStyle();

        const bool capturing = g_capturing == label;
        char text[ 48 ];
        if ( capturing )
            sprintf_s( text, "[-]" );
        else
            sprintf_s( text, "[%s]", KeyName( key ) );

        const char* label_end = ImGui::FindRenderedTextEnd( label );
        const ImVec2 label_size = ImGui::CalcTextSize( label, label_end );
        const ImVec2 text_size  = ImGui::CalcTextSize( text );

        const float w = ImGui::GetWindowWidth() - 34;
        const ImVec2 cur = window->DC.CursorPos;
        const ImRect frame_bb( cur + ImVec2( w - text_size.x, 0 ), cur + ImVec2( w, text_size.y ) );
        const ImRect total_bb( cur + ImVec2( 20, 0 ), frame_bb.Max );

        const ImGuiID id = window->GetID( "##hotkey" );
        ImGui::ItemAdd( frame_bb, id );
        ImGui::ItemSize( frame_bb, style.FramePadding.y );

        const bool hovered = ImGui::IsItemHovered();
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !capturing )
        {
            g_capturing = label;
            g_captureArmed = false;
        }
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
            ImGui::OpenPopup( "##bindmode" );
        if ( hovered && !capturing )
            ImGui::SetTooltip( "LMB: set key  |  RMB: mode (%s)", ModeName( mode ) );

        ImDrawList* dl = window->DrawList;
        if ( label != label_end )
            dl->AddText( ImVec2( total_bb.Min.x, frame_bb.GetCenter().y - label_size.y / 2 ),
                         ImGui::GetColorU32( ImGuiCol_Text ), label, label_end );

        for ( int i = -1; i < 2; ++i )
            for ( int j = -1; j < 2; ++j )
                dl->AddText( frame_bb.Min + ImVec2( (float)i, (float)j ), ImGui::GetColorU32( ImGuiCol_Border, 0.5f ), text );
        const ImU32 kcol = capturing ? IM_COL32( 255, 0, 0, 255 )
                         : hovered   ? IM_COL32( 141, 141, 141, 255 )
                                     : IM_COL32( 114, 114, 114, 255 );
        dl->AddText( frame_bb.Min, kcol, text );

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 3, 4 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 8, 4 ) );
        ImGui::PushStyleColor( ImGuiCol_PopupBg, ImVec4( ImColor( 35, 35, 35 ) ) );
        if ( ImGui::BeginPopup( "##bindmode" ) )
        {
            static const char* names[] = { "Hold", "Toggle", "Always on", "Off hotkey" };
            for ( int i = 0; i < 4; ++i )
                if ( ui::Selectable( names[ i ], mode == i, 0, ImVec2( 90, 0 ) ) )
                {
                    mode = i;
                    changed = true;
                }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar( 2 );

        if ( capturing )
        {
            // Ignore the click that opened capture until it is released,
            // otherwise every bind would instantly become M1.
            if ( !g_captureArmed )
            {
                if ( !KeyDown( VK_LBUTTON ) )
                    g_captureArmed = true;
            }
            else
            {
                for ( int vk = 1; vk < 256; ++vk )
                {
                    if ( !KeyDown( vk ) )
                        continue;
                    if ( vk == VK_ESCAPE )
                    {
                        // cancel
                    }
                    else if ( vk == VK_BACK )
                    {
                        key = 0;
                        changed = true;
                    }
                    else if ( vk == g_config.settings.misc.menu_key )
                    {
                        continue; // menu key, never bindable
                    }
                    else
                    {
                        key = vk;
                        changed = true;
                    }
                    g_capturing = nullptr;
                    break;
                }
            }
        }

        ImGui::PopID();
        return changed;
    }

    // Teamskeet-style list (port of "[FTC] Teamskeet binds" lua):
    // 200px wide, 20px header "KEYBINDS" on 20,20,20 with a 1px accent /
    // gradient line on top, one 15px row per active bind on 25,25,25,
    // NAME (accent) on the left, MODE (white) right-aligned.
    void RenderList()
    {
        auto& o = g_config.settings.visuals.others;
        if ( !o.keybind_list )
            return;

        struct Row { const char* name; const char* mode; };
        std::vector<Row> rows;
        {
            std::lock_guard<std::mutex> lock( g_mtx );
            const ULONGLONG now = GetTickCount64();
            for ( const char* id : g_order )
            {
                const BindState& s = g_binds[ id ];
                // Always-on binds are not "keybinds" — skeet hides them.
                if ( !s.display || !s.active || s.mode == kAlways )
                    continue;
                if ( now - s.lastSeen > 500 ) // feature no longer polling
                    continue;
                const char* mode = s.mode == kToggle ? "toggled" : s.mode == kOffHotkey ? "active" : "holding";
                rows.push_back( { s.display, mode } );
            }
        }

        const bool menuOpen = Menu::bOpen;
        if ( rows.empty() && !menuOpen )
            return;

        constexpr float W = 200.f, HEADER_H = 20.f, ROW_H = 15.f;
        const float H = HEADER_H + ROW_H * (float)rows.size();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;

        if ( o.keybind_x < 0 || o.keybind_y < 0 )
        {
            o.keybind_x = (int)( disp.x * 0.01f );
            o.keybind_y = (int)( disp.y * 0.4f );
        }

        // Window only provides dragging (menu open); everything is drawn
        // by hand. Re-apply the config position whenever it changed from
        // outside (config load), otherwise let ImGui move the window.
        static ImVec2 s_lastPos( -1.f, -1.f );
        const ImVec2 cfgPos( (float)o.keybind_x, (float)o.keybind_y );
        if ( cfgPos.x != s_lastPos.x || cfgPos.y != s_lastPos.y )
            ImGui::SetNextWindowPos( cfgPos, ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImVec2( W, H ), ImGuiCond_Always );

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
        if ( !menuOpen )
            flags |= ImGuiWindowFlags_NoInputs;

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, { 0.f, 0.f } );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowMinSize, { 1.f, 1.f } );
        if ( ImGui::Begin( "##keybinds_list", nullptr, flags ) )
        {
            ImVec2 p = ImGui::GetWindowPos();
            // keep it on screen
            p.x = std::floor( ( std::max )( 0.f, ( std::min )( p.x, disp.x - W ) ) );
            p.y = std::floor( ( std::max )( 0.f, ( std::min )( p.y, disp.y - H ) ) );
            o.keybind_x = (int)p.x;
            o.keybind_y = (int)p.y;
            s_lastPos   = ImVec2( p.x, p.y );
            if ( p.x != ImGui::GetWindowPos().x || p.y != ImGui::GetWindowPos().y )
                ImGui::SetWindowPos( p );

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 accent = IM_COL32( o.keybind_color.r, o.keybind_color.g, o.keybind_color.b, o.keybind_color.a );
            const ImU32 white  = IM_COL32( 255, 255, 255, 255 );

            // header
            dl->AddRectFilled( p, p + ImVec2( W, HEADER_H ), IM_COL32( 20, 20, 20, 255 ) );
            if ( o.keybind_gradient )
            {
                const ImU32 c0 = IM_COL32( 0, 200, 255, 255 ), c1 = IM_COL32( 255, 0, 255, 255 ), c2 = IM_COL32( 175, 255, 0, 255 );
                dl->AddRectFilledMultiColor( p, p + ImVec2( W / 2, 1.f ), c0, c1, c1, c0 );
                dl->AddRectFilledMultiColor( p + ImVec2( W / 2, 0.f ), p + ImVec2( W, 1.f ), c1, c2, c2, c1 );
            }
            else
            {
                dl->AddRectFilled( p, p + ImVec2( W, 1.f ), accent );
            }
            const char* title = "KEYBINDS";
            PixelFont::Draw( dl, p + ImVec2( std::floor( ( W - PixelFont::TextWidth( title ) ) / 2 ), 7.f ), white, title );

            // rows
            const ImU32 nameCol = o.keybind_gradient ? IM_COL32( 100, 200, 255, 255 ) : accent;
            const ImU32 modeCol = o.keybind_gradient ? IM_COL32( 175, 255, 0, 255 ) : white;
            float y = p.y + HEADER_H;
            for ( const Row& r : rows )
            {
                dl->AddRectFilled( ImVec2( p.x, y ), ImVec2( p.x + W, y + ROW_H ), IM_COL32( 25, 25, 25, 255 ) );
                PixelFont::Draw( dl, ImVec2( p.x + 3.f, y + 5.f ), nameCol, r.name );
                PixelFont::Draw( dl, ImVec2( p.x + W - PixelFont::TextWidth( r.mode ) - 11.f, y + 5.f ), modeCol, r.mode );
                y += ROW_H;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar( 3 );
    }
}
