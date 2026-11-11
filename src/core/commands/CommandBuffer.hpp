#pragma once
#include "sdk/UserCommand.hpp"

namespace Commands
{
    using CrcFn = uint32_t(__fastcall*)(void* command);
    using CopyFn = void*(__fastcall*)(void* verified, void* command);

    // Non-owning view. Lifetime and game-thread execution belong to the caller.
    // This class does not predict, change attack intent, or alter ring entries
    // other than the checksum fields and verified copy during Synchronize.
    class CommandBuffer
    {
    public:
        explicit CommandBuffer(void* input) : m_input(input) {}
        SDK::CUserCmd* Find(int sequence) const;
        SDK::CVerifiedUserCmd* Verified(int sequence) const;
        bool Synchronize(int sequence, CrcFn crc, CopyFn copy) const;
        bool SynchronizeNative(int sequence) const;
    private:
        void* m_input = nullptr;
    };
}
