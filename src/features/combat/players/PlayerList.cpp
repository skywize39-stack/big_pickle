#include "features/combat/players/PlayerList.hpp"
#include "sdk/SDK.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

// ============================================================
//  PlayerList.cpp — session index map + persistent entry store
//
//  Threading contract:
//    - UpdateFromSnapshot / ClearSession: game thread (CreateMove).
//    - AimAllowed / IsPreferred / GetVisualOverride: game thread
//      (rage, triggerbot) and render thread (ESP, chams).
//    - Menu + config access happens on the render thread.
//  Everything funnels through one short-critical-section mutex;
//  entries are always handed out as copies so no pointer can
//  escape across threads.
// ============================================================

namespace
{
    std::mutex g_plMutex;

    // Session roster: entity index -> display nickname (+ lowercase key).
    std::string g_nameDisplay[65];
    std::string g_nameKey[65];

    // Persistent overrides keyed by lowercase nickname.
    std::unordered_map<std::string, PlayerList::Entry> g_entries;

    std::string WideToUtf8( const std::wstring& wide )
    {
        if ( wide.empty() )
            return {};
        const int bytes = WideCharToMultiByte( CP_UTF8, 0, wide.data(),
            static_cast<int>( wide.size() ), nullptr, 0, nullptr, nullptr );
        if ( bytes <= 0 )
            return {};
        std::string out( static_cast<size_t>( bytes ), '\0' );
        if ( WideCharToMultiByte( CP_UTF8, 0, wide.data(), static_cast<int>( wide.size() ),
            out.data(), bytes, nullptr, nullptr ) != bytes )
            return {};
        return out;
    }

    std::wstring MultiByteToWide( const std::string& text, UINT codepage, DWORD flags )
    {
        if ( text.empty() )
            return {};
        const int chars = MultiByteToWideChar( codepage, flags, text.data(),
            static_cast<int>( text.size() ), nullptr, 0 );
        if ( chars <= 0 )
            return {};
        std::wstring out( static_cast<size_t>( chars ), L'\0' );
        if ( MultiByteToWideChar( codepage, flags, text.data(), static_cast<int>( text.size() ),
            out.data(), chars ) != chars )
            return {};
        return out;
    }

    // Engine player_info names are expected to be UTF-8. If the matching CSSO
    // server provides an ANSI/ACP nickname instead, convert it once to UTF-8
    // for ImGui instead of passing broken bytes into the renderer.
    std::string NormalizeNameToUtf8( const std::string& raw )
    {
        if ( raw.empty() )
            return {};
        std::wstring wide = MultiByteToWide( raw, CP_UTF8, MB_ERR_INVALID_CHARS );
        if ( wide.empty() )
            wide = MultiByteToWide( raw, CP_ACP, 0 );
        return WideToUtf8( wide );
    }

    std::string ToLower( const std::string& utf8 )
    {
        std::wstring wide = MultiByteToWide( utf8, CP_UTF8, MB_ERR_INVALID_CHARS );
        if ( !wide.empty() )
        {
            CharLowerBuffW( wide.data(), static_cast<DWORD>( wide.size() ) );
            if ( const std::string normalized = WideToUtf8( wide ); !normalized.empty() )
                return normalized;
        }

        std::string fallback = utf8;
        std::transform( fallback.begin(), fallback.end(), fallback.begin(),
            []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
        return fallback;
    }

    const PlayerList::Entry* FindEntryLocked( int entityIndex )
    {
        if ( entityIndex <= 0 || entityIndex >= 65 )
            return nullptr;
        const std::string& key = g_nameKey[ entityIndex ];
        if ( key.empty() )
            return nullptr;
        const auto it = g_entries.find( key );
        return it == g_entries.end() ? nullptr : &it->second;
    }
}

namespace PlayerList
{
    void UpdateFromSnapshot( const EntitySnapshot& snap )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );

        // Snapshot data is intentionally optimized for active entities and can
        // omit dead players/spectators. Player List must show the whole current
        // client roster, so query the confirmed IVEngineClient::GetPlayerInfo
        // path for every valid client slot and use snapshot names only as a
        // conservative fallback when player-info is temporarily unavailable.
        int max = snap.maxClients;
        if ( SDK::g_pEngine )
        {
            const int engineMax = SDK::g_pEngine->GetMaxClients();
            if ( engineMax > 0 && engineMax <= 64 )
                max = engineMax;
        }
        if ( max < 0 ) max = 0;
        if ( max > 64 ) max = 64;

