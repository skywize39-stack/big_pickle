#pragma once

// ==================================================================
//  Skeet / gamesense style widgets.
//
//  1:1 port of the custom widgets from the supplied "skeet" ImGui
//  sources (custom.cpp + the modified imgui_widgets.cpp of 1.89.9),
//  rewritten as a separate namespace on top of our stock ImGui 1.92
//  so the library itself stays untouched. Sizes, offsets and colors
//  are copied from the original code.
// ==================================================================

#include <imgui.h>

struct ImFont;
struct IDirect3DDevice9;

namespace ui
{
    // Fonts, created in D3DHook during ImGui init (verdana 12, verdana
    // bold 12, icon font 37). Null on failure -> current font is used.
    inline ImFont* f_normal = nullptr;
    inline ImFont* f_bold   = nullptr;
    inline ImFont* f_icons  = nullptr;

    void  LoadFonts();                   // call between CreateContext and first NewFrame
    void  ApplyStyle();                  // skeet colors / metrics
    ImU32 Accent( float a = 1.f );       // 141,188,0

    // Window chrome
    bool  Tab( const char* icon, bool cur );   // 68x68 icon tab; icon = "" draws config icon
    void  BeginGroupBox( const char* name, const ImVec2& size, bool resizing = false );
    void  EndGroupBox();
    // Group box height helper: fraction of the "##Groups" area height.
    float LastGroupBoxContentHeight(); // measured by the last EndGroupBox
    float BoxH( float frac );
    float BoxW();
    void  NextColumn();

    // Widgets (label handling: text after "##" is hidden, as in ImGui)
    bool  Checkbox( const char* label, bool* v, bool special = false );
    bool  SliderInt( const char* label, int* v, int mn, int mx, const char* fmt = "%d" );
    bool  SliderFloat( const char* label, float* v, float mn, float mx, const char* fmt = "%.1f" );
    bool  BeginCombo( const char* label, const char* preview, ImGuiComboFlags flags = 0 );
    void  EndCombo();
    bool  Combo( const char* label, int* v, const char* const items[], int count );
    bool  MultiCombo( const char* label, bool combos[], const char* const items[], int count );
    bool  Selectable( const char* label, bool selected, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2( 0, 0 ) );
    bool  Button( const char* label, const ImVec2& size = ImVec2( 0, 0 ) );   // 0 = full row width x 25
    bool  InputText( const char* label, char* buf, size_t size, ImGuiInputTextFlags flags = 0, const char* hint = nullptr );
    // 20x10 color swatch + skeet picker popup. Place after SameLine( GetWindowWidth() - 54 ).
    bool  ColorEdit( const char* id, float col[4], bool alpha );

    // Text, shifted by 20 px like the skeet TextEx patch.
    void  Text( const char* fmt, ... );
    void  TextColored( const ImVec4& col, const char* fmt, ... );
    void  TextDisabledWrapped( const char* text );

    // Framed list area (skeet-like) for listboxes.
    bool  BeginList( const char* id, float height );
    void  EndList();
}
