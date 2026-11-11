#pragma once

#include "utils/Math.hpp"

// ============================================================
//  ESPManager.hpp — per-frame ESP update and draw dispatcher
// ============================================================

namespace Visuals
{
    // Game-thread projection pass (FrameStageNotify, FRAME_RENDER_END):
    // converts the world snapshot to screen coordinates using the coherent
    // view matrix. Runs every rendered frame — including frames whose
    // CreateMove was skipped by DT recharge — and keeps W2S off the render
    // thread, where torn matrix reads caused the box flicker.
    void ProjectFrame();

    // Anchor marker screen position produced by the last ProjectFrame pass.
    bool GetAnchorScreen( Vec2& outPos, bool& outReturning );

    // Unit 2D directions for live enemies that are behind/outside the screen,
    // projected on the game thread from the coherent camera snapshot.
    bool GetOffscreenArrowDirections( Vec2 outDirections[65] );
}
