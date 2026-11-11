#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/UserCommand.hpp"
#include "sdk/Weapons.hpp"

namespace SDK
{
    // ------------------------------------------------------------------
    //  Entity offsets
    // ------------------------------------------------------------------
    inline int nLocalPlayerIndex      = 0;
    inline int nOriginOffset          = 0x404;
    inline int nHealthOffset          = 208;    // 0xD0
    inline int nTeamOffset            = 216;    // 0xD8
    inline int nFlagsOffset           = 1336;   // 0x538
    inline int nScopedOffset          = 0x8461;
    inline int nDefusingOffset        = 0x8468;
    inline int nFlashDurationOffset   = 0x84F4;
    inline int nFlashMaxAlphaOffset   = 0x84F0;
    inline int nWalkingOffset         = 0x8462;
    inline int nBombZoneOffset        = 0x846B;
    inline int nArmorOffset           = 0x85EC;
    inline int nHelmetOffset          = 0x85E8;
    // m_MoveType (byte, CBaseEntity). 0 = unknown -> ladder detection off.
    // TODO: fill from reverse (MOVETYPE_LADDER = 9, MOVETYPE_NOCLIP = 8).
    inline int nWeaponOwnerOffset     = 0xCE0; // DT_BaseCombatWeapon::m_hOwner
    inline int nMoveTypeOffset        = 0x1F0; // m_MoveType (byte), RE confirmed
    inline bool IsOnLadderOrNoclip( void* ent )
    {
        if ( !ent || nMoveTypeOffset <= 0 )
            return false;
        const unsigned char mt = *reinterpret_cast<const unsigned char*>(
            reinterpret_cast<uintptr_t>( ent ) + nMoveTypeOffset );
        return mt == 9 || mt == 8;
    }
    // bullet = EyeAngles + weapon_recoil_scale(2.0) * m_vecAimPunchAngle
    // m_vecAimPunchAngle (DT_CSLocal) = 0x8510; view punch m_vecPunchAngle = 0x1384
    inline int nPunchAngleOffset      = 0x8510;
    inline float flRecoilScale        = 2.0f;
    inline int nDefuserOffset         = 0x8608;
    inline int nAccountOffset         = 0x85E4;
    inline int nActiveWeaponOffset    = 0x12A0;
    inline int nShotsFiredOffset      = 0x8494;
    inline int nDuckAmountOffset      = 0x8558;
    inline int nDuckSpeedOffset       = 0x855C; // client RecvProp +0x269516
    inline int nLifeStateOffset       = 0xCF;
    inline int nProgressBarDurationOffset = 0x8498;
    inline int nIsPlantingOffset      = 0xEC8;   // m_bStartedArming (from reversed WeaponC4)
    inline int nBombPlantedOffset     = 0xAC;    // m_bBombPlanted (from GameRules)

    inline int nWeaponReloadOffset    = 0xCF6;
    inline int nWeaponStateOffset     = 0xD20;
    inline int nWeaponClip1Offset     = 0xD38;
    // Camera / limb angles (from reversed datamap DT_CSPlayer / DT_CSPlayer ->
    //               CBaseEntity): C_BaseEntity + nAngRotation = m_angRotation
    //               C_CSPlayer   + n_ayEyeOff    = m_angEye[] (pitch, yaw)
    inline int nAngRotationOffset     = 0x52C;   // m_angRotation (CBaseEntity)
    inline int nAnEyeOffset           = 0x85F0;  // m_angEye[0]   (C_CSPlayer)

    // CCSPlayerAnimState: pointer to the owning player (used to identify the
    // local player's animation state in the Update hook)
    inline int nAnimStateOwnerOffset  = 0x58;
    // Confirmed directly from the supplied client.dll network-property data.
    constexpr int nWeaponNextPrimaryAttack       = 0xCE8;
    constexpr int nWeaponNextSecondaryAttack     = 0xCEC;
    inline int nNextAttackOffset                 = 0x1158; // m_flNextAttack (DT_BCCLocalPlayerExclusive)
    inline int nTickBaseOffset             = 0x17DC; // m_nTickBase, confirmed in CPrediction::RunCommand client+0x2074E0
    // m_nFinalPredictedTick — sits right after m_nTickBase in the local-player
    // block. CPrediction::RunCommand reads, compares and increments it inside
    // the prediction path (client+0x20753C / 0x207806 / 0x2078D9). The
    // DoubleTap recharge fix increments it per skipped CL_Move so the client
    // prediction clock stays aligned with real frames.
    inline int nFinalPredictedTickOffset   = 0x17E0;
    inline int nVelocityOffset             = 0x148;  // m_vecVelocity[0] (CSSO, datamap sub_180126D10: offset 328 = 0x148)
    inline int nAbsVelocityOffset          = 0x1A8;  // m_vecAbsVelocity (datamap: 424 = 0x1A8; GetInaccuracy sub_18030A950 reads it for the move penalty)
    inline int nEFlagsOffset               = 0x1EC;  // m_iEFlags (datamap: 492 = 0x1EC)
    inline int nSimulationTimeOffset       = 0xA8;   // m_flSimulationTime (DT_BaseEntity, sub_180114330)

    enum { FL_ONGROUND = ( 1 << 0 ) };

    // m_iEFlags bit — "abs velocity needs recompute" (sub_180117000 UpdateAbsVelocity
    // checks this flag, then copies m_vecVelocity -> m_vecAbsVelocity).
    enum { EFL_DIRTY_ABSVELOCITY = 0x1000 };

