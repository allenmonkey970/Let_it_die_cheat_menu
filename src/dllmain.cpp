// LET IT DIE overlay mod menu: dinput8.dll proxy + D3D9 EndScene hook + ImGui.
// Delivered as a proxy dinput8.dll placed next to BrgGame-Steam.exe; data lives in .\lid_mod\.
// Renderer confirmed D3D9 (BrgEngine.ini AllowD3D11=False). Toggle menu with INSERT.
//
// Threading: window messages arrive on the game's window thread, EndScene runs on UE3's render thread.
// ImGui is not thread-safe, so hkWndProc only queues messages (with modifier state captured on that thread,
// where GetKeyState is valid); they are translated into ImGui events inside EndScene. Window style changes
// (borderless) run on our own worker thread: SetWindowPos from the render thread can deadlock against the
// game thread's WM_SIZE handler, which waits for the render thread.

#include <windows.h>
#include <windowsx.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "items.h"
#include "savequeue.h"
#include "scanner.h"
#include "settings.h"
#include "ue3.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "dxguid.lib")

// ---- forward decl from imgui_impl_win32 ----
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// =====================================================================
//  dinput8.dll proxy: forward the 5 exports to the real system DLL.
// =====================================================================
namespace proxy {
    HMODULE real = nullptr;
    using fnDirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    using fnDllCanUnloadNow   = HRESULT(WINAPI*)();
    using fnDllGetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    using fnDllRegisterServer = HRESULT(WINAPI*)();
    using fnDllUnregisterServer = HRESULT(WINAPI*)();

    fnDirectInput8Create   pDirectInput8Create   = nullptr;
    fnDllCanUnloadNow      pDllCanUnloadNow      = nullptr;
    fnDllGetClassObject    pDllGetClassObject    = nullptr;
    fnDllRegisterServer    pDllRegisterServer    = nullptr;
    fnDllUnregisterServer  pDllUnregisterServer  = nullptr;

    void load() {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\dinput8.dll");
        real = LoadLibraryA(path);
        if (!real) return;
        pDirectInput8Create   = (fnDirectInput8Create)  GetProcAddress(real, "DirectInput8Create");
        pDllCanUnloadNow      = (fnDllCanUnloadNow)      GetProcAddress(real, "DllCanUnloadNow");
        pDllGetClassObject    = (fnDllGetClassObject)    GetProcAddress(real, "DllGetClassObject");
        pDllRegisterServer    = (fnDllRegisterServer)    GetProcAddress(real, "DllRegisterServer");
        pDllUnregisterServer  = (fnDllUnregisterServer)  GetProcAddress(real, "DllUnregisterServer");
    }
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    if (!proxy::pDirectInput8Create) return E_FAIL;
    return proxy::pDirectInput8Create(h, v, r, o, u);
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return proxy::pDllCanUnloadNow ? proxy::pDllCanUnloadNow() : S_FALSE;
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    return proxy::pDllGetClassObject ? proxy::pDllGetClassObject(c, r, o) : E_FAIL;
}
extern "C" HRESULT WINAPI DllRegisterServer() {
    return proxy::pDllRegisterServer ? proxy::pDllRegisterServer() : E_FAIL;
}
extern "C" HRESULT WINAPI DllUnregisterServer() {
    return proxy::pDllUnregisterServer ? proxy::pDllUnregisterServer() : E_FAIL;
}

