#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>

namespace SDK
{
    // ------------------------------------------------------------------
    //  Generic vtable call helpers
    // ------------------------------------------------------------------
    template <typename T>
    inline T CallVFunc( void* pThis, int nIndex )
    {
        return ( *reinterpret_cast<T( __fastcall*** )( void* )>( pThis ) )[nIndex]( pThis );
    }

    template <typename T, typename... Args>
    inline T CallVFunc( void* pThis, int nIndex, Args... args )
    {
        return ( *reinterpret_cast<T( __fastcall*** )( void*, Args... )>( pThis ) )[nIndex]( pThis, args... );
    }

} // namespace SDK
