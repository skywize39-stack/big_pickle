#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/binds/KeyBinds.hpp"
#include "ui/menu/Menu.hpp"

#include "features/visuals/esp/ESPDraw.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "ui/menu/SkeetUI.hpp"
#include "resources/menu/SkeetAssets.hpp"
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx9.h>

#include "config/Config.hpp"
#include <unordered_map>
#include <string>
#include "core/GlobalVars.hpp"
#include "sdk/SDK.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "features/misc/voicekill/VoiceKill.hpp"

#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// ==================================================================
//  Skeet / gamesense menu (1:1 port of the supplied skeet sources)
//
//  Layout contract (from the original main.cpp):
//    - borderless window, rainbow strip on top, background pattern,
//      68x68 icon tabs in a 75 px column on the left;
//    - "##Groups" child at (90, 25) holding two columns of
//      resizable group boxes (custom.group_box);
//    - widgets are the skeet ones from SkeetUI (label above combos /
//      sliders, 9x9 checkboxes, hotkeys right aligned).
//  Every widget keeps its original config binding.
// ==================================================================

// Order = ChamsStyle enum (ChamsMaterials.hpp)
static const char* chamsStyleNames[] = {
    "Material", "Glow", "Metallic", "Wireframe", "Flat", "Glossy", "Chrome",
    "Glass", "Pearlescent", "Ghost", "Original"
};
static const char* chamsOverlayNames[] = { "Glow", "Wireframe", "Ghost", "Pulse" };

static const char* espBoxTypes[] = {
    "Full", "Corner"
};

static const char* healthModes[] = {
    "Off", "Bar"
};

// ------------------------------------------------------------------
//  Per-frame keyboard snapshot with proper edge detection.
//
//  GetAsyncKeyState's low bit ("pressed since last call") is consumed
//  system-wide by whoever calls it first — the D3D overlay now polls
//  keys every frame, so relying on (& 1) silently breaks toggles.
//  Snapshot the high bit once per frame instead.
// ------------------------------------------------------------------
static bool g_keyDown[256] = {};
static bool g_keyPrev[256] = {};

static void PollMenuKeys()
{
    memcpy( g_keyPrev, g_keyDown, sizeof( g_keyPrev ) );
    for ( int vk = 1; vk < 256; ++vk )
        g_keyDown[ vk ] = ( GetAsyncKeyState( vk ) & 0x8000 ) != 0;
    // A key already held during injection is not a new press. In particular,
    // Insert must not immediately close the initially open menu.
    static bool initialized=false;
    if (!initialized)
    {
        memcpy(g_keyPrev,g_keyDown,sizeof(g_keyPrev));
        initialized=true;
    }
}

static bool KeyPressed( int vk )
{
    return vk > 0 && vk < 256 && g_keyDown[ vk ] && !g_keyPrev[ vk ];
}

static bool g_menuKeyCapture = false, g_menuKeyArmed = false;
static int g_observedMenuKey = VK_INSERT;
static bool g_menuKeyNeedsRelease = false;

static int MenuKey()
{
    const int key = g_config.settings.misc.menu_key;
    return key >= VK_BACK && key < 256 ? key : VK_INSERT;
}

static void HandleMenuToggle()
{
    const int key = MenuKey();
    if (g_observedMenuKey != key)
    {
        g_observedMenuKey = key;
        g_menuKeyNeedsRelease = g_keyDown[key];
    }
    if (g_menuKeyNeedsRelease)
    {
        if (!g_keyDown[key]) g_menuKeyNeedsRelease = false;
        return;
    }
    if (g_menuKeyCapture || Binds::IsCapturing()) return;
    if (KeyPressed(key)) Menu::bOpen = !Menu::bOpen;
}

static void MenuKeyWidget()
{
    std::string label = g_menuKeyCapture ? "Press a key..." :
        std::string("Menu key: ") + Binds::KeyName(MenuKey());
    label += "###menu_key";
    if (ui::Button(label.c_str()))
    {
        g_menuKeyCapture = true;
        g_menuKeyArmed = false;
    }
    if (!g_menuKeyCapture) return;
    if (!g_menuKeyArmed)
    {
        if (!g_keyDown[VK_LBUTTON]) g_menuKeyArmed = true;
        return;
    }
    if (KeyPressed(VK_ESCAPE)) { g_menuKeyCapture = false; return; }
    for (int vk = VK_BACK; vk < 256; ++vk)
        if (KeyPressed(vk))
        {
            g_config.settings.misc.menu_key = vk;
            g_menuKeyCapture = false;
            g_menuKeyNeedsRelease = true;
            g_observedMenuKey = vk;
            break;
        }
}

// ------------------------------------------------------------------
//  Adapters: old row helpers mapped onto the skeet widgets.
// ------------------------------------------------------------------
// ------------------------------------------------------------------
//  Groupbox layout.
//  Auto mode (masonry): each box is as tall as its content (measured
//  last frame) and goes into the shorter column; the bottom box of each
//  column stretches to the bottom edge, like the original skeet layout.
//  Custom mode: drag a box by its title to any column / position; boxes
//  then stack in that order without gaps (still content-sized, bottom
//  box stretched). Custom orders are saved in the config
//  (misc.menu_layout) and cleared with "Reset layout".
// ------------------------------------------------------------------
struct BoxLayoutInfo
{
    float  manualH = 0.f;     // 0 = automatic height, layout-owned
    float  contentH  = 0.f;   // 0 = not measured yet
    bool   lastInCol = false; // auto mode: bottom box of its column last frame
    int    col       = -1;
    int    seenFrame = -1;
    ImRect rect;              // screen rect this frame
};
struct BoxPlace { int col; float y, h; };
struct TabArrangement
{
    bool custom = false;
    std::vector< std::string > col[ 2 ];
};

static constexpr int   kMaxTabs = 16;
static constexpr float kMinBoxH = 60.f;
static std::unordered_map< std::string, BoxLayoutInfo > g_boxInfo;
static TabArrangement g_arr[ kMaxTabs ];
static std::unordered_map< std::string, BoxPlace > g_place;   // custom mode, this frame
static std::vector< std::string > g_frameCol[ 2 ];             // boxes drawn this frame per column
static float       g_colY[ 2 ]{};
static float       g_layoutTop = 0.f, g_layoutBottom = 0.f, g_layoutX = 0.f;
static std::string g_colLast[ 2 ];
static std::string g_curBox;
static int         g_layoutTab = 0;
static std::string g_appliedLayout = "\x01";                  // config string last parsed

// dragging
static std::string g_dragBox;
static int         g_dragTab = -1;
static ImGuiID     g_dragId  = 0;
static ImVec2      g_dragGrabOffset;
static std::string g_resizeBox,g_appliedHeights="\x01";
static ImGuiID g_resizeId=0;
static int g_resizeTab=-1;

static void SaveBoxHeights()
{
    std::vector<std::string> records;
    for(const auto& pair:g_boxInfo)
        if(pair.second.manualH>0.f)
            records.push_back(pair.first+":"+std::to_string(pair.second.manualH)+";");
    std::sort(records.begin(),records.end());
    std::string text;for(const auto& record:records)text+=record;
    g_config.settings.misc.menu_box_heights=text;
    g_appliedHeights=text;
}

static void ApplyBoxHeights(const std::string& text)
{
    for(auto& pair:g_boxInfo)pair.second.manualH=0.f;
    size_t pos=0;
    while(pos<text.size())
    {
        const size_t end=text.find(';',pos);
        const std::string item=text.substr(pos,end==std::string::npos?end:end-pos);
        pos=end==std::string::npos?text.size():end+1;
        const size_t colon=item.rfind(':');
        if(colon==std::string::npos||colon==0)continue;
        const std::string number=item.substr(colon+1);char* tail=nullptr;
        const float height=std::strtof(number.c_str(),&tail);
        if(tail!=number.c_str()&&*tail=='\0'&&std::isfinite(height)&&height>=kMinBoxH)
            g_boxInfo[item.substr(0,colon)].manualH=std::clamp(height,kMinBoxH,4096.f);
    }
}

static float SnapBoxBottom(float bottom,float target)
{
    return fabsf(bottom-target)<=8.f ? target : bottom;
}

static std::string SerializeLayout()
{
    std::string out;
    for ( int t = 0; t < kMaxTabs; ++t )
    {
        if ( !g_arr[ t ].custom ) continue;
        for ( int c = 0; c < 2; ++c )
            for ( const std::string& n : g_arr[ t ].col[ c ] )
                out += std::to_string( t ) + ":" + std::to_string( c ) + ":" + n + ";";
    }
    return out;
}

static void ApplyLayoutString( const std::string& s )
{
    for ( auto& a : g_arr ) { a.custom = false; a.col[ 0 ].clear(); a.col[ 1 ].clear(); }
    size_t i = 0;
    while ( i < s.size() )
    {
        const size_t e = s.find( ';', i );
        const std::string item = s.substr( i, e == std::string::npos ? std::string::npos : e - i );
        i = e == std::string::npos ? s.size() : e + 1;
        const size_t a = item.find( ':' ), b = a == std::string::npos ? a : item.find( ':', a + 1 );
        if ( b == std::string::npos ) continue;
        const int t = atoi( item.substr( 0, a ).c_str() );
        const int c = atoi( item.substr( a + 1, b - a - 1 ).c_str() );
        if ( t < 0 || t >= kMaxTabs || c < 0 || c > 1 ) continue;
        g_arr[ t ].custom = true;
        g_arr[ t ].col[ c ].push_back( item.substr( b + 1 ) );
    }
}

