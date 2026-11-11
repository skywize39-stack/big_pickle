#include "core/modules/BuiltInModules.hpp"

#include "sdk/SDK.hpp"

namespace Modules
{
    AdapterModule::AdapterModule(const char* name, const char* category, int priority)
        : m_name(name ? name : "Unnamed"),
          m_category(category ? category : "General"),
          m_priority(priority)
    {
    }

    AdapterModule& AdapterModule::SetOnLoad(std::function<void()> fn)
    {
        m_onLoad = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnUnload(std::function<void()> fn)
    {
        m_onUnload = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnCreateMove(std::function<void(SDK::CUserCmd*, SDK::C_BaseEntity*)> fn)
    {
        m_onCreateMove = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnFrameRenderEnd(std::function<void()> fn)
    {
        m_onFrameRenderEnd = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnUpdate(std::function<void()> fn)
    {
        m_onUpdate = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnRender(std::function<void()> fn)
    {
        m_onRender = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnReset(std::function<void()> fn)
    {
        m_onReset = std::move(fn);
        return *this;
    }
    AdapterModule& AdapterModule::SetOnFrameStageNotify(std::function<void(int)> fn)
    {
        m_onFrameStageNotify = std::move(fn);
        return *this;
    }

    void AdapterModule::OnLoad()
    {
        if (m_onLoad)
            m_onLoad();
    }
    void AdapterModule::OnUnload()
    {
        if (m_onUnload)
            m_onUnload();
    }
    void AdapterModule::OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
    {
        if (m_onCreateMove)
            m_onCreateMove(cmd, local);
    }
    void AdapterModule::OnFrameRenderEnd()
    {
        if (m_onFrameRenderEnd)
            m_onFrameRenderEnd();
    }
    void AdapterModule::OnUpdate()
    {
        if (m_onUpdate)
            m_onUpdate();
    }
    void AdapterModule::OnRender()
    {
        if (m_onRender)
            m_onRender();
    }
    void AdapterModule::OnReset()
    {
        if (m_onReset)
            m_onReset();
    }
    void AdapterModule::OnFrameStageNotify(int stage)
    {
        if (m_onFrameStageNotify)
            m_onFrameStageNotify(stage);
    }
} // namespace Modules
#include "core/events/EventLog.hpp"
#include "features/visuals/grenades/Grenades.hpp"
#include "features/visuals/hud/Hud.hpp"
#include "features/visuals/viewmodel/Viewmodel.hpp"
#include "features/visuals/world/NightMode.hpp"
#include "features/visuals/smoke/NoSmoke.hpp"
#include "features/misc/voicekill/VoiceKill.hpp"
#include "features/misc/clantag/ClanTag.hpp"
#include "ui/binds/KeyBinds.hpp"
#include "features/networking/doubletap/DoubleTap.hpp"
#include "features/networking/doubletap/DefensiveDT.hpp"
#include "features/movement/autopeek/AutoPeek.hpp"

// Re-open namespace for registration helper.
namespace Modules
{
    void RegisterBuiltInModules()
    {
        auto& mgr = ModuleManager::Instance();

        {
            auto m = std::make_unique<AdapterModule>("Hud", "Visuals", 10);
            m->SetOnFrameRenderEnd([]() { Hud::ProjectFrame(); });
            m->SetOnRender([]() { Hud::Render(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("EventLog", "Visuals", 20);
            m->SetOnCreateMove([](SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
            {
                EventLog::UpdateWorldState();
            });
            m->SetOnFrameRenderEnd([]() { EventLog::ProjectFrame(); });
            m->SetOnRender([]() { EventLog::Render(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("Grenades", "Visuals", 30);
            m->SetOnCreateMove([](SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
            {
                Grenades::OnCreateMove(cmd, local);
            });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("Viewmodel", "Visuals", 40);
            m->SetOnUpdate([]() { Viewmodel::Update(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("NightMode", "Visuals", 40);
            m->SetOnUpdate([]() { NightMode::Update(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("NoSmoke", "Visuals", 40);
            m->SetOnUpdate([]() { NoSmoke::Update(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("VoiceKill", "Visuals", 50);
            m->SetOnRender([]() { VoiceKill::Update(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("Binds", "UI", 60);
            m->SetOnRender([]() { Binds::RenderList(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("ClanTag", "Misc", 70);
            m->SetOnCreateMove([](SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
            {
                ClanTag::Update(cmd);
            });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("DoubleTap", "Networking", 10);
            m->SetOnCreateMove([](SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
            {
                DoubleTap::OnCreateMove(cmd, local);
            });
            m->SetOnReset([]() { DoubleTap::Reset(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("DefensiveDT", "Networking", 11);
            m->SetOnCreateMove([](SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
            {
                int maxClients = SDK::g_pEngine ? SDK::g_pEngine->GetMaxClients() : 0;
                int localTeam = local ? local->GetTeamNum() : 0;
                DefensiveDT::OnCreateMove(cmd, local, localTeam, maxClients);
            });
            m->SetOnReset([]() { DefensiveDT::Reset(); });
            mgr.Register(std::move(m));
        }
        {
            auto m = std::make_unique<AdapterModule>("AutoPeek", "Movement", 30);
            m->SetOnReset([]() { AutoPeek::Reset(); });
            mgr.Register(std::move(m));
        }
    }
} // namespace Modules

