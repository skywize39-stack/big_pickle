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
    //  ClientFrameStage_t
    // ------------------------------------------------------------------
    enum ClientFrameStage_t
    {
        FRAME_UNDEFINED                         = -1,
        FRAME_START                             =  0,
        FRAME_NET_UPDATE_START                  =  1,
        FRAME_NET_UPDATE_POSTDATAUPDATE_START   =  2,
        FRAME_NET_UPDATE_POSTDATAUPDATE_END     =  3,
        FRAME_NET_UPDATE_END                    =  4,
        FRAME_RENDER_START                      =  5,
        FRAME_RENDER_END                        =  6,
    };

    class IBaseClientDLL
    {
    public:
        // CSSO x64: engine+0x90F20 calls FrameStageNotify at index 35 (+0x118)
    };

    // ------------------------------------------------------------------
    //  Math / screen-transform types
    // ------------------------------------------------------------------
    struct VMatrix
    {
        float m[4][4];
        float*       operator[]( int i )       { return m[i]; }
        const float* operator[]( int i ) const { return m[i]; }
    };

    using ScreenTransformFn = bool( __fastcall* )( void*, const VMatrix&, const Vec3&, Vec3& );
    using InternalW2SFn     = bool( __fastcall* )( void*, const Vec3&, Vec3& );

    // ------------------------------------------------------------------
    //  player_info_t
    // ------------------------------------------------------------------
    struct player_info_t
    {
        char name[32];
        char __pad[120];
    };

    // ------------------------------------------------------------------
    //  Engine interfaces
    // ------------------------------------------------------------------
    class IVEngineClient
    {
    public:
        bool          GetPlayerInfo( int ent_num, player_info_t* pinfo ) { return CallVFunc<bool>( this, 8, ent_num, pinfo ); }
        int           GetLocalPlayer()              { return CallVFunc<int>( this, 12 ); }
        float         GetTime()                     { return CallVFunc<float>( this, 14 ); }
        void          SetViewAngles( const Vec3& ang ) { CallVFunc<void>( this, 19, &ang ); }
        int           GetMaxClients()               { return CallVFunc<int>( this, 21 ); }
        bool          IsInGame()                    { return CallVFunc<bool>( this, 26 ); }
        bool          IsConnected()                 { return CallVFunc<bool>( this, 27 ); }
        const VMatrix& WorldToScreenMatrix()        { return CallVFunc<const VMatrix&>( this, 37 ); }

        // vtable slot 6: formats "cmd %s" and dispatches the command locally
        // (verified in engine.dll: sub_180093D50). Used to send chat via the
        // client-side "say" command.
        void          ClientCmd( const char* szCmd ) { CallVFunc<void>( this, 6, szCmd, 0 ); }
    };

    // ------------------------------------------------------------------
    //  Game events — engine.dll CGameEventManager
    //  Interface "GameEventManager003", object vtable @ 0x180475B40.
    //  The CGameEvent vtable (0x180475AC8) matches the SDK layout below.
    // ------------------------------------------------------------------
    class IGameEvent;
    class IGameEventListener
    {
    public:
        virtual ~IGameEventListener() {}                    // slot 0
        virtual void FireGameEvent( IGameEvent* event ) = 0; // slot 1
    };

    class IGameEvent
    {
    public:
        virtual ~IGameEvent() {}                                          // 0
        virtual const char* GetName() = 0;                                // 1
        virtual bool        IsReliable() = 0;                             // 2
        virtual bool        IsLocal() = 0;                                // 3
        virtual bool        IsEmpty( const char* keyname ) = 0;           // 4
        virtual bool        GetBool( const char* keyname, bool def = false ) = 0;        // 5
        virtual int         GetInt( const char* keyname, int def = 0 ) = 0;             // 6
        virtual float       GetFloat( const char* keyname, float def = 0.f ) = 0;       // 7
        virtual const char* GetString( const char* keyname, const char* def = "" ) = 0; // 8
    };

    class IGameEventManager2
    {
    public:
        virtual ~IGameEventManager2() {}                                   // 0
        virtual bool LoadEventsFromFile( const char* filename ) = 0;       // 1
        virtual void Reset() = 0;                                          // 2
        virtual bool AddListener( IGameEventListener* listener, const char* name, bool bServerSide ) = 0; // 3
        virtual bool FindListener( IGameEventListener* listener, const char* name ) = 0; // 4
        virtual bool RemoveListener( IGameEventListener* listener ) = 0;   // 5
        virtual IGameEvent* CreateEvent( const char* name, bool bForce ) = 0; // 6
        virtual bool FireEvent( IGameEvent* event, bool bDontBroadcast ) = 0; // 7
        virtual bool FireEventClientSide( IGameEvent* event ) = 0;         // 8
    };

    class IClientEntityList
    {
    public:
        void* GetClientEntity( int nIndex ) { return CallVFunc<void*>( this, 3, nIndex ); }
    };

} // namespace SDK