static void SaveLayoutToConfig()
{
    g_config.settings.misc.menu_layout = SerializeLayout();
    g_appliedLayout = g_config.settings.misc.menu_layout;
}

static void ResetMenuLayout()
{
    for ( auto& a : g_arr ) { a.custom = false; a.col[ 0 ].clear(); a.col[ 1 ].clear(); }
    for(auto& pair:g_boxInfo)pair.second.manualH=0.f;
    g_resizeBox.clear();
    SaveBoxHeights();
    SaveLayoutToConfig();
}

static void ResetConfiguration()
{
    g_config.reset();
    Binds::Reset();
    g_menuKeyCapture = g_menuKeyArmed = false;
    ResetMenuLayout();
}

static float BoxHeightFor( const BoxLayoutInfo& info )
{
    return ImMax(info.manualH>0.f?info.manualH:(info.contentH>0.f?info.contentH:120.f),kMinBoxH);
}

static void LayoutBegin( int tab )
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 start   = ImGui::GetCursorPos();
    g_layoutTab    = ( tab >= 0 && tab < kMaxTabs ) ? tab : 0;
    if(!g_resizeBox.empty() && g_resizeTab!=g_layoutTab)
    {
        if(ImGui::GetActiveID()==g_resizeId)ImGui::ClearActiveID();
        g_resizeBox.clear();SaveBoxHeights();
    }
    g_layoutX      = start.x;
    g_layoutTop    = start.y;
    g_layoutBottom = ImGui::GetWindowHeight() - st.WindowPadding.y;
    g_colY[ 0 ] = g_colY[ 1 ] = g_layoutTop;
    g_colLast[ 0 ].clear();
    g_colLast[ 1 ].clear();
    g_frameCol[ 0 ].clear();
    g_frameCol[ 1 ].clear();
    g_place.clear();

    // config loaded / changed from outside -> re-parse
    if ( g_config.settings.misc.menu_layout != g_appliedLayout )
    {
        g_appliedLayout = g_config.settings.misc.menu_layout;
        ApplyLayoutString( g_appliedLayout );
    }

    if(g_config.settings.misc.menu_box_heights!=g_appliedHeights)
    {
        g_appliedHeights=g_config.settings.misc.menu_box_heights;
        ApplyBoxHeights(g_appliedHeights);
        g_resizeBox.clear();
    }
    TabArrangement& arr = g_arr[ g_layoutTab ];
    if ( !arr.custom )
        return;

    // Custom: stack the boxes seen last frame in the saved order.
    const int frame = ImGui::GetFrameCount();
    for ( int c = 0; c < 2; ++c )
    {
        std::vector< const std::string* > vis;
        for ( const std::string& n : arr.col[ c ] )
        {
            auto it = g_boxInfo.find( n );
            if ( it != g_boxInfo.end() && it->second.seenFrame >= frame - 1 )
                vis.push_back( &n );
        }
        float y = g_layoutTop;
        for ( size_t i = 0; i < vis.size(); ++i )
        {
            const float avail = g_layoutBottom - y;
            float h = BoxHeightFor( g_boxInfo[ *vis[ i ] ] );
            const float reserve=static_cast<float>(vis.size()-i-1)*(kMinBoxH+st.ItemSpacing.y);
            const float maximum=ImMax(kMinBoxH,avail-reserve);
            if(i+1==vis.size() && g_boxInfo[*vis[i]].manualH<=0.f)h=maximum;
            h=ImClamp(h,kMinBoxH,maximum);
            g_place[ *vis[ i ] ] = { c, y, h };
            y += h + st.ItemSpacing.y;
        }
        g_colY[ c ] = y;
    }
}

static bool InArrangement( const TabArrangement& arr, const std::string& n, int* outCol = nullptr )
{
    for ( int c = 0; c < 2; ++c )
        for ( const std::string& x : arr.col[ c ] )
            if ( x == n ) { if ( outCol ) *outCol = c; return true; }
    return false;
}

static void LayoutEnd()
{
    TabArrangement& arr = g_arr[ g_layoutTab ];
    const ImGuiStyle& st = ImGui::GetStyle();

    if ( !arr.custom )
    {
        for ( auto& kv : g_boxInfo )
            if ( kv.second.col >= 0 )
                kv.second.lastInCol = ( kv.first == g_colLast[ kv.second.col ] );
        // remember the auto layout so a drag starts from what you see
        arr.col[ 0 ] = g_frameCol[ 0 ];
        arr.col[ 1 ] = g_frameCol[ 1 ];
    }

    if ( g_dragBox.empty() )
        return;
    if ( g_dragTab != g_layoutTab )
    {
        g_dragBox.clear();
        return;
    }

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList*  dl  = win->DrawList;
    const ImVec2 mouse = ImGui::GetMousePos();
    const float  colW  = ui::BoxW();
    const float  x0    = win->Pos.x + g_layoutX;
    const float  x1    = x0 + colW + st.ItemSpacing.x;
    const int    col   = mouse.x < x1 - st.ItemSpacing.x * 0.5f ? 0 : 1;

    // visible boxes of the target column (drawn this frame), minus the dragged one
    std::vector< std::string > vis;
    for ( const std::string& n : g_frameCol[ col ] )
        if ( n != g_dragBox ) vis.push_back( n );
    // sort by on-screen y (custom mode already is, auto mode too)
    size_t index = 0;
    while ( index < vis.size() && g_boxInfo[ vis[ index ] ].rect.GetCenter().y < mouse.y )
        ++index;

    // insertion marker
    const float cx = col == 0 ? x0 : x1;
    float ly;
    if ( vis.empty() )         ly = win->Pos.y + g_layoutTop - 4.f;
    else if ( index == 0 )     ly = g_boxInfo[ vis[ 0 ] ].rect.Min.y - st.ItemSpacing.y * 0.5f - 1.f;
    else                       ly = g_boxInfo[ vis[ index - 1 ] ].rect.Max.y + st.ItemSpacing.y * 0.5f;
    dl->AddRectFilled( ImVec2( cx, ly - 1.f ), ImVec2( cx + colW, ly + 1.f ), ui::Accent() );

    // ghost of the dragged box caption following the mouse
    {
        const BoxLayoutInfo& di = g_boxInfo[ g_dragBox ];
        const ImVec2 gp  = mouse - g_dragGrabOffset;
        const ImVec2 gsz( di.rect.GetWidth(), 18.f );
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled( gp, gp + gsz, IM_COL32( 23, 23, 23, 220 ) );
        fg->AddRect( gp, gp + gsz, ui::Accent() );
        const char* end = ImGui::FindRenderedTextEnd( g_dragBox.c_str() );
        fg->AddText( ui::f_bold ? ui::f_bold : ImGui::GetFont(), 12.f, gp + ImVec2( 8, 3 ), IM_COL32( 220, 220, 220, 255 ), g_dragBox.c_str(), end );
    }

    if ( ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
    {
        ImGui::KeepAliveID( g_dragId );
        return;
    }

    // drop: rebuild the arrangement with the box at the chosen spot
    arr.custom = true;
    for ( int c = 0; c < 2; ++c )
        arr.col[ c ].erase( std::remove( arr.col[ c ].begin(), arr.col[ c ].end(), g_dragBox ), arr.col[ c ].end() );
    auto& list = arr.col[ col ];
    if ( index < vis.size() )
    {
        auto it = std::find( list.begin(), list.end(), vis[ index ] );
        list.insert( it, g_dragBox );
    }
    else if ( !vis.empty() )
    {
        auto it = std::find( list.begin(), list.end(), vis.back() );
        list.insert( it == list.end() ? it : it + 1, g_dragBox );
    }
    else
        list.push_back( g_dragBox );

    if ( ImGui::GetActiveID() == g_dragId )
        ImGui::ClearActiveID();
    g_dragBox.clear();
    SaveLayoutToConfig();
}

static void BeginGroupBox( const char* name, float frac, int autoColumn = -1, bool autoTail = true )
{
    const ImGuiStyle& st = ImGui::GetStyle();
    BoxLayoutInfo& info  = g_boxInfo[ name ];
    TabArrangement& arr  = g_arr[ g_layoutTab ];

    int   col;
    float y, h;
    auto pl = g_place.find( name );
    if ( arr.custom && pl != g_place.end() )
    {
        col = pl->second.col;
        y   = pl->second.y;
        h   = pl->second.h;
    }
    else
    {
        if ( arr.custom )
        {
            // new / just re-appeared box: bottom of its saved column,
            // or of the shorter column when it's unknown
            int saved = -1;
            if ( InArrangement( arr, name, &saved ) )
                col = saved;
            else
            {
                col = g_colY[ 1 ] < g_colY[ 0 ] - 0.5f ? 1 : 0;
                arr.col[ col ].push_back( name );
            }
        }
        else
            col = autoColumn >= 0 ? autoColumn : (g_colY[ 1 ] < g_colY[ 0 ] - 0.5f ? 1 : 0);

        y = g_colY[ col ];
        const float avail = g_layoutBottom - y;
        h = info.manualH>0.f?info.manualH:(info.contentH>0.f?info.contentH:ui::BoxH(frac));
        h = ImMax( h, kMinBoxH );
        if ( !arr.custom && autoTail && info.manualH<=0.f && info.lastInCol && info.col == col )
            h = ImMax( h, avail );          // fill the column to the bottom
        const float maximum = !arr.custom && !autoTail ?
            ImMax(kMinBoxH, avail - kMinBoxH - st.ItemSpacing.y) : avail;
        if ( h > maximum )
            h = ImMax( maximum, kMinBoxH );   // too tall: box scrolls inside
        g_colY[ col ] = y + h + st.ItemSpacing.y;
    }

    info.col = col;
    info.seenFrame = ImGui::GetFrameCount();
    g_colLast[ col ] = name;
    g_frameCol[ col ].push_back( name );
    g_curBox = name;

    const ImVec2 local( g_layoutX + col * ( ui::BoxW() + st.ItemSpacing.x ), y );
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImVec2 screen = win->Pos + local - win->Scroll;
    const float  w = ui::BoxW();
    info.rect = ImRect( screen, screen + ImVec2( w, h ) );

    // Layout-owned resize. Changing a box must reflow its column, never
    // leave the widget at a height different from the placement rectangle.
    const ImRect grip(info.rect.Max-ImVec2(10,10),info.rect.Max-ImVec2(1,1));
    const bool resizeHover=ImGui::IsMouseHoveringRect(grip.Min,grip.Max) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if(g_resizeBox.empty() && g_dragBox.empty() && resizeHover &&
       ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive() &&
       !ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId))
    {
        g_resizeBox=name;g_resizeTab=g_layoutTab;g_resizeId=win->GetID((std::string(name)+"##boxresize").c_str());
        ImGui::SetActiveID(g_resizeId,win);
        arr.custom=true; // preserve column/order while height is being dragged
        SaveLayoutToConfig();
    }
    const bool resizing=g_resizeBox==name;
    if(resizeHover||resizing)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(resizing)
    {
        ImGui::KeepAliveID(g_resizeId);
        float bottom=ImGui::GetMousePos().y;
        bottom=SnapBoxBottom(bottom,win->Pos.y+g_layoutBottom-win->Scroll.y);
        for(const auto& pair:g_boxInfo)
            if(pair.first!=name && pair.second.seenFrame>=ImGui::GetFrameCount()-1 && pair.second.col!=col)
            {
                bottom=SnapBoxBottom(bottom,pair.second.rect.Max.y);
                bottom=SnapBoxBottom(bottom,pair.second.rect.Min.y-st.ItemSpacing.y);
            }
        const float maximum=ImMax(kMinBoxH,g_layoutBottom-y);
        info.manualH=ImClamp(bottom-screen.y,kMinBoxH,maximum);
        if(!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if(ImGui::GetActiveID()==g_resizeId)ImGui::ClearActiveID();
            g_resizeBox.clear();SaveBoxHeights();
        }
    }
    // Retain the existing resize-grip popup to return a panel to auto height.
    const std::string popup=std::string(name)+".popup";
    if(resizeHover && ImGui::IsMouseClicked(ImGuiMouseButton_Right))ImGui::OpenPopup(popup.c_str());
    if(ImGui::BeginPopup(popup.c_str()))
    {
        if(ImGui::Selectable("Calculate height",info.manualH<=0.f))
        {info.manualH=0.f;SaveBoxHeights();}
        ImGui::EndPopup();
    }

    // drag handle = caption strip
    const ImRect handle( screen + ImVec2( 0, -7 ), screen + ImVec2( w, 9 ) );
    const ImGuiID id = win->GetID( ( std::string( name ) + "##boxdrag" ).c_str() );
    if ( g_dragBox.empty() && g_resizeBox.empty() && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) &&
         ImGui::IsMouseHoveringRect( handle.Min, handle.Max ) &&
         ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) &&
         !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId ) )
    {
        g_dragBox = name;
        g_dragTab = g_layoutTab;
        g_dragId  = id;
        g_dragGrabOffset = ImGui::GetMousePos() - screen;
        ImGui::SetActiveID( id, win );   // keeps the menu window from moving
    }
    const bool dragged = g_dragBox == name;
    if ( dragged )
        ImGui::KeepAliveID( id );
    else if ( g_dragBox.empty() && ImGui::IsMouseHoveringRect( handle.Min, handle.Max ) &&
              ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) )
        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeAll );

    ImGui::SetCursorPos( local );
    if ( dragged ) ImGui::PushStyleVar( ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f );
    ui::BeginGroupBox( name, ImVec2( w, h ), resizing );
    if ( dragged ) ImGui::PopStyleVar();
}

