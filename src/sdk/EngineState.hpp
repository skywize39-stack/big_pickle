#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/EngineInterfaces.hpp"
#include "sdk/Models.hpp"
#include "sdk/Trace.hpp"

namespace SDK
{
    // ------------------------------------------------------------------
    //  Global interface pointers (defined in Interfaces.cpp)
    // ------------------------------------------------------------------
    inline IVEngineClient*    g_pEngine          = nullptr;
    inline IGameEventManager2* g_pGameEventManager = nullptr;
    inline IBaseClientDLL*    g_pClient          = nullptr;
    inline IClientEntityList* g_pEntityList      = nullptr;
    inline IVModelRender*     g_pModelRender     = nullptr;
    inline IVModelInfoClient* g_pModelInfo       = nullptr;
    inline IMaterialSystem*   g_pMaterialSystem  = nullptr;
    inline IStudioRender*     g_pStudioRender    = nullptr;
    inline IEngineTrace*      g_pEngineTrace     = nullptr;
    inline uintptr_t          g_uEngineBase      = 0;
    inline uintptr_t          g_uClientBase      = 0;
    inline uintptr_t          g_uMaterialSystemBase = 0;

    inline ScreenTransformFn  g_ScreenTransform  = nullptr;
    inline InternalW2SFn      g_InternalW2S      = nullptr;

} // namespace SDK
