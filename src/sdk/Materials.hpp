#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/VFunc.hpp"

namespace SDK
{
    // ------------------------------------------------------------------
    //  MaterialSystem084 / IMaterial — slots verified statically in the
    //  supplied CSSO materialsystem.dll (SHA-256: see NIGHT_MODE_REVERSE_RU).
    //  The helpers below verify the exact slot targets at runtime before
    //  night mode calls them, so a mismatched materialsystem build is skipped.
    // ------------------------------------------------------------------
    class IMaterial
    {
    public:
        // Real CSSO IMaterial vtable confirmed by the runtime RVA snapshot of
        // a material returned from MaterialSystem084::GetMaterial:
        //   [1]  = GetTextureGroupName  (RVA 0xFAF0)
        //   [27] = AlphaModulate        (RVA 0xDA50)
        //   [28] = ColorModulate        (RVA 0xDF30)
        //   [44] = GetAlphaModulation   (RVA 0xF050)
        //   [45] = GetColorModulation   (RVA 0xF0B0)
        bool IsNightModeCompatible() const
        {
            const HMODULE module = GetModuleHandleA( "materialsystem.dll" );
            if ( !module )
                return false;

            void** table = *reinterpret_cast<void***>( const_cast<IMaterial*>( this ) );
            const uintptr_t base = reinterpret_cast<uintptr_t>( module );
            return table &&
                table[1]  == reinterpret_cast<void*>( base + 0x0FAF0 ) &&
                table[27] == reinterpret_cast<void*>( base + 0x0DA50 ) &&
                table[28] == reinterpret_cast<void*>( base + 0x0DF30 ) &&
                table[44] == reinterpret_cast<void*>( base + 0x0F050 ) &&
                table[45] == reinterpret_cast<void*>( base + 0x0F0B0 );
        }

        // Slot 0 directly precedes the confirmed GetTextureGroupName (slot 1)
        // in the Source IMaterial interface.
        const char* GetName()
        {
            return CallVFunc<const char*>( this, 0 );
        }

        const char* GetTextureGroupName()
        {
            return CallVFunc<const char*>( this, 1 );
        }

        void AlphaModulate( float alpha )
        {
            CallVFunc<void>( this, 27, alpha );
        }

        void ColorModulate( float red, float green, float blue )
        {
            CallVFunc<void>( this, 28, red, green, blue );
        }

        float GetAlphaModulation()
        {
            return CallVFunc<float>( this, 44 );
        }

        void GetColorModulation( float* red, float* green, float* blue )
        {
            CallVFunc<void>( this, 45, red, green, blue );
        }
    };

    class IMaterialSystem
    {
    public:
        using MaterialHandle_t = uint16_t;
        static constexpr MaterialHandle_t INVALID_MATERIAL_HANDLE = 0xFFFF;

        // Search the exact three-method material dictionary sequence in the
        // actual factory-return table. The static function RVAs are fixed for
        // the supplied DLL, but their virtual prefix can differ by interface
        // inheritance/adapter layout.
        int ResolveNightModeEnumerationSlot() const
        {
            const HMODULE module = GetModuleHandleA( "materialsystem.dll" );
            if ( !module )
                return -1;

            void** table = *reinterpret_cast<void***>( const_cast<IMaterialSystem*>( this ) );
            const uintptr_t base = reinterpret_cast<uintptr_t>( module );
            if ( !table )
                return -1;

            constexpr int kSearchLimit = 256;
            for ( int slot = 0; slot + 2 < kSearchLimit; ++slot )
            {
                if ( table[slot]     == reinterpret_cast<void*>( base + 0x0208E0 ) && // InvalidMaterial
                     table[slot + 1] == reinterpret_cast<void*>( base + 0x01F4B0 ) && // GetMaterial
                     table[slot + 2] == reinterpret_cast<void*>( base + 0x01F640 ) )  // GetNumMaterials
                    return slot;
            }
            return -1;
        }

        // These legacy slots are retained unchanged. Night mode does not rely
        // on them; it uses the verified enumeration wrappers below.
        IMaterial* FindMaterialEx( const char* name, const char* group, bool complain, const char* complainPrefix )
        {
            using fn = IMaterial* ( __fastcall* )( void*, const char*, const char*, bool, const char* );
            return ( *reinterpret_cast<fn**>( this ) )[111]( this, name, group, complain, complainPrefix );
        }
        IMaterial* CreateMaterial( const char* name, void* keyValues )
        {
            using fn = IMaterial* ( __fastcall* )( void*, const char*, void* );
            return ( *reinterpret_cast<fn**>( this ) )[72]( this, name, keyValues );
        }

        MaterialHandle_t InvalidMaterialAt( int enumSlot )
        {
            return CallVFunc<MaterialHandle_t>( this, enumSlot );
        }

        IMaterial* GetMaterialAt( int enumSlot, MaterialHandle_t handle )
        {
            return CallVFunc<IMaterial*>( this, enumSlot + 1, handle );
        }

        uint16_t GetNumMaterialsAt( int enumSlot )
        {
            return CallVFunc<uint16_t>( this, enumSlot + 2 );
        }
    };

} // namespace SDK
