#include "features/networking/fakelag/FakeLag.hpp"
#include "core/hooks/D3DHook.hpp"
#include "config/Config.hpp"
#include "features/networking/fakeduck/FakeDuck.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/Exploits.hpp"
#include "third_party/MinHook/minhook.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

using namespace SDK;

extern void* g_pLocalPlayer;

// ============================================================
//  FakeLag.cpp — separate CL_Move FakeLag and SendDatagram
//  incoming-sequence Fake Ping paths.
//
//  FakeLag uses the verified cl_move send/choke branch only.
//  Fake Ping never touches that branch: SendDatagram writes its
//  incoming ack fields from a time-stamped sequence history.
// ============================================================

static constexpr uintptr_t kEngineImageBase = 0x180000000ull;
static constexpr uintptr_t kClMoveRva       = 0x0B1960ull;
static constexpr uintptr_t kTestBlRva       = 0x0B1AA1ull; // test bl, bl
static constexpr uintptr_t kChokedCmdsRva   = 0x6A3F70ull;
static constexpr uintptr_t kLastOutgoingRva = 0x6A3F6Cull;
static constexpr uint8_t   kTestBl[]        = { 0x84, 0xDB };
static constexpr uint8_t   kXorBl[]         = { 0x30, 0xDB }; // xor bl, bl

// Reversed from the supplied x64 engine.dll.
// CNetChan::SendDatagram starts at 0x1801F6500. Its prologue copies RCX into
// R14, then packet-header construction reads m_nInSequenceNr at +0x10 and
// m_nInReliableState at +0x1C. The stable prefix below makes the hook fail
// closed if another engine build is loaded.
static constexpr uintptr_t kSendDatagramRva = 0x1F6500ull;
static constexpr uint8_t kSendDatagramPrologue[] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x8D, 0x6C, 0x24, 0xC9, 0x48, 0x81, 0xEC, 0x00,
    0x01, 0x00, 0x00, 0x4C, 0x8B, 0xF1, 0x48, 0x8B, 0xFA
};
static constexpr size_t kNetchanInSequenceOffset = 0x10u;
static constexpr size_t kNetchanInReliableOffset = 0x1Cu;
static constexpr size_t kFakePingHistoryLimit    = 512u;

// CL_Move(float accumulatedExtraSamples, bool finalTick). The old integer
// prototype placed the first argument in RCX instead of XMM0 on x64.
using ClMoveFn = void( __fastcall* )( float accumulatedExtraSamples, bool finalTick );
static ClMoveFn oClMove = nullptr;

// CNetChan::SendDatagram(CNetChan* this, bf_write* datagram) returns an int
// status in EAX on the supplied engine build. The payload type is opaque here
// because Fake Ping only touches the confirmed CNetChan state before the call.
using SendDatagramFn = int( __fastcall* )( void* netChannel, void* datagram );
static SendDatagramFn oSendDatagram = nullptr;

static uint8_t* s_testBlPtr  = nullptr;
static DWORD    s_oldProtect = 0;
static bool     s_patched    = false;

bool FakeLag::s_shouldChoke = false;

static int s_tickCounter  = 0;
static int s_currentChoke = 0;

struct IncomingSequence
{
    int sequence = 0;
    int reliableState = 0;
    uint64_t capturedAtMs = 0;
};

static std::mutex s_fakePingMutex;
static std::deque<IncomingSequence> s_incomingSequences;
static void* s_sequenceChannel = nullptr;
static int s_lastIncomingSequence = -1;
static std::atomic<bool> s_fakePingActive{ false };
static std::atomic<bool> s_fakePingWasRequested{ false };
static std::atomic<int> s_fakePingEffectiveDelayMs{ 0 };

static int ClampFakePingDelayMs( int delayMs )
{
    if ( delayMs < 16 ) delayMs = 16;
    if ( delayMs > 218 ) delayMs = 218;
    return delayMs;
}

static void SetFakePingStatus( bool active, int effectiveDelayMs )
{
    s_fakePingEffectiveDelayMs.store( active ? effectiveDelayMs : 0,
        std::memory_order_relaxed );
    s_fakePingActive.store( active, std::memory_order_release );
}

