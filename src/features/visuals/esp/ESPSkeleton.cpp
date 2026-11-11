#include "features/visuals/esp/ESPSkeleton.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include "sdk/GameTypes.hpp"

void ESPSkeleton::Draw( const Vec2 bonesScreen[kMaxBones],
                        const bool boneVisible[kMaxBones],
                        const int8_t boneParent[kMaxBones],
                        const bool boneHitbox[kMaxBones],
                        int numBones,
                        float alpha )
{
    if ( numBones <= 0 || numBones > kMaxBones )
        return;

    const ImU32 color = ESPDraw::Col( 255, 255, 255, static_cast<int>( alpha * 255.f ) );

    for ( int i = 0; i < numBones; ++i )
    {
        if ( !boneHitbox[i] )
            continue;

        const int parent = boneParent[i];
        if ( parent < 0 || parent >= numBones )
            continue;

        if ( !boneVisible[i] || !boneVisible[parent] )
            continue;

        const Vec2& a = bonesScreen[i];
        const Vec2& b = bonesScreen[parent];

        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        if ( dx * dx + dy * dy < 1.f )
            continue;

        ESPDraw::Line( (int)a.x, (int)a.y, (int)b.x, (int)b.y, color );
    }
}
