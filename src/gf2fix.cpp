// gf2fix.asi - The Godfather II (2009, EA Redwood engine) borderless + raw input + APT UI hooks
// Loaded by Ultimate ASI Loader (dinput8.dll proxy). 32-bit only.

#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <dinput.h>
#include <stdio.h>
#include <math.h>
#include "MinHook.h"

#pragma comment(lib, "user32.lib")

// ------------------------------------------------------------------ config
static struct {
    BOOL  borderless    = TRUE;
    BOOL  rawInput      = TRUE;
    float sensitivity   = 1.0f;
    float textScale     = 1.0f;   // APT in-game text/subtitle glyph multiplier (1.0 = off)
    BOOL  subHook       = FALSE;  // install D3D9 draw-call hooks for subtitle scaling
    BOOL  log           = FALSE;
    BOOL  logApt        = FALSE;
    BOOL  logFiles      = FALSE;  // log every non-system CreateFile open (boot-asset trace)
} cfg;

static HMODULE g_hSelf = nullptr;
static wchar_t g_iniPath[MAX_PATH];
static wchar_t g_logPath[MAX_PATH];
static HWND    g_hWnd   = nullptr;
static int     g_screenW = 0, g_screenH = 0;

// F9/ScrollLock-triggered APT capture window (subtitle investigation)
static volatile DWORD g_captureUntil = 0;
static volatile LONG  g_capDsN = 0, g_capSvmN = 0;

typedef BOOL (WINAPI* SetCursorPos_t)(int, int);
static SetCursorPos_t oSetCursorPos = nullptr;

// ------------------------------------------------------------------ logging
static CRITICAL_SECTION g_logCs;
static void LogInit() { InitializeCriticalSection(&g_logCs); }
static void Log(const char* fmt, ...)
{
    if (!cfg.log && !cfg.logApt && !cfg.logFiles && !g_captureUntil) return;
    EnterCriticalSection(&g_logCs);
    FILE* f = _wfopen(g_logPath, L"a");
    if (!f) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        _snwprintf(g_logPath, MAX_PATH, L"%sgf2fix.log", tmp);
        f = _wfopen(g_logPath, L"a");
    }
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

static float IniFloat(const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, g_iniPath);
    if (!buf[0]) return def;
    return (float)_wtof(buf);
}

static void WriteDefaultIniIfMissing()
{
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return; // already present
    FILE* f = _wfopen(g_iniPath, L"w");
    if (!f) return; // read-only dir: plugin still works via built-in defaults below
    fputs(
        "[Borderless]\n"
        "; Borderless windowed at desktop resolution + refresh rate. 1 = on, 0 = game default.\n"
        "Enabled=1\n"
        "\n"
        "[RawInput]\n"
        "; True 1:1 raw mouse (no acceleration/smoothing/deadzone). 1 = on, 0 = game default.\n"
        "Enabled=1\n"
        "; Mouse multiplier. 1.0 = true 1:1.\n"
        "Sensitivity=1.0\n"
        "\n"
        "[UI]\n"
        "; HUD text glyph multiplier (1.0 = off). Does NOT affect subtitles.\n"
        "TextScale=1.0\n"
        "; Experimental D3D9 draw-call hooks. LEAVE AT 0 - enabling may prevent the game from booting.\n"
        "SubtitleHook=0\n"
        "\n"
        "[Debug]\n"
        "; Write gf2fix.log next to the game exe (diagnostics).\n"
        "Log=0\n"
        "LogApt=0\n"
        "; Trace every non-system file the engine opens (boot diagnostics).\n"
        "LogFiles=0\n",
        f);
    fclose(f);
}

static void LoadConfig()
{
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(g_iniPath, MAX_PATH, L"%s\\gf2fix.ini", exeDir);
    _snwprintf(g_logPath, MAX_PATH, L"%s\\gf2fix.log", exeDir);

    WriteDefaultIniIfMissing();

    cfg.borderless  = GetPrivateProfileIntW(L"Borderless", L"Enabled", 1, g_iniPath) != 0;
    cfg.rawInput    = GetPrivateProfileIntW(L"RawInput", L"Enabled", 1, g_iniPath) != 0;
    cfg.sensitivity = IniFloat(L"RawInput", L"Sensitivity", 1.0f);
    cfg.textScale   = IniFloat(L"UI", L"TextScale", 1.0f);
    cfg.subHook     = GetPrivateProfileIntW(L"UI", L"SubtitleHook", 0, g_iniPath) != 0;
    cfg.log         = GetPrivateProfileIntW(L"Debug", L"Log", 0, g_iniPath) != 0;
    cfg.logApt      = GetPrivateProfileIntW(L"Debug", L"LogApt", 0, g_iniPath) != 0;
    cfg.logFiles    = GetPrivateProfileIntW(L"Debug", L"LogFiles", 0, g_iniPath) != 0;
}

// ------------------------------------------------------------------ vtable hook helper
static void* VTableHook(void* iface, int index, void* detour)
{
    void** vtable = *reinterpret_cast<void***>(iface);
    void* orig = vtable[index];
    DWORD old;
    if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) {
        vtable[index] = detour;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    }
    return orig;
}

// ==================================================================
// Borderless windowed @ desktop resolution + refresh
// ==================================================================
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* D3D9_CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static D3D9_CreateDevice_t oD3DCreateDevice = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* D3D9Dev_Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static D3D9Dev_Reset_t oD3DReset = nullptr;

