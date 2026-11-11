#include "features/misc/clantag/ClanTag.hpp"

#include "config/Config.hpp"
#include "sdk/SDK.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    // Verified against the user-supplied engine.dll:
    // engine.dll+0x8B9E0 builds/broadcasts the ClanTagChanged event and takes
    // (tag, name) in RCX/RDX. The signature contains the function's stable
    // instruction skeleton; rel32/RIP displacements are intentionally masked.
    constexpr unsigned char kSetClanTagSignature[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
        0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF1, 0x48, 0x8B,
        0xFA, 0xB9, 0x40, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00,
        0x00, 0x48, 0x85, 0xC0, 0x74, 0x14, 0x48, 0x8D, 0x15, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0xE8, 0x00, 0x00, 0x00,
        0x00, 0x48, 0x8B, 0xD8, 0xEB, 0x02, 0x33, 0xDB, 0x4C, 0x8B,
        0xC6, 0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B,
        0xCB, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC7
    };
    constexpr char kSetClanTagMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxx????xxxx????xxxxxxxxxxxxx????xxxx????xxx";
    static_assert( sizeof( kSetClanTagSignature ) == sizeof( kSetClanTagMask ) - 1,
        "ClanTag signature and mask must have the same size" );

    constexpr size_t kMaxClanTagChars = 15u;
    constexpr size_t kMaxModuleScanBytes = 64u * 1024u * 1024u;

    using SetClanTagFn = void( __fastcall* )( const char* tag, const char* name );

    SetClanTagFn s_setClanTag = nullptr;
    bool s_resolveAttempted = false;
    bool s_applied = false;
    std::string s_lastTag;
    std::string s_animationSource;
    int s_lastFrameCommandTick = INT_MIN;
    size_t s_frame = 0u;

    bool MatchesSignature( const unsigned char* address )
    {
        for ( size_t i = 0; i < sizeof( kSetClanTagSignature ); ++i )
        {
            if ( kSetClanTagMask[ i ] == 'x' && address[ i ] != kSetClanTagSignature[ i ] )
                return false;
        }
        return true;
    }

    SetClanTagFn ResolveSetClanTag()
    {
        if ( s_resolveAttempted )
            return s_setClanTag;
        s_resolveAttempted = true;

        const HMODULE engine = GetModuleHandleA( "engine.dll" );
        if ( !engine )
            return nullptr;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( engine );
        if ( !dos || dos->e_magic != IMAGE_DOS_SIGNATURE )
            return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const unsigned char*>( engine ) + dos->e_lfanew );
        if ( !nt || nt->Signature != IMAGE_NT_SIGNATURE )
            return nullptr;

        const size_t imageSize = std::min<size_t>( nt->OptionalHeader.SizeOfImage,
            kMaxModuleScanBytes );
        if ( imageSize < sizeof( kSetClanTagSignature ) )
            return nullptr;

        const auto* image = reinterpret_cast<const unsigned char*>( engine );
        const unsigned char* found = nullptr;
        for ( size_t offset = 0; offset <= imageSize - sizeof( kSetClanTagSignature ); ++offset )
        {
            const auto* current = image + offset;
            if ( MatchesSignature( current ) )
            {
                // A multiple match is unsafe: never select an arbitrary body.
                if ( found )
                    return nullptr;
                found = current;
            }
        }

        if ( !found )
            return nullptr;

        s_setClanTag = reinterpret_cast<SetClanTagFn>( const_cast<unsigned char*>( found ) );
        return s_setClanTag;
    }

    std::string BuildFrame( const char* configured, size_t frame )
    {
        std::string base = configured ? configured : "";
        if ( base.size() > kMaxClanTagChars )
            base.resize( kMaxClanTagChars );
        if ( base.empty() )
            return {};

        // Reference-style frame timeline. It adds exactly one letter while
        // the `|` cursor is visible, blinks on the complete text, removes one
        // letter per frame, then leaves a short cursor/blank pause:
        // "" -> | -> p| -> pr| -> ... -> project| -> project -> ... -> |.
        const size_t buildFrames = base.size() + 2u;
        constexpr size_t fullTextBlinkFrames = 10u;
        const size_t eraseFrames = base.size() + 2u;
        constexpr size_t idleBlinkFrames = 10u;
        const size_t cycleFrames = buildFrames + fullTextBlinkFrames +
            eraseFrames + idleBlinkFrames;
        size_t phase = frame % cycleFrames;

        if ( phase < buildFrames )
        {
            if ( phase == 0u )
                return {};
            if ( phase == 1u )
                return "|";
            return base.substr( 0u, phase - 1u ) + "|";
        }

        phase -= buildFrames;
        if ( phase < fullTextBlinkFrames )
        {
            // Same 10-frame cursor cadence as the reference implementation.
            const bool showCursor = phase % 4u == 0u || phase % 4u == 3u;
            return showCursor ? base + "|" : base;
        }

        phase -= fullTextBlinkFrames;
        if ( phase < eraseFrames )
        {
            if ( phase >= base.size() )
                return phase == base.size() ? "|" : std::string{};
            return base.substr( 0u, base.size() - phase - 1u ) + "|";
        }

        phase -= eraseFrames;
        const bool showCursor = phase == 2u || phase == 3u ||
            phase == 6u || phase == 7u;
        return showCursor ? "|" : std::string{};
    }

    bool Apply( const std::string& tag )
    {
        const auto setClanTag = ResolveSetClanTag();
        if ( !setClanTag )
            return false;

        // The verified dispatcher exposes both event fields (`tag` and `name`).
        // Supplying the same bounded string to both keeps all local consumers
        // consistent and avoids dangling pointers beyond this call.
        setClanTag( tag.c_str(), tag.c_str() );
        s_lastTag = tag;
        s_applied = true;
        return true;
    }

    void ClearAppliedTag()
    {
        if ( const auto setClanTag = ResolveSetClanTag() )
            setClanTag( "", "" );
        s_lastTag.clear();
        s_applied = false;
    }
}

