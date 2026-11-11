#pragma once

// ============================================================
//  VoiceKill.hpp — canonical WAV playback through matching-engine
//  Voice_RecordStart for local kill and local death event libraries.
// ============================================================

namespace VoiceKill
{
    // Creates voice_kills and voice_deaths in the game working directory,
    // seeds the supplied kill clip if missing, and scans compatible WAV files.
    bool Init();

    // Re-scan either library after adding/removing WAV files. RefreshLibrary
    // remains as a compatibility alias that refreshes both folders.
    void RefreshKillLibrary();
    void RefreshDeathLibrary();
    void RefreshLibrary();

    int GetKillClipCount();
    const char* GetKillClipName( int index );
    const char* GetKillLibraryFolder();

    int GetDeathClipCount();
    const char* GetDeathClipName( int index );
    const char* GetDeathLibraryFolder();

    // Legacy kill-library aliases retained for existing callers.
    int GetClipCount();
    const char* GetClipName( int index );
    const char* GetLibraryFolder();

    // The event listener routes confirmed local attacker and local victim
    // player_death events here independently.
    void OnLocalKill();
    void OnLocalDeath();

    // Called every render frame to end voice recording when the WAV is done.
    void Update();

    // Stops recording and resets state during unload.
    void Shutdown();
}