// ------------------------------------------------------------------ subtitle vertex scaler
// Subtitles are drawn by a subsystem outside every APT text path, so we scale them
// at the vertex level: pretransformed (XYZRHW) quads sitting in the bottom band of
// the screen with a short, wide footprint = a subtitle line. Scaled about centroid.
typedef HRESULT (STDMETHODCALLTYPE* D3D9Dev_BeginScene_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE* D3D9Dev_DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* D3D9Dev_DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
static D3D9Dev_BeginScene_t oBeginScene = nullptr;
static D3D9Dev_DrawPrimitive_t oDrawPrimitive = nullptr;
static D3D9Dev_DrawPrimitiveUP_t oDrawPrimitiveUP = nullptr;

static BYTE g_vtmp[128 * 1024];
static float g_textK = 1.0f;   // live subtitle/text glyph multiplier
static volatile LONG g_frameScaled = 0;   // one subtitle batch scaled per frame
static DWORD g_subLastLog = 0;

static int VertsForPrim(D3DPRIMITIVETYPE t, UINT primCount)
{
    switch (t) {
    case D3DPT_TRIANGLELIST:  return (int)primCount * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return (int)primCount + 2;
    case D3DPT_LINELIST:      return (int)primCount * 2;
    case D3DPT_LINESTRIP:     return (int)primCount + 1;
    case D3DPT_POINTLIST:     return (int)primCount;
    }
    return 0;
}

static bool IsSubtitleBand(float xmin, float ymin, float xmax, float ymax)
{
    float W = (float)g_screenW, H = (float)g_screenH;
    if (W <= 0 || H <= 0) return false;
    float h = ymax - ymin, w = xmax - xmin;
    if (ymin < 0.70f * H || ymax > 1.00f * H) return false;  // bottom band only
    if (h <= 0.0f || h > 0.06f * H) return false;            // short (one text line)
    if (w < 0.10f * W) return false;                         // wide (a sentence)
    return true;
}

static bool ScanBounds(const void* data, int nvert, UINT stride,
                       float* xmin, float* ymin, float* xmax, float* ymax)
{
    if (nvert <= 0 || stride < 8) return false;
    const BYTE* p = (const BYTE*)data;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < nvert; i++) {
        const float* v = (const float*)(p + (size_t)i * stride);
        if (v[0] < x0) x0 = v[0]; if (v[0] > x1) x1 = v[0];
        if (v[1] < y0) y0 = v[1]; if (v[1] > y1) y1 = v[1];
    }
    *xmin = x0; *ymin = y0; *xmax = x1; *ymax = y1;
    return true;
}

static void ScaleAboutCentroid(void* data, int nvert, UINT stride, float k)
{
    BYTE* p = (BYTE*)data;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < nvert; i++) {
        const float* v = (const float*)(p + (size_t)i * stride);
        if (v[0] < x0) x0 = v[0]; if (v[0] > x1) x1 = v[0];
        if (v[1] < y0) y0 = v[1]; if (v[1] > y1) y1 = v[1];
    }
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    for (int i = 0; i < nvert; i++) {
        float* v = (float*)(p + (size_t)i * stride);
        v[0] = cx + (v[0] - cx) * k;
        v[1] = cy + (v[1] - cy) * k;
    }
}

static void LogSubscale(const char* tag, D3DPRIMITIVETYPE pt, int nv,
                        float x0, float y0, float x1, float y1, float k)
{
    DWORD now = GetTickCount();
    if (now - g_subLastLog < 1000) return;
    g_subLastLog = now;
    Log("SUBSCALE %s pt=%d nv=%d x[%.0f..%.0f] y[%.0f..%.0f] k=%.2f",
        tag, (int)pt, nv, x0, x1, y0, y1, k);
}

static HRESULT STDMETHODCALLTYPE hkBeginScene(IDirect3DDevice9* dev)
{
    g_frameScaled = 0;
    return oBeginScene(dev);
}

static HRESULT STDMETHODCALLTYPE hkDrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt,
                                                   UINT primCount, const void* data, UINT stride)
{
    float k = g_textK;
    if (k > 0.0f && k != 1.0f && !g_frameScaled && data && stride >= 8 && stride <= 256) {
        DWORD fvf = 0;
        dev->GetFVF(&fvf);
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
            int nv = VertsForPrim(pt, primCount);
            size_t need = (size_t)nv * stride;
            if (nv > 0 && need > 0 && need <= sizeof(g_vtmp)) {
                float x0, y0, x1, y1;
                if (ScanBounds(data, nv, stride, &x0, &y0, &x1, &y1) && IsSubtitleBand(x0, y0, x1, y1)) {
                    memcpy(g_vtmp, data, need);
                    ScaleAboutCentroid(g_vtmp, nv, stride, k);
                    InterlockedExchange(&g_frameScaled, 1);
                    LogSubscale("UP", pt, nv, x0, y0, x1, y1, k);
                    return oDrawPrimitiveUP(dev, pt, primCount, g_vtmp, stride);
                }
            }
        }
    }
    return oDrawPrimitiveUP(dev, pt, primCount, data, stride);
}

static HRESULT STDMETHODCALLTYPE hkDrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE pt,
                                                 UINT startVert, UINT primCount)
{
    float k = g_textK;
    if (k > 0.0f && k != 1.0f && !g_frameScaled) {
        DWORD fvf = 0;
        dev->GetFVF(&fvf);
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) {
            IDirect3DVertexBuffer9* vb = nullptr;
            UINT offset = 0, stride = 0;
            if (SUCCEEDED(dev->GetStreamSource(0, &vb, &offset, &stride)) && vb && stride >= 8 && stride <= 256) {
                int nv = VertsForPrim(pt, primCount);
                if (nv > 0) {
                    void* ptr = nullptr;
                    if (SUCCEEDED(vb->Lock(offset, (UINT)nv * stride, &ptr, D3DLOCK_READONLY)) && ptr) {
                        float x0, y0, x1, y1;
                        bool cand = ScanBounds(ptr, nv, stride, &x0, &y0, &x1, &y1) && IsSubtitleBand(x0, y0, x1, y1);
                        vb->Unlock();
                        if (cand) {
                            if (SUCCEEDED(vb->Lock(0, 0, &ptr, 0)) && ptr) {
                                ScaleAboutCentroid(ptr, nv, stride, k);
                                vb->Unlock();
                                InterlockedExchange(&g_frameScaled, 1);
                                LogSubscale("VB", pt, nv, x0, y0, x1, y1, k);
                            }
                        }
                    }
                }
                vb->Release();
            }
        }
    }
    return oDrawPrimitive(dev, pt, startVert, primCount);
}