// =====================================================================
//  D3D9 EndScene hook + ImGui overlay.
// =====================================================================
using EndScene_t = HRESULT(APIENTRY*)(IDirect3DDevice9*);
using Reset_t    = HRESULT(APIENTRY*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static EndScene_t oEndScene = nullptr;
static Reset_t    oReset    = nullptr;

static bool  g_imguiInit = false;
static volatile bool g_menuOpen = false;     // written on the render thread, read by hkWndProc
static HWND  g_hWnd      = nullptr;
static WNDPROC g_oWndProc = nullptr;
static bool  g_prevInsert = false;
static ImFont* g_fontTitle = nullptr;

struct QueuedMsg { UINT msg; WPARAM w; LPARAM l; int mods; };   // mods: 1 Ctrl, 2 Shift, 4 Alt
static std::mutex g_msgMu;
static std::vector<QueuedMsg> g_msgs;

// ---- borderless window toggle (applied only on the window-ops thread, see Worker) ----
static bool     g_borderlessApplied = false;  // what is actually applied to the window
static LONG_PTR g_origStyle         = 0;
static RECT     g_origRect          = {};
static HANDLE   g_winEvent          = nullptr;
static std::atomic<int> g_wantBorderless{ -1 };

static void RequestBorderless(bool on) {
    g_wantBorderless = on ? 1 : 0;
    if (g_winEvent) SetEvent(g_winEvent);
}

static void ApplyBorderless(bool on) {
    if (!g_hWnd || on == g_borderlessApplied) return;
    if (on) {
        g_origStyle = GetWindowLongPtr(g_hWnd, GWL_STYLE);
        GetWindowRect(g_hWnd, &g_origRect);
        HMONITOR mon = MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(mon, &mi);
        SetWindowLongPtr(g_hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtr(g_hWnd, GWL_STYLE, g_origStyle);
        SetWindowPos(g_hWnd, HWND_TOP, g_origRect.left, g_origRect.top,
                     g_origRect.right - g_origRect.left,
                     g_origRect.bottom - g_origRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_borderlessApplied = on;
}

// Messages FeedInput understands; only these are queued (bounded), so a hidden/minimised game that stops
// calling EndScene cannot grow the queue or replay stale clicks later.
static bool IsMenuInput(UINT msg) {
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
    }
    return false;
}

static LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_menuOpen) {
        if (IsMenuInput(msg)) {
            int mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 0) |
                       ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
            std::lock_guard<std::mutex> lk(g_msgMu);
            if (g_msgs.size() < 256) g_msgs.push_back({ msg, wParam, lParam, mods });
        }
        // Hide presses/moves from the game while the menu is open. Releases (KEYUP/BUTTONUP) still reach it, so a
        // key held when INSERT was pressed is not left stuck down after the menu closes.
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
            case WM_KEYDOWN:     case WM_SYSKEYDOWN:  case WM_CHAR: case WM_SYSCHAR:
                return 0;
            case WM_INPUT:                         // raw mouse/keyboard: let Windows clean up, hide it from the game
                return DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
    return CallWindowProc(g_oWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------
//  DirectInput: the game reads mouse/keyboard through dinput8 (which we proxy), not window messages, so
//  swallowing WM_* alone still lets the camera move. While the menu is open the game gets no presses/motion.
//  IDirectInputDevice8 vtable: GetDeviceState = 9, GetDeviceData = 10, GetDeviceInfo = 15 (A and W hooked).
//  Devices are classified by GetDeviceInfo (cached per device pointer), not by buffer size, so a gamepad or a
//  custom data format is never blanked (zeroed joystick axes would read as full deflection).
// ---------------------------------------------------------------------
using GDS_t = HRESULT(WINAPI*)(void*, DWORD, LPVOID);
using GDD_t = HRESULT(WINAPI*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GDS_t oGDS[2] = {};
static GDD_t oGDD[2] = {};
static std::string g_diStatus = "DirectInput: not hooked";
static std::atomic<long> g_cntDiState{ 0 }, g_cntDiData{ 0 }, g_cntDiBlocked{ 0 }, g_cntCursorGet{ 0 }, g_cntCursorSet{ 0 };

enum DevKind { DEV_OTHER = 0, DEV_MOUSE = 1, DEV_KEYBOARD = 2 };
static std::mutex g_devMu;
static struct { void* dev; int kind; } g_devKinds[32];
static int g_devCount = 0;

template <int I> static int KindOf(void* dev) {
    {
        std::lock_guard<std::mutex> lk(g_devMu);
        for (int i = 0; i < g_devCount; ++i) if (g_devKinds[i].dev == dev) return g_devKinds[i].kind;
    }
    DWORD type = 0;
    using GDI_t = HRESULT(WINAPI*)(void*, void*);
    GDI_t gdi = (GDI_t)(*(void***)dev)[15];
    if (I) { DIDEVICEINSTANCEW di{}; di.dwSize = sizeof di; if (SUCCEEDED(gdi(dev, &di))) type = di.dwDevType; }
    else   { DIDEVICEINSTANCEA di{}; di.dwSize = sizeof di; if (SUCCEEDED(gdi(dev, &di))) type = di.dwDevType; }
    int kind = GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_MOUSE ? DEV_MOUSE
             : GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_KEYBOARD ? DEV_KEYBOARD : DEV_OTHER;
    std::lock_guard<std::mutex> lk(g_devMu);
    if (g_devCount < 32) g_devKinds[g_devCount++] = { dev, kind };
    return kind;
}

template <int I> static HRESULT WINAPI hkGDS(void* self, DWORD cb, LPVOID data) {
    HRESULT hr = oGDS[I](self, cb, data);
    ++g_cntDiState;
    if (g_menuOpen && SUCCEEDED(hr) && data && KindOf<I>(self) != DEV_OTHER) { memset(data, 0, cb); ++g_cntDiBlocked; }  // = all released
    return hr;
}

// Buffered input: drop presses and motion, keep releases (high bit of dwData clear on a key/button).
template <int I> static HRESULT WINAPI hkGDD(void* self, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) {
    HRESULT hr = oGDD[I](self, cbObj, rg, inOut, flags);    // still drains the buffer
    ++g_cntDiData;
    if (!g_menuOpen || FAILED(hr) || !inOut) return hr;
    if (*inOut) ++g_cntDiBlocked;
    int kind = KindOf<I>(self);
    if (kind == DEV_OTHER) return hr;
    if (!rg || cbObj < sizeof(DIDEVICEOBJECTDATA)) { *inOut = 0; return hr; }
    DWORD kept = 0;
    for (DWORD i = 0; i < *inOut; ++i) {
        auto* e = (LPDIDEVICEOBJECTDATA)((BYTE*)rg + (size_t)i * cbObj);
        bool release = !(e->dwData & 0x80) && (kind == DEV_KEYBOARD || e->dwOfs >= DIMOFS_BUTTON0);
        if (release) { if (kept != i) memcpy((BYTE*)rg + (size_t)kept * cbObj, e, cbObj); ++kept; }
    }
    *inOut = kept;
    return hr;
}

// ---------------------------------------------------------------------
//  Cursor-based mouse look: UE3 viewports can turn the camera from GetCursorPos deltas and re-centre with
//  SetCursorPos, bypassing DirectInput. While the menu is open the game's SetCursorPos is ignored and its
//  GetCursorPos returns the last position the game itself set (zero delta). The overlay's own reads (ImGui
//  backend, inside EndScene) are marked with tOverlayCursor and see the real cursor.
// ---------------------------------------------------------------------
using GCP_t = BOOL(WINAPI*)(LPPOINT);
using SCP_t = BOOL(WINAPI*)(int, int);
static GCP_t oGetCursorPos = nullptr;
static SCP_t oSetCursorPos = nullptr;
static thread_local bool tOverlayCursor = false;
static std::atomic<long> g_gameCursorX{ LONG_MIN }, g_gameCursorY{ LONG_MIN };   // last game SetCursorPos
static std::atomic<long> g_openCursorX{ 0 }, g_openCursorY{ 0 };                 // cursor when the menu opened

static BOOL WINAPI hkGetCursorPos(LPPOINT p) {
    BOOL r = oGetCursorPos(p);
    if (g_menuOpen && !tOverlayCursor && r && p) {
        ++g_cntCursorGet;
        long gx = g_gameCursorX, gy = g_gameCursorY;
        if (gx != LONG_MIN) { p->x = gx; p->y = gy; } else { p->x = g_openCursorX; p->y = g_openCursorY; }
    }
    return r;
}
static BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (g_menuOpen && !tOverlayCursor) { ++g_cntCursorSet; return TRUE; }   // don't yank the overlay cursor
    g_gameCursorX = x; g_gameCursorY = y;
    return oSetCursorPos(x, y);
}

static void HookCursor() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* g = u32 ? (void*)GetProcAddress(u32, "GetCursorPos") : nullptr;
    void* st = u32 ? (void*)GetProcAddress(u32, "SetCursorPos") : nullptr;
    if (g && MH_CreateHook(g, &hkGetCursorPos, (void**)&oGetCursorPos) == MH_OK) MH_EnableHook(g);
    if (st && MH_CreateHook(st, &hkSetCursorPos, (void**)&oSetCursorPos) == MH_OK) MH_EnableHook(st);
}

static std::string InputDiag() {
    char b[256];
    _snprintf_s(b, sizeof b, _TRUNCATE, "Input reads seen: DI state %ld, DI buffered %ld, blocked %ld | cursor get blocked %ld, set blocked %ld",
                g_cntDiState.load(), g_cntDiData.load(), g_cntDiBlocked.load(), g_cntCursorGet.load(), g_cntCursorSet.load());
    return b;
}

std::string Overlay_InputDiag() { return g_diStatus + "\n" + InputDiag() + (g_menuOpen ? "\nmenu=open" : "\nmenu=closed"); }

static void HookDirectInput() {
    if (!proxy::pDirectInput8Create) { g_diStatus = "DirectInput: real dinput8 not loaded"; return; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return;
    void* hooked[4] = {};
    int nHooked = 0;
    const IID* iids[2] = { &IID_IDirectInput8A, &IID_IDirectInput8W };
    for (int w = 0; w < 2; ++w) {
        IDirectInput8W* di = nullptr;
        if (FAILED(proxy::pDirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[w], (LPVOID*)&di, nullptr)) || !di)
            continue;
        IDirectInputDevice8W* dev = nullptr;        // A/W CreateDevice share the ABI; any device gives the vtable
        if (FAILED(di->CreateDevice(GUID_SysMouse, &dev, nullptr)) || !dev) {
            dev = nullptr;
            if (FAILED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) dev = nullptr;
        }
        if (dev) {
            void** vt = *(void***)dev;
            void* gds = vt[9];
            void* gdd = vt[10];
            bool dupS = false, dupD = false;
            for (int i = 0; i < nHooked; ++i) { dupS |= hooked[i] == gds; dupD |= hooked[i] == gdd; }
            if (!dupS && MH_CreateHook(gds, w ? (void*)&hkGDS<1> : (void*)&hkGDS<0>, (void**)&oGDS[w]) == MH_OK &&
                MH_EnableHook(gds) == MH_OK) hooked[nHooked++] = gds;
            if (!dupD && MH_CreateHook(gdd, w ? (void*)&hkGDD<1> : (void*)&hkGDD<0>, (void**)&oGDD[w]) == MH_OK &&
                MH_EnableHook(gdd) == MH_OK) hooked[nHooked++] = gdd;
            dev->Release();
        }
        di->Release();
    }
    g_diStatus = nHooked ? "DirectInput: " + std::to_string(nHooked) + " functions hooked (mouse/keyboard blocked while menu is open)"
                         : "DirectInput: NOT hooked - game input is not blocked while the menu is open";
}

static ImGuiKey VkToKey(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (int)(vk - 'A'));
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (int)(vk - '0'));
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;           case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;     case VK_BACK: return ImGuiKey_Backspace;
        case VK_DELETE: return ImGuiKey_Delete;     case VK_SPACE: return ImGuiKey_Space;
        case VK_LEFT: return ImGuiKey_LeftArrow;    case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_HOME: return ImGuiKey_Home;         case VK_END: return ImGuiKey_End;
        case VK_PRIOR: return ImGuiKey_PageUp;      case VK_NEXT: return ImGuiKey_PageDown;
        default: return ImGuiKey_None;
    }
}

