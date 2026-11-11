#pragma once

// ============================================================
//  D3DHook.hpp — DirectX 9 EndScene/Present/Reset hooks
//               + ImGui lifecycle management
// ============================================================

#include <Windows.h>

extern HWND g_hGameWnd;

namespace D3D
{
    // Bootstraps EndScene/Present on a temporary HAL device (NULLREF fallback).
    bool AttachHooks( HWND hWnd = nullptr );

    // Returns the game window handle (available after first frame).
    HWND GetGameWindow();

    // Restores the original game WndProc and stops the Win32 ImGui backend
    // before the shared ImGui context is destroyed during DLL unload.
    void Shutdown();
}
