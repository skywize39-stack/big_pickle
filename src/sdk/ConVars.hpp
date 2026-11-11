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
    //  IConVar — ConVar value setters (vstdlib.dll, vtable 0x1800309B8)
    //  The in-memory float value at +0x54 is XOR-obfuscated with the object
    //  address, so values MUST be written through these virtuals (SetValue),
    //  never by raw memory writes.
    //    vtable slots: SetString=+0x58, SetFloat=+0x60, SetInt=+0x68
    //    SetFloat(value, force=0) early-outs when the value is unchanged.
    // ------------------------------------------------------------------
    class IConVar
    {
    public:
        virtual void Virtual00() = 0;
        virtual void Virtual01() = 0;
        virtual void Virtual02() = 0;
        virtual void Virtual03() = 0;
        virtual void Virtual04() = 0;
        virtual void Virtual05() = 0;
        virtual void Virtual06() = 0;
        virtual void Virtual07() = 0;
        virtual void Virtual08() = 0;
        virtual void Virtual09() = 0;
        virtual void Virtual10() = 0;
        virtual void SetString( const char* ) = 0;         // +0x58
        virtual void SetFloat( float, bool = false ) = 0;  // +0x60
        virtual void SetInt( int, bool = false ) = 0;      // +0x68

        // engine+0x341850: the root ConVar's float is XOR'd with its address.
        // Bounded parent traversal also rejects malformed aliases/cycles.
        bool TryGetFloat(float& result) const
        {
            __try
            {
                auto root = reinterpret_cast<uintptr_t>(this);
                for (int n = 0; root && n < 16; ++n)
                {
                    const auto parent = *reinterpret_cast<const uintptr_t*>(root + 0x38);
                    if (parent == root)
                    {
                        const uint32_t bits = *reinterpret_cast<const uint32_t*>(root + 0x54)
                            ^ static_cast<uint32_t>(root);
                        float value;
                        std::memcpy(&value, &bits, sizeof(value));
                        if (!std::isfinite(value)) return false;
                        result = value;
                        return true;
                    }
                    root = parent;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            return false;
        }

        // Registered min/max bounds. Layout verified against ClampValue
        // (sub_18000F3B0 in vstdlib.dll):
        //   +0x5C has_min (byte), +0x60 min (float)
        //   +0x64 has_max (byte), +0x68 max (float)
        bool  HasMin() const { return *( bool* )( ( uintptr_t )this + 0x5C ); }
        float GetMin() const { return *( float* )( ( uintptr_t )this + 0x60 ); }
        bool  HasMax() const { return *( bool* )( ( uintptr_t )this + 0x64 ); }
        float GetMax() const { return *( float* )( ( uintptr_t )this + 0x68 ); }

        // Removes the registered clamp range so SetValue will accept any
        // value (ClampValue returns immediately when both flags are clear).
        void ClearBounds()
        {
            *( bool* )( ( uintptr_t )this + 0x5C ) = false;
            *( bool* )( ( uintptr_t )this + 0x64 ) = false;
        }
    };

    // ------------------------------------------------------------------
    //  ICvar — "Cvar005" interface (vstdlib.dll)
    //  Acquired via VStdLib_GetICVarFactory() -> CreateInterface.
    //  The registered interface is named "Cvar005" (NOT CvarQuery002) and
    //  returns the CCvar singleton at &off_180041020, whose hash table is
    //  at object+0x58. Confirmed in vstdlib.dll:
    //    sub_180001070: registers "Cvar005" -> factory sub_180006400
    //    sub_180006400: returns &off_180041020 (singleton, vtable 0x18002F850)
    //    vtable slot 10/11 (+0x50/+0x58) = sub_180004D70 (FindConVar thunk,
    //      does `add rcx, 58h; jmp sub_180004D80` hash lookup)
    // ------------------------------------------------------------------
    class ICvar
    {
    public:
        static ICvar* Get()
        {
            static ICvar* inst = nullptr;
            static bool   done = false;
            if ( !done )
            {
                done = true;
                HMODULE hVstd = GetModuleHandleA( "vstdlib.dll" );
                if ( hVstd )
                {
                    typedef void* ( __cdecl* GetICVarFactoryFn )( );
                    auto fnFactory = reinterpret_cast<GetICVarFactoryFn>(
                        GetProcAddress( hVstd, "VStdLib_GetICVarFactory" ) );
                    if ( fnFactory )
                    {
                        typedef void* ( __stdcall* CreateInterfaceFn )( const char*, int* );
                        auto fnCreate = reinterpret_cast<CreateInterfaceFn>( fnFactory() );
                        if ( fnCreate )
                        {
                            inst = reinterpret_cast<ICvar*>( fnCreate( "Cvar005", nullptr ) );
                        }
                    }
                    // fallback: exported data symbol `cvar` caches the same
                    // singleton (populated during engine interface setup)
                    if ( !inst )
                    {
                        void** pSlot = reinterpret_cast<void**>( GetProcAddress( hVstd, "cvar" ) );
                        if ( pSlot && *pSlot )
                            inst = reinterpret_cast<ICvar*>( *pSlot );
                    }
                    // if ( inst )
                    //     printf_s( "[SDK] ICvar resolved: %p (vtable %p)\n", inst, *( void** )inst );
                }
                // if ( !inst )
                //     printf_s( "[SDK] FAIL: ICvar resolve failed\n" );
            }
            return inst;
        }

        // FindConVar(name) -> IConVar* (null if not registered)
        // Call the vtable slot DIRECTLY on `this`; object+0x50 is NOT an
        // interface vtable pointer for this singleton (it is zero).
        IConVar* FindConVar( const char* szName )
        {
            typedef IConVar* ( __fastcall* FindConVarFn )( void*, const char* );
            return ( *reinterpret_cast<FindConVarFn**>( this ) )[ 11 ]( this, szName );
        }
    };

} // namespace SDK
