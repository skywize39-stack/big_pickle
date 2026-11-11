#include "ui/binds/KeyBinds.hpp"
#include "features/visuals/camera/ThirdPerson.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"

using namespace SDK;

// ============================================================
//  ThirdPerson.cpp
//
//  CSSO's camera is client-side. The engine runs CInput::CAM_Think
//  every frame regardless; it only applies the thirdperson camera
//  when the m_fCameraInThirdPerson flag is set. The game exposes
//  this through the "thirdperson" console command whose handler is
//  CAM_ToThirdPerson (client+0x1CD440). It sets the flag and the
//  initial camera offset; CAM_Think then maintains distance/lag.
//
//  We call the native handlers directly instead of feeding cvars,
//  so no engine interface (ICvar) is required.
//
//  Online fix: CAM_Think also calls sub_18015FDB0 every tick. That
//  helper re-checks sv_cheats (a server cvar that's 0 on real
//  servers, 1 on bot/listen games) and when it's 0 bounces through
//  engine vfunc 264 (jmp [engine+0x108]) which snaps the camera
//  back to first person — so thirdperson died instantly online.
//  Init() NOPs that sv_cheats gate so the normal toggle path always
//  runs; then CAM_ToThirdPerson's flag is honored everywhere.
// ============================================================

// RVAs inside client.dll (imagebase 0x180000000 happened to match):
static constexpr uintptr_t kCamToThirdPerson = 0x1CD440;
static constexpr uintptr_t kCamToFirstPerson = 0x1CD320;

// sub_18015FDB0 + 0x3D: `test eax,eax;  jnz short loc ; sv_cheats==0`
//   85 C0 75 47  ->  EB 49 90 90 (unconditional jump to the toggle path)
static constexpr uintptr_t kSvCheatsGateRVA = 0x15FDED;
static constexpr uint8_t   kOriginalBytes[4] = { 0x85, 0xC0, 0x75, 0x47 };
static constexpr uint8_t   kPatchedBytes[4]  = { 0xEB, 0x49, 0x90, 0x90 };

static uint8_t* s_gatePtr      = nullptr;
static DWORD    s_oldProtect   = 0;
static bool     s_patched      = false;
static bool     s_active       = false;
static bool     s_toggleState  = false;
static bool     s_keyWasDown   = false;

using CamFn = void( __fastcall* )();

static bool ThirdPersonBindActive()
{
    const auto& misc = g_config.settings.misc;
    if ( !misc.thirdperson_on_key )
    {
        s_toggleState = false;
        s_keyWasDown = false;
        return true;
    }

    return Binds::Active( "thirdperson", misc.thirdperson_key, misc.thirdperson_key_mode, "Thirdperson" );
}

void ThirdPerson::Init()
{
    if ( s_patched || !g_uClientBase )
        return;

    s_gatePtr = reinterpret_cast<uint8_t*>( g_uClientBase + kSvCheatsGateRVA );

    DWORD oldProtect;
    if ( !VirtualProtect( s_gatePtr, 4, PAGE_EXECUTE_READWRITE, &oldProtect ) )
        return;
    s_oldProtect = oldProtect;

    memcpy( s_gatePtr, kPatchedBytes, 4 );
    s_patched = true;
}

void ThirdPerson::Shutdown()
{
    if ( !s_patched || !s_gatePtr )
        return;

    memcpy( s_gatePtr, kOriginalBytes, 4 );
    s_patched = false;

    if ( s_oldProtect )
    {
        DWORD dummy;
        VirtualProtect( s_gatePtr, 4, s_oldProtect, &dummy );
        s_oldProtect = 0;
    }
    s_gatePtr = nullptr;
    s_active = false;
    s_toggleState = false;
    s_keyWasDown = false;
}

bool ThirdPerson::IsActive()
{
    return s_active;
}

void ThirdPerson::Run()
{
    const bool gameReady = g_pEngine && g_pEngine->IsConnected() &&
                           g_pEngine->IsInGame();
    const bool want = g_config.settings.misc.thirdperson && gameReady &&
                      ThirdPersonBindActive();

    if ( want && !s_active )
    {
        auto fn = reinterpret_cast<CamFn>( g_uClientBase + kCamToThirdPerson );
        if ( fn )
        {
            fn();
            s_active = true;
        }
    }
    else if ( !want && s_active )
    {
        auto fn = reinterpret_cast<CamFn>( g_uClientBase + kCamToFirstPerson );
        if ( fn )
            fn();
        s_active = false;

        // Leaving a game or disabling the feature must not leave a stale
        // Toggle state that turns TP back on after a reconnect.
        if ( !gameReady || !g_config.settings.misc.thirdperson )
        {
            s_toggleState = false;
            s_keyWasDown = false;
        }
    }
}