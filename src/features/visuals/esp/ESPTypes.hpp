#pragma once

// ============================================================
//  ESPTypes.hpp — shared data types for the ESP subsystem
// ============================================================

#include "utils/Math.hpp"

// Screen-space bounding box for a player
struct PlayerBoxData
{
    Vec2  top{};
    Vec2  bot{};
    float height = 0.f;
    float width  = 0.f;
};
