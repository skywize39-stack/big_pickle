#include "core/modules/Module.hpp"
#include "sdk/SDK.hpp"

#include <algorithm>

namespace Modules
{
    void CallbackRegistry::Register(HookEvent event, std::function<void()> fn, int priority)
    {
        if ( !fn )
            return;
        const std::size_t idx = static_cast<std::size_t>( event );
        if ( idx >= 8 )
            return;
        m_entries[idx].push_back( { priority, std::move( fn ) } );
        std::sort( m_entries[idx].begin(), m_entries[idx].end(),
                   []( const CallbackEntry& a, const CallbackEntry& b )
                   {
                       return a.priority < b.priority;
                   } );
    }

    void CallbackRegistry::Clear(HookEvent event)
    {
        const std::size_t idx = static_cast<std::size_t>( event );
        if ( idx < 8 )
            m_entries[idx].clear();
    }

    void CallbackRegistry::ClearAll()
    {
        for ( auto& v : m_entries )
            v.clear();
    }

    void CallbackRegistry::Invoke(HookEvent event) const
    {
        const std::size_t idx = static_cast<std::size_t>( event );
        if ( idx >= 8 )
            return;
        for ( const auto& e : m_entries[idx] )
        {
            if ( e.fn )
                e.fn();
        }
    }

    ModuleManager& ModuleManager::Instance()
    {
        static ModuleManager instance;
        return instance;
    }

    void ModuleManager::Register(ModulePtr module)
    {
        if ( !module )
            return;
        std::lock_guard<std::mutex> lock( m_mutex );
        m_modules.push_back( std::move( module ) );
        m_sorted = false;
    }

    void ModuleManager::Unregister(const std::string& name)
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_modules.erase(
            std::remove_if( m_modules.begin(), m_modules.end(),
                            [ &name ]( const ModulePtr& m )
                            {
                                return m && m->GetName() == name;
                            } ),
            m_modules.end() );
    }

    void ModuleManager::SetModuleEnabled(const std::string& name, bool enabled)
    {
        for ( auto& m : m_modules )
        {
            if ( m && m->GetName() == name )
            {
                m->SetEnabled(enabled);
                return;
            }
        }
    }

    void ModuleManager::Clear()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        m_modules.clear();
        m_sorted = false;
    }

    void ModuleManager::SortModules()
    {
        if ( m_sorted )
            return;
        std::stable_sort( m_modules.begin(), m_modules.end(),
                          []( const ModulePtr& a, const ModulePtr& b )
                          {
                              if ( !a ) return false;
                              if ( !b ) return true;
                              return a->GetPriority() < b->GetPriority();
                          } );
        m_sorted = true;
    }

    void ModuleManager::LoadAll()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnLoad();
        }
        m_callbacks.Invoke( HookEvent::Load );
    }

    void ModuleManager::UnloadAll()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        for ( auto it = m_modules.rbegin(); it != m_modules.rend(); ++it )
        {
            if ( *it && (*it)->IsEnabled() )
                (*it)->OnUnload();
        }
        m_callbacks.Invoke( HookEvent::Unload );
    }

    void ModuleManager::DispatchCreateMove(SDK::CUserCmd* cmd, SDK::C_BaseEntity* local)
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::CreateMove );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnCreateMove( cmd, local );
        }
    }

    void ModuleManager::DispatchFrameRenderEnd()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::FrameRenderEnd );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnFrameRenderEnd();
        }
    }

    void ModuleManager::DispatchUpdate()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::Update );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnUpdate();
        }
    }

    void ModuleManager::DispatchRender()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::Render );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnRender();
        }
    }

    void ModuleManager::DispatchReset()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::Reset );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnReset();
        }
    }

    void ModuleManager::DispatchFrameStageNotify(int stage)
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        SortModules();
        m_callbacks.Invoke( HookEvent::FrameStageNotify );
        for ( auto& m : m_modules )
        {
            if ( m && m->IsEnabled() )
                m->OnFrameStageNotify( stage );
        }
    }

    Module* ModuleManager::GetModule(const std::string& name)
    {
        for ( auto& m : m_modules )
        {
            if ( m && m->GetName() == name )
                return m.get();
        }
        return nullptr;
    }
} // namespace Modules