static void ApplyBorderless(HWND hwnd)
{
    if (!cfg.borderless || !hwnd) return;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w != g_screenW || h != g_screenH) { g_screenW = w; g_screenH = h; }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR want = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) | WS_POPUP;
    if (style != want) SetWindowLongPtrW(hwnd, GWL_STYLE, want);

    RECT r; GetWindowRect(hwnd, &r);
    if (r.left != mi.rcMonitor.left || r.top != mi.rcMonitor.top ||
        (r.right - r.left) != w || (r.bottom - r.top) != h || style != want) {
        SetWindowPos(hwnd, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, w, h,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static void FixPresentParams(D3DPRESENT_PARAMETERS* pp, HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd ? hwnd : pp->hDeviceWindow, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    g_screenW = w; g_screenH = h;

    Log("FixPresentParams: %ux%u -> %dx%d windowed (desktop refresh)",
        pp->BackBufferWidth, pp->BackBufferHeight, w, h);
    pp->BackBufferWidth  = w;
    pp->BackBufferHeight = h;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

static HRESULT STDMETHODCALLTYPE hkD3DReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (cfg.borderless && pp) FixPresentParams(pp, nullptr);
    HRESULT hr = oD3DReset(dev, pp);
    Log("IDirect3DDevice9::Reset -> %08x", (unsigned)hr);
    if (SUCCEEDED(hr)) ApplyBorderless(g_hWnd);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkD3DCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
    HWND hwnd, DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** outDev)
{
    if (cfg.borderless && pp) {
        FixPresentParams(pp, hwnd);
        ApplyBorderless(hwnd);   // window exists at this point
    }
    HRESULT hr = oD3DCreateDevice(d3d, adapter, type, hwnd, behavior, pp, outDev);
    Log("IDirect3D9::CreateDevice -> %08x (%dx%d)", (unsigned)hr,
        pp ? (int)pp->BackBufferWidth : 0, pp ? (int)pp->BackBufferHeight : 0);
    if (SUCCEEDED(hr) && outDev && *outDev) {
        oD3DReset = (D3D9Dev_Reset_t)VTableHook(*outDev, 16, (void*)hkD3DReset);
        Log("d3d9 device hooked (Reset=16) subHook=%d", cfg.subHook ? 1 : 0);
        ApplyBorderless(hwnd);
    }
    return hr;
}

static IDirect3D9* WINAPI hkDirect3DCreate9(UINT sdkVersion)
{
    IDirect3D9* d3d = oDirect3DCreate9(sdkVersion);
    Log("Direct3DCreate9(%u) -> %p", sdkVersion, d3d);
    if (d3d) oD3DCreateDevice = (D3D9_CreateDevice_t)VTableHook(d3d, 16, (void*)hkD3DCreateDevice);
    return d3d;
}

// Block the game from switching the actual display mode
static LONG WINAPI hkChangeDisplaySettingsExW(LPCWSTR dev, DEVMODEW* dm, HWND hwnd, DWORD flags, LPVOID param)
{
    Log("ChangeDisplaySettingsExW blocked (%ux%u@%u)", dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmDisplayFrequency : 0);
    return DISP_CHANGE_SUCCESSFUL;
}
static LONG WINAPI hkChangeDisplaySettingsExA(LPCSTR dev, DEVMODEA* dm, HWND hwnd, DWORD flags, LPVOID param)
{
    Log("ChangeDisplaySettingsExA blocked (%ux%u@%u)", dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmDisplayFrequency : 0);
    return DISP_CHANGE_SUCCESSFUL;
}

// ==================================================================
// Raw mouse input
// ==================================================================
static volatile LONG g_accumDX = 0, g_accumDY = 0;       // consumed by DInput path
static volatile LONG g_curDX = 0, g_curDY = 0;           // consumed by GetCursorPos path
static float g_vcX = 0, g_vcY = 0; static bool g_vcInit = false;
static volatile LONG g_lastRawTick = 0;

static void InjectRawDeltas(LONG dx, LONG dy)
{
    dx = (LONG)(dx * cfg.sensitivity);
    dy = (LONG)(dy * cfg.sensitivity);
    InterlockedAdd(&g_accumDX, dx); InterlockedAdd(&g_accumDY, dy);
    InterlockedAdd(&g_curDX, dx);   InterlockedAdd(&g_curDY, dy);
    InterlockedExchange(&g_lastRawTick, (LONG)GetTickCount());
    // drive the real cursor 1:1 so WM_MOUSEMOVE-based menus get raw movement
    g_vcX += dx; g_vcY += dy;
    float minX = (float)GetSystemMetrics(SM_XVIRTUALSCREEN), minY = (float)GetSystemMetrics(SM_YVIRTUALSCREEN);
    float maxX = minX + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, maxY = minY + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1;
    if (g_vcX < minX) g_vcX = minX; if (g_vcX > maxX) g_vcX = maxX;
    if (g_vcY < minY) g_vcY = minY; if (g_vcY > maxY) g_vcY = maxY;
    if (oSetCursorPos) oSetCursorPos((int)lroundf(g_vcX), (int)lroundf(g_vcY));
}

static bool RawAlive() { return (GetTickCount() - (DWORD)g_lastRawTick) < 2000; }

static WNDPROC oWndProc = nullptr;
static volatile LONG g_gdsCalls = 0, g_gddCalls = 0, g_gcpCalls = 0;
static volatile LONG g_gdsNative = 0;
static volatile LONG g_wmInputN = 0, g_wmMoveN = 0;
static volatile LONG g_wmInDX = 0, g_wmInDY = 0;
static volatile LONG g_lastMoveLP = 0;
static DWORD g_inputLastReport = 0;

static void ReportInputRates()
{
    DWORD now = GetTickCount();
    if (now - g_inputLastReport < 1000) return;
    g_inputLastReport = now;
    Log("input/s: WM_INPUT=%ld (sum %+ld,%+ld) WM_MOUSEMOVE=%ld lastLP=%08lx GDS=%ld GCP=%ld raw=%d",
        InterlockedExchange(&g_wmInputN, 0), InterlockedExchange(&g_wmInDX, 0), InterlockedExchange(&g_wmInDY, 0),
        InterlockedExchange(&g_wmMoveN, 0), (long)InterlockedExchange(&g_lastMoveLP, 0),
        g_gdsCalls, g_gcpCalls, RawAlive() ? 1 : 0);
}

static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        InterlockedIncrement(&g_wmMoveN);
        InterlockedExchange(&g_lastMoveLP, (LONG)lp);
        ReportInputRates();
        break;
    case WM_DISPLAYCHANGE:
        g_screenW = 0; // force re-measure; watchdog re-applies borderless
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            POINT p;
            if (GetPhysicalCursorPos(&p)) { g_vcX = (float)p.x; g_vcY = (float)p.y; g_vcInit = true; }
        }
        break;
    }
    return CallWindowProcW(oWndProc, hwnd, msg, wp, lp);
}

