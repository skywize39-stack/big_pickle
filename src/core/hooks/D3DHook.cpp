#include "core/hooks/D3DHook.hpp"
#include "core/GlobalVars.hpp"
#include "core/runtime/FeatureDispatcher.hpp"
#include "ui/menu/Menu.hpp"
#include "ui/menu/SkeetUI.hpp"
#include "config/Config.hpp"
#include "features/visuals/esp/ESPDraw.hpp"
#include "third_party/MinHook/minhook.h"
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <mutex>

// ImGui's Win32 header leaves this declaration commented so applications can
// provide their own WndProc. The bundled backend exports the implementation.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

// ============================================================
//  D3DHook.cpp — DirectX 9 hook + ImGui frame management
//
//  Responsibilities:
//    - Bootstrap EndScene/Present hooks on a private temporary device
//    - Install real device hooks (Present/PresentEx, Reset/ResetEx,
//      SwapChainPresent) on first real EndScene call
//    - Manage ImGui context lifecycle (init, invalidate, recreate)
//    - Drive FeatureDispatcher::OnUpdate / FeatureDispatcher::OnRender each frame
// ============================================================

static const GUID kIID_IDirect3DDevice9Ex =
    { 0xB18B10CE, 0x2649, 0x405A, { 0x87, 0x0F, 0x95, 0xF7, 0x77, 0xD4, 0x31, 0x3A } }; // IID_IDirect3DDevice9Ex (d3d9.h)

static constexpr int VT_RESET      = 16;
static constexpr int VT_PRESENT    = 17;
static constexpr int VT_ENDSCENE   = 42;
static constexpr int VT_PRESENTEX  = 121;
static constexpr int VT_RESETEX    = 132; // 128 is CheckDeviceState(HWND), NOT ResetEx
static constexpr int VT_SC_PRESENT = 3;

typedef HRESULT( __stdcall* EndScene_t        )( IDirect3DDevice9* );
typedef HRESULT( __stdcall* Present_t         )( IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA* );
typedef HRESULT( __stdcall* Reset_t           )( IDirect3DDevice9*, D3DPRESENT_PARAMETERS* );
typedef HRESULT( __stdcall* ResetEx_t         )( IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX* );
typedef HRESULT( __stdcall* SwapChainPresent_t)( IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD );
typedef HRESULT( __stdcall* PresentEx_t )( IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD );

// Keep one trampoline per actual method address. Bootstrap, real devices and
// replacement swapchains need not share implementations on every driver.
struct MethodHook { void* target; void* original; };
static std::vector<MethodHook> g_methodHooks;
static std::mutex g_hookMutex;
static IDirect3DDevice9* g_activeDevice = nullptr; // borrowed; never retains Reset resources
static bool g_frameRendered = false;
static thread_local bool g_insideOverlay = false;
static thread_local unsigned g_presentDepth = 0;

static bool    g_bImGuiInited       = false;
static bool    g_bRealHooksReady    = false;
HWND           g_hGameWnd           = nullptr;
static ImVec2  g_backbufferSize{};
static INT64   g_perfFreq           = 0;
static INT64   g_lastFrameTime      = 0;
static int     g_skipRenderFrames   = 0;
static void*   g_swapChainPresentFn = nullptr;
static WNDPROC g_originalWndProc     = nullptr;
static bool    g_wndProcInstalled     = false;
static bool    g_virtualCursorActive  = false;
static ImVec2  g_virtualCursorPos{};
static HCURSOR g_savedOsCursor = nullptr;
static bool g_osCursorHidden = false;
static BOOL g_savedDeviceCursor = FALSE;
static bool g_deviceCursorHidden = false;
static int  g_gammaDetected   = -1; // -1 unknown, 0 none, 1 conversion
static int  g_gammaProbeTries = 0;

// Forward declarations
static HRESULT __stdcall Hooked_EndScene( IDirect3DDevice9* pDevice );
static HRESULT __stdcall Hooked_Present( IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA* );
static HRESULT __stdcall Hooked_Reset( IDirect3DDevice9*, D3DPRESENT_PARAMETERS* );
static HRESULT __stdcall Hooked_ResetEx( IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX* );

static HRESULT __stdcall Hooked_SwapChainPresent( IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD );
static HRESULT __stdcall Hooked_PresentEx( IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD );
static LRESULT CALLBACK Hooked_WndProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam );
static void RestoreMenuCursor();
static bool GetBackbufferSize( IDirect3DDevice9* pDevice, UINT& outW, UINT& outH );

// ----------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------
static bool CreateHookIfNeeded( void* target, void* detour )
{
    if (!target) return false;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    for (const auto& hook : g_methodHooks)
        if (hook.target==target) return true;
    void* original=nullptr;
    if (MH_CreateHook(target,detour,&original)!=MH_OK || !original) return false;
    // Publish before enabling: another render thread may enter immediately.
    g_methodHooks.push_back({target,original});
    const auto status=MH_EnableHook(target);
    if (status==MH_OK || status==MH_ERROR_ENABLED) return true;
    g_methodHooks.pop_back();
    MH_RemoveHook(target);
    return false;
}

template<class Fn> static Fn OriginalMethod(void* object,int slot)
{
    const auto target=(*reinterpret_cast<void***>(object))[slot];
    std::lock_guard<std::mutex> lock(g_hookMutex);
    for (const auto& hook : g_methodHooks)
        if (hook.target==target) return reinterpret_cast<Fn>(hook.original);
    return nullptr;
}

