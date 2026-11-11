#pragma once

// ============================================================
//  GlobalVars.hpp — process-wide singletons
// ============================================================

#include "platform/windows/Platform.hpp"

extern IDirect3DDevice9* g_pD3Device;
extern bool              g_bUnloading;
extern HMODULE           g_hModule;
