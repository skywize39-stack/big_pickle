#pragma once

// ============================================================
//  Platform.hpp — Windows / STL / DirectX compatibility includes
//  Legacy umbrella. Prefer explicit dependencies in new code.
// ============================================================

#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <format>

using namespace std::chrono_literals;

#include "core/diagnostics/DebugLog.hpp"

#include <d3d9.h>
#include <d3dx9.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

#include "third_party/MinHook/minhook.h"
