#pragma once

// ============================================================
//  AntiAim.hpp — anti-aim, runs in game thread (CreateMove)
//
//  Reversed from client.dll (CSSO):
//    - CInput::CreateMove is already hooked (CreateMoveHook)
//    - cmd->viewangles are ScrambledFloat9 (SetYaw/SetPitch)
//    - bSendPacket is not exposed in this CreateMove signature;
//      real/fake split uses FakeLag::s_shouldChoke instead.
//
//  The last applied fake angles are stored so the FrameStageNotify
//  hook can mirror them onto the local model for thirdperson.
// ============================================================

namespace AntiAim
{
    // Last fake angles written into the cmd (for thirdperson visual)
    extern float lastFakeYaw;
    extern float lastFakePitch;

    // 0 = none, 1 = left, 2 = right, 3 = back
    extern int manualSide;
    // 0 = inactive, 1 = head left, 2 = head right (last freestand decision)
    extern int freestandSide;

    void Run( void* pCInput, int seq, void* pLocalPlayer );

    // Crosshair arrows + FS label (render thread)
    void DrawIndicator();
}