static void RegisterRawMouse(HWND hwnd)
{
    RAWINPUTDEVICE rid = { 0x01, 0x02, 0, hwnd }; // HID mouse
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        Log("Raw input registered on hwnd %p", hwnd);
    } else {
        Log("RegisterRawInputDevices FAILED (%lu)", GetLastError());
    }
}

// ---- DirectInput mouse hooks ----------------------------------------------
typedef HRESULT (WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DirectInput8Create_t oDirectInput8Create = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* DI8_CreateDevice_t)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
static DI8_CreateDevice_t oDICreateDevice = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* DID8_GetDeviceState_t)(IDirectInputDevice8W*, DWORD, LPVOID);
static DID8_GetDeviceState_t oDIGetDeviceState = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* DID8_GetDeviceData_t)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static DID8_GetDeviceData_t oDIGetDeviceData = nullptr;
typedef HRESULT (STDMETHODCALLTYPE* DID8_SetDataFormat_t)(IDirectInputDevice8W*, LPCDIDATAFORMAT);
static DID8_SetDataFormat_t oDISetDataFormat = nullptr;

static IDirectInputDevice8W* g_mouseDevs[8] = {};
static volatile LONG g_mouseDevCount = 0;
static volatile LONG g_ofsX = FIELD_OFFSET(DIMOUSESTATE, lX);
static volatile LONG g_ofsY = FIELD_OFFSET(DIMOUSESTATE, lY);

static bool IsMouseDev(void* dev)
{
    LONG n = g_mouseDevCount;
    for (LONG i = 0; i < n; i++) if (g_mouseDevs[i] == dev) return true;
    return false;
}

static void ConsumeAccum(LONG* dx, LONG* dy)
{
    *dx = InterlockedExchange(&g_accumDX, 0);
    *dy = InterlockedExchange(&g_accumDY, 0);
}

static HRESULT STDMETHODCALLTYPE hkSetDataFormat(IDirectInputDevice8W* dev, LPCDIDATAFORMAT fmt)
{
    if (fmt && fmt->dwObjSize == sizeof(DIOBJECTDATAFORMAT)) {
        for (DWORD i = 0; i < fmt->dwNumObjs; i++) {
            const GUID* g = fmt->rgodf[i].pguid;
            if (!g) continue;
            if (*g == GUID_XAxis) InterlockedExchange(&g_ofsX, (LONG)fmt->rgodf[i].dwOfs);
            if (*g == GUID_YAxis) InterlockedExchange(&g_ofsY, (LONG)fmt->rgodf[i].dwOfs);
        }
        Log("SetDataFormat: cbData=%lu ofsX=%ld ofsY=%ld", fmt->dwDataSize, g_ofsX, g_ofsY);
    }
    return oDISetDataFormat(dev, fmt);
}

static HRESULT STDMETHODCALLTYPE hkGetDeviceState(IDirectInputDevice8W* dev, DWORD cbData, LPVOID data)
{
    HRESULT hr = oDIGetDeviceState(dev, cbData, data);
    if (IsMouseDev(dev) && SUCCEEDED(hr)) {
        LONG nn = InterlockedIncrement(&g_gdsNative);
        if (nn % 600 == 1) Log("GDS native #%ld cb=%lu rawAlive=%d", nn, cbData, RawAlive() ? 1 : 0);
    }
    if (cfg.rawInput && RawAlive() && SUCCEEDED(hr) && IsMouseDev(dev) && data && cbData >= 8) {
        LONG dx, dy; ConsumeAccum(&dx, &dy);
        *(LONG*)((BYTE*)data + g_ofsX) = dx;
        *(LONG*)((BYTE*)data + g_ofsY) = dy;
        LONG n = InterlockedIncrement(&g_gdsCalls);
        if (n % 60 == 1)
            Log("GDS #%ld cb=%lu inject dx=%ld dy=%ld btn=%02x", n, cbData, dx, dy,
                cbData >= 13 ? ((BYTE*)data)[12] : 0);
    }
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* DID8_Acquire_t)(IDirectInputDevice8W*);
static DID8_Acquire_t oDIAcquire = nullptr, oDIUnacquire = nullptr;
static HRESULT STDMETHODCALLTYPE hkAcquire(IDirectInputDevice8W* dev)
{
    if (IsMouseDev(dev)) Log("mouse Acquire");
    return oDIAcquire(dev);
}
static HRESULT STDMETHODCALLTYPE hkUnacquire(IDirectInputDevice8W* dev)
{
    if (IsMouseDev(dev)) Log("mouse Unacquire");
    return oDIUnacquire(dev);
}

