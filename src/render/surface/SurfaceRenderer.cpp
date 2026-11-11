#include "render/surface/SurfaceRenderer.hpp"
#include "features/visuals/grenades/Grenades.hpp"
#include "features/visuals/esp/ESPManager.hpp"
#include "features/visuals/esp/BoxGeometry.hpp"
#include "resources/fonts/WeaponIconFontData.hpp"
#include "features/visuals/esp/WeaponIcons.hpp"
#include "sdk/SDK.hpp"
#include "sdk/GameTypes.hpp"
#include "config/Config.hpp"
#include "core/hooks/D3DHook.hpp"
#include "features/combat/players/PlayerList.hpp"
#include "features/visuals/camera/ThirdPerson.hpp"
#include "third_party/MinHook/minhook.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <chrono>

// ============================================================================
// SurfaceRenderer.cpp
//
// This module contains a deliberately narrow PaintTraverse hook and a small
// VGUI marker.  The marker relies only on data verified from the supplied
// vguimatsurface.dll; it does not use a generic Source SDK slot layout.
// ============================================================================

namespace SurfaceRenderer
{
    using CreateInterfaceFn = void* ( * )( const char* name, int* returnCode );
    using PaintTraverseFn = void ( __fastcall* )( void* thisptr,
                                                    uintptr_t panel,
                                                    bool forceRepaint,
                                                    bool allowForce );
    using GetPanelNameFn = const char* ( __fastcall* )( void* thisptr,
                                                         uintptr_t panel );
    using DrawSetColorFn = void ( __fastcall* )( void* thisptr,
                                                  int red, int green, int blue, int alpha );
    using DrawFilledRectFn = void ( __fastcall* )( void* thisptr,
                                                    int x0, int y0, int x1, int y1 );
    using CreateFontFn = uint16_t ( __fastcall* )( void* thisptr );
    using SetFontGlyphSetFn = bool ( __fastcall* )( void* thisptr, uint16_t font,
        const char* windowsFontName, int tall, int weight, int blur, int scanlines,
        int flags, int rangeMin, int rangeMax );
    using SetTextFontFn = void ( __fastcall* )( void* thisptr, uint16_t font );
    using SetTextColorFn = void ( __fastcall* )( void* thisptr,
        int red, int green, int blue, int alpha );
    using SetTextPosFn = void ( __fastcall* )( void* thisptr, int x, int y );
    using PrintTextFn = void ( __fastcall* )( void* thisptr, const wchar_t* text,
        int textLength, int drawType );

    // Verified against the supplied x64 vgui2.dll VGuiPanel010 singleton.
    // Slot 35 is Solve; GetName is slot 36, immediately before ClassName.
    static constexpr int kVt_GetPanelName = 36;
    static constexpr int kVt_PaintTraverse = 41;
    static constexpr uintptr_t kRvaVGuiPanelGetName = 0x1D360;

    // Verified by static reversing of the supplied vguimatsurface.dll:
    //   CreateInterface registration: VGuiSurface033
    //   Factory object vtable RVA:     0x169378
    //   DrawSetColor:                  vtable[11], RVA 0x148B0
    //   DrawFilledRect:                vtable[12], RVA 0x11BF0
    static constexpr const char* kSurfaceInterface = "VGuiSurface033";
    static constexpr uintptr_t kRva_SurfaceVtable = 0x169378u;
    static constexpr uintptr_t kRva_DrawSetColor = 0x148B0u;
    static constexpr uintptr_t kRva_DrawFilledRect = 0x11BF0u;
    static constexpr int kVt_DrawSetColor = 11;
    static constexpr int kVt_DrawFilledRect = 12;
    static constexpr int kVt_SetTextFont = 17;
    static constexpr int kVt_SetTextColor = 19;
    static constexpr int kVt_SetTextPos = 20;
    static constexpr int kVt_PrintText = 22;
    static constexpr int kVt_CreateFont = 66;
    static constexpr int kVt_SetFontGlyphSet = 67;

    static constexpr uintptr_t kRva_SetTextFont = 0x14A30u;
    static constexpr uintptr_t kRva_SetTextColor = 0x14960u;
    static constexpr uintptr_t kRva_SetTextPos = 0x14A40u;
    static constexpr uintptr_t kRva_PrintText = 0x138D0u;
    static constexpr uintptr_t kRva_CreateFont = 0x10A40u;
    static constexpr uintptr_t kRva_SetFontGlyphSet = 0x1A4D0u;

    static HMODULE s_vgui2 = nullptr;
    static HMODULE s_vguiMatSurface = nullptr;
    static void* s_hookTarget = nullptr;
    static void* s_surface = nullptr;
    static PaintTraverseFn oPaintTraverse = nullptr;

    // ------------------------------------------------------------------
    //  Direct Surface font cache
    //
    //  The verified VGuiSurface033 subset exposes no text-measure method, so
    //  advance widths are measured once per font with GDI using the same
    //  parameters vguimatsurface passes to CreateFontA. In CWin32Font the
    //  per-character advance is abcA + abcB + abcC, and the blur/outline
    //  corrections it applies to those three fields cancel out in that sum,
    //  so plain GDI ABC widths match what DrawPrintText actually advances.
    //  This is what makes centred text land on the real centre instead of the
    //  previous fixed 5px-per-glyph guess.
    // ------------------------------------------------------------------
    static constexpr int kNameFontMin = 8;
    static constexpr int kNameFontMax = 20;
    static constexpr int kItemFontMin = 6;
    static constexpr int kItemFontMax = 12;
    static constexpr int kNameFontCount = kNameFontMax - kNameFontMin + 1;
    static constexpr int kItemFontCount = kItemFontMax - kItemFontMin + 1;
    // Weapon-icon glyph sizes. The embedded CS2GunIcons.ttf is pre-created in
    // this range so the menu can switch sizes without creating fonts mid-draw.
    static constexpr int kIconFontMin = 10;
    static constexpr int kIconFontMax = 24;
    static constexpr int kIconFontCount = kIconFontMax - kIconFontMin + 1;

    static constexpr wchar_t kWidthAsciiFirst = 32;
    static constexpr wchar_t kWidthAsciiLast = 255;
    static constexpr wchar_t kWidthCyrillicFirst = 0x400;
    static constexpr wchar_t kWidthCyrillicLast = 0x45F;
    static constexpr int kWidthAsciiCount = kWidthAsciiLast - kWidthAsciiFirst + 1;
    static constexpr int kWidthCyrillicCount = kWidthCyrillicLast - kWidthCyrillicFirst + 1;

    struct SurfaceFont
    {
        uint16_t handle = 0;
        int      tall = 0;
        int      height = 0;
        short    fallbackWidth = 0;
        short    asciiWidth[kWidthAsciiCount]{};
        short    cyrillicWidth[kWidthCyrillicCount]{};
        bool     ready = false;
    };

    static SurfaceFont s_nameFonts[kNameFontCount];
    static SurfaceFont s_itemFonts[kItemFontCount];
    static SurfaceFont s_iconFonts[kIconFontCount]; // embedded CS2GunIcons.ttf glyphs
    static bool s_iconFontReady = false;
    static bool s_fontsReady = false;

    static std::atomic_uint s_callCount{ 0 };
    static std::atomic_bool s_live{ false };
    static std::atomic_bool s_reported{ false };
    static std::atomic_bool s_surfaceReported{ false };
    static thread_local unsigned int t_paintDepth = 0;

    static bool IsAddressInsideModule( HMODULE module, const void* address )
    {
        if ( !module || !address )
            return false;

        const auto base = reinterpret_cast<uintptr_t>( module );
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( base );
        if ( dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 )
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>( base +
            static_cast<uintptr_t>( dos->e_lfanew ) );
        if ( nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0 )
            return false;

        const auto candidate = reinterpret_cast<uintptr_t>( address );
        return candidate >= base && candidate < base + nt->OptionalHeader.SizeOfImage;
    }

    static bool IsExecutableAddressInsideModule( HMODULE module, const void* address )
    {
        if ( !IsAddressInsideModule( module, address ) )
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if ( !VirtualQuery( address, &mbi, sizeof( mbi ) ) || mbi.State != MEM_COMMIT )
            return false;

        const DWORD protection = mbi.Protect & 0xFFu;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    static bool IsGameHudPanel( void* panelInterface, uintptr_t panel )
    {
        if ( !panelInterface || !panel || !s_vgui2 )
            return false;

        auto** vtable = *reinterpret_cast<void***>( panelInterface );
        if ( !vtable )
            return false;

        // Exact VGuiPanel010 GetName trampoline recovered from the supplied
        // DLL. Slot 35 is Solve and must never be treated as a string-returning
        // method; the first name method is slot 36 / RVA 0x1D360.
        const uintptr_t expected = reinterpret_cast<uintptr_t>( s_vgui2 ) +
                                   kRvaVGuiPanelGetName;
        if ( reinterpret_cast<uintptr_t>( vtable[kVt_GetPanelName] ) != expected )
            return false;

        const auto getName = reinterpret_cast<GetPanelNameFn>( vtable[kVt_GetPanelName] );
        const char* name = getName( panelInterface, panel );
        return name && _stricmp( name, "MatSystemTopPanel" ) == 0;
    }

    static bool ValidatePaintTraverseTarget( const void* target )
    {
        if ( !IsExecutableAddressInsideModule( s_vgui2, target ) )
            return false;

        // The supplied vgui2.dll has this prologue at RVA 0x1D480.  Keeping
        // this check makes an incompatible client version fail closed rather
        // than placing a detour on an arbitrary vtable slot.
        static constexpr unsigned char kExpectedPrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x48, 0x8B, 0x01, 0x41, 0x0F, 0xB6
        };
        return std::memcmp( target, kExpectedPrologue, sizeof( kExpectedPrologue ) ) == 0;
    }

