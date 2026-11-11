#pragma once

// ============================================================
//  Utils.hpp — pattern-scan and misc helpers
// ============================================================

#include "platform/windows/Platform.hpp"
#include "utils/Math.hpp"

namespace Utils
{
    // Scans a loaded module for a byte pattern.
    // szPattern — raw bytes, szMask — 'x' for match, '?' for wildcard.
    uint8_t* FindPattern( HMODULE hHandle, const char* szPattern, const char* szMask );
}
