#include "features/visuals/world/NightMode.hpp"
#include "sdk/EngineState.hpp"
#include "config/Config.hpp"
#include <vector>
#include <cstring>

namespace
{
    struct NightMaterialState
    {
        SDK::IMaterial* material = nullptr;
        float red = 1.f;
        float green = 1.f;
        float blue = 1.f;
        float alpha = 1.f;
        bool staticProp = false;
    };

    enum class NightWorkPhase
    {
        Idle,
        Scanning,
        Applying,
        Restoring,
        Active
    };

    struct NightTintSettings
    {
        int intensity = 0;
        int red = 255;
        int green = 255;
        int blue = 255;
        uint64_t key = 0;
    };

    // Keep the amount deliberately small: scans can touch ~2600 dictionary
    // entries, so all material work is spread across consecutive frames.
    static constexpr size_t kNightMaterialsPerFrame = 64u;
    static std::vector<NightMaterialState> s_nightMaterials;
    static NightWorkPhase s_nightPhase = NightWorkPhase::Idle;
    static size_t s_nightMaterialCursor = 0u;
    static uint32_t s_nightScanHandle = 0u;
    static uint16_t s_nightScanCount = 0u;
    static int s_nightEnumSlot = -1;
    static bool s_nightApplied = false;
    static bool s_nightWasInGame = false;
    static bool s_nightRejected = false;
    static uint64_t s_nightAppliedSettingsKey = 0;

    // A material can be created after the original scan (map streaming / late
    // static props). It may therefore be tinted by the engine state while not
    // appearing in our original-value cache. A one-shot fallback scan on disable
    // resets those late World/StaticProp materials to default modulation.
    static bool s_nightFallbackRestoreActive = false;
    static bool s_nightFallbackRestoreDone = false;
    static uint32_t s_nightFallbackHandle = 0u;
    static uint16_t s_nightFallbackCount = 0u;
    static int s_nightFallbackEnumSlot = -1;

    static bool IsNightModeGroup( const char* group )
    {
        if ( !group || !group[0] )
            return false;

        // These are Source material texture-group names. Do not tint models,
        // particle materials, UI, sky or viewmodels.
        return std::strstr( group, "World" ) != nullptr ||
               std::strstr( group, "StaticProp" ) != nullptr;
    }

    // The name pointer comes from a reverse-confirmed virtual call, but this
    // small guard keeps diagnostic printf from dereferencing an unreadable
    // result if a material implementation differs from the common path.
    static bool CopyNightGroupName( const char* source, char ( &out )[64] )
    {
        out[0] = '\0';
        if ( !source )
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        if ( !VirtualQuery( source, &mbi, sizeof( mbi ) ) || mbi.State != MEM_COMMIT )
            return false;
        const DWORD protection = mbi.Protect;
        if ( ( protection & PAGE_GUARD ) ||
             ( protection & 0xFFu ) == PAGE_NOACCESS )
            return false;

        const auto begin = reinterpret_cast<uintptr_t>( source );
        const auto end = reinterpret_cast<uintptr_t>( mbi.BaseAddress ) + mbi.RegionSize;
        if ( begin >= end )
            return false;
        const size_t available = static_cast<size_t>( end - begin );
        const size_t limit = available < 63u ? available : 63u;
        for ( size_t i = 0; i < limit; ++i )
        {
            const unsigned char c = static_cast<unsigned char>( source[i] );
            if ( c == '\0' )
            {
                out[i] = '\0';
                return i > 0;
            }
            if ( c < 0x20u || c > 0x7Eu )
                return false;
            out[i] = static_cast<char>( c );
        }
        out[limit] = '\0';
        return limit > 0;
    }

    static NightTintSettings GetNightTintSettings()
    {
        const auto& night = g_config.settings.visuals.others;
        NightTintSettings settings{};
        settings.intensity = night.night_mode_intensity < 0 ? 0 :
            ( night.night_mode_intensity > 100 ? 100 : night.night_mode_intensity );
        settings.red = night.night_mode_color.r < 0 ? 0 :
            ( night.night_mode_color.r > 255 ? 255 : night.night_mode_color.r );
        settings.green = night.night_mode_color.g < 0 ? 0 :
            ( night.night_mode_color.g > 255 ? 255 : night.night_mode_color.g );
        settings.blue = night.night_mode_color.b < 0 ? 0 :
            ( night.night_mode_color.b > 255 ? 255 : night.night_mode_color.b );
        settings.key =
            ( static_cast<uint64_t>( settings.intensity ) << 24 ) |
            ( static_cast<uint64_t>( settings.red ) << 16 ) |
            ( static_cast<uint64_t>( settings.green ) << 8 ) |
              static_cast<uint64_t>( settings.blue );
        return settings;
    }