// Translate one queued window message into ImGui input events (render thread). Mouse position is polled by
// ImGui_ImplWin32_NewFrame (GetCursorPos works from any thread), so WM_MOUSEMOVE is not needed here.
static void FeedInput(const QueuedMsg& m) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (m.mods & 1) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (m.mods & 2) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (m.mods & 4) != 0);
    switch (m.msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP:   io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP:   io.AddMouseButtonEvent(1, false); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: io.AddMouseButtonEvent(2, true); break;
        case WM_MBUTTONUP:   io.AddMouseButtonEvent(2, false); break;
        case WM_MOUSEWHEEL:  io.AddMouseWheelEvent(0.0f, (float)GET_WHEEL_DELTA_WPARAM(m.w) / WHEEL_DELTA); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            ImGuiKey k = VkToKey(m.w);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN);
            break;
        }
        case WM_CHAR:
            if (m.w > 0 && m.w < 0x10000) io.AddInputCharacterUTF16((unsigned short)m.w);
            break;
    }
}

// Opening the menu: drop any button/key state ImGui may still think is held from before it was hidden.
static void ResetInput() {
    ImGuiIO& io = ImGui::GetIO();
    for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, false);
    io.ClearInputKeys();
}

// ---------------------------------------------------------------------
//  Look & feel
// ---------------------------------------------------------------------
static const ImVec4 kAccent   = ImVec4(0.80f, 0.14f, 0.16f, 1.00f);   // blood red
static const ImVec4 kYellow   = ImVec4(0.98f, 0.82f, 0.22f, 1.00f);   // LET IT DIE yellow
static const ImVec4 kGood     = ImVec4(0.45f, 0.90f, 0.50f, 1.00f);
static const ImVec4 kBad      = ImVec4(1.00f, 0.42f, 0.38f, 1.00f);
static const ImVec4 kMuted    = ImVec4(0.62f, 0.62f, 0.66f, 1.00f);

