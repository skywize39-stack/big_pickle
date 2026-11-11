#pragma once
#include <Windows.h>

// ============================================================
//  Interfaces.hpp — engine interface initialisation
// ============================================================

namespace SDK
{
    // Scans engine.dll for the ScreenTransform / InternalW2S functions.
    void ScanScreenTransform( HMODULE hEngine );

    // Resolves all engine interfaces and KeyValues function pointers.
    void FindOffsets();
}
