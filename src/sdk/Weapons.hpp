#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/EngineState.hpp"

namespace SDK
{
    // ------------------------------------------------------------------
    //  Weapon data access — runtime via client.dll functions
    //
    //  sub_18025E6F0(classname) → internal index (uint16)
    //  sub_18025E6C0(index)     → weapon data pointer (char*)
    //  Weapon data struct is 0xDF0 (3568) bytes per entry
    //
    //  Key combat offsets in weapon data:
    //    +0xB14: float  Penetration
    //    +0xB18: int    Damage
    //    +0xB1C: float  HeadshotMultiplier
    //    +0xB20: float  Range
    //    +0xB24: float  RangeModifier
    //    +0xB28: int    Bullets
    //    +0x788: int    WeaponType
    //    +0x794: float  WeaponArmorRatio
    //    +0x2EC: int    NumPenetrations (at +0x2EC = 748)
    // ------------------------------------------------------------------
    using WeaponDataNameToIndexFn = unsigned short ( __fastcall* )( const char* name );
    using WeaponDataIndexToPtrFn  = char* ( __fastcall* )( unsigned short index );
    using WeaponDataInvalidIdxFn  = unsigned short ( __fastcall* )();

    inline WeaponDataNameToIndexFn fnWeaponDataNameToIndex = nullptr;
    inline WeaponDataIndexToPtrFn  fnWeaponDataIndexToPtr  = nullptr;
    inline WeaponDataInvalidIdxFn  fnWeaponDataInvalidIdx  = nullptr;

    // IsBreakableEntity — client.dll sub_1801E1790 (predicate for pen exit logic)
    using IsBreakableEntityFn = bool ( __fastcall* )( void* entity );
    inline IsBreakableEntityFn fnIsBreakableEntity = nullptr;

    inline int nWeaponClassnameOffset = 0xDDC;   // m_szClassname in weapon entity
    // CBaseCSGrenade client registration: m_fThrowTime=0xED0 and
    // m_flThrowStrength=0xED4 (client.dll registration block at 0x18002FA3C/67).
    inline constexpr int nGrenadeThrowTimeOffset = 0xED0;
    inline constexpr int nGrenadeThrowStrengthOffset = 0xED4;

    // Weapon data struct layout — only the fields we need
    struct WeaponData
    {
        // Confirmed CSSO weapon-data movement fields used by the Supremacy
        // AutoStop port. `maxPlayerSpeedAlt` is the scoped/alternate value.
        uint8_t _padMaxSpeed0[0x77C];
        float   maxPlayerSpeed;      // +0x77C
        uint8_t _padMaxSpeed1[0x784 - 0x780];
        float   maxPlayerSpeedAlt;   // +0x784
        uint8_t _pad0[0x794 - 0x788];
        float   armorRatio;          // +0x794 — WeaponArmorRatio
        uint8_t _pad1[0xB14 - 0x798];
        float   penetration;         // +0xB14
        int     damage;              // +0xB18
        float   headshotMultiplier;  // +0xB1C
        float   range;               // +0xB20
        float   rangeModifier;       // +0xB24
        int     bullets;             // +0xB28
        float   cycleTime;           // +0xB2C
        float   cycleTimeAlt;        // +0xB30
        float   spread;              // +0xB34
        float   spreadAlt;           // +0xB38
        float   inaccuracyCrouch;    // +0xB3C
        float   inaccuracyCrouchAlt; // +0xB40
        float   inaccuracyStand;     // +0xB44
        float   inaccuracyStandAlt;  // +0xB48
        float   inaccuracyJump;      // +0xB4C
        float   inaccuracyJumpAlt;   // +0xB50
        float   inaccuracyLand;      // +0xB54
        float   inaccuracyLandAlt;   // +0xB58
        float   inaccuracyLadder;    // +0xB5C
        float   inaccuracyLadderAlt; // +0xB60
        float   inaccuracyFire;      // +0xB64
        float   inaccuracyFireAlt;   // +0xB68
        float   inaccuracyMove;      // +0xB6C
        float   inaccuracyMoveAlt;   // +0xB70
        float   inaccuracyAir;       // +0xB74
        float   inaccuracyAirAlt;    // +0xB78
        uint8_t _pad2[0xB88 - 0xB7C];
        float   inaccuracyScale;     // +0xB88
        float   inaccuracyPitch;     // +0xB8C
        float   inaccuracyAltStand;  // +0xB90
    };

    // Read weapon data pointer from weapon entity's classname
    // Returns nullptr on failure
    inline const WeaponData* GetWeaponDataFromEntity( void* weaponEntity )
    {
        if ( !weaponEntity || !fnWeaponDataNameToIndex || !fnWeaponDataIndexToPtr )
            return nullptr;

        const char* classname = reinterpret_cast<const char*>(
            reinterpret_cast<uintptr_t>( weaponEntity ) + nWeaponClassnameOffset );
        if ( !classname || !classname[0] )
            return nullptr;

        // CSSO's fnWeaponDataNameToIndex expects the short name without the
        // "weapon_" prefix (e.g. "ak47", not "weapon_ak47"). Strip it if present.
        // This was the root cause of "rage never fires until you pick up a specific
        // weapon": every lookup returned fnWeaponDataInvalidIdx(), wepInfo stayed
        // null and RunRage returned immediately on every command.
        const char* shortName = classname;
        if ( shortName[0] == 'w' && shortName[1] == 'e' && shortName[2] == 'a' &&
             shortName[3] == 'p' && shortName[4] == 'o' && shortName[5] == 'n' &&
             shortName[6] == '_' )
            shortName += 7; // skip "weapon_"

        // Try short name first (the common case after stripping).
        unsigned short idx = fnWeaponDataNameToIndex( shortName );
        if ( fnWeaponDataInvalidIdx && idx == fnWeaponDataInvalidIdx() )
        {
            // Fallback: some CSSO weapons may already store the full classname.
            idx = fnWeaponDataNameToIndex( classname );
            if ( idx == fnWeaponDataInvalidIdx() )
                return nullptr;
        }

        return reinterpret_cast<const WeaponData*>( fnWeaponDataIndexToPtr( idx ) );
    }

} // namespace SDK
