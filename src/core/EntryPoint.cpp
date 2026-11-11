#include "platform/windows/Platform.hpp"
#include "core/GlobalVars.hpp"
#include "core/hooks/D3DHook.hpp"
#include "core/hooks/CreateMoveHook.hpp"
#include "sdk/SDK.hpp"
#include "sdk/Interfaces.hpp"
#include "core/runtime/FeatureDispatcher.hpp"
#include "config/Config.hpp"
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>

// ============================================================
//  EntryPoint.cpp — DLL entry point, main and unload threads
// ============================================================

DWORD WINAPI MainThread( PVOID pModule )
{
    if ( MH_Initialize() != MH_OK )
        return 0ul;

    // Release builds stay silent by design: no console window is created and
    // every diagnostic is compiled out through DBGLOG (see utils/Common.hpp).
    // Define CSSO_DEBUG_LOG=1 to bring the console and all logs back.
#if CSSO_DEBUG_LOG
    AllocConsole();

    FILE* fDummy;
    freopen_s( &fDummy, "conin$",  "r", stdin  );
    freopen_s( &fDummy, "conout$", "w", stdout );
    freopen_s( &fDummy, "conout$", "w", stderr );
    setvbuf( stdout, nullptr, _IONBF, 0 );
    setvbuf( stderr, nullptr, _IONBF, 0 );

    SetConsoleTitleA( "CSSO" );

    DBGLOG( "loaded\n" );
#endif

    if ( !D3D::AttachHooks() )
        return 0ul;

    SDK::FindOffsets();
    Hooks::Init();

    return 1ul;
}

DWORD WINAPI UnloadThread( PVOID pModule )
{
    while ( !GetAsyncKeyState( VK_END ) && !g_sdk.unhook )
        std::this_thread::sleep_for( 1s );

    std::this_thread::sleep_for( 1s );

    g_bUnloading = true;

    // Restore the game window procedure before the ImGui context disappears.
    // This prevents the window from retaining a callback inside our DLL.
    D3D::Shutdown();
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext();

    Hooks::Shutdown();

    MH_DisableHook( MH_ALL_HOOKS );
    MH_RemoveHook ( MH_ALL_HOOKS );
    MH_Uninitialize();

#if CSSO_DEBUG_LOG
    FreeConsole();
    fclose( stdin  );
    fclose( stdout );
    fclose( stderr );
#endif

    Beep( 200, 200 );
    FreeLibraryAndExitThread( (HMODULE)pModule, 1ul );
    return 1ul;
}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD dwReason, LPVOID )
{
    if ( dwReason == DLL_PROCESS_ATTACH )
    {
        g_hModule = hModule;
        Beep( 200, 200 );
        DisableThreadLibraryCalls( hModule );

        HANDLE hMain = CreateThread( nullptr, 0, MainThread,   hModule, 0, nullptr );
        if ( hMain ) CloseHandle( hMain );

        HANDLE hUnload = CreateThread( nullptr, 0, UnloadThread, hModule, 0, nullptr );
        if ( hUnload ) CloseHandle( hUnload );
    }
    return TRUE;
}
