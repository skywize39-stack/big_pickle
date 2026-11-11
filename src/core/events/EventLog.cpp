#include "core/events/EventLog.hpp"
#include "sdk/SDK.hpp"
#include "sdk/WorldEntity.hpp"
#include "config/Config.hpp"
#include "platform/windows/Platform.hpp"
#include "ui/menu/Menu.hpp"
#include "core/prediction/EnginePrediction.hpp"

#include <Windows.h>
#include <imgui.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#define ELOG( ... ) DBGLOG( "[logs] " __VA_ARGS__ )
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // Consecutive "spread" misses per player (adaptive hitchance, rage).
    std::atomic<int> g_spreadMisses[ 65 ]{};

    // Consecutive record-driven "unknown pose/timing" misses per player.
    // While the counter passes the threshold, Rage drops that player's lag
    // records for the tick and scans only the live snapshot: the rewind
    // pose clearly stopped matching the server's, and another record shot
    // re-misses into the same gap.
    std::atomic<int> g_poseMisses[ 65 ]{};

    // ---- Bomb timer state (written by FireGameEvent, read by Render) ----
    std::atomic<bool>      g_bombActive{ false };
    std::atomic<bool>      g_bombDefused{ false };
    std::atomic<ULONGLONG> g_bombPlantTick{ 0 };     // GetTickCount64() at plant
    std::atomic<ULONGLONG> g_bombDefuseTick{ 0 };
    std::atomic<float>     g_bombC4Timer{ 40.f };
    std::atomic<bool> g_bombNative{false};
    std::atomic<float> g_bombSeconds{0.f}, g_defuseSeconds{0.f}, g_defuseLength{0.f};    // mp_c4timer cached at plant

    float ReadC4Timer()
    {
        float c4time = 40.f;
        __try
        {
            auto* pCvar = SDK::ICvar::Get();
            if ( !pCvar ) return c4time;
            auto* cv = pCvar->FindConVar( "mp_c4timer" );
            if ( !cv ) return c4time;
            // Walk +0x38 parent link to the root ConVar (same as FeatureDispatcher.cpp).
            uintptr_t cur = reinterpret_cast<uintptr_t>( cv );
            for ( int d = 0; cur && d < 8; ++d )
            {
                const uintptr_t parent = *reinterpret_cast<uintptr_t*>( cur + 0x38 );
                if ( !parent || parent == cur ) break;
                cur = parent;
            }
            // Float at +0x54 is XOR'd with the low 32 bits of the root address.
            uint32_t bits = *reinterpret_cast<uint32_t*>( cur + 0x54 ) ^ static_cast<uint32_t>( cur );
            float v; memcpy( &v, &bits, 4 );
            if ( v >= 5.f && v <= 300.f ) c4time = v;
        }
        __except ( EXCEPTION_EXECUTE_HANDLER ) {}
        return c4time;
    }
}

namespace
{
    struct Line
    {
        std::string prefix;  // colored part ("hit", "miss", "hurt")
        ImU32       color;
        std::string text;
        ULONGLONG   time;
    };

    struct PendingShot
    {
        unsigned long long id = 0;
        int commandNumber = 0;
        bool chargeCommand = false;
        bool fired = false;
        bool weaponFired = false;
        ULONGLONG fireTime = 0;
        int       target = 0;
        int       hitgroup = -1;
        float     hitchance = 0.f;
        float     damage = 0.f;
        Vec3      eye{};
        Vec3      point{};
        bool      fromRecord = false;
        int       backtrackTicks = 0;
        ULONGLONG time = 0;
        bool      hasImpact = false;
        Vec3      impact{};      // farthest impact of this shot
        bool      hurt = false;  // player_hurt received
        bool      targetDied = false;
    };

    constexpr size_t    kMaxLines   = 10;
    constexpr ULONGLONG kLineLife   = 5000; // ms on screen
    constexpr ULONGLONG kLineFade   = 500;
    constexpr ULONGLONG kShotWindow = 900;  // ms to wait for server events

    struct Tracer { Vec3 start; Vec3 end; ULONGLONG time; unsigned long long fireSerial; };
    unsigned long long g_fireSerial = 0;
    Vec3 g_fireEye{};
    bool g_fireEyeValid=false;
    ULONGLONG g_fireTime=0;
    struct Marker { Vec3 pos; int dmg; bool head; bool kill; ULONGLONG time; };
    constexpr ULONGLONG kTracerLife = 2500;
    constexpr ULONGLONG kMarkerLife = 1200;
    std::deque<Tracer>      g_tracers;
    std::deque<Marker>      g_markers;
    Vec3      g_lastImpact{};
    ULONGLONG g_lastImpactTime = 0;
    ULONGLONG g_screenMarkerTime = 0;

    std::mutex              g_mtx;
    // Diagnostics (shown while the menu is open).
    int g_cntRaw = 0, g_localUid = 0; int g_cntEvents = 0, g_cntHurt = 0, g_cntImpact = 0, g_cntProj = 0, g_cntShots = 0;
    bool g_initOk = false;
    std::deque<Line>        g_lines;
    std::deque<PendingShot> g_shots;
    unsigned long long g_nextShotId = 0, g_activeShotId = 0;

    // A request is NOT a server-confirmed shot. Persist both for live diagnosis.
    void AuditShot(const char* stage, const PendingShot& s)
    {
        static FILE* f = nullptr;
        static bool tried = false;
        static unsigned rows = 0;
        if (!tried)
        {
            tried = true;
            char path[1024]{};
            GetModuleFileNameA(nullptr, path, sizeof(path));
            path[sizeof(path)-1] = 0;
            char* slash = std::strrchr(path, '\\');
            if (!slash) slash = std::strrchr(path, '/');
            if (slash) *(slash + 1) = 0; else path[0] = 0;
            const std::string filename = std::string(path) + "csso_shot_audit.csv";
            f = std::fopen(filename.c_str(), "w");
            if (f) std::fprintf(f, "stage,time_ms,id,cmd,target,hg,hc,damage,bt,bt_ticks,fired,weapon_fire,eye_x,eye_y,eye_z,aim_x,aim_y,aim_z,impact_x,impact_y,impact_z,has_impact,hurt\n");
        }
        if (!f || rows++ >= 20000) return;
        std::fprintf(f, "%s,%llu,%llu,%d,%d,%d,%.3f,%.3f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d\n",
            stage, GetTickCount64(), s.id, s.commandNumber, s.target, s.hitgroup,
            s.hitchance, s.damage, int(s.fromRecord), s.backtrackTicks, int(s.fired), int(s.weaponFired),
            s.eye.x,s.eye.y,s.eye.z,s.point.x,s.point.y,s.point.z,
            s.impact.x,s.impact.y,s.impact.z,int(s.hasImpact),int(s.hurt));
        std::fflush(f);
    }

