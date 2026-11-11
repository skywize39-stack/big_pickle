#include "features/visuals/smoke/NoSmoke.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "platform/windows/Platform.hpp"
#include "third_party/MinHook/minhook.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// ============================================================
//  NoSmoke.cpp
//
//  Reversed (client.dll, image base 0x180000000):
//    C_ParticleSmokeGrenade main vtable        RVA 0x6F8ED8 (entity+0)
//    slot 92  (+0x2E0) particle-system builder RVA 0x32CF60
//             (creates the DME particle system of the cloud:
//              "DmeAlphaCosineUpdater", "DmeSmokeGrenadeUpdater", ...)
//    slot 180 (+0x5A0) Update                  RVA 0x32D5E0
//             (adds the cloud weight to g_SmokeFogOverlayAlpha when the
//              local eye is inside the cloud radius)
//    g_SmokeFogOverlayAlpha (float)            RVA 0x95A298
//             (reset to 0 every frame at RVA 0x163C61, DrawSmokeFogOverlay
//              RVA 0x357820 early-outs when it is 0)
//
//  1) Update detour: while No Smoke is on the overlay alpha is never
//     accumulated -> no gray screen fog inside the smoke.
//  2) Builder detour: while No Smoke is on a NEW smoke never builds its
//     particle system -> no cloud is drawn. A smoke that was already
//     built before the option was turned on keeps its particles until
//     it fades (the render slot of the particle object is not confirmed,
//     so it is intentionally not hooked).
//
//  Both hooks verify the function prologue and the vtable slot before
//  installing; on mismatch (other client build) nothing is hooked.
// ============================================================

namespace
{
    constexpr uintptr_t kSmokeVtableRVA  = 0x6F8ED8;
    constexpr uintptr_t kBuildRVA        = 0x32CF60;  // vtable slot 92
    constexpr uintptr_t kUpdateRVA       = 0x32D5E0;  // vtable slot 180
    constexpr uintptr_t kFogAlphaRVA     = 0x95A298;
    constexpr int       kBuildSlot       = 92;