    static void ClearNightWorkWithoutDereference()
    {
        s_nightMaterials.clear();
        s_nightPhase = NightWorkPhase::Idle;
        s_nightMaterialCursor = 0u;
        s_nightScanHandle = 0u;
        s_nightScanCount = 0u;
        s_nightEnumSlot = -1;
        s_nightApplied = false;
        s_nightAppliedSettingsKey = 0u;
        s_nightFallbackRestoreActive = false;
        s_nightFallbackRestoreDone = false;
        s_nightFallbackHandle = 0u;
        s_nightFallbackCount = 0u;
        s_nightFallbackEnumSlot = -1;
    }

    static void ApplyNightTint( const NightMaterialState& state,
                                const NightTintSettings& settings )
    {
        if ( !state.material || !state.material->IsNightModeCompatible() )
            return;

        const float tintBlend = static_cast<float>( settings.intensity ) / 100.f;
        const float tintMulR = 1.f + ( static_cast<float>( settings.red ) / 255.f - 1.f ) * tintBlend;
        const float tintMulG = 1.f + ( static_cast<float>( settings.green ) / 255.f - 1.f ) * tintBlend;
        const float tintMulB = 1.f + ( static_cast<float>( settings.blue ) / 255.f - 1.f ) * tintBlend;
        const float propLift = state.staticProp ? 1.12f : 1.f;
        const float finalR = state.red * tintMulR * propLift;
        const float finalG = state.green * tintMulG * propLift;
        const float finalB = state.blue * tintMulB * propLift;
        state.material->ColorModulate( finalR > 1.f ? 1.f : finalR,
                                       finalG > 1.f ? 1.f : finalG,
                                       finalB > 1.f ? 1.f : finalB );
    }

    static void ProcessNightRestoreBatch()
    {
        size_t processed = 0u;
        while ( s_nightMaterialCursor < s_nightMaterials.size() &&
                processed < kNightMaterialsPerFrame )
        {
            const NightMaterialState& state = s_nightMaterials[s_nightMaterialCursor++];
            ++processed;
            // This pointer entered the cache only after the confirmed IMaterial
            // validation during scan. Do not run that validation again on restore:
            // a false-negative here leaves a previously tinted material behind.
            if ( state.material )
            {
                state.material->ColorModulate( state.red, state.green, state.blue );
                state.material->AlphaModulate( state.alpha );
            }
        }

        if ( s_nightMaterialCursor >= s_nightMaterials.size() )
            ClearNightWorkWithoutDereference();
    }

    // Disabling is a single user action. Restore every cached original in the
    // same Update so the map cannot remain partially tinted after the checkbox
    // is cleared. Batched scanning/applying still avoids hitches while working.
    static void RestoreNightMaterialsNow()
    {
        size_t restored = 0u;
        for ( const NightMaterialState& state : s_nightMaterials )
        {
            // Cached entries were validated before tinting. While still in-game,
            // restore through the already confirmed real IMaterial slots rather
            // than potentially skipping an entry on a second predicate check.
            if ( !state.material )
                continue;
            state.material->ColorModulate( state.red, state.green, state.blue );
            state.material->AlphaModulate( state.alpha );
            ++restored;
        }
        if ( !s_nightMaterials.empty() )
            DBGLOG( "[night] restored: %zu world/static-prop materials\n", restored );
        ClearNightWorkWithoutDereference();
    }

    static bool StartNightFallbackRestore()
    {
        auto* materials = SDK::g_pMaterialSystem;
        const int enumSlot = materials ? materials->ResolveNightModeEnumerationSlot() : -1;
        if ( !materials || enumSlot < 0 )
            return false;

        s_nightFallbackRestoreActive = true;
        s_nightFallbackHandle = 0u;
        s_nightFallbackCount = materials->GetNumMaterialsAt( enumSlot );
        s_nightFallbackEnumSlot = enumSlot;
        return true;
    }

