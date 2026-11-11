#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/EngineState.hpp"

extern IDirect3DDevice9* g_pD3Device;

namespace SDK
{
    // ------------------------------------------------------------------
    //  World-to-screen helper — safe from BOTH the game thread and the
    //  render thread (D3D EndScene). The per-frame ESP re-projection calls
    //  it from the render thread, so it must never depend on CreateMove.
    // ------------------------------------------------------------------
    struct WorldProjection
    {
        VMatrix matrix{};
        float width=0.f,height=0.f;
        bool active=false,valid=false,hudCamera=false;
    };
    inline thread_local WorldProjection g_worldProjection;
    struct HudProjection
    {
        VMatrix matrix{};
        int frame=-1;
        ULONGLONG time=0;
        bool valid=false;
    };
    inline HudProjection g_hudProjection;
    inline std::mutex g_hudProjectionMutex;

    inline bool ValidProjectionMatrix(const VMatrix& matrix)
    {
        float norm=0.f;
        for(int r=0;r<4;++r)for(int c=0;c<4;++c)
            if(!std::isfinite(matrix[r][c]) || fabsf(matrix[r][c])>1e9f)return false;
        for(int c=0;c<4;++c)norm+=fabsf(matrix[3][c]);
        return norm>.000001f;
    }

    inline bool CopyNativeViewMatrix(VMatrix& output)
    {
        // Same verified getter as engine.dll InternalW2S +0xF8680:
        // mov rcx,[rip+disp] at +0xA; call [vtable+0x68] at +0x1A.
        // Do not use the unverified IVEngineClient slot 37 fallback here.
        __try
        {
            const auto* code=reinterpret_cast<const unsigned char*>(g_InternalW2S);
            if(!code || code[0xA]!=0x48 || code[0xB]!=0x8B || code[0xC]!=0x0D ||
                code[0x17]!=0x48 || code[0x18]!=0x8B || code[0x19]!=0x01 ||
                code[0x1A]!=0xFF || code[0x1B]!=0x50 || code[0x1C]!=0x68)return false;
            const int32_t displacement=*reinterpret_cast<const int32_t*>(code+0xD);
            const auto slot=reinterpret_cast<uintptr_t>(code+0x11)+displacement;
            void* view=*reinterpret_cast<void**>(slot);
            if(!view)return false;
            void** vtable=*reinterpret_cast<void***>(view);
            using Getter=const VMatrix* (__fastcall*)(void*);
            const VMatrix* matrix=reinterpret_cast<Getter>(vtable[13])(view);
            if(!matrix)return false;
            std::memcpy(&output,matrix,sizeof(output));
            return ValidProjectionMatrix(output);
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }

    inline int EngineRenderFrame()
    {
        __try
        {
            if(!g_uClientBase)return -1;
            const uintptr_t globals=*reinterpret_cast<const uintptr_t*>(g_uClientBase+0x81CC50);
            return globals ? *reinterpret_cast<const int*>(globals+0x04) : -1;
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
    }

    inline void PublishHudProjection()
    {
        HudProjection next{};
        next.valid=CopyNativeViewMatrix(next.matrix);
        next.frame=EngineRenderFrame();next.time=GetTickCount64();
        std::lock_guard<std::mutex> lock(g_hudProjectionMutex);
        g_hudProjection=next;
    }

    inline void BeginWorldProjection(float width,float height)
    {
        WorldProjection next{};next.active=true;next.width=width;next.height=height;
        if(std::isfinite(width) && std::isfinite(height) && width>0.f && height>0.f)
        {
            const int frame=EngineRenderFrame();const ULONGLONG now=GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(g_hudProjectionMutex);
                if(g_hudProjection.valid && frame>=0 && g_hudProjection.frame==frame &&
                    now>=g_hudProjection.time && now-g_hudProjection.time<=100)
                {next.matrix=g_hudProjection.matrix;next.valid=true;next.hudCamera=true;}
            }
            if(!next.valid)next.valid=CopyNativeViewMatrix(next.matrix);
        }
        g_worldProjection=next;
    }
    inline void EndWorldProjection(){g_worldProjection={};}
    struct ScopedWorldProjection
    {
        WorldProjection previous;
        ScopedWorldProjection(float width,float height):previous(g_worldProjection)
        {BeginWorldProjection(width,height);}
        ~ScopedWorldProjection(){g_worldProjection=previous;}
    };

    struct ProjectionClip {float x,y,w;};
    inline ProjectionClip ToProjectionClip(const Vec3& point)
    {
        const auto& m=g_worldProjection.matrix;
        return {m[0][0]*point.x+m[0][1]*point.y+m[0][2]*point.z+m[0][3],
            m[1][0]*point.x+m[1][1]*point.y+m[1][2]*point.z+m[1][3],
            m[3][0]*point.x+m[3][1]*point.y+m[3][2]*point.z+m[3][3]};
    }
    inline bool ProjectFrozen(const Vec3& point,Vec2& screen)
    {
        if(!g_worldProjection.valid || !std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z))return false;
        const auto clip=ToProjectionClip(point);
        if(!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w<.001f)return false;
        screen.x=(1.f+clip.x/clip.w)*g_worldProjection.width*.5f;
        screen.y=(1.f-clip.y/clip.w)*g_worldProjection.height*.5f;
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    }