static void SetupStyle() {
    ImGuiIO& io = ImGui::GetIO();
    // Latin-1 + General Punctuation (item names use curly apostrophes/quotes, dashes, ellipsis).
    static const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0x2122, 0x2122, 0 };
    if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, kRanges);
        const char* bold = GetFileAttributesA("C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES
                         ? "C:\\Windows\\Fonts\\segoeuib.ttf" : "C:\\Windows\\Fonts\\segoeui.ttf";
        g_fontTitle = io.Fonts->AddFontFromFileTTF(bold, 21.0f, nullptr, kRanges);
    }
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8; s.ChildRounding = 6; s.FrameRounding = 5; s.PopupRounding = 6;
    s.GrabRounding = 4; s.TabRounding = 5; s.ScrollbarRounding = 6;
    s.WindowPadding = ImVec2(14, 12); s.FramePadding = ImVec2(9, 5); s.ItemSpacing = ImVec2(8, 7);
    s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.TabBarBorderSize = 2;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]           = ImVec4(0.07f, 0.07f, 0.08f, 0.97f);
    c[ImGuiCol_ChildBg]            = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg]            = ImVec4(0.09f, 0.09f, 0.10f, 0.98f);
    c[ImGuiCol_Border]             = ImVec4(0.30f, 0.10f, 0.11f, 0.80f);
    c[ImGuiCol_Text]               = ImVec4(0.93f, 0.92f, 0.90f, 1.00f);
    c[ImGuiCol_TextDisabled]       = kMuted;
    c[ImGuiCol_TitleBg]            = ImVec4(0.12f, 0.04f, 0.05f, 1.00f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.45f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.12f, 0.04f, 0.05f, 1.00f);
    c[ImGuiCol_FrameBg]            = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.30f, 0.15f, 0.16f, 1.00f);
    c[ImGuiCol_CheckMark]          = kYellow;
    c[ImGuiCol_SliderGrab]         = kAccent;
    c[ImGuiCol_Button]             = ImVec4(0.55f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(0.75f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_ButtonActive]       = ImVec4(0.90f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_Header]             = ImVec4(0.40f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_HeaderHovered]      = ImVec4(0.58f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_HeaderActive]       = ImVec4(0.70f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_Separator]          = ImVec4(0.30f, 0.12f, 0.13f, 1.00f);
    c[ImGuiCol_Tab]                = ImVec4(0.16f, 0.08f, 0.09f, 1.00f);
    c[ImGuiCol_TabHovered]         = ImVec4(0.65f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_TabSelected]        = ImVec4(0.55f, 0.10f, 0.12f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]= kYellow;
    c[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.12f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(0.45f, 0.14f, 0.15f, 1.00f);
}

static void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void Heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

static bool CheatToggle(const char* label, const char* desc, ue3::Cheat c) {
    bool on = ue3::GetCheat(c);
    bool changed = ImGui::Checkbox(label, &on);
    if (changed) settings::SetCheat(c, on);
    ImGui::Indent(30); Hint(desc); ImGui::Unindent(30);
    return changed;
}

static ImTextureID TexId() { return (ImTextureID)(uintptr_t)items::Atlas(); }

static void DrawIcon(const items::Item& it, float size) {
    float u0, v0, u1, v1;
    if (it.icon >= 0 && items::Atlas()) {
        items::IconUV(it.icon, u0, v0, u1, v1);
        ImGui::Image(TexId(), ImVec2(size, size), ImVec2(u0, v0), ImVec2(u1, v1));
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }
}

static void DrawPending() {
    auto pending = savequeue::Pending();
    Heading(pending.empty() ? "Waiting for you to quit: nothing" : "Waiting for you to quit");
    if (pending.empty()) { Hint("Queued items and fighter upgrades show up here."); return; }
    for (size_t i = 0; i < pending.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("Cancel")) savequeue::Remove(i);
        ImGui::SameLine();
        const char* what = pending[i].op.rfind("locker", 0) == 0 ? "Coin Locker: " : "";
        ImGui::Text("%s%s", what, pending[i].label.c_str());
        ImGui::PopID();
    }
    Hint("Applied automatically the moment you quit the game; ready on your next launch.");
}