static HRESULT STDMETHODCALLTYPE hkGetDeviceData(IDirectInputDevice8W* dev, DWORD cbElem,
    LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD flags)
{
    HRESULT hr = oDIGetDeviceData(dev, cbElem, rgdod, pdwInOut, flags);
    if (cfg.rawInput && IsMouseDev(dev) && rgdod && pdwInOut && (SUCCEEDED(hr) || hr == DI_BUFFEROVERFLOW)) {
        DWORD n = *pdwInOut;
        DIDEVICEOBJECTDATA* lastX = nullptr; DIDEVICEOBJECTDATA* lastY = nullptr;
        for (DWORD i = 0; i < n; i++) {
            if ((LONG)rgdod[i].dwOfs == g_ofsX) { rgdod[i].dwData = 0; lastX = &rgdod[i]; }
            else if ((LONG)rgdod[i].dwOfs == g_ofsY) { rgdod[i].dwData = 0; lastY = &rgdod[i]; }
        }
        LONG dx, dy; ConsumeAccum(&dx, &dy);
        if (lastX) lastX->dwData = (DWORD)dx;
        if (lastY) lastY->dwData = (DWORD)dy;
        if (InterlockedIncrement(&g_gddCalls) % 600 == 1)
            Log("GetDeviceData x%ld (mouse, buffered, raw deltas injected)", g_gddCalls);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkDICreateDevice(IDirectInput8W* di, REFGUID rguid,
    LPDIRECTINPUTDEVICE8W* devOut, LPUNKNOWN unk)
{
    HRESULT hr = oDICreateDevice(di, rguid, devOut, unk);
    if (SUCCEEDED(hr) && devOut && *devOut && rguid == GUID_SysMouse) {
        LONG i = g_mouseDevCount;
        if (i < 8) { g_mouseDevs[i] = *devOut; InterlockedIncrement(&g_mouseDevCount); }
        oDISetDataFormat = (DID8_SetDataFormat_t)VTableHook(*devOut, 11, (void*)hkSetDataFormat);
        oDIGetDeviceState = (DID8_GetDeviceState_t)VTableHook(*devOut, 9, (void*)hkGetDeviceState);
        oDIGetDeviceData  = (DID8_GetDeviceData_t)VTableHook(*devOut, 10, (void*)hkGetDeviceData);
        oDIAcquire = (DID8_Acquire_t)VTableHook(*devOut, 7, (void*)hkAcquire);
        oDIUnacquire = (DID8_Acquire_t)VTableHook(*devOut, 8, (void*)hkUnacquire);
        Log("DirectInput mouse device created and hooked (%p)", *devOut);
    }
    return hr;
}

static HRESULT WINAPI hkDirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN unk)
{
    HRESULT hr = oDirectInput8Create(inst, ver, riid, out, unk);
    Log("DirectInput8Create -> %08x", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out && riid == IID_IDirectInput8W)
        oDICreateDevice = (DI8_CreateDevice_t)VTableHook(*out, 3, (void*)hkDICreateDevice);
    if (SUCCEEDED(hr) && out && *out && riid == IID_IDirectInput8A)
        oDICreateDevice = (DI8_CreateDevice_t)VTableHook(*out, 3, (void*)hkDICreateDevice);
    return hr;
}

// ---- Cursor-position path (menus) ------------------------------------------
typedef BOOL (WINAPI* GetCursorPos_t)(LPPOINT);
static GetCursorPos_t oGetCursorPos = nullptr;

static BOOL WINAPI hkGetCursorPos(LPPOINT p)
{
    if (!cfg.rawInput) return oGetCursorPos(p);
    if (!g_vcInit) { if (oGetCursorPos(p)) { g_vcX = (float)p->x; g_vcY = (float)p->y; g_vcInit = true; } }
    p->x = (int)lroundf(g_vcX);
    p->y = (int)lroundf(g_vcY);
    if (InterlockedIncrement(&g_gcpCalls) % 3600 == 1) Log("GetCursorPos x%ld (virtual cursor)", g_gcpCalls);
    return TRUE;
}

static BOOL WINAPI hkSetCursorPos(int x, int y)
{
    if (cfg.rawInput) { g_vcX = (float)x; g_vcY = (float)y; g_vcInit = true; }
    static volatile LONG s_n = 0;
    if (InterlockedIncrement(&s_n) % 600 == 1) Log("SetCursorPos(%d,%d) #%ld", x, y, s_n);
    return oSetCursorPos(x, y);
}

typedef BOOL (WINAPI* ClipCursor_t)(CONST RECT*);
static ClipCursor_t oClipCursor = nullptr;
static BOOL WINAPI hkClipCursor(CONST RECT* r)
{
    if (r) Log("ClipCursor(%ld,%ld,%ld,%ld)", r->left, r->top, r->right, r->bottom);
    else Log("ClipCursor(NULL)");
    return oClipCursor(r);
}

// ------------------------------------------------------------------
// Boot-asset trace: log every non-system file the engine opens (opt-in via
// [Debug] LogFiles=1). Deduped by path hash so repeated .str opens don't flood.
// Used to identify what the publisher/studio boot logos actually load from.
// ------------------------------------------------------------------
typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI* CreateFileA_t)(LPCSTR,  DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t oCreateFileW = nullptr;
static CreateFileA_t oCreateFileA = nullptr;

static volatile LONG g_openSeen = 0;
static DWORD g_openHash[1024] = { 0 };   // 0 = empty slot

static bool FileInterestingW(const wchar_t* p)
{
    // skip OS / device / pipe noise
    if (_wcsnicmp(p, L"\\\\?\\", 4) == 0) return false;
    if (_wcsnicmp(p, L"\\Device\\", 8) == 0) return false;
    if (_wcsnicmp(p, L"\\\\.\\pipe", 8) == 0) return false;
    if (wcsstr(p, L"\\Windows\\") || wcsstr(p, L"\\windows\\")) return false;
    if (wcsstr(p, L"\\System32\\") || wcsstr(p, L"\\system32\\")) return false;
    if (wcsstr(p, L"\\SysWOW64\\") || wcsstr(p, L"\\syswow64\\")) return false;
    return true;
}

static bool OpenSeen(DWORD h)
{
    if (!h) h = 1;
    DWORD i = h & 1023;
    for (int k = 0; k < 8; k++) {
        DWORD slot = (i + k) & 1023;
        if (g_openHash[slot] == 0) { g_openHash[slot] = h; return false; }
        if (g_openHash[slot] == h) return true;
    }
    return true; // table region full -> suppress
}

static DWORD FnvW(const wchar_t* s)
{
    DWORD h = 2166136261u;
    for (; *s; ++s) { wchar_t c = *s | 32; h ^= (BYTE)c; h *= 16777619u; }
    return h;
}
static DWORD FnvA(const char* s)
{
    DWORD h = 2166136261u;
    for (; *s; ++s) { char c = *s | 32; h ^= (BYTE)c; h *= 16777619u; }
    return h;
}

static HANDLE WINAPI hkCreateFileW(LPCWSTR fn, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fa, HANDLE ht)
{
    if (fn && FileInterestingW(fn) && !OpenSeen(FnvW(fn)))
        Log("OPEN #%ld W: %ls", InterlockedIncrement(&g_openSeen), fn);
    return oCreateFileW(fn, a, s, sa, cd, fa, ht);
}

static HANDLE WINAPI hkCreateFileA(LPCSTR fn, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fa, HANDLE ht)
{
    if (fn) {
        wchar_t w[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, fn, -1, w, MAX_PATH) > 0 &&
            FileInterestingW(w) && !OpenSeen(FnvA(fn)))
            Log("OPEN #%ld A: %s", InterlockedIncrement(&g_openSeen), fn);
    }
    return oCreateFileA(fn, a, s, sa, cd, fa, ht);
}