static bool IsGameWindow(HWND window)
{
    DWORD process=0;
    return window && IsWindow(window) &&
        GetWindowThreadProcessId(window,&process) && process==GetCurrentProcessId();
}

static bool IsActiveDevice(IDirect3DDevice9* device)
{
    if (g_activeDevice) return device==g_activeDevice;
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(device->GetCreationParameters(&params)) || !IsGameWindow(params.hFocusWindow))
        return false;
    // Ignore helper/hidden devices, including our bootstrap window.
    if (!IsWindowVisible(params.hFocusWindow)) return false;
    g_activeDevice=device;
    return true;
}

static bool GetBackbufferSize( IDirect3DDevice9* pDevice, UINT& outW, UINT& outH )
{
    IDirect3DSurface9* pBB = nullptr;
    if ( FAILED( pDevice->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB ) ) )
        return false;
    D3DSURFACE_DESC desc{};
    const HRESULT described=pBB->GetDesc( &desc );
    pBB->Release();
    if (FAILED(described)) return false;
    outW = desc.Width;
    outH = desc.Height;
    return outW > 0 && outH > 0;
}

static void InvalidateImGuiDevice()
{
    // Menu device objects (texture + fonts) are managed by different lifetime
    // rules than ImGui's backend objects. They must be disposed on every reset
    // regardless of whether ImGui itself finished initializing, otherwise a
    // leaked D3DPOOL_DEFAULT texture can make ResetEx fail / fall back to a
    // mismatched windowed mode (this is what shows up as a stretched frame).
    Menu::InvalidateDeviceObjects();
    if ( !g_bImGuiInited )
    {
        g_swapChainPresentFn = nullptr;
        return;
    }
    RestoreMenuCursor();
    ImGui_ImplDX9_InvalidateDeviceObjects();
    g_backbufferSize     = {};
    g_skipRenderFrames   = 3;
    g_gammaDetected      = -1;
    g_gammaProbeTries    = 0;
    g_swapChainPresentFn = nullptr;
    g_frameRendered = false;
}

