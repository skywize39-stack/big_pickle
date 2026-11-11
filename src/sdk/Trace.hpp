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
    //  Engine trace structures (reversed from engine.dll)
    //  CGameTrace layout confirmed via sub_18018F520 (trace copy)
    //  and sub_180196850 (trace fill) — CSSO x64 specific
    // ------------------------------------------------------------------
    struct alignas(16) Ray_t
    {
        Vec3  m_Start;           // +0x00
        float _pad0;             // +0x0C (aligned to 16)
        Vec3  m_Delta;           // +0x10
        float _pad1;             // +0x1C
        Vec3  m_StartOffset;     // +0x20
        float _pad2;             // +0x2C
        Vec3  m_Extents;         // +0x30
        float _pad3;             // +0x3C
        bool  m_IsRay;           // +0x40
        bool  m_IsSwept;         // +0x41
        uint8_t _pad4[0x0E];    // +0x42..0x4F
    };
    static_assert( sizeof( Ray_t ) == 0x50, "Ray_t size mismatch" );

    struct cplane_t
    {
        Vec3  normal;            // +0x00
        float dist;              // +0x0C
        uint8_t type;            // +0x10
        uint8_t signbits;        // +0x11
        uint8_t pad[0x02];       // +0x12
    };
    static_assert( sizeof( cplane_t ) == 0x14, "cplane_t size mismatch" );

    // CSSO csurface_t — reversed from engine.dll trace fill
    // Starts at CGameTrace + 0x40 (no m_pSkip on CSSO x64)
    struct csurface_t
    {
        const char*    name;         // +0x00 (8 bytes x64 pointer)
        short          surfaceProps; // +0x08 — surface index for ISurfaceProps
        unsigned short flags;        // +0x0A — SURF_* flags (bit 7 = solid)
    };
    static_assert( sizeof( csurface_t ) == 0x10, "csurface_t unexpected size (MSVC pads to 16)" );

    struct alignas(16) CGameTrace
    {
        Vec3       startpos;           // +0x00
        Vec3       endpos;             // +0x0C
        cplane_t   plane;              // +0x18 (20 bytes)
        float      fraction;           // +0x2C
        int        contents;           // +0x30
        uint16_t   dispFlags;          // +0x34
        char       allsolid;           // +0x36
        char       startsolid;         // +0x37
        float      fractionleftsolid;  // +0x38
        uint8_t    _pad1[4];           // +0x3C (skipped in trace copy)
        csurface_t surface;            // +0x40 (16 bytes with MSVC padding)
        uint16_t   hitgroup;           // +0x50 — hitgroup WORD (confirmed: fire path sub_18029C390 reads [trace+50h] as word)
        uint8_t    _pad3[2];           // +0x52
        uint16_t   hitbox;             // +0x54 — bone-derived hitbox word (TraceToHitbox writes bone+172)
        uint8_t    _pad4[2];           // +0x56
        void*      m_pEnt;            // +0x58 — hit entity (confirmed: TraceAttack loads [trace+0x58] for IsBreakable)
        uint32_t   hitboxIndex;        // +0x60 — hitbox index (TraceToHitbox writes winner loop index)
        uint8_t    _pad5[12];          // +0x64 (align to 0x70)
    };
    static_assert( sizeof( CGameTrace ) == 0x70, "CGameTrace size mismatch" );

    // ------------------------------------------------------------------
    //  ITraceFilter — for TraceRay entity filtering
    //
    //  NOTE: game's vtable layout has 2 padding methods at indices 0/1,
    //  then ShouldHitEntity at index 2 and GetTraceType at index 3.
    //  Our definition must match or the game will call garbage at vtable[2].
    // ------------------------------------------------------------------
    class ITraceFilter
    {
    public:
        virtual void __fastcall Pad0() = 0;               // vtable[0]  — not called, supplied for layout
        virtual void __fastcall Pad1() = 0;               // vtable[1]  — not called, supplied for layout
        virtual bool ShouldHitEntity( void* pEntity, int contentsMask ) = 0;  // vtable[2]
        virtual int  GetTraceType() const = 0;            // vtable[3]
    };

    // Simple trace filter that skips a specific entity
    class CTraceFilterSimple : public ITraceFilter
    {
    public:
        void* m_pPassEnt;

        CTraceFilterSimple( void* pPassEntity ) : m_pPassEnt( pPassEntity ) {}

        void __fastcall Pad0() override {}
        void __fastcall Pad1() override {}
        bool ShouldHitEntity( void* pEntity, int contentsMask ) override
        {
            return pEntity != m_pPassEnt;
        }
        int GetTraceType() const override { return 0; } // TRACE_EVERYTHING
    };

    // Trace that ONLY tests the target entity (the wall/world is skipped) —
    // engine equivalent of ClipRayToEntity. Used for wallbang hit chance:
    // reachability through the wall is proven separately by the center-line
    // penetration sim, so the spread measurement must ignore the world.
    class CTraceFilterTargetOnly : public ITraceFilter
    {
    public:
        void* m_target;

        explicit CTraceFilterTargetOnly( void* pTarget ) : m_target( pTarget ) {}

        void __fastcall Pad0() override {}
        void __fastcall Pad1() override {}
        bool ShouldHitEntity( void* pEntity, int contentsMask ) override
        {
            return pEntity == m_target;
        }
        int GetTraceType() const override { return 2; } // TRACE_ENTITIES_ONLY
    };

    // ------------------------------------------------------------------
    //  IEngineTrace — interface ptr stored in engine global (engine+0x595E70)
    //  Vtable layout confirmed from game code (client.dll):
    //    vtable[0] = GetPointContents   (sub_18029DAA0 @ 0x18029dbb2)
    //    vtable[5] = TraceRay           (sub_18029C390 @ 0x18029c830)
    // ------------------------------------------------------------------
    class IEngineTrace
    {
    private:
        static bool s_vtableVerified;

        // Verify the resolved interface: its vtable[5] must equal the known-good
        // direct TraceRay address (engine.dll + 0x1B6E90, proven working in-game).
        static bool VerifyVtable( IEngineTrace* inst )
        {
            if ( !inst )
                return false;
            void** vtab = *reinterpret_cast<void***>( inst );
            HMODULE hEngine = GetModuleHandleA( "engine.dll" );
            if ( !vtab || !hEngine )
                return false;
            return vtab[5] == reinterpret_cast<void*>(
                (uintptr_t)hEngine + 0x1B6E90 );
        }

    public:
        // Lazy init: read global ptr on first use
        static IEngineTrace* Get()
        {
            static IEngineTrace* pInstance = nullptr;
            static bool resolved = false;
            if ( !resolved )
            {
                resolved = true;
                HMODULE hEngine = GetModuleHandleA( "engine.dll" );
                if ( hEngine )
                {
                    uintptr_t* pGlobalPtr = reinterpret_cast<uintptr_t*>(
                        (uintptr_t)hEngine + 0x595E70 );
                    if ( pGlobalPtr && *pGlobalPtr )
                    {
                        pInstance = reinterpret_cast<IEngineTrace*>( *pGlobalPtr );
                        s_vtableVerified = VerifyVtable( pInstance );
                        // printf_s( "[SDK] IEngineTrace resolved: %p (vtable %s)\n",
                        //           pInstance,
                        //           s_vtableVerified ? "OK" : "MISMATCH, direct-offset fallback" );
                    }
                    else
                    {
                        // printf_s( "[SDK] FAIL: IEngineTrace global ptr null at %p\n", pGlobalPtr );
                    }
                }
            }
            return pInstance;
        }

        void TraceRay( const Ray_t& ray, unsigned int mask,
                       ITraceFilter* pFilter, CGameTrace* pTrace )
        {
            if ( s_vtableVerified )
            {
                void** vtab = *reinterpret_cast<void***>( this );
                if ( vtab )
                {
                    using Fn = void( __fastcall* )(
                        void*, const Ray_t&, unsigned int, ITraceFilter*, CGameTrace* );
                    reinterpret_cast<Fn>( vtab[5] )( this, ray, mask, pFilter, pTrace );
                    return;
                }
            }
            static auto fn = reinterpret_cast<void( __fastcall* )(
                void*, const Ray_t&, unsigned int, ITraceFilter*, CGameTrace* )>(
                reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) ) + 0x1B6E90 );
            if ( fn )
                fn( this, ray, mask, pFilter, pTrace );
        }

        // vtable[0] — GetPointContents (game sub_18029DAA0 @ 0x18029dbb2).
        // Returns content flags at world position.
        unsigned int GetPointContents( const Vec3& pos, unsigned int mask, void* pCollide )
        {
            if ( s_vtableVerified )
            {
                void** vtab = *reinterpret_cast<void***>( this );
                if ( vtab )
                {
                    using Fn = unsigned int( __fastcall* )(
                        void*, const Vec3&, unsigned int, void* );
                    return reinterpret_cast<Fn>( vtab[0] )( this, pos, mask, pCollide );
                }
            }
            using Fn = unsigned int( __fastcall* )(
                void*, const Vec3&, unsigned int, void* );
            static auto fn = reinterpret_cast<Fn>(
                reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) ) + 0x1B6CE0 );
            if ( fn )
                return fn( this, pos, mask, pCollide );
            return 0;
        }
    };
    inline bool IEngineTrace::s_vtableVerified = false;

    // Standard trace masks — CSSO specific content flags
    enum
    {
        MASK_SOLID           = 0x200400B,
        MASK_PLAYERSOLID     = 0x201400B,   // +0x10000 (player clip)
        // server.dll penetration helper uses 0x0600400B for its hull fallback.
        // The former 0x0200400B omitted CONTENTS_DEBRIS and misses thin props.
        MASK_SHOT_HULL       = 0x600400B,
        MASK_VISIBLE         = 0x200400B,
        MASK_SHOT            = 0x4600400B,  // server-confirmed primary bullet trace mask
        CONTENTS_HITBOX      = 0x40000000,
        CONTENTS_SOLID       = 0x02008008,
        CONTENTS_MOVEABLE    = 0x00002000,
        CONTENTS_WINDOW      = 0x00000004,
    };

    // Surface flags (csurface_t.flags)
    enum : unsigned short
    {
        SURF_LIGHT       = 0x0001,
        SURF_SKY         = 0x0020,
        SURF_NODRAW      = 0x0080,
        SURF_HITBOX      = 0x8000,
    };

    // CharTexMaterial — ASCII character codes from surfaceproperties_t.gamematerial
    enum CharTexMaterial : uint16_t
    {
        CHAR_TEX_CONCRETE  = 'C', // 67
        CHAR_TEX_METAL     = 'M', // 77
        CHAR_TEX_DIRT      = 'D', // 68
        CHAR_TEX_VENT      = 'V', // 86
        CHAR_TEX_GRATE     = 'G', // 71
        CHAR_TEX_TILE      = 'Z', // 90
        CHAR_TEX_SLOSH     = 'S', // 83
        CHAR_TEX_WOOD      = 'W', // 87
        CHAR_TEX_FLESH     = 'F', // 70
        CHAR_TEX_WATER     = 'O', // 79
        CHAR_TEX_GLASS     = 'Y', // 89
        CHAR_TEX_CLIP      = 'I', // 73
        CHAR_TEX_PLASTIC   = 'L', // 76
        CHAR_TEX_CARDBOARD = 'U', // 85
    };

    // ------------------------------------------------------------------
    //  surfaceproperties_t — surface data returned by ISurfacePropsClient::GetSurfaceData
    //  Offsets confirmed from this game's client/server FireBullet paths:
    //    result read at +0x48 (penMod), +0x4C (damageMod), +0x50 (material)
    //  NOTE: NOT the vphysics 128-byte layout; use these offsets.
    // ------------------------------------------------------------------
    struct surfaceproperties_t
    {
        uint8_t  _pad0[0x48];         // physics + audio + sounds
        float    penetrationModifier; // +0x48 — wallbang penetration
        float    damageModifier;      // +0x4C — wallbang damage
        uint16_t gamematerial;        // +0x50 — material char code
        uint8_t  climbable;           // +0x52
        uint8_t  _pad1[0x80 - 0x53];  // pad to 128 bytes (unused tail)
    };
    static_assert( sizeof( surfaceproperties_t ) == 0x80, "surfaceproperties_t size mismatch" );

    // ------------------------------------------------------------------
    //  ISurfacePropsClient — client.dll wrapper
    //  Global: client.dll + 0x91C140 → ISurfacePropsClient*
    //  Vtable[5] = GetSurfaceData(int surfaceIndex)
    //  Confirmed from sub_18029DAA0 reading [rax+28h]
    // ------------------------------------------------------------------
    class ISurfacePropsClient
    {
    public:
        static ISurfacePropsClient* Get()
        {
            static ISurfacePropsClient* inst = nullptr;
            static bool resolved = false;
            if ( !resolved )
            {
                resolved = true;
                uintptr_t clientBase = (uintptr_t)GetModuleHandleA( "client.dll" );
                if ( clientBase )
                {
                    uintptr_t* pGlobal = reinterpret_cast<uintptr_t*>( clientBase + 0x91C140 );
                    if ( pGlobal && *pGlobal )
                    {
                        inst = reinterpret_cast<ISurfacePropsClient*>( *pGlobal );
                        // printf_s( "[SDK] ISurfacePropsClient resolved: %p\n", inst );
                    }
                    else
                    {
                        // printf_s( "[SDK] FAIL: ISurfacePropsClient global null at %p\n", pGlobal );
                    }
                }
            }
            return inst;
        }

        const surfaceproperties_t* GetSurfaceData( int surfaceIndex )
        {
            using Fn = const surfaceproperties_t* ( __fastcall* )( void*, int );
            return ( *reinterpret_cast<Fn**>( this ) )[5]( this, surfaceIndex );
        }
    };

} // namespace SDK