// ==================================================================
// APT UI hooks (subtitle scaling + diagnostics)
// ==================================================================
// gAptFuncs table (RVA from static analysis):
//   +0x40 pfnDrawString      void __cdecl (void* ctx, void* p1, void* p2)
//   +0x68 pfnSetVertexMatrix void __cdecl (const float m[6])  // {a,b,c,d,tx,ty}
typedef void (__cdecl* AptDrawString_t)(void*, void*, void*);
typedef void (__cdecl* AptSetVertexMatrix_t)(const void*);
static AptDrawString_t oAptDrawString = nullptr;
static AptSetVertexMatrix_t oAptSetVertexMatrix = nullptr;

static volatile LONG g_svmCalls = 0, g_dsCalls = 0;
static float g_svmMin[6] = { 1e30f,1e30f,1e30f,1e30f,1e30f,1e30f };
static float g_svmMax[6] = {-1e30f,-1e30f,-1e30f,-1e30f,-1e30f,-1e30f };
static DWORD g_svmLastReport = 0;
static float g_lastMtx[6] = { 1,0,0,1,0,0 };

static void __cdecl hkAptSetVertexMatrix(const void* m)
{
    const float* src = (const float*)m;
    memcpy((void*)g_lastMtx, src, sizeof(g_lastMtx));
    if (g_captureUntil && GetTickCount() < g_captureUntil) {
        LONG n = InterlockedIncrement(&g_capSvmN);
        if (n % 10 == 1 && n < 60000)
            Log("CAP SVM #%ld {%.4f, %.4f, %.4f, %.4f, %.2f, %.2f}",
                n, src[0], src[1], src[2], src[3], src[4], src[5]);
    }
    if (cfg.logApt) {
        LONG n = InterlockedIncrement(&g_svmCalls);
        for (int i = 0; i < 6; i++) {
            if (src[i] < g_svmMin[i]) g_svmMin[i] = src[i];
            if (src[i] > g_svmMax[i]) g_svmMax[i] = src[i];
        }
        DWORD now = GetTickCount();
        if (now - g_svmLastReport > 2000) {
            g_svmLastReport = now;
            Log("SetVertexMatrix calls=%ld ranges: a[%.3f..%.3f] b[%.3f..%.3f] c[%.3f..%.3f] d[%.3f..%.3f] tx[%.1f..%.1f] ty[%.1f..%.1f]",
                n, g_svmMin[0], g_svmMax[0], g_svmMin[1], g_svmMax[1], g_svmMin[2], g_svmMax[2],
                g_svmMin[3], g_svmMax[3], g_svmMin[4], g_svmMax[4], g_svmMin[5], g_svmMax[5]);
            if (n <= 200)
                Log("  sample matrix: {%.4f, %.4f, %.4f, %.4f, %.2f, %.2f}", src[0], src[1], src[2], src[3], src[4], src[5]);
        }
    }
    // NOTE: no scaling here. The APT text renderer builds its own transform and
    // ignores this matrix; scaling it only blows up HUD panels/bars, not glyphs.
    // Text size is driven by the per-object font-size float at ctx+0x70, scaled
    // in hkAptDrawString instead.
    oAptSetVertexMatrix(m);
}

// APT text object layout (edi/a1 in the renderer at 0x5cbdb0):
//   +0x24/+0x28 = text position (x,y), +0x2c/+0x30 = second box corner
//   +0x34       = alignment, +0x70 = font size (float), +0x84 = font handle
// Glyph scale = (font table factor) * [ctx+0x70], fed to the emitter at 0x48d310.
static const DWORD APT_TEXT_FONTSIZE = 0x70;

static void __cdecl hkAptDrawString(void* a1, void* a2, void* a3)
{
    if (g_captureUntil && GetTickCount() < g_captureUntil && a1) {
        LONG n = InterlockedIncrement(&g_capDsN);
        if (n < 8000) {
            BYTE* c = (BYTE*)a1;
            DWORD type = *(DWORD*)(c + 0x1c);
            Log("CAP DS #%ld ctx=%p type=%lu f24=%.2f f28=%.2f f2c=%.2f f30=%.2f f70=%.2f f74=%.2f f78=%.2f f7c=%.2f",
                n, a1, type,
                *(float*)(c + 0x24), *(float*)(c + 0x28), *(float*)(c + 0x2c), *(float*)(c + 0x30),
                *(float*)(c + 0x70), *(float*)(c + 0x74), *(float*)(c + 0x78), *(float*)(c + 0x7c));
        }
    }
    if (cfg.logApt >= 2) {
        Log("DS ctx=%p m={%.4f, %.4f, %.4f, %.4f, %.2f, %.2f}", a1,
            g_lastMtx[0], g_lastMtx[1], g_lastMtx[2], g_lastMtx[3], g_lastMtx[4], g_lastMtx[5]);
    } else if (cfg.logApt && InterlockedIncrement(&g_dsCalls) % 30 == 1) {
        Log("AptDrawString x%ld (ctx=%p p1=%p p2=%p)", g_dsCalls, a1, a2, a3);
    }

    // DIAGNOSTIC BUILD: scaling disabled. Subtitles use the type-1 renderer
    // (indirect jmp [0x1ce3ae0]) whose object layout differs from type-2; the
    // +0x70 font-size field only applies to type-2 (HUD) text. Capture first.
    oAptDrawString(a1, a2, a3);
}