    static bool ValidateSurface( void* surface )
    {
        if ( !surface || !s_vguiMatSurface )
            return false;

        auto** vtable = *reinterpret_cast<void***>( surface );
        const auto moduleBase = reinterpret_cast<uintptr_t>( s_vguiMatSurface );
        if ( reinterpret_cast<uintptr_t>( vtable ) != moduleBase + kRva_SurfaceVtable )
            return false;

        return IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_DrawSetColor] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_DrawFilledRect] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_SetTextFont] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_SetTextColor] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_SetTextPos] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_PrintText] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_CreateFont] ) &&
               IsExecutableAddressInsideModule( s_vguiMatSurface, vtable[kVt_SetFontGlyphSet] ) &&
               reinterpret_cast<uintptr_t>( vtable[kVt_DrawSetColor] ) == moduleBase + kRva_DrawSetColor &&
               reinterpret_cast<uintptr_t>( vtable[kVt_DrawFilledRect] ) == moduleBase + kRva_DrawFilledRect &&
               reinterpret_cast<uintptr_t>( vtable[kVt_SetTextFont] ) == moduleBase + kRva_SetTextFont &&
               reinterpret_cast<uintptr_t>( vtable[kVt_SetTextColor] ) == moduleBase + kRva_SetTextColor &&
               reinterpret_cast<uintptr_t>( vtable[kVt_SetTextPos] ) == moduleBase + kRva_SetTextPos &&
               reinterpret_cast<uintptr_t>( vtable[kVt_PrintText] ) == moduleBase + kRva_PrintText &&
               reinterpret_cast<uintptr_t>( vtable[kVt_CreateFont] ) == moduleBase + kRva_CreateFont &&
               reinterpret_cast<uintptr_t>( vtable[kVt_SetFontGlyphSet] ) == moduleBase + kRva_SetFontGlyphSet;
    }

    // Replicates the advance width that CMatSystemSurface::DrawPrintText will
    // apply, using GDI with the same font parameters vguimatsurface passes to
    // CreateFontA. CWin32Font falls back to the maximum character width when
    // GetCharABCWidthsW fails, which is exactly what happens for the raster
    // "Small Fonts" face, so the fallback is mirrored here rather than treated
    // as an error.
    static bool MeasureSurfaceFont( const char* faceName, int tall, int weight,
                                    bool antialiased, SurfaceFont& out )
    {
        HDC dc = ::CreateCompatibleDC( nullptr );
        if ( !dc )
            return false;

        HFONT font = ::CreateFontA( tall, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            antialiased ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, faceName );
        if ( !font )
        {
            ::DeleteDC( dc );
            return false;
        }

        HGDIOBJ previous = ::SelectObject( dc, font );

        TEXTMETRICW metrics{};
        if ( ::GetTextMetricsW( dc, &metrics ) )
        {
            out.height = metrics.tmHeight;
            out.fallbackWidth = static_cast<short>( metrics.tmMaxCharWidth );
        }
        else
        {
            out.height = tall;
            out.fallbackWidth = static_cast<short>( tall / 2 + 1 );
        }

        {
            ABC abc[kWidthAsciiCount]{};
            const bool ok = ::GetCharABCWidthsW( dc, kWidthAsciiFirst,
                                                  kWidthAsciiLast, abc ) != FALSE;
            for ( int i = 0; i < kWidthAsciiCount; ++i )
                out.asciiWidth[i] = ok
                    ? static_cast<short>( abc[i].abcA + static_cast<int>( abc[i].abcB ) + abc[i].abcC )
                    : out.fallbackWidth;
        }
        {
            ABC abc[kWidthCyrillicCount]{};
            const bool ok = ::GetCharABCWidthsW( dc, kWidthCyrillicFirst,
                                                  kWidthCyrillicLast, abc ) != FALSE;
            for ( int i = 0; i < kWidthCyrillicCount; ++i )
                out.cyrillicWidth[i] = ok
                    ? static_cast<short>( abc[i].abcA + static_cast<int>( abc[i].abcB ) + abc[i].abcC )
                    : out.fallbackWidth;
        }

        ::SelectObject( dc, previous );
        ::DeleteObject( font );
        ::DeleteDC( dc );
        return true;
    }

    // Every selectable size is registered once during initialization. Creating
    // a Surface font while PaintTraverse is running would allocate font page
    // textures inside the engine's own draw pass, so the menu only switches
    // between handles that already exist.
    static bool CreateSurfaceFontFamily( void* surface, const char* faceName,
                                         int weight, int flags, bool antialiased,
                                         int firstSize, SurfaceFont* fonts, int count,
                                         int rangeMin = 0, int rangeMax = 0 )
    {
        auto** vtable = *reinterpret_cast<void***>( surface );
        const auto createFont = reinterpret_cast<CreateFontFn>( vtable[kVt_CreateFont] );
        const auto setGlyphSet = reinterpret_cast<SetFontGlyphSetFn>( vtable[kVt_SetFontGlyphSet] );

        int created = 0;
        for ( int i = 0; i < count; ++i )
        {
            SurfaceFont& font = fonts[i];
            font = SurfaceFont{};
            font.tall = firstSize + i;

            const uint16_t handle = createFont( surface );
            if ( !handle )
                continue;
            if ( !setGlyphSet( surface, handle, faceName, font.tall, weight, 0, 0,
                               flags, rangeMin, rangeMax ) )
                continue;
            if ( !MeasureSurfaceFont( faceName, font.tall, weight, antialiased, font ) )
                continue;

            font.handle = handle;
            font.ready = true;
            ++created;
        }
        return created == count;
    }

    static int ClampInt( int value, int low, int high )
    {
        if ( value < low ) return low;
        if ( value > high ) return high;
        return value;
    }

    // Nicknames: antialiased Verdana at the configured size.
    static const SurfaceFont* ActiveNameFont()
    {
        if ( !s_fontsReady )
            return nullptr;
        const int size = ClampInt( g_config.settings.visuals.esp.name_font_size,
                                   kNameFontMin, kNameFontMax );
        const SurfaceFont& font = s_nameFonts[size - kNameFontMin];
        return font.ready ? &font : nullptr;
    }

    // Flags, weapon names and world labels: non-antialiased Small Fonts.
    static const SurfaceFont* ActiveItemFont()
    {
        if ( !s_fontsReady )
            return nullptr;
        const int size = ClampInt( g_config.settings.visuals.esp.item_font_size,
                                   kItemFontMin, kItemFontMax );
        const SurfaceFont& font = s_itemFonts[size - kItemFontMin];
        return font.ready ? &font : nullptr;
    }

    // Weapon-icon glyphs (embedded CS2GunIcons.ttf) at the configured size.
    static const SurfaceFont* ActiveIconFont()
    {
        if ( !s_iconFontReady )
            return nullptr;
        const int size = ClampInt( g_config.settings.visuals.esp.weapon_icon_size,
                                   kIconFontMin, kIconFontMax );
        const int exact = size - kIconFontMin;
        if ( s_iconFonts[exact].ready )
            return &s_iconFonts[exact];

        // The configured size may have failed to register in the engine; fall
        // back to the nearest working size instead of dropping icons entirely.
        for ( int offset = 1; offset < kIconFontCount; ++offset )
        {
            if ( exact - offset >= 0 && s_iconFonts[exact - offset].ready )
                return &s_iconFonts[exact - offset];
            if ( exact + offset < kIconFontCount && s_iconFonts[exact + offset].ready )
                return &s_iconFonts[exact + offset];
        }
        return nullptr;
    }

    // Installs the embedded CS2GunIcons.ttf as a private GDI font so the
    // engine's SetFontGlyphSet("CS2 Gun Icons", ...) can resolve it without
    // touching the system font registry.
    static void InstallEmbeddedIconFont()
    {
        char tempPath[ MAX_PATH ]{};
        if ( !::GetTempPathA( (DWORD)sizeof( tempPath ), tempPath ) )
            return;
        char fontPath[ MAX_PATH ]{};
        _snprintf_s( fontPath, sizeof( fontPath ), _TRUNCATE,
                     "%sCS2GunIcons_embedded.ttf", tempPath );

        FILE* f = nullptr;
        if ( fopen_s( &f, fontPath, "wb" ) != 0 || !f )
        {
            DBGLOG( "[painttraverse] cannot write %s\n", fontPath );
            return;
        }
        const size_t written = fwrite( ESPDrawData::kFontData, 1,
                                       ESPDrawData::kFontDataSize, f );
        fclose( f );
        if ( written != ESPDrawData::kFontDataSize )
        {
            DBGLOG( "[painttraverse] short font write (%zu)\n", written );
            return;
        }

        // FR_PRIVATE: visible only to this process, no registry writes.
        if ( !::AddFontResourceExA( fontPath, FR_PRIVATE, 0 ) )
            DBGLOG( "[painttraverse] AddFontResourceExA failed for %s\n",
                         fontPath );
    }

    static bool ResolveSurface()
    {
        s_vguiMatSurface = GetModuleHandleA( "vguimatsurface.dll" );
        if ( !s_vguiMatSurface )
        {
            DBGLOG( "[painttraverse] vguimatsurface.dll is not loaded\n" );
            return false;
        }

        const auto createInterface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress( s_vguiMatSurface, "CreateInterface" ) );
        if ( !createInterface )
        {
            DBGLOG( "[painttraverse] vguimatsurface CreateInterface is missing\n" );
            return false;
        }

        void* candidate = createInterface( kSurfaceInterface, nullptr );
        if ( !ValidateSurface( candidate ) )
        {
            DBGLOG( "[painttraverse] %s validation failed; marker disabled\n", kSurfaceInterface );
            return false;
        }

        s_surface = candidate;

        // Verdana is a TrueType system face with Cyrillic coverage, so it keeps
        // nicknames readable and measurable. FONTFLAG_ANTIALIAS (0x010) plus
        // FONTFLAG_OUTLINE (0x200) is the smoothed variant the names use.
        const bool nameOk = CreateSurfaceFontFamily( s_surface, "Verdana", 700,
            0x210, true, kNameFontMin, s_nameFonts, kNameFontCount );

        // Small Fonts is a raster face: requesting FONTFLAG_ANTIALIAS would
        // blur its pixel grid, so only FONTFLAG_OUTLINE (0x200) is set here.
        const bool itemOk = CreateSurfaceFontFamily( s_surface, "Small Fonts", 500,
            0x200, false, kItemFontMin, s_itemFonts, kItemFontCount );

        // Weapon icons: embedded CS2GunIcons.ttf, private-installed via GDI so
        // SetFontGlyphSet resolves the "CS2 Gun Icons" face. The icon glyph range
        // is requested EXPLICITLY — a default range makes the engine fall back
        // to another face for ASCII letters, which rendered wrong glyphs before.
        InstallEmbeddedIconFont();
        for ( auto& font : s_iconFonts )
            font = SurfaceFont{};
        const int created = CreateSurfaceFontFamily( s_surface, "CS2 Gun Icons", 400,
            0x010, true, kIconFontMin, s_iconFonts, kIconFontCount, 0x21, 0x7E );
        s_iconFontReady = created > 0;
        if ( !s_iconFontReady )
            DBGLOG( "[painttraverse] CS2GunIcons surface font failed;"
                         " weapon icons fall back to text\n" );
        else if ( created != kIconFontCount )
        {
            DBGLOG( "[painttraverse] CS2GunIcons: %d/%d sizes created\n",
                         created, kIconFontCount );
            for ( int i = 0; i < kIconFontCount; ++i )
            {
                if ( !s_iconFonts[i].ready )
                    DBGLOG( "[painttraverse] CS2GunIcons size %d failed\n",
                                 kIconFontMin + i );
            }
        }

        s_fontsReady = nameOk && itemOk;
        if ( !s_fontsReady )
        {
            DBGLOG( "[painttraverse] Surface font initialization failed"
                         " (verdana=%d smallfonts=%d)\n",
                         nameOk ? 1 : 0, itemOk ? 1 : 0 );
            s_surface = nullptr;
            return false;
        }

        if ( !s_surfaceReported.exchange( true, std::memory_order_relaxed ) )
        {
            DBGLOG( "[painttraverse] %s verified; rect=vtable[%d], text=vtable[%d]\n",
                         kSurfaceInterface, kVt_DrawFilledRect, kVt_PrintText );
            DBGLOG( "[painttraverse] fonts ready: Verdana %d-%d AA,"
                         " Small Fonts %d-%d no-AA\n",
                         kNameFontMin, kNameFontMax, kItemFontMin, kItemFontMax );
        }
        return true;
    }

    struct DirectVGuiRenderer
    {
        void* surface = nullptr;
        DrawSetColorFn setColor = nullptr;
        DrawFilledRectFn filledRect = nullptr;
        SetTextFontFn setTextFont = nullptr;
        SetTextColorFn setTextColor = nullptr;
        SetTextPosFn setTextPos = nullptr;
        PrintTextFn printText = nullptr;
    };

    static int ClampByte( int value )
    {
        if ( value < 0 ) return 0;
        if ( value > 255 ) return 255;
        return value;
    }

    static void DirectFill( const DirectVGuiRenderer& renderer,
                            int left, int top, int right, int bottom,
                            int red, int green, int blue, int alpha )
    {
        if ( right <= left || bottom <= top || alpha <= 0 )
            return;
        renderer.setColor( renderer.surface, red, green, blue, alpha );
        renderer.filledRect( renderer.surface, left, top, right, bottom );
    }

    static void DirectOutline( const DirectVGuiRenderer& renderer,
                               int left, int top, int right, int bottom,
                               int red, int green, int blue, int alpha )
    {
        DirectFill( renderer, left, top, right, top + 1, red, green, blue, alpha );
        DirectFill( renderer, left, bottom - 1, right, bottom, red, green, blue, alpha );
        DirectFill( renderer, left, top, left + 1, bottom, red, green, blue, alpha );
        DirectFill( renderer, right - 1, top, right, bottom, red, green, blue, alpha );
    }

    static void DirectPlayerFrame(const DirectVGuiRenderer& renderer,
        int left, int top, int right, int bottom, const cfg::esp_t& esp,
        const cfg::box_color_t& color)
    {
        auto segments = [&](auto emit) {
            BoxGeometry::Segments(left, top, right, bottom, esp.box_type == 1, emit);
        };
        if (esp.outline_box)
            segments([&](int l, int t, int r, int b) {
                DirectFill(renderer, l-1, t-1, r+1, b+1, 1, 1, 1, ClampByte(color.a));
            });
        segments([&](int l, int t, int r, int b) {
            DirectFill(renderer, l, t, r, b, ClampByte(color.r), ClampByte(color.g),
                ClampByte(color.b), ClampByte(color.a));
        });
    }

    // Surface subset has no verified line/triangle method. Build a small
    // outlined arrow from confirmed filled rectangles only.
    static void DirectLine( const DirectVGuiRenderer& renderer, float fromX, float fromY,
                            float toX, float toY, int thickness,
                            int red, int green, int blue, int alpha )
    {
        const float dx = toX - fromX;
        const float dy = toY - fromY;
        const float absX = fabsf( dx );
        const float absY = fabsf( dy );
        const float longer = absX > absY ? absX : absY;

        // Hard cap. float-to-int conversion of a NaN / infinity / absurd
        // coordinate is undefined and can yield INT_MIN, and even a merely
        // huge finite value would spin millions of DrawFilledRect calls and
        // stall the frame. No legitimate line spans more than the viewport.
        if ( !std::isfinite( longer ) || longer > 4096.f )
            return;

        const int steps = static_cast<int>( longer );
        const int half = thickness > 1 ? thickness / 2 : 0;
        if ( steps <= 0 )
        {
            const int x = static_cast<int>( fromX );
            const int y = static_cast<int>( fromY );
            DirectFill( renderer, x - half, y - half, x + half + 1, y + half + 1,
                        red, green, blue, alpha );
            return;
        }
        for ( int i = 0; i <= steps; ++i )
        {
            const float t = static_cast<float>( i ) / static_cast<float>( steps );
            const int x = static_cast<int>( fromX + dx * t );
            const int y = static_cast<int>( fromY + dy * t );
            DirectFill( renderer, x - half, y - half, x + half + 1, y + half + 1,
                        red, green, blue, alpha );
        }
    }

    // ------------------------------------------------------------------
    //  Filled primitives
    //
    //  The verified VGuiSurface033 subset exposes only DrawSetColor and
    //  DrawFilledRect. There is no triangle, circle or line primitive, so
    //  solid shapes — which read far better than wireframes at marker size —
    //  are rasterised here one pixel row at a time.
    // ------------------------------------------------------------------
    static void DirectFilledTriangle( const DirectVGuiRenderer& renderer,
                                      float x0, float y0,
                                      float x1, float y1,
                                      float x2, float y2,
                                      int red, int green, int blue, int alpha )
    {
        if ( alpha <= 0 )
            return;

        const float minY = (std::min)( (std::min)( y0, y1 ), y2 );
        const float maxY = (std::max)( (std::max)( y0, y1 ), y2 );
        if ( !std::isfinite( minY ) || !std::isfinite( maxY ) )
            return;

        const int top    = static_cast<int>( floorf( minY ) );
        const int bottom = static_cast<int>( ceilf ( maxY ) );
        if ( bottom - top > 512 )
            return;

        for ( int y = top; y <= bottom; ++y )
        {
            const float yc = static_cast<float>( y ) + 0.5f;
            float lo =  1e30f;
            float hi = -1e30f;

            // Intersect the scanline with each of the three edges.
            for ( int e = 0; e < 3; ++e )
            {
                const float ax = e == 0 ? x0 : ( e == 1 ? x1 : x2 );
                const float ay = e == 0 ? y0 : ( e == 1 ? y1 : y2 );
                const float bx = e == 0 ? x1 : ( e == 1 ? x2 : x0 );
                const float by = e == 0 ? y1 : ( e == 1 ? y2 : y0 );

                if ( ( ay <= yc && by > yc ) || ( by <= yc && ay > yc ) )
                {
                    const float t = ( yc - ay ) / ( by - ay );
                    const float x = ax + ( bx - ax ) * t;
                    if ( x < lo ) lo = x;
                    if ( x > hi ) hi = x;
                }
            }

            if ( lo > hi )
                continue;

            const int left  = static_cast<int>( floorf( lo ) );
            const int right = static_cast<int>( ceilf ( hi ) );
            if ( right <= left )
                continue;

            DirectFill( renderer, left, y, right, y + 1, red, green, blue, alpha );
        }
    }

    static void DirectFilledCircle( const DirectVGuiRenderer& renderer,
                                    float cx, float cy, float radius,
                                    int red, int green, int blue, int alpha )
    {
        if ( alpha <= 0 || !std::isfinite( radius ) || radius <= 0.f || radius > 256.f )
            return;
        if ( !std::isfinite( cx ) || !std::isfinite( cy ) )
            return;

        const int top    = static_cast<int>( floorf( cy - radius ) );
        const int bottom = static_cast<int>( ceilf ( cy + radius ) );
        const float rr = radius * radius;

        for ( int y = top; y <= bottom; ++y )
        {
            const float dy = static_cast<float>( y ) + 0.5f - cy;
            const float span = rr - dy * dy;
            if ( span <= 0.f )
                continue;

            const float half = sqrtf( span );
            const int left  = static_cast<int>( floorf( cx - half ) );
            const int right = static_cast<int>( ceilf ( cx + half ) );
            if ( right <= left )
                continue;

            DirectFill( renderer, left, y, right, y + 1, red, green, blue, alpha );
        }
    }

    // PlayerInfo names are UTF-8 in the CSSO client. Surface::DrawPrintText
    // accepts UTF-16, so never decode the UTF-8 bytes through CP_ACP: that is
    // what previously produced mojibake for Cyrillic nicknames. ASCII/legacy
    // names retain a CP_ACP fallback only when strict UTF-8 decoding rejects
    // their byte sequence.
    static int Utf8ToSurfaceWide( const char* text, wchar_t* wide, int capacity )
    {
        if ( !text || !text[0] || !wide || capacity < 2 )
            return 0;

        const int utf8Length = ::MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS,
            text, -1, wide, capacity );
        if ( utf8Length > 1 )
            return utf8Length;

        return ::MultiByteToWideChar( CP_ACP, 0, text, -1, wide, capacity );
    }

    static void DirectTextWithFont( const DirectVGuiRenderer& renderer, uint16_t font,
                                    int x, int y, int red, int green, int blue,
                                    int alpha, const char* text )
    {
        if ( !font || !text || !text[0] || !renderer.setTextFont ||
             !renderer.setTextColor || !renderer.setTextPos || !renderer.printText )
            return;

        wchar_t wide[96]{};
        const int length = Utf8ToSurfaceWide( text, wide,
                                               static_cast<int>( _countof( wide ) ) );
        if ( length <= 1 )
            return;

        renderer.setTextFont( renderer.surface, font );
        renderer.setTextColor( renderer.surface, red, green, blue, alpha );
        renderer.setTextPos( renderer.surface, x, y );
        renderer.printText( renderer.surface, wide, length - 1, 0 );
    }

    static void DirectGlyphWithFont( const DirectVGuiRenderer& renderer, uint16_t font,
                                     int x, int y, int red, int green, int blue,
                                     int alpha, uint16_t codepoint )
    {
        if ( !font || !codepoint || !renderer.setTextFont ||
             !renderer.setTextColor || !renderer.setTextPos || !renderer.printText )
            return;

        const wchar_t glyph[] = { static_cast<wchar_t>( codepoint ), L'\0' };
        renderer.setTextFont( renderer.surface, font );
        renderer.setTextColor( renderer.surface, red, green, blue, alpha );
        renderer.setTextPos( renderer.surface, x, y );
        renderer.printText( renderer.surface, glyph, 1, 0 );
    }

    // Centres one weapon icon glyph on `centerX`. The advance width comes from
    // the GDI measurement taken at font creation, matching how the engine
    // positions DrawPrintText, so the icon is centred on the box at any width.
    static void DirectIconCentered( const DirectVGuiRenderer& renderer,
                                    int centerX, int y,
                                    int red, int green, int blue, int alpha,
                                    uint16_t codepoint )
    {
        const SurfaceFont* iconFont = ActiveIconFont();
        if ( !iconFont || !codepoint )
            return;

        // CS2GunIcons.ttf keys each icon on an ASCII letter, so the measured
        // width lives in the font's `asciiWidth` table at the codepoint index.
        const int index = static_cast<int>( codepoint );
        if ( index < kWidthAsciiFirst || index > kWidthAsciiLast )
            return;
        const int width = iconFont->asciiWidth[index - kWidthAsciiFirst];
        DirectGlyphWithFont( renderer, iconFont->handle,
                             centerX - width / 2, y,
                             red, green, blue, alpha, codepoint );
    }

    static int SurfaceGlyphWidth( const SurfaceFont& font, wchar_t glyph )
    {
        if ( glyph >= kWidthAsciiFirst && glyph <= kWidthAsciiLast )
            return font.asciiWidth[glyph - kWidthAsciiFirst];
        if ( glyph >= kWidthCyrillicFirst && glyph <= kWidthCyrillicLast )
            return font.cyrillicWidth[glyph - kWidthCyrillicFirst];
        return font.fallbackWidth;
    }

    static int SurfaceTextWidth( const SurfaceFont& font, const wchar_t* text, int length )
    {
        int width = 0;
        for ( int i = 0; i < length; ++i )
            width += SurfaceGlyphWidth( font, text[i] );
        return width;
    }

    // Left-aligned draw with an explicit font. Used for the flag column, where
    // the anchor is the right edge of the box rather than a centre.
    static void DirectTextFont( const DirectVGuiRenderer& renderer,
                                const SurfaceFont* font, int x, int y,
                                int red, int green, int blue, int alpha,
                                const char* text )
    {
        if ( !font )
            return;
        DirectTextWithFont( renderer, font->handle, x, y, red, green, blue,
                            alpha, text );
    }

    // Centred draw using the measured advance widths of the selected font, so
    // the label is centred on `centerX` for both Latin and Cyrillic strings and
    // at every configurable size.
    static void DirectTextCenteredFont( const DirectVGuiRenderer& renderer,
                                        const SurfaceFont* font, int centerX, int y,
                                        int red, int green, int blue, int alpha,
                                        const char* text )
    {
        if ( !font || !text || !text[0] )
            return;

        wchar_t wide[96]{};
        const int wideLength = Utf8ToSurfaceWide( text, wide,
                                                   static_cast<int>( _countof( wide ) ) );
        if ( wideLength <= 1 )
            return;

        const int width = SurfaceTextWidth( *font, wide, wideLength - 1 );
        DirectTextWithFont( renderer, font->handle, centerX - width / 2, y,
                            red, green, blue, alpha, text );
    }

    enum class SurfaceItemKind : unsigned char
    {
        None,
        Weapon,
        C4,
        HeGrenade,
        Flashbang,
        Smoke,
        Molotov,
        Decoy
    };

    static const char* StripWeaponPrefix( const char* name )
    {
        return name && _strnicmp( name, "weapon_", 7 ) == 0 ? name + 7 : name;
    }

    static uint16_t SurfaceWeaponIconCodepoint( const char* weaponName )
    {
        // Icon glyph comes from the embedded CS2GunIcons.ttf via the
        // weapons::gunIcons map. weaponName may carry the "weapon_" prefix
        // (C_BaseEntity::GetWeaponName) or be bare (SurfaceWeaponNameFromModel);
        // weapons::get normalises both forms. It returns 0 when the weapon has
        // no dedicated glyph, and the caller falls back to literal classname
        // text. NB: CS2GunIcons.ttf has no MP5SD glyph, so the map deliberately
        // points weapon_mp5sd at MP7's icon (N).
        return static_cast<uint16_t>( weapons::get( weaponName ) );
    }

    static SurfaceItemKind ClassifyWorldModel( const char* modelPath )
    {
        if ( !modelPath || !modelPath[0] )
            return SurfaceItemKind::None;

        // Projectile models are tested before generic w_* weapons because
        // grenade filenames use the same model prefix.
        if ( std::strstr( modelPath, "flashbang" ) )
            return SurfaceItemKind::Flashbang;
        if ( std::strstr( modelPath, "smokegrenade" ) || std::strstr( modelPath, "smoke" ) )
            return SurfaceItemKind::Smoke;
        if ( std::strstr( modelPath, "molotov" ) || std::strstr( modelPath, "incendiary" ) )
            return SurfaceItemKind::Molotov;
        if ( std::strstr( modelPath, "hegrenade" ) || std::strstr( modelPath, "fraggrenade" ) )
            return SurfaceItemKind::HeGrenade;
        if ( std::strstr( modelPath, "decoy" ) )
            return SurfaceItemKind::Decoy;
        if ( std::strstr( modelPath, "defuser" ) )
            return SurfaceItemKind::Weapon;
        if ( std::strstr( modelPath, "w_c4" ) || std::strstr( modelPath, "w_ied" ) ||
             std::strstr( modelPath, "/c4" ) || std::strstr( modelPath, "c4_" ) )
            return SurfaceItemKind::C4;

        // CSSO builds do not all use the CSGO `models/weapons/w_*` layout.
        // Grenade and C4 paths were already filtered above; accept the wider
        // weapons path family so dropped w_, v_ and legacy weapon models are
        // not silently discarded by a CS:GO-only prefix test.
        if ( std::strstr( modelPath, "models/weapons/" ) ||
             std::strstr( modelPath, "models/weapons2/" ) ||
             std::strstr( modelPath, "models/w_" ) )
            return SurfaceItemKind::Weapon;
        return SurfaceItemKind::None;
    }

    // The active weapon classname offset is not reliable on every CSSO build.
    // Prefer the model name of the actual weapon entity when it is available;
    // this prevents a stale/incorrect classname from turning a P90 into a
    // pistol icon. The returned strings are stable literals.
    static const char* SurfaceWeaponNameFromModel( void* weaponEntity )
    {
        if ( !weaponEntity || !SDK::g_pModelInfo )
            return nullptr;

        auto* weapon = reinterpret_cast<SDK::C_BaseEntity*>( weaponEntity );
        SDK::model_t* model = weapon->GetModel();
        const char* path = model ? SDK::g_pModelInfo->GetModelName( model ) : nullptr;
        if ( !path || !path[0] )
            return nullptr;

        if ( std::strstr( path, "p90" ) ) return "p90";
        if ( std::strstr( path, "ak47" ) ) return "ak47";
        if ( std::strstr( path, "m4a1" ) )
            return std::strstr( path, "silencer" ) ? "m4a1_silencer" : "m4a1";
        if ( std::strstr( path, "awp" ) ) return "awp";
        if ( std::strstr( path, "famas" ) ) return "famas";
        if ( std::strstr( path, "galil" ) ) return "galilar";
        if ( std::strstr( path, "aug" ) ) return "aug";
        if ( std::strstr( path, "sg556" ) || std::strstr( path, "sg552" ) ||
             std::strstr( path, "sg553" ) ) return "sg553";
        if ( std::strstr( path, "ssg08" ) || std::strstr( path, "scout" ) ) return "ssg08";
        if ( std::strstr( path, "scar20" ) || std::strstr( path, "sg550" ) ) return "scar20";
        if ( std::strstr( path, "g3sg1" ) ) return "g3sg1";
        if ( std::strstr( path, "m249" ) ) return "m249";
        if ( std::strstr( path, "negev" ) ) return "negev";
        if ( std::strstr( path, "mac10" ) ) return "mac10";
        if ( std::strstr( path, "mp5" ) ) return "mp5sd";
        if ( std::strstr( path, "mp7" ) ) return "mp7";
        if ( std::strstr( path, "mp9" ) || std::strstr( path, "tmp" ) ) return "mp9";
        if ( std::strstr( path, "ump45" ) ) return "ump45";
        if ( std::strstr( path, "bizon" ) ) return "bizon";
        if ( std::strstr( path, "p250" ) || std::strstr( path, "p228" ) ) return "p250";
        if ( std::strstr( path, "deagle" ) ) return "deagle";
        if ( std::strstr( path, "glock" ) ) return "glock";
        if ( std::strstr( path, "usp" ) ) return "usp_silencer";
        if ( std::strstr( path, "fiveseven" ) ) return "fiveseven";
        if ( std::strstr( path, "tec9" ) ) return "tec9";
        if ( std::strstr( path, "cz75" ) ) return "cz75a";
        if ( std::strstr( path, "revolver" ) ) return "revolver";
        if ( std::strstr( path, "nova" ) ) return "nova";
        if ( std::strstr( path, "xm1014" ) ) return "xm1014";
        if ( std::strstr( path, "mag7" ) ) return "mag7";
        if ( std::strstr( path, "sawedoff" ) ) return "sawedoff";
        if ( std::strstr( path, "knife" ) ) return "knife";
        if ( std::strstr( path, "molotov" ) ) return "molotov";
        if ( std::strstr( path, "incendiary" ) || std::strstr( path, "incgrenade" ) ) return "incgrenade";
        if ( std::strstr( path, "flashbang" ) ) return "flashbang";
        if ( std::strstr( path, "hegrenade" ) ) return "hegrenade";
        if ( std::strstr( path, "smokegrenade" ) ) return "smokegrenade";
        if ( std::strstr( path, "decoy" ) ) return "decoy";
        return nullptr;
    }

    static const char* SurfaceItemLabel( SurfaceItemKind kind )
    {
        switch ( kind )
        {
        case SurfaceItemKind::C4:        return "C4";
        case SurfaceItemKind::HeGrenade: return "HE";
        case SurfaceItemKind::Flashbang: return "FLASH";
        case SurfaceItemKind::Smoke:     return "SMOKE";
        case SurfaceItemKind::Molotov:   return "MOLOTOV";
        case SurfaceItemKind::Decoy:     return "DECOY";
        case SurfaceItemKind::Weapon:    return "WEAPON";
        default:                         return "";
        }
    }

    static const char* SurfaceWorldWeaponLabel( const char* modelPath )
    {
        if ( !modelPath )
            return "WEAPON";
        if ( std::strstr( modelPath, "ak47" ) ) return "AK47";
        if ( std::strstr( modelPath, "m4a1" ) ) return "M4A1";
        if ( std::strstr( modelPath, "awp" ) ) return "AWP";
        if ( std::strstr( modelPath, "ssg08" ) || std::strstr( modelPath, "scout" ) ) return "SCOUT";
        if ( std::strstr( modelPath, "deagle" ) ) return "DEAGLE";
        if ( std::strstr( modelPath, "glock" ) ) return "GLOCK";
        if ( std::strstr( modelPath, "usp" ) ) return "USP";
        if ( std::strstr( modelPath, "p250" ) ) return "P250";
        if ( std::strstr( modelPath, "fiveseven" ) ) return "FIVESEVEN";
        if ( std::strstr( modelPath, "tec9" ) ) return "TEC9";
        if ( std::strstr( modelPath, "mac10" ) ) return "MAC10";
        if ( std::strstr( modelPath, "mp9" ) ) return "MP9";
        if ( std::strstr( modelPath, "mp7" ) ) return "MP7";
        if ( std::strstr( modelPath, "ump45" ) ) return "UMP45";
        if ( std::strstr( modelPath, "p90" ) ) return "P90";
        if ( std::strstr( modelPath, "famas" ) ) return "FAMAS";
        if ( std::strstr( modelPath, "galilar" ) ) return "GALIL";
        if ( std::strstr( modelPath, "aug" ) ) return "AUG";
        if ( std::strstr( modelPath, "sg556" ) ) return "SG553";
        if ( std::strstr( modelPath, "xm1014" ) ) return "XM1014";
        if ( std::strstr( modelPath, "mag7" ) ) return "MAG7";
        if ( std::strstr( modelPath, "nova" ) ) return "NOVA";
        return "WEAPON";
    }

    // A compact shape icon rather than a game-font glyph. It keeps the icon
    // path independent of an unverified icon font/glyph map and uses only the
    // already reversed Surface rectangle methods.
    static void DrawSurfaceItemIcon( const DirectVGuiRenderer& renderer, int centerX, int y,
                                     SurfaceItemKind kind, int red, int green, int blue,
                                     int alpha )
    {
        const int shadowAlpha = alpha * 2 / 5;
        if ( kind == SurfaceItemKind::Weapon )
        {
            DirectFill( renderer, centerX - 8, y + 3, centerX + 7, y + 5,
                        1, 1, 1, shadowAlpha );
            DirectFill( renderer, centerX - 6, y + 2, centerX + 6, y + 4,
                        red, green, blue, alpha );
            DirectFill( renderer, centerX + 5, y + 1, centerX + 10, y + 3,
                        red, green, blue, alpha );
            DirectFill( renderer, centerX - 5, y + 4, centerX - 2, y + 8,
                        red, green, blue, alpha );
            return;
        }

        if ( kind == SurfaceItemKind::C4 )
        {
            DirectOutline( renderer, centerX - 7, y + 1, centerX + 7, y + 8,
                           1, 1, 1, shadowAlpha );
            DirectFill( renderer, centerX - 6, y + 2, centerX + 6, y + 7,
                        red, green, blue, alpha );
            DirectFill( renderer, centerX - 3, y, centerX - 1, y + 2,
                        235, 235, 235, alpha );
            DirectFill( renderer, centerX + 2, y + 3, centerX + 4, y + 5,
                        235, 75, 75, alpha );
            return;
        }

        // Grenade body, neck and pin. The type is conveyed by its label and
        // colour; no unsupported texture or glyph table is required.
        DirectFill( renderer, centerX - 4, y + 2, centerX + 5, y + 9,
                    1, 1, 1, shadowAlpha );
        DirectFill( renderer, centerX - 3, y + 3, centerX + 4, y + 8,
                    red, green, blue, alpha );
        DirectFill( renderer, centerX - 1, y + 1, centerX + 2, y + 3,
                    red, green, blue, alpha );
        DirectFill( renderer, centerX + 4, y + 2, centerX + 7, y + 3,
                    235, 235, 235, alpha );
    }

    // Player weapon ESP deliberately uses DirectTextCentered in the render
    // loop below. No glyph/font icon helper is active in this configuration.

    static bool DirectWorldToScreen( const Vec3& world, Vec2& screen )
    {
        // Reuse the project W2S helper: it projects against the live D3D9
        // viewport rather than the desktop/window client rectangle. This is
        // the same coordinate basis that positioned the old ImGui ESP.
        return SDK::WorldToScreen( world, screen );
    }

    // PaintTraverse is kept as the direct working ESP path. Its live entity
    // projection gets a two-frame local gate: one transient W2S sample cannot
    // create a visible box, but normal players become drawable on the next
    // PaintTraverse call without depending on the EndScene ESP cache.
    struct DirectProjectionSample
    {
        Vec2  head{};
        Vec2  feet{};
        int   consecutive = 0;
        bool  hasSample = false;
    };
    static DirectProjectionSample s_directProjection[65];

    // One flag per player index, set by DrawDirectPlayerBoxes for every
    // player whose ESP was actually emitted this frame. The off-screen arrow
    // pass reads it so a player that already has a box never also grows an
    // arrow. Cleared at the top of the box pass, which runs first.
    static bool s_directPlayerDrawn[65];

    static bool AcceptDirectPlayerProjection( int entityIndex, const Vec2& head,
                                              const Vec2& feet )
    {
        if ( entityIndex <= 0 || entityIndex >= 65 || !g_pD3Device ||
             !std::isfinite( head.x ) || !std::isfinite( head.y ) ||
             !std::isfinite( feet.x ) || !std::isfinite( feet.y ) )
            return false;

        D3DVIEWPORT9 vp{};
        if ( FAILED( g_pD3Device->GetViewport( &vp ) ) || !vp.Width || !vp.Height )
            return false;

        const float height = feet.y - head.y;
        if ( !std::isfinite( height ) || height < 4.f )
            return false;

        // At close range the projected head/feet can span more than the whole
        // viewport. That is still a valid player projection, so do not reject
        // it solely because its raw height is large. Reject only projections
        // that are completely outside one side of the viewport; this keeps the
        // false-positive guard while allowing near players to be clipped later.
        const float marginX = static_cast<float>( vp.Width ) * 0.25f;
        const float marginY = static_cast<float>( vp.Height ) * 0.25f;
        const bool outsideLeft  = head.x < -marginX && feet.x < -marginX;
        const bool outsideRight = head.x > static_cast<float>( vp.Width ) + marginX &&
                                  feet.x > static_cast<float>( vp.Width ) + marginX;
        const bool outsideTop   = head.y < -marginY && feet.y < -marginY;
        const bool outsideBottom = head.y > static_cast<float>( vp.Height ) + marginY &&
                                   feet.y > static_cast<float>( vp.Height ) + marginY;
        if ( outsideLeft || outsideRight || outsideTop || outsideBottom )
            return false;

        auto& previous = s_directProjection[entityIndex];
        if ( !previous.hasSample )
        {
            previous.head = head;
            previous.feet = feet;
            previous.consecutive = 1;
            previous.hasSample = true;
            return false;
        }

        const float oldHeight = previous.feet.y - previous.head.y;
        const float centerX = ( head.x + feet.x ) * 0.5f;
        const float centerY = ( head.y + feet.y ) * 0.5f;
        const float oldCenterX = ( previous.head.x + previous.feet.x ) * 0.5f;
        const float oldCenterY = ( previous.head.y + previous.feet.y ) * 0.5f;
        const float dx = centerX - oldCenterX;
        const float dy = centerY - oldCenterY;
        const float jump = sqrtf( dx * dx + dy * dy );
        const float allowedJump = ( oldHeight > 0.f ? oldHeight * 5.f : 250.f ) + 250.f;
        const bool geometryMatches = std::isfinite( oldHeight ) && oldHeight >= 4.f &&
            jump <= allowedJump && height <= oldHeight * 3.f && oldHeight <= height * 3.f;

        previous.head = head;
        previous.feet = feet;
        previous.consecutive = geometryMatches ? previous.consecutive + 1 : 1;
        return previous.consecutive >= 2;
    }

    static int DrawDirectPlayerBoxes( const DirectVGuiRenderer& renderer )
    {
        using namespace SDK;

        // Reset before any early return: the arrow pass must never read a
        // stale "this player has a box" flag from the previous frame.
        for ( int i = 0; i < 65; ++i )
            s_directPlayerDrawn[i] = false;

        const auto& esp = g_config.settings.visuals.esp;
        // `esp.esp` is the master toggle. `esp.box` controls only the frame;
        // name, health, armour, weapon and flags must stay independently usable.
        if ( !esp.esp || !g_pEngine || !g_pEntityList ||
             !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
            return 0;

        const int localIndex = g_pEngine->GetLocalPlayer();
        auto* local = reinterpret_cast<C_BaseEntity*>(
            g_pEntityList->GetClientEntity( localIndex ) );
        if ( !local )
            return 0;
        const int localTeam = local->GetTeamNum();
        const int maxClients = g_pEngine->GetMaxClients() < 64
            ? g_pEngine->GetMaxClients() : 64;

        // Font selection happens once per pass: the menu can only choose among
        // sizes registered during initialization, so no Surface font is created
        // while the engine is inside its own paint traversal.
        const SurfaceFont* nameFont = ActiveNameFont();
        const SurfaceFont* itemFont = ActiveItemFont();
        const int nameHeight = nameFont ? nameFont->height : 11;
        const int itemHeight = itemFont ? itemFont->height : 9;

        const bool showLocal = esp.local && ThirdPerson::IsActive();

        int submitted = 0;
        for ( int i = 1; i <= maxClients; ++i )
        {
            if ( i == localIndex && !showLocal )
                continue;

            auto* entity = reinterpret_cast<C_BaseEntity*>(
                g_pEntityList->GetClientEntity( i ) );
            if ( !entity )
                continue;
            if ( entity->GetTeamNum() == localTeam && i != localIndex )
                continue;

            // Direct Surface ESP intentionally has no dormant fade/cache:
            // the snapshot's dormant flag is authoritative and hides the
            // player immediately on the first dormant tick.
            const bool isDormant = *(bool*)( reinterpret_cast<uintptr_t>( entity ) + 0x1F6 );
            if ( isDormant || entity->GetHealth() <= 0 || entity->GetLifeState() != 0 )
                continue;

            // PlayerList: visuals off means no Surface ESP at all — box, fill,
            // name, health, armour, weapon and flags. Chams and skeletons are
            // filtered on their own paths; this is the box/name renderer, and
            // without this check the box stayed visible while chams vanished.
            const auto plOverride = PlayerList::GetVisualOverride( i );
            if ( plOverride.hide )
                continue;

            const Vec3 feetWorld = entity->GetAbsOrigin();
            const Vec3 headWorld{ feetWorld.x, feetWorld.y, feetWorld.z + 72.f };
            Vec2 feet{};
            Vec2 head{};
            if ( !DirectWorldToScreen( feetWorld, feet ) ||
                 !DirectWorldToScreen( headWorld, head ) ||
                 !AcceptDirectPlayerProjection( i, head, feet ) )
                continue;

            const float height = feet.y - head.y;
            const float halfWidth = height / 3.8f;
            const D3DVIEWPORT9 clipVp = [&]()
            {
                D3DVIEWPORT9 value{};
                g_pD3Device->GetViewport( &value );
                return value;
            }();
            // Clip only the draw rectangle. The raw projection remains used by
            // the temporal validator above, while near-camera players keep a
            // visible box instead of being discarded for leaving the viewport.
            const float rawLeft = feet.x - halfWidth;
            const float rawRight = feet.x + halfWidth;
            const float rawTop = head.y;
            const float rawBottom = feet.y;
            const int left = static_cast<int>( rawLeft < 0.f ? 0.f : rawLeft );
            const int top = static_cast<int>( rawTop < 0.f ? 0.f : rawTop );
            const int right = static_cast<int>(
                rawRight > static_cast<float>( clipVp.Width )
                    ? static_cast<float>( clipVp.Width ) : rawRight );
            const int bottom = static_cast<int>(
                rawBottom > static_cast<float>( clipVp.Height )
                    ? static_cast<float>( clipVp.Height ) : rawBottom );
            if ( right - left < 3 || bottom - top < 3 )
                continue;

            // PlayerList highlight replaces the configured box colour for this
            // player only. The ImGui path can no longer draw boxes, so without
            // this the highlight option had no visible effect at all.
            cfg::box_color_t highlightColor{};
            if ( plOverride.highlight )
            {
                highlightColor.r = plOverride.hl_r;
                highlightColor.g = plOverride.hl_g;
                highlightColor.b = plOverride.hl_b;
                highlightColor.a = plOverride.hl_a;
            }
            const auto& boxColor = plOverride.highlight ? highlightColor : esp.box_color;
            const int alpha = ClampByte( boxColor.a );
            const int shadowAlpha = alpha * 2 / 5;

            // Use the supplied ESP's compact layout, but keep every draw call
            // inside the statically verified VGuiSurface033 subset.
            const int health = entity->GetHealth() < 0 ? 0 :
                ( entity->GetHealth() > 100 ? 100 : entity->GetHealth() );
            const int healthRed = 255 - health * 255 / 100;
            const int healthGreen = health * 255 / 100;
            const int armour = entity->GetArmor() < 0 ? 0 :
                ( entity->GetArmor() > 100 ? 100 : entity->GetArmor() );

            if ( esp.box && esp.fill_box )
            {
                const auto& fillColor = esp.fill_color;
                DirectFill( renderer, left, top, right, bottom,
                            ClampByte( fillColor.r ), ClampByte( fillColor.g ),
                            ClampByte( fillColor.b ), ClampByte( fillColor.a ) );
            }

            if (esp.box)
                DirectPlayerFrame(renderer, left, top, right, bottom, esp, boxColor);

            // Health sits one compact lane to the left of the frame. Its label
            // follows the filled edge, mirroring the reference ESP rather than
            // spending a lower text lane on a redundant "HP" string.
            if ( esp.health != 0 )
            {
                const int barLeft = left - 5;
                const int filled = ( bottom - top - 2 ) * health / 100;
                DirectFill( renderer, barLeft, top, barLeft + 4, bottom,
                            1, 1, 1, shadowAlpha );
                DirectFill( renderer, barLeft + 1, bottom - filled - 1,
                            barLeft + 3, bottom - 1,
                            healthRed, healthGreen, 35, alpha );
                if ( health > 0 && health < 100 )
                {
                    char healthText[8]{};
                    sprintf_s( healthText, "%d", health );
                    DirectTextCenteredFont( renderer, itemFont, barLeft + 2,
                                            bottom - filled - itemHeight / 2 - 1,
                                            235, 235, 235, alpha, healthText );
                }
            }

            // The armour lane stays directly beneath the frame, as in the
            // supplied layout. No reload/ammo data are assumed from this SDK.
            int bottomY = bottom + 4;
            if ( esp.armor != 0 && armour > 0 )
            {
                const int filled = ( right - left - 2 ) * armour / 100;
                DirectFill( renderer, left, bottom + 1, right, bottom + 5,
                            1, 1, 1, shadowAlpha );
                DirectFill( renderer, left + 1, bottom + 2, left + 1 + filled, bottom + 4,
                            75, 145, 245, alpha );
                bottomY = bottom + 9;
            }

            // Direct Surface text: name above, weapon below, dense flags on
            // the right. Lane offsets follow the measured font heights so the
            // rows do not overlap the frame when a larger size is selected.
            player_info_t info{};
            const int centerX = ( left + right ) / 2;
            if ( esp.name && nameFont && g_pEngine->GetPlayerInfo( i, &info ) &&
                 info.name[0] )
                DirectTextCenteredFont( renderer, nameFont, centerX,
                                        top - nameHeight - 2,
                                        235, 235, 235, alpha, info.name );

            if ( esp.weapon )
            {
                void* activeWeapon = entity->GetActiveWeapon();
                // Prefer the weapon classname ("weapon_p90") for the icon: it is
                // what weapons::gunIcons is keyed on and tracks the *actual*
                // fired weapon. The model-path name is only used as a fallback
                // when the classname offset is missing/empty on a CSSO build —
                // model substring matching alone mislabels weapons (e.g. an
                // M4A4 could match a wrong name) and left glock as text.
                const char* clsName = entity->GetWeaponName();
                const char* raw = ( clsName && clsName[0] &&
                                    _strnicmp( clsName, "weapon_", 7 ) == 0 )
                                  ? clsName
                                  : SurfaceWeaponNameFromModel( activeWeapon );
                const char* weapon = StripWeaponPrefix( raw );
                const char* displayWeapon = weapon;
                if ( weapon && ( _stricmp( weapon, "sg556" ) == 0 ||
                                 _stricmp( weapon, "sg552" ) == 0 ||
                                 _stricmp( weapon, "sg553" ) == 0 ) )
                    displayWeapon = "sg553";
                if ( displayWeapon && displayWeapon[0] )
                {
                    // Icon mode: single glyph from the embedded CS2GunIcons
                    // font, drawn in the same Surface pass as the boxes —
                    // same thread, same frame, so the icon's lifetime is
                    // exactly the box's lifetime by construction.
                    const uint16_t cp = SurfaceWeaponIconCodepoint( raw );
                    if ( esp.weapon_style != 0 && s_iconFontReady && cp )
                    {
                        DirectIconCentered( renderer, centerX, bottomY,
                                            255, 255, 255, alpha, cp );
                    }
                    else
                    {
                        DirectTextCenteredFont( renderer, itemFont, centerX,
                                                bottomY, 205, 205, 205, alpha,
                                                displayWeapon );
                    }
                }
            }

            if ( esp.show_flags )
            {
                int flagY = top - 2;
                const auto addFlag = [&]( bool enabled, int red, int green, int blue,
                                          const char* label )
                {
                    if ( enabled )
                    {
                        DirectTextFont( renderer, itemFont, right + 4, flagY,
                                        red, green, blue, alpha, label );
                        flagY += itemHeight + 1;
                    }
                };
                addFlag( esp.flag_scoped && entity->IsScoped(), 55, 160, 235, "ZOOM" );
                addFlag( esp.flag_defuse && entity->IsDefusing(), 235, 75, 75, "DEFUSE" );

                // Reuse the same safe remote-plant path as CreateMove snapshot
                // capture: read m_bStartedArming only after the active weapon
                // is confirmed as C4. This uses existing verified offsets.
                bool isPlanting = false;
                if ( esp.flag_plant && entity->GetTeamNum() == 2 && entity->IsInBombZone() )
                {
                    void* activeWeapon = entity->GetActiveWeapon();
                    if ( activeWeapon )
                    {
                        const char* weaponClass = reinterpret_cast<const char*>(
                            reinterpret_cast<uintptr_t>( activeWeapon ) + nWeaponClassnameOffset );
                        if ( weaponClass && std::strstr( weaponClass, "c4" ) )
                            isPlanting = *(bool*)( reinterpret_cast<uintptr_t>( activeWeapon ) + nIsPlantingOffset );
                    }
                }
                addFlag( isPlanting, 245, 165, 55, "PLANT" );
                addFlag( esp.flag_kit && entity->HasDefuser(), 235, 235, 235, "KIT" );
                addFlag( esp.flag_helmet && entity->HasHelmet(), 235, 235, 235, "HK" );
                addFlag( esp.flag_flashed && entity->GetFlashDuration() > 0.1f &&
                         entity->GetFlashMaxAlpha() > 1.f,
                         235, 220, 90, "FLASH" );
            }

            // This player has on-screen ESP this frame; the arrow pass skips
            // him so the two features never mark the same player twice.
            s_directPlayerDrawn[i] = true;
            ++submitted;
        }
        return submitted;
    }

    // ------------------------------------------------------------------
    //  Skeletons
    //
    //  The old ImGui skeleton renderer consumed g_players, which downstream of
    //  the FrameStageNotify hook never filled on this build — so skeletons
    //  never drew alongside the already-working boxes. This pass reads the
    //  world-space bone positions captured by CaptureSkeletonBones into the
    //  CreateMove snapshot (the same snapshot that always ticks), projects
    //  each hitbox bone on the render thread and strokes lines to its parent.
    // ------------------------------------------------------------------
    static void DrawDirectSkeletons( const DirectVGuiRenderer& renderer )
    {
        using namespace SDK;
        const auto& esp = g_config.settings.visuals.esp;
        if ( !esp.esp || !esp.skeletons || !g_pEngine || !g_pEntityList ||
             !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
            return;

        EntitySnapshot snap;
        {
            std::lock_guard<std::mutex> lock( g_snapshotMutex );
            snap = g_snapshotData;
        }
        if ( snap.localPlayerIndex <= 0 || snap.localPlayerIndex >= 65 ||
             snap.maxClients <= 0 )
            return;

        const int localIndex = snap.localPlayerIndex;
        const int maxClients = snap.maxClients < 64 ? snap.maxClients : 64;
        auto* local = reinterpret_cast<C_BaseEntity*>(
            g_pEntityList->GetClientEntity( localIndex ) );
        if ( !local )
            return;
        const int localTeam = local->GetTeamNum();

        for ( int i = 1; i <= maxClients; ++i )
        {
            if ( i == localIndex )
                continue;

            const auto& ps = snap.players[i];
            if ( !ps.alive || !ps.bones_valid )
                continue;
            if ( ps.team == localTeam )
                continue;
            if ( ps.dormant )
                continue;
            // PlayerList: a hidden player gets no skeleton either.
            if ( PlayerList::GetVisualOverride( i ).hide )
                continue;

            const int numBones = ps.num_bones;
            if ( numBones <= 0 || numBones > kMaxBones )
                continue;

            // Project each used bone once. DirectWorldToScreen returns false
            // for behind-camera points; such bones are simply skipped below.
            Vec2 pos[kMaxBones];
            bool onscreen[kMaxBones];
            bool any = false;
            for ( int b = 0; b < numBones; ++b )
            {
                onscreen[b] = DirectWorldToScreen( ps.world_bones[b], pos[b] );
                any = any || onscreen[b];
            }
            if ( !any )
                continue;

            for ( int b = 0; b < numBones; ++b )
            {
                if ( !ps.bone_hitbox[b] )
                    continue;
                const int parent = ps.bone_parent[b];
                if ( parent < 0 || parent >= numBones )
                    continue;
                if ( !onscreen[b] || !onscreen[parent] )
                    continue;

                const Vec2& a = pos[b];
                const Vec2& c = pos[parent];
                const float dx = a.x - c.x;
                const float dy = a.y - c.y;
                if ( !std::isfinite( dx ) || !std::isfinite( dy ) ||
                     dx * dx + dy * dy < 1.f )
                    continue;

                DirectLine( renderer, a.x, a.y, c.x, c.y, 1, 255, 255, 255, 235 );
            }
        }
    }

    // ------------------------------------------------------------------
    //  Off-screen arrows
    //
    //  Resolved here, from live entity data, instead of through the
    //  FrameStageNotify projection cache (Visuals::ProjectFrame -> g_screen
    //  -> GetOffscreenArrowDirections). That cache is fed by
    //  IBaseClientDLL::FrameStageNotify, and on this build it stays empty:
    //  the box/name ESP does its own projection inside DrawDirectPlayerBoxes
    //  and is therefore unaffected, which is exactly why boxes rendered
    //  while no arrow ever appeared. Arrows now read the same live entity
    //  data as the boxes, so both features share one confirmed source.
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    //  Off-screen marker shapes. Indices match esp.offscreen_arrow_style and
    //  the kArrowStyleNames combo in Menu.cpp.
    // ------------------------------------------------------------------
    enum
    {
        kArrowTriangle = 0,
        kArrowChevron  = 1,
        kArrowRadarPip = 2,
        kArrowArrow    = 3,
        kArrowStyleCount
    };

    static void DrawArrowMarker( const DirectVGuiRenderer& renderer,
                                 int style,
                                 float centerX, float centerY, float radius,
                                 float dirX, float dirY, float size,
                                 int red, int green, int blue, int alpha,
                                 bool outline )
    {
        if ( alpha <= 0 || size <= 0.f )
            return;

        const float perpX = -dirY;
        const float perpY = dirX;

        // Head geometry: tip sits on the marker ring, the base is `headLen`
        // back along the direction and spans `halfW` to each side.
        const float headLen = 15.f * size;
        const float halfW   = 7.5f * size;
        const int   shadowA = alpha * 2 / 5;

        const float tipX  = centerX + dirX * radius;
        const float tipY  = centerY + dirY * radius;
        const float baseX = tipX - dirX * headLen;
        const float baseY = tipY - dirY * headLen;

        auto headTriangle = [&]( float grow, int cr, int cg, int cb, int ca )
        {
            const float hl = headLen + grow;
            const float hw = halfW + grow;
            const float bx = tipX - dirX * hl;
            const float by = tipY - dirY * hl;
            DirectFilledTriangle( renderer,
                                  tipX + dirX * grow, tipY + dirY * grow,
                                  bx + perpX * hw,    by + perpY * hw,
                                  bx - perpX * hw,    by - perpY * hw,
                                  cr, cg, cb, ca );
        };

        switch ( style )
        {
        case kArrowTriangle:
        {
            if ( outline ) headTriangle( 2.f, 1, 1, 1, shadowA );
            headTriangle( 0.f, red, green, blue, alpha );
            return;
        }

        case kArrowChevron:
        {
            // Two stacked V shapes, the classic "turn this way" marker.
            const int core = static_cast<int>( 2.f * size + 0.5f ) + 1;

            auto chevron = [&]( float dist, int thick,
                                int cr, int cg, int cb, int ca )
            {
                const float tx = centerX + dirX * dist;
                const float ty = centerY + dirY * dist;
                const float back = headLen * 0.72f;
                DirectLine( renderer,
                            tx - dirX * back + perpX * halfW,
                            ty - dirY * back + perpY * halfW,
                            tx, ty, thick, cr, cg, cb, ca );
                DirectLine( renderer,
                            tx - dirX * back - perpX * halfW,
                            ty - dirY * back - perpY * halfW,
                            tx, ty, thick, cr, cg, cb, ca );
            };

            const float gap = headLen * 0.78f;
            if ( outline )
            {
                chevron( radius,        core + 2, 1, 1, 1, shadowA );
                chevron( radius - gap,  core + 2, 1, 1, 1, shadowA );
            }
            chevron( radius,       core, red, green, blue, alpha );
            chevron( radius - gap, core, red, green, blue, alpha );
            return;
        }

        case kArrowRadarPip:
        {
            // A dot on the ring plus a pointer head — reads like a radar
            // contact with a bearing.
            const float dotR   = 3.f * size + 1.f;
            const float dotAt  = radius - headLen * 0.9f;
            const float dotX   = centerX + dirX * dotAt;
            const float dotY   = centerY + dirY * dotAt;

            if ( outline )
            {
                DirectFilledCircle( renderer, dotX, dotY, dotR + 2.f,
                                    1, 1, 1, shadowA );
                headTriangle( 2.f, 1, 1, 1, shadowA );
            }
            DirectFilledCircle( renderer, dotX, dotY, dotR,
                                red, green, blue, alpha );
            headTriangle( 0.f, red, green, blue, alpha );
            return;
        }

        case kArrowArrow:
        default:
        {
            // Full arrow: tapered head on a straight shaft.
            const float shaftLen = headLen * 1.15f;
            const float shaftX   = baseX - dirX * shaftLen;
            const float shaftY   = baseY - dirY * shaftLen;
            const int   shaftT   = static_cast<int>( 3.f * size + 0.5f ) + 1;

            if ( outline )
            {
                DirectLine( renderer, shaftX, shaftY, baseX, baseY,
                            shaftT + 2, 1, 1, 1, shadowA );
                headTriangle( 2.f, 1, 1, 1, shadowA );
            }
            DirectLine( renderer, shaftX, shaftY, baseX, baseY,
                        shaftT, red, green, blue, alpha );
            headTriangle( 0.f, red, green, blue, alpha );
            return;
        }
        }
    }

    static bool DirectWorldToScreenNdc( const Vec3& world, Vec2& screen, Vec3& ndc )
    {
        if ( !SDK::g_InternalW2S || !g_pD3Device )
            return false;
        if ( !std::isfinite( world.x ) || !std::isfinite( world.y ) ||
             !std::isfinite( world.z ) )
            return false;

        D3DVIEWPORT9 vp{};
        if ( FAILED( g_pD3Device->GetViewport( &vp ) ) || !vp.Width || !vp.Height )
            return false;

        // Non-zero return means the point was rejected (behind the camera).
        if ( SDK::g_InternalW2S( nullptr, world, ndc ) )
            return false;
        if ( !std::isfinite( ndc.x ) || !std::isfinite( ndc.y ) )
            return false;

        screen.x = ( 1.0f + ndc.x ) * static_cast<float>( vp.Width ) * 0.5f;
        screen.y = ( 1.0f - ndc.y ) * static_cast<float>( vp.Height ) * 0.5f;
        return std::isfinite( screen.x ) && std::isfinite( screen.y );
    }

    static void DrawDirectOffscreenArrows( const DirectVGuiRenderer& renderer )
    {
        using namespace SDK;
        const auto& esp = g_config.settings.visuals.esp;

        if ( !esp.esp || !esp.offscreen_arrows || !g_pD3Device ||
             !g_pEngine || !g_pEntityList ||
             !g_pEngine->IsInGame() || !g_pEngine->IsConnected() )
            return;

        D3DVIEWPORT9 viewport{};
        if ( FAILED( g_pD3Device->GetViewport( &viewport ) ) || !viewport.Width || !viewport.Height )
            return;

        const int localIndex = g_pEngine->GetLocalPlayer();
        auto* local = reinterpret_cast<C_BaseEntity*>(
            g_pEntityList->GetClientEntity( localIndex ) );
        if ( !local )
            return;
        const int localTeam = local->GetTeamNum();
        const Vec3 eye = local->EyePosition();

        // Real view angles. EntitySnapshot::cameraYaw/Pitch are captured in
        // CreateMove BEFORE aimbot/anti-aim rewrite the command, so the arrows
        // point at where the camera actually looks instead of spinning with
        // anti-aim.
        float yaw = 0.f;
        float pitch = 0.f;
        {
            std::lock_guard<std::mutex> lock( g_snapshotMutex );
            yaw = g_snapshotData.cameraYaw;
            pitch = g_snapshotData.cameraPitch;
        }
        if ( !std::isfinite( yaw ) || !std::isfinite( pitch ) )
            return;

        const float yawRad   = yaw * ( 3.14159265f / 180.f );
        const float pitchRad = pitch * ( 3.14159265f / 180.f );
        const float cosYaw   = cosf( yawRad );
        const float sinYaw   = sinf( yawRad );
        const float cosPitch = cosf( pitchRad );
        const float sinPitch = sinf( pitchRad );

        // 3D camera basis vectors (Source AngleVectors, zero roll):
        //   forward = ( cosYaw*cosPitch,  sinYaw*cosPitch, -sinPitch )
        //   right   = ( sinYaw,          -cosYaw,           0        )
        //   up      = ( sinPitch*cosYaw,  sinPitch*sinYaw,  cosPitch )
        // The z-component of `up` matters: it carries the vertical (dz) term so
        // the bearing is correct when the camera pitches or the target is above
        // / below the view direction. right/up here are the un-mirrored basis.
        const float camRightX =  sinYaw;
        const float camRightY = -cosYaw;
        // camRightZ = 0 (right sits in the horizontal plane)
        const float camUpX = sinPitch * cosYaw;
        const float camUpY = sinPitch * sinYaw;
        const float camUpZ = cosPitch;

        const int maxClients = g_pEngine->GetMaxClients() < 64
            ? g_pEngine->GetMaxClients() : 64;

        const float centerX = static_cast<float>( viewport.Width ) * 0.5f;
        const float centerY = static_cast<float>( viewport.Height ) * 0.5f;
        const float shorter = viewport.Width < viewport.Height ?
            static_cast<float>( viewport.Width ) : static_cast<float>( viewport.Height );

        // Style/scale are clamped here rather than trusted from the config
        // file — a hand-edited .cfg must never produce a broken marker.
        const int style = ( esp.offscreen_arrow_style >= 0 &&
                            esp.offscreen_arrow_style < kArrowStyleCount )
            ? esp.offscreen_arrow_style : kArrowTriangle;
        const float scale = ( std::isfinite( esp.offscreen_arrow_scale ) &&
                              esp.offscreen_arrow_scale >= 0.5f &&
                              esp.offscreen_arrow_scale <= 2.f )
            ? esp.offscreen_arrow_scale : 1.f;

        const float radius = shorter * 0.42f * ( 0.85f + 0.15f * scale );

        // Match the box pass: while a projection stays inside this margin the
        // player still gets a (clipped) box, so no arrow is wanted for him.
        const float marginX = static_cast<float>( viewport.Width ) * 0.25f;
        const float marginY = static_cast<float>( viewport.Height ) * 0.25f;

        const auto& color = esp.offscreen_arrow_color;
        const int red = ClampByte( color.r );
        const int green = ClampByte( color.g );
        const int blue = ClampByte( color.b );
        const int alpha = ClampByte( color.a );
        const bool outline = esp.offscreen_arrow_outline;

        for ( int i = 1; i <= maxClients; ++i )
        {
            if ( i == localIndex )
                continue;

            // Already drawn with a box/name/health ESP this frame.
            if ( s_directPlayerDrawn[i] )
                continue;

            auto* entity = reinterpret_cast<C_BaseEntity*>(
                g_pEntityList->GetClientEntity( i ) );
            if ( !entity )
                continue;
            if ( entity->GetTeamNum() == localTeam )
                continue;

            const bool isDormant = *(bool*)( reinterpret_cast<uintptr_t>( entity ) + 0x1F6 );
            if ( isDormant || entity->GetHealth() <= 0 || entity->GetLifeState() != 0 )
                continue;

            // PlayerList: a hidden player gets no marker on any ESP path.
            if ( PlayerList::GetVisualOverride( i ).hide )
                continue;

            const Vec3 origin = entity->GetAbsOrigin();
            Vec2 screen{};
            Vec3 ndc{};
            bool inFront = DirectWorldToScreenNdc( origin, screen, ndc );

            // ScreenTransform rejects what is behind the camera, but it can
            // also hand back scaled-up NDC for points straddling the near
            // plane, so the magnitude is checked alongside the return code.
            if ( inFront &&
                 ( fabsf( ndc.x ) > 2.f || fabsf( ndc.y ) > 2.f ) )
                inFront = false;

            float dirX = 0.f;
            float dirY = 0.f;

            if ( inFront )
            {
                // In front of the camera: the projected point already carries
                // the answer, and no camera basis is needed.
                if ( screen.x >= -marginX &&
                     screen.x <= static_cast<float>( viewport.Width ) + marginX &&
                     screen.y >= -marginY &&
                     screen.y <= static_cast<float>( viewport.Height ) + marginY )
                    continue;

                dirX = screen.x - centerX;
                dirY = screen.y - centerY;
                const float length = sqrtf( dirX * dirX + dirY * dirY );
                if ( !std::isfinite( length ) || length < 1.f )
                    continue;
                dirX /= length;
                dirY /= length;
            }
            else
            {
                // Behind the camera: no projection exists at all, so build
                // the bearing from the 3D camera basis. Project the world-space
                // direction onto the camera's right and up vectors (the screen
                // plane), then normalize. The `dz * camUpZ` term carries the
                // vertical offset, so pitch is handled and arrows point up when
                // the target is above the camera's view direction. Screen-space
                // Y grows downward (a positive ndc.y is "up"), so the up axis is
                // negated for the screen-space direction.
                const float dx = origin.x - eye.x;
                const float dy = origin.y - eye.y;
                const float dz = origin.z - eye.z;
                const float right = dx * camRightX + dy * camRightY;
                const float up    = dx * camUpX + dy * camUpY + dz * camUpZ;
                const float length = sqrtf( right * right + up * up );
                if ( !std::isfinite( length ) || length < 1.f )
                    continue;
                dirX =  right / length;
                dirY = -up / length;
            }

            if ( !std::isfinite( dirX ) || !std::isfinite( dirY ) ||
                 fabsf( dirX ) + fabsf( dirY ) < 0.01f )
                continue;

            DrawArrowMarker( renderer, style, centerX, centerY, radius,
                             dirX, dirY, scale,
                             red, green, blue, alpha, outline );
        }
    }


    // Robust model path for world entities. C_BaseEntity::GetModel (entity
    // vtable slot 8) returns garbage for non-player entities on CSSO, so go
    // through IClientRenderable (entity + 8) and validate the result.
    static bool IsSaneModelPath( const char* p )
    {
        if ( !p ) return false;
        for ( int i = 0; i < 7; ++i )
            if ( !p[i] ) return false;
        return _strnicmp( p, "models", 6 ) == 0;
    }

    static const char* SehModelName( void* renderable, int slot )
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>( renderable );
            using Fn = SDK::model_t* ( __fastcall* )( void* );
            SDK::model_t* m = reinterpret_cast<Fn>( vt[slot] )( renderable );
            if ( !m ) return nullptr;
            const char* name = SDK::g_pModelInfo->GetModelName( m );
            return IsSaneModelPath( name ) ? name : nullptr;
        }
        __except ( 1 ) { return nullptr; }
    }

    static const char* SehWeaponClassname( void* ent )
    {
        __try
        {
            const char* c = reinterpret_cast<const char*>(
                reinterpret_cast<uintptr_t>( ent ) + SDK::nWeaponClassnameOffset );
            return _strnicmp( c, "weapon_", 7 ) == 0 ? c : nullptr;
        }
        __except ( 1 ) { return nullptr; }
    }


    static bool SehIsDormant( void* ent )
    {
        __try
        {
            void* net = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( ent ) + 0x10 );
            void** vt = *reinterpret_cast<void***>( net );
            using Fn = bool ( __fastcall* )( void* );
            return reinterpret_cast<Fn>( vt[8] )( net ); // IClientNetworkable::IsDormant
        }
        __except ( 1 ) { return false; }
    }

    // "AK47", "AWP", "GLOCK18"... from classname, else from model filename
    // (w_rif_ak47.mdl -> AK47).
    static const char* NiceWorldWeaponLabel( SDK::C_BaseEntity* ent, const char* modelPath )
    {
        static char out[48];
        const char* src = nullptr;
        char tmp[96] = {};
        if ( const char* cls = SehWeaponClassname( ent ) )
            src = cls + 7;
        else if ( modelPath )
        {
            const char* f = std::strrchr( modelPath, '/' );
            f = f ? f + 1 : modelPath;
            strncpy_s( tmp, f, _TRUNCATE );
            if ( char* dot = std::strrchr( tmp, '.' ) ) *dot = 0;
            char* t = tmp;
            if ( _strnicmp( t, "w_", 2 ) == 0 ) t += 2;
            const char* pre[] = { "rif_", "snip_", "pist_", "smg_", "shot_", "mach_", "eq_", "knife_" };
            for ( const char* pr : pre )
                if ( _strnicmp( t, pr, std::strlen( pr ) ) == 0 ) { t += std::strlen( pr ); break; }
            const char* suf[] = { "_dropped", "_thrown" };
            for ( const char* sf : suf )
                if ( char* x = std::strstr( t, sf ) ) *x = 0;
            src = t;
        }
        if ( !src || !src[0] )
            return "WEAPON";
        size_t i = 0;
        for ( ; src[i] && i < sizeof( out ) - 1; ++i )
            out[i] = static_cast<char>( toupper( static_cast<unsigned char>( src[i] ) ) );
        out[i] = 0;
        return out;
    }
    static const char* WorldEntityModelPath( SDK::C_BaseEntity* entity )
    {
        static int s_slot = -1; // learned once a sane name is found
        void* renderable = reinterpret_cast<void*>( reinterpret_cast<uintptr_t>( entity ) + 8 );
        if ( s_slot >= 0 )
            return SehModelName( renderable, s_slot );
        const int slots[] = { 8, 9, 10 };
        for ( int slot : slots )
            if ( const char* n = SehModelName( renderable, slot ) )
            {
                s_slot = slot;
                return n;
            }
        return nullptr;
    }

    const char* GetWorldModelPath( void* entity )
    {
        return entity ? WorldEntityModelPath( reinterpret_cast<SDK::C_BaseEntity*>( entity ) ) : nullptr;
    }

    bool IsEntityDormant( void* entity )
    {
        return entity ? SehIsDormant( entity ) : true;
    }

    struct DroppedWeaponBox { int left = 0, top = 0, right = 0, bottom = 0; };

    static bool ReadDroppedWorldBounds(void* entity, Vec3& mins, Vec3& maxs)
    {
        if (!entity || !SDK::g_uClientBase) return false;
        // Matching client.dll: renderable +8, slot22 = 0x11A250. It forwards
        // to 0x16F800/0x16EC40, which calls [vtable+0xA8] (slot21) for
        // local model bounds and returns world bounds. Slot22 is confirmed by
        // the [vtable+0xB0] fallback in 0x16EC40; never infer the table start
        // from SetupBones. Do not call an unverified foreign-build override.
        __try
        {
            void* renderable = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entity)+8);
            void** vt = *reinterpret_cast<void***>(renderable);
            if (!vt || reinterpret_cast<uintptr_t>(vt[22]) != SDK::g_uClientBase+0x11A250) return false;
            using BoundsFn = void(__fastcall*)(void*, Vec3&, Vec3&);
            reinterpret_cast<BoundsFn>(vt[22])(renderable, mins, maxs);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    template<class Project>
    static bool ProjectDroppedWorldBounds(const Vec3& mins, const Vec3& maxs,
                                           Project project, DroppedWeaponBox& box)
    {
        const float lo[3] = {mins.x,mins.y,mins.z};
        const float hi[3] = {maxs.x,maxs.y,maxs.z};
        float extent = 0.f;
        for (int i=0;i<3;++i)
        {
            if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) ||
                fabsf(lo[i])>1e7f || fabsf(hi[i])>1e7f ||
                hi[i]<lo[i] || hi[i]-lo[i]>1024.f) return false;
            extent += hi[i]-lo[i];
        }
        if (extent<.01f) return false;
        float left=1e30f, top=1e30f, right=-1e30f, bottom=-1e30f;
        for(int i=0;i<8;++i)
        {
            const Vec3 world{(i&1)?maxs.x:mins.x, (i&2)?maxs.y:mins.y, (i&4)?maxs.z:mins.z};
            Vec2 screen{};
            // A box crossing the camera plane is omitted, not expanded into
            // a huge rectangle. Keep its existing name label as the fallback.
            if (!project(world,screen) || !std::isfinite(screen.x) ||
                !std::isfinite(screen.y) || fabsf(screen.x)>100000.f ||
                fabsf(screen.y)>100000.f) return false;
            left=(std::min)(left,screen.x); right=(std::max)(right,screen.x);
            top=(std::min)(top,screen.y); bottom=(std::max)(bottom,screen.y);
        }
        box={static_cast<int>(floorf(left))-2,static_cast<int>(floorf(top))-2,
             static_cast<int>(ceilf(right))+2,static_cast<int>(ceilf(bottom))+2};
        return box.right>box.left && box.bottom>box.top;
    }

    static void DrawDroppedWeaponBox(const DirectVGuiRenderer& renderer,
                                      const DroppedWeaponBox& box,
                                      int red, int green, int blue)
    {
        DirectOutline(renderer,box.left-1,box.top-1,box.right+1,box.bottom+1,0,0,0,185);
        DirectOutline(renderer,box.left,box.top,box.right,box.bottom,red,green,blue,235);
    }

    // World ESP is intentionally based on base-entity model paths, not on a
    // guessed CClientClass slot or a weapon-only classname field. Entity list,
    // GetModel and IVModelInfoClient::GetModelName already exist in this SDK.
    static int DrawDirectWorldItems( const DirectVGuiRenderer& renderer )
    {
        using namespace SDK;
        const auto& esp = g_config.settings.visuals.esp;
        static bool s_reportedWorldDisabled = false;
        static bool s_reportedModelInfoMissing = false;
        if ( !esp.weapons )
        {
            if ( !s_reportedWorldDisabled )
            {
                s_reportedWorldDisabled = true;
                DBGLOG( "[world_esp] disabled: enable esp.weapons\n" );
            }
            return 0;
        }
        if ( !g_pEngine || !g_pEntityList || !g_pEngine->IsInGame() ||
             !g_pEngine->IsConnected() )
            return 0;
        if ( !g_pModelInfo )
        {
            if ( !s_reportedModelInfoMissing )
            {
                s_reportedModelInfoMissing = true;
                DBGLOG( "[world_esp] IVModelInfoClient unavailable\n" );
            }
            return 0;
        }

        // Small servers may allocate dropped weapons below index65. Skip
        // only the actual player slots, not a hard-coded 64-player range.
        const int kFirstWorldEntity = std::clamp(g_pEngine->GetMaxClients(), 0, 64)+1;
        constexpr int kLastWorldEntity = 2048;
        const SurfaceFont* itemFont = ActiveItemFont();
        int entityCount = 0;
        int modelCount = 0;
        int matchCount = 0;
        int projectedCount = 0;
        int heldCount = 0;
        static char s_sampleWeaponModel[128] = "";
        static char s_uniq[16][96] = {};
        static int  s_uniqCount = 0;
        static int s_worldModelSamples = 0;
        int submitted = 0;
        for ( int i = kFirstWorldEntity; i <= kLastWorldEntity; ++i )
        {
            auto* entity = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( i ) );
            if ( !entity )
                continue;
            ++entityCount;

            const char* modelPath = WorldEntityModelPath( entity );
            // Fallback for weapons: build a path-like label from classname.
            static thread_local char s_clsPath[96];
            if ( !modelPath )
            {
                if ( const char* cls = SehWeaponClassname( entity ) )
                {
                    sprintf_s( s_clsPath, "models/weapons/w_%s", cls + 7 );
                    modelPath = s_clsPath;
                }
            }
            if ( modelPath && modelPath[0] )
            {
                ++modelCount;
                if ( std::strstr( modelPath, "weapon" ) || std::strstr( modelPath, "w_" ) )
                    strncpy_s( s_sampleWeaponModel, modelPath, _TRUNCATE );
                if ( modelPath[0] != '*' )
                {
                    bool seen = false;
                    for ( int k = 0; k < s_uniqCount; ++k )
                        if ( std::strcmp( s_uniq[k], modelPath ) == 0 ) { seen = true; break; }
                    if ( !seen && s_uniqCount < 16 )
                        strncpy_s( s_uniq[s_uniqCount++], modelPath, _TRUNCATE );
                }
                if ( s_worldModelSamples < 12 && std::strstr( modelPath, "weapon" ) )
                {
                    DBGLOG( "[world_esp] sample slot=%d model=%s\n", i, modelPath );
                    ++s_worldModelSamples;
                }
            }
            const SurfaceItemKind kind = ClassifyWorldModel( modelPath );
            if ( kind == SurfaceItemKind::None )
                continue;
            ++matchCount;

            // Skip view models. Weapon entities: show only when on the ground
            // (m_hOwner invalid / m_iState == 0). Thrown grenade projectiles
            // ("_thrown" models) are not weapons and are always shown.
            if ( std::strstr( modelPath, "/v_" ) )
                continue;
            const Vec3 itemPos = entity->GetAbsOrigin();
            {
                bool nearPlayer = false;
                const int maxCl = g_pEngine->GetMaxClients();
                for ( int pi = 1; pi <= maxCl && pi <= 64; ++pi )
                {
                    auto* pl = reinterpret_cast<C_BaseEntity*>( g_pEntityList->GetClientEntity( pi ) );
                    if ( !pl || pl->GetLifeState() != 0 || pl->GetHealth() <= 0 )
                        continue;
                    const Vec3 o = pl->GetAbsOrigin();
                    const float dx = itemPos.x - o.x, dy = itemPos.y - o.y;
                    if ( dx * dx + dy * dy < 20.f * 20.f &&
                         itemPos.z > o.z + 20.f && itemPos.z < o.z + 75.f )
                    { nearPlayer = true; break; }
                }
                if ( nearPlayer && !std::strstr( modelPath, "thrown" ) )
                    continue;
            }
            // Phantoms: dormant (out of PVS, stale position) or parked at origin.
            if ( SehIsDormant( entity ) ||
                 ( fabsf( itemPos.x ) < 1.f && fabsf( itemPos.y ) < 1.f && fabsf( itemPos.z ) < 1.f ) )
                continue;
            const bool isDefuser = std::strstr( modelPath, "defuser" ) != nullptr;
            if ( !isDefuser && !std::strstr( modelPath, "thrown" ) && !std::strstr( modelPath, "projectile" ) )
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>( entity );
                const unsigned int owner = *reinterpret_cast<unsigned int*>( base + nWeaponOwnerOffset );
                const int state = *reinterpret_cast<int*>( base + nWeaponStateOffset );
                const bool hasOwner = owner != 0xFFFFFFFFu &&
                    g_pEntityList->GetClientEntity( owner & 0xFFF ) != nullptr;
                // Hidden only when BOTH say "carried" (robust to one bad field).
                if ( hasOwner || state != 0 )
                {
                    ++heldCount;
                    continue;
                }
            }

            Vec2 screen{};
            if ( !DirectWorldToScreen( itemPos, screen ) )
                continue;
            ++projectedCount;

            int red = 220, green = 220, blue = 220;
            if ( isDefuser )
            {
                red = 90; green = 160; blue = 255;
            }
            else if ( kind == SurfaceItemKind::C4 )
            {
                red = 235; green = 75; blue = 75;
            }
            else if ( kind == SurfaceItemKind::HeGrenade || kind == SurfaceItemKind::Molotov )
            {
                red = 245; green = 150; blue = 55;
            }
            else if ( kind == SurfaceItemKind::Flashbang )
            {
                red = 235; green = 225; blue = 115;
            }
            else if ( kind == SurfaceItemKind::Smoke )
            {
                red = 135; green = 185; blue = 185;
            }

            int centerX = static_cast<int>( screen.x );
            int iconY = static_cast<int>( screen.y ) - 5;
            if (esp.weapon_boxes && kind == SurfaceItemKind::Weapon && !isDefuser)
            {
                Vec3 mins{}, maxs{};
                DroppedWeaponBox box{};
                if (ReadDroppedWorldBounds(entity,mins,maxs) &&
                    ProjectDroppedWorldBounds(mins,maxs,DirectWorldToScreen,box))
                {
                    DrawDroppedWeaponBox(renderer,box,red,green,blue);
                    centerX = box.left + (box.right-box.left)/2;
                    iconY = box.bottom+4;
                }
            }
            const char* label = kind == SurfaceItemKind::Weapon
                ? ( isDefuser ? "DEFUSE KIT" : NiceWorldWeaponLabel( entity, modelPath ) ) : SurfaceItemLabel( kind );
            DirectTextCenteredFont( renderer, itemFont, centerX, iconY,
                                    red, green, blue, 235, label );
            ++submitted;
        }
        static unsigned int s_worldReportTicks = 0;
        if ( ( ++s_worldReportTicks % 180u ) == 1u )
            DBGLOG( "[world_esp] entities=%d models=%d matched=%d projected=%d drawn=%d\n",
                         entityCount, modelCount, matchCount, projectedCount, submitted );
        return submitted;
    }

    struct SurfaceWarningVisual
    {
        unsigned long long identity=0;
        unsigned frame=0;
        bool ready=false, offscreen=false;
        float seconds=-1.f;
    };
    static SurfaceWarningVisual s_surfaceWarnings[16];
    static unsigned s_surfaceWarningFrame=0;

    static SurfaceWarningVisual& SurfaceWarningFor(unsigned long long identity)
    {
        auto* oldest=&s_surfaceWarnings[0];
        for(auto& state:s_surfaceWarnings)
        {
            if(state.identity==identity) return state;
            if(!state.identity || state.frame<oldest->frame) oldest=&state;
        }
        *oldest={};oldest->identity=identity;return *oldest;
    }

    static Vec2 SeparateSurfaceWarnings(Vec2 anchor,Vec2 tangent,float width,float height,
                                         const Vec2* placed,int count)
    {
        const auto free=[&](Vec2 point){
            for(int i=0;i<count;++i){const float x=point.x-placed[i].x,y=point.y-placed[i].y;
                if(x*x+y*y<58.f*58.f)return false;}return true;
        };
        if(free(anchor))return anchor;
        for(int i=0;i<16;++i)
        {
            const float shift=64.f*(i/2+1)*(i%2?-1.f:1.f);
            const Vec2 point{(std::clamp)(anchor.x+tangent.x*shift,54.f,width-54.f),
                             (std::clamp)(anchor.y+tangent.y*shift,54.f,height-54.f)};
            if(free(point))return point;
        }
        Vec2 best{-1.f,-1.f};float bestDistance=1e30f;
        for(float y=54.f;y<=height-54.f;y+=64.f)
            for(float x=54.f;x<=width-54.f;x+=64.f)
            {
                const Vec2 point{x,y};if(!free(point))continue;
                const float dx=x-anchor.x,dy=y-anchor.y,distance=dx*dx+dy*dy;
                if(distance<bestDistance){bestDistance=distance;best=point;}
            }
        return best;
    }

    static Vec2 SurfaceGrenadeDirection(const Vec3& point,const SDK::VMatrix* matrix,
                                          float width,float height,Vec2 fallback)
    {
        if(!matrix)return fallback;
        const auto& m=*matrix;
        const float x=(m[0][0]*point.x+m[0][1]*point.y+m[0][2]*point.z+m[0][3])*width;
        const float y=-(m[1][0]*point.x+m[1][1]*point.y+m[1][2]*point.z+m[1][3])*height;
        const float length=sqrtf(x*x+y*y);
        if(!std::isfinite(length)||length<.0001f)return fallback;
        return {x/length,y/length};
    }

    static void DirectSurfaceArc(const DirectVGuiRenderer& renderer,Vec2 center,
                                   float radius,float fraction,int red,int green,int blue,int alpha)
    {
        if(!std::isfinite(fraction)||fraction<=0.f||!std::isfinite(radius)||radius<=0.f||radius>256.f||
           !std::isfinite(center.x)||!std::isfinite(center.y)||fabsf(center.x)>100000.f||fabsf(center.y)>100000.f)return;
        fraction=(std::min)(fraction,1.f);
        // Antialiased two-pixel ring, using only the already verified Surface
        // filled-rectangle/color methods. Batch equal-coverage pixels into runs.
        const int left=static_cast<int>(floorf(center.x-radius-2.f));
        const int right=static_cast<int>(ceilf(center.x+radius+2.f));
        const int top=static_cast<int>(floorf(center.y-radius-2.f));
        const int bottom=static_cast<int>(ceilf(center.y+radius+2.f));
        const float end=6.283185307f*fraction;
        for(int y=top;y<=bottom;++y)
        {
            int runAlpha=0,runStart=left;
            for(int x=left;x<=right+1;++x)
            {
                int coverageAlpha=0;
                if(x<=right)
                {
                    const float dx=x+.5f-center.x,dy=y+.5f-center.y;
                    const float coverage=(std::clamp)(1.5f-fabsf(sqrtf(dx*dx+dy*dy)-radius),0.f,1.f);
                    if(coverage>0.f)
                    {
                        float angle=atan2f(dy,dx)+1.570796327f;
                        if(angle<0.f)angle+=6.283185307f;
                        if(fraction>=1.f||angle<=end)coverageAlpha=static_cast<int>(alpha*coverage+.5f);
                    }
                }
                if(coverageAlpha!=runAlpha)
                {
                    if(runAlpha>0)DirectFill(renderer,runStart,y,x,y+1,red,green,blue,runAlpha);
                    runAlpha=coverageAlpha;runStart=x;
                }
            }
        }
    }

    static const SurfaceFont* SurfaceWarningTextFont()
    {
        const auto& font=s_itemFonts[(std::clamp)(10,kItemFontMin,kItemFontMax)-kItemFontMin];
        return font.ready ? &font : ActiveItemFont();
    }

    static void DrawSurfaceWarningBadge(const DirectVGuiRenderer& renderer,
                                         const Grenades::SurfaceGrenadeWarning& warning,
                                         Vec2 center,bool timers,float seconds)
    {
        int red=213,green=216,blue=215;
        if(warning.danger){red=239;green=83;blue=90;}
        else if(warning.kind==3){red=231;green=174;blue=92;}
        const bool timer=timers && seconds>=0.f;
        DirectFilledCircle(renderer,center.x,center.y+2.f,22.f,0,0,0,75);
        DirectFilledCircle(renderer,center.x,center.y,20.f,7,8,10,235);
        DirectFilledCircle(renderer,center.x,center.y,18.f,20,21,23,245);
        DirectSurfaceArc(renderer,center,18.f,1.f,timer?63:red,timer?65:green,timer?69:blue,timer?170:255);
        if(timer)
        {
            const float total=(warning.kind==2||warning.kind==4)?3.f:(warning.kind==3?2.f:1.5f);
            DirectSurfaceArc(renderer,center,18.f,(std::clamp)(seconds/total,0.f,1.f),red,green,blue,255);
        }
        const char* weapon=nullptr;
        switch(warning.kind)
        {
        case 0:weapon="weapon_hegrenade";break;
        case 1:weapon="weapon_flashbang";break;
        case 2:weapon="weapon_smokegrenade";break;
        case 3:weapon="weapon_molotov";break;
        case 4:weapon="weapon_decoy";break;
        default:break;
        }
        const uint16_t glyph=weapon?SurfaceWeaponIconCodepoint(weapon):0;
        const int size=timer?21:24;
        const auto* icon=&s_iconFonts[size-kIconFontMin];
        if(!icon->ready)icon=ActiveIconFont();
        const auto* textFont=SurfaceWarningTextFont();
        if(icon&&glyph>=kWidthAsciiFirst&&glyph<=kWidthAsciiLast)
        {
            const int width=SurfaceGlyphWidth(*icon,static_cast<wchar_t>(glyph));
            const int height=icon->height>0?icon->height:icon->tall;
            DirectGlyphWithFont(renderer,icon->handle,static_cast<int>(center.x)-width/2,
                static_cast<int>(center.y)-height/2-(timer?4:0),237,238,239,255,glyph);
        }
        else DirectTextCenteredFont(renderer,textFont,static_cast<int>(center.x),
                 static_cast<int>(center.y)-(timer?12:7),237,238,239,255,"!");
        if(timer)
        {
            char text[24]{};sprintf_s(text,"~%.1f",seconds);
            DirectTextCenteredFont(renderer,textFont,static_cast<int>(center.x),
                                   static_cast<int>(center.y)+3,206,208,210,245,text);
        }
        if(warning.kind==0&&warning.damage>0)
        {
            char text[24]{};sprintf_s(text,"-%d HP",warning.damage);
            DirectTextCenteredFont(renderer,textFont,static_cast<int>(center.x),
                                   static_cast<int>(center.y)+25,red,green,blue,255,text);
        }
    }

    template<class Project>
    static void DrawSurfaceGrenadeFrame(const DirectVGuiRenderer& renderer,
                                         const Grenades::SurfaceGrenadeFrame& frame,
                                         float width,float height,float dt,
                                         const SDK::VMatrix* camera,Project project)
    {
        if(!std::isfinite(width)||!std::isfinite(height)||width<128.f||height<128.f)return;
        ++s_surfaceWarningFrame;
        const auto projectPoint=[&](const Vec3& point,Vec2& screen){
            return project(point,screen)&&std::isfinite(screen.x)&&std::isfinite(screen.y)&&
                   fabsf(screen.x)<100000.f&&fabsf(screen.y)<100000.f;
        };
        const auto path=[&](const Vec3* points,const bool* bounced,int count,
                            int red,int green,int blue,int alpha)
        {
            Vec2 previous{};bool previousOk=false;
            for(int i=0;i<count;++i)
            {
                Vec2 next{};const bool ok=projectPoint(points[i],next);
                if(previousOk&&ok)
                {
                    const float dx=next.x-previous.x,dy=next.y-previous.y;
                    if(dx*dx+dy*dy>.25f)
                        DirectLine(renderer,previous.x,previous.y,next.x,next.y,1,red,green,blue,alpha);
                }
                if(bounced&&bounced[i]&&ok)
                    DirectFilledCircle(renderer,next.x,next.y,2.5f,255,255,255,255);
                previous=next;previousOk=ok;
            }
        };
        const int trajectoryCount=(std::clamp)(frame.trajectoryCount,0,Grenades::kSurfaceTrajectoryPoints);
        if(frame.trajectoryEnabled&&trajectoryCount>=2)
        {
            path(frame.trajectory,frame.bounced,trajectoryCount,255,255,255,220);
            Vec2 end{};
            if(projectPoint(frame.trajectory[trajectoryCount-1],end)&&end.x>=0.f&&end.x<=width&&end.y>=0.f&&end.y<=height)
            {
                const int green=frame.detonates?135:255,blue=frame.detonates?70:255;
                DirectSurfaceArc(renderer,end,5.f,1.f,255,green,blue,245);
                char text[48]{};
                if(frame.detonates&&frame.trajectoryDamage>0)sprintf_s(text,"HE | %d DMG",frame.trajectoryDamage);
                else strcpy_s(text,frame.detonates?"DETONATE":"LAND");
                DirectTextFont(renderer,SurfaceWarningTextFont(),static_cast<int>(end.x)+8,
                               static_cast<int>(end.y)-8,255,green,blue,245,text);
            }
        }
        if(!frame.warningsEnabled)
        {for(auto& visual:s_surfaceWarnings)visual={};return;}
        Vec2 placed[Grenades::kSurfaceWarningMax]{};int placedCount=0;
        const int warningCount=(std::clamp)(frame.warningCount,0,Grenades::kSurfaceWarningMax);
        for(int i=0;i<warningCount;++i)
        {
            const auto& warning=frame.warnings[i];
            const int count=(std::clamp)(warning.count,0,Grenades::kSurfaceWarningPoints);
            if(count<2)continue;
            path(warning.points,nullptr,count,warning.danger?208:198,
                 warning.danger?114:200,warning.danger?119:202,warning.danger?115:100);
            auto& visual=SurfaceWarningFor(warning.identity?warning.identity:static_cast<unsigned long long>(i+1));
            const float margin=visual.ready?(visual.offscreen?68.f:44.f):54.f;
            Vec2 anchor{};
            const bool inFront=projectPoint(warning.points[count-1],anchor);
            const bool offscreen=!inFront||anchor.x<margin||anchor.x>width-margin||anchor.y<margin||anchor.y>height-margin;
            Vec2 direction=SurfaceGrenadeDirection(warning.points[count-1],camera,width,height,warning.edgeDirection);
            if(!std::isfinite(direction.x)||!std::isfinite(direction.y))direction={0.f,-1.f};
            if(offscreen)
            {
                const float sx=fabsf(direction.x)>.0001f?(width*.5f-54.f)/fabsf(direction.x):1e8f;
                const float sy=fabsf(direction.y)>.0001f?(height*.5f-54.f)/fabsf(direction.y):1e8f;
                const float scale=(std::min)(sx,sy);
                anchor={width*.5f+direction.x*scale,height*.5f+direction.y*scale};
            }
            const Vec2 tangent=offscreen?Vec2{-direction.y,direction.x}:Vec2{1.f,0.f};
            const Vec2 center=SeparateSurfaceWarnings(anchor,tangent,width,height,placed,placedCount);
            if(center.x<0.f)continue;
            placed[placedCount++]=center;
            if(offscreen)
            {
                const Vec2 side{-direction.y,direction.x};
                DirectFilledTriangle(renderer,center.x+direction.x*31.f,center.y+direction.y*31.f,
                    center.x+direction.x*24.f+side.x*4.f,center.y+direction.y*24.f+side.y*4.f,
                    center.x+direction.x*24.f-side.x*4.f,center.y+direction.y*24.f-side.y*4.f,
                    warning.danger?239:211,warning.danger?83:214,warning.danger?90:213,235);
            }
            else if(fabsf(center.x-anchor.x)>1.f||fabsf(center.y-anchor.y)>1.f)
                DirectLine(renderer,anchor.x,anchor.y,center.x,center.y,1,160,162,165,100);
            if(!visual.ready||s_surfaceWarningFrame-visual.frame>2||warning.seconds<0.f||
                visual.seconds<0.f||warning.seconds<=0.f)visual.seconds=warning.seconds;
            else visual.seconds+=(warning.seconds-visual.seconds)*(1.f-expf(-(std::clamp)(dt,0.f,.1f)/.04f));
            visual.ready=true;visual.offscreen=offscreen;visual.frame=s_surfaceWarningFrame;
            // No screen-position interpolation: a world point belongs to this camera/frame.
            DrawSurfaceWarningBadge(renderer,warning,center,frame.timers,visual.seconds);
        }
    }

    static void DrawDirectGrenades(const DirectVGuiRenderer& renderer)
    {
        if(!SDK::g_pEngine||!SDK::g_pEngine->IsInGame()||!SDK::g_pEngine->IsConnected()||!g_pD3Device)return;
        D3DVIEWPORT9 viewport{};
        if(FAILED(g_pD3Device->GetViewport(&viewport))||!viewport.Width||!viewport.Height)return;
        const auto now=std::chrono::steady_clock::now();
        static auto last=now;
        const float dt=std::chrono::duration<float>(now-last).count();last=now;
        Grenades::SurfaceGrenadeFrame frame{};Grenades::CopySurfaceGrenadeFrame(frame);
        SDK::VMatrix camera{};const bool validCamera=SDK::CopyNativeViewMatrix(camera);
        DrawSurfaceGrenadeFrame(renderer,frame,static_cast<float>(viewport.Width),
            static_cast<float>(viewport.Height),dt,validCamera?&camera:nullptr,DirectWorldToScreen);
    }

    static void __fastcall PaintTraverseDetour( void* thisptr,
                                                 uintptr_t panel,
                                                 bool forceRepaint,
                                                 bool allowForce )
    {
        ++t_paintDepth;

        // Resolve the confirmed panel identity before calling the original
        // traversal. No panel method is invoked after children may mutate it.
        const bool isGameHudPanel = t_paintDepth == 1u &&
                                    IsGameHudPanel( thisptr, panel );

        // Preserve the engine traversal before performing the Surface pass.
        const PaintTraverseFn original = oPaintTraverse;
        if ( original )
            original( thisptr, panel, forceRepaint, allowForce );

        s_callCount.fetch_add( 1u, std::memory_order_relaxed );
        s_live.store( true, std::memory_order_relaxed );

        // Submit once after the confirmed game HUD traversal. Console and
        // other top-level panels do not match MatSystemTopPanel.
        if ( isGameHudPanel && s_surface && ValidateSurface( s_surface ) )
        {
            auto** vtable = *reinterpret_cast<void***>( s_surface );
            DirectVGuiRenderer renderer{};
            renderer.surface = s_surface;
            renderer.setColor = reinterpret_cast<DrawSetColorFn>( vtable[kVt_DrawSetColor] );
            renderer.filledRect = reinterpret_cast<DrawFilledRectFn>( vtable[kVt_DrawFilledRect] );
            renderer.setTextFont = reinterpret_cast<SetTextFontFn>( vtable[kVt_SetTextFont] );
            renderer.setTextColor = reinterpret_cast<SetTextColorFn>( vtable[kVt_SetTextColor] );
            renderer.setTextPos = reinterpret_cast<SetTextPosFn>( vtable[kVt_SetTextPos] );
            renderer.printText = reinterpret_cast<PrintTextFn>( vtable[kVt_PrintText] );
            // Snapshot the same main-HUD camera used by the stable Surface ESP.
            SDK::PublishHudProjection();
            DrawDirectPlayerBoxes( renderer );
            DrawDirectSkeletons( renderer );
            DrawDirectOffscreenArrows( renderer );
            DrawDirectWorldItems( renderer );
            DrawDirectGrenades( renderer );
        }

        --t_paintDepth;
    }

    bool IsLive()
    {
        return s_live.load( std::memory_order_relaxed );
    }

    void PollStatus()
    {
        if ( s_live.load( std::memory_order_relaxed ) &&
             !s_reported.exchange( true, std::memory_order_relaxed ) )
        {
            DBGLOG( "[painttraverse] callback verified; calls=%u\n",
                         s_callCount.load( std::memory_order_relaxed ) );
        }
    }

    bool Init()
    {
        if ( s_hookTarget && oPaintTraverse )
            return true;

        s_vgui2 = GetModuleHandleA( "vgui2.dll" );
        if ( !s_vgui2 )
        {
            DBGLOG( "[painttraverse] vgui2.dll is not loaded\n" );
            return false;
        }

        const auto createInterface = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress( s_vgui2, "CreateInterface" ) );
        if ( !createInterface )
        {
            DBGLOG( "[painttraverse] vgui2 CreateInterface is missing\n" );
            return false;
        }

        void* ipanel = createInterface( "VGuiPanel010", nullptr );
        if ( !ipanel )
        {
            DBGLOG( "[painttraverse] VGuiPanel010 is unavailable\n" );
            return false;
        }

        auto** vtable = *reinterpret_cast<void***>( ipanel );
        if ( !vtable )
        {
            DBGLOG( "[painttraverse] IPanel vtable is null\n" );
            return false;
        }

        s_hookTarget = vtable[kVt_PaintTraverse];
        if ( !ValidatePaintTraverseTarget( s_hookTarget ) )
        {
            DBGLOG( "[painttraverse] target validation failed at %p\n", s_hookTarget );
            s_hookTarget = nullptr;
            return false;
        }

        // Surface resolution is intentionally outside the PaintTraverse call.
        // Failure disables only the marker; it does not prevent the validated
        // liveness hook from being installed.
        ResolveSurface();

        const MH_STATUS createStatus = MH_CreateHook(
            s_hookTarget, &PaintTraverseDetour, reinterpret_cast<void**>( &oPaintTraverse ) );
        if ( createStatus != MH_OK )
        {
            DBGLOG( "[painttraverse] MH_CreateHook failed: %d\n", static_cast<int>( createStatus ) );
            oPaintTraverse = nullptr;
            s_hookTarget = nullptr;
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook( s_hookTarget );
        if ( enableStatus != MH_OK )
        {
            DBGLOG( "[painttraverse] MH_EnableHook failed: %d\n", static_cast<int>( enableStatus ) );
            MH_RemoveHook( s_hookTarget );
            oPaintTraverse = nullptr;
            s_hookTarget = nullptr;
            return false;
        }

        DBGLOG( "[painttraverse] installed on IPanel::PaintTraverse vtable[%d]\n",
                     kVt_PaintTraverse );
        return true;
    }

    void Shutdown()
    {
        if ( s_hookTarget )
        {
            MH_DisableHook( s_hookTarget );
            MH_RemoveHook( s_hookTarget );
        }

        s_hookTarget = nullptr;
        oPaintTraverse = nullptr;
        s_surface = nullptr;
        s_vguiMatSurface = nullptr;
        s_vgui2 = nullptr;

        // Surface font handles belong to the unloading module's interface, so
        // the cache is invalidated before anything can draw with a stale entry.
        s_fontsReady = false;
        s_iconFontReady = false;
        for ( auto& font : s_iconFonts )
            font = SurfaceFont{};
        for ( auto& font : s_nameFonts )
            font = SurfaceFont{};
        for ( auto& font : s_itemFonts )
            font = SurfaceFont{};

        s_callCount.store( 0, std::memory_order_relaxed );
        s_live.store( false, std::memory_order_relaxed );
        s_reported.store( false, std::memory_order_relaxed );
        s_surfaceReported.store( false, std::memory_order_relaxed );
        t_paintDepth = 0;
    }
}