    // ------------------------------------------------------------------
    //  C_BaseEntity — thin accessor wrapper
    // ------------------------------------------------------------------
    class C_BaseEntity
    {
    public:
        Vec3 GetAbsOrigin()           { return *CallVFunc<Vec3*>( this, 9 ); }
        Vec3 GetViewOffset()          { return *reinterpret_cast<Vec3*>( (uintptr_t)this + 0x13C ); }
        Vec3 EyePosition()            { return GetAbsOrigin() + GetViewOffset(); }
        int  GetHealth()              { return *reinterpret_cast<int*>( (uintptr_t)this + nHealthOffset ); }
        int  GetTeamNum()             { return *reinterpret_cast<int*>( (uintptr_t)this + nTeamOffset ); }
        int  GetFlags()               { return *reinterpret_cast<int*>( (uintptr_t)this + nFlagsOffset ); }
        int  GetLifeState()           { return *reinterpret_cast<BYTE*>( (uintptr_t)this + nLifeStateOffset ); }
        bool IsScoped()               { return *(bool*)( (uintptr_t)this + nScopedOffset ); }
        bool IsDefusing()             { return *(bool*)( (uintptr_t)this + nDefusingOffset ); }
        bool IsWalking()              { return *(bool*)( (uintptr_t)this + nWalkingOffset ); }
        bool IsInBombZone()           { return *(bool*)( (uintptr_t)this + nBombZoneOffset ); }
        bool HasDefuser()             { return *(bool*)( (uintptr_t)this + nDefuserOffset ); }
        bool HasHelmet()              { return *(bool*)( (uintptr_t)this + nHelmetOffset ); }
        int  GetArmor()               { return *reinterpret_cast<int*>( (uintptr_t)this + nArmorOffset ); }
        int  GetAccount()             { return *reinterpret_cast<int*>( (uintptr_t)this + nAccountOffset ); }
        int  GetActiveWeaponHandle()  { return *reinterpret_cast<int*>( (uintptr_t)this + nActiveWeaponOffset ); }
        int  GetShotsFired()          { return *reinterpret_cast<int*>( (uintptr_t)this + nShotsFiredOffset ); }
        Vec3 GetVelocity()            { return *reinterpret_cast<Vec3*>( (uintptr_t)this + nVelocityOffset ); }
        float GetNextAttack()         { return *(float*)( (uintptr_t)this + nNextAttackOffset ); }
        int&  TickBase()              { return *reinterpret_cast<int*>( (uintptr_t)this + nTickBaseOffset ); }
        float GetSimulationTime()     { return *(float*)( (uintptr_t)this + nSimulationTimeOffset ); }
        float GetFlashDuration()      { return *(float*)( (uintptr_t)this + nFlashDurationOffset ); }
        float GetFlashMaxAlpha()      { return *(float*)( (uintptr_t)this + nFlashMaxAlphaOffset ); }
        int  GetProgressBarDuration() { return *reinterpret_cast<int*>( (uintptr_t)this + nProgressBarDurationOffset ); }

        model_t* GetModel()
        {
            return *CallVFunc<model_t**>( this, 8 );
        }

        void* GetActiveWeapon()
        {
            if ( !g_pEntityList ) return nullptr;
            int handle = GetActiveWeaponHandle();
            return g_pEntityList->GetClientEntity( handle & 0xFFF );
        }

        const char* GetWeaponName()
        {
            void* weapon = GetActiveWeapon();
            if ( !weapon ) return "";
            return (const char*)( (uintptr_t)weapon + nWeaponClassnameOffset );
        }
    };

    // ------------------------------------------------------------------
    //  Weapon entity helpers
    // ------------------------------------------------------------------
    inline bool IsWeaponReloading( void* weapon )
    {
        return *(bool*)( (uintptr_t)weapon + nWeaponReloadOffset );
    }

    inline int GetWeaponState( void* weapon )
    {
        return *reinterpret_cast<int*>( (uintptr_t)weapon + nWeaponStateOffset );
    }

    inline int GetWeaponClip1( void* weapon )
    {
        return *reinterpret_cast<int*>( (uintptr_t)weapon + nWeaponClip1Offset );
    }

    inline float GetWeaponNextPrimaryAttack( void* weapon )
    {
        return *reinterpret_cast<float*>( (uintptr_t)weapon + nWeaponNextPrimaryAttack );
    }

    inline float GetWeaponNextSecondaryAttack( void* weapon )
    {
        return *reinterpret_cast<float*>( (uintptr_t)weapon + nWeaponNextSecondaryAttack );
    }

    inline bool IsEntityAlive( void* entity )
    {
        int  health = *reinterpret_cast<int*>( (uintptr_t)entity + nHealthOffset );
        BYTE life   = *reinterpret_cast<BYTE*>( (uintptr_t)entity + nLifeStateOffset );
        return health > 0 && life == 0;
    }

    // ------------------------------------------------------------------
    //  Tick conversion (CSSO runs 64 tick)
    // ------------------------------------------------------------------
    constexpr float kTickInterval = 1.0f / 64.0f;

    inline int TimeToTicks( float time )
    {
        return static_cast<int>( time / kTickInterval );
    }

    // ------------------------------------------------------------------
    //  Interface factory helper
    // ------------------------------------------------------------------
    inline void* GetInterface( HMODULE hModule, const char* szName )
    {
        typedef void* ( __stdcall* CreateInterfaceFn )( const char*, int* );
        auto fn = reinterpret_cast<CreateInterfaceFn>( GetProcAddress( hModule, "CreateInterface" ) );
        if ( !fn )
            return nullptr;
        return fn( szName, nullptr );
    }

} // namespace SDK
