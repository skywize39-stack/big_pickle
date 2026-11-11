#include "sdk/Interfaces.hpp"
#include "sdk/SDK.hpp"

// ============================================================
//  Interfaces.cpp — engine interface resolution
// ============================================================

namespace SDK
{

void ScanScreenTransform( HMODULE hEngine )
{
    // Byte pattern for the ScreenTransform function body
    const uint8_t pattern[] = {
        0xF3, 0x0F, 0x10, 0x02,
        0xF3, 0x41, 0x0F, 0x59, 0x00,
        0xF3, 0x0F, 0x10, 0x4A, 0x08,
        0xF3, 0x41, 0x0F, 0x59, 0x48, 0x08,
        0xF3, 0x0F, 0x10, 0x52, 0x04,
        0xF3, 0x41, 0x0F, 0x59, 0x50, 0x04,
    };

    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>( hEngine );
    auto* pNt  = reinterpret_cast<IMAGE_NT_HEADERS*>( (uintptr_t)hEngine + pDos->e_lfanew );
    auto* pSec = IMAGE_FIRST_SECTION( pNt );

    for ( int i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSec )
    {
        char szName[9] = {};
        memcpy( szName, pSec->Name, 8 );
        if ( strcmp( szName, ".text" ) != 0 )
            continue;

        auto* pStart = reinterpret_cast<uint8_t*>( hEngine ) + pSec->VirtualAddress;
        auto* pEnd   = pStart + pSec->Misc.VirtualSize;

        for ( auto* p = pStart; p < pEnd - sizeof( pattern ); ++p )
        {
            if ( memcmp( p, pattern, sizeof( pattern ) ) == 0 )
            {
                g_ScreenTransform = reinterpret_cast<ScreenTransformFn>( p );
                g_InternalW2S     = reinterpret_cast<InternalW2SFn>( (uint8_t*)p - 0xF0 );
                // printf_s( "[SDK] ScreenTransform at %p\n", g_ScreenTransform );
                // printf_s( "[SDK] InternalW2S     at %p\n", g_InternalW2S );
                return;
            }
        }
    }
    // printf_s( "[SDK] FAIL: ScreenTransform not found!\n" );
}

void FindOffsets()
{
    HMODULE hEngine         = GetModuleHandleA( "engine.dll" );
    HMODULE hClient         = GetModuleHandleA( "client.dll" );
    HMODULE hMaterialSystem = GetModuleHandleA( "materialsystem.dll" );
    HMODULE hStudioRender   = GetModuleHandleA( "studiorender.dll" );
    if ( !hStudioRender )
        hStudioRender = LoadLibraryA( "studiorender.dll" );

    g_uEngineBase  = (uintptr_t)hEngine;
    g_uClientBase  = (uintptr_t)hClient;

    // printf_s( "[SDK] engine.dll          = %p\n", hEngine );
    // printf_s( "[SDK] client.dll          = %p\n", hClient );
    // printf_s( "[SDK] materialsystem.dll  = %p\n", hMaterialSystem );
    // printf_s( "[SDK] studiorender.dll    = %p\n", hStudioRender );

    // if ( !hEngine )         printf_s( "[SDK] FAIL: engine.dll not loaded\n" );
    // if ( !hClient )         printf_s( "[SDK] FAIL: client.dll not loaded\n" );
    // if ( !hMaterialSystem ) printf_s( "[SDK] FAIL: materialsystem.dll not loaded\n" );
    // if ( !hStudioRender )   printf_s( "[SDK] FAIL: studiorender.dll not loaded\n" );

    g_pEngine       = reinterpret_cast<IVEngineClient*>   ( GetInterface( hEngine,  "VEngineClient016" ) );
    g_pGameEventManager = reinterpret_cast<IGameEventManager2*>( GetInterface( hEngine,  "GameEventManager003" ) );
    g_pEntityList   = reinterpret_cast<IClientEntityList*>( GetInterface( hClient,  "ClientEntityList004" ) );
    g_pClient       = reinterpret_cast<IBaseClientDLL*>   ( GetInterface( hClient,  "BaseClientDLL023" ) );
    g_pModelRender  = reinterpret_cast<IVModelRender*>    ( GetInterface( hEngine,  "VModelRender017" ) );

    // Try multiple VModelInfoClient versions
    const char* modelInfoVersions[] = {
        "VModelInfoClient008", "VModelInfoClient006", "VModelInfoClient005",
        "VModelInfoClient004", "VModelInfoClient003", "VModelInfoClient002",
        "VModelInfoClient001", "VModelInfoClient000"
    };
    for ( const char* ver : modelInfoVersions )
    {
        g_pModelInfo = reinterpret_cast<IVModelInfoClient*>( GetInterface( hEngine, ver ) );
        if ( g_pModelInfo ) break;
    }
    // if ( !g_pModelInfo )
    //     printf_s( "[SDK] FAIL: All VModelInfoClient versions failed\n" );

    g_pMaterialSystem = reinterpret_cast<IMaterialSystem*>( GetInterface( hMaterialSystem, "MaterialSystem084" ) );
    g_pStudioRender   = reinterpret_cast<IStudioRender*>  ( GetInterface( hStudioRender,   "StudioRender026" ) );

    // printf_s( "[SDK] IVEngineClient:    %p %s\n", g_pEngine,        g_pEngine        ? "" : "(FAIL)" );
    // printf_s( "[SDK] IBaseClientDLL:    %p %s\n", g_pClient,        g_pClient        ? "" : "(FAIL)" );
    // printf_s( "[SDK] IClientEntityList: %p %s\n", g_pEntityList,    g_pEntityList    ? "" : "(FAIL)" );
    // printf_s( "[SDK] IVModelRender:     %p %s\n", g_pModelRender,   g_pModelRender   ? "" : "(FAIL)" );
    // printf_s( "[SDK] IVModelInfoClient: %p %s\n", g_pModelInfo,     g_pModelInfo     ? "" : "(FAIL)" );
    // printf_s( "[SDK] IMaterialSystem:   %p %s\n", g_pMaterialSystem,g_pMaterialSystem? "" : "(FAIL)" );
    // printf_s( "[SDK] IStudioRender:     %p %s\n", g_pStudioRender,  g_pStudioRender  ? "" : "(FAIL)" );

    // IEngineTrace — resolved via global ptr in engine.dll
    g_pEngineTrace = IEngineTrace::Get();
    // printf_s( "[SDK] IEngineTrace:     %p %s\n", g_pEngineTrace, g_pEngineTrace ? "" : "(FAIL)" );

    // KeyValues function pointers from materialsystem.dll
    if ( hMaterialSystem )
    {
        g_uMaterialSystemBase = (uintptr_t)hMaterialSystem;
        fnKeyValues_Allocate  = reinterpret_cast<KeyValues_AllocateFn> ( g_uMaterialSystemBase + 0x111D50 );
        fnKeyValues_SetName   = reinterpret_cast<KeyValues_SetNameFn>  ( g_uMaterialSystemBase + 0x111D00 );
        fnKeyValues_SetInt    = reinterpret_cast<KeyValues_SetIntFn>   ( g_uMaterialSystemBase + 0x114F30 );
        fnKeyValues_SetString = reinterpret_cast<KeyValues_SetStringFn>( g_uMaterialSystemBase + 0x114FB0 );
    }

    // Weapon data function pointers from client.dll
    if ( hClient )
    {
        auto clientBase = (uintptr_t)hClient;
        fnWeaponDataNameToIndex = reinterpret_cast<WeaponDataNameToIndexFn>( clientBase + 0x25E6F0 );
        fnWeaponDataIndexToPtr  = reinterpret_cast<WeaponDataIndexToPtrFn>( clientBase + 0x25E6C0 );
        fnWeaponDataInvalidIdx  = reinterpret_cast<WeaponDataInvalidIdxFn>( clientBase + 0x18B290 );
        fnIsBreakableEntity     = reinterpret_cast<IsBreakableEntityFn>( clientBase + 0x1E1790 );
        // printf_s( "[SDK] WeaponData functions: NameToIdx=%p IdxToPtr=%p InvalidIdx=%p\n",
        //           fnWeaponDataNameToIndex, fnWeaponDataIndexToPtr, fnWeaponDataInvalidIdx );
    }

    ScanScreenTransform( hEngine );
}

} // namespace SDK
