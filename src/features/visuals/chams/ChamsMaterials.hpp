#pragma once

// ============================================================
//  ChamsMaterials.hpp — chams material factory and cache
// ============================================================

#include "sdk/SDK.hpp"

// Chams style enumeration. Indices 0..6 keep their old meaning so saved
// configs stay valid (0 was always the lit "Flat", 4 the unlit one).
enum ChamsStyle : int
{
    CHAMS_MATERIAL    = 0,  // soft lit (half-lambert) solid color
    CHAMS_GLOW        = 1,  // additive fresnel glow, dark core
    CHAMS_METALLIC    = 2,  // tinted reflections + phong highlight
    CHAMS_WIREFRAME   = 3,
    CHAMS_FLAT        = 4,  // unlit solid color
    CHAMS_GLOSSY      = 5,  // plastic: strong white specular
    CHAMS_CHROME      = 6,  // mirror chrome tinted by the color
    CHAMS_GLASS       = 7,  // translucent body + reflective rim
    CHAMS_PEARLESCENT = 8,  // soft pastel shine
    CHAMS_GHOST       = 9,  // additive see-through
    CHAMS_ORIGINAL    = 10, // game material (use with an overlay)
    CHAMS_COUNT
};

// Overlay layer styles (drawn over the base pass)
enum ChamsOverlay : int
{
    OVERLAY_GLOW      = 0,
    OVERLAY_WIREFRAME = 1,
    OVERLAY_GHOST     = 2,
    OVERLAY_PULSE     = 3, // glow that breathes
    OVERLAY_COUNT
};

namespace ChamsMaterials
{
    // Returns a cached material for style + pass + color. Colors are baked
    // into the material (envmap / phong tints can't be modulated), so new
    // colors create a material; creation is rate-limited per style/pass
    // so dragging a color picker doesn't create hundreds of materials.
    // Returns nullptr for CHAMS_ORIGINAL or on failure.
    SDK::IMaterial* Get( int style, bool ignorez, int r, int g, int b, int a );

    // Forget every cached pointer (on unload).
    void Reset();

    // Converts HSV to RGB (0-1 range each).
    void HSVtoRGB( float h, float s, float v, float& r, float& g, float& b );
}
