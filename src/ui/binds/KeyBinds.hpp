#pragma once

// ------------------------------------------------------------------
//  KeyBinds — single bind system for every hotkey (skeet-style).
//  Modes: 0 Hold, 1 Toggle, 2 Always on, 3 Off hotkey (active unless held).
//  Every feature calls Binds::Active() once per tick with a stable id;
//  the state machine (toggle latch, edge detection) lives here, and the
//  active binds are listed in the "keybinds" overlay.
// ------------------------------------------------------------------
namespace Binds
{
    enum Mode { kHold = 0, kToggle = 1, kAlways = 2, kOffHotkey = 3 };

    // Resolves the bind state. `id` must be a string literal (stable ptr).
    // `display` is the name shown in the keybind list (nullptr = hidden).
    bool Active( const char* id, int key, int mode, const char* display = nullptr );

    // Raw "is this VK physically held" (0 / out of range -> false).
    bool KeyDown( int vk );
    bool IsCapturing();
    void Reset();

    const char* KeyName( int vk );
    const char* ModeName( int mode );

    // ImGui widget: [KEY] button, LMB = capture (Esc clears, Backspace unbinds),
    // RMB = mode popup. Returns true when key or mode changed.
    bool Widget( const char* label, int& key, int& mode );

    // Skeet-style draggable "keybinds" list (called from render thread).
    void RenderList();
}