static void EndGroupBox()
{
    ui::EndGroupBox();
    if ( !g_curBox.empty() )
        g_boxInfo[ g_curBox ].contentH = ui::LastGroupBoxContentHeight();
}

static bool RowCombo( const char* label, int* v, const char* const items[], int count )
{
    return ui::Combo( label, v, items, count );
}

static void RowSliderFloat( const char* label, float* v, float mn, float mx, const char* fmt = "%.1f" )
{
    ui::SliderFloat( label, v, mn, mx, fmt );
}

static void RowSliderInt( const char* label, int* v, int mn, int mx, const char* fmt = "%d" )
{
    ui::SliderInt( label, v, mn, mx, fmt );
}

static bool RowInputText( const char* label, char* buf, size_t size )
{
    return ui::InputText( label, buf, size );
}

static bool RowBeginCombo( const char* label, const char* preview )
{
    return ui::BeginCombo( label, preview );
}

// Color swatch at the right edge of the current line
// (skeet: SameLine( GetWindowWidth() - 54 ) + ColorEdit4).
static bool InlineColor( const char* id, int& r, int& g, int& b, int* a )
{
    float c[4] = { r / 255.f, g / 255.f, b / 255.f, a ? *a / 255.f : 1.f };
    ImGui::SameLine( ImGui::GetWindowWidth() - 54 );
    if ( !ui::ColorEdit( id, c, a != nullptr ) )
        return false;
    r = (int)( c[0] * 255.f + 0.5f );
    g = (int)( c[1] * 255.f + 0.5f );
    b = (int)( c[2] * 255.f + 0.5f );
    if ( a ) *a = (int)( c[3] * 255.f + 0.5f );
    return true;
}

static bool InlineColor( const char* id, cfg::box_color_t& col )
{
    return InlineColor( id, col.r, col.g, col.b, &col.a );
}

static bool RowColor4( const char* label, int& r, int& g, int& b, int& a )
{
    const char* h = strstr( label, "##" );
    const int   n = h ? (int)( h - label ) : (int)strlen( label );
    ui::Text( "%.*s", n, label );
    return InlineColor( label, r, g, b, &a );
}

static void WrappedText( const char* text )
{
    ui::TextDisabledWrapped( text );
}

static void MaskMultiCombo( const char* label, int& mask, const char* const names[], const int bits[], int count )
{
    bool v[ 16 ] = {};
    for ( int i = 0; i < count; ++i )
        v[ i ] = ( mask & bits[ i ] ) != 0;
    if ( ui::MultiCombo( label, v, names, count ) )
        for ( int i = 0; i < count; ++i )
        {
            if ( v[ i ] ) mask |=  bits[ i ];
            else          mask &= ~bits[ i ];
        }
}

static void BoolMultiCombo( const char* label, bool* const ptrs[], const char* const names[], int count )
{
    bool v[ 16 ] = {};
    for ( int i = 0; i < count; ++i )
        v[ i ] = *ptrs[ i ];
    if ( ui::MultiCombo( label, v, names, count ) )
        for ( int i = 0; i < count; ++i )
            *ptrs[ i ] = v[ i ];
}

// ==================================================================
//  Tabs
// ==================================================================

// The old four-panel rage layout cannot be meaningfully reused by the
// three-panel page. Reset only that tab, leaving every other tab untouched.
static void MigrateRageLayout()
{
    auto& arr = g_arr[g_layoutTab];
    if (!arr.custom) return;
    const auto has = [&](const char* name) { return InArrangement(arr, name); };
    if (has("Weapon type##rage") || has("Other##rage")) return;
    if (!has("Aimbot##rage") && !has("Weapon##rage") &&
        !has("Accuracy##rage") && !has("Target selection##rage") &&
        !has("Overrides##rage") && !has("Keys##rage")) return;
    arr = TabArrangement{};
    for (const char* name : {"Aimbot##rage", "Weapon##rage", "Accuracy##rage",
                            "Target selection##rage", "Overrides##rage", "Keys##rage"})
    {
        auto it = g_boxInfo.find(name);
        if (it != g_boxInfo.end()) it->second.manualH = 0.f;
    }
    g_place.clear();
    g_colY[0] = g_colY[1] = g_layoutTop;
    SaveBoxHeights();
    SaveLayoutToConfig();
}

