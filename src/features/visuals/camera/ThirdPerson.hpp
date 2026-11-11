#pragma once

// ============================================================
//  ThirdPerson.hpp — thirdperson camera, runs in game thread
//
//  Reversed from client.dll (CSSO):
//    sub_1801CD440 = CInput::CAM_ToThirdPerson (the "thirdperson"
//                    command handler — sets camera offset + flag)
//    sub_1801CD320 = CInput::CAM_ToFirstPerson
//    byte at 0x8A7584 = m_fCameraInThirdPerson flag
//
//  Online servers run with sv_cheats 0. CInput::CAM_Think calls
//  sub_18015FDB0 every frame, which when sv_cheats == 0 bounces
//  through engine vfunc 264 (camera reset to first person) and
//  never reaches the toggle logic — killing thirdperson each tick.
//  Init() patches that 4-byte check (85 C0 75 47 -> EB 49 90 90)
//  so sv_cheats==0 flows into the normal toggle path instead.
// ============================================================

namespace ThirdPerson
{
    // Patch the sv_cheats check inside sub_18015FDB0 (called by CAM_Think).
    void Init();
    // Restore the original bytes.
    void Shutdown();

    // Updates the camera request from the configured Always/Hold/Toggle mode.
    void Run();

    // True only after the native thirdperson handler has been enabled.
    bool IsActive();
}