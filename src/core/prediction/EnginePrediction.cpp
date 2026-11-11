#include "core/prediction/EnginePrediction.hpp"
#include "sdk/SDK.hpp"
#include "sdk/build/CssoBuild.hpp"
#include "core/commands/CommandBuffer.hpp"
#include "sdk/GameTypes.hpp"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace SDK;

namespace EnginePrediction
{
    namespace
    {
        // These are CSSO x64 bindings, NOT the donor's CPrediction layout.
        constexpr uintptr_t kPrediction = Build::Prediction::Object, kPredictionTable = Build::Prediction::Vtable;
        constexpr uintptr_t kUpdate = Build::Prediction::Update;
        constexpr uintptr_t kLastFrame = Build::Prediction::LastFrame, kPredicted = Build::Prediction::PredictedCount;
        constexpr uintptr_t kEnginePrediction = Build::Prediction::EngineInterface, kSignon = Build::Prediction::Signon;
        constexpr uintptr_t kDelta = Build::Prediction::DeltaTick, kOutgoing = Build::Prediction::LastOutgoing;
        constexpr uintptr_t kChoked = Build::Prediction::Choked, kAck = Build::Prediction::Acknowledged;
        using UpdateFn = void(__fastcall*)(void*, int, bool, int, int);

        struct Context
        {
            bool active = false, reliable = false, touched = false;
            void* input = nullptr;
            C_BaseEntity* local = nullptr;
            uintptr_t prediction = 0, globals = 0;
            int sequence = 0, ack = 0, delta = 0, outgoing = 0, choked = 0;
            int fireTick = 0;
            Vec3 eye{}, velocity{};
        } s;
        struct Observation
        {
            void* local = nullptr;
            int sequence = 0, tick = 0;
        } s_natural;
        bool s_replaying = false;
        int s_seenReplay = 0, s_replayTick = 0;
        const char* s_failure = "not started";