// ---------------------------------------------------------------------
//  Tabs
// ---------------------------------------------------------------------
static void TabCheats() {
    Heading("Survival");
    CheatToggle("Infinite Durability", "Weapons and armor never wear down. Repairing still works.", ue3::DURABILITY);
    CheatToggle("Infinite Ammo", "Guns never use up bullets. Reloading and swapping work normally.", ue3::AMMO);
    CheatToggle("Infinite Stamina", "Attacking, guarding and dodging cost no stamina.", ue3::STAMINA);
    if (ue3::Faulted())
        ImGui::TextColored(kBad, "Paused: a game call failed. Save, then restart the game to re-enable.");
    else if (ue3::Unavailable())
        ImGui::TextColored(kBad, "Unavailable: some game functions were not found (Settings > Diagnostics).");
    else if (!ue3::Ready())
        ImGui::TextColored(kMuted, "Arming... (finishes once you are in the game world)");
    else
        ImGui::TextColored(kMuted, "%s", ue3::CheatStats().c_str());

    Heading("Currency");
    if (ImGui::Button("+100,000 Kill Coins", ImVec2(200, 0))) ue3::QueueAddMoney(100000);
    ImGui::SameLine();
    if (ImGui::Button("+100,000 SPLithium", ImVec2(200, 0))) ue3::QueueAddSpirit(100000);
    Hint("The game caps each at 500,000; anything above is cut back.");
}

