#include "features/visuals/chams/ChamsMaterials.hpp"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <unordered_map>

// ============================================================
//  ChamsMaterials.cpp — material factory, cache and HSV helper
// ============================================================

namespace
{
    std::unordered_map<uint64_t, SDK::IMaterial*> g_cache;
    // last material handed out per style/pass (used while rate-limited)
    SDK::IMaterial* g_lastPerSlot[ CHAMS_COUNT ][ 2 ] = {};
    ULONGLONG       g_lastCreate [ CHAMS_COUNT ][ 2 ] = {};
    int             g_counter = 0;

    uint64_t Key( int style, bool ignorez, int r, int g, int b, int a )
    {
        return ( (uint64_t)style << 40 ) | ( (uint64_t)( ignorez ? 1 : 0 ) << 32 ) |
               ( (uint64_t)( r & 255 ) << 24 ) | ( (uint64_t)( g & 255 ) << 16 ) |
               ( (uint64_t)( b & 255 ) << 8 ) | (uint64_t)( a & 255 );
    }

    void Str( void* kv, const char* k, const char* v ) { SDK::SetKeyValueString( kv, k, v ); }
    void Int( void* kv, const char* k, int v )         { SDK::SetKeyValueInt( kv, k, v ); }

    void Vec( void* kv, const char* k, float x, float y, float z )
    {
        char buf[ 64 ];
        sprintf_s( buf, "[%.3f %.3f %.3f]", x, y, z );
        SDK::SetKeyValueString( kv, k, buf );
    }

    void Flt( void* kv, const char* k, float v )
    {
        char buf[ 32 ];
        sprintf_s( buf, "%.3f", v );
        SDK::SetKeyValueString( kv, k, buf );
    }

    SDK::IMaterial* Build( int style, bool ignorez, int ir, int ig, int ib, int ia )
    {
        const float r = ir / 255.f, g = ig / 255.f, b = ib / 255.f, a = ia / 255.f;
        const bool unlit = style == CHAMS_FLAT || style == CHAMS_WIREFRAME || style == CHAMS_GHOST;

        void* kv = SDK::CreateKeyValues( unlit ? "UnlitGeneric" : "VertexLitGeneric" );
        if ( !kv )
            return nullptr;

        Str( kv, "$basetexture", "vgui/white" );
        Int( kv, "$model",   1 );
        Int( kv, "$nofog",   1 );
        Int( kv, "$ignorez", ignorez ? 1 : 0 );
        Int( kv, "$nocull",  style == CHAMS_WIREFRAME ? 1 : 0 );
        // $color works on every shader, $color2 is the model-only tint.
        Vec( kv, "$color",  r, g, b );
        Vec( kv, "$color2", r, g, b );
        float alpha = a;

        // Phong in VertexLitGeneric needs a normal map; a flat one keeps
        // the model's own smooth normals.
        auto phong = [ & ]( float exponent, float boost, const char* fresnel )
        {
            Str( kv, "$bumpmap", "dev/flat_normal" );
            Int( kv, "$phong", 1 );
            Flt( kv, "$phongexponent", exponent );
            Flt( kv, "$phongboost", boost );
            Str( kv, "$phongfresnelranges", fresnel );
        };
        auto envmap = [ & ]( float tr, float tg, float tb )
        {
            Str( kv, "$envmap", "env_cubemap" );
            Vec( kv, "$envmaptint", tr, tg, tb );
            Int( kv, "$envmapcontrast", 1 );
            Flt( kv, "$envmapsaturation", 1.f );
        };

        switch ( style )
        {
        case CHAMS_MATERIAL:
            Int( kv, "$halflambert", 1 );
            break;

        case CHAMS_GLOW:
            // Dark additive core, bright colored fresnel edge. If the
            // fresnel params aren't supported the whole body gets a soft
            // additive tint instead, which still reads as a glow.
            Int( kv, "$additive", 1 );
            Vec( kv, "$color2", r * 0.15f, g * 0.15f, b * 0.15f );
            envmap( r, g, b );
            Int( kv, "$envmapfresnel", 1 );
            Str( kv, "$envmapfresnelminmaxexp", "[0 1 2]" );
            phong( 4.f, 6.f, "[0 0.5 1]" );
            Vec( kv, "$phongtint", r, g, b );
            Int( kv, "$rimlight", 1 );
            Flt( kv, "$rimlightexponent", 2.f );
            Flt( kv, "$rimlightboost", 4.f );
            break;

        case CHAMS_METALLIC:
            Int( kv, "$halflambert", 1 );
            phong( 25.f, 8.f, "[0.4 0.8 1]" );
            Vec( kv, "$phongtint", 0.6f + r * 0.4f, 0.6f + g * 0.4f, 0.6f + b * 0.4f );
            envmap( r * 0.7f, g * 0.7f, b * 0.7f );
            break;

        case CHAMS_WIREFRAME:
            Int( kv, "$wireframe", 1 );
            break;

        case CHAMS_FLAT:
            break;

        case CHAMS_GLOSSY:
            Int( kv, "$halflambert", 1 );
            phong( 60.f, 14.f, "[0.2 0.6 1]" );
            Vec( kv, "$phongtint", 1.f, 1.f, 1.f );
            envmap( 0.15f, 0.15f, 0.15f );
            break;

        case CHAMS_CHROME:
            Vec( kv, "$color2", r * 0.25f, g * 0.25f, b * 0.25f );
            phong( 120.f, 20.f, "[0.5 1 2]" );
            Vec( kv, "$phongtint", 1.f, 1.f, 1.f );
            envmap( 0.4f + r * 0.8f, 0.4f + g * 0.8f, 0.4f + b * 0.8f );
            Flt( kv, "$envmapcontrast", 1.f );
            break;

        case CHAMS_GLASS:
            Int( kv, "$translucent", 1 );
            alpha = a * 0.45f;
            phong( 80.f, 20.f, "[0.6 1 3]" );
            Vec( kv, "$phongtint", 1.f, 1.f, 1.f );
            envmap( r, g, b );
            Int( kv, "$envmapfresnel", 1 );
            Str( kv, "$envmapfresnelminmaxexp", "[0.2 1 2]" );
            break;

        case CHAMS_PEARLESCENT:
            Int( kv, "$halflambert", 1 );
            phong( 8.f, 4.f, "[0.1 0.5 1]" );
            // shine shifted toward the complementary pastel
            Vec( kv, "$phongtint", 0.6f + ( 1.f - r ) * 0.4f, 0.6f + ( 1.f - g ) * 0.4f, 0.6f + ( 1.f - b ) * 0.4f );
            envmap( 0.25f + r * 0.25f, 0.25f + g * 0.25f, 0.25f + b * 0.25f );
            break;

        case CHAMS_GHOST:
            Int( kv, "$additive", 1 );
            alpha = a * 0.6f;
            break;
        }

        Flt( kv, "$alpha", alpha );

        char name[ 64 ];
        sprintf_s( name, "_csso_chams_%d_%d_%d", style, ignorez ? 1 : 0, ++g_counter );
        return SDK::g_pMaterialSystem->CreateMaterial( name, kv );
    }
}