static void ClearFakePingState()
{
    {
        std::lock_guard<std::mutex> lock( s_fakePingMutex );
        s_incomingSequences.clear();
        s_sequenceChannel = nullptr;
        s_lastIncomingSequence = -1;
    }
    s_fakePingWasRequested.store( false, std::memory_order_release );
    SetFakePingStatus( false, 0 );
}

static bool ShouldApplyFakePing()
{
    const auto& fp = g_config.settings.misc.fake_ping;
    return fp.enabled && g_pEngine && g_pEngine->IsInGame() &&
        g_pEngine->IsConnected();
}

static void CaptureIncomingSequenceLocked( void* netChannel, uint64_t nowMs )
{
    const auto* bytes = reinterpret_cast<const uint8_t*>( netChannel );
    const int sequence = *reinterpret_cast<const int*>( bytes + kNetchanInSequenceOffset );
    const int reliable = *reinterpret_cast<const int*>( bytes + kNetchanInReliableOffset );

    if ( netChannel != s_sequenceChannel )
    {
        s_incomingSequences.clear();
        s_sequenceChannel = netChannel;
        s_lastIncomingSequence = -1;
    }

    if ( sequence <= s_lastIncomingSequence )
        return;

    s_lastIncomingSequence = sequence;
    s_incomingSequences.push_front( { sequence, reliable, nowMs } );
    while ( s_incomingSequences.size() > kFakePingHistoryLimit )
        s_incomingSequences.pop_back();
}

static bool SelectDelayedIncomingSequenceLocked( uint64_t nowMs, int wantedDelayMs,
    IncomingSequence& out, int& effectiveDelayMs )
{
    for ( const auto& entry : s_incomingSequences )
    {
        if ( nowMs < entry.capturedAtMs )
            continue;
        const uint64_t ageMs = nowMs - entry.capturedAtMs;
        if ( ageMs < static_cast<uint64_t>( wantedDelayMs ) )
            continue;

        out = entry;
        effectiveDelayMs = static_cast<int>( ageMs );
        return true;
    }
    return false;
}

static void PatchChoke( bool enable )
{
    if ( !s_testBlPtr )
        return;

    if ( enable && !s_patched )
    {
        memcpy( s_testBlPtr, kXorBl, sizeof( kXorBl ) );
        s_patched = true;
    }
    else if ( !enable && s_patched )
    {
        memcpy( s_testBlPtr, kTestBl, sizeof( kTestBl ) );
        s_patched = false;
    }
}

// Reference rage cheats suspend FakeLag for as long as Double Tap is armed:
// choked commands eat the same sv_maxusrcmdprocessticks budget the tickbase
// shift needs (charge + choke <= limit), so running both at once made DT
// shift fewer ticks / fail and the shot desync. FakeLag resumes by itself
// the moment the DT bind is released or DT is disabled.
bool FakeLag::IsActive()
{
    if ( !g_config.settings.misc.fakelag.enabled )
        return false;
    if ( g_config.settings.misc.doubletap.enabled && DoubleTap::IsBindActive() )
        return false;
    return true;
}

bool FakeLag::CanControlPackets()
{
    return oClMove && s_testBlPtr;
}

// False while the game window is minimized or in the background (alt-tab).
// The engine keeps pumping CreateMove frames there; any choke schedule or DT
// charge that survives the tab-out is what made the game hitch/stretch after
// returning. Every packet-scheduling and recharge path stands down on this.
bool FakeLag::GameWindowUsable()
{
    const HWND hWnd = D3D::GetGameWindow();
    if ( !hWnd || IsIconic( hWnd ) )
        return false;
    return GetForegroundWindow() == hWnd;
}

