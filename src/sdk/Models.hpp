#pragma once

#include "utils/Math.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include "sdk/Materials.hpp"

namespace SDK
{
    struct ModelRenderInfo_t
    {
        Vec3          origin;
        Vec3          angles;
        void*         pRenderable;
        void*         pModel;
        void*         pModelToWorld;
        void*         pLightingOffset;
        void*         pLightingOrigin;
        int           skin;
        int           body;
        int           hitboxset;
        unsigned short instance;
        unsigned short flags;
        unsigned short pad2;
        unsigned char  lod;
        unsigned char  pad3[3];
    };

    class CModelInfo {};

    class IVModelRender
    {
    public:
        void DrawModelExecute( void* pState, ModelRenderInfo_t* pInfo, void* pRenderData )
        {
            using fn = void( __fastcall* )( void*, void*, void*, void* );
            ( *reinterpret_cast<fn**>( this ) )[20]( this, pState, pInfo, pRenderData );
        }
    };

    using model_t = void;

    // ------------------------------------------------------------------
    //  Studio model structures (reversed from client.dll sub_18010DB40)
    //
    //  studiohdr_t:
    //    +0x9C = numbones (int)
    //    +0xA0 = pBone offset from studiohdr start (int)
    //    +0xB0 = pHitboxset offset from studiohdr start (int)
    //    +0xB4 = numhitboxsets (int)
    //
    //  mstudiohitboxset_t (12 bytes):
    //    +0x00 = sznameindex (int)
    //    +0x04 = numhitboxes (int)
    //    +0x08 = hitboxindex — offset from hitboxset to hitbox array (int)
    //
    //  mstudiobbox_t (0x44 = 68 bytes, confirmed via imul rax,44h):
    //    +0x00 = bone index (int)
    //    +0x04 = group / hitgroup (int: 1=head, 2=chest, 3=stomach...)
    //    +0x08 = bbmin (Vec3)
    //    +0x14 = bbmax (Vec3)
    //    +0x20 = szhitboxnameindex (int)
    //    +0x24 = hitboxcollisiongroup (int)
    //    +0x28 = m_vecBBMin (Vec3)
    //    +0x34 = m_vecBBMax (Vec3)
    //    +0x40 = m_flRadius (float)
    // ------------------------------------------------------------------
    struct mstudiobbox_t
    {
        int   bone;              // +0x00
        int   group;             // +0x04  hitgroup (1=head, 2=chest, ...)
        Vec3  bbmin;             // +0x08
        Vec3  bbmax;             // +0x14
        int   szhitboxnameindex; // +0x20
        int   hitboxcollision;   // +0x24
        Vec3  m_vecBBMin;        // +0x28
        Vec3  m_vecBBMax;        // +0x34
        float m_flRadius;        // +0x40
    };
    static_assert( sizeof( mstudiobbox_t ) == 0x44, "mstudiobbox_t size must be 0x44 (68)" );

    struct mstudiohitboxset_t
    {
        int   sznameindex;   // +0x00
        int   numhitboxes;   // +0x04
        int   hitboxindex;   // +0x08  offset from this struct to hitbox array

        mstudiobbox_t* GetHitbox( int i ) const
        {
            return reinterpret_cast<mstudiobbox_t*>(
                reinterpret_cast<uintptr_t>( this ) + hitboxindex ) + i;
        }
    };

    struct studiohdr_t
    {
        int id;             // +0x00
        int version;        // +0x04
        long checksum;      // +0x08
        char name[64];      // +0x10
        int datalen;        // +0x50

        int NumBones() const
        {
            return *reinterpret_cast<const int*>(
                reinterpret_cast<uintptr_t>( this ) + 0x9C );
        }

        int NumHitboxSets() const
        {
            return *reinterpret_cast<const int*>(
                reinterpret_cast<uintptr_t>( this ) + 0xB4 );
        }

        mstudiohitboxset_t* GetHitboxSet( int i ) const
        {
            int offset = *reinterpret_cast<const int*>(
                reinterpret_cast<uintptr_t>( this ) + 0xB0 );
            return reinterpret_cast<mstudiohitboxset_t*>(
                reinterpret_cast<uintptr_t>( this ) + offset ) + i;
        }
    };

    class IVModelInfoClient
    {
    public:
        int         GetModelIndex( const char* name )
        {
            using fn = int( __fastcall* )( void*, const char* );
            return ( *reinterpret_cast<fn**>( this ) )[2]( this, name );
        }
        const char* GetModelName( model_t* model )
        {
            using fn = const char* ( __fastcall* )( void*, model_t* );
            return ( *reinterpret_cast<fn**>( this ) )[3]( this, model );
        }
        studiohdr_t* GetStudiomodel( const model_t* model )
        {
            using fn = studiohdr_t* ( __fastcall* )( void*, const model_t* );
            return ( *reinterpret_cast<fn**>( this ) )[32]( this, model );
        }
    };

    class IStudioRender
    {
    public:
        void SetColorModulation( float* color )
        {
            using fn = void( __fastcall* )( void*, float* );
            ( *reinterpret_cast<fn**>( this ) )[27]( this, color );
        }
        void SetAlphaModulation( float alpha )
        {
            using fn = void( __fastcall* )( void*, float );
            ( *reinterpret_cast<fn**>( this ) )[28]( this, alpha );
        }
        void SetForcedMaterial( IMaterial* mat, int materialType = 0, int extraFlags = 0 )
        {
            using fn = void( __fastcall* )( void*, IMaterial*, int, int );
            ( *reinterpret_cast<fn**>( this ) )[33]( this, mat, materialType, extraFlags );
        }
    };

    enum { MATERIAL_VAR_IGNOREZ = 0x20 };

} // namespace SDK
