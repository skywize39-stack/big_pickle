#include "features/combat/rage/AutoStop.hpp"
#include "sdk/SDK.hpp"
#include "core/prediction/EnginePrediction.hpp"
#include <cmath>
#include <algorithm>

using namespace SDK;

namespace AutoStop
{
    float ResolveMaxSpeed(const WeaponData* data,bool scoped)
    {
        if(!data)return 0.f;
        const float speed=scoped&&data->maxPlayerSpeedAlt>1.f ?
            data->maxPlayerSpeedAlt:data->maxPlayerSpeed;
        return std::isfinite(speed)&&speed>1.f?speed:0.f;
    }

    // Fatality stop_to_speed selection with CSSO native replay probes. Do not
    // transplant its CMoveData layout: CSSO has different fields and ABI.
    // Both the held-wish preview and zero-wish friction result come from the
    // engine's own movement, and the final modified command is re-predicted.
    void ApplyMovement(void* pCmd,void* pLocalPlayer,float yaw,float maxSpeed,bool forceAccuracy)
    {
        if(!EnginePrediction::Reliable()||!pCmd||!pLocalPlayer||!std::isfinite(yaw)||!std::isfinite(maxSpeed)||maxSpeed<=1.f)return;
        auto* cmd=reinterpret_cast<CUserCmd*>(pCmd);
        auto* local=reinterpret_cast<C_BaseEntity*>(pLocalPlayer);
        if(!(local->GetFlags()&FL_ONGROUND)||(cmd->buttons&IN_JUMP))return;
        const float target=forceAccuracy?0.f:(std::max)(maxSpeed/3.f-.1f,0.f);
        const CUserCmd original = *cmd;
        const Vec3 predicted=EnginePrediction::PreviewVelocity(cmd,local);
        if (!EnginePrediction::Reliable()) return;
        const Vec3 velocity=EnginePrediction::VelocityAfterFriction(local);
        if (!EnginePrediction::Reliable()) return;
        const float speed=std::sqrt(velocity.x*velocity.x+velocity.y*velocity.y);
        const float futureSpeed=std::sqrt(predicted.x*predicted.x+predicted.y*predicted.y);
        if(!std::isfinite(speed)||!std::isfinite(futureSpeed))return;
        // Do not unnecessarily freeze a movement command already inside the
        // accuracy band. This is the distinguishing behaviour of slide-stop.
        if(futureSpeed<=target+.001f)return;
        if(speed>target+.001f)
        {
            const float velocityYaw=std::atan2(velocity.y,velocity.x);
            const float angle=yaw*(3.14159265359f/180.f)-velocityYaw;
            const float amount=(std::min)(speed-target,maxSpeed);
            cmd->forwardmove.Encode(-std::cos(angle)*amount);
            cmd->sidemove.Encode(-std::sin(angle)*amount);
            cmd->upmove.Encode(0.f);
        }
        else
        {
            // Current friction-adjusted velocity is already accurate, but held
            // W/A/S/D would accelerate OUT of the band. Limit that wish movement.
            const float forward=cmd->forwardmove.Decode(),side=cmd->sidemove.Decode();
            const float wishLength=std::sqrt(forward*forward+side*side);
            if(!std::isfinite(wishLength)||wishLength<=.0001f)
            { return; }
            const float rad=yaw*(3.14159265359f/180.f),cs=std::cos(rad),sn=std::sin(rad);
            const float wishX=(cs*forward+sn*side)/wishLength;
            const float wishY=(sn*forward-cs*side)/wishLength;
            const float current=velocity.x*wishX+velocity.y*wishY;
            const float limit=(std::clamp)(current+(target-speed),0.f,maxSpeed);
            const float ratio=(std::min)(1.f,limit/wishLength);
            cmd->forwardmove.Encode(forward*ratio);
            cmd->sidemove.Encode(side*ratio);
            cmd->upmove.Encode(0.f);
        }
        // Source scales input during duck. Compensate the *command*, not a
        // fabricated velocity. CSSO's datamap exposes m_flDuckAmount here.
        const float duck = *reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(local) + nDuckAmountOffset);
        if (std::isfinite(duck) && duck >= 0.f && duck <= 1.f)
        {
            const float factor = 1.f - .66f*duck;
            cmd->forwardmove.Encode(cmd->forwardmove.Decode()/factor);
            cmd->sidemove.Encode(cmd->sidemove.Decode()/factor);
        }
        EnginePrediction::Refresh(cmd,local);
        if (!EnginePrediction::Reliable())
        {
            // A failed preview never leaves a half-applied movement command.
            *cmd = original;
        }
    }
}
