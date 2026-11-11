#pragma once

namespace SDK { struct CUserCmd; }

namespace FakeDuck
{
    // Once per normal CL_Move, including moves DT would otherwise skip.
    void UpdateRequest(void* local);
    bool WantsControl();
    bool IsActive();
    bool OwnsPackets();
    void Prepare(SDK::CUserCmd* cmd, void* local);
    void AfterPrediction(SDK::CUserCmd* cmd, void* local);
    void Reset();
}