    // CS:GO-style .pcf cloud ("explosion_smokegrenade", created by
    // C_SmokeGrenadeProjectile::DoSmokeEffect RVA 0x2B1430).
    constexpr uintptr_t kCollRenderRVA   = 0x465050;  // CParticleCollection::Render
    constexpr uintptr_t kFindDefRVA      = 0x4611B0;  // GetParticleSystemTypeByName(mgr, name)
    constexpr uintptr_t kParticleMgrRVA  = 0x86E0D0;  // CParticleMgr* (pointer variable)
    constexpr uintptr_t kCollDefOffset   = 88;        // CParticleCollection -> definition
    // 48 89 5C 24 ?? 56 41 54 41 57 48 83 EC 60   (0xCC = wildcard)
    constexpr uint8_t kCollRenderPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0xCC, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60 };
    // 48 8B 89 ?? ?? ?? ?? E9
    constexpr uint8_t kFindDefPrologue[]    = { 0x48, 0x8B, 0x89, 0xCC, 0xCC, 0xCC, 0xCC, 0xE9 };
    constexpr int       kUpdateSlot      = 180;

    // 40 53 55 48 83 EC 48 48 8B DA
    constexpr uint8_t kBuildPrologue[]  = { 0x40, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0xDA };
    // 40 53 48 83 EC 20 80 B9
    constexpr uint8_t kUpdatePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9 };

    // Builder takes (this, rdx=pointer arg); extra register args are passed
    // through untouched so the original sees exactly what the game sent.
    using BuildFn  = void( __fastcall* )( void*, void*, void*, void* );
    using UpdateFn = void( __fastcall* )( void* );
    // (pColl, pMesh, flags, pAttach) + 2 passthrough stack slots, so the
    // original gets exactly what the game passed even if there are more args.
    using CollRenderFn = void( __fastcall* )( void*, void*, uintptr_t, void*, void*, void* );
    using FindDefFn    = void*( __fastcall* )( void*, const char* );

    CollRenderFn oCollRender = nullptr;
    FindDefFn    s_findDef   = nullptr;
    bool         s_renderHooked = false;
    void*        s_smokeDef  = nullptr;   // definition of "explosion_smokegrenade"
    ULONGLONG    s_defLookupAt = 0;
    long s_renderSkipped = 0;

    // C_SmokeGrenadeProjectile::DoSmokeEffect (this = entity+0x10).
    // Returning early keeps m_bDidSmokeEffect (this+0xD20) at 0, so the
    // game simply retries later and the cloud appears once the option is off.
    constexpr uintptr_t kDoSmokeRVA = 0x2B1430;
    // 48 89 5C 24 ?? 55 48 8B EC 48 83 EC 70 83 B9
    constexpr uint8_t kDoSmokePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0xCC, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70, 0x83, 0xB9 };
    using DoSmokeFn = void( __fastcall* )( void*, void*, void*, void* );
    DoSmokeFn oDoSmoke = nullptr;
    bool      s_doSmokeHooked = false;
    long s_doSmokeBlocked = 0;

    void __fastcall DoSmokeDetour( void* thisptr, void* a2, void* a3, void* a4 )
    {
        if ( g_config.settings.visuals.others.no_smoke &&
             g_config.settings.visuals.others.no_smoke_particles )
        {
            ++s_doSmokeBlocked;
            return;
        }
        oDoSmoke( thisptr, a2, a3, a4 );
    }

    BuildFn   oBuild   = nullptr;
    UpdateFn  oUpdate  = nullptr;
    uintptr_t s_client = 0;
    bool      s_buildHooked  = false;
    bool      s_updateHooked = false;

    // Diagnostics (printed every 2 s while No Smoke is on).
    long s_updCallsSmoke = 0, s_updCallsOther = 0;
    long s_buildCallsSmoke = 0, s_buildCallsOther = 0;
    ULONGLONG s_lastDiag = 0;

    bool IsSmokeEntity( void* ent )
    {
        return ent && s_client &&
               *reinterpret_cast<uintptr_t*>( ent ) == s_client + kSmokeVtableRVA;
    }

    bool WantNoSmoke()
    {
        return g_config.settings.visuals.others.no_smoke;
    }

    void __fastcall BuildDetour( void* thisptr, void* a2, void* a3, void* a4 )
    {
        if ( IsSmokeEntity( thisptr ) ) ++s_buildCallsSmoke; else ++s_buildCallsOther;
        if ( WantNoSmoke() && g_config.settings.visuals.others.no_smoke_particles &&
             IsSmokeEntity( thisptr ) )
            return;   // no particle system -> no visible cloud
        oBuild( thisptr, a2, a3, a4 );
    }

    void __fastcall UpdateDetour( void* thisptr )
    {
        if ( IsSmokeEntity( thisptr ) ) ++s_updCallsSmoke; else ++s_updCallsOther;
        if ( WantNoSmoke() && IsSmokeEntity( thisptr ) )
            return;   // overlay alpha is not accumulated
        oUpdate( thisptr );
    }

    void* LookupSmokeDef()
    {
        if ( !s_findDef )
            return nullptr;
        void* mgr = *reinterpret_cast<void**>( s_client + kParticleMgrRVA );
        if ( !mgr )
            return nullptr;
        return s_findDef( mgr, "explosion_smokegrenade" );
    }

    void __fastcall CollRenderDetour( void* coll, void* mesh, uintptr_t flags, void* attach, void* a5, void* a6 )
    {
        if ( coll && WantNoSmoke() )
        {
            // Refresh the definition pointer once a second (map change safe).
            const ULONGLONG now = GetTickCount64();
            if ( now - s_defLookupAt > 1000 )
            {
                s_defLookupAt = now;
                s_smokeDef = LookupSmokeDef();
            }
            void* def = *reinterpret_cast<void**>( (uintptr_t)coll + kCollDefOffset );
            // Children are rendered recursively from the parent's Render, so
            // skipping the root collection hides every sub-effect too.
            if ( def && def == s_smokeDef )
            {
                ++s_renderSkipped;
                return;
            }
        }
        oCollRender( coll, mesh, flags, attach, a5, a6 );
    }

    bool MatchMasked( const uint8_t* p, const uint8_t* pat, size_t len )
    {
        for ( size_t i = 0; i < len; ++i )
            if ( pat[ i ] != 0xCC && p[ i ] != pat[ i ] )
                return false;
        return true;
    }

    bool VerifyPrologue( uintptr_t fnRVA, const uint8_t* pat, size_t len )
    {
        __try
        {
            return MatchMasked( reinterpret_cast<const uint8_t*>( s_client + fnRVA ), pat, len );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return false;
        }
    }

    bool VerifyTarget( uintptr_t fnRVA, int slot, const uint8_t* prologue, size_t len )
    {
        const auto* fn = reinterpret_cast<const uint8_t*>( s_client + fnRVA );
        const auto* vt = reinterpret_cast<const uintptr_t*>( s_client + kSmokeVtableRVA );
        __try
        {
            if ( std::memcmp( fn, prologue, len ) != 0 )
                return false;
            if ( vt[ slot ] != s_client + fnRVA )
                return false;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return false;
        }
        return true;
    }
}