    PendingShot* ActiveShot()
    {
        for (auto& s : g_shots) if (s.id == g_activeShotId) return &s;
        return nullptr;
    }
    PendingShot* ConfirmRequest(ULONGLONG now, bool weaponEvent)
    {
        // Only pair evidence with a pending request in the event window.
        for (auto& s : g_shots)
        {
            if (now - s.time > kShotWindow || s.hurt) continue;
            if (weaponEvent ? s.weaponFired : s.fired) continue;
            s.fired = true;
            s.weaponFired = s.weaponFired || weaponEvent;
            if (!s.fireTime) s.fireTime = now;
            g_activeShotId = s.id;
            AuditShot(weaponEvent ? "weapon_fire" : "evidence", s);
            return &s;
        }
        // A manual/untracked shot must not steal the previous rage request.
        g_activeShotId = 0;
        return nullptr;
    }

    bool DuplicateEvent(const std::string& sig, bool server, ULONGLONG now)
    {
        struct Seen { std::string sig; bool server; bool paired; ULONGLONG time; };
        static std::deque<Seen> seen;
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        while (!seen.empty() && now - seen.front().time > 100) seen.pop_front();
        for (auto& x : seen)
            if (!x.paired && x.server != server && x.sig == sig)
            { x.paired = true; return true; }
        seen.push_back({sig, server, false, now});
        if (seen.size() > 256) seen.pop_front();
        return false;
    }


    const char* HitgroupName( int hg )
    {
        switch ( hg )
        {
        case 1:  return "head";
        case 2:  return "chest";
        case 3:  return "stomach";
        case 4:  return "left arm";
        case 5:  return "right arm";
        case 6:  return "left leg";
        case 7:  return "right leg";
        case 10: return "gear";
        default: return "body";
        }
    }

    // player_info_t: name[32] then int userid at +32 (see KillSay).
    int UserIdOf( int index )
    {
        if ( !SDK::g_pEngine || index <= 0 )
            return 0;
        SDK::player_info_t info{};
        if ( !SDK::g_pEngine->GetPlayerInfo( index, &info ) )
            return 0;
        return *reinterpret_cast<const int*>( reinterpret_cast<const char*>( &info ) + 32 );
    }

    int IndexOfUserId( int userid )
    {
        if ( userid <= 0 )
            return 0;
        for ( int i = 1; i <= 64; ++i )
            if ( UserIdOf( i ) == userid )
                return i;
        return 0;
    }

    std::string NameOf( int index )
    {
        if ( !SDK::g_pEngine || index <= 0 )
            return "?";
        SDK::player_info_t info{};
        if ( !SDK::g_pEngine->GetPlayerInfo( index, &info ) || !info.name[ 0 ] )
            return "?";
        info.name[ 31 ] = 0;
        return info.name;
    }

    struct ConsoleFilterState
    {
        SDK::IConVar* vars[3]{};
        std::string saved[3];
        bool active = false;
    } g_consoleFilter;

