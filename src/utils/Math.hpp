#pragma once

// ============================================================
//  Math.hpp — basic math / colour types used across the project
// ============================================================

#include "platform/windows/Platform.hpp"

// ------------------------------------------------------------------
//  Vec2
// ------------------------------------------------------------------
struct Vec2
{
    float x = 0.f, y = 0.f;

    void Clear() { x = y = 0.f; }
};

// ------------------------------------------------------------------
//  Vec3
// ------------------------------------------------------------------
struct Vec3
{
    float x = 0.f, y = 0.f, z = 0.f;

    void Clear() { x = y = z = 0.f; }

    Vec3 operator+( const Vec3& o ) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-( const Vec3& o ) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*( float s )       const { return { x * s,   y * s,   z * s   }; }
    Vec3 operator+( float v )       const { return { x + v,   y + v,   z + v   }; }
    Vec3 operator-( float v )       const { return { x - v,   y - v,   z - v   }; }

    Vec3& operator+=( const Vec3& o ) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=( const Vec3& o ) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator+=( float v )       { x += v;   y += v;   z += v;   return *this; }
    Vec3& operator-=( float v )       { x -= v;   y -= v;   z -= v;   return *this; }
};

// ------------------------------------------------------------------
//  Color
// ------------------------------------------------------------------
struct Color
{
    uint8_t r = 255, g = 255, b = 255, a = 255;

    D3DCOLOR GetD3DColor() const { return D3DCOLOR_RGBA( r, g, b, a ); }
};