static void TabItems() {
    const auto& all = items::All();
    static char search[64] = "";
    static int slotFilter = 0;
    static const char* kSlots[]    = { "Everything", "Weapons", "Head", "Body", "Legs", "R&D materials" };
    static const char* kSlotKeys[] = { "",           "Arm",     "Head", "Body", "Legs", "Material" };
    static int selected = -1;
    static std::vector<int> view;
    static std::string lastSearch = "\x01";
    static int lastSlot = -1;
    static size_t lastCount = 0;
    static bool showUnlisted = false, lastUnlisted = false;

    if (all.empty()) {
        ImGui::TextColored(kBad, "Item catalog not available");
        Hint(items::Error().c_str());
        Hint("Expected lid_mod\\items.tsv and lid_mod\\icons_atlas.png next to dinput8.dll.");
        return;
    }
    if (!items::Error().empty()) ImGui::TextColored(kBad, "%s", items::Error().c_str());

    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##search", "Search by name or part ID", search, sizeof search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##slot", &slotFilter, kSlots, IM_ARRAYSIZE(kSlots));
    ImGui::SameLine();
    ImGui::Checkbox("Show variants without a game icon", &showUnlisted);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("These parts have no icon in the game's files, so the game may not show them\n"
                          "in your Coin Locker (Demon Gun 1 is one of them).");

    if (lastSearch != search || lastSlot != slotFilter || lastCount != all.size() || lastUnlisted != showUnlisted) {
        lastSearch = search; lastSlot = slotFilter; lastCount = all.size(); lastUnlisted = showUnlisted;
        std::string q(search);
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        view.clear();
        for (int i = 0; i < (int)all.size(); ++i) {
            const auto& it = all[i];
            if (!showUnlisted && !it.gameIcon) continue;
            if (slotFilter && it.slot != kSlotKeys[slotFilter]) continue;
            if (!q.empty()) {
                std::string hay = it.name + " " + it.ptid;
                std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
                if (hay.find(q) == std::string::npos) continue;
            }
            view.push_back(i);
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%d items", (int)view.size());

    const float detailW = 250.0f;
    ImGui::BeginChild("grid", ImVec2(-detailW - 8, 0), ImGuiChildFlags_Borders);
    const float cell = 56.0f, pad = 3.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (std::max)(1, (int)((avail + 6) / (cell + pad * 2 + 6)));
    int rows = ((int)view.size() + cols - 1) / cols;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    ImGuiListClipper clip;
    clip.Begin(rows, cell + pad * 2 + 6);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            for (int c = 0; c < cols; ++c) {
                int vi = r * cols + c;
                if (vi >= (int)view.size()) break;
                int idx = view[vi];
                const auto& it = all[idx];
                if (c) ImGui::SameLine();
                ImGui::PushID(idx);
                bool sel = idx == selected;
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, kYellow);
                else     ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.18f, 1));
                bool clicked;
                if (it.icon >= 0 && items::Atlas()) {
                    float u0, v0, u1, v1; items::IconUV(it.icon, u0, v0, u1, v1);
                    clicked = ImGui::ImageButton("ic", TexId(), ImVec2(cell, cell), ImVec2(u0, v0), ImVec2(u1, v1));
                } else {
                    char tag[8]; snprintf(tag, sizeof tag, "%.3s", it.name.c_str());
                    clicked = ImGui::Button(tag, ImVec2(cell + pad * 2, cell + pad * 2));
                }
                ImGui::PopStyleColor();
                if (clicked) selected = idx;
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(it.name.c_str());
                    ImGui::TextColored(kMuted, "%s", it.ptid.c_str());
                    if (it.reqValue > 0) ImGui::TextColored(kMuted, "Requires %s %d", it.reqStat.c_str(), it.reqValue);
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("detail", ImVec2(detailW, 0), ImGuiChildFlags_Borders);
    if (selected < 0 || selected >= (int)all.size()) {
        Hint("Pick an item on the left.");
    } else {
        const auto& it = all[selected];
        DrawIcon(it, 128);
        if (g_fontTitle) ImGui::PushFont(g_fontTitle);
        ImGui::TextWrapped("%s", it.name.c_str());
        if (g_fontTitle) ImGui::PopFont();
        bool material = it.slot == "Material";
        ImGui::TextColored(kMuted, "%s  -  %s", it.ptid.c_str(),
                           it.slot == "Arm" ? "Weapon" : material ? "R&D material" : it.slot.c_str());
        if (it.reqValue > 0) ImGui::TextColored(kYellow, "Requires %s %d", it.reqStat.c_str(), it.reqValue);
        if (!it.gameIcon)
            ImGui::TextColored(kBad, "No icon in the game files - the game may not show this part in the locker.");
        ImGui::Spacing();
        static int qty = 1;
        if (material) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##qty", &qty, 1, 10, "Quantity: %d");
            Hint("Each one takes its own Coin Locker slot.");
        }
        ImGui::BeginDisabled(!savequeue::Ready());
        if (ImGui::Button("Send to Coin Locker", ImVec2(-1, 34))) {
            int n = material ? qty : 1;
            for (int i = 0; i < n; ++i) savequeue::Add({ material ? "locker_item" : "locker", it.ptid, it.name });
        }
        ImGui::EndDisabled();
        Hint("Permanent. Waiting in your Coin Locker the next time you launch.");
        if (!material) {
        ImGui::Spacing();
        static int hand = 0;
        if (it.slot == "Arm") { ImGui::RadioButton("Right hand", &hand, 0); ImGui::SameLine(); ImGui::RadioButton("Left hand", &hand, 1); }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.23f, 1));
        if (ImGui::Button("Try it on now", ImVec2(-1, 0))) {
            std::wstring site = it.slot == "Arm" ? (hand ? L"Arm_L" : L"Arm_R") : std::wstring(it.slot.begin(), it.slot.end());
            ue3::QueuePartEquip(site, std::wstring(it.ptid.begin(), it.ptid.end()));
        }
        ImGui::PopStyleColor();
        Hint("Temporary preview: gone after you swap it out or reload.");
        }
    }
    DrawPending();
    ImGui::EndChild();
}

