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
    //  KeyValues function pointers (from materialsystem.dll)
    // ------------------------------------------------------------------
    using KeyValues_AllocateFn  = void* ( __fastcall* )( int size );
    using KeyValues_SetNameFn   = void* ( __fastcall* )( void* kv, const char* name );
    using KeyValues_SetIntFn    = void  ( __fastcall* )( void* kv, const char* key, int value );
    using KeyValues_SetStringFn = void  ( __fastcall* )( void* kv, const char* key, const char* value );

    inline KeyValues_AllocateFn  fnKeyValues_Allocate  = nullptr;
    inline KeyValues_SetNameFn   fnKeyValues_SetName   = nullptr;
    inline KeyValues_SetIntFn    fnKeyValues_SetInt    = nullptr;
    inline KeyValues_SetStringFn fnKeyValues_SetString = nullptr;

    inline void* CreateKeyValues( const char* shaderName )
    {
        if ( !fnKeyValues_Allocate || !fnKeyValues_SetName )
            return nullptr;
        void* kv = fnKeyValues_Allocate( 64 );
        if ( kv )
            fnKeyValues_SetName( kv, shaderName );
        return kv;
    }

    inline void SetKeyValueInt( void* kv, const char* key, int value )
    {
        if ( kv && fnKeyValues_SetInt )
            fnKeyValues_SetInt( kv, key, value );
    }

    inline void SetKeyValueString( void* kv, const char* key, const char* value )
    {
        if ( kv && fnKeyValues_SetString )
            fnKeyValues_SetString( kv, key, value );
    }

} // namespace SDK