void ChamsMaterials::HSVtoRGB( float h, float s, float v, float& r, float& g, float& b )
{
    int   i  = (int)( h * 6.0f );
    float f  = h * 6.0f - (float)i;
    float p  = v * ( 1.0f - s );
    float q  = v * ( 1.0f - f * s );
    float t  = v * ( 1.0f - ( 1.0f - f ) * s );
    float rr, gg, bb;
    switch ( i % 6 )
    {
    case 0: rr=v;  gg=t;  bb=p;  break;
    case 1: rr=q;  gg=v;  bb=p;  break;
    case 2: rr=p;  gg=v;  bb=t;  break;
    case 3: rr=p;  gg=q;  bb=v;  break;
    case 4: rr=t;  gg=p;  bb=v;  break;
    case 5: rr=v;  gg=p;  bb=q;  break;
    default: rr=gg=bb=0;          break;
    }
    r = rr; g = gg; b = bb;
}

SDK::IMaterial* ChamsMaterials::Get( int style, bool ignorez, int r, int g, int b, int a )
{
    if ( style < 0 || style >= CHAMS_COUNT || style == CHAMS_ORIGINAL )
        return nullptr;
    if ( !SDK::g_pMaterialSystem || !SDK::fnKeyValues_Allocate )
        return nullptr;

    const uint64_t key = Key( style, ignorez, r, g, b, a );
    auto it = g_cache.find( key );
    if ( it != g_cache.end() )
        return g_lastPerSlot[ style ][ ignorez ] = it->second;

    // Rate limit: at most one new material per style/pass every 60 ms.
    const ULONGLONG now = GetTickCount64();
    SDK::IMaterial*& last = g_lastPerSlot[ style ][ ignorez ];
    if ( last && now - g_lastCreate[ style ][ ignorez ] < 60 )
        return last;

    SDK::IMaterial* mat = Build( style, ignorez, r, g, b, a );
    g_lastCreate[ style ][ ignorez ] = now;
    if ( mat )
    {
        g_cache[ key ] = mat;
        last = mat;
    }
    return mat ? mat : last;
}

void ChamsMaterials::Reset()
{
    g_cache.clear();
    for ( auto& s : g_lastPerSlot ) s[ 0 ] = s[ 1 ] = nullptr;
    for ( auto& s : g_lastCreate )  s[ 0 ] = s[ 1 ] = 0;
}