static void TabFighter() {
    Heading("Max stats");
    Hint("Raises HP, STR, DEX, VIT, STM and LUK to the highest your fighter's type allows, and the level to "
         "match. Done to the save after you quit (live XP crashes the stat screen, so this is the safe way).");
    ImGui::BeginDisabled(!savequeue::Ready());
    if (ImGui::Button("Max current fighter", ImVec2(220, 32)))
        savequeue::Add({ "fighter_max", "current", "Max stats: current fighter" });
    ImGui::SameLine();
    if (ImGui::Button("Max every fighter", ImVec2(220, 32)))
        savequeue::Add({ "fighter_max", "all", "Max stats: every fighter" });
    ImGui::EndDisabled();
    DrawPending();
}

static void TabSettings() {
    Heading("Window");
    bool borderless = settings::Get().borderless;
    if (ImGui::Checkbox("Borderless window", &borderless)) {
        settings::SetBorderless(borderless);
        RequestBorderless(borderless);
    }
    Hint("All toggles are remembered between launches (lid_mod\\settings.ini).");

    Heading("Save helper");
    ImGui::TextWrapped("%s", savequeue::HelperStatus().c_str());

    if (ImGui::CollapsingHeader("Diagnostics")) {
        ImGui::Text("FPS %.0f  |  Direct3D 9", ImGui::GetIO().Framerate);
        ImGui::TextWrapped("Bridge: %s", ue3::Status().c_str());
        ImGui::TextWrapped("%s", ue3::Diag().c_str());
        ImGui::TextWrapped("%s", g_diStatus.c_str());
        ImGui::TextWrapped("%s", InputDiag().c_str());
        ImGui::TextWrapped("Scanner: %s (%d frozen)", Scanner_LastStatus(), Scanner_FreezeCount());
    }
}

