#pragma once

#include "sdk/SDK.hpp"

namespace FakeLag
{
    void Run();
    bool HookClMove();
    void UnhookClMove();

    extern bool s_shouldChoke;

    // FakeLag enabled AND not suspended by an armed Double Tap.
    bool IsActive();

    bool CanControlPackets();
    // False while the game window is minimized or in the background (alt-tab).
    // Packet scheduling and DT charge state must stand down there.
    bool GameWindowUsable();
    // Engine commands already waiting for release; -1 when transport is unavailable.
    int GetChokedCommands();
    void SetCurrentMoveChoke(bool choke);

    bool IsPatched();
    void ForceRestore();

    // One-shot request consumed by the next CL_Move.  DT uses it to make the
    // original attack command the first choked command of one CLC_Move burst.
    void RequestChokeCurrentMove();
    void ResetSchedule();
    // Fatality-style fill fake commands (stub).
    void FillFakeCommands();

    // Active only when FakeLag is off, DT is not owning a burst/recharge and
    // Fake Ping owns the verified CL_Move choke schedule.
    bool IsFakePingActive();
    int GetFakePingEffectiveDelayMs();
}