int FakeLag::GetChokedCommands()
{
    if (!CanControlPackets()) return -1;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
    if (!base) return -1;
    __try
    {
        const int pending = *reinterpret_cast<const int*>(base + kChokedCmdsRva);
        return pending >= 0 && pending < MULTIPLAYER_BACKUP ? pending : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void FakeLag::SetCurrentMoveChoke(bool choke)
{
    ResetSchedule();
    s_shouldChoke = choke && CanControlPackets();
    PatchChoke(s_shouldChoke);
}

bool FakeLag::IsPatched()
{
    return s_patched;
}

void FakeLag::ForceRestore()
{
    PatchChoke( false );
}

void FakeLag::RequestChokeCurrentMove()
{
    // CreateMove issues this only after the first real DT attack command has
    // been created. CL_Move consumes it once, so a ready bind never changes
    // ordinary packet cadence.
    s_shouldChoke = true;
    s_tickCounter = 0;
    s_currentChoke = 0;

    // This is invoked by CreateMove while the original CL_Move is still
    // executing, but before its later test bl, bl send/choke decision.
    PatchChoke( true );
}

void FakeLag::ResetSchedule()
{
    // This belongs exclusively to packet-choke FakeLag/DT. Do not clear the
    // independent SendDatagram fake-latency history here: DT uses this reset
    // after a burst and genuine Fake Ping must not become another FakeLag.
    s_shouldChoke = false;
    s_tickCounter = 0;
    s_currentChoke = 0;
}

void FakeLag::FillFakeCommands()
{
    // Fatality-style fill fake commands (stub).
    // Full implementation requires client_state access.
}

bool FakeLag::IsFakePingActive()
{
    return s_fakePingActive.load( std::memory_order_acquire );
}

int FakeLag::GetFakePingEffectiveDelayMs()
{
    return s_fakePingEffectiveDelayMs.load( std::memory_order_relaxed );
}

static int __fastcall SendDatagramHook( void* netChannel, void* datagram )
{
    if ( !oSendDatagram )
        return 0;

    if ( !netChannel || !ShouldApplyFakePing() )
    {
        // Clearing an STL deque on every regular packet is needless work. Do
        // it once as Fake Ping is disabled/disconnected, then leave traffic
        // fully untouched until the user enables it again.
        if ( s_fakePingWasRequested.exchange( false, std::memory_order_acq_rel ) )
            ClearFakePingState();
        else
            SetFakePingStatus( false, 0 );
        return oSendDatagram( netChannel, datagram );
    }

    s_fakePingWasRequested.store( true, std::memory_order_release );
    const uint64_t nowMs = static_cast<uint64_t>( GetTickCount64() );
    const int wantedDelayMs = ClampFakePingDelayMs(
        g_config.settings.misc.fake_ping.delay_ms );

    IncomingSequence selected{};
    int effectiveDelayMs = 0;
    bool apply = false;
    {
        std::lock_guard<std::mutex> lock( s_fakePingMutex );
        CaptureIncomingSequenceLocked( netChannel, nowMs );
        apply = SelectDelayedIncomingSequenceLocked( nowMs, wantedDelayMs,
            selected, effectiveDelayMs );
    }

    if ( !apply )
    {
        // Keep traffic completely untouched until enough confirmed incoming
        // history exists to satisfy the selected delay.
        SetFakePingStatus( false, 0 );
        return oSendDatagram( netChannel, datagram );
    }

    auto* bytes = reinterpret_cast<uint8_t*>( netChannel );
    int* const inSequence = reinterpret_cast<int*>( bytes + kNetchanInSequenceOffset );
    int* const inReliable = reinterpret_cast<int*>( bytes + kNetchanInReliableOffset );
    const int originalSequence = *inSequence;
    const int originalReliable = *inReliable;

    // Only the packet being built sees the older incoming acknowledgement.
    // The live CNetChan state is restored after the original call.
    *inSequence = selected.sequence;
    *inReliable = selected.reliableState;
    SetFakePingStatus( true, effectiveDelayMs );

    const int result = oSendDatagram( netChannel, datagram );

    // Do not overwrite a value that changed during the original call. Restore
    // each field independently so an unexpected sequence update cannot leave
    // the other temporary acknowledgement value behind.
    if ( *inSequence == selected.sequence )
        *inSequence = originalSequence;
    if ( *inReliable == selected.reliableState )
        *inReliable = originalReliable;

    return result;
}

static void __fastcall ClMoveHook( float accumulatedExtraSamples, bool finalTick )
{
    if ( !oClMove )
        return;

    FakeDuck::UpdateRequest(g_pLocalPlayer);
    if (FakeDuck::WantsControl()) DoubleTap::Reset();

    // Direct charge/discharge transport from the supplied reference. While the
    // local budget is incomplete, do not create or rewrite a command at all.
    if ( DoubleTap::TrySkipMoveForRecharge() )
    {
        FakeLag::ResetSchedule();
        PatchChoke( false );
        return;
    }

    // A regular fake-lag choke is applied at entry. Fake Ping does not use
    // this site and therefore cannot create a second choke scheduler.
    // FD decides inside this command after prediction. A missing/invalid
    // CreateMove must default to sending, never repeat yesterday's choke.
    PatchChoke( !FakeDuck::WantsControl() && FakeLag::s_shouldChoke );
    oClMove( accumulatedExtraSamples, finalTick );
    PatchChoke( false );

    // Reference tickbase reconciliation after the real send.
    Exploits->NetUpdate();

    // Reference HandleTeleport drain: after the ordinary move (which may
    // carry the arming shot), spend the withheld charge as REAL extra
    // CL_Moves. Each extra runs the full engine CreateMove + prediction
    // pipeline, so the follow-up bullet is selected by the ordinary
    // rage pipeline for its own command — donor parity. The final extra
    // releases with finalTick=true so the whole drain flushes in one go.
    if ( Exploits->TeleportThisTick() )
    {
        const uintptr_t engineBase = reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) );
        const int outgoingBefore = engineBase
            ? *reinterpret_cast<const int*>( engineBase + kLastOutgoingRva ) : -1;
        const int chokedBefore = engineBase
            ? *reinterpret_cast<const int*>( engineBase + kChokedCmdsRva ) : -1;

        const auto shiftMove = []( bool finalTick )
        {
            PatchChoke( false );
            oClMove( 0.0f, finalTick );
        };
        Exploits->HandleTeleport( shiftMove );

        const int outgoingAfter = engineBase
            ? *reinterpret_cast<const int*>( engineBase + kLastOutgoingRva ) : -1;
        const int chokedAfter = engineBase
            ? *reinterpret_cast<const int*>( engineBase + kChokedCmdsRva ) : -1;
        DBGLOG( "[doubletap] DRAIN out=%d->%d choke=%d->%d shift=%d\n",
            outgoingBefore, outgoingAfter, chokedBefore, chokedAfter,
            Exploits->tickbase_shift );

        FakeLag::ResetSchedule();
        return;
    }

    FakeLag::ResetSchedule();
}