    const char* ConsoleVarString(SDK::IConVar* var)
    {
        // engine.dll console filter at +0xD6723/+0xD6748 reads root+0x48;
        // +0x38 is the parent pointer. Do not write encoded ConVar storage.
        __try
        {
            uintptr_t root = reinterpret_cast<uintptr_t>(var);
            for (int i = 0; root && i < 8; ++i)
            {
                const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(root + 0x38);
                if (!parent || parent == root)
                    return *reinterpret_cast<const char* const*>(root + 0x48);
                root = parent;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return nullptr;
    }

    bool EnableConsoleFilter(SDK::IConVar* enable, SDK::IConVar* text, SDK::IConVar* out)
    {
        if (g_consoleFilter.active) return true;
        SDK::IConVar* vars[] = {enable, text, out};
        std::string saved[3];
        for (int i = 0; i < 3; ++i)
        {
            if (!vars[i]) return false;
            const char* value = ConsoleVarString(vars[i]);
            if (!value) return false;
            saved[i] = value;
        }
        for (int i = 0; i < 3; ++i)
        {
            g_consoleFilter.vars[i] = vars[i];
            g_consoleFilter.saved[i] = saved[i];
        }
        // Mode 1 discards nonmatching spew (mode 2 only dims it).
        text->SetString("[big pickle]");
        out->SetString("");
        enable->SetInt(1);
        g_consoleFilter.active = true;
        return true;
    }

    void RestoreConsoleFilter()
    {
        if (!g_consoleFilter.active) return;
        // Remove our filter while restoring strings, then restore old mode.
        g_consoleFilter.vars[0]->SetInt(0);
        g_consoleFilter.vars[1]->SetString(g_consoleFilter.saved[1].c_str());
        g_consoleFilter.vars[2]->SetString(g_consoleFilter.saved[2].c_str());
        g_consoleFilter.vars[0]->SetString(g_consoleFilter.saved[0].c_str());
        g_consoleFilter.active = false;
    }

    void UpdateConsoleFilter()
    {
        if (!g_config.settings.visuals.others.event_logs_console)
        {
            RestoreConsoleFilter();
            return;
        }
        if (g_consoleFilter.active) return;
        auto* cvar = SDK::ICvar::Get();
        if (!cvar) return;
        EnableConsoleFilter(cvar->FindConVar("con_filter_enable"),
            cvar->FindConVar("con_filter_text"), cvar->FindConVar("con_filter_text_out"));
    }

    struct ConsoleColor { unsigned char r, g, b, a; };
    static_assert(sizeof(ConsoleColor) == 4, "tier0 Color layout");
    using ConsoleColorPrintFn = void (__cdecl*)(const ConsoleColor&, const char*, ...);
    ConsoleColorPrintFn g_consoleColorPrint = nullptr;
    using ConsolePrintFn = void (__cdecl*)(const char*, ...);
    ConsolePrintFn g_consolePrint = nullptr;

    bool ResolveConsolePrint()
    {
        if (g_consolePrint || g_consoleColorPrint) return true;
        // engine.dll imports these exact tier0 exports. No debug console,
        // developer cvar or local/server command dispatch is involved.
        const HMODULE tier0 = GetModuleHandleA("tier0.dll");
        if (!tier0) return false;
        g_consoleColorPrint = reinterpret_cast<ConsoleColorPrintFn>(
            GetProcAddress(tier0, "?ConColorMsg@@YAXAEBVColor@@PEBDZZ"));
        g_consolePrint = reinterpret_cast<ConsolePrintFn>(
            GetProcAddress(tier0, "?ConMsg@@YAXPEBDZZ"));
        if (!g_consolePrint)
            g_consolePrint = reinterpret_cast<ConsolePrintFn>(GetProcAddress(tier0, "Msg"));
        return g_consolePrint != nullptr || g_consoleColorPrint != nullptr;
    }

    void AddLine( const char* prefix, ImU32 color, const std::string& text )
    {
        // caller holds g_mtx
        g_lines.push_back( { prefix, color, text, GetTickCount64() } );
        while ( g_lines.size() > kMaxLines )
            g_lines.pop_front();

        if (g_config.settings.visuals.others.event_logs_console && ResolveConsolePrint())
        {
            // Names/log text are DATA, not commands or printf format strings.
            std::string clean = text;
            for (char& c : clean) if (c == '\n' || c == '\r') c = ' ';
            if (g_consoleColorPrint)
            {
                const ConsoleColor tint{
                    static_cast<unsigned char>((color >> IM_COL32_R_SHIFT) & 255),
                    static_cast<unsigned char>((color >> IM_COL32_G_SHIFT) & 255),
                    static_cast<unsigned char>((color >> IM_COL32_B_SHIFT) & 255), 255};
                // One complete colored message retains the filter tag.
                g_consoleColorPrint(tint, "[big pickle] %s %s\n", prefix, clean.c_str());
            }
            else if (g_consolePrint)
                g_consolePrint("[big pickle] %s %s\n", prefix, clean.c_str());
        }
    }

    float Len( const Vec3& v ) { return sqrtf( v.x * v.x + v.y * v.y + v.z * v.z ); }

    std::string BacktrackInfo(const PendingShot& s)
    {
        if (!s.fromRecord) return {};
        char text[48];
        sprintf_s(text, "bt: %d ticks", s.backtrackTicks);
        return text;
    }

    // Resolve a shot that got no player_hurt.
    void ResolveMiss( const PendingShot& s, bool print )
    {
        const std::string name = NameOf( s.target );
        const char* reason = "unknown";

        if ( s.targetDied )
            reason = "death";
        else if ( s.hasImpact )
        {
            const Vec3 toAim{ s.point.x - s.eye.x, s.point.y - s.eye.y, s.point.z - s.eye.z };
            const Vec3 toImp{ s.impact.x - s.eye.x, s.impact.y - s.eye.y, s.impact.z - s.eye.z };
            const float aimDist = Len( toAim );
            const float impDist = Len( toImp );

            if ( impDist + 16.f < aimDist )
                reason = "possible occlusion";
            else if ( aimDist > 1.f && impDist > 1.f )
            {
                // Perpendicular distance between the bullet ray and the
                // aimpoint, measured at the aimpoint's depth.
                const Vec3 d{ toImp.x / impDist, toImp.y / impDist, toImp.z / impDist };
                const float t = toAim.x * d.x + toAim.y * d.y + toAim.z * d.z;
                const Vec3 closest{ d.x * t - toAim.x, d.y * t - toAim.y, d.z * t - toAim.z };
                const float off = Len( closest );
                // Smallest capsule radius in CSS player models is ~3.2u.
                reason = off > 3.5f ? "possible spread" : "unknown pose/timing";
            }
        }
        // Endpoint geometry alone cannot establish spread versus a bad pose.
        // Both heuristic labels still feed their own per-player streak
        // counters: spread misses raise adaptive hitchance (internal_hvh
        // rule), and repeated record pose/timing misses pause further
        // record shots on this player — they never re-miss into the same
        // broken rewind. Live-pose shots are unaffected by the pause.
        if ( s.fired && s.target > 0 && s.target < 65 )
        {
            if ( !std::strcmp( reason, "possible spread" ) )
            {
                int v = g_spreadMisses[ s.target ] + 1;
                if ( v <= 64 ) g_spreadMisses[ s.target ] = v;
            }
            else if ( !std::strcmp( reason, "unknown pose/timing" ) && s.fromRecord )
            {
                int v = g_poseMisses[ s.target ] + 1;
                if ( v <= 64 ) g_poseMisses[ s.target ] = v;
            }
        }
        if ( !print )
            return;

        const std::string bt = BacktrackInfo(s);
        const std::string suffix = bt.empty() ? "" : ", " + bt;
        char buf[ 256 ];
        sprintf_s( buf, "shot to %s's %s due to %s (hc: %d%%, dmg: %d%s)",
            name.c_str(), HitgroupName( s.hitgroup ), reason,
            static_cast<int>( s.hitchance + 0.5f ), static_cast<int>( s.damage + 0.5f ),
            suffix.c_str() );
        AddLine( "miss", IM_COL32( 255, 90, 90, 255 ), buf );
    }

    void ExpireShotRequests(ULONGLONG now)
    {
        // Resolve shots whose server-event window has passed.
        while ( !g_shots.empty() && now - (g_shots.front().fired ? g_shots.front().fireTime : g_shots.front().time) > kShotWindow )
        {
            const PendingShot s = g_shots.front();
            g_shots.pop_front();
            AuditShot(s.hurt ? "resolved_hit" : (s.fired ? "resolved_miss" : "unconfirmed"), s);
            if ( s.fired && !s.hurt )
                ResolveMiss( s, g_config.settings.visuals.others.event_logs ||
                    g_config.settings.visuals.others.event_logs_console );
        }

    }

    // Screen-space cache built in ProjectFrame (frame-coherent matrix).
    constexpr int kTracerSegs = 12;
    struct TracerScreen { Vec2 p[ kTracerSegs + 1 ]; bool ok[ kTracerSegs + 1 ]; float alpha; };
    struct MarkerScreen { Vec2 p; bool ok; int dmg; bool head; bool kill; float age; };
    std::vector<TracerScreen> g_tracerScr;
    std::vector<MarkerScreen> g_markerScr;

    void ProjectWorld( ULONGLONG now )
    {
        // caller holds g_mtx
        while ( !g_tracers.empty() && now - g_tracers.front().time > kTracerLife )
            g_tracers.pop_front();
        while ( !g_markers.empty() && now - g_markers.front().time > kMarkerLife )
            g_markers.pop_front();
        g_tracerScr.clear();
        g_markerScr.clear();

        if ( !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
        {
            g_tracers.clear();
            g_markers.clear();
            return;
        }

        for ( const Tracer& t : g_tracers )
        {
            TracerScreen ts{};
            ts.alpha = 1.f - static_cast<float>( now - t.time ) / kTracerLife;
            // Start a bit in front of the eye: the eye itself sits on the
            // near plane and its projection flips in and out every frame.
            Vec3 d{ t.end.x - t.start.x, t.end.y - t.start.y, t.end.z - t.start.z };
            const float len = Len( d );
            if ( len < 40.f )
                continue;
            const Vec3 s0{ t.start.x + d.x / len * 24.f, t.start.y + d.y / len * 24.f,
                           t.start.z + d.z / len * 24.f };
            // Exact homogeneous near-plane + screen-edge clipping avoids
            // popping between coarse segments when the beam crosses the camera.
            if(SDK::g_worldProjection.active)
            {
                ts.ok[0]=ts.ok[1]=SDK::ProjectWorldLine(s0,t.end,ts.p[0],ts.p[1]);
                g_tracerScr.push_back(ts);
                continue;
            }
            for ( int i = 0; i <= kTracerSegs; ++i )
            {
                const float f = static_cast<float>( i ) / kTracerSegs;
                const Vec3 w{ s0.x + ( t.end.x - s0.x ) * f, s0.y + ( t.end.y - s0.y ) * f,
                              s0.z + ( t.end.z - s0.z ) * f };
                ts.ok[ i ] = SDK::WorldToScreen( w, ts.p[ i ] );
            }
            g_tracerScr.push_back( ts );
        }

        for ( const Marker& m : g_markers )
        {
            MarkerScreen ms{};
            ms.ok = SDK::WorldToScreen( m.pos, ms.p );
            ms.dmg = m.dmg; ms.head = m.head; ms.kill = m.kill;
            ms.age = static_cast<float>( now - m.time );
            g_markerScr.push_back( ms );
        }
    }

    void DrawWorld( ULONGLONG now )
    {
        // caller holds g_mtx
        const auto& o = g_config.settings.visuals.others;
        ImDrawList* dl = ImGui::GetForegroundDrawList();

        if ( o.bullet_tracers )
        {
            for ( const TracerScreen& t : g_tracerScr )
            {
                const ImU32 c = IM_COL32( 255, 255, 255, static_cast<int>( t.alpha * 200.f ) );
                for ( int i = 0; i < kTracerSegs; ++i )
                    if ( t.ok[ i ] && t.ok[ i + 1 ] )
                        dl->AddLine( { t.p[ i ].x, t.p[ i ].y }, { t.p[ i + 1 ].x, t.p[ i + 1 ].y }, c, 1.f );
            }
        }

        if ( o.hitmarker )
        {
            for ( const MarkerScreen& m : g_markerScr )
            {
                if ( !m.ok )
                    continue;
                const float a = 1.f - m.age / kMarkerLife;
                const int al = static_cast<int>( ( a < 0.f ? 0.f : a ) * 255.f );
                const float g = 3.f, l = 5.f;
                const ImU32 c = IM_COL32( 255, 255, 255, al );
                const Vec2& sp = m.p;
                dl->AddLine( { sp.x - g, sp.y - g }, { sp.x - g - l, sp.y - g - l }, c, 1.f );
                dl->AddLine( { sp.x + g, sp.y - g }, { sp.x + g + l, sp.y - g - l }, c, 1.f );
                dl->AddLine( { sp.x - g, sp.y + g }, { sp.x - g - l, sp.y + g + l }, c, 1.f );
                dl->AddLine( { sp.x + g, sp.y + g }, { sp.x + g + l, sp.y + g + l }, c, 1.f );

                if ( o.hitmarker_damage )
                {
                    char t[ 16 ];
                    sprintf_s( t, "%d", m.dmg );
                    const ImU32 dc = m.kill ? IM_COL32( 255, 60, 60, al ) :
                        ( m.head ? IM_COL32( 255, 200, 60, al ) : IM_COL32( 255, 255, 255, al ) );
                    const float rise = m.age / kMarkerLife * 40.f;
                    const ImVec2 ts = ImGui::CalcTextSize( t );
                    const ImVec2 tp{ sp.x - ts.x * 0.5f, sp.y - 22.f - rise };
                    dl->AddText( { tp.x + 1.f, tp.y + 1.f }, IM_COL32( 0, 0, 0, al ), t );
                    dl->AddText( tp, dc, t );
                }
            }

            if ( now - g_screenMarkerTime < 250 )
            {
                const float a = 1.f - ( now - g_screenMarkerTime ) / 250.f;
                const ImVec2 c{ ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f };
                const ImU32 col = IM_COL32( 255, 255, 255, static_cast<int>( a * 255.f ) );
                const float g = 4.f, l = 5.f;
                dl->AddLine( { c.x - g, c.y - g }, { c.x - g - l, c.y - g - l }, col, 1.f );
                dl->AddLine( { c.x + g, c.y - g }, { c.x + g + l, c.y - g - l }, col, 1.f );
                dl->AddLine( { c.x - g, c.y + g }, { c.x - g - l, c.y + g + l }, col, 1.f );
                dl->AddLine( { c.x + g, c.y + g }, { c.x + g + l, c.y + g + l }, col, 1.f );
            }
        }
    }

    class Listener final : public SDK::IGameEventListener
    {
        bool serverSide;
    public:
        explicit Listener(bool server) : serverSide(server) {}
        void FireGameEvent( SDK::IGameEvent* ev ) override
        {
            if ( !ev || !SDK::g_pEngine )
                return;
            const char* n = ev->GetName();
            if ( !n )
                return;

            // Pair only opposite-listener copies. Repeated same-source events
            // (rapid fire / identical surface contacts) remain distinct.
            char sig[256];
            sprintf_s(sig, "%s|%d|%d|%d|%d|%d|%.9g|%.9g|%.9g|%s", n,
                ev->GetInt("userid"), ev->GetInt("attacker"),
                ev->GetInt("dmg_health"), ev->GetInt("health"), ev->GetInt("hitgroup"),
                ev->GetFloat("x"), ev->GetFloat("y"), ev->GetFloat("z"),
                ev->GetString("weapon"));
            if (DuplicateEvent(sig, serverSide, GetTickCount64())) return;

            ++g_cntRaw;
            if ( !std::strcmp( n, "bomb_planted" ) )
            {
                g_bombC4Timer.store( ReadC4Timer() );
                g_bombPlantTick.store( GetTickCount64() );
                g_bombDefused.store( false );
                g_bombActive.store( true );
                ELOG( "bomb_planted: c4timer=%.1f", g_bombC4Timer.load() );
            }

            if ( !std::strcmp( n, "bomb_defused" ) )
            {
                g_bombDefused.store( true );
                g_bombDefuseTick.store( GetTickCount64() );
            }

            if ( !std::strcmp( n, "bomb_exploded" ) || !std::strcmp( n, "round_end" ) ||
                 !std::strcmp( n, "round_start" ) )
            {
                g_bombActive.store( false );
                g_bombDefused.store( false );
            }

            const int localIdx = SDK::g_pEngine->GetLocalPlayer();
            const int localUid = UserIdOf( localIdx );
            g_localUid = localUid;
            ELOG( "event %s userid=%d attacker=%d dmg=%d (local idx=%d uid=%d)", n,
                ev->GetInt( "userid" ), ev->GetInt( "attacker" ), ev->GetInt( "dmg_health" ),
                localIdx, localUid );
            if ( localUid <= 0 )
                return;

            std::lock_guard<std::mutex> lock( g_mtx );
            ++g_cntEvents;
            if ( !std::strcmp( n, "bullet_impact" ) ) ++g_cntImpact;
            if ( !std::strcmp( n, "player_hurt" ) ) ++g_cntHurt;

            if (!std::strcmp(n, "weapon_fire"))
            {
                if (ev->GetInt("userid") == localUid)
                {
                    ++g_fireSerial;
                    PendingShot* request=ConfirmRequest(GetTickCount64(), true);
                    g_fireTime=GetTickCount64();g_fireEyeValid=false;
                    if(request){g_fireEye=request->eye;g_fireEyeValid=true;}
                    else if(SDK::g_pEntityList)
                    {
                        auto* local=reinterpret_cast<SDK::C_BaseEntity*>(SDK::g_pEntityList->GetClientEntity(localIdx));
                        if(local){g_fireEye=local->EyePosition();g_fireEyeValid=true;}
                    }
                }
                return;
            }

            if ( !std::strcmp( n, "bullet_impact" ) )
            {
                if ( ev->GetInt( "userid" ) != localUid )
                    return;
                const Vec3 p{ ev->GetFloat( "x" ), ev->GetFloat( "y" ), ev->GetFloat( "z" ) };
                g_lastImpact = p;
                g_lastImpactTime = GetTickCount64();
                PendingShot* shot = ActiveShot();
                // Without weapon_fire, impact can confirm a request, but only
                // a short surface batch is grouped. Attribution is best effort.
                if (!shot || (!shot->weaponFired && g_lastImpactTime - shot->fireTime > 30))
                    shot = ConfirmRequest(g_lastImpactTime, false);
                if ( g_config.settings.visuals.others.bullet_tracers && SDK::g_pEntityList )
                {
                    void* local = SDK::g_pEntityList->GetClientEntity( localIdx );
                    if ( local )
                    {
                        // Penetration emits one impact per surface: keep a
                        // single beam per shot by extending a fresh one.
                        const Vec3 eye = shot ? shot->eye :
                            (g_fireEyeValid ? g_fireEye : reinterpret_cast<SDK::C_BaseEntity*>(local)->EyePosition());
                        if ( !g_tracers.empty() && g_tracers.back().fireSerial == g_fireSerial &&
                            ((g_fireSerial>0 && g_lastImpactTime-g_fireTime<=kShotWindow) ||
                             g_lastImpactTime-g_tracers.back().time<30) )
                        {
                            Tracer& t = g_tracers.back();
                            const Vec3 a{ p.x - t.start.x, p.y - t.start.y, p.z - t.start.z };
                            const Vec3 b{ t.end.x - t.start.x, t.end.y - t.start.y, t.end.z - t.start.z };
                            if ( Len( a ) > Len( b ) ) t.end = p;
                        }
                        else
                        {
                            g_tracers.push_back( { eye, p, g_lastImpactTime, g_fireSerial } );
                            while ( g_tracers.size() > 24 ) g_tracers.pop_front();
                        }
                    }
                }
                if (shot)
                {
                    const Vec3 a{p.x-shot->eye.x,p.y-shot->eye.y,p.z-shot->eye.z};
                    const Vec3 b{shot->impact.x-shot->eye.x,shot->impact.y-shot->eye.y,shot->impact.z-shot->eye.z};
                    if (!shot->hasImpact || Len(a) > Len(b)) shot->impact = p;
                    shot->hasImpact = true;
                    AuditShot("impact", *shot);
                }
                return;
            }

            if ( !std::strcmp( n, "player_hurt" ) )
            {
                const int attacker = ev->GetInt( "attacker" );
                const int victim   = ev->GetInt( "userid" );
                const int dmg      = ev->GetInt( "dmg_health" );
                const int hp       = ev->GetInt( "health" );
                const int hg       = ev->GetInt( "hitgroup" );

                if ( attacker == localUid && victim != localUid &&
                     g_config.settings.visuals.others.hitmarker )
                {
                    const ULONGLONG now = GetTickCount64();
                    g_screenMarkerTime = now;
                    Vec3 pos = g_lastImpact;
                    if ( now - g_lastImpactTime > 300 )
                    {
                        // No impact this shot: fall back to victim chest.
                        void* ent = SDK::g_pEntityList ?
                            SDK::g_pEntityList->GetClientEntity( IndexOfUserId( victim ) ) : nullptr;
                        if ( ent )
                        {
                            pos = *reinterpret_cast<const Vec3*>(
                                reinterpret_cast<uintptr_t>( ent ) + SDK::nOriginOffset );
                            pos.z += 48.f;
                        }
                    }
                    g_markers.push_back( { pos, dmg, hg == 1, hp <= 0, now } );
                    while ( g_markers.size() > 16 ) g_markers.pop_front();
                }

                if ( attacker == localUid && victim != localUid )
                {
                    // A hit clears the adaptive-hitchance spread streak and
                    // any open record-pose pause for this victim.
                    const int hIdx = IndexOfUserId( victim );
                    if ( hIdx > 0 && hIdx < 65 )
                    {
                        g_spreadMisses[ hIdx ] = 0;
                        g_poseMisses[ hIdx ] = 0;
                    }
                }

                std::string extra;
                if (attacker == localUid && victim != localUid)
                {
                    const int vIdx = IndexOfUserId(victim);
                    PendingShot* hit = ActiveShot();
                    if (!hit || hit->target != vIdx || hit->hurt) hit = nullptr;
                    if (!hit) for (auto i = g_shots.rbegin(); i != g_shots.rend(); ++i)
                        if (i->fired && !i->hurt && i->target == vIdx) { hit = &*i; break; }
                    // Hurt alone is evidence when fire/impact events are missing.
                    if (!hit) for (auto& s : g_shots)
                        if (!s.fired && s.target == vIdx && GetTickCount64()-s.time <= kShotWindow)
                        { hit = &s; break; }
                    if (hit)
                    {
                        hit->fired = true;
                        if (!hit->fireTime) hit->fireTime = GetTickCount64();
                        hit->hurt = true;
                        AuditShot("hurt", *hit);
                        if (hit->hitgroup != hg && hit->hitgroup >= 0)
                        {
                            char m[64];
                            sprintf_s(m, " [aimed: %s, hc: %d%%]", HitgroupName(hit->hitgroup), int(hit->hitchance+.5f));
                            extra = m;
                        }
                        const std::string bt = BacktrackInfo(*hit);
                        if (!bt.empty()) extra += " [" + bt + "]";
                    }
                }
                if (!g_config.settings.visuals.others.event_logs &&
                    !g_config.settings.visuals.others.event_logs_console) return;
                if (attacker == localUid && victim != localUid)
                {
                    const int vIdx = IndexOfUserId(victim);
                    char buf[ 256 ];
                    sprintf_s( buf, "%s in the %s for %d damage (%d health remaining)%s",
                        NameOf( vIdx ).c_str(), HitgroupName( hg ), dmg, hp, extra.c_str() );
                    AddLine( "hit", IM_COL32( 160, 220, 60, 255 ), buf );
                }
                else if ( victim == localUid && attacker != localUid && attacker > 0 )
                {
                    char buf[ 256 ];
                    sprintf_s( buf, "by %s in the %s for %d damage (%d health remaining)",
                        NameOf( IndexOfUserId( attacker ) ).c_str(), HitgroupName( hg ), dmg, hp );
                    AddLine( "hurt", IM_COL32( 255, 200, 60, 255 ), buf );
                }
                return;
            }

            if ( !std::strcmp( n, "player_death" ) )
            {
                const int vIdx = IndexOfUserId( ev->GetInt( "userid" ) );
                for ( auto& s : g_shots )
                    if ( s.target == vIdx && !s.hurt )
                        s.targetDied = true;
                return;
            }

            if ( !std::strcmp( n, "round_start" ) )
            {
                g_shots.clear();
                g_activeShotId = 0;
                g_fireEyeValid=false;g_fireTime=0;g_tracers.clear();
                for ( auto& m : g_spreadMisses ) m = 0;
                for ( auto& m : g_poseMisses )   m = 0;
            }

        }
    };

    // bServerSide=true and false register different records (see KillSay).
    Listener g_server{true};
    Listener g_client{false};
    bool     g_listening = false;

    const char* kEvents[] = { "weapon_fire", "player_hurt", "bullet_impact", "player_death", "round_start",
                              "bomb_planted", "bomb_defused", "bomb_exploded", "round_end" };

}

namespace EventLog
{
    bool Init()
    {
        ResolveConsolePrint();
        UpdateConsoleFilter();
        if ( !SDK::g_pGameEventManager )
        {
            ELOG( "Init: g_pGameEventManager == nullptr -> no events" );
            return false;
        }
        // This engine delivers some events only to the server-side record
        // and others only client-side (see KillSay), so register both and
        // drop the duplicate in FireGameEvent.
        bool ok = false;
        for ( const char* e : kEvents )
        {
            ok |= SDK::g_pGameEventManager->AddListener( &g_client, e, false );
            ok |= SDK::g_pGameEventManager->AddListener( &g_server, e, true );
        }
        g_listening = ok;
        g_initOk = ok;
        ELOG( "Init: gameEventManager=%p listeners=%s", (void*)SDK::g_pGameEventManager, ok ? "OK" : "FAILED" );
        return ok;
    }

    void Shutdown()
    {
        RestoreConsoleFilter();
        if ( g_listening && SDK::g_pGameEventManager )
        {
            SDK::g_pGameEventManager->RemoveListener( &g_client );
            SDK::g_pGameEventManager->RemoveListener( &g_server );
        }
        g_listening = false;
    }

    void OnRageShot( int targetIndex, int hitgroup, float hitchance, float damage,
                     const Vec3& eye, const Vec3& aimPoint, bool fromRecord,
                     int commandNumber, bool chargeCommand, int backtrackTicks )
    {
        // Track command requests separately from fire evidence. Only printed
        // lines depend on event_logs; audit CSV also records unconfirmed requests.
        std::lock_guard<std::mutex> lock( g_mtx );
        ExpireShotRequests(GetTickCount64());
        PendingShot s;
        s.id = ++g_nextShotId;
        s.commandNumber = commandNumber;
        s.chargeCommand = chargeCommand;
        s.target = targetIndex;
        s.hitgroup = hitgroup;
        s.hitchance = hitchance;
        s.damage = damage;
        s.eye = eye;
        s.point = aimPoint;
        s.fromRecord = fromRecord;
        s.backtrackTicks = fromRecord ? (std::max)(0, backtrackTicks) : 0;
        s.time = GetTickCount64();
        ++g_cntShots;
        ELOG( "rage shot -> target=%d hg=%d hc=%.1f", targetIndex, hitgroup, hitchance );
        // R8 wind-up can commit several commands before one real discharge.
        // Update its one unconfirmed request, preserving identity.
        if (chargeCommand && !g_shots.empty() && g_shots.back().chargeCommand &&
            !g_shots.back().fired)
        {
            s.id = g_shots.back().id;
            g_shots.back() = s;
            AuditShot("request_update", s);
            return;
        }
        AuditShot("request", s);
        g_shots.push_back( s );
        while ( g_shots.size() > 16 )
            g_shots.pop_front();
    }

    void Push( const char* fmt, ... )
    {
        char buf[ 256 ];
        va_list va;
        va_start( va, fmt );
        vsnprintf( buf, sizeof( buf ), fmt, va );
        va_end( va );
        std::lock_guard<std::mutex> lock( g_mtx );
        AddLine( "info", IM_COL32( 120, 180, 255, 255 ), buf );
    }

    void ProjectFrame()
    {
        // Apply setting changes on the game thread, not the D3D render thread.
        UpdateConsoleFilter();
        std::lock_guard<std::mutex> lock( g_mtx );
        ++g_cntProj;
        ProjectWorld( GetTickCount64() );
    }

    void UpdateWorldState()
    {
        static bool hadNativeBomb = false;
        if (!SDK::g_pEngine || !SDK::g_pEngine->IsInGame() || !SDK::g_pEntityList)
        {
            hadNativeBomb = false;
            g_bombNative = false; g_bombActive = false; g_bombDefused = false;
            return;
        }
        const float now = SDK::WorldEntity::CurTime();
        if (now < 0.f) return; // retain event fallback if native clock unavailable
        for (int i = 1; i <= 2048; ++i)
        {
            void* ent = SDK::g_pEntityList->GetClientEntity(i);
            const char* name = SDK::WorldEntity::ClassName(ent);
            if (!name || std::strcmp(name, "CPlantedC4")) continue;
            // client.dll RecvProp registrations +0x27EB3..+0x27F8D.
            bool ticking=false, defused=false;
            float blow=0.f, duration=0.f, defuseEnd=0.f, defuseLen=0.f;
            if (!SDK::WorldEntity::Read(ent,0xCE0,ticking) ||
                !SDK::WorldEntity::Read(ent,0xCF4,blow) ||
                !SDK::WorldEntity::Read(ent,0xCF8,duration) ||
                !SDK::WorldEntity::Read(ent,0xD10,defused) ||
                !std::isfinite(blow) || !std::isfinite(duration)) continue;
            if (!ticking && !defused) continue;
            const float remaining=(std::max)(0.f,blow-now);
            if (!defused && remaining<=0.f) continue;
            SDK::WorldEntity::Read(ent,0xD08,defuseLen);
            SDK::WorldEntity::Read(ent,0xD0C,defuseEnd);
            if (defused && !g_bombDefused.load()) g_bombDefuseTick=GetTickCount64();
            g_bombNative=true; g_bombActive=true; g_bombDefused=defused;
            g_bombSeconds=remaining;
            if (duration>=5.f && duration<=300.f) g_bombC4Timer=duration;
            g_defuseSeconds=(std::isfinite(defuseEnd) && defuseEnd>now) ? defuseEnd-now : 0.f;
            g_defuseLength=std::isfinite(defuseLen) ? defuseLen : 0.f;
            hadNativeBomb=true;
            return;
        }
        if (hadNativeBomb)
        {
            g_bombActive=false;g_bombDefused=false;hadNativeBomb=false;
        }
        g_bombNative=false;g_defuseSeconds=0.f;
    }

    // ---- Bomb Timer panel ----
    static void RenderBombTimer()
    {
        if ( !g_config.settings.visuals.others.bomb_timer )
            return;
        if ( !g_bombActive.load() || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() )
            return;

        const ULONGLONG nowTick = GetTickCount64();
        const float c4timer   = g_bombC4Timer.load();
        const float elapsed   = static_cast<float>( nowTick - g_bombPlantTick.load() ) / 1000.f;
        const float timeLeft  = g_bombNative.load() ? g_bombSeconds.load() : c4timer - elapsed;

        if ( g_bombDefused.load() && nowTick - g_bombDefuseTick.load() > 3000 )
        {
            g_bombActive.store( false );
            return;
        }
        if ( timeLeft <= 0.f && !g_bombDefused.load() )
        {
            g_bombActive.store( false );
            return;
        }

        ImDrawList* dl     = ImGui::GetForegroundDrawList();
        const ImVec2 disp  = ImGui::GetIO().DisplaySize;
        const float  panelW = 200.f;
        const float  px     = disp.x * 0.5f - panelW * 0.5f;
        const float  py     = disp.y - 72.f;

        // Background
        dl->AddRectFilled( { px - 5.f, py - 5.f }, { px + panelW + 5.f, py + 52.f },
                           IM_COL32( 0, 0, 0, 210 ), 4.f );
        dl->AddRect( { px - 5.f, py - 5.f }, { px + panelW + 5.f, py + 52.f },
                     IM_COL32( 80, 80, 80, 200 ), 4.f );

        if ( g_bombDefused.load() )
        {
            const char* msg = "BOMB DEFUSED";
            const ImVec2 sz = ImGui::CalcTextSize( msg );
            dl->AddText( { px + panelW * 0.5f - sz.x * 0.5f, py + 8.f },
                         IM_COL32( 90, 200, 255, 255 ), msg );
            return;
        }

        // Title + time
        const char* title = "BOMB";
        const ImVec2 tsz = ImGui::CalcTextSize( title );
        dl->AddText( { px + panelW * 0.5f - tsz.x * 0.5f, py + 2.f },
                     IM_COL32( 255, 80, 80, 255 ), title );

        char timeBuf[16];
        const float defuse = g_defuseSeconds.load();
        sprintf_s( timeBuf, "%.1fs", timeLeft );
        const ImVec2 tbsz = ImGui::CalcTextSize( timeBuf );
        // colour: green > 15s, yellow > 5s, red < 5s
        const ImU32 timeCol = ( timeLeft > 15.f ) ? IM_COL32( 80, 220, 80, 255 )
                            : ( timeLeft > 5.f   ) ? IM_COL32( 255, 200, 60, 255 )
                                                   : IM_COL32( 255, 60, 60, 255 );
        dl->AddText( { px + panelW - tbsz.x - 2.f, py + 2.f }, timeCol, timeBuf );

        // Progress bar
        const float barY   = py + 18.f;
        const float frac   = (std::clamp)( timeLeft / c4timer, 0.f, 1.f );
        const ImU32 fillC  = ( frac > 0.5f ) ? IM_COL32( 80, 200, 80, 240 )
                           : ( frac > 0.2f ) ? IM_COL32( 255, 200, 60, 240 )
                                             : IM_COL32( 255, 60, 60, 240 );
        dl->AddRectFilled( { px, barY }, { px + panelW, barY + 8.f },
                           IM_COL32( 40, 40, 40, 240 ), 3.f );
        dl->AddRectFilled( { px, barY }, { px + panelW * frac, barY + 8.f }, fillC, 3.f );
        if (g_bombNative.load() && defuse > 0.f)
        {
            char defuseText[48];
            sprintf_s(defuseText,"DEFUSE %.1fs",defuse);
            dl->AddText({px,barY+11.f}, defuse < timeLeft ? IM_COL32(90,200,255,255) : IM_COL32(255,80,80,255), defuseText);
        }
    }

    void Render()
    {
        RenderBombTimer();
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock( g_mtx );

        static ULONGLONG s_lastStatus = 0;
        if ( now - s_lastStatus > 3000 )
        {
            s_lastStatus = now;
            const auto& o = g_config.settings.visuals.others;
            ELOG( "status: render OK | cfg logs=%d tracers=%d hitmarker=%d | init=%d raw=%d hurt=%d impact=%d shots=%d proj=%d | tracers=%d markers=%d lines=%d",
                o.event_logs, o.bullet_tracers, o.hitmarker, g_initOk ? 1 : 0, g_cntRaw,
                g_cntHurt, g_cntImpact, g_cntShots, g_cntProj,
                (int)g_tracers.size(), (int)g_markers.size(), (int)g_lines.size() );
        }

        ExpireShotRequests(now);

        while ( !g_lines.empty() && now - g_lines.front().time > kLineLife )
            g_lines.pop_front();

        DrawWorld( now );


        if ( !g_config.settings.visuals.others.event_logs || g_lines.empty() )
            return;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float y = 6.f;
        const float x = 8.f;
        const float lh = ImGui::GetTextLineHeight() + 2.f;
        for ( const Line& l : g_lines )
        {
            const ULONGLONG age = now - l.time;
            float a = 1.f;
            if ( age > kLineLife - kLineFade )
                a = static_cast<float>( kLineLife - age ) / kLineFade;
            // slide-in for fresh lines
            const float slide = age < 150 ? ( 1.f - age / 150.f ) * -20.f : 0.f;
            const int alpha = static_cast<int>( a * 255.f );

            auto shadowText = [ & ]( float px, ImU32 col, const char* t )
            {
                dl->AddText( { px + 1.f, y + 1.f }, IM_COL32( 0, 0, 0, alpha ), t );
                dl->AddText( { px, y }, ( col & 0x00FFFFFF ) | ( static_cast<ImU32>( alpha ) << 24 ), t );
                return ImGui::CalcTextSize( t ).x;
            };

            float cx = x + slide;
            cx += shadowText( cx, IM_COL32( 160, 160, 160, 255 ), "[big pickle] " );
            cx += shadowText( cx, l.color, l.prefix.c_str() );
            cx += shadowText( cx, IM_COL32( 255, 255, 255, 255 ), " " );
            shadowText( cx, IM_COL32( 255, 255, 255, 255 ), l.text.c_str() );
            y += lh;
        }
    }
}

int EventLog::GetSpreadMisses( int index )
{
    if ( index <= 0 || index >= 65 )
        return 0;
    return g_spreadMisses[ index ].load();
}

int EventLog::GetPoseMisses( int index )
{
    if ( index <= 0 || index >= 65 )
        return 0;
    return g_poseMisses[ index ].load();
}
