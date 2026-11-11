#include "features/visuals/chams/ChamsHook.hpp"
#include "features/visuals/chams/ChamsMaterials.hpp"
#include "sdk/SDK.hpp"
#include "config/Config.hpp"
#include "features/combat/players/PlayerList.hpp"
#include <cmath>
#include <cstring>

// ============================================================
//  ChamsHook.cpp — DrawModelExecute detour implementation
// ============================================================

#define CHAMS_LOG(fmt, ...) ((void)0)

using DrawModelExecuteFn = void(__fastcall*)(void*, void*, void*, void*);

// Accessed by Chams::Init / Shutdown
DrawModelExecuteFn g_origDrawModelExecute = nullptr;
bool               g_hookActive           = false;

// Nova-engine model render info layout
struct NovaModelRenderInfo_t
{
    Vec3          origin;
    Vec3          angles;
    void*         pRenderable;
    void*         pModel;
    void*         pModelToWorld;
    void*         pLightingOffset;
    void*         pLightingOrigin;
    int           flags;
    int           entity_index;
    int           skin;
    int           body;
    int           hitboxset;
    unsigned short instance;
    unsigned short pad;
};

// Engine third-person camera flag (client.dll + 0x8A7584, see
// ThirdPerson.hpp). Viewmodel chams are first-person only — in third
// person the camera pulls back and painted viewmodel parts would float
// beside the properly chams'd local model.
static bool IsThirdPerson()
{
    const uintptr_t clientBase =
        reinterpret_cast<uintptr_t>( GetModuleHandleA( "client.dll" ) );
    return clientBase &&
        *reinterpret_cast<uint8_t*>( clientBase + 0x8A7584 ) != 0;
}

// ------------------------------------------------------------------
//  Pass helpers
// ------------------------------------------------------------------
static void DrawPass( SDK::IMaterial* mat, void* thisptr, void* pState,
                      void* pInfoRaw, void* pRenderData )
{
    float white[3] = { 1.f, 1.f, 1.f };
    SDK::g_pStudioRender->SetForcedMaterial( mat, 0, 0 );
    SDK::g_pStudioRender->SetColorModulation( white );
    SDK::g_pStudioRender->SetAlphaModulation( 1.f );
    g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
    SDK::g_pStudioRender->SetForcedMaterial( nullptr, 0, 0 );
}

static int ClampStyle( int s )
{
    return ( s < 0 || s >= CHAMS_COUNT ) ? CHAMS_MATERIAL : s;
}

// Overlay style -> material style; pulse breathes via quantized alpha
// (16 cached materials) so it works without runtime modulation.
static SDK::IMaterial* OverlayMaterial( int overlayStyle, int r, int g, int b, int a )
{
    int style = CHAMS_GLOW;
    switch ( overlayStyle )
    {
    case OVERLAY_WIREFRAME: style = CHAMS_WIREFRAME; break;
    case OVERLAY_GHOST:     style = CHAMS_GHOST;     break;
    case OVERLAY_PULSE:
    {
        const float t = (float)( GetTickCount64() % 100000ULL ) / 1000.f;
        const float k = 0.25f + 0.75f * ( 0.5f + 0.5f * sinf( t * 3.f ) );
        const int   q = (int)( k * 15.f + 0.5f );        // 0..15
        a = ( a * ( q + 1 ) ) / 16;
        break;
    }
    default: break;
    }
    return ChamsMaterials::Get( style, false, r, g, b, a );
}

static void ApplyRainbow( bool on, float speed, int& r, int& g, int& b )
{
    if ( !on ) return;
    const float time = (float)GetTickCount64() / 1000.f;
    float hue = fmodf( time * speed, 1.f );
    hue = floorf( hue * 64.f ) / 64.f;                   // 64 cached hues
    float rr, gg, bb;
    ChamsMaterials::HSVtoRGB( hue, 1.f, 1.f, rr, gg, bb );
    r = (int)( rr * 255 ); g = (int)( gg * 255 ); b = (int)( bb * 255 );
}

// Draws one model with a single-pass group material plus optional
// overlay (no xray pass for local / arms / weapon).
static bool ApplyGroupChams( void* thisptr, void* pState, void* pInfoRaw, void* pRenderData,
                             int r, int g, int b, int a, int style,
                             bool overlay, int or_, int og, int ob, int oa )
{
    if ( !SDK::g_pStudioRender )
        return false;
    auto& cfg = g_config.settings.visuals.chams;
    style = ClampStyle( style );

    SDK::IMaterial* base = ChamsMaterials::Get( style, false, r, g, b, a );
    if ( !base && style != CHAMS_ORIGINAL )
        return false;
    DrawPass( base, thisptr, pState, pInfoRaw, pRenderData );

    if ( overlay )
        if ( SDK::IMaterial* ov = OverlayMaterial( cfg.overlay_style, or_, og, ob, oa ) )
            DrawPass( ov, thisptr, pState, pInfoRaw, pRenderData );
    return true;
}

