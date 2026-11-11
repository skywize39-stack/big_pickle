#include "features/misc/voicekill/VoiceKill.hpp"

#include "config/Config.hpp"
#include "core/GlobalVars.hpp"
#include "sdk/SDK.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr int kVoiceKillWavResource = 301;
    constexpr char kVoiceKillFolderName[] = "voice_kills";
    constexpr char kVoiceDeathFolderName[] = "voice_deaths";
    constexpr char kSeedClipName[] = "mellstroy-raduet.wav";
    constexpr char kVoiceInputFile[] = "voice_input.wav";
    constexpr DWORD kMaxVoiceWavBytes = 32u * 1024u * 1024u;
    constexpr DWORD kLibrarySampleRate = 11025;
    constexpr uintptr_t kVoiceRecordStartRva = 0x7ACC0;
    constexpr uintptr_t kVoiceRecordStopRva = 0x7B180;
    // Matching engine.dll globals assigned only by Voice_RecordStart's WAV
    // loader: data pointer at 0x64D060 and byte count at 0x64D068.
    constexpr uintptr_t kVoiceInputDataRva = 0x64D060;
    constexpr uintptr_t kVoiceInputBytesRva = 0x64D068;
    // Updated by engine voice init from Steam's optimal input rate. Matching
    // xrefs at engine+0x7AA98 / +0x7AAD3 write rate and rate*2 here.
    constexpr uintptr_t kVoiceEncoderSampleRateRva = 0x57C24C;
    constexpr uint8_t kVoiceRecordStartPrologue[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
        0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40,
    };
    constexpr uint8_t kVoiceRecordStopPrologue[] = {
        0x48, 0x83, 0xEC, 0x28, 0x80, 0x3D,
    };

    using VoiceRecordStartFn = bool( __fastcall* )( const char* micDataOutput,
        const char* decompressedOutput, const char* inputWavPath );
    using VoiceRecordStopFn = void( __fastcall* )( );

    bool s_playing = false;
    ULONGLONG s_stopAtMs = 0;
    char s_killLibraryFolder[ MAX_PATH ]{};
    char s_deathLibraryFolder[ MAX_PATH ]{};
    std::vector<std::string> s_killClips;
    std::vector<std::string> s_deathClips;
    size_t s_killSequenceIndex = 0;
    size_t s_deathSequenceIndex = 0;
    uint32_t s_randomState = 0xA53C9E17u;

    static uint16_t ReadLe16( const uint8_t* p )
    {
        return static_cast<uint16_t>( p[ 0 ] ) |
            static_cast<uint16_t>( p[ 1 ] << 8 );
    }

    static uint32_t ReadLe32( const uint8_t* p )
    {
        return static_cast<uint32_t>( p[ 0 ] ) |
            ( static_cast<uint32_t>( p[ 1 ] ) << 8 ) |
            ( static_cast<uint32_t>( p[ 2 ] ) << 16 ) |
            ( static_cast<uint32_t>( p[ 3 ] ) << 24 );
    }

    static bool HasFourCc( const uint8_t* p, const char ( &value )[ 5 ] )
    {
        return std::memcmp( p, value, 4 ) == 0;
    }

    // Voice input is deliberately strict: the confirmed seed is 16-bit mono
    // PCM 11025 Hz, so reject malformed or incompatible user WAV files before
    // ever handing them to the engine voice recorder.
    static bool GetCompatibleWaveDurationMs( const uint8_t* bytes, DWORD size,
        ULONGLONG& outMs, const uint8_t** outPcm = nullptr,
        DWORD* outPcmBytes = nullptr )
    {
        outMs = 0;
        if ( outPcm )
            *outPcm = nullptr;
        if ( outPcmBytes )
            *outPcmBytes = 0;
        if ( !bytes || size < 20 || !HasFourCc( bytes, "RIFF" ) ||
             !HasFourCc( bytes + 8, "WAVE" ) )
            return false;

        uint32_t byteRate = 0;
        bool compatibleFmt = false;
        DWORD cursor = 12;
        while ( cursor + 8 <= size )
        {
            const uint8_t* const chunk = bytes + cursor;
            const uint32_t chunkSize = ReadLe32( chunk + 4 );
            const DWORD payload = cursor + 8;
            if ( payload > size || chunkSize > size - payload )
                return false;

            if ( HasFourCc( chunk, "fmt ") )
            {
                if ( chunkSize < 16 )
                    return false;
                const uint16_t format = ReadLe16( bytes + payload );
                const uint16_t channels = ReadLe16( bytes + payload + 2 );
                const uint32_t sampleRate = ReadLe32( bytes + payload + 4 );
                const uint16_t bits = ReadLe16( bytes + payload + 14 );
                byteRate = ReadLe32( bytes + payload + 8 );
                compatibleFmt = format == 1 && channels == 1 &&
                    sampleRate == kLibrarySampleRate && bits == 16 &&
                    byteRate == kLibrarySampleRate * 2;
            }
            else if ( HasFourCc( chunk, "data") )
            {
                if ( !compatibleFmt || !byteRate || !chunkSize )
                    return false;
                outMs = ( static_cast<ULONGLONG>( chunkSize ) * 1000ull +
                    byteRate - 1ull ) / byteRate;
                if ( outMs == 0 )
                    return false;
                if ( outPcm )
                    *outPcm = bytes + payload;
                if ( outPcmBytes )
                    *outPcmBytes = chunkSize;
                return true;
            }

            const uint64_t next = static_cast<uint64_t>( payload ) + chunkSize +
                ( chunkSize & 1u );
            if ( next > size )
                return false;
            cursor = static_cast<DWORD>( next );
        }
        return false;
    }

    static bool ReadCompatibleWaveDurationMs( const char* path, ULONGLONG& outMs )
    {
        outMs = 0;
        if ( !path || !path[ 0 ] )
            return false;

        HANDLE file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
            return false;

        const DWORD size = GetFileSize( file, nullptr );
        if ( size == INVALID_FILE_SIZE || size < 44 || size > kMaxVoiceWavBytes )
        {
            CloseHandle( file );
            return false;
        }

        std::vector<uint8_t> bytes( size );
        DWORD read = 0;
        const bool ok = ReadFile( file, bytes.data(), size, &read, nullptr ) &&
            read == size;
        CloseHandle( file );
        return ok && GetCompatibleWaveDurationMs( bytes.data(), size, outMs );
    }

    static bool ReadCompatibleWavePcm( const char* path, ULONGLONG& outMs,
        std::vector<uint8_t>& outPcm )
    {
        outMs = 0;
        outPcm.clear();
        if ( !path || !path[ 0 ] )
            return false;

        HANDLE file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
            return false;

        const DWORD size = GetFileSize( file, nullptr );
        if ( size == INVALID_FILE_SIZE || size < 44 || size > kMaxVoiceWavBytes )
        {
            CloseHandle( file );
            return false;
        }

        std::vector<uint8_t> bytes( size );
        DWORD read = 0;
        const bool readOk = ReadFile( file, bytes.data(), size, &read, nullptr ) &&
            read == size;
        CloseHandle( file );
        if ( !readOk )
            return false;

        const uint8_t* pcm = nullptr;
        DWORD pcmBytes = 0;
        if ( !GetCompatibleWaveDurationMs( bytes.data(), size, outMs, &pcm,
                                           &pcmBytes ) || !pcm || !pcmBytes )
            return false;
        outPcm.assign( pcm, pcm + pcmBytes );
        return true;
    }

    static bool ResampleLibraryPcmForEngine( const std::vector<uint8_t>& source,
        DWORD targetRate, std::vector<uint8_t>& outPcm )
    {
        outPcm.clear();
        if ( source.empty() || ( source.size() & 1u ) || targetRate < 8000 ||
             targetRate > 48000 )
            return false;
        if ( targetRate == kLibrarySampleRate )
        {
            outPcm = source;
            return true;
        }

        const uint64_t sourceFrames = source.size() / 2u;
        const uint64_t targetFrames = ( sourceFrames * targetRate +
            kLibrarySampleRate / 2u ) / kLibrarySampleRate;
        if ( !targetFrames || targetFrames > kMaxVoiceWavBytes / 2u )
            return false;

        outPcm.resize( static_cast<size_t>( targetFrames * 2u ) );
        const auto* const input = reinterpret_cast<const int16_t*>( source.data() );
        auto* const output = reinterpret_cast<int16_t*>( outPcm.data() );
        for ( uint64_t i = 0; i < targetFrames; ++i )
        {
            const uint64_t scaled = i * kLibrarySampleRate;
            const uint64_t left = scaled / targetRate;
            const uint64_t right = std::min<uint64_t>( left + 1u,
                sourceFrames - 1u );
            const uint64_t fraction = scaled % targetRate;
            const int64_t a = input[ left ];
            const int64_t b = input[ right ];
            output[ i ] = static_cast<int16_t>( a +
                ( ( b - a ) * static_cast<int64_t>( fraction ) +
                  static_cast<int64_t>( targetRate / 2u ) ) /
                static_cast<int64_t>( targetRate ) );
        }
        return true;
    }

    static bool EnsureLibraryFolder( const char* folderName,
        char ( &outFolder )[ MAX_PATH ] )
    {
        if ( outFolder[ 0 ] )
            return true;
        if ( !folderName || !folderName[ 0 ] )
            return false;

        // The game process working directory is its actual game folder. Keep
        // user clips there instead of beside a DLL that may have been injected
        // from a separate downloads/tools directory.
        char gameDirectory[ MAX_PATH ]{};
        const DWORD copied = GetCurrentDirectoryA( static_cast<DWORD>(
            sizeof( gameDirectory ) ), gameDirectory );
        if ( !copied || copied >= sizeof( gameDirectory ) )
            return false;

        const int written = _snprintf_s( outFolder, sizeof( outFolder ),
            _TRUNCATE, "%s\\%s", gameDirectory, folderName );
        if ( written < 0 )
            return false;

        if ( !CreateDirectoryA( outFolder, nullptr ) &&
             GetLastError() != ERROR_ALREADY_EXISTS )
            return false;
        return true;
    }

    static bool EnsureKillLibraryFolder()
    {
        return EnsureLibraryFolder( kVoiceKillFolderName, s_killLibraryFolder );
    }

    static bool EnsureDeathLibraryFolder()
    {
        return EnsureLibraryFolder( kVoiceDeathFolderName, s_deathLibraryFolder );
    }

    static bool ExtractSeedClipIfMissing()
    {
        if ( !EnsureKillLibraryFolder() )
            return false;

        char seedPath[ MAX_PATH ]{};
        if ( _snprintf_s( seedPath, sizeof( seedPath ), _TRUNCATE, "%s\\%s",
                          s_killLibraryFolder, kSeedClipName ) < 0 )
            return false;
        if ( GetFileAttributesA( seedPath ) != INVALID_FILE_ATTRIBUTES )
            return true;

        HRSRC resource = FindResourceA( g_hModule,
            MAKEINTRESOURCEA( kVoiceKillWavResource ), RT_RCDATA );
        if ( !resource )
            return false;
        const DWORD size = SizeofResource( g_hModule, resource );
        HGLOBAL data = LoadResource( g_hModule, resource );
        const auto* bytes = data
            ? static_cast<const uint8_t*>( LockResource( data ) ) : nullptr;
        ULONGLONG durationMs = 0;
        if ( !bytes || !size || !GetCompatibleWaveDurationMs( bytes, size, durationMs ) )
            return false;

        HANDLE file = CreateFileA( seedPath, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
            return false;
        DWORD written = 0;
        const bool ok = WriteFile( file, bytes, size, &written, nullptr ) &&
            written == size;
        CloseHandle( file );
        if ( !ok )
        {
            DeleteFileA( seedPath );
            return false;
        }
        return true;
    }

    static bool HasWavExtension( const char* filename )
    {
        if ( !filename )
            return false;
        const size_t length = std::strlen( filename );
        if ( length < 5 )
            return false;
        const char* extension = filename + length - 4;
        return ( extension[ 0 ] == '.' ) &&
            ( extension[ 1 ] == 'w' || extension[ 1 ] == 'W' ) &&
            ( extension[ 2 ] == 'a' || extension[ 2 ] == 'A' ) &&
            ( extension[ 3 ] == 'v' || extension[ 3 ] == 'V' );
    }

    static bool IsSimpleFilename( const char* filename )
    {
        return filename && filename[ 0 ] && !std::strchr( filename, '\\' ) &&
            !std::strchr( filename, '/' ) && !std::strchr( filename, ':' );
    }

    static bool BuildClipPath( const char* libraryFolder, const char* filename,
        char (&outPath)[ MAX_PATH ] )
    {
        if ( !libraryFolder || !libraryFolder[ 0 ] || !IsSimpleFilename( filename ) ||
             !HasWavExtension( filename ) )
            return false;
        return _snprintf_s( outPath, sizeof( outPath ), _TRUNCATE, "%s\\%s",
            libraryFolder, filename ) >= 0;
    }

    static bool IsListedClip( const std::vector<std::string>& clips,
        const char* filename )
    {
        for ( const auto& clip : clips )
        {
            if ( clip == filename )
                return true;
        }
        return false;
    }

    static void WriteLe16( uint8_t* p, uint16_t value )
    {
        p[ 0 ] = static_cast<uint8_t>( value & 0xFFu );
        p[ 1 ] = static_cast<uint8_t>( value >> 8 );
    }

    static void WriteLe32( uint8_t* p, uint32_t value )
    {
        p[ 0 ] = static_cast<uint8_t>( value & 0xFFu );
        p[ 1 ] = static_cast<uint8_t>( ( value >> 8 ) & 0xFFu );
        p[ 2 ] = static_cast<uint8_t>( ( value >> 16 ) & 0xFFu );
        p[ 3 ] = static_cast<uint8_t>( value >> 24 );
    }

    static bool WriteCanonicalVoiceInputToDirectory( const std::vector<uint8_t>& pcm,
        DWORD sampleRate, const char* directory )
    {
        if ( pcm.empty() || pcm.size() > kMaxVoiceWavBytes || sampleRate < 8000 ||
             sampleRate > 48000 || !directory || !directory[ 0 ] )
            return false;

        char target[ MAX_PATH ]{};
        if ( _snprintf_s( target, sizeof( target ), _TRUNCATE, "%s\\%s",
                          directory, kVoiceInputFile ) < 0 )
            return false;

        // The engine loader does not enumerate RIFF chunks. It treats offset
        // 0x24 as `data` and offset 0x28 as its byte count, so write exactly
        // RIFF(36 + PCM) + WAVE + fmt(16) + data + PCM with no metadata.
        uint8_t header[ 44 ]{};
        std::memcpy( header + 0, "RIFF", 4 );
        WriteLe32( header + 4, 36u + static_cast<uint32_t>( pcm.size() ) );
        std::memcpy( header + 8, "WAVEfmt ", 8 );
        WriteLe32( header + 16, 16 );
        WriteLe16( header + 20, 1 );
        WriteLe16( header + 22, 1 );
        WriteLe32( header + 24, sampleRate );
        WriteLe32( header + 28, sampleRate * 2u );
        WriteLe16( header + 32, 2 );
        WriteLe16( header + 34, 16 );
        std::memcpy( header + 36, "data", 4 );
        WriteLe32( header + 40, static_cast<uint32_t>( pcm.size() ) );

        HANDLE file = CreateFileA( target, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if ( file == INVALID_HANDLE_VALUE )
            return false;
        DWORD written = 0;
        const bool headerOk = WriteFile( file, header, sizeof( header ), &written,
            nullptr ) && written == sizeof( header );
        const bool pcmOk = headerOk && WriteFile( file, pcm.data(),
            static_cast<DWORD>( pcm.size() ), &written, nullptr ) &&
            written == pcm.size();
        CloseHandle( file );
        return pcmOk;
    }

    static void StageKnownModRoots( const std::vector<uint8_t>& pcm, DWORD sampleRate,
        const char* directory, int depthLeft, int& outCopies, int& outModRoots )
    {
        if ( pcm.empty() || sampleRate < 8000 || sampleRate > 48000 || !directory ||
             !directory[ 0 ] || depthLeft < 0 )
            return;

        // `MOD` resolves to the directory containing this gameinfo.txt. The
        // executable is not always directly above it in CSSO installations,
        // so find only real Source game roots rather than guessing a name.
        char gameInfo[ MAX_PATH ]{};
        if ( _snprintf_s( gameInfo, sizeof( gameInfo ), _TRUNCATE,
                          "%s\\gameinfo.txt", directory ) >= 0 &&
             GetFileAttributesA( gameInfo ) != INVALID_FILE_ATTRIBUTES )
        {
            ++outModRoots;
            if ( WriteCanonicalVoiceInputToDirectory( pcm, sampleRate, directory ) )
                ++outCopies;
        }

        if ( depthLeft == 0 )
            return;

        char query[ MAX_PATH ]{};
        if ( _snprintf_s( query, sizeof( query ), _TRUNCATE, "%s\\*", directory ) < 0 )
            return;

        WIN32_FIND_DATAA findData{};
        HANDLE find = FindFirstFileA( query, &findData );
        if ( find == INVALID_HANDLE_VALUE )
            return;

        do
        {
            if ( !( findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ||
                 ( findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) ||
                 findData.cFileName[ 0 ] == '.' )
                continue;

            char child[ MAX_PATH ]{};
            if ( _snprintf_s( child, sizeof( child ), _TRUNCATE, "%s\\%s",
                              directory, findData.cFileName ) >= 0 )
                StageKnownModRoots( pcm, sampleRate, child, depthLeft - 1,
                    outCopies, outModRoots );
        } while ( FindNextFileA( find, &findData ) );
        FindClose( find );
    }

    static bool StageVoiceInputForModRoot( const std::vector<uint8_t>& pcm,
        DWORD sampleRate, int& outCopies, int& outModRoots )
    {
        outCopies = 0;
        outModRoots = 0;
        if ( pcm.empty() || pcm.size() > kMaxVoiceWavBytes || sampleRate < 8000 ||
             sampleRate > 48000 )
            return false;

        char currentDirectory[ MAX_PATH ]{};
        const DWORD currentLength = GetCurrentDirectoryA( static_cast<DWORD>(
            sizeof( currentDirectory ) ), currentDirectory );
        if ( currentLength && currentLength < sizeof( currentDirectory ) &&
             WriteCanonicalVoiceInputToDirectory( pcm, sampleRate, currentDirectory ) )
            ++outCopies;

        char exePath[ MAX_PATH ]{};
        const DWORD exeLength = GetModuleFileNameA( nullptr, exePath,
            static_cast<DWORD>( sizeof( exePath ) ) );
        if ( !exeLength || exeLength >= sizeof( exePath ) )
            return outCopies > 0;

        char* slash = std::strrchr( exePath, '\\' );
        if ( !slash )
            return outCopies > 0;
        *slash = '\0';

        if ( WriteCanonicalVoiceInputToDirectory( pcm, sampleRate, exePath ) )
            ++outCopies;

        // Search a bounded directory tree below the executable. This covers
        // launchers that put the active Source MOD root below another folder
        // while avoiding junction traversal and arbitrary disk-wide writes.
        StageKnownModRoots( pcm, sampleRate, exePath, 4, outCopies, outModRoots );
        return outCopies > 0;
    }

    static uint8_t* GetMatchingEngineBase()
    {
        return reinterpret_cast<uint8_t*>( GetModuleHandleA( "engine.dll" ) );
    }

    static DWORD GetEngineVoiceSampleRate()
    {
        auto* const engine = GetMatchingEngineBase();
        if ( !engine )
            return kLibrarySampleRate;

        const DWORD rate = *reinterpret_cast<const DWORD*>(
            engine + kVoiceEncoderSampleRateRva );
        // Accept only standard mono voice rates. A stale/uninitialised global
        // must fall back to the validated library rate rather than corrupt PCM.
        return rate >= 8000 && rate <= 48000 ? rate : kLibrarySampleRate;
    }

    static VoiceRecordStartFn ResolveVoiceRecordStart()
    {
        auto* const engine = GetMatchingEngineBase();
        if ( !engine )
            return nullptr;

        const auto* const candidate = engine + kVoiceRecordStartRva;
        if ( std::memcmp( candidate, kVoiceRecordStartPrologue,
                          sizeof( kVoiceRecordStartPrologue ) ) != 0 )
            return nullptr;
        return reinterpret_cast<VoiceRecordStartFn>( candidate );
    }

    static VoiceRecordStopFn ResolveVoiceRecordStop()
    {
        auto* const engine = GetMatchingEngineBase();
        if ( !engine )
            return nullptr;

        const auto* const candidate = engine + kVoiceRecordStopRva;
        if ( std::memcmp( candidate, kVoiceRecordStopPrologue,
                          sizeof( kVoiceRecordStopPrologue ) ) != 0 )
            return nullptr;
        return reinterpret_cast<VoiceRecordStopFn>( candidate );
    }

    static bool GetLoadedVoiceInput( const void*& outData, DWORD& outBytes )
    {
        outData = nullptr;
        outBytes = 0;
        auto* const engine = GetMatchingEngineBase();
        if ( !engine )
            return false;

        // The checked engine build stores these exact globals in .data and
        // Voice_GetCompressedData reads the WAV branch only when data != 0.
        const auto* const dataSlot = reinterpret_cast<void* const*>(
            engine + kVoiceInputDataRva );
        const auto* const bytesSlot = reinterpret_cast<const DWORD*>(
            engine + kVoiceInputBytesRva );
        if ( !dataSlot || !bytesSlot )
            return false;

        outData = *dataSlot;
        outBytes = *bytesSlot;
        return outData != nullptr && outBytes > 0 && outBytes <= kMaxVoiceWavBytes;
    }

    static void StopPlayback()
    {
        if ( !s_playing )
            return;

        // Confirmed native body used by the -voicerecord callback. Calling it
        // directly avoids waiting for ClientCmd to be queued and makes the
        // clip stop on the frame its PCM duration expires.
        if ( const auto stop = ResolveVoiceRecordStop() )
            stop();
        else if ( SDK::g_pEngine )
            SDK::g_pEngine->ClientCmd( "-voicerecord" );
        s_playing = false;
        s_stopAtMs = 0;
    }
}

namespace VoiceKill
{
    void RefreshKillLibrary();
    void RefreshDeathLibrary();

    static void RefreshClipLibrary( const char* libraryFolder,
        std::vector<std::string>& clips, char ( &selected )[ 128 ] )
    {
        clips.clear();
        if ( !libraryFolder || !libraryFolder[ 0 ] )
            return;

        char query[ MAX_PATH ]{};
        if ( _snprintf_s( query, sizeof( query ), _TRUNCATE, "%s\\*.wav",
                          libraryFolder ) < 0 )
            return;

        WIN32_FIND_DATAA findData{};
        HANDLE find = FindFirstFileA( query, &findData );
        if ( find != INVALID_HANDLE_VALUE )
        {
            do
            {
                if ( ( findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ||
                     !IsSimpleFilename( findData.cFileName ) ||
                     !HasWavExtension( findData.cFileName ) )
                    continue;

                char candidate[ MAX_PATH ]{};
                ULONGLONG durationMs = 0;
                if ( BuildClipPath( libraryFolder, findData.cFileName, candidate ) &&
                     ReadCompatibleWaveDurationMs( candidate, durationMs ) )
                    clips.emplace_back( findData.cFileName );
            } while ( FindNextFileA( find, &findData ) );
            FindClose( find );
        }

        std::sort( clips.begin(), clips.end() );
        if ( clips.empty() )
            selected[ 0 ] = '\0';
        else if ( !IsListedClip( clips, selected ) )
            strncpy_s( selected, clips.front().c_str(), _TRUNCATE );
    }

    static const char* SelectClip( const std::vector<std::string>& clips,
        const char* selected, int mode, size_t& sequenceIndex )
    {
        if ( clips.empty() )
            return nullptr;
        mode = std::clamp( mode, 0, 2 );
        if ( mode == 0 )
            return IsListedClip( clips, selected ) ? selected : clips.front().c_str();
        if ( mode == 1 )
        {
            // Deterministic xorshift avoids CRT rand/global state and still
            // gives a fresh library item on every local death event.
            s_randomState ^= s_randomState << 13;
            s_randomState ^= s_randomState >> 17;
            s_randomState ^= s_randomState << 5;
            return clips[ s_randomState % clips.size() ].c_str();
        }

        const char* next = clips[ sequenceIndex % clips.size() ].c_str();
        ++sequenceIndex;
        return next;
    }

    static void StartSelectedClip( const char* libraryFolder,
        const std::vector<std::string>& clips, const char* selected, int mode,
        size_t& sequenceIndex )
    {
        if ( s_playing || !SDK::g_pEngine || !SDK::g_pEngine->IsInGame() ||
             !SDK::g_pEngine->IsConnected() )
            return;

        const char* clipName = SelectClip( clips, selected, mode, sequenceIndex );
        if ( !clipName || !clipName[ 0 ] )
            return;

        char selectedPath[ MAX_PATH ]{};
        ULONGLONG durationMs = 0;
        std::vector<uint8_t> libraryPcm;
        const auto start = ResolveVoiceRecordStart();
        if ( !BuildClipPath( libraryFolder, clipName, selectedPath ) ||
             !ReadCompatibleWavePcm( selectedPath, durationMs, libraryPcm ) )
        {
            return;
        }

        // Library files stay strict 11025 Hz. Steam voice may initialise its
        // encoder at a different optimal rate (44100 on this observed path),
        // so resample the PCM to the rate that Voice_GetCompressedData uses.
        const DWORD engineRate = GetEngineVoiceSampleRate();
        std::vector<uint8_t> enginePcm;
        if ( !ResampleLibraryPcmForEngine( libraryPcm, engineRate, enginePcm ) )
        {
            return;
        }

        // CSSO's file voice loader assumes a bare, 44-byte PCM WAV header and
        // never skips metadata chunks. Re-pack the resampled PCM into that
        // exact layout before following the native MOD-path contract.
        int stagedCopies = 0;
        int stagedModRoots = 0;
        if ( !StageVoiceInputForModRoot( enginePcm, engineRate, stagedCopies,
                                         stagedModRoots ) )
        {
            return;
        }
        if ( !start )
            return;

        if ( !start( nullptr, nullptr, kVoiceInputFile ) )
            return;
        const void* loadedData = nullptr;
        DWORD loadedBytes = 0;
        if ( !GetLoadedVoiceInput( loadedData, loadedBytes ) )
        {
            if ( const auto stop = ResolveVoiceRecordStop() )
                stop();
            else
                SDK::g_pEngine->ClientCmd( "-voicerecord" );
            return;
        }

        s_playing = true;
        s_stopAtMs = GetTickCount64() + durationMs + 50ull;
    }

    bool Init()
    {
        if ( !EnsureKillLibraryFolder() || !EnsureDeathLibraryFolder() )
            return false;
        ExtractSeedClipIfMissing();
        RefreshKillLibrary();
        RefreshDeathLibrary();
        return true;
    }

    void RefreshKillLibrary()
    {
        if ( EnsureKillLibraryFolder() )
            RefreshClipLibrary( s_killLibraryFolder, s_killClips,
                g_config.settings.misc.voice_kill.selected );
    }

    void RefreshDeathLibrary()
    {
        if ( EnsureDeathLibraryFolder() )
            RefreshClipLibrary( s_deathLibraryFolder, s_deathClips,
                g_config.settings.misc.voice_kill.death_selected );
    }

    // Kept as a compatibility alias for existing menu calls/config workflows.
    void RefreshLibrary()
    {
        RefreshKillLibrary();
        RefreshDeathLibrary();
    }

    int GetKillClipCount()
    {
        return static_cast<int>( s_killClips.size() );
    }

    const char* GetKillClipName( int index )
    {
        if ( index < 0 || index >= static_cast<int>( s_killClips.size() ) )
            return "";
        return s_killClips[ index ].c_str();
    }

    const char* GetKillLibraryFolder()
    {
        EnsureKillLibraryFolder();
        return s_killLibraryFolder;
    }

    int GetDeathClipCount()
    {
        return static_cast<int>( s_deathClips.size() );
    }

    const char* GetDeathClipName( int index )
    {
        if ( index < 0 || index >= static_cast<int>( s_deathClips.size() ) )
            return "";
        return s_deathClips[ index ].c_str();
    }

    const char* GetDeathLibraryFolder()
    {
        EnsureDeathLibraryFolder();
        return s_deathLibraryFolder;
    }

    // Legacy names retain their prior kill-library meaning.
    int GetClipCount() { return GetKillClipCount(); }
    const char* GetClipName( int index ) { return GetKillClipName( index ); }
    const char* GetLibraryFolder() { return GetKillLibraryFolder(); }

    void OnLocalKill()
    {
        const auto& vk = g_config.settings.misc.voice_kill;
        if ( vk.enabled )
            StartSelectedClip( s_killLibraryFolder, s_killClips, vk.selected,
                vk.mode, s_killSequenceIndex );
    }

    void OnLocalDeath()
    {
        const auto& vk = g_config.settings.misc.voice_kill;
        if ( vk.death_enabled )
            StartSelectedClip( s_deathLibraryFolder, s_deathClips,
                vk.death_selected, vk.death_mode, s_deathSequenceIndex );
    }

    void Update()
    {
        if ( !s_playing )
            return;
        const auto& vk = g_config.settings.misc.voice_kill;
        if ( ( !vk.enabled && !vk.death_enabled ) || !SDK::g_pEngine ||
             !SDK::g_pEngine->IsInGame() || !SDK::g_pEngine->IsConnected() ||
             GetTickCount64() >= s_stopAtMs )
            StopPlayback();
    }

    void Shutdown()
    {
        StopPlayback();
        s_killClips.clear();
        s_deathClips.clear();
        s_killSequenceIndex = 0;
        s_deathSequenceIndex = 0;
    }
}
