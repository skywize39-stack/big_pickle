#include "features/visuals/chams/Chams.hpp"
#include "features/visuals/chams/ChamsHook.hpp"
#include "features/visuals/chams/ChamsMaterials.hpp"
#include "sdk/SDK.hpp"

// ============================================================
//  Chams.cpp — vtable hook install / remove lifecycle
// ============================================================

#define CHAMS_LOG(fmt, ...) ((void)0)

constexpr int DRAW_MODEL_EXECUTE_INDEX = 20;

using DrawModelExecuteFn = void(__fastcall*)(void*, void*, void*, void*);

// Declared in ChamsHook.cpp
extern DrawModelExecuteFn g_origDrawModelExecute;
extern bool               g_hookActive;

void Chams::Init()
{
    CHAMS_LOG( "=== Init ===" );

    if ( !SDK::g_pModelRender )
    {
        CHAMS_LOG( "FAIL: g_pModelRender is null" );
        return;
    }

    void** vtable = nullptr;
    __try
    {
        vtable = *reinterpret_cast<void***>( SDK::g_pModelRender );
    }
    __except ( EXCEPTION_EXECUTE_HANDLER )
    {
        CHAMS_LOG( "CRASH reading vtable: code=0x%08X", GetExceptionCode() );
        return;
    }

    CHAMS_LOG( "VModelRender vtable=%p", vtable );
    CHAMS_LOG( "Hooking vtable[%d] = %p (DrawModelExecute)",
               DRAW_MODEL_EXECUTE_INDEX, vtable[DRAW_MODEL_EXECUTE_INDEX] );

    DWORD oldProtect = 0;
    if ( !VirtualProtect( &vtable[DRAW_MODEL_EXECUTE_INDEX], sizeof(void*),
                          PAGE_READWRITE, &oldProtect ) )
    {
        CHAMS_LOG( "FAIL: VirtualProtect failed: %d", GetLastError() );
        return;
    }

    g_origDrawModelExecute = reinterpret_cast<DrawModelExecuteFn>( vtable[DRAW_MODEL_EXECUTE_INDEX] );
    vtable[DRAW_MODEL_EXECUTE_INDEX] = reinterpret_cast<void*>( &hkDrawModelExecute );

    VirtualProtect( &vtable[DRAW_MODEL_EXECUTE_INDEX], sizeof(void*), oldProtect, &oldProtect );

    g_hookActive = true;
    CHAMS_LOG( "Hook installed: vtable[%d] hooked, original=%p",
               DRAW_MODEL_EXECUTE_INDEX, g_origDrawModelExecute );
    CHAMS_LOG( "g_pStudioRender = %p", SDK::g_pStudioRender );
    CHAMS_LOG( "=== Init done ===" );
}

void Chams::Shutdown()
{
    CHAMS_LOG( "Shutdown" );

    if ( g_hookActive && SDK::g_pModelRender )
    {
        __try
        {
            void** vtable = *reinterpret_cast<void***>( SDK::g_pModelRender );
            DWORD oldProtect = 0;
            VirtualProtect( &vtable[DRAW_MODEL_EXECUTE_INDEX], sizeof(void*),
                            PAGE_READWRITE, &oldProtect );
            vtable[DRAW_MODEL_EXECUTE_INDEX] = reinterpret_cast<void*>( g_origDrawModelExecute );
            VirtualProtect( &vtable[DRAW_MODEL_EXECUTE_INDEX], sizeof(void*),
                            oldProtect, &oldProtect );
            CHAMS_LOG( "Hook removed from vtable[%d]", DRAW_MODEL_EXECUTE_INDEX );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            CHAMS_LOG( "CRASH during shutdown: code=0x%08X", GetExceptionCode() );
        }
        g_hookActive = false;
    }

    g_origDrawModelExecute = nullptr;

    ChamsMaterials::Reset();
}