        template<class T> T& At(uintptr_t base, uintptr_t offset)
        { return *reinterpret_cast<T*>(base + offset); }
        bool Finite(const Vec3& v)
        { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        unsigned char& Predicted(CUserCmd* cmd)
        { return reinterpret_cast<unsigned char*>(cmd)[0x72]; }

        bool MatchesImage(uintptr_t base, uint32_t timestamp, uint32_t size)
        {
            if (!base || At<uint16_t>(base, 0) != 0x5A4D) return false;
            const auto pe = At<uint32_t>(base, 0x3C);
            return pe >= 0x40 && pe <= 0x1000 && At<uint32_t>(base, pe) == 0x4550 &&
                At<uint32_t>(base, pe+8) == timestamp && At<uint32_t>(base, pe+0x50) == size;
        }
        // No native functions are executed inside this SEH probe. A different
        // build, loading screen, missing input or stale history leaves us off.
        bool Bind(void* input, C_BaseEntity* local, int seq)
        {
            __try
            {
                if (!MatchesImage(g_uClientBase, Build::Client.timestamp, Build::Client.imageSize) ||
                    !MatchesImage(g_uEngineBase, Build::Engine.timestamp, Build::Engine.imageSize))
                { s_failure = "DLL build mismatch"; return false; }
                const auto pred = At<uintptr_t>(g_uEngineBase, kEnginePrediction);
                if (pred != g_uClientBase+kPrediction ||
                    At<uintptr_t>(pred, 0) != g_uClientBase+kPredictionTable ||
                    At<uintptr_t>(At<uintptr_t>(pred, 0), 3*sizeof(uintptr_t)) != g_uClientBase+kUpdate ||
                    At<uintptr_t>(g_uClientBase, Build::Prediction::Input) != reinterpret_cast<uintptr_t>(input) ||
                    At<int>(g_uEngineBase, kSignon) != 6 || At<unsigned char>(pred, 0xC))
                { s_failure = "prediction/input not ready"; return false; }
                const int delta = At<int>(g_uEngineBase, kDelta);
                const int ack = At<int>(g_uEngineBase, kAck);
                const int outgoing = At<int>(g_uEngineBase, kOutgoing);
                const int choked = At<int>(g_uEngineBase, kChoked);
                if (delta <= 0 || ack < 0 || seq <= ack ||
                    static_cast<int64_t>(seq)-ack >= MULTIPLAYER_BACKUP || choked < 0 ||
                    choked >= MULTIPLAYER_BACKUP || static_cast<int64_t>(outgoing)+choked+1 != seq)
                { s_failure = "network command window invalid"; return false; }
                if (s_natural.local != local || s_natural.sequence <= 0 ||
                    s_natural.sequence >= seq || seq-s_natural.sequence >= MULTIPLAYER_BACKUP ||
                    std::abs(local->TickBase()-s_natural.tick) >= MULTIPLAYER_BACKUP)
                { s_failure = "waiting for native RunCommand history"; return false; }
                auto* commands = At<CUserCmd*>(reinterpret_cast<uintptr_t>(input), CINPUT_OFFSET_COMMANDS);
                auto* verified = At<CVerifiedUserCmd*>(reinterpret_cast<uintptr_t>(input), CINPUT_OFFSET_VERIFIED);
                if (!commands || !verified)
                { s_failure = "command/verified ring unavailable"; return false; }
                for (int n = ack+1; n <= seq; ++n)
                    if (commands[n%MULTIPLAYER_BACKUP].command_number != n)
                    { s_failure = "command ring gap"; return false; }
                const auto globals = At<uintptr_t>(g_uClientBase, Build::Prediction::Globals);
                if (!globals || !std::isfinite(At<float>(globals, 0x1C)) ||
                    std::fabs(At<float>(globals, 0x1C)-kTickInterval) > .00001f ||
                    At<unsigned char>(pred, 0xF) || At<int>(pred, kPredicted) < 0 ||
                    At<int>(pred, kPredicted) >= MULTIPLAYER_BACKUP)
                { s_failure = "clock/prediction state invalid"; return false; }
                s.input = input; s.local = local; s.sequence = seq;
                s.prediction = pred; s.globals = globals;
                s.ack = ack; s.delta = delta; s.outgoing = outgoing; s.choked = choked;
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            { s_failure = "prediction binding unavailable"; return false; }
        }
        CUserCmd* Command()
        {
            return Commands::CommandBuffer(s.input).Find(s.sequence);
        }
        void Sync()
        {
            Commands::CommandBuffer(s.input).SynchronizeNative(s.sequence);
        }
        bool SameNetworkWindow()
        {
            return At<int>(g_uEngineBase,kSignon) == 6 &&
                At<int>(g_uEngineBase,kAck) == s.ack && At<int>(g_uEngineBase,kDelta) == s.delta &&
                At<int>(g_uEngineBase,kOutgoing) == s.outgoing && At<int>(g_uEngineBase,kChoked) == s.choked;
        }
        bool Replay(CUserCmd* cmd)
        {
            s.reliable = false;
            if (!s.active || s_replaying || cmd != Command() || cmd->command_number != s.sequence ||
                !SameNetworkWindow())
            { s_failure = "replay context changed"; return false; }

            // Only the current preview is attack-free. Older commands retain
            // their genuine buttons and engine-owned first-time effect flags.
            // The final command and its verified copy are restored immediately.
            const CUserCmd original = *cmd;
            cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
            cmd->impulse = 0;
            cmd->weaponselect = 0;
            cmd->weaponsubtype = 0;
            Predicted(cmd) = 1;
            Sync();
            auto& count = At<int>(s.prediction, kPredicted);
            count = (std::min)(count, s.sequence-s.ack-1);
            // Force the native world restore + ComputeFirstCommand path.
            // CSSO uses +10 for last frame and +14 for count (not donor +1C).
            At<int>(s.prediction, kLastFrame) = -1;
            s.touched = true;
            s_seenReplay = 0;
            s_replaying = true;
            reinterpret_cast<UpdateFn>(g_uClientBase+kUpdate)(reinterpret_cast<void*>(s.prediction),
                s.delta, true, s.ack, s.sequence);
            s_replaying = false;
            *cmd = original;
            Sync();
            if (s_seenReplay != s.sequence || s_replayTick < 0 ||
                s.local->TickBase() != s_replayTick+1 || count != s.sequence-s.ack)
            { s_failure = "native command not simulated"; return false; }
            s.fireTick = s_replayTick;
            s.eye = s.local->EyePosition();
            s.velocity = s.local->GetVelocity();
            s.reliable = Finite(s.eye) && Finite(s.velocity);
            s_failure = s.reliable ? "ok" : "native result non-finite";
            return s.reliable;
        }
    }

    bool Replaying() { return s_replaying; }
    // Called BEFORE the original RunCommand, so time is the fire tick, not
    // the incremented tickbase. Speculative runs never update natural history.
    void ObserveRunCommand(void* player, void* usercmd)
    {
        if (!player || !usercmd || player != g_pLocalPlayer) return;
        const auto* cmd = reinterpret_cast<const CUserCmd*>(usercmd);
        const int tick = reinterpret_cast<C_BaseEntity*>(player)->TickBase();
        if (s_replaying)
        {
            if (player == s.local && cmd->command_number == s.sequence)
            { s_seenReplay = cmd->command_number; s_replayTick = tick; }
        }
        else
        {
            s_natural.local = player;
            s_natural.sequence = cmd->command_number;
            s_natural.tick = tick;
        }
    }

    void ResetSession()
    {
        // Map teardown must invalidate observations WITHOUT Finish: its saved
        // input/entity pointers can already belong to the destroyed level.
        s = {};
        s_natural = {};
        s_replaying = false;
        s_seenReplay = s_replayTick = 0;
        s_failure = "waiting for current-map native history";
    }

    void Start(void* input, int seq, void* player)
    {
        Finish();
        s_failure = "local player unavailable";
        auto* local = reinterpret_cast<C_BaseEntity*>(player);
        if (!local || !input || seq <= 0 || !g_pEngine || !g_pEngine->IsInGame())
        { s_natural = {}; return; }
        if (!Bind(input, local, seq)) return;
        s.active = true;
        Refresh(Command(), local);
    }
    void Refresh(void* cmd, void* player)
    {
        if (!s.active || player != s.local) return;
        Replay(reinterpret_cast<CUserCmd*>(cmd));
    }
    void Finish()
    {
        if (s.active && s.touched && SameNetworkWindow())
        {
            // The attack-free preview is NOT a final simulation. Make the
            // engine execute the final movement/angles/buttons normally once.
            auto* cmd = Command();
            if (cmd && cmd->command_number == s.sequence)
            {
                Predicted(cmd) = 0;
                auto& count = At<int>(s.prediction, kPredicted);
                count = (std::clamp)(count, 0, s.sequence-s.ack-1);
                Sync();
            }
        }
        s = {};
    }
    bool Active() { return s.active; }
    bool Reliable() { return s.active && s.reliable; }
    const char* FailureReason() { return s_failure; }
    float CurTime()
    {
        return Reliable() ? static_cast<float>(s.fireTick)*kTickInterval :
            (g_pEngine ? g_pEngine->GetTime() : 0.f);
    }
    Vec3 EyePosition(void* player)
    {
        if (Reliable() && (!player || player == s.local)) return s.eye;
        return player ? reinterpret_cast<C_BaseEntity*>(player)->EyePosition() : Vec3{};
    }
    Vec3 PredictedVelocity() { return Reliable() ? s.velocity : Vec3{}; }

    // Transactional probes: both the command bytes AND the native final
    // prediction are restored. Never accumulate multiple movement ticks.
    Vec3 PreviewVelocity(void* command, void* player)
    {
        auto* cmd = reinterpret_cast<CUserCmd*>(command);
        if (!Reliable() || player != s.local || !cmd) return {};
        auto* current = Command();
        if (cmd == current) return Replay(current) ? s.velocity : Vec3{};
        const CUserCmd saved = *current;
        *current = *cmd;
        const bool ok = Replay(current);
        const Vec3 result = s.velocity;
        *current = saved;
        const char* failure = s_failure;
        const bool restored = Replay(current);
        if (!ok || !restored)
        {
            s.reliable = false;
            if (!ok) s_failure = failure;
            return {};
        }
        return result;
    }
    Vec3 VelocityAfterFriction(void* player)
    {
        if (!Reliable() || player != s.local) return {};
        auto probe = *Command();
        probe.forwardmove.Encode(0.f); probe.sidemove.Encode(0.f); probe.upmove.Encode(0.f);
        // This is an actual zero-wish native movement tick, including surface
        // friction/collision/duck, not a transplanted donor CMoveData struct.
        return PreviewVelocity(&probe, player);
    }

    ScopedAccuracyState::ScopedAccuracyState(const Vec3* velocityOverride)
    {
        if (!Reliable()) return;
        m_globals = s.globals;
        m_curtime = At<float>(m_globals,0xC); m_frametime = At<float>(m_globals,0x10);
        At<float>(m_globals,0xC) = static_cast<float>(s.fireTick)*kTickInterval;
        At<float>(m_globals,0x10) = kTickInterval;
        // Live HC now uses native entity velocity without borrowing model
        // output. A rest-cone query is explicitly hypothetical, not shot HC.
        if (!velocityOverride) return;
        m_local = reinterpret_cast<uintptr_t>(s.local);
        m_velocity = At<Vec3>(m_local,nAbsVelocityOffset);
        m_dirtyVelocity = At<int>(m_local,nEFlagsOffset) & EFL_DIRTY_ABSVELOCITY;
        At<Vec3>(m_local,nAbsVelocityOffset) = *velocityOverride;
        At<int>(m_local,nEFlagsOffset) &= ~EFL_DIRTY_ABSVELOCITY;
    }
    ScopedAccuracyState::~ScopedAccuracyState()
    {
        if (m_globals)
        { At<float>(m_globals,0xC) = m_curtime; At<float>(m_globals,0x10) = m_frametime; }
        if (m_local)
        {
            At<Vec3>(m_local,nAbsVelocityOffset) = m_velocity;
            auto& flags = At<int>(m_local,nEFlagsOffset);
            flags = (flags & ~EFL_DIRTY_ABSVELOCITY) | m_dirtyVelocity;
        }
    }
}

int EnginePrediction::ComputeCurrentLimit( int command_number )
    {
        // Fatality-style: iterate command history from last_command_ack to
        // current command, accumulating choked commands to compute the
        // real-time packet limit (sv_maxusrcmdprocessticks budget).
        if ( command_number <= 0 )
            return 16;

        const uintptr_t engineBase = reinterpret_cast<uintptr_t>( GetModuleHandleA( "engine.dll" ) );
        if ( !engineBase )
            return 16;

        int lastAck = 0;
        __try
        {
            lastAck = *reinterpret_cast<const int*>( engineBase + Build::Prediction::Acknowledged );
        }
        __except ( EXCEPTION_EXECUTE_HANDLER )
        {
            return 16;
        }

        const int maxProcessTicks = 16; // sv_maxusrcmdprocessticks

        // Start from the last acknowledged command's limit (or default 16).
        int limit = 16;
        if ( lastAck > 0 )
        {
            auto& ackInfo = GetPredInfo( lastAck );
            if ( ackInfo.sequence == lastAck )
                limit = ackInfo.limit;
        }

        // Walk forward through the command ring, accumulating choked commands.
        for ( int i = lastAck + 1; i <= command_number; ++i )
        {
            auto& info = GetPredInfo( i );

            // Validate that this command exists in the ring.
            if ( info.sequence != i )
                continue;

            // Add choked commands from this step.
            limit += info.choked_commands;

            // Apply skip_fake_commands adjustment (Fatality-style).
            if ( info.skip_fake_commands )
                limit = -info.limit; // negative adjustment when skipping fake commands
            else
                limit = info.limit;

            // Clamp to valid range.
            if ( limit < 0 ) limit = 0;
            if ( limit > maxProcessTicks ) limit = maxProcessTicks;

            info.limit = limit;
        }

        return limit;
    }

    EnginePrediction::PredInfo& EnginePrediction::GetPredInfo( int command_number )
    {
        auto it = s_predInfo.find( command_number );
        if ( it != s_predInfo.end() )
            return it->second;

        PredInfo info;
        info.sequence = command_number;
        info.limit = 16;
        s_predInfo[ command_number ] = info;
        return s_predInfo[ command_number ];
    }

    void EnginePrediction::ClearPredInfo()
    {
        s_predInfo.clear();
    }