static void TabRage()
{
    auto& a = g_config.settings.aimbot;
    auto& profiles = a.weapon_profiles;
    profiles.editor_profile = std::clamp(profiles.editor_profile, 0, cfg::RAGE_WEAPON_PROFILE_COUNT - 1);
    MigrateRageLayout();

    // Global edits the legacy shared settings. Choosing a weapon class turns
    // weapon profiles back on without copying over any saved class settings.
    static const char* weaponTypes[] = {
        "Global", "Default / Unknown", "Pistols", "Heavy pistols (Deagle/R8)",
        "SMGs", "Rifles", "Scout / SSG 08", "AWP", "Auto snipers",
        "Shotguns", "LMGs", "Knife / Utility"
    };
    BeginGroupBox("Weapon type##rage", 0.10f, 0, false);
    int weaponType = profiles.enabled ? profiles.editor_profile + 1 : 0;
    if (RowCombo("##weapon_type", &weaponType, weaponTypes, IM_ARRAYSIZE(weaponTypes)))
    {
        profiles.enabled = weaponType > 0;
        if (profiles.enabled) profiles.editor_profile = weaponType - 1;
    }
    EndGroupBox();

    static const char* stopModes[] = { "Off", "Slide", "Full" };
    static const char* preferenceModes[] = { "Neutral", "Prefer head", "Prefer body" };
    static const char* safeModes[] = { "Off", "Prefer safe center", "Force safe center" };
    const auto drawSettings = [&](auto& p)
    {
        BeginGroupBox("Aimbot##rage", 0.90f, 0);
        ui::Checkbox("Enabled##aim", &a.enabled);
        ui::Checkbox("Rage mode", &a.rage);
        ui::Checkbox("On key", &a.on_key);
        if (a.on_key) Binds::Widget("Aimbot key##aim", a.key, a.key_mode);
        if (a.rage)
        {
            if (profiles.enabled) ui::Checkbox("Profile enabled", &profiles.profile[profiles.editor_profile].enabled);
            static const char* hbNames[] = { "Head", "Chest", "Stomach", "Left arm", "Right arm", "Left leg", "Right leg" };
            static const int hbBits[] = { cfg::HITGROUP_BIT_HEAD, cfg::HITGROUP_BIT_CHEST,
                cfg::HITGROUP_BIT_STOMACH, cfg::HITGROUP_BIT_LEFTARM, cfg::HITGROUP_BIT_RIGHTARM,
                cfg::HITGROUP_BIT_LEFTLEG, cfg::HITGROUP_BIT_RIGHTLEG };
            MaskMultiCombo("Target hitbox", p.hitgroup_mask, hbNames, hbBits, IM_ARRAYSIZE(hbNames));
            static const char* mpNames[] = { "Head", "Chest", "Stomach", "Legs", "Feet" };
            bool* const mpPtrs[] = { &p.multipoint_head, &p.multipoint_chest,
                &p.multipoint_stomach, &p.multipoint_legs, &p.multipoint_feet };
            BoolMultiCombo("Multi-point", mpPtrs, mpNames, IM_ARRAYSIZE(mpNames));
            RowSliderFloat("Multi-point scale", &p.multipoint_scale, 0.f, 150.f, "%.0f%%");
            RowSliderFloat("Stomach scale", &p.stomach_scale, 0.f, 150.f, "%.0f%%");
            RowSliderFloat("Minimum hit chance", &p.hit_chance, 0.f, 100.f, "%.0f%%");
            RowSliderInt("Minimum damage", &p.min_damage, 1, 100);
            if constexpr (requires { p.min_damage_override_on_key; })
            {
                ui::Checkbox("Minimum damage override", &p.min_damage_override_on_key);
                if (p.min_damage_override_on_key)
                {
                    RowSliderInt("Override minimum damage", &p.min_damage_override, 1, 100);
                    Binds::Widget("Override key##md", p.min_damage_override_key, p.min_damage_override_key_mode);
                    ui::Checkbox("Override indicator", &p.min_damage_override_indicator);
                }
                RowCombo("Safe point", &p.safe_point_mode, safeModes, IM_ARRAYSIZE(safeModes));
                RowCombo("Hitbox preference", &p.hitbox_preference, preferenceModes, IM_ARRAYSIZE(preferenceModes));
                ui::Checkbox("Force body aim", &p.force_body_on_key);
                if (p.force_body_on_key) Binds::Widget("Body aim key##fb", p.force_body_key, p.force_body_key_mode);
                ui::Checkbox("Force head aim", &p.force_head_on_key);
                if (p.force_head_on_key) Binds::Widget("Head aim key##fh", p.force_head_key, p.force_head_key_mode);
            }
            RowCombo("Quick stop", &p.auto_stop, stopModes, IM_ARRAYSIZE(stopModes));
            ui::Checkbox("Quick stop on key", &p.auto_stop_on_key);
            if (p.auto_stop_on_key) Binds::Widget("Stop key##as", p.auto_stop_key, p.auto_stop_key_mode);
        }
        else
        {
            static const char* bones[] = { "Head", "Neck", "Chest", "Stomach" };
            RowCombo("Bone##aim", &a.bone, bones, IM_ARRAYSIZE(bones));
            RowSliderInt("Bone index (0 = auto)##aim", &a.bone_index, 0, 127);
            a.bone_index = std::clamp(a.bone_index, 0, 127);
            RowSliderFloat("Field of view##aim", &a.fov, 1.f, 180.f, "%.1f");
            ui::Checkbox("Silent aim##aim", &a.silent);
        }
        EndGroupBox();

        BeginGroupBox("Other##rage", 1.f, 1);
        if (a.rage)
        {
            ui::Checkbox("Automatic fire", &p.auto_fire);
            ui::Checkbox("Automatic penetration", &p.auto_wall);
            if (p.auto_wall) RowSliderFloat("Penetration minimum damage", &p.wall_min_dmg, 1.f, 100.f, "%.0f");
            ui::Checkbox("Automatic scope", &p.auto_scope);
            ui::Checkbox("Automatic revolver (R8)", &a.auto_revolver);
        }

        auto& dt = g_config.settings.misc.doubletap;
        ui::Checkbox("Double tap", &dt.enabled, true);
        if (dt.enabled)
        {
            Binds::Widget("Double tap key##dt", dt.key, dt.key_mode);
            RowSliderInt("Shift commands##dt", &dt.ticks, 2, 15);
            ui::Checkbox("Defensive##dt", &dt.defensive);
            ui::Checkbox("Indicator##dt", &dt.indicator);
        }

        auto& m = g_config.settings.misc;
        ui::Checkbox("Quick peek assist", &m.auto_peek);
        InlineColor("##peek_color", m.auto_peek_color);
        if (m.auto_peek)
        {
            Binds::Widget("Peek key##peek", m.auto_peek_key, m.auto_peek_mode);
            RowSliderFloat("Quick peek assist distance", &m.auto_peek_max_distance, 50.f, 500.f, "%.0f u");
        }
        if (a.rage)
        {
            RowSliderFloat("Maximum FOV", &p.fov, 1.f, 180.f, "%.1f");
            if constexpr (requires { p.fire_delay; })
                RowSliderInt("Fire delay", &p.fire_delay, 0, 14, "%d ticks");
            ui::Checkbox("Backtrack", &a.backtrack);
            if (a.backtrack)
            {
                RowSliderInt("Backtrack window##bt", &a.backtrack_ticks, 1, 12, "%d ticks");
                ui::Checkbox("Backtrack-only test", &a.backtrack_only_test, true);
            }
            else a.backtrack_only_test = false;
        }
        EndGroupBox();
    };
    if (profiles.enabled) drawSettings(profiles.profile[profiles.editor_profile]);
    else drawSettings(a);
}

static void TabAntiAim()
{
    auto& aa = g_config.settings.misc.antiaim;
    auto& fl = g_config.settings.misc.fakelag;
    auto& fp = g_config.settings.misc.fake_ping;

    BeginGroupBox( "Anti-aimbot angles", 0.62f );
    ui::Checkbox( "Enabled##aa", &aa.enabled );
    if ( aa.enabled )
    {
        static const char* yawModes[]   = { "Backward", "Left", "Right", "Jitter", "Spin", "Backward Jitter" };
        static const char* pitchModes[] = { "Down", "Up", "Zero" };
        static const char* baseModes[]  = { "Local view", "At targets" };

        RowCombo( "Pitch##aa", &aa.pitch_mode, pitchModes, IM_ARRAYSIZE( pitchModes ) );
        RowCombo( "Yaw base##aa", &aa.base_yaw, baseModes, IM_ARRAYSIZE( baseModes ) );
        RowCombo( "Yaw##aa", &aa.yaw_mode, yawModes, IM_ARRAYSIZE( yawModes ) );
        if ( aa.yaw_mode == 4 )
            RowSliderFloat( "Spin speed##aa", &aa.spin_speed, 1.f, 90.f, "%.0f" );

        ui::Checkbox( "Yaw jitter##aa", &aa.jitter_enabled );
        if ( aa.jitter_enabled )
        {
            static const char* jitterModes[] = { "Offset", "Random", "Sway" };
            RowCombo( "Jitter mode##aa", &aa.jitter_mode, jitterModes, IM_ARRAYSIZE( jitterModes ) );
            RowSliderFloat( "Jitter range##aa", &aa.jitter_range, 1.f, 180.f, "%.0f" );
            RowSliderInt( "Jitter speed##aa", &aa.jitter_speed, 1, 16, "%d" );
            WrappedText( "applied on top of the base yaw" );
        }

        ui::Checkbox( "Freestanding on key##aa", &aa.freestand_on_key );
        if ( aa.freestand_on_key )
        {
            Binds::Widget( "Freestanding key##fs", aa.freestand_key, aa.freestand_key_mode );
            ui::Checkbox( "Disable jitter on freestanding##aa", &aa.freestand_disable_jitter );
            ui::Checkbox( "Flip freestanding side##aa", &aa.freestand_flip );
        }

        ui::Checkbox( "Manual anti-aim##aa", &aa.manual_enabled );
        if ( aa.manual_enabled )
        {
            static int s_manualMode = 1; // display only: manuals are always press-to-toggle
            int m1 = s_manualMode, m2 = s_manualMode, m3 = s_manualMode;
            Binds::Widget( "Manual left##aa",  aa.manual_left_key,  m1 );
            Binds::Widget( "Manual right##aa", aa.manual_right_key, m2 );
            Binds::Widget( "Manual back##aa",  aa.manual_back_key,  m3 );
            ui::Checkbox( "Disable jitter on manual##aa", &aa.manual_disable_jitter );
            ui::Checkbox( "Manual arrows##aa", &aa.manual_indicator );
            WrappedText( "press again to release; overrides freestand" );
        }
    }
    EndGroupBox();

    BeginGroupBox( "Fake ping", 0.38f );
    ui::Checkbox( "Enabled##fp", &fp.enabled );
    if ( fp.enabled )
    {
        RowSliderInt( "Added latency##fp", &fp.delay_ms, 16, 218, "%d ms" );
        ui::Checkbox( "Indicator##fp", &fp.indicator );
        WrappedText( "Incoming sequence latency; does not choke CL_Move" );
        if ( fl.enabled )
            WrappedText( "Independent from Fake Lag packet choking" );
    }
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)


    BeginGroupBox( "Fake lag", 0.4f );
    ui::Checkbox( "Enabled##fl", &fl.enabled );
    if ( fl.enabled )
    {
        RowSliderInt( "Limit##fl", &fl.choke_ticks, 1, 14 );
        RowSliderInt( "Variance##fl", &fl.variance, 0, 6 );
    }
    auto& fd = g_config.settings.misc.fake_duck;
    ui::Checkbox( "Fake duck", &fd.enabled );
    if ( fd.enabled )
        Binds::Widget( "Fake duck key##fd", fd.key, fd.key_mode );
    EndGroupBox();

}