void ClanTag::Update( const SDK::CUserCmd* cmd )
{
    const auto& cfg = g_config.settings.misc.clantag;
    const bool inGame = SDK::g_pEngine && SDK::g_pEngine->IsInGame();
    if ( !cfg.enabled )
    {
        // Clear immediately while connected. Once disconnected, avoid any
        // engine call because its event system may already be tearing down.
        if ( s_applied && inGame )
            ClearAppliedTag();
        s_lastFrameCommandTick = INT_MIN;
        s_frame = 0u;
        return;
    }

    if ( !inGame || !cmd )
    {
        s_lastFrameCommandTick = INT_MIN;
        s_frame = 0u;
        return;
    }

    std::string source = cfg.text;
    if ( source.size() > kMaxClanTagChars )
        source.resize( kMaxClanTagChars );
    if ( source != s_animationSource )
    {
        // A changed text must begin from the initial empty/cursor frame rather
        // than resuming at an unrelated position in the previous cycle.
        s_animationSource = source;
        s_frame = 0u;
        s_lastFrameCommandTick = INT_MIN;
    }

    // The reference's `(ticks / 20)` is a discrete command-tick clock. Use
    // the same model here: CreateMove may run more than once around choking or
    // shifting, but identical command ticks never consume another frame.
    const float boundedMs = std::clamp( cfg.speed_ms, 100.f, 2000.f );
    const int computedTicksPerFrame = static_cast<int>(
        boundedMs / ( SDK::kTickInterval * 1000.f ) + 0.999f );
    const int ticksPerFrame = computedTicksPerFrame > 0 ? computedTicksPerFrame : 1;
    const int commandTick = cmd->tick_count;
    if ( s_lastFrameCommandTick != INT_MIN )
    {
        const int elapsedTicks = commandTick - s_lastFrameCommandTick;
        if ( elapsedTicks >= 0 && elapsedTicks < ticksPerFrame )
            return;
    }

    const std::string tag = BuildFrame( source.c_str(), s_frame );
    if ( tag != s_lastTag || !s_applied )
    {
        // Do not consume a frame if the engine callback is unavailable or the
        // call cannot be made. This is the only place s_frame advances.
        if ( !Apply( tag ) )
            return;
    }

    ++s_frame;
    s_lastFrameCommandTick = commandTick;
}

void ClanTag::Shutdown()
{
    // Empty only the tag this module applied, and only while engine.dll remains
    // mapped. This avoids calling a stale engine function during process exit.
    if ( s_applied && GetModuleHandleA( "engine.dll" ) )
        ClearAppliedTag();

    s_setClanTag = nullptr;
    s_resolveAttempted = false;
    s_applied = false;
    s_lastTag.clear();
    s_animationSource.clear();
    s_lastFrameCommandTick = INT_MIN;
    s_frame = 0u;
}
