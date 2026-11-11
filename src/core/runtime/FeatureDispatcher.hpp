#pragma once
#include "sdk/UserCommand.hpp"

namespace SDK { class C_BaseEntity; }

// Lifecycle orchestration only. Feature state belongs to each feature module.
namespace FeatureDispatcher
{
    void OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local);
    void OnFrameRenderEnd();
    void OnUpdate();
    void OnRender();
}