static void TabTriggerbot()
{
    auto& t = g_config.settings.triggerbot;

    BeginGroupBox( "Triggerbot", 1.f );
    ui::Checkbox( "Enabled##trig", &t.enabled );
    RowSliderInt( "Delay##trig", &t.delay_ms, 0, 200, "%d ms" );
    static const char* hitgroupNames[] = { "Any", "Head", "Chest", "Stomach" };
    RowCombo( "Hitgroup##trig", &t.hitgroup, hitgroupNames, IM_ARRAYSIZE( hitgroupNames ) );
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "Activation##trig", 1.f );
    ui::Checkbox( "On key##trig", &t.on_key );
    if ( t.on_key )
        Binds::Widget( "Trigger key##trig", t.key, t.key_mode );
    EndGroupBox();
}

static void TabVisuals()
{
    auto& e = g_config.settings.visuals.esp;

    BeginGroupBox( "Player ESP", 1.f );
    ui::Checkbox( "Enabled##esp", &e.esp );
    ui::Checkbox( "Local player##esp", &e.local );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Show ESP on yourself (thirdperson only)." );

    ui::Checkbox( "Bounding box##box", &e.box );
    InlineColor( "##boxcol", e.box_color );
    if ( e.box )
    {
        RowCombo( "Box type##box", &e.box_type, espBoxTypes, IM_ARRAYSIZE( espBoxTypes ) );
        ui::Checkbox( "Fill box##box", &e.fill_box );
        InlineColor( "##fillcol", e.fill_color );
    }

    ui::Checkbox( "Name", &e.name );
    RowCombo( "Health##mode", &e.health, healthModes, IM_ARRAYSIZE( healthModes ) );
    RowCombo( "Armor##mode", &e.armor, healthModes, IM_ARRAYSIZE( healthModes ) );
    ui::Checkbox( "Weapon", &e.weapon );
    if ( e.weapon )
    {
        static const char* weaponModes[] = { "Text", "Icon" };
        RowCombo( "Weapon style##weaponEsp", &e.weapon_style, weaponModes, IM_ARRAYSIZE( weaponModes ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Icon uses the embedded csgo icon font." );
        if ( e.weapon_style != 0 )
        {
            RowSliderInt( "Icon size##weaponEsp", &e.weapon_icon_size, 10, 24 );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "CSSO Gun-Icons glyph size in pixels." );
        }
    }
    ui::Checkbox( "Skeleton", &e.skeletons );
    ui::Checkbox( "Dropped weapons / grenades", &e.weapons );
    if ( e.weapons )
        ui::Checkbox( "Dropped weapon boxes", &e.weapon_boxes );

    ui::Checkbox( "Flags##flags", &e.show_flags );
    if ( e.show_flags )
    {
        static const char* flagNames[] = { "Scoped", "Defusing", "Planting", "Kit", "Helmet", "Flashed" };
        bool* const flagPtrs[] = { &e.flag_scoped, &e.flag_defuse, &e.flag_plant,
                                   &e.flag_kit, &e.flag_helmet, &e.flag_flashed };
        BoolMultiCombo( "Flag list##flags", flagPtrs, flagNames, IM_ARRAYSIZE( flagNames ) );
    }
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "Off-screen arrows", 0.5f );
    ui::Checkbox( "Enabled##offscreen", &e.offscreen_arrows );
    InlineColor( "##offcol", e.offscreen_arrow_color );
    if ( e.offscreen_arrows )
    {
        static const char* arrowStyles[] = { "Triangle", "Chevron", "Radar Pip", "Arrow" };
        RowCombo( "Style##offscreen", &e.offscreen_arrow_style, arrowStyles, IM_ARRAYSIZE( arrowStyles ) );
        RowSliderFloat( "Size##offscreen", &e.offscreen_arrow_scale, 0.5f, 2.f, "%.2f" );
        ui::Checkbox( "Outline##offscreen", &e.offscreen_arrow_outline );
    }
    EndGroupBox();

    BeginGroupBox( "Fonts##esp", 0.5f );
    // Ranges match the Surface fonts pre-created in SurfaceRenderer.
    RowSliderInt( "Name size##font", &e.name_font_size, 8, 20 );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Verdana, antialiased." );
    RowSliderInt( "Item size##font", &e.item_font_size, 6, 12 );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Small Fonts, no antialiasing.\nUsed for flags and weapon names." );
    EndGroupBox();
}

static void TabChams()
{
    auto& c = g_config.settings.visuals.chams;

    BeginGroupBox( "Player models", 1.f );
    ui::Checkbox( "Enabled##chams", &c.chams );
    ui::Checkbox( "Rainbow", &c.rainbow );
    if ( c.rainbow )
        RowSliderFloat( "Rainbow speed##rainbow", &c.rainbow_speed, 0.1f, 5.0f, "%.1f" );
    RowColor4( "Visible##visColor", c.visible_r, c.visible_g, c.visible_b, c.visible_a );
    RowCombo( "Visible style##visible", &c.chams_style_visible, chamsStyleNames, IM_ARRAYSIZE( chamsStyleNames ) );
    RowColor4( "Occluded##occColor", c.occluded_r, c.occluded_g, c.occluded_b, c.occluded_a );
    RowCombo( "Occluded style##occluded", &c.chams_style_occluded, chamsStyleNames, IM_ARRAYSIZE( chamsStyleNames ) );
    ui::Checkbox( "Overlay##enemyOv", &c.overlay );
    InlineColor( "##enemyOvColor", c.overlay_r, c.overlay_g, c.overlay_b, &c.overlay_a );
    RowCombo( "Overlay style##ovstyle", &c.overlay_style, chamsOverlayNames, IM_ARRAYSIZE( chamsOverlayNames ) );
    WrappedText( "Original = game textures; add an overlay on top. Occluded Original = no xray." );
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "Local player##chams", 1.f / 3.f );
    ui::Checkbox( "Enabled##localChams", &c.local );
    InlineColor( "##localColor", c.local_r, c.local_g, c.local_b, &c.local_a );
    if ( c.local )
    {
        RowCombo( "Style##local", &c.local_style, chamsStyleNames, IM_ARRAYSIZE( chamsStyleNames ) );
        ui::Checkbox( "Overlay##localOv", &c.local_overlay );
        InlineColor( "##localOvColor", c.local_ov_r, c.local_ov_g, c.local_ov_b, &c.local_ov_a );
    }
    EndGroupBox();

    BeginGroupBox( "Arms##chams", 1.f / 3.f );
    ui::Checkbox( "Enabled##armsChams", &c.arms );
    InlineColor( "##armsColor", c.arms_r, c.arms_g, c.arms_b, &c.arms_a );
    if ( c.arms )
    {
        RowCombo( "Style##arms", &c.arms_style, chamsStyleNames, IM_ARRAYSIZE( chamsStyleNames ) );
        ui::Checkbox( "Overlay##armsOv", &c.arms_overlay );
        InlineColor( "##armsOvColor", c.arms_ov_r, c.arms_ov_g, c.arms_ov_b, &c.arms_ov_a );
    }
    EndGroupBox();

    BeginGroupBox( "Weapon##chams", 1.f / 3.f );
    ui::Checkbox( "Enabled##weaponChams", &c.weapon );
    InlineColor( "##weaponColor", c.weapon_r, c.weapon_g, c.weapon_b, &c.weapon_a );
    if ( c.weapon )
    {
        RowCombo( "Style##weapon", &c.weapon_style, chamsStyleNames, IM_ARRAYSIZE( chamsStyleNames ) );
        ui::Checkbox( "Overlay##weaponOv", &c.weapon_overlay );
        InlineColor( "##weaponOvColor", c.weapon_ov_r, c.weapon_ov_g, c.weapon_ov_b, &c.weapon_ov_a );
    }
    EndGroupBox();
}