static void DrawMenu() {
    static bool showResults = true;
    ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 420), ImVec2(4000, 4000));
    ImGui::Begin("LET IT DIE  //  MOD MENU###lidmod", nullptr, ImGuiWindowFlags_NoCollapse);

    if (ue3::Faulted())
        ImGui::TextColored(kBad, "A game call failed and cheats were paused for safety. Save, then restart the game.");

    static std::vector<std::string> results;                 // captured once the queue has loaded
    static bool haveResults = false;
    if (!haveResults && savequeue::Ready()) { results = savequeue::LastResults(); haveResults = true; }
    if (showResults && results.size() > 1) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.16f, 0.11f, 1));
        ImGui::BeginChild("results", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::TextColored(kGood, "Applied after your last session (%s):", results[0].c_str());
        for (size_t i = 1; i < results.size(); ++i) {
            const std::string& line = results[i];
            bool ok = line.rfind("OK", 0) == 0;
            const char* text = line.size() > 5 && (ok || line.rfind("FAIL", 0) == 0) ? line.c_str() + 5 : line.c_str();
            ImGui::TextColored(ok ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kBad, "  %s", text);
        }
        if (ImGui::SmallButton("Dismiss")) showResults = false;
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Cheats"))   { TabCheats();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Items"))    { TabItems();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Fighter"))  { TabFighter();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings")) { TabSettings(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();

    // Status strip, bottom-left of the screen, so feedback is visible from any tab.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 12, vp->Pos.y + vp->Size.y - 12), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(kMuted, "INSERT hides the menu   |   %s", ue3::Status().c_str());
    ImGui::End();
}

static HRESULT APIENTRY hkEndScene(IDirect3DDevice9* dev) {
    // Toggle on INSERT edge.
    bool insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    bool justOpened = false;
    if (insert && !g_prevInsert) { g_menuOpen = !g_menuOpen; justOpened = g_menuOpen; }
    g_prevInsert = insert;

    if (!g_imguiInit) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        g_hWnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();

        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        // The game hides the OS cursor during play, and SetCursor from the render thread has no effect on the
        // game's window anyway: ImGui draws its own cursor while the menu is open and leaves the OS one alone.
        ImGui::GetIO().MouseDrawCursor = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SetupStyle();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX9_Init(dev);
        g_oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        g_imguiInit = true;
        if (settings::Get().borderless) RequestBorderless(true);
    }

    std::vector<QueuedMsg> msgs;
    { std::lock_guard<std::mutex> lk(g_msgMu); msgs.swap(g_msgs); }

    if (g_menuOpen) {
        if (justOpened) {
            ResetInput();
            POINT pt{}; tOverlayCursor = true; GetCursorPos(&pt); tOverlayCursor = false;
            g_openCursorX = pt.x; g_openCursorY = pt.y;
        }
        for (auto& m : msgs) FeedInput(m);
        items::EnsureTexture(dev);
        ImGui_ImplDX9_NewFrame();
        tOverlayCursor = true;            // ImGui's own GetCursorPos must see the real cursor
        ImGui_ImplWin32_NewFrame();
        tOverlayCursor = false;
        ImGui::NewFrame();
        DrawMenu();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    return oEndScene(dev);
}

static HRESULT APIENTRY hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_imguiInit) ImGui_ImplDX9_InvalidateDeviceObjects();
    items::OnDeviceLost();
    HRESULT hr = oReset(dev, pp);
    if (g_imguiInit) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

// Build a throwaway device once to read the shared D3D9 vtable, then MinHook the real funcs. Retry-safe: the
// vtable is cached, and a hook created by an earlier attempt counts as done.
static bool InstallHooks() {
    static void* pReset = nullptr;
    static void* pEndScene = nullptr;
    if (!pEndScene) {
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) return false;
        HWND hwnd = GetDesktopWindow();
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd;
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        if (FAILED(hr) || !dev) { d3d->Release(); return false; }
        void** vtbl = *reinterpret_cast<void***>(dev);
        pReset    = vtbl[16];
        pEndScene = vtbl[42];
        dev->Release();
        d3d->Release();
    }
    { MH_STATUS st = MH_Initialize(); if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false; }
    auto ok = [](MH_STATUS st) { return st == MH_OK || st == MH_ERROR_ALREADY_CREATED; };
    if (!ok(MH_CreateHook(pEndScene, &hkEndScene, reinterpret_cast<void**>(&oEndScene)))) return false;
    if (!ok(MH_CreateHook(pReset,    &hkReset,    reinterpret_cast<void**>(&oReset))))    return false;
    MH_STATUS e1 = MH_EnableHook(pEndScene), e2 = MH_EnableHook(pReset);
    return (e1 == MH_OK || e1 == MH_ERROR_ENABLED) && (e2 == MH_OK || e2 == MH_ERROR_ENABLED);
}

static DWORD WINAPI Worker(LPVOID) {
    settings::Load();
    Scanner_Start();               // after Load, so an early RPC cannot save defaults over the user's ini
    items::LoadFiles();            // TSV + PNG decode here, texture upload later on the render thread
    savequeue::StartHelper();      // applies queued save edits after the game exits
    // d3d9.dll is statically imported, so it is already loaded; small wait lets the game create its device.
    for (int i = 0; i < 60; ++i) {
        if (InstallHooks()) break;
        Sleep(500);
    }
    HookDirectInput();
    HookCursor();
    if (ue3::Install()) settings::ApplyCheatsFromSettings();

    // Window-ops loop: style/position changes block until the game thread handles the messages, which is
    // fine here (nothing waits on this thread) but could deadlock on the render thread.
    for (;;) {
        WaitForSingleObject(g_winEvent, INFINITE);
        int want = g_wantBorderless.exchange(-1);
        if (want >= 0 && g_hWnd) ApplyBorderless(want != 0);
    }
}

// =====================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        proxy::load();
        settings::ModDir();        // initialise the shared path before any of our threads exist
        g_winEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