static bool g_aptHooked = false;

// --- low-level renderer probes -------------------------------------------------
// The subtitle UI bypasses DrawString entirely and calls a glyph renderer
// directly with `this` in EAX (unusual convention), so these detours are naked
// stubs: they preserve every register/flag, log the context, then tail-jump to
// the trampoline. ProbeType2 = 0x5cbdb0 (type-2 renderer), ProbeType1 = the
// packed type-1 target read from [0x1ce3ae0].
static void* oType2Render = nullptr;
static void* oType1Render = nullptr;
static volatile LONG g_t2N = 0, g_t1N = 0;

static void DumpCtx(const char* tag, void* ctx)
{
    if (!ctx) return;
    BYTE* c = (BYTE*)ctx;
    Log("CAP %s ctx=%p type=%lu f24=%.2f f28=%.2f f2c=%.2f f30=%.2f f70=%.2f f74=%.2f f78=%.2f f7c=%.2f",
        tag, ctx, *(DWORD*)(c + 0x1c),
        *(float*)(c + 0x24), *(float*)(c + 0x28), *(float*)(c + 0x2c), *(float*)(c + 0x30),
        *(float*)(c + 0x70), *(float*)(c + 0x74), *(float*)(c + 0x78), *(float*)(c + 0x7c));
}

static void __cdecl ProbeType2(void* ctx)
{
    LONG n = InterlockedIncrement(&g_t2N);
    if (g_captureUntil && GetTickCount() < g_captureUntil && n < 8000) DumpCtx("T2", ctx);
}
static void __cdecl ProbeType1(void* ctx)
{
    LONG n = InterlockedIncrement(&g_t1N);
    if (g_captureUntil && GetTickCount() < g_captureUntil && n < 8000) DumpCtx("T1", ctx);
}

__declspec(naked) static void hkType2Render(void)
{
    __asm {
        push eax
        pushfd
        pushad
        push dword ptr[esp + 0x24]
        call ProbeType2
        add  esp, 4
        popad
        popfd
        pop  eax
        jmp  dword ptr[oType2Render]
    }
}
__declspec(naked) static void hkType1Render(void)
{
    __asm {
        push eax
        pushfd
        pushad
        push dword ptr[esp + 0x24]
        call ProbeType1
        add  esp, 4
        popad
        popfd
        pop  eax
        jmp  dword ptr[oType1Render]
    }
}

static bool g_type2Hooked = false;
static void TryHookRenderers(uintptr_t base)
{
    if (!g_type2Hooked) {
        void* p = (void*)(base + (0x5cbdb0 - 0x400000));
        if (MH_CreateHook(p, (void*)hkType2Render, &oType2Render) == MH_OK) { MH_EnableHook(p); g_type2Hooked = true; }
    }
    if (!oType1Render) {
        void* p = *(void**)(base + (0x1ce3ae0 - 0x400000));
        if (p && MH_CreateHook(p, (void*)hkType1Render, &oType1Render) == MH_OK) {
            MH_EnableHook(p);
            Log("type1 renderer hooked at %p", p);
        }
    }
}

static void TryHookApt()
{
    if (g_aptHooked) return;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (*(WORD*)base != 0x5A4D) return; // MZ
    // Inline-hook the renderer FUNCTIONS, not the gAptFuncs table slots: the
    // movie/subtitle UI calls DrawString through a cached pointer and never
    // touches the table, so a slot swap misses subtitles entirely.
    void* pDS  = (void*)(base + (0x5CC5B0 - 0x400000));
    void* pSVM = (void*)(base + (0x5C52E0 - 0x400000));
    if (MH_CreateHook(pDS, (void*)hkAptDrawString, (void**)&oAptDrawString) == MH_OK &&
        MH_CreateHook(pSVM, (void*)hkAptSetVertexMatrix, (void**)&oAptSetVertexMatrix) == MH_OK) {
        MH_EnableHook(pDS);
        MH_EnableHook(pSVM);
        g_aptHooked = true;
        void* t1 = *(void**)(base + (0x1ce3ae0 - 0x400000));
        Log("APT inline hooks installed: DrawString=%p SetVertexMatrix=%p type1Target=%p",
            pDS, pSVM, t1);
    }
    TryHookRenderers(base);
}

// F9-style capture: global key poll (independent of window focus / message delivery).
// Press Scroll Lock or Pause during gameplay with subtitles on screen -> 6s APT dump.
static void StartCapture(const char* why)
{
    g_capDsN = 0; g_capSvmN = 0; g_t2N = 0; g_t1N = 0;
    g_captureUntil = GetTickCount() + 6000;
    Log("=== CAPTURE START 6s via %s (screen=%dx%d textScale=%.3f gdsNative=%ld) ===",
        why, g_screenW, g_screenH, g_textK, g_gdsNative);
}

static DWORD WINAPI CapturePollThread(LPVOID)
{
    bool prevScroll = false, prevPause = false;
    for (;;) {
        Sleep(60);
        bool scroll = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        if (scroll && !prevScroll) StartCapture("SCROLLLOCK");
        prevScroll = scroll;
        bool pause = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
        if (pause && !prevPause) StartCapture("PAUSE");
        prevPause = pause;
    }
    return 0;
}

