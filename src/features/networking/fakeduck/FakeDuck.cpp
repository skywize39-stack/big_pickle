#include "features/networking/fakeduck/FakeDuck.hpp"
#include "features/networking/fakelag/FakeLag.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include "config/Config.hpp"
#include "ui/binds/KeyBinds.hpp"
#include "sdk/SDK.hpp"
#include "sdk/build/CssoBuild.hpp"

#include <algorithm>
#include <cmath>

using namespace SDK;

namespace
{
    bool requested = false, active = false, ownsPackets = false, starting = true;
    uintptr_t owner = 0;
    int preparedCommand = 0, lastCommand = 0, inputDuckButtons = 0;
    int chokeLimit = 14, chokedCommands = 0;
    float interval = 0.f;
    constexpr int duckButtons = IN_DUCK | IN_BULLRUSH;

    bool MatchesBuild(uintptr_t base, Build::ModuleIdentity expected)
    {
        if (!base) return false;
        __try
        {
            if (*reinterpret_cast<const uint16_t*>(base) != 0x5A4D) return false;
            const auto offset = *reinterpret_cast<const uint32_t*>(base + 0x3C);
            if (offset < 0x40 || offset > 0x1000) return false;
            const uintptr_t pe = base + offset;
            return *reinterpret_cast<const uint32_t*>(pe) == 0x4550 &&
                *reinterpret_cast<const uint32_t*>(pe + 8) == expected.timestamp &&
                *reinterpret_cast<const uint32_t*>(pe + 0x50) == expected.imageSize;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Eligible(void* player)
    {
        if (!player || !g_pEngine || !g_pEngine->IsConnected() || !g_pEngine->IsInGame() ||
            !FakeLag::CanControlPackets() ||
            !MatchesBuild(g_uClientBase, Build::Client) ||
            !MatchesBuild(g_uEngineBase, Build::Engine)) return false;
        auto* local = reinterpret_cast<C_BaseEntity*>(player);
        __try
        {
            return local->GetHealth() > 0 && local->GetLifeState() == 0 &&
                (local->GetFlags() & FL_ONGROUND) && !IsOnLadderOrNoclip(player) &&
                !*reinterpret_cast<const bool*>(reinterpret_cast<uintptr_t>(player) + nDefusingOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadTiming()
    {
        __try
        {
            const uintptr_t globals = *reinterpret_cast<const uintptr_t*>(
                g_uClientBase + Build::Prediction::Globals);
            if (!globals) return false;
            interval = *reinterpret_cast<const float*>(globals + 0x1C);
            chokedCommands = *reinterpret_cast<const int*>(g_uEngineBase + Build::Prediction::Choked);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!std::isfinite(interval) || interval < .001f || interval > .1f ||
            chokedCommands < 0 || chokedCommands > 62) return false;

        // Supplied server default is 16; local/listen-server overrides can
        // lower it. A remote, server-only ConVar is not advertised to clients.
        chokeLimit = 14;
        if (auto* cvars = ICvar::Get())
            if (auto* limit = cvars->FindConVar("sv_maxusrcmdprocessticks"))
            {
                float value = 0.f;
                if (limit->TryGetFloat(value) && value > 0.f && value < 1024.f)
                    chokeLimit = std::clamp(static_cast<int>(value) - 1, 0, 14);
            }
        return chokeLimit >= 2;
    }

    void SendNow()
    {
        FakeLag::SetCurrentMoveChoke(false);
        ownsPackets = true;
    }

    void StopCommand(CUserCmd* cmd, void* local, bool restoreButtons)
    {
        if (restoreButtons && cmd && cmd->command_number == preparedCommand)
        {
            cmd->buttons = (cmd->buttons & ~duckButtons) | inputDuckButtons;
            if (EnginePrediction::Active()) EnginePrediction::Refresh(cmd, local);
        }
        if (active || ownsPackets) SendNow();
        active = false;
        starting = true;
        preparedCommand = 0;
        owner = 0;
    }
}

void FakeDuck::UpdateRequest(void* local)
{
    ownsPackets = false;
    const auto& cfg = g_config.settings.misc.fake_duck;
    // Resolve even when disabled so the bind list never retains a stale entry.
    const bool bind = Binds::Active("fakeduck", cfg.key, cfg.key_mode, cfg.enabled ? "Fake duck" : nullptr);
    requested = cfg.enabled && bind && Eligible(local);
    if (!requested && active) StopCommand(nullptr, local, false);
}

bool FakeDuck::WantsControl() { return requested || active || ownsPackets; }
bool FakeDuck::IsActive() { return active; }
bool FakeDuck::OwnsPackets() { return ownsPackets; }

void FakeDuck::Prepare(CUserCmd* cmd, void* local)
{
    if (!cmd || !requested || !Eligible(local) || (cmd->buttons & (IN_JUMP | IN_USE)) || !ReadTiming())
    {
        StopCommand(nullptr, local, false);
        return;
    }
    const auto address = reinterpret_cast<uintptr_t>(local);
    if (owner != address || (lastCommand && cmd->command_number != lastCommand + 1))
        starting = true;
    owner = address;
    lastCommand = preparedCommand = cmd->command_number;
    inputDuckButtons = cmd->buttons & duckButtons;
    active = ownsPackets = true;
    // client+289F55 sets bit 22 whenever duck is pressed. Keeping that bit
    // through the unduck phase avoids the transition penalty at +289F6E.
    // No writes to duck amount, stamina, flags, tickbase or view offset.
    cmd->buttons |= duckButtons;
    FakeLag::SetCurrentMoveChoke(false); // safe until prediction validated
}

void FakeDuck::AfterPrediction(CUserCmd* cmd, void* local)
{
    if (!active || !cmd || preparedCommand != cmd->command_number) return;
    if (!EnginePrediction::Reliable() || !Eligible(local))
    {
        StopCommand(cmd, local, true);
        return;
    }
    const auto address = reinterpret_cast<uintptr_t>(local);
    const float amount = *reinterpret_cast<const float*>(address + nDuckAmountOffset);
    const float speed = *reinterpret_cast<const float*>(address + nDuckSpeedOffset);
    if (!std::isfinite(amount) || amount < 0.f || amount > 1.001f ||
        !std::isfinite(speed) || speed <= 0.f || speed > 8.001f)
    {
        StopCommand(cmd, local, true);
        return;
    }

    // Fatality's initial crouch and queue alignment: never begin a cycle
    // halfway through a pre-existing fake-lag packet.
    if (starting && amount < .8f) { SendNow(); return; }
    if (starting && chokedCommands > 0)
    {
        cmd->buttons &= ~IN_DUCK;
        EnginePrediction::Refresh(cmd, local);
        if (!EnginePrediction::Reliable()) { StopCommand(cmd, local, true); return; }
        SendNow();
        starting = false;
        return;
    }
    starting = false;
    // Native CSSO crouch speed is duckSpeed * .8 (client+28B097), not
    // the donor's unscaled value. The provisional DUCK preview is optimistic:
    // deciding from it alone starts crouching one tick too late. Validate the
    // actual UNDUCK command natively, then reserve its remaining crouch ticks.
    const int remaining = chokeLimit - chokedCommands;
    const float optimisticTicks = std::ceil(
        (std::max)(0.f, 1.f - amount) / (interval * speed * .8f));
    if (remaining > 0 && optimisticTicks <= remaining)
    {
        cmd->buttons &= ~IN_DUCK;
        EnginePrediction::Refresh(cmd, local);
        if (!EnginePrediction::Reliable()) { StopCommand(cmd, local, true); return; }
        const float unduckAmount = *reinterpret_cast<const float*>(address + nDuckAmountOffset);
        const float unduckSpeed = *reinterpret_cast<const float*>(address + nDuckSpeedOffset);
        if (!std::isfinite(unduckAmount) || unduckAmount < 0.f || unduckAmount > 1.001f ||
            !std::isfinite(unduckSpeed) || unduckSpeed <= 0.f || unduckSpeed > 8.001f)
        { StopCommand(cmd, local, true); return; }
        const float neededTicks = std::ceil(
            (std::max)(0.f, 1.f - unduckAmount) / (interval * unduckSpeed * .8f));
        if (neededTicks > remaining)
        {
            cmd->buttons |= IN_DUCK;
            EnginePrediction::Refresh(cmd, local);
            if (!EnginePrediction::Reliable()) { StopCommand(cmd, local, true); return; }
        }
    }
    // The release command remains crouched; do not undo the last duck tick
    // just because the provisional crouch preview reached 1.0.
    FakeLag::SetCurrentMoveChoke(chokedCommands < chokeLimit);
}

void FakeDuck::Reset()
{
    if (active || ownsPackets) FakeLag::SetCurrentMoveChoke(false);
    requested = active = ownsPackets = false;
    starting = true;
    owner = 0;
    preparedCommand = lastCommand = inputDuckButtons = 0;
}