void FakeLag::Run()
{
    const auto& fl = g_config.settings.misc.fakelag;
    if (FakeDuck::OwnsPackets()) return;

    // Alt-tab / minimized: stop scheduling chokes and clear the pending
    // schedule. Otherwise a choke window survives the tab-out and the first
    // frames after returning stall/snap.
    if ( !GameWindowUsable() )
    {
        ResetSchedule();
        return;
    }

    // A queued DT has just armed its original shot from CreateMove. Do not
    // reset s_shouldChoke here: CL_Move has not reached its send/choke branch.
    if ( DoubleTap::IsBurstPending() )
        return;

    // Fake Ping has a separate SendDatagram hook. This scheduler now owns only
    // the established FakeLag packet choke cadence.
    if ( !FakeLag::IsActive() || DoubleTap::IsRecharging() || !g_pEngine ||
         !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
    {
        ResetSchedule();
        return;
    }

    const uintptr_t engineBase = reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) );
    if ( !engineBase )
        return;

    const int choked = *reinterpret_cast<const int*>( engineBase + kChokedCmdsRva );
    if ( choked == 0 && s_tickCounter > 0 )
    {
        s_tickCounter = 0;
        s_currentChoke = 0;
    }

    if ( s_currentChoke == 0 )
    {
        int base = fl.choke_ticks;
        if ( base < 1 ) base = 1;
        if ( base > 14 ) base = 14;

        int variance = fl.variance;
        if ( variance > 0 )
        {
            const int tick = static_cast<int>( g_pEngine->GetTime() * 1000.0f );
            variance = ( tick % ( variance * 2 + 1 ) ) - variance;
        }

        s_currentChoke = base + variance;
        if ( s_currentChoke < 0 ) s_currentChoke = 0;
        if ( s_currentChoke > 14 ) s_currentChoke = 14;
    }

    if ( s_tickCounter < s_currentChoke )
    {
        s_shouldChoke = true;
        ++s_tickCounter;
    }
    else
    {
        s_shouldChoke = false;
        s_currentChoke = 0;
        s_tickCounter = 0;
    }
}