// ==================================================================
// Init threads
// ==================================================================
static BOOL CALLBACK FindOwnWindow(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 64) && _wcsicmp(cls, L"Godfather2WndClass") == 0) {
        g_hWnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI WindowThread(LPVOID)
{
    // wait for game window
    for (int i = 0; i < 600 && !g_hWnd; i++) {
        EnumWindows(FindOwnWindow, 0);
        if (!g_hWnd) Sleep(100);
    }
    if (!g_hWnd) { Log("Game window never appeared"); return 1; }
    Log("Found game window %p", g_hWnd);

    ApplyBorderless(g_hWnd);

    if (cfg.rawInput) {
        oWndProc = (WNDPROC)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    }

    // watchdog: keep borderless geometry (resolution changes, game repositioning)
    for (;;) {
        Sleep(1000);
        if (g_captureUntil && GetTickCount() >= g_captureUntil) {
            Log("=== CAPTURE END: DrawString=%ld SetVertexMatrix=%ld T2=%ld T1=%ld ===",
                g_capDsN, g_capSvmN, g_t2N, g_t1N);
            g_captureUntil = 0;
        }
        if (cfg.borderless && g_hWnd && IsWindow(g_hWnd)) ApplyBorderless(g_hWnd);
        if (g_screenH == 0) { g_screenW = GetSystemMetrics(SM_CXSCREEN); g_screenH = GetSystemMetrics(SM_CYSCREEN); }
        g_textK = cfg.textScale;
        TryHookApt();
        TryHookRenderers((uintptr_t)GetModuleHandleW(nullptr));
    }
    return 0;
}

// Raw input is received on our own message-only window: the game's PeekMessage
// loop never dispatches WM_INPUT to its own window, so registering there is useless.
static LRESULT CALLBACK RawWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size && size < 128) {
            BYTE buf[128];
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE &&
                    (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    InjectRawDeltas(ri->data.mouse.lLastX, ri->data.mouse.lLastY);
                    InterlockedIncrement(&g_wmInputN);
                    InterlockedAdd(&g_wmInDX, ri->data.mouse.lLastX);
                    InterlockedAdd(&g_wmInDY, ri->data.mouse.lLastY);
                }
            }
        }
        ReportInputRates();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI RawThread(LPVOID)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RawWndProc;
    wc.hInstance = g_hSelf;
    wc.lpszClassName = L"gf2fix_rawwnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"gf2fix_rawwnd", L"gf2fix raw", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hSelf, nullptr);
    if (!hwnd) { Log("raw message window FAILED (%lu)", GetLastError()); return 1; }
    RegisterRawMouse(hwnd);
    MSG msg;
    for (;;) {
        if (GetMessageW(&msg, hwnd, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static DWORD WINAPI HookThread(LPVOID)
{
    MH_Initialize();

    if (cfg.borderless) {
        HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
        if (hD3D9) {
            void* p = (void*)GetProcAddress(hD3D9, "Direct3DCreate9");
            if (p && MH_CreateHook(p, (void*)hkDirect3DCreate9, (void**)&oDirect3DCreate9) == MH_OK)
                MH_EnableHook(p);
            Log("Direct3DCreate9 hook: %p", p);
        }
        HMODULE hU = GetModuleHandleA("user32.dll");
        void* p1 = (void*)GetProcAddress(hU, "ChangeDisplaySettingsExW");
        if (p1 && MH_CreateHook(p1, (void*)hkChangeDisplaySettingsExW, nullptr) == MH_OK) MH_EnableHook(p1);
        void* p2 = (void*)GetProcAddress(hU, "ChangeDisplaySettingsExA");
        if (p2 && MH_CreateHook(p2, (void*)hkChangeDisplaySettingsExA, nullptr) == MH_OK) MH_EnableHook(p2);
    }

    if (cfg.rawInput) {
        HMODULE hDI = LoadLibraryA("dinput8.dll"); // resolves through ASI-loader proxy to real dinput8
        if (hDI) {
            void* p = (void*)GetProcAddress(hDI, "DirectInput8Create");
            if (p && MH_CreateHook(p, (void*)hkDirectInput8Create, (void**)&oDirectInput8Create) == MH_OK)
                MH_EnableHook(p);
            Log("DirectInput8Create hook: %p", p);
        }
        HMODULE hU = GetModuleHandleA("user32.dll");
        void* p = (void*)GetProcAddress(hU, "GetCursorPos");
        if (p && MH_CreateHook(p, (void*)hkGetCursorPos, (void**)&oGetCursorPos) == MH_OK) MH_EnableHook(p);
        p = (void*)GetProcAddress(hU, "SetCursorPos");
        if (p && MH_CreateHook(p, (void*)hkSetCursorPos, (void**)&oSetCursorPos) == MH_OK) MH_EnableHook(p);
        p = (void*)GetProcAddress(hU, "ClipCursor");
        if (p && MH_CreateHook(p, (void*)hkClipCursor, (void**)&oClipCursor) == MH_OK) MH_EnableHook(p);
    }

    if (cfg.logFiles) {
        HMODULE hK = GetModuleHandleA("kernel32.dll");
        void* pw = (void*)GetProcAddress(hK, "CreateFileW");
        if (pw && MH_CreateHook(pw, (void*)hkCreateFileW, (void**)&oCreateFileW) == MH_OK) MH_EnableHook(pw);
        void* pa = (void*)GetProcAddress(hK, "CreateFileA");
        if (pa && MH_CreateHook(pa, (void*)hkCreateFileA, (void**)&oCreateFileA) == MH_OK) MH_EnableHook(pa);
        Log("CreateFile file-trace hooks: W=%p A=%p", pw, pa);
    }
    Log("gf2fix loaded: borderless=%d raw=%d sens=%.2f textScale=%.2f",
        cfg.borderless, cfg.rawInput, cfg.sensitivity, cfg.textScale);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hSelf = hModule;
        LogInit();
        LoadConfig();
        CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, CapturePollThread, nullptr, 0, nullptr);
        if (cfg.rawInput) CreateThread(nullptr, 0, RawThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
