#pragma once

// ============================================================
//  ESPSkeleton.hpp — skeleton ESP
//
//  Draw receives the per-player bone positions already converted to
//  screen space by the game thread (CreateMove hook). This module is a
//  pure renderer — it never calls SetupBones, reads entity memory, or
//  does world-to-screen. Those happen in the snapshot path so they run
//  on the thread the engine expects.
//
//  Bone hierarchy comes from mstudiobone_t.parent (216-byte table,
//  offset +4). Each bone is drawn as a line to its parent, but only
//  if it is a BONE_USED_BY_HITBOX bone (0x200 flag).
// ============================================================

#include "sdk/GameTypes.hpp"

namespace ESPSkeleton
{
    // bonesScreen:   kMaxBones screen-space positions (indexed by bone index).
    // boneVisible:   kMaxBones per-bone on-screen flags.
    // boneParent:    kMaxBones parent indices (-1 = root / not drawn).
    // boneHitbox:    kMaxBones true if BONE_USED_BY_HITBOX.
    // numBones:      actual bone count (0..kMaxBones).
    // alpha:         0..1 fade multiplier.
    void Draw( const Vec2 bonesScreen[kMaxBones],
               const bool boneVisible[kMaxBones],
               const int8_t boneParent[kMaxBones],
               const bool boneHitbox[kMaxBones],
               int numBones,
               float alpha );
}