bool NoSmoke::Init()
{
    s_client = (uintptr_t)GetModuleHandleA( "client.dll" );
    if ( !s_client )
        return false;

    if ( VerifyTarget( kUpdateRVA, kUpdateSlot, kUpdatePrologue, sizeof( kUpdatePrologue ) ) )
    {
        void* target = (void*)( s_client + kUpdateRVA );
        if ( MH_CreateHook( target, &UpdateDetour, (void**)&oUpdate ) == MH_OK &&
             MH_EnableHook( target ) == MH_OK )
            s_updateHooked = true;
    }

    if ( VerifyTarget( kBuildRVA, kBuildSlot, kBuildPrologue, sizeof( kBuildPrologue ) ) )
    {
        void* target = (void*)( s_client + kBuildRVA );
        if ( MH_CreateHook( target, &BuildDetour, (void**)&oBuild ) == MH_OK &&
             MH_EnableHook( target ) == MH_OK )
            s_buildHooked = true;
    }

    if ( VerifyPrologue( kFindDefRVA, kFindDefPrologue, sizeof( kFindDefPrologue ) ) )
        s_findDef = reinterpret_cast<FindDefFn>( s_client + kFindDefRVA );

    if ( s_findDef && VerifyPrologue( kCollRenderRVA, kCollRenderPrologue, sizeof( kCollRenderPrologue ) ) )
    {
        void* target = (void*)( s_client + kCollRenderRVA );
        if ( MH_CreateHook( target, &CollRenderDetour, (void**)&oCollRender ) == MH_OK &&
             MH_EnableHook( target ) == MH_OK )
            s_renderHooked = true;
    }

    if ( VerifyPrologue( kDoSmokeRVA, kDoSmokePrologue, sizeof( kDoSmokePrologue ) ) )
    {
        void* target = (void*)( s_client + kDoSmokeRVA );
        if ( MH_CreateHook( target, &DoSmokeDetour, (void**)&oDoSmoke ) == MH_OK &&
             MH_EnableHook( target ) == MH_OK )
            s_doSmokeHooked = true;
    }

    DBGLOG( "[nosmoke] update hook=%d build hook=%d render hook=%d finddef=%d dosmoke hook=%d\n",
        s_updateHooked ? 1 : 0, s_buildHooked ? 1 : 0, s_renderHooked ? 1 : 0, s_findDef ? 1 : 0,
        s_doSmokeHooked ? 1 : 0 );
    return s_updateHooked || s_buildHooked || s_renderHooked || s_doSmokeHooked;
}

void NoSmoke::Shutdown()
{
    if ( !s_client )
        return;
    if ( s_updateHooked )
    {
        MH_DisableHook( (void*)( s_client + kUpdateRVA ) );
        MH_RemoveHook ( (void*)( s_client + kUpdateRVA ) );
        s_updateHooked = false;
    }
    if ( s_doSmokeHooked )
    {
        MH_DisableHook( (void*)( s_client + kDoSmokeRVA ) );
        MH_RemoveHook ( (void*)( s_client + kDoSmokeRVA ) );
        s_doSmokeHooked = false;
    }
    if ( s_renderHooked )
    {
        MH_DisableHook( (void*)( s_client + kCollRenderRVA ) );
        MH_RemoveHook ( (void*)( s_client + kCollRenderRVA ) );
        s_renderHooked = false;
    }
    if ( s_buildHooked )
    {
        MH_DisableHook( (void*)( s_client + kBuildRVA ) );
        MH_RemoveHook ( (void*)( s_client + kBuildRVA ) );
        s_buildHooked = false;
    }
}

void NoSmoke::Update()
{
    // Belt and braces: if the Update hook could not be installed, still
    // clear the overlay alpha. The game rebuilds it from 0 every frame,
    // so writing 0 here is harmless when the option is off.
    if ( !s_client || !WantNoSmoke() )
        return;
    if ( !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        return;
    *reinterpret_cast<float*>( s_client + kFogAlphaRVA ) = 0.f;

    const ULONGLONG now = GetTickCount64();
    if ( now - s_lastDiag < 2000 )
        return;
    s_lastDiag = now;

    // How many live entities carry the C_ParticleSmokeGrenade vtable?
    int smokeEnts = 0;
    if ( SDK::g_pEntityList )
    {
        for ( int i = 1; i < 2048; ++i )
        {
            void* ent = SDK::g_pEntityList->GetClientEntity( i );
            if ( IsSmokeEntity( ent ) )
            {
                ++smokeEnts;
                DBGLOG( "[nosmoke] smoke ent #%d gate=%u stage=%u r=%.1f w=%.3f\n", i,
                    *reinterpret_cast<uint8_t*>( (uintptr_t)ent + 0x9A8 ),
                    *reinterpret_cast<uint8_t*>( (uintptr_t)ent + 0x9C0 ),
                    *reinterpret_cast<float*>( (uintptr_t)ent + 0x1584 ),
                    *reinterpret_cast<float*>( (uintptr_t)ent + 0x9C4 ) );
            }
        }
    }
    DBGLOG( "[nosmoke] pcf def=%p render skipped=%ld dosmoke blocked=%ld\n",
        s_smokeDef, s_renderSkipped, s_doSmokeBlocked );
    DBGLOG( "[nosmoke] hooks upd=%d build=%d | ents=%d | upd calls smoke=%ld other=%ld | build calls smoke=%ld other=%ld\n",
        s_updateHooked ? 1 : 0, s_buildHooked ? 1 : 0, smokeEnts,
        s_updCallsSmoke, s_updCallsOther, s_buildCallsSmoke, s_buildCallsOther );
}