static void VoiceSection( const char* title, bool& enabled, int& mode, char* selected, size_t selectedSize,
                          const char* idSuffix, void ( *refresh )(), int clipCount,
                          const char* ( *clipName )( int ), const char* folder )
{
    static const char* voiceModeNames[] = { "Manual", "Random", "In order" };
    char id[ 64 ];

    sprintf_s( id, "%s##%s", title, idSuffix );
    ui::Checkbox( id, &enabled );
    mode = std::clamp( mode, 0, 2 );
    sprintf_s( id, "Mode##%s", idSuffix );
    RowCombo( id, &mode, voiceModeNames, IM_ARRAYSIZE( voiceModeNames ) );
    sprintf_s( id, "Refresh folder##%s", idSuffix );
    if ( ui::Button( id ) )
        refresh();

    sprintf_s( id, "Sound##%s", idSuffix );
    if ( mode == 0 && RowBeginCombo( id, selected[ 0 ] ? selected : "No compatible WAV files" ) )
    {
        for ( int i = 0; i < clipCount; ++i )
        {
            const char* name = clipName( i );
            const bool sel = std::strcmp( selected, name ) == 0;
            ImGui::PushID( i );
            if ( ui::Selectable( name, sel ) )
                strncpy_s( selected, selectedSize, name, _TRUNCATE );
            if ( sel )
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ui::EndCombo();
    }
    else if ( mode != 0 )
    {
        char buf[ 96 ];
        sprintf_s( buf, "%s over %d sound(s)", mode == 1 ? "Random" : "In order", clipCount );
        WrappedText( buf );
    }

    char fbuf[ 512 ];
    sprintf_s( fbuf, "Folder: %s", folder );
    WrappedText( fbuf );
}

static void TabMisc()
{
    auto& m = g_config.settings.misc;

    BeginGroupBox( "Movement", 0.35f );
    ui::Checkbox( "Bunny hop", &m.bhop );
    ui::Checkbox( "Air strafe", &m.autostrafe );
    if ( m.autostrafe )
    {
        RowSliderFloat( "Strafe smoothing", &m.autostrafe_smoothing, 0.f, 100.f, "%.0f" );
        ui::Checkbox( "Avoid walls", &m.autostrafe_avoid_walls );
    }
    EndGroupBox();

    BeginGroupBox( "Camera", 0.3f );
    ui::Checkbox( "Remove flash", &m.no_flash );
    ui::Checkbox( "Remove scope overlay", &m.remove_scope_overlay );
    RowSliderFloat( "Scope field of view", &m.scope_fov, 0.f, 90.f, "%.0f" );
    ui::Checkbox( "Third person", &m.thirdperson );
    ui::Checkbox( "Third person on key", &m.thirdperson_on_key );
    if ( m.thirdperson_on_key )
        Binds::Widget( "Third person key##tp", m.thirdperson_key, m.thirdperson_key_mode );
    EndGroupBox();

    BeginGroupBox( "Scope crosshair", 0.35f );
    auto& xh = m.crosshair;
    WrappedText( "Crosshair drawn while scoped. Requires 'Remove scope overlay' in Camera." );
    RowSliderFloat( "Thickness##xh", &xh.scope_thickness, 1.f, 8.f, "%.1f" );
    RowSliderFloat( "Length##xh", &xh.scope_length, 0.f, 400.f, "%.0f" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "0 = classic full-screen lines,\n"
                           "else centered lines of this length." );
    if ( xh.scope_length > 0.f )
    {
        RowSliderFloat( "Gap##xh", &xh.scope_gap, 0.f, 50.f, "%.0f" );
        ui::Checkbox( "T style##xh", &xh.scope_t_style );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Removes the top line." );
    }
    ui::Checkbox( "Dot##xh", &xh.scope_dot );
    if ( xh.scope_dot )
        RowSliderFloat( "Dot size##xh", &xh.scope_dot_size, 1.f, 8.f, "%.1f" );
    ui::Checkbox( "Outline##xh", &xh.scope_outline );
    InlineColor( "##scopecol", xh.scope_color );
    EndGroupBox();

    BeginGroupBox( "Viewmodel", 0.35f );
    auto& vm = m.viewmodel;
    ui::Checkbox( "Enabled##vm", &vm.enabled );
    if ( vm.enabled )
    {
        // UI ranges, not convar limits (UpdateViewmodel clears the bounds).
        RowSliderFloat( "Field of view##vm", &vm.fov, -50.f, 300.f, "%.1f" );
        RowSliderFloat( "Offset X##vm", &vm.offset_x, -100.f, 100.f, "%.2f" );
        RowSliderFloat( "Offset Y##vm", &vm.offset_y, -100.f, 100.f, "%.2f" );
        RowSliderFloat( "Offset Z##vm", &vm.offset_z, -100.f, 100.f, "%.2f" );
        RowSliderFloat( "Recoil##vm", &vm.recoil, -10.f, 10.f, "%.2f" );
    }
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "World", 0.42f );
    auto& night = g_config.settings.visuals.others;
    ui::Checkbox( "Watermark", &night.watermark );
    ui::Checkbox( "Keybind list", &night.keybind_list );
    InlineColor( "##kbcol", night.keybind_color );
    if ( night.keybind_list )
        ui::Checkbox( "Keybind gradient", &night.keybind_gradient );
    ui::Checkbox( "Event logs", &night.event_logs );
    ui::Checkbox( "Logs to console", &night.event_logs_console );
    ui::Checkbox( "Bomb timer", &night.bomb_timer );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Shows planted C4 countdown panel at bottom-center.\n"
                           "Turns blue and shows defuse bar when someone is defusing." );
    ui::Checkbox( "Bullet tracers", &night.bullet_tracers );
    ui::Checkbox( "Hit marker", &night.hitmarker );
    if ( night.hitmarker )
        ui::Checkbox( "Hit marker damage", &night.hitmarker_damage );
    ui::Checkbox( "Remove smoke", &night.no_smoke );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Hides smoke grenade clouds; restored when disabled." );
    if ( night.no_smoke )
        ui::Checkbox( "Remove smoke particles", &night.no_smoke_particles );
    ui::Checkbox( "Molotov zone", &night.molotov_zone );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Ground fill + burn countdown at active fires." );
    InlineColor( "##molo", night.molotov_color );
    ui::Checkbox( "Smoke timer", &night.smoke_timer );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Ground ring + countdown at active smokes." );
    InlineColor( "##smkcol", night.smoke_timer_color );
    ui::Checkbox( "Night mode", &night.night_mode );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Darkens world and static props; restored when disabled." );
    InlineColor( "##nightcol", night.night_mode_color.r, night.night_mode_color.g, night.night_mode_color.b, nullptr );
    if ( night.night_mode )
    {
        RowSliderInt( "Night mode intensity##night", &night.night_mode_intensity, 0, 100, "%d%%" );
        WrappedText( "0% = original color; 100% = full tint" );
    }
    auto& gp = m.grenade_prediction;
    ui::Checkbox( "Grenade prediction##gp", &gp.enabled );
    if ( gp.enabled )
        ui::Checkbox( "HE damage estimate##gp", &gp.damage_indicator );
    ui::Checkbox( "Grenade warning##gp", &gp.warning );
    if ( gp.warning )
    {
        ui::Checkbox( "Grenade timers##gp", &gp.timers );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Show countdown ring on the grenade warning icon.\n"
                               "HE/Flash: 1.5s  |  Molotov: 2.0s  |  Smoke/Decoy: 3.0s" );
    }
    EndGroupBox();

    BeginGroupBox( "Voice", 0.33f );
    auto& vk = m.voice_kill;
    VoiceSection( "Voice on kill", vk.enabled, vk.mode, vk.selected, sizeof( vk.selected ), "voice_kill",
                  &VoiceKill::RefreshKillLibrary, VoiceKill::GetKillClipCount(),
                  &VoiceKill::GetKillClipName, VoiceKill::GetKillLibraryFolder() );
    VoiceSection( "Voice on death", vk.death_enabled, vk.death_mode, vk.death_selected, sizeof( vk.death_selected ), "voice_death",
                  &VoiceKill::RefreshDeathLibrary, VoiceKill::GetDeathClipCount(),
                  &VoiceKill::GetDeathClipName, VoiceKill::GetDeathLibraryFolder() );
    WrappedText( "WAV only: PCM 16-bit, mono, 11025 Hz." );
    if ( vk.enabled || vk.death_enabled )
        WrappedText( "Uses matching engine Voice_RecordStart voice path." );
    EndGroupBox();

    BeginGroupBox( "Other##misc", 0.25f );
    auto& ct = m.clantag;
    ui::Checkbox( "Clan tag spammer##ct", &ct.enabled );
    if ( ct.enabled )
    {
        char tagBuffer[ sizeof( ct.text ) ];
        strncpy_s( tagBuffer, sizeof( tagBuffer ), ct.text, _TRUNCATE );
        if ( RowInputText( "Clan tag text##clantag", tagBuffer, sizeof( tagBuffer ) ) )
            strncpy_s( ct.text, sizeof( ct.text ), tagBuffer, _TRUNCATE );
        RowSliderFloat( "Frame interval##clantag", &ct.speed_ms, 100.f, 2000.f, "%.0f ms" );
        WrappedText( "Uses at most 15 visible characters." );
    }

    auto& ks = m.killsay;
    ui::Checkbox( "Kill say##ks", &ks.enabled );
    if ( ks.enabled )
    {
        RowSliderFloat( "Cooldown##ks", &ks.cooldown_ms, 100.f, 5000.f, "%.0f ms" );
        char buf[128];
        strncpy_s( buf, ks.message, sizeof( buf ) - 1 );
        buf[ sizeof( buf ) - 1 ] = '\0';
        if ( RowInputText( "Message##ks", buf, sizeof( buf ) ) )
            strncpy_s( ks.message, buf, sizeof( ks.message ) - 1 );
    }

    if (ui::Button("Unload##unhook")) g_sdk.unhook = true;
    EndGroupBox();
}

