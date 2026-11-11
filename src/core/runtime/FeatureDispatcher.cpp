#include "core/runtime/FeatureDispatcher.hpp"
#include "core/modules/Module.hpp"
#include "core/modules/BuiltInModules.hpp"

#include "sdk/Projection.hpp"
#include <imgui.h>

namespace
{
    struct ModuleRegistrar
    {
        ModuleRegistrar()
        {
            Modules::RegisterBuiltInModules();
            Modules::ModuleManager::Instance().LoadAll();
        }
    } g_registrar;
}

void FeatureDispatcher::OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
{
    Modules::ModuleManager::Instance().DispatchCreateMove(cmd, local);
}

void FeatureDispatcher::OnFrameRenderEnd()
{
    Modules::ModuleManager::Instance().DispatchFrameRenderEnd();
}

void FeatureDispatcher::OnUpdate()
{
    Modules::ModuleManager::Instance().DispatchUpdate();
}

void FeatureDispatcher::OnRender()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    SDK::ScopedWorldProjection projection(display.x, display.y);
    Modules::ModuleManager::Instance().DispatchFrameRenderEnd();
    Modules::ModuleManager::Instance().DispatchRender();
}