static void RecreateImGuiDevice()
{
    if ( !g_bImGuiInited ) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

// ImGui owns the only visible menu pointer. Do not loop Win32 ShowCursor:
// its display counter is shared with the game and drifts on repeated toggles.
static void RestoreMenuCursor()
{
    if (g_osCursorHidden && GetForegroundWindow()==g_hGameWnd && !GetCursor())
        SetCursor(g_savedOsCursor);
    g_osCursorHidden=false;
    g_savedOsCursor=nullptr;
    if (g_deviceCursorHidden && g_pD3Device)
        g_pD3Device->ShowCursor(g_savedDeviceCursor);
    g_deviceCursorHidden=false;
}

static void SyncMenuCursor(IDirect3DDevice9* device)
{
    const bool visible=!g_bUnloading && Menu::bOpen && g_hGameWnd &&
        GetForegroundWindow()==g_hGameWnd && !IsIconic(g_hGameWnd);
    ImGui::GetIO().MouseDrawCursor=visible;
    if (!visible) { RestoreMenuCursor(); return; }

    const HCURSOR previous=SetCursor(nullptr);
    if (!g_osCursorHidden || previous) g_savedOsCursor=previous;
    g_osCursorHidden=true;
    if (device)
    {
        const BOOL previousDevice=device->ShowCursor(FALSE);
        if (!g_deviceCursorHidden) g_savedDeviceCursor=previousDevice;
        g_deviceCursorHidden=true;
    }
}

static bool FeedMenuRawMouse( const LPARAM rawInputHandle )
{
    UINT byteCount = 0;
    if ( GetRawInputData( reinterpret_cast<HRAWINPUT>( rawInputHandle ), RID_INPUT,
        nullptr, &byteCount, sizeof( RAWINPUTHEADER ) ) != 0 ||
        byteCount < sizeof( RAWINPUT ) )
        return false;

    std::vector<unsigned char> buffer( byteCount );
    if ( GetRawInputData( reinterpret_cast<HRAWINPUT>( rawInputHandle ), RID_INPUT,
        buffer.data(), &byteCount, sizeof( RAWINPUTHEADER ) ) != byteCount )
        return false;

    const auto* raw = reinterpret_cast<const RAWINPUT*>( buffer.data() );
    if ( raw->header.dwType != RIM_TYPEMOUSE )
        return false;

    ImGuiIO& io = ImGui::GetIO();
    if ( !g_virtualCursorActive )
    {
        g_virtualCursorActive = true;
        g_virtualCursorPos = ImVec2( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
    }

    if ( raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE )
    {
        // Absolute raw devices report 0..65535 coordinates.
        g_virtualCursorPos.x = std::clamp(
            static_cast<float>( raw->data.mouse.lLastX ) * io.DisplaySize.x / 65535.0f,
            0.0f, io.DisplaySize.x );
        g_virtualCursorPos.y = std::clamp(
            static_cast<float>( raw->data.mouse.lLastY ) * io.DisplaySize.y / 65535.0f,
            0.0f, io.DisplaySize.y );
    }
    else
    {
        g_virtualCursorPos.x = std::clamp( g_virtualCursorPos.x +
            static_cast<float>( raw->data.mouse.lLastX ), 0.0f, io.DisplaySize.x );
        g_virtualCursorPos.y = std::clamp( g_virtualCursorPos.y +
            static_cast<float>( raw->data.mouse.lLastY ), 0.0f, io.DisplaySize.y );
    }
    io.AddMouseSourceEvent( ImGuiMouseSource_Mouse );
    io.AddMousePosEvent( g_virtualCursorPos.x, g_virtualCursorPos.y );
    // Keep the rendered software cursor in sync immediately. The event queue
    // is still submitted for the next NewFrame, while this direct value avoids
    // one visible frame of latency in the overlay.
    io.MousePos = g_virtualCursorPos;
    return true;
}

static void UpdateVirtualMenuCursor()
{
    if ( !Menu::bOpen )
    {
        g_virtualCursorActive = false;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if ( !g_virtualCursorActive )
    {
        g_virtualCursorActive = true;
        g_virtualCursorPos = ImVec2( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
    }
    io.AddMousePosEvent( g_virtualCursorPos.x, g_virtualCursorPos.y );
    io.MousePos = g_virtualCursorPos;
}

static bool IsMenuInputMessage( const UINT message )
{
    switch ( message )
    {
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_INPUT:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_IME_CHAR:
    case WM_IME_COMPOSITION:
    case WM_SETCURSOR:
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK Hooked_WndProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    // Alt-Tab can change the desktop mode before Source has completed its
    // D3D9 reset. Drop the cached size and pause overlay drawing for a few
    // presents so ImGui cannot submit vertices using the pre-switch target.
    // The original WndProc still receives these messages below.
    if ( message == WM_ACTIVATEAPP || message == WM_DISPLAYCHANGE ||
         message == WM_DPICHANGED )
    {
        g_backbufferSize = {};
        g_skipRenderFrames = wParam ? 4 : 8;
        g_frameRendered = false;
    }
    if (!g_bUnloading && g_bImGuiInited &&
        (message==WM_SETFOCUS || message==WM_KILLFOCUS))
    {
        ImGui_ImplWin32_WndProcHandler(hWnd,message,wParam,lParam);
        if (message==WM_KILLFOCUS)
        {
            g_virtualCursorActive=false;
            ImGui::GetIO().MouseDrawCursor=false;
            RestoreMenuCursor();
        }
    }
    // The backend records wheel, text/IME, button, keyboard and cursor input.
    // Most of its paths deliberately return 0 after consuming a message, so
    // input-class messages must be blocked explicitly while the menu is open.
    if ( !g_bUnloading && g_bImGuiInited && Menu::bOpen )
    {
        // Reassert on cursor/mouse messages: the game can replace its OS
        // cursor between rendered frames. Non-client resize borders retain
        // the normal system pointer.
        if (message==WM_SETCURSOR)
        {
            if (LOWORD(lParam)==HTCLIENT)
            {
                SyncMenuCursor(g_pD3Device);
                return TRUE;
            }
            return g_originalWndProc
                ? CallWindowProcW(g_originalWndProc,hWnd,message,wParam,lParam)
                : DefWindowProcW(hWnd,message,wParam,lParam);
        }
        if (message==WM_MOUSEMOVE || message==WM_INPUT)
            SyncMenuCursor(g_pD3Device);
        // CSSO keeps the physical pointer centered for relative mouse input.
        // Translate its raw deltas into a virtual ImGui pointer before the
        // game sees the message, rather than relying on GetCursorPos.
        if ( message == WM_INPUT )
        {
            FeedMenuRawMouse( lParam );
            return 1L;
        }

        // The game synthesizes WM_MOUSEMOVE at its locked screen centre. Do
        // not pass that synthetic position to ImGui or it would overwrite the
        // virtual pointer advanced by raw deltas above.
        if ( message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE ||
            message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE )
            return 1L;

        const LRESULT backendResult = ImGui_ImplWin32_WndProcHandler(
            hWnd, message, wParam, lParam );
        if ( backendResult || IsMenuInputMessage( message ) )
            return 1L;
    }

    return g_originalWndProc
        ? CallWindowProcW( g_originalWndProc, hWnd, message, wParam, lParam )
        : DefWindowProcW( hWnd, message, wParam, lParam );
}

static bool InstallGameWndProc()
{
    if ( g_wndProcInstalled )
        return true;
    if ( !g_hGameWnd || !IsWindow( g_hGameWnd ) )
        return false;

    SetLastError( ERROR_SUCCESS );
    const auto previous = reinterpret_cast<WNDPROC>( SetWindowLongPtrW(
        g_hGameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>( Hooked_WndProc ) ) );
    if ( !previous && GetLastError() != ERROR_SUCCESS )
        return false;

    g_originalWndProc = previous;
    g_wndProcInstalled = true;
    return true;
}

static void RestoreGameWndProc()
{
    if ( g_wndProcInstalled && g_hGameWnd && IsWindow( g_hGameWnd ) && g_originalWndProc )
        SetWindowLongPtrW( g_hGameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>( g_originalWndProc ) );

    g_originalWndProc = nullptr;
    g_wndProcInstalled = false;
    g_virtualCursorActive = false;
    g_virtualCursorPos = {};
}

static void UpdateImGuiDeltaTime()
{
    if ( g_perfFreq == 0 )
    {
        ::QueryPerformanceFrequency( reinterpret_cast<LARGE_INTEGER*>( &g_perfFreq ) );
        ::QueryPerformanceCounter ( reinterpret_cast<LARGE_INTEGER*>( &g_lastFrameTime ) );
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        return;
    }
    INT64 now = 0;
    ::QueryPerformanceCounter( reinterpret_cast<LARGE_INTEGER*>( &now ) );
    ImGui::GetIO().DeltaTime = static_cast<float>(
        static_cast<double>( now - g_lastFrameTime ) / static_cast<double>( g_perfFreq ) );
    g_lastFrameTime = now;
}

static void SyncDisplaySizeFromBackbuffer( IDirect3DDevice9* pDevice )
{
    UINT bbW = 0, bbH = 0;
    if ( !GetBackbufferSize( pDevice, bbW, bbH ) )
    {
        if ( g_backbufferSize.x > 0.f && g_backbufferSize.y > 0.f )
            ImGui::GetIO().DisplaySize = g_backbufferSize;
        return;
    }
    g_backbufferSize = ImVec2( (float)bbW, (float)bbH );
    ImGui::GetIO().DisplaySize = g_backbufferSize;
}

static bool KeyDown( int vk )
{
    return ( ::GetAsyncKeyState( vk ) & 0x8000 ) != 0;
}

static void PollImGuiKeys()
{
    ImGuiIO& io = ImGui::GetIO();

    io.AddKeyEvent( ImGuiMod_Ctrl,  KeyDown( VK_CONTROL ) );
    io.AddKeyEvent( ImGuiMod_Shift, KeyDown( VK_SHIFT   ) );
    io.AddKeyEvent( ImGuiMod_Alt,   KeyDown( VK_MENU    ) );

    for ( int i = 0; i < 10; ++i )
        io.AddKeyEvent( static_cast<ImGuiKey>( ImGuiKey_0 + i ), KeyDown( '0' + i ) );
    for ( int i = 0; i < 26; ++i )
        io.AddKeyEvent( static_cast<ImGuiKey>( ImGuiKey_A + i ), KeyDown( 'A' + i ) );

    static constexpr struct { WORD vk; ImGuiKey key; } specials[] =
    {
        { VK_SPACE,      ImGuiKey_Space },
        { VK_RETURN,     ImGuiKey_Enter },
        { VK_ESCAPE,     ImGuiKey_Escape },
        { VK_BACK,       ImGuiKey_Backspace },
        { VK_TAB,        ImGuiKey_Tab },
        { VK_LEFT,       ImGuiKey_LeftArrow },
        { VK_RIGHT,      ImGuiKey_RightArrow },
        { VK_UP,         ImGuiKey_UpArrow },
        { VK_DOWN,       ImGuiKey_DownArrow },
        { VK_PRIOR,      ImGuiKey_PageUp },
        { VK_NEXT,       ImGuiKey_PageDown },
        { VK_HOME,       ImGuiKey_Home },
        { VK_END,        ImGuiKey_End },
        { VK_INSERT,     ImGuiKey_Insert },
        { VK_DELETE,     ImGuiKey_Delete },
        { VK_CAPITAL,    ImGuiKey_CapsLock },
        { VK_OEM_COMMA,  ImGuiKey_Comma },
        { VK_OEM_MINUS,  ImGuiKey_Minus },
        { VK_OEM_PERIOD, ImGuiKey_Period },
        { VK_OEM_2,      ImGuiKey_Slash },
        { VK_OEM_1,      ImGuiKey_Semicolon },
        { VK_OEM_PLUS,   ImGuiKey_Equal },
        { VK_OEM_4,      ImGuiKey_LeftBracket },
        { VK_OEM_5,      ImGuiKey_Backslash },
        { VK_OEM_6,      ImGuiKey_RightBracket },
        { VK_OEM_7,      ImGuiKey_Apostrophe },
        { VK_OEM_3,      ImGuiKey_GraveAccent },
    };
    for ( const auto& s : specials )
        io.AddKeyEvent( s.key, KeyDown( s.vk ) );
}

// Synthesises character input for InputText widgets from raw key polls
// (no WndProc available). Edge-triggered per key; layout-aware through
// ToUnicode so shift/capslock/numpad produce correct characters.
static void PollImGuiTextInput()
{
    ImGuiIO& io = ImGui::GetIO();
    static bool sPrevDown[256] = {};

    BYTE ks[256] = {};
    const bool haveLayout = ::GetKeyboardState( ks ) != FALSE;
    if ( haveLayout )
    {
        ks[VK_SHIFT]   = KeyDown( VK_SHIFT )   ? 0x80 : 0;
        ks[VK_CONTROL] = KeyDown( VK_CONTROL ) ? 0x80 : 0;
        ks[VK_MENU]    = KeyDown( VK_MENU    ) ? 0x80 : 0;
    }

    for ( int vk = 0; vk < 256; ++vk )
    {
        const bool candidate = ( vk >= 'A' && vk <= 'Z' ) ||
                               ( vk >= '0' && vk <= '9' ) ||
                               ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 ) ||
                               vk == VK_SPACE ||
                               ( vk >= VK_OEM_1 && vk <= VK_OEM_3 ) ||
                               ( vk >= VK_OEM_4 && vk <= VK_OEM_8 );
        if ( !candidate )
            continue;

        const bool down = KeyDown( vk );
        const bool prev = sPrevDown[ vk ];
        sPrevDown[ vk ] = down;

        if ( !down || prev || !io.WantTextInput || !haveLayout )
            continue;

        wchar_t wc[8] = {};
        const UINT scan = ::MapVirtualKeyW( vk, MAPVK_VK_TO_VSC );
        const int n = ::ToUnicode( vk, scan, ks, wc, 8, 0 );
        for ( int c = 0; c < n && c < 8; ++c )
            if ( wc[c] >= 0x20 )
                io.AddInputCharacterUTF16( wc[c] );
    }
}

static void PollImGuiInput()
{
    ImGuiIO& io = ImGui::GetIO();
    POINT pt{};
    if ( ::GetCursorPos( &pt ) && ::ScreenToClient( g_hGameWnd, &pt ) )
        io.AddMousePosEvent( (float)pt.x, (float)pt.y );
    else
        io.AddMousePosEvent( -FLT_MAX, -FLT_MAX );
    io.AddMouseButtonEvent( 0, KeyDown( VK_LBUTTON ) );
    io.AddMouseButtonEvent( 1, KeyDown( VK_RBUTTON ) );
    io.AddMouseButtonEvent( 2, KeyDown( VK_MBUTTON ) );

    PollImGuiKeys();
    PollImGuiTextInput();
}

// ----------------------------------------------------------------
//  ImGui initialisation (called once on first real device)
// ----------------------------------------------------------------
static bool InitImGui( IDirect3DDevice9* pDevice )
{
    g_pD3Device = pDevice;

    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(pDevice->GetCreationParameters(&params)) || !IsGameWindow(params.hFocusWindow))
        return false;
    g_hGameWnd = params.hFocusWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false;
    fontCfg.OversampleH = 1;
    fontCfg.OversampleV = 1;
    fontCfg.PixelSnapH  = true;

    // Default font contains only a limited Latin range. Player List names are
    // UTF-8 and need actual Cyrillic glyphs after engine/ACP normalization.
    // Tahoma is present on supported Windows clients and also matches Surface
    // ESP's established Russian-friendly choice; fallback preserves startup.
    const ImWchar* cyrillic = io.Fonts->GetGlyphRangesCyrillic();
    if ( !io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\tahoma.ttf", 15.f,
        &fontCfg, cyrillic ) )
    {
        io.Fonts->AddFontDefault( &fontCfg );
    }

    // Menu chrome fonts (window title / group captions). Segoe UI ships with
    // every supported Windows client; on failure the pointers stay null and
    // Menu::Render falls back to the default font at the same size.
    Menu::f_title  = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeuib.ttf", 20.f, &fontCfg, cyrillic );
    Menu::f_header = io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf",  15.f, &fontCfg, cyrillic );

    // Skeet menu fonts: verdana 12, verdana bold 12, embedded icon font 37.
    ui::LoadFonts();

    // Keep one software pointer at the virtual raw-input position. OS and
    // D3D hardware pointer visibility is managed explicitly below; the Win32
    // backend must not independently replace/restore it.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    ImGui::GetMainViewport()->PlatformHandle = g_hGameWnd;

    if ( !ImGui_ImplWin32_Init( g_hGameWnd ) )
    {
        ImGui::DestroyContext();
        return false;
    }
    // Subclassing can fail under another overlay/window wrapper. Keep the
    // renderer alive and use the existing polling input fallback in that case.
    InstallGameWndProc();
    if (!ImGui_ImplDX9_Init(pDevice))
    {
        RestoreGameWndProc();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    g_config.refresh();
    g_bImGuiInited = true;

    return true;
}

// ----------------------------------------------------------------
//  Per-frame render guard
// ----------------------------------------------------------------
static bool CanRenderOverlay( IDirect3DDevice9* pDevice )
{
    if ( !g_bImGuiInited || g_skipRenderFrames > 0 )
    {
        if ( g_skipRenderFrames > 0 ) --g_skipRenderFrames;
        return false;
    }
    if ( pDevice->TestCooperativeLevel() != D3D_OK ) return false;
    if ( !g_hGameWnd || ::IsIconic( g_hGameWnd ) )   return false;
    UINT bbW = 0, bbH = 0;
    return GetBackbufferSize( pDevice, bbW, bbH );
}

// ----------------------------------------------------------------
//  Menu gamma handling
//  Some setups convert everything written to the backbuffer
//  linear->sRGB even with D3DRS_SRGBWRITEENABLE forced off, which
//  washes the dark skeet palette out (23 -> 85, 34 -> 102). Auto mode
//  draws one probe pixel and reads it back; if 64 comes back as ~137
//  the conversion is active and ImGui colors are pre-linearized.
// ----------------------------------------------------------------
static unsigned char g_srgbToLinear[256];
static bool g_srgbLutReady = false;

static void BuildSrgbLut()
{
    if ( g_srgbLutReady ) return;
    for ( int i = 0; i < 256; ++i )
    {
        const float c = i / 255.f;
        const float l = c <= 0.04045f ? c / 12.92f : powf( ( c + 0.055f ) / 1.055f, 2.4f );
        g_srgbToLinear[i] = (unsigned char)( l * 255.f + 0.5f );
    }
    g_srgbLutReady = true;
}

static void LinearizeDrawData( ImDrawData* dd )
{
    BuildSrgbLut();
    for ( ImDrawList* list : dd->CmdLists )
        for ( ImDrawVert& v : list->VtxBuffer )
        {
            unsigned char* c = reinterpret_cast<unsigned char*>( &v.col );
            // bytes 0..2 = color (either RGB or BGR order), byte 3 = alpha
            c[0] = g_srgbToLinear[ c[0] ];
            c[1] = g_srgbToLinear[ c[1] ];
            c[2] = g_srgbToLinear[ c[2] ];
        }
}

// Reads backbuffer pixel (0,0). Returns -1 on failure.
static int ReadBackbufferPixel0( IDirect3DDevice9* pDevice, IDirect3DSurface9* pBB )
{
    D3DSURFACE_DESC desc{};
    if ( FAILED( pBB->GetDesc( &desc ) ) ) return -1;
    if ( desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8 ) return -1;
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* sys = nullptr;
    int result = -1;
    if ( SUCCEEDED( pDevice->CreateRenderTarget( 1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr ) ) &&
         SUCCEEDED( pDevice->CreateOffscreenPlainSurface( 1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr ) ) )
    {
        const RECT src{ 0, 0, 1, 1 };
        if ( SUCCEEDED( pDevice->StretchRect( pBB, &src, rt, nullptr, D3DTEXF_NONE ) ) &&
             SUCCEEDED( pDevice->GetRenderTargetData( rt, sys ) ) )
        {
            D3DLOCKED_RECT lr{};
            if ( SUCCEEDED( sys->LockRect( &lr, nullptr, D3DLOCK_READONLY ) ) )
            {
                result = static_cast<const unsigned char*>( lr.pBits )[1]; // green
                sys->UnlockRect();
            }
        }
    }
    if ( sys ) sys->Release();
    if ( rt )  rt->Release();
    return result;
}

// ----------------------------------------------------------------
//  Main per-frame render call
// ----------------------------------------------------------------
static bool RenderImGuiOverlay( IDirect3DDevice9* pDevice )
{
    if ( !CanRenderOverlay( pDevice ) ) return false;

    D3DVIEWPORT9 vpBefore{};
    const bool hadViewport=SUCCEEDED(pDevice->GetViewport(&vpBefore));
    RECT scissorBefore{};
    const bool hadScissor = SUCCEEDED(pDevice->GetScissorRect(&scissorBefore));
    D3DMATRIX matWorld{}, matView{}, matProj{};
    const bool hadWorld=SUCCEEDED(pDevice->GetTransform(D3DTS_WORLD,&matWorld));
    const bool hadView=SUCCEEDED(pDevice->GetTransform(D3DTS_VIEW,&matView));
    const bool hadProj=SUCCEEDED(pDevice->GetTransform(D3DTS_PROJECTION,&matProj));

    IDirect3DStateBlock9* pStateBlock = nullptr;
    if (FAILED(pDevice->CreateStateBlock(D3DSBT_ALL,&pStateBlock)) || !pStateBlock) return false;
    if (FAILED(pStateBlock->Capture())) { pStateBlock->Release(); return false; }

    ImGui_ImplDX9_NewFrame();
    UpdateImGuiDeltaTime();
    SyncDisplaySizeFromBackbuffer( pDevice );
    // World-to-screen code and the ImGui backend must use the same physical
    // frame size. After Alt-Tab Source can leave a stale viewport (for
    // example, the old fullscreen size) while the backbuffer is already at
    // the new size. That mismatch makes X coordinates progressively drift,
    // which looks like the right side of the scene is stretched.
    UINT frameW = 0, frameH = 0;
    if ( GetBackbufferSize( pDevice, frameW, frameH ) )
    {
        D3DVIEWPORT9 frameViewport{};
        frameViewport.X = 0;
        frameViewport.Y = 0;
        frameViewport.Width = frameW;
        frameViewport.Height = frameH;
        frameViewport.MinZ = 0.0f;
        frameViewport.MaxZ = 1.0f;
        pDevice->SetViewport( &frameViewport );
    }
    SyncMenuCursor(pDevice);

    // Do not call ImGui_ImplWin32_NewFrame here: it polls GetCursorPos every
    // frame, while CSSO pins that physical pointer to the screen center. The
    // backend's WndProc still supplies buttons, wheel, keyboard and text; raw
    // WM_INPUT deltas below are the authoritative menu pointer position.
    if (g_wndProcInstalled) UpdateVirtualMenuCursor();
    else if (GetForegroundWindow()==g_hGameWnd) PollImGuiInput();
    ImGui::NewFrame();

    FeatureDispatcher::OnUpdate();
    FeatureDispatcher::OnRender();

    Menu::Render( pDevice );

    // Menu::Render toggles bOpen on the Insert edge. Run the sync once more so
    // the initial centre position is available in the same rendered frame.
    if (g_wndProcInstalled) UpdateVirtualMenuCursor();

    // Apply the Insert toggle in this same frame, including cursor restore.
    SyncMenuCursor(pDevice);

    const int gammaMode = g_config.settings.misc.menu_gamma; // 0 auto, 1 normal, 2 fix
    const bool probing  = gammaMode == 0 && g_gammaDetected < 0;
    if ( probing )
        ImGui::GetForegroundDrawList()->AddRectFilled( ImVec2( 0, 0 ), ImVec2( 1, 1 ), IM_COL32( 64, 64, 64, 255 ) );

    ImGui::EndFrame();
    ImGui::Render();

    const bool linearize = gammaMode == 2 || ( gammaMode == 0 && g_gammaDetected == 1 );
    if ( linearize )
        LinearizeDrawData( ImGui::GetDrawData() );

    // ImGui colors are authored in display space: no sRGB write, no
    // sRGB texture decode (unless compensating for a forced conversion).
    // The state block below restores the game's values.
    pDevice->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
    pDevice->SetRenderState( D3DRS_COLORWRITEENABLE, 0xFFFFFFFF );
    pDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, linearize ? TRUE : FALSE );
    ImGui_ImplDX9_RenderDrawData( ImGui::GetDrawData() );

    pStateBlock->Apply();
    pStateBlock->Release();
    // Restore the transforms and viewport explicitly as a second line of
    // defence. The state block restores the game's render target and render
    // states; unlike the old code, we never swap targets mid-frame, so the
    // game's viewport cannot become paired with a different-sized surface.
    if (hadWorld) pDevice->SetTransform(D3DTS_WORLD,&matWorld);
    if (hadView) pDevice->SetTransform(D3DTS_VIEW,&matView);
    if (hadProj) pDevice->SetTransform(D3DTS_PROJECTION,&matProj);
    if ( hadViewport )
    {
        D3DVIEWPORT9 restoredViewport = vpBefore;
        // Do not re-install a pre-Alt-Tab viewport whose dimensions no
        // longer match the active backbuffer. The next Source frame will set
        // its own viewport; until then, a full-target viewport prevents the
        // overlay and world projection from using different X scales.
        if ( frameW && frameH &&
             ( restoredViewport.Width != frameW || restoredViewport.Height != frameH ) )
        {
            restoredViewport.X = 0;
            restoredViewport.Y = 0;
            restoredViewport.Width = frameW;
            restoredViewport.Height = frameH;
            restoredViewport.MinZ = 0.0f;
            restoredViewport.MaxZ = 1.0f;
        }
        pDevice->SetViewport( &restoredViewport );
    }
    if (hadScissor) pDevice->SetScissorRect(&scissorBefore);
    return true;
}

// ----------------------------------------------------------------
//  Real device hook installation
// ----------------------------------------------------------------
static void InstallSwapChainPresentHook( IDirect3DDevice9* pDevice )
{
    IDirect3DSwapChain9* pSwapChain = nullptr;
    if ( FAILED( pDevice->GetSwapChain( 0, &pSwapChain ) ) || !pSwapChain ) return;
    void** scVt = *reinterpret_cast<void***>( pSwapChain );
    void*  presentFn = scVt[VT_SC_PRESENT];
    pSwapChain->Release();
    if ( presentFn == g_swapChainPresentFn ) return;
    if (CreateHookIfNeeded(presentFn,reinterpret_cast<void*>(&Hooked_SwapChainPresent)))
        g_swapChainPresentFn=presentFn;
}

static void InstallHooksFromRealDevice( IDirect3DDevice9* pDevice )
{
    if (g_bRealHooksReady) return;
    void** vt=*reinterpret_cast<void***>(pDevice);
    const bool reset=CreateHookIfNeeded(vt[VT_RESET],reinterpret_cast<void*>(&Hooked_Reset));
    const bool present=CreateHookIfNeeded(vt[VT_PRESENT],reinterpret_cast<void*>(&Hooked_Present));
    const bool scene=CreateHookIfNeeded(vt[VT_ENDSCENE],reinterpret_cast<void*>(&Hooked_EndScene));
    IDirect3DDevice9Ex* ex=nullptr;
    bool exReady=true;
    if (SUCCEEDED(pDevice->QueryInterface(kIID_IDirect3DDevice9Ex,reinterpret_cast<void**>(&ex))) && ex)
    {
        void** exVt=*reinterpret_cast<void***>(ex);
        const bool exReset=CreateHookIfNeeded(exVt[VT_RESETEX],reinterpret_cast<void*>(&Hooked_ResetEx));
        const bool exPresent=CreateHookIfNeeded(exVt[VT_PRESENTEX],reinterpret_cast<void*>(&Hooked_PresentEx));
        exReady=exReset && exPresent;
        ex->Release();
    }
    InstallSwapChainPresentHook(pDevice);
    g_bRealHooksReady=reset && present && scene && exReady;
}

static bool PrepareGameDevice(IDirect3DDevice9* device)
{
    if (g_bUnloading || !IsActiveDevice(device)) return false;
    InstallHooksFromRealDevice(device);
    if (!g_bImGuiInited && !InitImGui(device)) return false;
    return true;
}

static HRESULT __stdcall Hooked_EndScene(IDirect3DDevice9* device)
{
    const auto original=OriginalMethod<EndScene_t>(device,VT_ENDSCENE);
    if (!original) return D3DERR_INVALIDCALL;
    if (!g_insideOverlay && PrepareGameDevice(device) && !g_frameRendered)
    {
        // EndScene has NOT run yet: draw inside the game's active scene.
        // Do not pin a return address: Steam/driver overlays may reach us first.
        g_insideOverlay=true;
        g_frameRendered=RenderImGuiOverlay(device);
        g_insideOverlay=false;
    }
    return original(device);
}

// Present is a fallback for wrappers that bypass the hooked EndScene path.
// Never draw outside a successful BeginScene/EndScene pair.
static void RenderBeforePresent(IDirect3DDevice9* device)
{
    if (g_insideOverlay || !PrepareGameDevice(device) || g_frameRendered) return;
    const auto end=OriginalMethod<EndScene_t>(device,VT_ENDSCENE);
    if (!end || !CanRenderOverlay(device)) return;
    g_insideOverlay=true;
    if (SUCCEEDED(device->BeginScene()))
    {
        g_frameRendered=RenderImGuiOverlay(device);
        end(device);
    }
    g_insideOverlay=false;
}

struct PresentScope
{
    bool outer;
    explicit PresentScope(IDirect3DDevice9* device):outer(g_presentDepth++==0)
    { if (outer) RenderBeforePresent(device); }
    ~PresentScope() { --g_presentDepth; if (outer) g_frameRendered=false; }
};

static HRESULT __stdcall Hooked_Present(IDirect3DDevice9* device,
    const RECT* src,const RECT* dst,HWND window,const RGNDATA* dirty)
{
    const auto original=OriginalMethod<Present_t>(device,VT_PRESENT);
    if (!original) return D3DERR_INVALIDCALL;
    if (g_bUnloading || (g_activeDevice && device!=g_activeDevice))
        return original(device,src,dst,window,dirty);
    PresentScope frame(device);
    return original(device,src,dst,window,dirty);
}

static HRESULT __stdcall Hooked_PresentEx(IDirect3DDevice9Ex* device,
    const RECT* src,const RECT* dst,HWND window,const RGNDATA* dirty,DWORD flags)
{
    const auto original=OriginalMethod<PresentEx_t>(device,VT_PRESENTEX);
    if (!original) return D3DERR_INVALIDCALL;
    if (g_bUnloading || (g_activeDevice && device!=g_activeDevice))
        return original(device,src,dst,window,dirty,flags);
    PresentScope frame(device);
    return original(device,src,dst,window,dirty,flags);
}

static HRESULT __stdcall Hooked_SwapChainPresent(IDirect3DSwapChain9* chain,
    const RECT* src,const RECT* dst,HWND window,const RGNDATA* dirty,DWORD flags)
{
    const auto original=OriginalMethod<SwapChainPresent_t>(chain,VT_SC_PRESENT);
    if (!original) return D3DERR_INVALIDCALL;
    IDirect3DDevice9* device=nullptr;
    if (!g_bUnloading && SUCCEEDED(chain->GetDevice(&device)) && device)
    {
        IDirect3DSwapChain9* primary=nullptr;
        const bool mainChain=SUCCEEDED(device->GetSwapChain(0,&primary)) && primary==chain;
        if (primary) primary->Release();
        if (mainChain && (!g_activeDevice || device==g_activeDevice))
        {
            PresentScope frame(device);
            const HRESULT result=original(chain,src,dst,window,dirty,flags);
            device->Release();
            return result;
        }
        device->Release();
    }
    return original(chain,src,dst,window,dirty,flags);
}

static HRESULT __stdcall Hooked_Reset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* params)
{
    const auto original=OriginalMethod<Reset_t>(device,VT_RESET);
    if (!original) return D3DERR_INVALIDCALL;
    if (g_bUnloading || device!=g_activeDevice) return original(device,params);

    // Release all DEFAULT-pool resources before the single Reset call. Do not
    // retry with a saved structure: Source rewrites presentation parameters
    // during the focus transition, and replaying the old fullscreen mode can
    // leave the game stretched after Alt-Tab.
    InvalidateImGuiDevice();
    HRESULT result=original(device,params);
    if (SUCCEEDED(result)) { RecreateImGuiDevice(); InstallSwapChainPresentHook(device); }
    return result;
}

static HRESULT __stdcall Hooked_ResetEx(IDirect3DDevice9Ex* device,
    D3DPRESENT_PARAMETERS* params,D3DDISPLAYMODEEX* mode)
{
    const auto original=OriginalMethod<ResetEx_t>(device,VT_RESETEX);
    if (!original) return D3DERR_INVALIDCALL;
    if (g_bUnloading || device!=g_activeDevice) return original(device,params,mode);

    // Keep ResetEx in the same one-pass lifecycle. The game owns and may
    // update both structures while focus is changing.
    InvalidateImGuiDevice();
    HRESULT result=original(device,params,mode);
    if (SUCCEEDED(result)) { RecreateImGuiDevice(); InstallSwapChainPresentHook(device); }
    return result;
}

// ----------------------------------------------------------------
//  Public API
// ----------------------------------------------------------------
bool D3D::AttachHooks(HWND)
{
    IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return false;
    // A private hidden window avoids the desktop/foreign focus window and
    // HAL matches the real render path better than a NULLREF-only bootstrap.
    HWND helper=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if (!helper) { d3d->Release(); return false; }
    D3DPRESENT_PARAMETERS params{};
    params.Windowed=TRUE;
    params.SwapEffect=D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow=helper;
    params.BackBufferWidth=1;
    params.BackBufferHeight=1;
    IDirect3DDevice9* temp=nullptr;
    HRESULT result=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,helper,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,&params,&temp);
    if (FAILED(result) || !temp)
        result=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_NULLREF,helper,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,&params,&temp);
    bool scene=false,present=false;
    if (SUCCEEDED(result) && temp)
    {
        void** vt=*reinterpret_cast<void***>(temp);
        scene=CreateHookIfNeeded(vt[VT_ENDSCENE],reinterpret_cast<void*>(&Hooked_EndScene));
        present=CreateHookIfNeeded(vt[VT_PRESENT],reinterpret_cast<void*>(&Hooked_Present));
        temp->Release();
    }
    DestroyWindow(helper);
    d3d->Release();
    return scene || present;
}

HWND D3D::GetGameWindow()
{
    return g_hGameWnd;
}

void D3D::Shutdown()
{
    RestoreMenuCursor();
    RestoreGameWndProc();
    Menu::InvalidateDeviceObjects();
    if ( g_bImGuiInited )
        ImGui_ImplWin32_Shutdown();
    g_bImGuiInited = false;
    g_activeDevice = nullptr;
    g_frameRendered = false;
}