void __fastcall hkDrawModelExecute( void* thisptr, void* pState,
                                    void* pInfoRaw, void* pRenderData )
{
    if ( !g_origDrawModelExecute ) return;

    const auto* pInfo = reinterpret_cast<const NovaModelRenderInfo_t*>( pInfoRaw );

    const char* modelName = nullptr;
    if ( pInfo && pInfo->pModel && SDK::g_pModelInfo )
        modelName = SDK::g_pModelInfo->GetModelName( pInfo->pModel );

    auto& cfg = g_config.settings.visuals.chams;

    // ---- viewmodel groups: arms and weapon ----
    // Viewmodel weapon models are "models/weapons/v_*"; arms either come as
    // their own "arms"/"sleeve" model or are baked into the v_ model (then
    // the arms toggle simply has nothing extra to paint).
    if ( cfg.chams && modelName )
    {
        const bool isArms = strstr( modelName, "arms" ) != nullptr ||
                            strstr( modelName, "sleeve" ) != nullptr;
        const bool isViewModelWeapon = !isArms &&
            strstr( modelName, "models/weapons/v_" ) != nullptr;

        if ( isArms || isViewModelWeapon )
        {
            if ( !IsThirdPerson() && ( isArms ? cfg.arms : cfg.weapon ) )
            {
                const int r = isArms ? cfg.arms_r     : cfg.weapon_r;
                const int g = isArms ? cfg.arms_g     : cfg.weapon_g;
                const int b = isArms ? cfg.arms_b     : cfg.weapon_b;
                const int a = isArms ? cfg.arms_a     : cfg.weapon_a;
                const int s = isArms ? cfg.arms_style : cfg.weapon_style;
                const bool ov = isArms ? cfg.arms_overlay : cfg.weapon_overlay;
                const int ovr = isArms ? cfg.arms_ov_r : cfg.weapon_ov_r;
                const int ovg = isArms ? cfg.arms_ov_g : cfg.weapon_ov_g;
                const int ovb = isArms ? cfg.arms_ov_b : cfg.weapon_ov_b;
                const int ova = isArms ? cfg.arms_ov_a : cfg.weapon_ov_a;
                if ( ApplyGroupChams( thisptr, pState, pInfoRaw, pRenderData,
                                      r, g, b, a, s, ov, ovr, ovg, ovb, ova ) )
                    return;
            }
            // A viewmodel model never enters the enemy player path below.
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }
    }

    const bool isPlayer = modelName &&
        ( strstr( modelName, "models/player" ) != nullptr ||
          strstr( modelName, "models/ragdoll" ) != nullptr );

    if ( isPlayer && SDK::g_pStudioRender )
    {
        if ( !SDK::g_pEntityList || !SDK::g_pEngine ||
             !SDK::g_pEngine->IsInGame() || !SDK::g_pEngine->IsConnected() )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        const int localIndex  = SDK::g_pEngine->GetLocalPlayer();
        const int entityIndex = pInfo->entity_index;

        if ( localIndex <= 0 || entityIndex <= 0 )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        auto* pLocal  = reinterpret_cast<SDK::C_BaseEntity*>( SDK::g_pEntityList->GetClientEntity( localIndex  ) );
        auto* pEntity = reinterpret_cast<SDK::C_BaseEntity*>( SDK::g_pEntityList->GetClientEntity( entityIndex ) );

        if ( !pLocal || !pEntity )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        if ( pEntity == pLocal )
        {
            // Local player model (visible in thirdperson). Single-pass
            // material from the local group.
            if ( cfg.local &&
                 ApplyGroupChams( thisptr, pState, pInfoRaw, pRenderData,
                     cfg.local_r, cfg.local_g, cfg.local_b, cfg.local_a, cfg.local_style,
                     cfg.local_overlay, cfg.local_ov_r, cfg.local_ov_g, cfg.local_ov_b, cfg.local_ov_a ) )
                return;

            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        const int localTeam  = pLocal ->GetTeamNum();
        const int entityTeam = pEntity->GetTeamNum();
        if ( localTeam <= 0 || entityTeam <= 0 || entityTeam == localTeam )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        if ( !cfg.chams )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        // PlayerList: hidden players keep the default material pipeline.
        if ( PlayerList::GetVisualOverride( entityIndex ).hide )
        {
            g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
            return;
        }

        const int visStyle = ClampStyle( cfg.chams_style_visible );
        const int occStyle = ClampStyle( cfg.chams_style_occluded );

        int vr = cfg.visible_r,  vg = cfg.visible_g,  vb = cfg.visible_b,  va = cfg.visible_a;
        int orr= cfg.occluded_r, og = cfg.occluded_g, ob = cfg.occluded_b, oa = cfg.occluded_a;
        if ( cfg.rainbow )
        {
            ApplyRainbow( true, cfg.rainbow_speed, vr, vg, vb );
            orr = vr; og = vg; ob = vb;
        }

        // occluded (through walls); "Original" = no xray pass
        if ( occStyle != CHAMS_ORIGINAL )
            if ( SDK::IMaterial* xray = ChamsMaterials::Get( occStyle, true, orr, og, ob, oa ) )
                DrawPass( xray, thisptr, pState, pInfoRaw, pRenderData );

        // visible base; "Original" = the game's own textures
        SDK::IMaterial* vis = ChamsMaterials::Get( visStyle, false, vr, vg, vb, va );
        DrawPass( visStyle == CHAMS_ORIGINAL ? nullptr : vis, thisptr, pState, pInfoRaw, pRenderData );

        if ( cfg.overlay )
            if ( SDK::IMaterial* ov = OverlayMaterial( cfg.overlay_style, cfg.overlay_r, cfg.overlay_g, cfg.overlay_b, cfg.overlay_a ) )
                DrawPass( ov, thisptr, pState, pInfoRaw, pRenderData );
        return;
    }

    g_origDrawModelExecute( thisptr, pState, pInfoRaw, pRenderData );
}
