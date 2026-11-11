#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SDK
{
    class CUserCmd;
    class C_BaseEntity;
}

namespace Modules
{
    /**
     * @brief Hook events that a module may react to.
     */
    enum class HookEvent
    {
        Load,
        Unload,
        CreateMove,
        FrameRenderEnd,
        Update,
        Render,
        Reset,
        FrameStageNotify
    };

    /**
     * @brief Base class for every cheat feature exposed as a module.
     */
    class Module
    {
    public:
        virtual ~Module() = default;

        virtual const char* GetName() const = 0;
        virtual const char* GetCategory() const { return "General"; }
        virtual int GetPriority() const { return 100; }
        virtual bool IsEnabled() const { return m_enabled; }
        virtual void SetEnabled(bool enabled) { m_enabled = enabled; }
    protected:
        bool m_enabled = true;
    public:

        virtual void OnLoad() {}
        virtual void OnUnload() {}

        virtual void OnCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local) {}
        virtual void OnFrameRenderEnd() {}
        virtual void OnUpdate() {}
        virtual void OnRender() {}
        virtual void OnReset() {}
        virtual void OnFrameStageNotify(int stage) {}
    };

    using ModulePtr = std::unique_ptr<Module>;

    struct CallbackEntry
    {
        int priority = 100;
        std::function<void()> fn;
    };

    class CallbackRegistry
    {
    public:
        void Register(HookEvent event, std::function<void()> fn, int priority = 100);
        void Clear(HookEvent event);
        void ClearAll();
        void Invoke(HookEvent event) const;

    private:
        std::vector<CallbackEntry> m_entries[8];
    };

    class ModuleManager
    {
    public:
        static ModuleManager& Instance();

        void Register(ModulePtr module);
        void Unregister(const std::string& name);
        void Clear();

        void LoadAll();
        void UnloadAll();

        void DispatchCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local);
        void DispatchFrameRenderEnd();
        void DispatchUpdate();
        void DispatchRender();
        void DispatchReset();
        void DispatchFrameStageNotify(int stage);

        void SetModuleEnabled(const std::string& name, bool enabled);
        Module* GetModule(const std::string& name);
        const std::vector<ModulePtr>& GetModules() const { return m_modules; }
        std::size_t GetModuleCount() const { return m_modules.size(); }

        CallbackRegistry& GetCallbacks() { return m_callbacks; }

    private:
        ModuleManager() = default;
        void SortModules();

        std::vector<ModulePtr> m_modules;
        bool m_sorted = false;
        CallbackRegistry m_callbacks;
        std::mutex m_mutex;
    };

} // namespace Modules