static void TabPlayers()
{
    using PlayerList::Entry;

    static std::vector<PlayerList::OnlineRow> rows;
    static int  selected = -1;
    static char selDisplay[64] = "";
    static Entry edit{};             // working copy shown in the editor
    static bool  hasEntry = false;   // stored entry existed for this player

    rows = PlayerList::GetOnlineRows();

    // Selection is tracked by nickname and re-resolved every frame
    // (entity indices shift when players join/leave).
    if ( selDisplay[0] )
    {
        int match = -1;
        for ( int i = 0; i < (int)rows.size(); ++i )
            if ( rows[i].name == selDisplay ) { match = i; break; }
        selected = match;
        if ( match < 0 )
        {
            selDisplay[0] = '\0';
            edit     = Entry{};
            hasEntry = false;
        }
    }
    else
    {
        selected = -1;
    }

    BeginGroupBox( "Players", 1.f );
    if ( rows.empty() )
    {
        WrappedText( "No players detected - connect to a server." );
    }
    else
    {
        if ( ui::BeginList( "##playerrows", 260.f ) )
        {
            for ( int i = 0; i < (int)rows.size(); ++i )
            {
                char label[112];
                sprintf_s( label, "[%d] %s", rows[i].index, rows[i].name.c_str() );

                // Throwaway probe: never read rows into `edit`.
                Entry probe;
                if ( PlayerList::TryGetCopy( rows[i].name, probe ) )
                    strcat_s( label, " *" );

                ImGui::PushID( i );
                if ( ui::Selectable( label, i == selected ) )
                {
                    selected = i;
                    strncpy_s( selDisplay, rows[i].name.c_str(), sizeof( selDisplay ) - 1 );
                    selDisplay[ sizeof( selDisplay ) - 1 ] = '\0';
                    edit     = Entry{};
                    hasEntry = PlayerList::TryGetCopy( rows[i].name, edit );
                }
                ImGui::PopID();
            }
        }
        ui::EndList();
        WrappedText( "* = saved override" );
    }
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "Adjustments", 1.f );
    if ( selected >= 0 && selected < (int)rows.size() && selDisplay[0] )
    {
        const char* key = selDisplay;   // commit key = display nickname

        ui::Text( "%s%s", selDisplay, hasEntry ? "" : "  (default)" );

        if ( ui::Checkbox( "Aim target", &edit.aim ) )
            PlayerList::Commit( key, edit );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Aimbot / rage / triggerbot may target this player." );

        if ( ui::Checkbox( "Priority target", &edit.prefer ) )
            PlayerList::Commit( key, edit );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Preferred players win target selection." );

        if ( ui::Checkbox( "Visuals", &edit.visuals ) )
            PlayerList::Commit( key, edit );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "ESP and chams are drawn for this player." );

        if ( ui::Checkbox( "Highlight box", &edit.highlight ) )
            PlayerList::Commit( key, edit );
        if ( InlineColor( "##hlcol", edit.hl_r, edit.hl_g, edit.hl_b, &edit.hl_a ) )
            PlayerList::Commit( key, edit );

        if ( ui::Button( "Reset selected" ) )
        {
            PlayerList::Remove( key );
            edit     = Entry{};
            hasEntry = false;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Delete the saved entry for this player." );
        if ( ui::Button( "Clear all saved" ) )
        {
            PlayerList::ClearAll();
            edit     = Entry{};
            hasEntry = false;
        }
    }
    else
    {
        WrappedText( "Select a player on the left to edit their overrides." );
    }
    EndGroupBox();
}

static void TabConfig()
{
    static char nameBuf[64] = "";
    static bool deleteArmed = false, resetArmed = false;

    BeginGroupBox( "Presets", 1.f );
    {
        auto& list = g_config.settings.config_list;
        if ( ui::BeginList( "##configs", 200.f ) )
        {
            for ( int i = 0; i < (int)list.size(); ++i )
            {
                ImGui::PushID( i );
                if ( ui::Selectable( list[ i ].c_str(), g_config.settings.config_id == i ) )
                {
                    g_config.settings.config_id = i;
                    deleteArmed = false;
                }
                ImGui::PopID();
            }
        }
        ui::EndList();
    }

    const int  id    = g_config.settings.config_id;
    const bool valid = id >= 0 && id < (int)g_config.settings.config_list.size();

    if ( ui::Button( "Load##cfg" ) && valid )
        g_config.load( g_config.settings.config_list[ id ] );

    if ( ui::Button( "Save##cfg" ) && valid )
        g_config.save( g_config.settings.config_list[ id ] );

    if ( ui::Button( deleteArmed ? "Sure?##cfgdel" : "Delete##cfgdel" ) )
    {
        if ( deleteArmed )
        {
            if ( valid )
                g_config.remove( g_config.settings.config_list[ id ] );
            deleteArmed = false;
        }
        else
        {
            deleteArmed = true;
        }
    }
    else if ( deleteArmed && ImGui::IsKeyPressed( ImGuiKey_Escape ) )
    {
        deleteArmed = false;
    }

    if (ui::Button(resetArmed ? "Confirm reset##cfgreset" : "Reset config##cfgreset"))
    {
        if (resetArmed) { ResetConfiguration(); resetArmed = false; deleteArmed = false; }
        else resetArmed = true;
    }
    if (resetArmed && ImGui::IsKeyPressed(ImGuiKey_Escape)) resetArmed = false;

    if ( ui::Button( "Refresh##cfg" ) )
        g_config.refresh();
    EndGroupBox();

    // columns are chosen automatically (see BeginGroupBox)

    BeginGroupBox( "Create", 0.4f );
    WrappedText( "Type a name and save the current settings under it." );
    ui::InputText( "Config name##cfgname", nameBuf, 64, ImGuiInputTextFlags_CharsNoBlank, "config name" );
    if ( ui::Button( "Save as new" ) && nameBuf[0] )
    {
        g_config.save( nameBuf );
        g_config.refresh();
        auto& list = g_config.settings.config_list;
        const std::string saved = nameBuf;
        for ( int i = 0; i < (int)list.size(); ++i )
            if ( list[i] == saved )
            {
                g_config.settings.config_id = i;
                break;
            }
        nameBuf[0] = '\0';
    }
    EndGroupBox();

    BeginGroupBox( "Menu", 0.6f );
    {
        MenuKeyWidget();
        static const char* kGammaModes[] = { "Auto", "Normal", "Gamma fix" };
        ui::Combo( "Menu colors", &g_config.settings.misc.menu_gamma, kGammaModes, IM_ARRAYSIZE( kGammaModes ) );
        WrappedText( "If the menu looks washed out / too light, pick Gamma fix. If it looks too dark, pick Normal." );
        if ( ui::Button( "Reset layout" ) )
            ResetMenuLayout();
        WrappedText( "Drag a groupbox by its title to move it; boxes snap into place." );
    }
    EndGroupBox();
}

// ==================================================================
//  Frame (window layout from the original main.cpp)
// ==================================================================

// 8x8 tile of the skeet background (kBackground repeats it exactly).
static const unsigned char kBgTile[8][8] = {
    { 13, 21, 13, 21, 13, 21, 13, 21 },
    { 21, 21, 14, 21, 21, 21, 14, 21 },
    { 13, 21, 13, 21, 13, 21, 13, 21 },
    { 13, 21, 21, 21, 13, 21, 21, 21 },
    { 14, 21, 14, 21, 14, 21, 14, 21 },
    { 21, 21, 14, 21, 21, 21, 14, 21 },
    { 14, 21, 14, 21, 14, 21, 14, 21 },
    { 14, 21, 21, 21, 14, 21, 21, 21 },
};

static IDirect3DTexture9* s_background = nullptr;
static bool s_backgroundTried = false;

void Menu::InvalidateDeviceObjects()
{
    if (s_background) s_background->Release();
    s_background=nullptr;
    s_backgroundTried=false;
}

static IDirect3DTexture9* CreatePatternTexture( IDirect3DDevice9* pDevice )
{
    constexpr UINT W = 648, H = 548;
    IDirect3DTexture9* tex = nullptr;
    // MANAGED works on plain devices; Ex devices need a lockable DEFAULT texture.
    if ( FAILED( pDevice->CreateTexture( W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr ) ) )
    {
        tex = nullptr;
        if ( FAILED( pDevice->CreateTexture( W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) ) )
            return nullptr;
    }
    D3DLOCKED_RECT lr{};
    if ( FAILED( tex->LockRect( 0, &lr, nullptr, 0 ) ) )
    {
        tex->Release();
        return nullptr;
    }
    for ( UINT y = 0; y < H; ++y )
    {
        DWORD* row = reinterpret_cast<DWORD*>( static_cast<unsigned char*>( lr.pBits ) + y * lr.Pitch );
        for ( UINT x = 0; x < W; ++x )
        {
            const DWORD g = kBgTile[ y & 7 ][ x & 7 ];
            row[ x ] = 0xFF000000u | ( g << 16 ) | ( g << 8 ) | g;
        }
    }
    tex->UnlockRect( 0 );
    return tex;
}