    static void ProcessNightFallbackRestoreBatch()
    {
        auto* materials = SDK::g_pMaterialSystem;
        if ( !materials || s_nightFallbackEnumSlot < 0 )
        {
            s_nightFallbackRestoreActive = false;
            s_nightFallbackRestoreDone = true;
            return;
        }

        size_t processed = 0u;
        while ( s_nightFallbackHandle < s_nightFallbackCount &&
                processed < kNightMaterialsPerFrame )
        {
            const uint32_t handle = s_nightFallbackHandle++;
            ++processed;
            auto* material = materials->GetMaterialAt(
                s_nightFallbackEnumSlot,
                static_cast<SDK::IMaterialSystem::MaterialHandle_t>( handle ) );
            if ( !material || !material->IsNightModeCompatible() )
                continue;

            char group[64]{};
            if ( !CopyNightGroupName( material->GetTextureGroupName(), group ) ||
                 !IsNightModeGroup( group ) )
                continue;

            // Materials absent from the original cache have no recoverable source
            // modulation. Source world/static-prop defaults are neutral white/1.
            material->ColorModulate( 1.f, 1.f, 1.f );
            material->AlphaModulate( 1.f );
        }

        if ( s_nightFallbackHandle >= s_nightFallbackCount )
        {
            s_nightFallbackRestoreActive = false;
            s_nightFallbackRestoreDone = true;
            s_nightFallbackEnumSlot = -1;
            DBGLOG( "[night] fallback restore: scanned %u materials\n", s_nightFallbackCount );
        }
    }

    static bool StartNightScan( const NightTintSettings& settings )
    {
        auto* materials = SDK::g_pMaterialSystem;
        const int enumSlot = materials ? materials->ResolveNightModeEnumerationSlot() : -1;
        if ( !materials || enumSlot < 0 )
        {
            if ( !s_nightRejected )
            {
                s_nightRejected = true;
                DBGLOG( "[night] skipped: matching MaterialSystem material-dictionary sequence unavailable\n" );
            }
            return false;
        }

        // The verified sequence identifies InvalidMaterial, GetMaterial(uint16)
        // and GetNumMaterials. It can begin at a different interface slot on
        // the factory-return object, so all three calls are relative to it.
        s_nightMaterials.clear();
        s_nightEnumSlot = enumSlot;
        s_nightScanCount = materials->GetNumMaterialsAt( enumSlot );
        s_nightScanHandle = 0u;
        s_nightMaterialCursor = 0u;
        s_nightApplied = false;
        s_nightAppliedSettingsKey = settings.key;
        s_nightPhase = NightWorkPhase::Scanning;
        return true;
    }

    static void ProcessNightScanBatch( const NightTintSettings& settings )
    {
        auto* materials = SDK::g_pMaterialSystem;
        if ( !materials || s_nightEnumSlot < 0 )
        {
            ClearNightWorkWithoutDereference();
            return;
        }

        size_t processed = 0u;
        while ( s_nightScanHandle < s_nightScanCount &&
                processed < kNightMaterialsPerFrame )
        {
            const uint32_t handle = s_nightScanHandle++;
            ++processed;
            auto* material = materials->GetMaterialAt(
                s_nightEnumSlot, static_cast<SDK::IMaterialSystem::MaterialHandle_t>( handle ) );
            if ( !material || !material->IsNightModeCompatible() )
                continue;

            char group[64]{};
            if ( !CopyNightGroupName( material->GetTextureGroupName(), group ) ||
                 !IsNightModeGroup( group ) )
                continue;

            float red = 1.f, green = 1.f, blue = 1.f;
            material->GetColorModulation( &red, &green, &blue );
            const float alpha = material->GetAlphaModulation();
            const bool staticProp = std::strstr( group, "StaticProp" ) != nullptr;
            s_nightMaterials.push_back( { material, red, green, blue, alpha, staticProp } );
        }

        // A slider/color change during discovery does not require a second scan:
        // no material has been modulated yet, so simply use the newest key later.
        s_nightAppliedSettingsKey = settings.key;
        if ( s_nightScanHandle >= s_nightScanCount )
        {
            s_nightMaterialCursor = 0u;
            s_nightPhase = NightWorkPhase::Applying;
        }
    }