bool FakeLag::HookClMove()
{
    const uintptr_t engineBase = reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) );
    if ( !engineBase )
        return false;

    s_testBlPtr = reinterpret_cast<uint8_t*>( engineBase + kTestBlRva );
    const auto* sendDatagram = reinterpret_cast<const uint8_t*>( engineBase + kSendDatagramRva );
    if ( memcmp( s_testBlPtr, kTestBl, sizeof( kTestBl ) ) != 0 ||
         memcmp( sendDatagram, kSendDatagramPrologue, sizeof( kSendDatagramPrologue ) ) != 0 )
        return false; // refuse an unverified engine build

    const void* clMove = reinterpret_cast<void*>( engineBase + kClMoveRva );
    if ( MH_CreateHook( const_cast<void*>( clMove ), &ClMoveHook,
        reinterpret_cast<void**>( &oClMove ) ) != MH_OK )
        return false;

    if ( MH_CreateHook( const_cast<uint8_t*>( sendDatagram ), &SendDatagramHook,
        reinterpret_cast<void**>( &oSendDatagram ) ) != MH_OK )
    {
        MH_RemoveHook( const_cast<void*>( clMove ) );
        oClMove = nullptr;
        return false;
    }

    DWORD oldProtect;
    if ( !VirtualProtect( s_testBlPtr, sizeof( kTestBl ), PAGE_EXECUTE_READWRITE, &oldProtect ) )
    {
        MH_RemoveHook( const_cast<uint8_t*>( sendDatagram ) );
        MH_RemoveHook( const_cast<void*>( clMove ) );
        oSendDatagram = nullptr;
        oClMove = nullptr;
        return false;
    }
    s_oldProtect = oldProtect;

    if ( MH_EnableHook( const_cast<void*>( clMove ) ) != MH_OK ||
         MH_EnableHook( const_cast<uint8_t*>( sendDatagram ) ) != MH_OK )
    {
        MH_DisableHook( const_cast<void*>( clMove ) );
        MH_DisableHook( const_cast<uint8_t*>( sendDatagram ) );
        MH_RemoveHook( const_cast<void*>( clMove ) );
        MH_RemoveHook( const_cast<uint8_t*>( sendDatagram ) );
        DWORD dummy;
        VirtualProtect( s_testBlPtr, sizeof( kTestBl ), s_oldProtect, &dummy );
        s_testBlPtr = nullptr;
        s_oldProtect = 0;
        oClMove = nullptr;
        oSendDatagram = nullptr;
        return false;
    }

    return true;
}

void FakeLag::UnhookClMove()
{
    FakeDuck::Reset();
    const uintptr_t engineBase = reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) );
    if ( !engineBase )
        return;

    PatchChoke( false );

    void* const clMove = reinterpret_cast<void*>( engineBase + kClMoveRva );
    void* const sendDatagram = reinterpret_cast<void*>( engineBase + kSendDatagramRva );
    MH_DisableHook( clMove );
    MH_RemoveHook( clMove );
    MH_DisableHook( sendDatagram );
    MH_RemoveHook( sendDatagram );
    oClMove = nullptr;
    oSendDatagram = nullptr;
    ClearFakePingState();

    if ( s_testBlPtr && s_oldProtect )
    {
        DWORD dummy;
        VirtualProtect( s_testBlPtr, sizeof( kTestBl ), s_oldProtect, &dummy );
        s_testBlPtr = nullptr;
        s_oldProtect = 0;
    }
}