    inline bool ProjectWorldLine(const Vec3& from,const Vec3& to,Vec2& first,Vec2& second)
    {
        if(!g_worldProjection.active || !g_worldProjection.valid)return false;
        if(!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) ||
            !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z))return false;
        const ProjectionClip a=ToProjectionClip(from),b=ToProjectionClip(to);
        const float fa[]={a.w-.001f,a.w+a.x,a.w-a.x,a.w+a.y,a.w-a.y};
        const float fb[]={b.w-.001f,b.w+b.x,b.w-b.x,b.w+b.y,b.w-b.y};
        float enter=0.f,leave=1.f;
        for(int p=0;p<5;++p)
        {
            if(!std::isfinite(fa[p]) || !std::isfinite(fb[p]))return false;
            if(fa[p]<0.f && fb[p]<0.f)return false;
            if(fa[p]<0.f || fb[p]<0.f)
            {
                const float t=fa[p]/(fa[p]-fb[p]);
                if(fa[p]<0.f)enter=(std::max)(enter,t);else leave=(std::min)(leave,t);
            }
        }
        if(enter>leave)return false;
        const auto project=[&](float t,Vec2& screen)
        {
            const float w=a.w+(b.w-a.w)*t;
            if(w<.00099f)return false;
            screen.x=(1.f+(a.x+(b.x-a.x)*t)/w)*g_worldProjection.width*.5f;
            screen.y=(1.f-(a.y+(b.y-a.y)*t)/w)*g_worldProjection.height*.5f;
            return std::isfinite(screen.x) && std::isfinite(screen.y);
        };
        return project(enter,first) && project(leave,second);
    }

    inline bool WorldToScreen( const Vec3& origin, Vec2& screen )
    {
        // One immutable camera and the actual overlay target size for the batch.
        if(g_worldProjection.active)return ProjectFrozen(origin,screen);
        if ( !g_pEngine || !g_pD3Device || !std::isfinite( origin.x ) ||
             !std::isfinite( origin.y ) || !std::isfinite( origin.z ) )
            return false;

        D3DVIEWPORT9 vp{};
        if ( FAILED( g_pD3Device->GetViewport( &vp ) ) || !vp.Width || !vp.Height )
            return false;

        // Primary path: the pattern-scanned engine W2S — the exact function
        // that produced the old per-tick ESP NDC and the AutoPeek marker.
        // It is pure math over the engine's view-matrix global (the game's
        // own HUD reads it during rendering), so calling it from EndScene is
        // equivalent to the game's own per-frame usage. Returns non-zero on
        // failure (behind camera / off screen).
        if ( g_InternalW2S )
        {
            Vec3 ndc;
            if ( g_InternalW2S( nullptr, origin, ndc ) )
                return false;

            if ( !std::isfinite( ndc.x ) || !std::isfinite( ndc.y ) )
                return false;

            screen.x = ( 1.0f + ndc.x ) * static_cast<float>( vp.Width ) * 0.5f;
            screen.y = ( 1.0f - ndc.y ) * static_cast<float>( vp.Height ) * 0.5f;
            return std::isfinite( screen.x ) && std::isfinite( screen.y );
        }

        // Fallback: IVEngineClient matrix. NOTE — the WorldToScreenMatrix
        // vtable slot was never confirmed in this CSSO build (it returned
        // garbage and blanked every box when used as the primary path), so
        // this branch exists only for the missing-pattern case.
        const auto& matrix = g_pEngine->WorldToScreenMatrix();
        const float clipX = matrix[0][0] * origin.x + matrix[0][1] * origin.y +
            matrix[0][2] * origin.z + matrix[0][3];
        const float clipY = matrix[1][0] * origin.x + matrix[1][1] * origin.y +
            matrix[1][2] * origin.z + matrix[1][3];
        const float clipW = matrix[3][0] * origin.x + matrix[3][1] * origin.y +
            matrix[3][2] * origin.z + matrix[3][3];

        if ( !std::isfinite( clipX ) || !std::isfinite( clipY ) ||
             !std::isfinite( clipW ) || clipW < 0.001f )
            return false;

        const float invW = 1.0f / clipW;
        screen.x = ( 1.0f + clipX * invW ) * static_cast<float>( vp.Width ) * 0.5f;
        screen.y = ( 1.0f - clipY * invW ) * static_cast<float>( vp.Height ) * 0.5f;
        return std::isfinite( screen.x ) && std::isfinite( screen.y );
    }

} // namespace SDK