    static void ProcessNightApplyBatch( const NightTintSettings& settings )
    {
        if ( s_nightAppliedSettingsKey != settings.key )
        {
            // Always tint from the cached originals, never from last frame's tint.
            // Restarting a small batch is cheap and avoids a full restore/rescan.
            s_nightAppliedSettingsKey = settings.key;
            s_nightMaterialCursor = 0u;
        }

        size_t processed = 0u;
        while ( s_nightMaterialCursor < s_nightMaterials.size() &&
                processed < kNightMaterialsPerFrame )
        {
            ApplyNightTint( s_nightMaterials[s_nightMaterialCursor++], settings );
            ++processed;
        }

        if ( s_nightMaterialCursor >= s_nightMaterials.size() )
        {
            s_nightApplied = true;
            s_nightPhase = NightWorkPhase::Active;
            DBGLOG( "[night] %s: %zu world/static-prop materials (intensity=%d color=%d,%d,%d; batched=%zu/frame)\n",
                      s_nightMaterials.empty() ? "no eligible materials" : "applied",
                      s_nightMaterials.size(), settings.intensity, settings.red,
                      settings.green, settings.blue, kNightMaterialsPerFrame );
        }
    }

    static void UpdateNightMode()
    {
        const bool inGame = SDK::g_pEngine && SDK::g_pEngine->IsInGame();
        if ( !inGame )
        {
            // Never touch cached IMaterial pointers after a map leave.
            if ( s_nightWasInGame )
            {
                ClearNightWorkWithoutDereference();
                s_nightRejected = false;
            }
            s_nightWasInGame = false;
            return;
        }

        s_nightWasInGame = true;
        const bool enabled = g_config.settings.visuals.others.night_mode;
        const NightTintSettings settings = GetNightTintSettings();

        if ( !enabled )
        {
            // First restore exact cached originals. Then perform one independent
            // dictionary pass for late materials that were not present in cache.
            if ( !s_nightFallbackRestoreDone && !s_nightFallbackRestoreActive )
            {
                if ( !s_nightMaterials.empty() )
                    RestoreNightMaterialsNow();
                else
                    ClearNightWorkWithoutDereference();

                // ClearNightWork resets fallback state; start after it.
                if ( !StartNightFallbackRestore() )
                    s_nightFallbackRestoreDone = true;
            }
            if ( s_nightFallbackRestoreActive )
                ProcessNightFallbackRestoreBatch();
            return;
        }

        // A re-enable during a batched fallback restore cancels that stale pass.
        // The next disable will start a fresh dictionary sweep for the live map.
        s_nightFallbackRestoreActive = false;
        s_nightFallbackHandle = 0u;
        s_nightFallbackCount = 0u;
        s_nightFallbackEnumSlot = -1;
        s_nightFallbackRestoreDone = false;

        if ( s_nightPhase == NightWorkPhase::Restoring )
        {
            // If re-enabled while restore is still in progress, retint from the
            // cached source RGB. Some entries may already be original, which is OK.
            s_nightPhase = NightWorkPhase::Applying;
            s_nightMaterialCursor = 0u;
            s_nightAppliedSettingsKey = settings.key;
        }

        if ( s_nightPhase == NightWorkPhase::Idle )
        {
            if ( !StartNightScan( settings ) )
                return;
        }

        if ( s_nightPhase == NightWorkPhase::Scanning )
        {
            ProcessNightScanBatch( settings );
            return;
        }

        if ( s_nightPhase == NightWorkPhase::Applying )
        {
            ProcessNightApplyBatch( settings );
            return;
        }

        if ( s_nightPhase == NightWorkPhase::Active &&
             s_nightAppliedSettingsKey != settings.key )
        {
            // A new tint recolors the cached set in small batches; it does not
            // call GetMaterialAt or scan the 2600-entry dictionary again.
            s_nightApplied = false;
            s_nightAppliedSettingsKey = settings.key;
            s_nightMaterialCursor = 0u;
            s_nightPhase = NightWorkPhase::Applying;
            ProcessNightApplyBatch( settings );
        }
    }
}


void NightMode::Update()
{
    UpdateNightMode();
}
