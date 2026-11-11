#include "core/commands/CommandBuffer.hpp"
#include "sdk/EngineState.hpp"
#include "sdk/build/CssoBuild.hpp"

using namespace SDK;

CUserCmd* Commands::CommandBuffer::Find(int sequence) const
{
    if (!m_input || sequence < 0) return nullptr;
    auto* ring = *reinterpret_cast<CUserCmd**>(
        reinterpret_cast<uintptr_t>(m_input) + CINPUT_OFFSET_COMMANDS);
    if (!ring) return nullptr;
    auto* command = &ring[sequence % MULTIPLAYER_BACKUP];
    return command->command_number == sequence ? command : nullptr;
}

CVerifiedUserCmd* Commands::CommandBuffer::Verified(int sequence) const
{
    if (!m_input || sequence < 0) return nullptr;
    auto* ring = *reinterpret_cast<CVerifiedUserCmd**>(
        reinterpret_cast<uintptr_t>(m_input) + CINPUT_OFFSET_VERIFIED);
    return ring ? &ring[sequence % MULTIPLAYER_BACKUP] : nullptr;
}

bool Commands::CommandBuffer::Synchronize(int sequence, CrcFn crc, CopyFn copy) const
{
    if (!crc || !copy) return false;
    auto* command = Find(sequence);
    auto* verified = Verified(sequence);
    if (!command || !verified) return false;
    const uint32_t checksum = crc(command);
    command->field_D.Set(static_cast<uint8_t>(checksum >> 0));
    command->field_C.Set(static_cast<uint8_t>(checksum >> 8));
    command->field_B.Set(static_cast<uint8_t>(checksum >> 16));
    command->field_A.Set(static_cast<uint8_t>(checksum >> 24));
    copy(verified, command);
    return true;
}

bool Commands::CommandBuffer::SynchronizeNative(int sequence) const
{
    if (!g_uClientBase) return false;
    return Synchronize(sequence,
        reinterpret_cast<CrcFn>(g_uClientBase + Build::Commands::Crc),
        reinterpret_cast<CopyFn>(g_uClientBase + Build::Commands::VerifiedCopy));
}
