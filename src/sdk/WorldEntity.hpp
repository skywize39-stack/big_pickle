#pragma once
#include "sdk/SDK.hpp"
#include <cmath>

namespace SDK::WorldEntity
{
    // client.dll CPlantedC4 GetClientClass +0x27DDA0 is slot 2 in
    // networkable vtable +0x6A1BE0. ClientClass network name is at +0x10.
    inline const char* ClassName(void* entity)
    {
        if (!entity) return nullptr;
        __try
        {
            void* net = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entity)+0x10);
            void** vt = *reinterpret_cast<void***>(net);
            using Fn = void* (__fastcall*)(void*);
            void* cls = reinterpret_cast<Fn>(vt[2])(net);
            return cls ? *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(cls)+0x10) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    inline float CurTime()
    {
        __try
        {
            if (!g_uClientBase) return -1.f;
            const uintptr_t globals = *reinterpret_cast<const uintptr_t*>(g_uClientBase+0x81CC50);
            if (!globals) return -1.f;
            const float t = *reinterpret_cast<const float*>(globals+0x0C);
            return std::isfinite(t) && t >= 0.f ? t : -1.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
    }

    template<class T> inline bool Read(void* entity, uintptr_t offset, T& result)
    {
        if (!entity) return false;
        __try { result = *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(entity)+offset); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}
