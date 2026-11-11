#pragma once

#include "core/modules/Module.hpp"
#include "sdk/SDK.hpp"

#include <functional>
#include <string>

namespace Modules
{
    // ------------------------------------------------------------------
    //  AdapterModule turns any existing free-function feature into a
    //  Module subclass without rewriting the feature's internals. This is
    //  the migration bridge: new features should implement Module directly,
    //  legacy features can be wrapped here until they are converted.
    // ------------------------------------------------------------------
    class AdapterModule : public Module
    {
    public:
        AdapterModule(const char* name, const char* category = "General", int priority = 100);

        AdapterModule& SetOnLoad(std::function<void()> fn);
        AdapterModule& SetOnUnload(std::function<void()> fn);
        AdapterModule& SetOnCreateMove(std::function<void(SDK::CUserCmd*, SDK::C_BaseEntity*)> fn);
        AdapterModule& SetOnFrameRenderEnd(std::function<void()> fn);
        AdapterModule& SetOnUpdate(std::function<void()> fn);
        AdapterModule& SetOnRender(std::function<void()> fn);
        AdapterModule& SetOnReset(std::function<void()> fn);
        AdapterModule& SetOnFrameStageNotify(std::function<void(int)> fn);

        const char* GetName() const override { return m_name.c_str(); }
        const char* GetCategory() const override { return m_category.c_str(); }
        int GetPriority() const override { return m_priority; }

        void OnLoad() override;
        void OnUnload() override;
        void OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local) override;
        void OnFrameRenderEnd() override;
        void OnUpdate() override;
        void OnRender() override;
        void OnReset() override;
        void OnFrameStageNotify(int stage) override;

    private:
        std::string m_name;
        std::string m_category;
        int m_priority = 100;

        std::function<void()> m_onLoad;
        std::function<void()> m_onUnload;
        std::function<void(SDK::CUserCmd*, SDK::C_BaseEntity*)> m_onCreateMove;
        std::function<void()> m_onFrameRenderEnd;
        std::function<void()> m_onUpdate;
        std::function<void()> m_onRender;
        std::function<void()> m_onReset;
        std::function<void(int)> m_onFrameStageNotify;
    };

    void RegisterBuiltInModules();
} // namespace Modules
