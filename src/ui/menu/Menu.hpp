#pragma once

#include <d3d9.h>

struct ImFont;

namespace Menu
{
	void Render( IDirect3DDevice9* pDevice );
	void InvalidateDeviceObjects();

	inline bool bOpen = true; // open on the first rendered frame after injection

	// Menu chrome fonts, built once during ImGui init in D3DHook. Null until
	// then and on load failure — Render falls back to the default font.
	inline ImFont* f_title  = nullptr;
	inline ImFont* f_header = nullptr;
}
