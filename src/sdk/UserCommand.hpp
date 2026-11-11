#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>

namespace SDK
{
    // ------------------------------------------------------------------
    //  ScrambledFloat — XOR encrypted float (9 bytes)
    //  Layout: [k1][init_flag][k0][encrypted DWORD][k2][k3]
    // ------------------------------------------------------------------
    #pragma pack(push, 1)
    struct ScrambledFloat9
    {
        uint8_t k1;           // [0x00] key byte 1
        uint8_t init_flag;    // [0x01] MUST BE 1
        uint8_t k0;           // [0x02] key byte 0
        int32_t encrypted;    // [0x03] XOR encrypted float
        uint8_t k2;           // [0x07] key byte 2
        uint8_t k3;           // [0x08] key byte 3

        float Decode() const
        {
            const uint32_t key = k0 | ( k1 << 8 ) | ( k2 << 16 ) | ( k3 << 24 );
            const int32_t plain = encrypted ^ key;
            return *reinterpret_cast<const float*>( &plain );
        }

        void Encode( float val )
        {
            const uint32_t key = k0 | ( k1 << 8 ) | ( k2 << 16 ) | ( k3 << 24 );
            encrypted = *reinterpret_cast<const int32_t*>( &val ) ^ key;
        }
    };
    #pragma pack(pop)
    static_assert( sizeof( ScrambledFloat9 ) == 9, "ScrambledFloat9 size mismatch" );

    // ------------------------------------------------------------------
    //  ScrambledByte — XOR encrypted byte (4 bytes)
    // ------------------------------------------------------------------
    #pragma pack(push, 1)
    struct ScrambledByte
    {
        uint8_t k1;
        uint8_t init_flag;    // MUST BE 1
        uint8_t key;
        uint8_t encrypted;

        uint8_t Get() const { return encrypted ^ key; }
        void    Set( uint8_t val ) { encrypted = key ^ val; }
    };
    #pragma pack(pop)
    static_assert( sizeof( ScrambledByte ) == 4, "ScrambledByte size mismatch" );

    // ------------------------------------------------------------------
    //  CUserCmd layout — CSSO (116 bytes / 0x74)
    // ------------------------------------------------------------------
    #pragma pack(push, 1)
    struct CUserCmd
    {
        ScrambledByte  field_A;          // +0x00  scrambled CRC byte
        uint8_t        pad_04[4];        // +0x04
        int            command_number;   // +0x08
        int            tick_count;       // +0x0C
        ScrambledByte  field_B;          // +0x10  scrambled CRC byte
        uint8_t        pad_14[2];        // +0x14

        // View angles (ScrambledFloat each = 9 bytes)
        ScrambledFloat9 viewangles_yaw;    // +0x16
        ScrambledFloat9 viewangles_pitch;  // +0x1F
        ScrambledFloat9 viewangles_roll;   // +0x28

        // Movement (ScrambledFloat each = 9 bytes)
        ScrambledFloat9 forwardmove;       // +0x31
        ScrambledFloat9 sidemove;          // +0x3A
        ScrambledFloat9 upmove;            // +0x43

        int            buttons;         // +0x4C
        uint8_t        impulse;         // +0x50
        uint8_t        pad_51[3];       // +0x51
        int            weaponselect;    // +0x54
        int            weaponsubtype;   // +0x58
        ScrambledByte  field_C;         // +0x5C  scrambled CRC byte
        uint8_t        pad_60[4];       // +0x60
        int            random_seed;     // +0x64
        int            mousedx;         // +0x68
        ScrambledByte  field_D;         // +0x6C  scrambled CRC byte
        uint8_t        pad_70[4];       // +0x70
    };
    #pragma pack(pop)
    static_assert( sizeof( CUserCmd ) == 0x74, "CUserCmd size mismatch" );

    // ------------------------------------------------------------------
    //  Helper accessors
    // ------------------------------------------------------------------
    inline float GetYaw  ( const CUserCmd* c ) { return c->viewangles_yaw.Decode(); }
    inline float GetPitch( const CUserCmd* c ) { return c->viewangles_pitch.Decode(); }
    inline void  SetYaw  ( CUserCmd* c, float v ) { c->viewangles_yaw.Encode( v ); }
    inline void  SetPitch( CUserCmd* c, float v ) { c->viewangles_pitch.Encode( v ); }

    struct CVerifiedUserCmd
    {
        CUserCmd cmd;
        int      crc;
    };
    static_assert( sizeof( CVerifiedUserCmd ) == 0x78, "CVerifiedUserCmd size mismatch" );

    // ------------------------------------------------------------------
    //  Button flags
    // ------------------------------------------------------------------
    enum
    {
        IN_ATTACK        = ( 1 << 0 ),
        IN_JUMP          = ( 1 << 1 ),
        IN_DUCK          = ( 1 << 2 ),
        IN_FORWARD       = ( 1 << 3 ),
        IN_BACK          = ( 1 << 4 ),
        IN_USE           = ( 1 << 5 ),
        IN_CANCEL        = ( 1 << 6 ),
        IN_LEFT          = ( 1 << 7 ),
        IN_RIGHT         = ( 1 << 8 ),
        IN_MOVELEFT      = ( 1 << 9 ),
        IN_MOVERIGHT     = ( 1 << 10 ),
        IN_ATTACK2       = ( 1 << 11 ),
        IN_BULLRUSH      = ( 1 << 22 ), // CSSO movement +0x289F55 / +0x289F6E
        MULTIPLAYER_BACKUP = 90,
    };

    enum Hitgroup
    {
        HITGROUP_GENERIC  = 0,
        HITGROUP_HEAD     = 1,
        HITGROUP_CHEST    = 2,
        HITGROUP_STOMACH  = 3,
        HITGROUP_LEFTARM  = 4,
        HITGROUP_RIGHTARM = 5,
        HITGROUP_LEFTLEG  = 6,
        HITGROUP_RIGHTLEG = 7,
        HITGROUP_GEAR     = 10,
    };

    enum
    {
        CINPUT_OFFSET_COMMANDS = 0x108,
        CINPUT_OFFSET_VERIFIED = 0x110,
    };

} // namespace SDK
