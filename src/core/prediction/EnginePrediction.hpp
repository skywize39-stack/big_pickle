#pragma once

// Guarded CSSO x64 native CPrediction::Update replay. Uses the engine's
// command ring, acknowledged history and entity restore/save paths. No local
// movement approximation and no standalone extra RunCommand call. Preview
// attacks are suppressed; Finish invalidates the current preview so the engine
// executes the final command normally. Requires the verified DLL builds and
// an observed natural RunCommand. Unknown history fails closed for rage.

#include "utils/Math.hpp"
#include <cstdint>
#include <unordered_map>

namespace EnginePrediction
{
    // Per-command tickbase state, mirrors Fatality's prediction::get_pred_info().
    // Tracks command-level limit / choked / skip_fake_commands for
    // compute_current_limit() and attempt_shift_back().
    struct PredInfo
    {
        int sequence = 0;
        int choked_commands = 0;
        int sent_commands = 0;
        int limit = 16;          // sv_maxusrcmdprocessticks default
        bool skip_fake_commands = false;
    };

    class ScopedAccuracyState
    {
    public:
        explicit ScopedAccuracyState(const Vec3* velocityOverride = nullptr);
        ~ScopedAccuracyState();
        ScopedAccuracyState(const ScopedAccuracyState&) = delete;
        ScopedAccuracyState& operator=(const ScopedAccuracyState&) = delete;
    private:
        uintptr_t m_local = 0, m_globals = 0;
        Vec3 m_velocity{};
        int m_dirtyVelocity = 0;
        float m_curtime = 0.f, m_frametime = 0.f;
    };

    bool Replaying();
    void ObserveRunCommand(void* player, void* cmd);
    const char* FailureReason();

    void ResetSession();

    void Start( void* pCInput, int sequence_number, void* pLocalPlayer );
    void Finish();
    void Refresh(void* pCmd, void* pLocalPlayer);
    bool Reliable();
    Vec3 PreviewVelocity(void* pCmd,void* pLocalPlayer);
    Vec3 VelocityAfterFriction(void* pLocalPlayer);

    bool  Active();

    float CurTime();
    Vec3  EyePosition( void* pLocalPlayer = nullptr );
    Vec3  PredictedVelocity();

    // Fatality-style packet limit computation.
    int ComputeCurrentLimit( int command_number = 0 );

    // Per-command info accessor (Fatality-style prediction::get_pred_info).
    PredInfo& GetPredInfo( int command_number );
    void    ClearPredInfo();

    // Fatality-style shift-back state (namespace-scope inline variables).
    inline static std::unordered_map<int, PredInfo> s_predInfo;
    inline static int s_toRecharge = 0;
    inline static int s_toShift = 0;
    inline static int s_toAdjust = 0;
    inline static bool s_skipNextAdjust = false;

    // Accessors.
    inline int   GetToRecharge()  { return s_toRecharge;  }
    inline int   GetToShift()     { return s_toShift;     }
    inline int   GetToAdjust()    { return s_toAdjust;    }
    inline void  SetToRecharge(int v) { s_toRecharge = v; }
    inline void  SetToShift(int v)    { s_toShift = v;    }
    inline void  SetToAdjust(int v)   { s_toAdjust = v;   }
    inline bool  GetSkipNextAdjust()  { return s_skipNextAdjust; }
    inline void  SetSkipNextAdjust(bool v) { s_skipNextAdjust = v; }
}