        // The local client is not a Player List roster entry. Clear its slot
        // explicitly so a stale previous name cannot remain after reconnect.
        const int localPlayer = SDK::g_pEngine ? SDK::g_pEngine->GetLocalPlayer() : 0;
        for ( int i = 1; i <= max; ++i )
        {
            if ( i == localPlayer )
            {
                g_nameDisplay[ i ].clear();
                g_nameKey[ i ].clear();
                continue;
            }

            std::string rawName;
            SDK::player_info_t info{};
            if ( SDK::g_pEngine && SDK::g_pEngine->GetPlayerInfo( i, &info ) &&
                 info.name[ 0 ] != '\0' )
            {
                const size_t length = strnlen_s( info.name, sizeof( info.name ) );
                rawName.assign( info.name, length );
            }
            else if ( snap.players[ i ].name[ 0 ] != '\0' )
            {
                rawName = snap.players[ i ].name;
            }

            const std::string display = NormalizeNameToUtf8( rawName );
            if ( !display.empty() )
            {
                g_nameDisplay[ i ] = display;
                g_nameKey[ i ]     = ToLower( display );
            }
            else
            {
                g_nameDisplay[ i ].clear();
                g_nameKey[ i ].clear();
            }
        }

        for ( int i = max + 1; i < 65; ++i )
        {
            g_nameDisplay[ i ].clear();
            g_nameKey[ i ].clear();
        }
    }

    void ClearSession()
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        for ( int i = 0; i < 65; ++i )
        {
            g_nameDisplay[ i ].clear();
            g_nameKey[ i ].clear();
        }
    }

    bool AimAllowed( int entityIndex )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        const Entry* e = FindEntryLocked( entityIndex );
        return !e || e->aim;
    }

    bool IsPreferred( int entityIndex )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        const Entry* e = FindEntryLocked( entityIndex );
        return e && e->prefer;
    }

    Override GetVisualOverride( int entityIndex )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        Override o;
        if ( const Entry* e = FindEntryLocked( entityIndex ) )
        {
            o.hide      = !e->visuals;
            o.highlight = e->highlight;
            o.hl_r = e->hl_r;  o.hl_g = e->hl_g;
            o.hl_b = e->hl_b;  o.hl_a = e->hl_a;
        }
        return o;
    }

    std::vector<OnlineRow> GetOnlineRows()
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        std::vector<OnlineRow> rows;
        for ( int i = 1; i < 65; ++i )
            if ( !g_nameDisplay[ i ].empty() )
                rows.push_back( { i, g_nameDisplay[ i ] } );
        return rows;
    }

    bool TryGetCopy( const std::string& nickname, Entry& out )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        const auto it = g_entries.find( ToLower( nickname ) );
        if ( it == g_entries.end() )
            return false;
        out = it->second;
        return true;
    }

    void Commit( const std::string& nickname, const Entry& in )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        g_entries[ ToLower( nickname ) ] = in;
    }

    void Remove( const std::string& nickname )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        g_entries.erase( ToLower( nickname ) );
    }

    void ClearAll()
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        g_entries.clear();
    }

    std::vector<std::pair<std::string, Entry>> Export()
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        return { g_entries.begin(), g_entries.end() };
    }

    void Import( const std::vector<std::pair<std::string, Entry>>& rows )
    {
        std::lock_guard<std::mutex> lock( g_plMutex );
        g_entries.clear();
        for ( const auto& row : rows )
            g_entries[ ToLower( row.first ) ] = row.second;
    }

    std::string EncodeName( const std::string& name )
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve( name.size() );
        for ( const unsigned char c : name )
        {
            const bool safe = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
                ( c >= '0' && c <= '9' ) || c == '-' || c == '_' ||
                c >= 128;   // keep multibyte UTF-8 readable in the file
            if ( safe )
            {
                out += static_cast<char>( c );
            }
            else
            {
                out += '%';
                out += hex[ c >> 4 ];
                out += hex[ c & 0xF ];
            }
        }
        return out;
    }

    std::string DecodeName( const std::string& encoded )
    {
        const auto nibble = []( char c ) -> int
        {
            if ( c >= '0' && c <= '9' ) return c - '0';
            if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
            if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
            return -1;
        };

        std::string out;
        out.reserve( encoded.size() );
        for ( size_t i = 0; i < encoded.size(); ++i )
        {
            if ( encoded[ i ] == '%' && i + 2 < encoded.size() )
            {
                const int hi = nibble( encoded[ i + 1 ] );
                const int lo = nibble( encoded[ i + 2 ] );
                if ( hi >= 0 && lo >= 0 )
                {
                    out += static_cast<char>( ( hi << 4 ) | lo );
                    i += 2;
                    continue;
                }
            }
            out += encoded[ i ];
        }
        return out;
    }
}