static int s_tab = 0;

static ImVec2 g_appliedMenuSize(-1.f,-1.f);
static void RequestMenuGeometry(const ImVec2& initial)
{
    ImGuiIO& io=ImGui::GetIO();
    auto& geometry=g_config.settings.misc;
    ImVec2 wanted(geometry.menu_width,geometry.menu_height);
    if(!std::isfinite(wanted.x))wanted.x=initial.x;
    if(!std::isfinite(wanted.y))wanted.y=initial.y;
    wanted.x=ImClamp(wanted.x,500.f,ImMax(500.f,io.DisplaySize.x-20.f));
    wanted.y=ImClamp(wanted.y,586.f,ImMax(586.f,io.DisplaySize.y-20.f));
    if(geometry.menu_width!=g_appliedMenuSize.x || geometry.menu_height!=g_appliedMenuSize.y)
        ImGui::SetNextWindowSize(wanted,ImGuiCond_Always);
    ImGui::SetNextWindowPos((io.DisplaySize-wanted)*0.5f,ImGuiCond_Once);
}

static void CaptureMenuGeometry(const ImVec2& size)
{
    g_config.settings.misc.menu_width=size.x;
    g_config.settings.misc.menu_height=size.y;
    g_appliedMenuSize=size;
}

void Menu::Render( IDirect3DDevice9* pDevice )
{
    PollMenuKeys();

    HandleMenuToggle();

    if ( !bOpen )
        return;

    ui::ApplyStyle();

    // Background pattern, created like the original
    // (D3DXCreateTextureFromFileInMemoryEx 1920x1080, managed pool).
    if ( !s_background && !s_backgroundTried && pDevice )
    {
        s_backgroundTried = true;
        // If the game device is IDirect3DDevice9Ex, D3DPOOL_MANAGED is
        // invalid and the call fails -> no background, the window shows
        // the flat 34,34,34 WindowBg and the whole menu looks lighter than
        // the original. MANAGED only fails on Ex devices, and Ex keeps
        // D3DPOOL_DEFAULT textures across ResetEx, so fall back to that pool.
        if ( FAILED( D3DXCreateTextureFromFileInMemoryEx( pDevice, SkeetAssets::kBackground, sizeof( SkeetAssets::kBackground ),
                1920, 1080, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT,
                0, NULL, NULL, &s_background ) ) )
        {
            s_background = nullptr;
            if ( FAILED( D3DXCreateTextureFromFileInMemoryEx( pDevice, SkeetAssets::kBackground, sizeof( SkeetAssets::kBackground ),
                    1920, 1080, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT,
                    0, NULL, NULL, &s_background ) ) )
                s_background = nullptr;
        }
        // No D3DX (missing d3dx9_43.dll / failed decode): build the same
        // image by hand. The 648x548 pattern is an exact 8x8 tile repeat.
        if ( !s_background )
            s_background = CreatePatternTexture( pDevice );
    }

    // Icon font glyphs: C rage, B anti-aim, D legit, E visuals,
    // G models, F misc, H players; "" = drawn config icon.
    static const char* kPages[] = { "C", "B", "D", "E", "G", "F", "H", "" };
    constexpr int kPageCount = IM_ARRAYSIZE( kPages );

    const ImColor fade_colors[] = { ImColor( 12, 12, 12 ), ImColor( 55, 177, 218 ), ImColor( 201, 84, 192 ), ImColor( 204, 227, 54 ) };

    ImGui::PushFont( ui::f_normal, 12.f );

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 kInitSize( 720.f, 42.f + 68.f * kPageCount + 20.f );
    RequestMenuGeometry(kInitSize);

    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowMinSize, ImVec2( 500, 42.f + ( 68.f * kPageCount ) ) );

    ImGui::Begin( "##csso_menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings );
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImDrawList*  draw   = window->DrawList;
        const ImVec2 pos    = window->Pos;
        const ImVec2 size   = window->Size;
        // Capture BEFORE config buttons: a load below must survive to next frame.
        CaptureMenuGeometry(size);
        const float  tabsH  = 68.f * kPageCount;
        const ImU32  shade  = ImColor( 0.f, 0.f, 0.f, 0.43f );

        draw->AddRect( pos + ImVec2( 1, 1 ), pos + size - ImVec2( 1, 1 ), ImColor( 1.f, 1.f, 1.f, 0.1f ) );
        draw->AddRectFilled( pos + ImVec2( 2, 2 ), pos + size - ImVec2( 2, 2 ), ImGui::GetColorU32( ImGuiCol_WindowBg ) );

        if ( s_background )
            draw->AddImage( (ImTextureID)(intptr_t)s_background, pos + ImVec2( 5, 5 ), pos + size - ImVec2( 5, 5 ) );
        else
            draw->AddRectFilled( pos + ImVec2( 5, 5 ), pos + size - ImVec2( 5, 5 ), ImColor( 18, 18, 18 ) ); // average of the pattern
        draw->AddRect( pos + ImVec2( 5, 5 ), pos + size - ImVec2( 5, 5 ), ImColor( 1.f, 1.f, 1.f, 0.1f ) );

        draw->AddRectFilled( pos + ImVec2( 6, 6 ), pos + ImVec2( 75, 21 ), ImColor( 12, 12, 12 ) );
        draw->AddRectFilled( pos + ImVec2( 6, 21 + tabsH ), pos + ImVec2( 75, size.y - 6 ), ImColor( 12, 12, 12 ) );
        draw->AddLine( pos + ImVec2( 75, 8 ), pos + ImVec2( 75, 21 ), ImColor( 1.f, 1.f, 1.f, 0.05f ) );
        draw->AddLine( pos + ImVec2( 74, 8 ), pos + ImVec2( 74, 21 ), ImGui::GetColorU32( ImGuiCol_Border ) );
        draw->AddLine( pos + ImVec2( 75, 21 + tabsH ), pos + ImVec2( 75, size.y - 6 ), ImColor( 1.f, 1.f, 1.f, 0.05f ) );
        draw->AddLine( pos + ImVec2( 74, 21 + tabsH ), pos + ImVec2( 74, size.y - 6 ), ImGui::GetColorU32( ImGuiCol_Border ) );

        draw->AddRectFilledMultiColor( ImVec2( pos.x + 6, pos.y + 7 ), ImVec2( pos.x + size.x / 2 - 6, pos.y + 9 ), fade_colors[ 0 ], fade_colors[ 0 ], fade_colors[ 0 ], fade_colors[ 0 ] );
        draw->AddRectFilledMultiColor( ImVec2( pos.x + 6 + size.x / 2, pos.y + 7 ), ImVec2( pos.x + size.x - 7, pos.y + 9 ), fade_colors[ 0 ], fade_colors[ 0 ], fade_colors[ 0 ], fade_colors[ 0 ] );

        draw->AddRectFilledMultiColor( ImVec2( pos.x + 7, pos.y + 8 ), ImVec2( pos.x + size.x / 2 + 6, pos.y + 10 ), fade_colors[ 1 ], fade_colors[ 2 ], fade_colors[ 2 ], fade_colors[ 1 ] );
        draw->AddRectFilledMultiColor( ImVec2( pos.x + size.x / 2 + 6, pos.y + 8 ), ImVec2( pos.x + size.x - 7, pos.y + 10 ), fade_colors[ 2 ], fade_colors[ 3 ], fade_colors[ 3 ], fade_colors[ 2 ] );

        draw->AddRectFilledMultiColor( ImVec2( pos.x + 7, pos.y + 9 ), ImVec2( pos.x + size.x / 2 + 6, pos.y + 11 ), shade, shade, shade, shade );
        draw->AddRectFilledMultiColor( ImVec2( pos.x + size.x / 2 + 6, pos.y + 9 ), ImVec2( pos.x + size.x - 7, pos.y + 11 ), shade, shade, shade, shade );

        ImGui::SetCursorPos( ImVec2( 6, 21 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
        ImGui::BeginGroup();

        for ( int i = 0; i < kPageCount; ++i )
            if ( ui::Tab( kPages[ i ], s_tab == i ) )
                s_tab = i;

        ImGui::EndGroup();
        ImGui::PopStyleVar();

        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10, 10 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 8 ) );

        ImGui::SetCursorPos( ImVec2( 90, 25 - ImGui::GetStyle().WindowPadding.y ) );
        ImGui::BeginChild( "##Groups", ImVec2( size.x - 105, size.y - 40 + ImGui::GetStyle().WindowPadding.y ),
                           ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar );

        if (s_tab != kPageCount - 1) g_menuKeyCapture = false;
        LayoutBegin( s_tab );
        switch ( s_tab )
        {
        case 0: TabRage();       break;
        case 1: TabAntiAim();    break;
        case 2: TabTriggerbot(); break;
        case 3: TabVisuals();    break;
        case 4: TabChams();      break;
        case 5: TabMisc();       break;
        case 6: TabPlayers();    break;
        default: TabConfig();    break;
        }
        LayoutEnd();

        ImGui::EndChild();

        ImGui::PopStyleVar( 2 );
    }
    ImGui::End();

    ImGui::PopStyleVar( 2 );
    ImGui::PopFont();
}
