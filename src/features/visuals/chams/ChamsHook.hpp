#pragma once

// ============================================================
//  ChamsHook.hpp — DrawModelExecute vtable detour
// ============================================================

// The detour function itself (called by the vtable patch).
void __fastcall hkDrawModelExecute( void* thisptr, void* pState,
                                    void* pInfoRaw, void* pRenderData );
