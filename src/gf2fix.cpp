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
    BOOL  aptHooks      = FALSE;  // install exe-RVA APT hooks (ONLY valid on the original packed exe)
    BOOL  cursorHooks   = FALSE;  // detour user32 GetCursorPos/SetCursorPos/ClipCursor
    BOOL  cameraProbe   = FALSE;  // hook the mouse-look functions and dump the camera look rigs
    BOOL  deDamp        = FALSE;  // hold the look smoothing coefficient at Smooth, per call
    // rig+0x20/+0x24 times [self+0x60] is a low-pass coefficient, not a gain: 1.0 tracks
    // the mouse exactly, 0.75 is the value the engine authors.  Clamped to <= 1.0 because
    // above that the recurrence is unstable and the view oscillates.  Measured live: this
    // branch only runs in the front-end, so Smooth does not reach gameplay at all.
    float smooth        = 0.75f;
    float lookGain      = 1.0f;   // multiplier on the look targets (degrees per mouse count)
    float rampTime      = 0.0f;   // >0 forces rig+0x10, the seconds the target takes to ramp in
    // On-foot look targets, yaw then pitch, measured live.  <= 0 leaves that axis alone.
    float ref[2]        = { 3.75f, 2.50f };
    // The look caller has a path that runs both deltas through the filter at 0x6C0600
    // (an accumulator with a saturation limit and a hold/decay timer) before the camera
    // sees them.  Measured live: gameplay never takes that path, so these are inert
    // there.  Kept gated and off by default.
    BOOL  filterBypass  = FALSE;  // feed the converters the raw delta instead
    BOOL  filterLog     = FALSE;  // log raw vs filtered delta plus the filter's own state
    float filterGain    = 1.0f;   // multiplier on the raw delta when bypassing
    // [cam+0x60] is a blend coefficient the engine multiplies into several post-converter
    // stages. Measured live: 1.00 on foot, decaying to 0.41 in the car. Forcing it lower
    // was tested and changed nothing. 0 = leave alone.
    float carCam60      = 0.0f;
    // Overrides for two single-reader engine constants; see ApplyDataLevers. 0 = leave
    // the engine's value alone, which is also what a missing or unreadable ini gives.
    float carSpeedThresh = 0.0f;   // 0xE50FA8, 25.0: speed that starts the in-car damping ramp
    float carCentreDelay = 0.0f;   // 0xE50E44, 0.1s: idle time before the chase cam engages
    // THE ACTUAL IN-CAR FIX; see ApplyLookDivisor. Scales the vehicle look by making the
    // engine's 59.94Hz integration divisor track the real frame time. 0 = leave alone.
    float carLookScale   = 0.0f;
    // THE VERTICAL FIX; see ApplyPitchGate. Keeps the mouse in charge of pitch once the
    // car is moving, instead of letting an auto system take the axis away.
    bool  carPitchMouse  = false;
    // ALL SMOOTHING OFF; see ApplyNoSmoothPatches. Forces the in-car camera lerps that the
    // engine caps below 1.0 to snap onto their target in the same frame.
    bool  carNoSmooth    = false;
    // THE LAST SMOOTHER; see ApplySpringStiffness. Raises the look spring's stiffness so the
    // in-car camera closes on its target in frames instead of about a second. 0 = leave alone.
    float carSpringK     = 0.0f;
    // The same spring is also what drags the view back behind the car the moment the mouse
    // stops, so one stiffness cannot be both crisp and gentle. These three split it by phase
    // while driving; see ApplySpringStiffness.
    float carCentreIdle  = 0.0f;   // s of no mouse movement before the glide back starts
    float carHoldK       = 0.0f;   // stiffness during that wait: low holds the view in place
    float carCentreK     = 0.0f;   // stiffness for the glide itself: low eases, high snaps
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
    if (!cfg.log && !cfg.logApt && !cfg.logFiles && !cfg.cameraProbe && !cfg.filterLog &&
        !g_captureUntil) return;
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
        "; gf2fix - borderless window, 1:1 raw mouse and in-car camera fixes for\n"
        "; The Godfather II. This file is written on the first run and every value below\n"
        "; is already the recommended one, so nothing needs editing. Delete it to get these\n"
        "; defaults back. The optional keys are documented in README.md.\n"
        "\n"
        "[Borderless]\n"
        "; OFF by default because Modern Fixes already does borderless at your desktop\n"
        "; resolution and refresh rate, and its supersampling needs its own. Set to 1 only if\n"
        "; you use a plain ASI loader with no borderless option of its own.\n"
        "Enabled=0\n"
        "\n"
        "[RawInput]\n"
        "; True 1:1 mouse: no acceleration, smoothing or deadzone. 0 = the game's input.\n"
        "Enabled=1\n"
        "; Mouse multiplier. 1.0 = true 1:1; raise it only if 1.0 feels slow on your DPI.\n"
        "Sensitivity=1.0\n"
        "\n"
        "[Camera]\n"
        "; The game divides its in-car look by a hardcoded console frame rate (59.94 Hz) and\n"
        "; then integrates it with a spring, so on a fast PC the camera turns far less than\n"
        "; the mouse does and keeps sliding lazily behind it. These values undo that, and the\n"
        "; same applies to the cover and aim cameras. All of them are re-read every 2 seconds,\n"
        "; so the feel can be tuned while playing.\n"
        "; How much of your mouse movement the camera keeps, at any frame rate.\n"
        "CarLookScale=1.0\n"
        "; Pitch normally stops following the mouse as soon as the car moves. 1 = keep it.\n"
        "CarPitchMouse=1\n"
        "; How fast the view catches up with the mouse. Engine value is 175; higher is crisper.\n"
        "CarSpringK=5000\n"
        "; Seconds of no mouse movement before the view eases back behind the car.\n"
        "CarCentreIdle=2.5\n"
        "; Stiffness during that wait (keeps the view where you left it), then during the\n"
        "; glide back. A glide lasts roughly 100/k seconds, so 40 is about 2.5 seconds.\n"
        "CarHoldK=5\n"
        "CarCentreK=40\n"
        "; How strongly the camera position is pulled back onto the car, 0..1. 0.40 is the\n"
        "; stock in-car amount; 1.00 leaves the view jittery over bumps and corners.\n"
        "CarCam60=0.40\n"
        "; Two dampeners switched far out of reach: the chase-cam latch that takes yaw away\n"
        "; from the mouse, and the speed damping of both axes. 0 = the engine's own values.\n"
        "CarCentreDelay=1000000\n"
        "CarSpeedThresh=1000000\n"
        "\n"
        "[UI]\n"
        "; HUD glyph multiplier (1.0 = off). Does not affect subtitles.\n"
        "TextScale=1.0\n"
        "\n"
        "[Debug]\n"
        "; 1 writes gf2fix.log next to this file. Diagnostics only.\n"
        "Log=0\n",
        f);
    fclose(f);
}

// Re-read on every launch and again every 2s while playing, so the in-car feel can be
// tuned without restarting.  A missing key falls back to the measured on-foot profile.
static void LoadCameraCfg()
{
    cfg.deDamp   = GetPrivateProfileIntW(L"Camera", L"DeDamp", 0, g_iniPath) != 0;
    cfg.lookGain = IniFloat(L"Camera", L"LookGain", 1.0f);
    if (cfg.lookGain < 0.1f || cfg.lookGain > 10.0f) cfg.lookGain = 1.0f;

    cfg.smooth = IniFloat(L"Camera", L"Smooth", 0.75f);
    if (cfg.smooth < 0.05f || cfg.smooth > 1.0f) cfg.smooth = 0.75f;

    cfg.rampTime = IniFloat(L"Camera", L"RampTime", 0.0f);
    if (cfg.rampTime < 0.0f || cfg.rampTime > 10.0f) cfg.rampTime = 0.0f;
    if (cfg.rampTime > 0.0f && cfg.rampTime < 1e-4f) cfg.rampTime = 1e-4f;

    static const wchar_t* const key[2] = { L"RefYawTarget", L"RefPitchTarget" };
    static const float def[2] = { 3.75f, 2.50f };
    for (int k = 0; k < 2; ++k) {
        float v = IniFloat(L"Camera", key[k], def[k]);
        cfg.ref[k] = (v > 0.0f && v < 1e4f) ? v : 0.0f;   // 0 = leave this axis alone
    }

    cfg.filterBypass = GetPrivateProfileIntW(L"Camera", L"FilterBypass", 0, g_iniPath) != 0;
    cfg.filterGain   = IniFloat(L"Camera", L"FilterGain", 1.0f);
    if (cfg.filterGain < 0.05f || cfg.filterGain > 10.0f) cfg.filterGain = 1.0f;
    // A [Debug] key, read here so it can be turned on mid-session like the rest.
    cfg.filterLog    = GetPrivateProfileIntW(L"Debug", L"FilterLog", 0, g_iniPath) != 0;

    cfg.carCam60 = IniFloat(L"Camera", L"CarCam60", 0.0f);
    if (!(cfg.carCam60 > 0.0f) || cfg.carCam60 > 1.0f) cfg.carCam60 = 0.0f;

    // 0 = leave the engine's constant alone, so a missing or unreadable ini is stock.
    cfg.carSpeedThresh = IniFloat(L"Camera", L"CarSpeedThresh", 0.0f);
    if (!(cfg.carSpeedThresh > 0.0f)) cfg.carSpeedThresh = 0.0f;
    cfg.carCentreDelay = IniFloat(L"Camera", L"CarCentreDelay", 0.0f);
    if (!(cfg.carCentreDelay > 0.0f)) cfg.carCentreDelay = 0.0f;

    cfg.carLookScale = IniFloat(L"Camera", L"CarLookScale", 0.0f);
    if (!(cfg.carLookScale > 0.0f) || cfg.carLookScale > 10.0f) cfg.carLookScale = 0.0f;

    cfg.carPitchMouse = GetPrivateProfileIntW(L"Camera", L"CarPitchMouse", 0, g_iniPath) != 0;
    cfg.carNoSmooth   = GetPrivateProfileIntW(L"Camera", L"CarNoSmooth", 0, g_iniPath) != 0;

    // Explicit Euler is stable while k*dt^2 < 4, so the ceiling below is well inside the
    // range where the spring would diverge at any frame rate the game actually runs at.
    cfg.carSpringK = IniFloat(L"Camera", L"CarSpringK", 0.0f);
    if (!(cfg.carSpringK > 0.0f)) cfg.carSpringK = 0.0f;
    if (cfg.carSpringK > 20000.0f) cfg.carSpringK = 20000.0f;

    cfg.carCentreIdle = IniFloat(L"Camera", L"CarCentreIdle", 0.0f);
    if (!(cfg.carCentreIdle > 0.0f) || cfg.carCentreIdle > 60.0f) cfg.carCentreIdle = 0.0f;
    cfg.carHoldK = IniFloat(L"Camera", L"CarHoldK", 0.0f);
    if (!(cfg.carHoldK > 0.0f) || cfg.carHoldK > 20000.0f) cfg.carHoldK = 0.0f;
    cfg.carCentreK = IniFloat(L"Camera", L"CarCentreK", 0.0f);
    if (!(cfg.carCentreK > 0.0f) || cfg.carCentreK > 20000.0f) cfg.carCentreK = 0.0f;
}

// One formatting of the [Camera] settings, shared by the load banner, the 10s dump and
// the live-reload log.  Those three sites each spelled their own format string once
// already and one of them printed garbage for an entire session.
static void CamCfgText(char* buf, size_t n)
{
    _snprintf(buf, n,
              "bypass=%d fgain=%.2f dedamp=%d smooth=%.3f gain=%.2f ramp=%.4f "
              "ref=(%.3f,%.3f) cam60=%.3f lscale=%.2f pmouse=%d nosmooth=%d spring=%.0f "
              "idle=%.2fs hold=%.0f centre=%.0f spd=%.4g delay=%.4g",
              cfg.filterBypass ? 1 : 0, cfg.filterGain,
              cfg.deDamp ? 1 : 0, cfg.smooth, cfg.lookGain, cfg.rampTime,
              cfg.ref[0], cfg.ref[1], cfg.carCam60, cfg.carLookScale,
              cfg.carPitchMouse ? 1 : 0, cfg.carNoSmooth ? 1 : 0, cfg.carSpringK,
              cfg.carCentreIdle, cfg.carHoldK, cfg.carCentreK,
              cfg.carSpeedThresh, cfg.carCentreDelay);
    buf[n - 1] = 0;
}

// Every [Camera] lever that works by rewriting engine state from inside the look
// converter hooks. One predicate, because the three call sites drifted apart once
// already and a lever that never installed its hook looks exactly like a lever that
// does not work. CarCentreIdle and the spring's frame-time cap both read the camera from
// inside the converter hooks, so they need them installed too.
static bool NeedCameraHooks()
{
    return cfg.cameraProbe || cfg.deDamp || cfg.carCam60 > 0.0f || cfg.carLookScale > 0.0f ||
           cfg.carCentreIdle > 0.0f || cfg.carSpringK > 0.0f;
}

static void LoadConfig()
{
    // Both files live next to this plugin, not next to the game exe: an ASI is normally loaded
    // from a shared plugin folder (Modern Fixes keeps its own ini in scripts\), and the shipped
    // file is deliberately named rawmouse.asi there, so keeping the config beside it is what a
    // user can find. g_hSelf is assigned by DllMain before this runs; the exe directory is only
    // a fallback for the case where the module handle was not captured.
    wchar_t dir[MAX_PATH];
    if (!g_hSelf || !GetModuleFileNameW(g_hSelf, dir, MAX_PATH)) dir[0] = 0;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    else {
        if (!GetModuleFileNameW(nullptr, dir, MAX_PATH)) dir[0] = 0;
        slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
    }
    _snwprintf(g_iniPath, MAX_PATH, L"%s\\gf2fix.ini", dir);
    _snwprintf(g_logPath, MAX_PATH, L"%s\\gf2fix.log", dir);

    WriteDefaultIniIfMissing();

    // Off by default: Modern Fixes already does borderless at the desktop resolution and
    // refresh, and its superscaling requires its own borderless mode, so both handlers on the
    // same device is a conflict. Set Enabled=1 when the loader does not do it.
    cfg.borderless  = GetPrivateProfileIntW(L"Borderless", L"Enabled", 0, g_iniPath) != 0;
    cfg.rawInput    = GetPrivateProfileIntW(L"RawInput", L"Enabled", 1, g_iniPath) != 0;
    cfg.sensitivity = IniFloat(L"RawInput", L"Sensitivity", 1.0f);
    cfg.cursorHooks = GetPrivateProfileIntW(L"RawInput", L"CursorHooks", 0, g_iniPath) != 0;
    cfg.textScale   = IniFloat(L"UI", L"TextScale", 1.0f);
    cfg.subHook     = GetPrivateProfileIntW(L"UI", L"SubtitleHook", 0, g_iniPath) != 0;
    cfg.log         = GetPrivateProfileIntW(L"Debug", L"Log", 0, g_iniPath) != 0;
    cfg.logApt      = GetPrivateProfileIntW(L"Debug", L"LogApt", 0, g_iniPath) != 0;
    cfg.logFiles    = GetPrivateProfileIntW(L"Debug", L"LogFiles", 0, g_iniPath) != 0;
    cfg.aptHooks    = GetPrivateProfileIntW(L"Debug", L"AptHooks", 0, g_iniPath) != 0;
    cfg.cameraProbe = GetPrivateProfileIntW(L"Debug", L"CameraProbe", 0, g_iniPath) != 0;
    LoadCameraCfg();
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

// ------------------------------------------------------------------ detour helper
// Another ASI (Modern Fixes) may already have inline-hooked the same export.
// A second detour over an existing one corrupts both trampolines and the game
// faults on the next call, so refuse targets that don't start like a prologue.
static bool TargetLooksHooked(const void* p)
{
    const BYTE* b = (const BYTE*)p;
    if (!b) return true;
    if (b[0] == 0xE9) return true;
    if (b[0] == 0xEB) return true;
    if (b[0] == 0xFF && b[1] == 0x25) return true;
    if (b[0] == 0x68 && b[6] == 0xC3) return true;
    return false;
}

static bool TryDetour(const char* name, void* target, void* detour, void** orig)
{
    if (!target) { Log("%s: export not found", name); return false; }
    if (TargetLooksHooked(target)) { Log("%s: %p already hooked by another mod, skipped", name, target); return false; }
    if (MH_CreateHook(target, detour, orig) != MH_OK) { Log("%s: MH_CreateHook failed at %p", name, target); return false; }
    if (MH_EnableHook(target) != MH_OK) { Log("%s: MH_EnableHook failed at %p", name, target); return false; }
    return true;
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
static volatile LONG g_gddOddElem = 0;
static volatile LONG g_gdsNative = 0;
static volatile LONG g_wmInputN = 0, g_wmMoveN = 0;
static volatile LONG g_wmInDX = 0, g_wmInDY = 0;
static volatile LONG g_lastMoveLP = 0;
// Native DirectInput counts the game actually read this second, so the look rate the
// camera ends up with can be compared against the real mouse movement behind it.
static volatile LONG g_diSumX = 0, g_diSumY = 0, g_diN = 0;
static DWORD g_inputLastReport = 0;

static void ReportInputRates()
{
    DWORD now = GetTickCount();
    if (now - g_inputLastReport < 1000) return;
    g_inputLastReport = now;
    Log("input/s: WM_INPUT=%ld (sum %+ld,%+ld) WM_MOUSEMOVE=%ld lastLP=%08lx GDS=%ld GCP=%ld raw=%d "
        "DI=%ld reads (sum %+ld,%+ld)",
        InterlockedExchange(&g_wmInputN, 0), InterlockedExchange(&g_wmInDX, 0), InterlockedExchange(&g_wmInDY, 0),
        InterlockedExchange(&g_wmMoveN, 0), (long)InterlockedExchange(&g_lastMoveLP, 0),
        g_gdsCalls, g_gcpCalls, RawAlive() ? 1 : 0,
        InterlockedExchange(&g_diN, 0), InterlockedExchange(&g_diSumX, 0),
        InterlockedExchange(&g_diSumY, 0));
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
    if (n > (LONG)(sizeof(g_mouseDevs) / sizeof(g_mouseDevs[0]))) n = sizeof(g_mouseDevs) / sizeof(g_mouseDevs[0]);
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
    LONG ox = g_ofsX, oy = g_ofsY;
    if (IsMouseDev(dev) && SUCCEEDED(hr)) {
        LONG nn = InterlockedIncrement(&g_gdsNative);
        if (nn % 600 == 1) Log("GDS native #%ld cb=%lu rawAlive=%d", nn, cbData, RawAlive() ? 1 : 0);
    }
    if (cfg.rawInput && RawAlive() && SUCCEEDED(hr) && IsMouseDev(dev) && data &&
        cbData >= (DWORD)(ox + 4) && cbData >= (DWORD)(oy + 4)) {
        LONG dx, dy; ConsumeAccum(&dx, &dy);
        *(LONG*)((BYTE*)data + ox) = dx;
        *(LONG*)((BYTE*)data + oy) = dy;
        LONG n = InterlockedIncrement(&g_gdsCalls);
        if (n % 60 == 1)
            Log("GDS #%ld cb=%lu inject dx=%ld dy=%ld btn=%02x", n, cbData, dx, dy,
                cbData >= 13 ? ((BYTE*)data)[12] : 0);
    }
    // Whatever the game ends up reading, injected or native: this is the input side of
    // the ratio against the delta the look converters actually receive.
    if (SUCCEEDED(hr) && IsMouseDev(dev) && data &&
        cbData >= (DWORD)(ox + 4) && cbData >= (DWORD)(oy + 4)) {
        InterlockedExchangeAdd(&g_diSumX, *(LONG*)((BYTE*)data + ox));
        InterlockedExchangeAdd(&g_diSumY, *(LONG*)((BYTE*)data + oy));
        InterlockedIncrement(&g_diN);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkGetDeviceData(IDirectInputDevice8W* dev, DWORD cbElem,
    LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD flags)
{
    HRESULT hr = oDIGetDeviceData(dev, cbElem, rgdod, pdwInOut, flags);
    // rgdod is indexed with the caller's element size, not ours.
    if (cbElem != sizeof(DIDEVICEOBJECTDATA) && pdwInOut && *pdwInOut) {
        if (InterlockedIncrement(&g_gddOddElem) % 600 == 1)
            Log("GetDeviceData: cbElem=%lu != %lu, leaving buffer alone", cbElem, (DWORD)sizeof(DIDEVICEOBJECTDATA));
        return hr;
    }
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

// The device vtable is shared by every instance of the class, so patch it once.
// Re-patching it for a second device would store our own detour as the "original"
// and recurse until the stack dies.
static void* g_diVtablePatched = nullptr;
static void HookMouseVtable(void* dev)
{
    void** vt = *reinterpret_cast<void***>(dev);
    if (g_diVtablePatched == vt) return;
    // Verified against the SDK vtbl layout with offsetof (see vidx_c.c):
    // GetDeviceState=9 GetDeviceData=10 SetDataFormat=11; IDirectInput8 CreateDevice=3.
    if (vt[9] == (void*)hkGetDeviceState) { Log("DI vtable %p already patched elsewhere", vt); return; }
    oDISetDataFormat = (DID8_SetDataFormat_t)VTableHook(dev, 11, (void*)hkSetDataFormat);
    oDIGetDeviceState = (DID8_GetDeviceState_t)VTableHook(dev, 9, (void*)hkGetDeviceState);
    oDIGetDeviceData  = (DID8_GetDeviceData_t)VTableHook(dev, 10, (void*)hkGetDeviceData);
    g_diVtablePatched = vt;
    Log("DirectInput device vtable %p patched (GDS/GDD/SetDataFormat)", vt);
}

static HRESULT STDMETHODCALLTYPE hkDICreateDevice(IDirectInput8W* di, REFGUID rguid,
    LPDIRECTINPUTDEVICE8W* devOut, LPUNKNOWN unk)
{
    HRESULT hr = oDICreateDevice(di, rguid, devOut, unk);
    if (SUCCEEDED(hr) && devOut && *devOut && rguid == GUID_SysMouse) {
        LONG i = InterlockedIncrement(&g_mouseDevCount);
        if (i <= 8) g_mouseDevs[i - 1] = *devOut;
        HookMouseVtable(*devOut);
        Log("DirectInput mouse device %p created (total %ld)", *devOut, g_mouseDevCount);
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

// ==================================================================
// Mouse-look probe (in-car camera diagnosis)
// ==================================================================
// Both look axes funnel through two sibling leaf functions that convert the
// control-map delta into a camera yaw/pitch offset.  They share a "look rig":
//   +0x00 yaw rest    +0x04 yaw target    +0x08 pitch rest   +0x0c pitch target
//   +0x10 look time constant - the target is lerped in from rest with
//        alpha = (time spent looking)/this, and the accumulator (on the camera at
//        +0x74/+0x78, not on the rig) resets the moment |delta| drops to the 0.9
//        deadzone, which is what springs the view back to the rest angle.
//   +0x20 yaw scale   +0x24 pitch scale
// The camera object also contributes [self+0x60], a blend weight that decays from
// 1.00 to ~0.40 over ~10s after you get in a car.
static struct LookRig {
    void* rig;  void* self;  int axis;  int slot;  int dead;
    float last[8];
    float wrote[2];             // the two look targets we last wrote, so we recognise our own
    int   miss;                 // consecutive unreadable polls before a slot is retired
    DWORD lastLog;  int haveLast;
    DWORD tick;
    volatile LONG raised;
    volatile LONG calls;
} g_rigs[16];
static volatile LONG g_rigN      = 0;
static volatile LONG g_axisCalls[2] = { 0, 0 };
static volatile LONG g_rejectN[2]   = { 0, 0 };
static void*         g_lastRig[2]   = { nullptr, nullptr };
static void*         oLookX = nullptr;
static void*         oLookY = nullptr;
static bool          g_camTried = false;

// Offsets of the seven rig floats the probe reports: yaw rest/target, pitch
// rest/target, time constant, yaw scale, pitch scale.
static const int g_rigOff[7] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x20, 0x24 };

// The converter's tail, verified by disassembly, is
//     *out = wrap(prev + wrap(desired - prev) * rig[scale] * [self+0x60] * arg4)
// with desired = lerp(rest, target, alpha) * XSens * deg2rad * delta.  So rig+0x20/+0x24
// are a LOW-PASS COEFFICIENT on the per-frame turn rate, not a sensitivity gain.  On foot
// it is 0.75 * 1.00 = 0.75; in a car 0.50 * ~0.40 = ~0.20, so the view closes only a
// fifth of the gap to where the mouse says it should be, every frame.  That is the
// damping - and it also explains why raising the scale instead made things worse: at
// k = 3.0 the recurrence becomes out = 3*desired - 2*prev, which overshoots and
// oscillates forever ("bounces in spot" at high DPI).
//
// Two consequences.  The coefficient has to be written as Smooth/[self+0x60], recomputed
// on every call because the blend weight decays over ~10s, and it must never exceed 1.0.
// Sensitivity is a separate lever - the look targets at +0x04/+0x0c, in degrees per mouse
// count - which is also the only lever the converter's alternate branch has, since that
// one computes out = target * deg2rad * delta and reads neither scale nor [self+0x60].
//
// The engine re-authors the rig on EVERY call, so a value only sticks if it is written
// inside the hook, immediately before the converter reads it.  Patching the rig from a
// background poll instead changes nothing the player can feel.
//
// The two rest ends (+0x00/+0x08) are live per-call state rather than parameters and are
// left alone, as is [self+0x60] itself - it is the camera blend weight the engine needs,
// so its deficit is cancelled through the scale instead of overwritten.
static const int g_scaleOff[2]  = { 0x20, 0x24 };
static const int g_targetOff[2] = { 0x04, 0x0c };

static bool SafeF(const void* base, int off, float* out)
{
    __try { *out = *(const float*)((const char*)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return *out <= 1e12f && *out >= -1e12f;   // also rejects NaN
}

static bool SafeInt(const void* base, int off, int* out)
{
    __try { *out = *(const int*)((const char*)base + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static bool SafeWriteF(void* base, int off, float v)
{
    __try { *(float*)((char*)base + off) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// A value no rig field can legitimately hold: marks "we have not written this".
static float Never()
{
    static const unsigned u = 0x7FC00000u;
    return *(const float*)&u;
}

static float AsFloat(unsigned u) { return *(const float*)&u; }

static volatile LONG g_forceN = 0;
static DWORD      g_forceLogAt = 0;
static volatile LONG g_cam60N = 0;
static DWORD      g_cam60LogAt = 0;
static DWORD      g_argLogAt[2] = { 0, 0 };
static unsigned   g_lastArgs[2][5] = { { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 } };
// Whether the CarPitchMouse code patch at 0x6C6828 is currently installed. Written by
// ApplyPitchGate, read by the diagnostics so the log says which branch pitch took.
static bool       g_pitchGateOn = false;

// Hold this call's smoothing coefficient at cfg.smooth and raise the look target to the
// on-foot profile.  Runs inside the hook, immediately before the converter reads the rig.
// r is only used for the diagnostics counters and may be null.
static void CamDeDamp(int axis, void* rig, void* self, LookRig* r)
{
    // ApplyLookDivisor has already forced cam+0x60 if CarCam60 is set, so this reads the
    // value the converter is actually going to see. The quotient below therefore lands on
    // k = Smooth exactly. It used to be read after a local cam+0x60 write, which made the
    // two levers multiply and pushed k to 1.0/0.10 = 10.0, four times past its stability
    // limit.
    float c60 = 0.0f;
    bool have60 = self && SafeF(self, 0x60, &c60) && c60 > 1e-4f;

    if (!cfg.deDamp) return;

    float scale = 0.0f, wantS = 0.0f, target = 0.0f, wantT = 0.0f;
    bool didS = false, didT = false;

    // Written unconditionally, for two reasons: the blend weight decays over ~10s so the
    // quotient has to be recomputed, and the engine authors the scale once per camera
    // change and then leaves it alone - without this, Smooth could never be lowered live.
    // A weight above 1.0 is not clamped: dividing by it is still the correct way to land
    // the product on Smooth, and clamping would push the coefficient past 1.0.
    if (have60) {
        wantS = cfg.smooth / c60;
        if (wantS > 200.0f) wantS = 200.0f;
        if (SafeF(rig, g_scaleOff[axis], &scale) && scale != wantS &&
            SafeWriteF(rig, g_scaleOff[axis], wantS))
            didS = true;
    }

    // Raise-only unless the value is our own last write, so a cutscene or map camera that
    // is already freer keeps its own feel and LookGain can still be dialled down live.
    if (cfg.ref[axis] > 0.0f && SafeF(rig, g_targetOff[axis], &target)) {
        wantT = cfg.ref[axis] * cfg.lookGain;
        bool owned = r && r->wrote[axis] == target;
        if (target != wantT && (target < wantT || owned) &&
            SafeWriteF(rig, g_targetOff[axis], wantT)) {
            didT = true;
            if (r) r->wrote[axis] = wantT;
        }
    }

    if (cfg.rampTime > 0.0f) SafeWriteF(rig, 0x10, cfg.rampTime);

    if (!didS && !didT) return;
    if (r) r->raised++;
    LONG n = InterlockedIncrement(&g_forceN);
    // Hundreds of calls a second, so the log is gated by time, not by count.
    if ((long)(GetTickCount() - g_forceLogAt) > 2000) {
        g_forceLogAt = GetTickCount();
        Log("CAM dedamp axis=%d rig=%p self=%p scale %.4f -> %.4f (cam60 %.4f, k=%.3f) "
            "target %.4f -> %.4f (gain %.2f) total %ld",
            axis, rig, self, scale, wantS, c60, (didS ? wantS : scale) * c60,
            target, wantT, cfg.lookGain, (long)n);
    }
}

// ------------------------------------------------------- frame-rate look divisor
// THE in-car sluggishness. Both vehicle look integrators scale the mouse delta by
//
//     delta / divisor * dt          divisor = [0x112a6f0] yaw, [0x112a7a8] pitch
//
// (yaw at 0x6C2031 inside 0x6C1DE0; pitch is 0x6C8900, called from 0x6C6A52). Both
// divisors are written at init as `1.0 / [0x110ae4c]`, and **[0x110ae4c] = 59.94** - a
// hardcoded NTSC console frame rate. So the factor is `dt * 59.94`, i.e. exactly 1.0 at
// 59.94fps and 0.45 at the ~150fps this machine runs: the car throws away more than half
// of every mouse count purely because the frame rate is high. It also explains
// [cam+0x60], which 0x6C85E0 computes as clamp(dt/divisor, 0, 1) - measured asymptote
// 0.395 == 0.0066 * 59.94 - and which then scales every 0x6C89B0 lerp in the camera.
//
// The on-foot path does NOT have this: 0x6D9108 passes the *constant* [0xd5f374] = 1/30
// instead of the live dt, so its factor is 1.998 at any frame rate. That asymmetry is the
// whole complaint - the car is ~4.4x heavier than on foot at 150fps.
//
// Fix: rewrite both divisors to `dt / CarLookScale` on every vehicle converter call,
// using the same dt the integrator is about to multiply by ([cam+0x2cc], the converter's
// arg3). The dt cancels, so the factor becomes CarLookScale exactly, at any frame rate.
// 1.0 is the engine's own design value at 59.94fps; 1.998 would match on foot.
//
// Which paths have the bug, and the discriminator. There are exactly three places that
// call the converters, and they differ in what they feed 0x6C8900 as `dt`:
//
//   car      0x6C622D / 0x6C6A3A   fld [esi+0x2cc]   live dt   -> BUGGED, rig = self+0xC0
//   third    0x6E28F3 / 0x6E2919   fld [ebp+8]       live dt   -> BUGGED, rig = self+0x1BC
//   on foot  0x6D908D / 0x6D90BA   fld [0xd5f374]    CONST 1/30 -> immune, rig on the stack
//
// So the fix is keyed on the CONVERTER RETURN ADDRESS, not on the rig. The first version
// tested `rig == self+0xC0`, which matched only the car and silently left the third
// camera on the stock divisor - that is the "reversing slows the camera down again"
// report: reverse uses the third camera, whose rig sits at self+0x1BC.
//
// The on-foot path must stay on the stock divisor: it integrates with the constant 1/30,
// so feeding it our dt-derived value would multiply walking look by 59.94/30 = 2.0.
static const unsigned kOnFootRva[2] = { 0x2D908D, 0x2D90BA };
static uintptr_t    g_exeBase     = 0;
static unsigned int g_divVa[2]  = { 0x112a6f0, 0x112a7a8 };
static float      g_divStock[2] = { 0.0f, 0.0f };
static float      g_divLast[2]  = { -1.0f, -1.0f };
static bool         g_divOver     = false;   // we are currently holding the divisor off stock
static unsigned   g_siteSeen[8];
static LONG         g_siteN       = 0;

static void WriteDivisor(int i, float want)
{
    if (g_divLast[i] == want) return;
    if (SafeWriteF((void*)g_divVa[i], 0, want)) g_divLast[i] = want;
    else { cfg.carLookScale = 0.0f; Log("CAM divisor write failed, disabled"); }
}

static void ApplyLookDivisor(unsigned siteRva, void* self, float dt)
{
    if (cfg.carLookScale <= 0.0f && !g_divOver) return;   // off, and nothing to undo
    // Latched before the first write, so a non-vehicle call can put the engine's own
    // values back. They are 0 in the image and authored at runtime as 1/[0x110ae4c].
    for (int i = 0; i < 2; ++i) {
        if (g_divStock[i] != 0.0f) continue;
        float v = 0.0f;
        if (SafeF((void*)g_divVa[i], 0, &v) && v > 1e-6f && v < 1.0f) g_divStock[i] = v;
    }
    if (g_divStock[0] == 0.0f || g_divStock[1] == 0.0f) return;

    bool vehicle = cfg.carLookScale > 0.0f &&
                   siteRva != kOnFootRva[0] && siteRva != kOnFootRva[1];
    if (vehicle && (!(dt > 1e-5f) || !(dt < 1.0f))) vehicle = false;   // NaN/absurd guard
    float want = vehicle ? dt / cfg.carLookScale : 0.0f;
    WriteDivisor(0, vehicle ? want : g_divStock[0]);
    WriteDivisor(1, vehicle ? want : g_divStock[1]);
    g_divOver = vehicle;

    // Side effect of shrinking the divisor: 0x6C85E0 computes cam+0x60 = clamp(dt/pdiv, 0, 1),
    // so pdiv -> dt/CarLookScale drives cam+0x60 up to 1.0. That field multiplies every
    // 0x6C89B0/0x6C8940 camera lerp (cur + (target-cur)*cam60*k), so at 1.0 the camera keeps
    // no smoothing at all and snaps onto its target each frame - which reads as jitter.
    // CarCam60 puts the stock value back (~0.40 in a car), but only on the paths we scaled:
    // on foot the engine authors 1.00 and must keep it. Written every call because 0x6C85E0
    // re-authors it once per frame.
    if (cfg.carCam60 > 0.0f && vehicle && self) {
        float had = 0.0f;
        if (SafeF(self, 0x60, &had) && had != cfg.carCam60 &&
            SafeWriteF(self, 0x60, cfg.carCam60)) {
            LONG n = InterlockedIncrement(&g_cam60N);
            if ((long)(GetTickCount() - g_cam60LogAt) > 2000) {
                g_cam60LogAt = GetTickCount();
                Log("CAM car60 site=0x%x self=%p cam60 %.4f -> %.4f (writes %ld)",
                    siteRva, self, had, cfg.carCam60, (long)n);
            }
        }
    }

    // One line per unseen call site, so a camera mode we have not identified yet shows up
    // in the log with its verdict instead of silently staying on the stock divisor.
    if (cfg.cameraProbe && siteRva != 0) {
        bool known = false;
        for (LONG i = 0; i < g_siteN && i < 8; ++i) if (g_siteSeen[i] == siteRva) { known = true; break; }
        if (!known && g_siteN < 8) {
            g_siteSeen[g_siteN++] = siteRva;
            Log("CAM divisor site rva=0x%x -> %s (dt=%.6f)", siteRva,
                vehicle ? "SCALED" : "stock", dt);
        }
    }

    static DWORD at = 0;
    if (vehicle && (long)(GetTickCount() - at) > 2000) {
        at = GetTickCount();
        Log("CAM divisor site=0x%x dt=%.6f scale=%.3f -> %.6f (stock %.6f, factor %.3f was %.3f)",
            siteRva, dt, cfg.carLookScale, want, g_divStock[0], cfg.carLookScale,
            dt / g_divStock[0]);
    }
}

static bool PlausiblePtr(const void* p)
{
    uintptr_t a = (uintptr_t)p;
    return a >= 0x00010000u && a < 0x7ff00000u && (a & 3) == 0;
}

// Both converters start by asking 0x410eb0 which gameplay context is active, and take a
// completely different tail when it says yes: out = target * deg2rad * delta, reading
// neither scale nor [self+0x60].  Which branch runs therefore decides whether Smooth or
// LookGain is the effective lever, so it is worth knowing.  The query is pure table
// chasing - context byte at ctx+0x52c, slot byte at ctx+0x567, handler at
// [[0x113d294]] + slot*4 + 0x44, taken when [handler+4] == 2 - so replicate it read-only
// rather than hooking a third function.  Diagnostic only: it gates nothing, and the
// addresses are this exe's, so a failure just reports -1.
static int LookBranch()
{
    __try {
        const char* ctx = *(const char**)0x12233b4;
        if (!PlausiblePtr(ctx) || !*(const unsigned char*)(ctx + 0x52c)) return 0;
        unsigned slot = *(const unsigned char*)(ctx + 0x567);
        if (slot == 0x12) return 0;
        const char* mgr = *(const char**)0x113d294;
        if (!PlausiblePtr(mgr)) return 0;
        const char* base = *(const char**)mgr;
        if (!PlausiblePtr(base)) return 0;
        const char* h = *(const char**)(base + slot * 4 + 0x44);
        if (!PlausiblePtr(h)) return 0;
        return *(const int*)(h + 4) == 2 ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// A look rig is the struct the converter lerps: a positive time constant at +0x10
// and at least one usable scale float at +0x20/+0x24.
static bool RigSane(const void* rig, float* out)
{
    float range, sx, sy;
    if (!SafeF(rig, 0x10, &range) || range <= 1e-6f || range > 1e6f) return false;
    if (!SafeF(rig, 0x20, &sx) || !SafeF(rig, 0x24, &sy)) return false;
    if (sx < -1e6f || sx > 1e6f || sy < -1e6f || sy > 1e6f) return false;
    if (sx > -1e-9f && sx < 1e-9f && sy > -1e-9f && sy < 1e-9f) return false;
    out[0] = range; out[1] = sx; out[2] = sy;
    return true;
}

// Per-axis totals for the current second.  Sampling one call every 2s cannot say whether
// the camera is getting the mouse movement, because most frames of a flick are quiet; the
// sums can.  Fixed point so the accumulation stays atomic.
static volatile LONG g_lookSum[2] = { 0, 0 };   // sum of delta, in 1e-4 units
static volatile LONG g_lookAbs[2] = { 0, 0 };   // sum of |delta|, in 1e-4 units
static volatile LONG g_lookN[2]   = { 0, 0 };
static void* g_lastSelf[2] = { nullptr, nullptr };
static DWORD g_lookRateAt[2] = { 0, 0 };

// Phase tracking for CarCentreIdle. NOT the camera's own movement accumulators at
// cam+0x74/0x78: those count up only while the converter's delta is above the engine's 0.9
// per-frame gate, and a real turn measures about 0.11 per frame (28 units in a second over
// 150 frames), so the fix #7 log shows them sitting at zero even mid-flick - the car takes
// the direct converter branch that ignores that gate entirely. The delta itself is the
// signal instead, against a threshold well clear of both ends: idle seconds measured
// |delta|/s of 0.05 (about 0.0003 per frame) against 5-28 while steering, so 0.004 is ten
// times the noise floor and eight times below the slowest deliberate look. It is a value in
// the engine's own units, after its input normaliser, so it does not move with mouse DPI.
// car+0x2bc says whether the camera asking is a chase camera at all. Both converters are
// called every frame whether or not the mouse moves, so these stamps are one frame stale at
// worst.
static volatile LONG g_lastSteer = 0;
static volatile LONG g_carSeen   = 0;
// The converter's dt argument in microseconds, so the spring can be capped by the frame time
// it is actually being integrated with. Fixed point because an atomic float is a nuisance.
static volatile LONG g_dtMicro   = 0;

// Everything between the converter's output and the actual view.  The converter is
// provably fine in the car (it asks for up to 280 deg/s there), so if the view still turns
// slowly the loss is in here: 0x6C8900/0x6C89B0 integrate and decay the pitch through
// ydiv/pdiv and cam+0x60, the global look object at 0x112a700 carries the yaw the
// gameplay branch recomputes from the vehicle's facing every frame, and cam+0x288..0x290
// is the smoothed vehicle orientation the chase camera is pulled toward.  Read-only.
static void DumpLookState(int axis)
{
    float ydiv = 0, pdiv = 0, e10 = 0, e0c = 0, e14 = 0, e44 = 0, e6e8 = 0;
    SafeF((void*)0x112a6f0, 0, &ydiv);
    SafeF((void*)0x112a7a8, 0, &pdiv);
    SafeF((void*)0xe50e10, 0, &e10);
    SafeF((void*)0xe50e0c, 0, &e0c);
    SafeF((void*)0xe50e14, 0, &e14);
    SafeF((void*)0xe50e44, 0, &e44);
    SafeF((void*)0x112a6e8, 0, &e6e8);

    const void* L = (const void*)0x112a700;
    float f34 = 0, f40 = 0, f44 = 0, f48 = 0, f4c = 0, f50 = 0, f54 = 0, f58 = 0;
    float f60 = 0, f68 = 0, f6c = 0, f70 = 0;
    int b30 = 0, b38 = 0, b3a = 0;
    SafeF(L, 0x34, &f34); SafeF(L, 0x40, &f40); SafeF(L, 0x44, &f44); SafeF(L, 0x48, &f48);
    SafeF(L, 0x4c, &f4c); SafeF(L, 0x50, &f50); SafeF(L, 0x54, &f54); SafeF(L, 0x58, &f58);
    SafeF(L, 0x60, &f60); SafeF(L, 0x68, &f68); SafeF(L, 0x6c, &f6c); SafeF(L, 0x70, &f70);
    SafeInt(L, 0x30, &b30); SafeInt(L, 0x38, &b38); SafeInt(L, 0x3a, &b3a);
    Log("LOOKOBJ ydiv=%.6f pdiv=%.6f e10=%.4f e0c=%.4f e14=%.4f e44=%.4f e6e8=%.4f | "
        "+30=%d +34=%.5f +38=%d +3a=%d axis=(%.4f,%.4f,%.4f) yaw=%.5f +50=%.5f +54=%.5f "
        "+58=%.5f +60=%.5f +68=%.5f +6c=%.5f +70=%.5f",
        ydiv, pdiv, e10, e0c, e14, e44, e6e8,
        b30 & 0xff, f34, b38 & 0xff, b3a & 0xff, f40, f44, f48, f4c, f50, f54,
        f58, f60, f68, f6c, f70);

    void* self = g_lastSelf[axis];
    void* rig  = g_lastRig[axis];
    float c60 = 0, pacc = 0, f288 = 0, f28c = 0, f290 = 0, r94 = 0, r9c = 0, sacc = 0;
    int b2bc = -1;
    if (self) {
        SafeF(self, 0x60, &c60);   SafeF(self, 0x284, &pacc);
        SafeF(self, 0x288, &f288); SafeF(self, 0x28c, &f28c); SafeF(self, 0x290, &f290);
        SafeF(self, 0x2f0, &sacc);
        SafeInt(self, 0x2bc, &b2bc);
    }
    if (rig) { SafeF(rig, 0x94, &r94); SafeF(rig, 0x9c, &r9c); }
    // Live values of the two overridden constants, so the log proves the write landed.
    float fa8 = 0;
    SafeF((void*)0xe50fa8, 0, &fa8);
    Log("LOOKCAM self=%p rig=%p cam60=%.4f +2bc=%d pitchAcc=%.5f follow=(%.4f,%.4f,%.4f) "
        "rest=(%.4f,%.4f) spdAcc=%.4f spdThresh=%.6g",
        self, rig, c60, b2bc & 0xff, pacc, f288, f28c, f290, r94, r9c, sacc, fa8);

    // Everything the pitch branch at 0x6C6825 depends on, so the log can say which side
    // of it a given second was on. +0x270/+0x278 are the pitch-lock flags (either zeroes
    // +0x280 and aims at rig+0x94/+0x9c instead of the mouse), +0x204/+0x2f8 are the
    // frame stamp and window that force the mouse path for a while after a camera change,
    // and rig+0xf0/+0xf4 are the pitch limits the auto path scales by cam+0x298.
    float dt = 0, f298 = 0, rf0 = 0, rf4 = 0;
    int b270 = -1, b278 = -1, i204 = 0, i2f8 = 0;
    if (self) {
        SafeF(self, 0x2cc, &dt); SafeF(self, 0x298, &f298);
        SafeInt(self, 0x270, &b270); SafeInt(self, 0x278, &b278);
        SafeInt(self, 0x204, &i204); SafeInt(self, 0x2f8, &i2f8);
    }
    if (rig) { SafeF(rig, 0xf0, &rf0); SafeF(rig, 0xf4, &rf4); }
    Log("LOOKPITCH self=%p dt=%.6f pdiv=%.6f factor=%.3f gate=%s +270=%d +278=%d +204=%d "
        "+2f8=%d +298=%.4f lim=(%.2f,%.2f)",
        self, dt, pdiv, pdiv > 1e-9f ? dt / pdiv : 0.0f, g_pitchGateOn ? "FORCED" : "stock",
        b270 < 0 ? -1 : (b270 & 0xff), b278 < 0 ? -1 : (b278 & 0xff), i204, i2f8, f298,
        rf0, rf4);
}

static void LookRate(int axis)
{
    DWORD now = GetTickCount();
    if ((long)(now - g_lookRateAt[axis]) < 1000) return;
    g_lookRateAt[axis] = now;
    long sum  = InterlockedExchange(&g_lookSum[axis], 0);
    long absm = InterlockedExchange(&g_lookAbs[axis], 0);
    long n    = InterlockedExchange(&g_lookN[axis], 0);
    void* self = g_lastSelf[axis];
    void* rig  = g_lastRig[axis];
    float tgt = 0.0f;
    int flag = -1;
    if (rig)  SafeF(rig, g_targetOff[axis], &tgt);
    // [cam+0x2bc] is the vehicle flag.  It gates the block after the converter call that
    // maintains cam+0x2f0, and 0x6C5740 branches on it to take the chase-camera follow
    // target from the vehicle ([cam+0xe8]) instead of from a script query - which is why
    // the camera re-centres on the car's heading rather than holding the mouse's.
    if (self) SafeInt(self, 0x2bc, &flag);
    // Both axes' converter call counts for the current 10s window, so a second in which the
    // pitch converter stops being called at all shows up as a flat y= between two lines.
    Log("LOOK rate axis=%d calls=%ld (win x=%ld y=%ld) delta/s=%+.2f |delta|/s=%.2f deg/s=%+.1f |deg|/s=%.1f "
        "tgt=%.3f self=%p +2bc=%d",
        axis, n, (long)g_axisCalls[0], (long)g_axisCalls[1],
        sum / 1e4, absm / 1e4, sum / 1e4 * tgt, absm / 1e4 * tgt, tgt, self,
        flag < 0 ? -1 : (flag & 0xff));
    if (axis == 0) DumpLookState(0);
}

// f points at the EFLAGS slot written by the probe stub: f[-1..-8] =
// EAX ECX EDX EBX oldESP EBP ESI EDI, f[1] = return address, f[2..] = stack args.
// Which register/argument carries the rig differs between the two converters, so
// the frame is searched instead of trusting one fixed slot.
static void __cdecl CamProbe(int axis, unsigned* f)
{
    InterlockedIncrement(&g_axisCalls[axis]);
    if (cfg.cameraProbe) {
        long fx = (long)(AsFloat(f[4]) * 1e4f);
        InterlockedExchangeAdd(&g_lookSum[axis], fx);
        InterlockedExchangeAdd(&g_lookAbs[axis], fx < 0 ? -fx : fx);
        InterlockedIncrement(&g_lookN[axis]);
        LookRate(axis);
    }
    static const int slotOf[6] = { 2, 3, -2, -1, -8, -6 };
    for (int s = 0; s < 6; ++s) {
        void* cand = (void*)f[slotOf[s]];
        float v[3];
        if (!PlausiblePtr(cand) || !RigSane(cand, v)) continue;
        g_lastRig[axis] = cand;
        void* self = PlausiblePtr((void*)f[-2]) ? (void*)f[-2] : nullptr;
        g_lastSelf[axis] = self;

        if (cfg.carSpringK > 0.0f || cfg.carCentreIdle > 0.0f) {
            float dt = AsFloat(f[5]);
            if (dt > 0.0002f && dt < 0.2f) InterlockedExchange(&g_dtMicro, (LONG)(dt * 1e6f));
        }
        if (cfg.carCentreIdle > 0.0f && self) {
            float d = AsFloat(f[4]);
            if (d < 0.0f) d = -d;
            if (d > 0.004f) InterlockedExchange(&g_lastSteer, (LONG)GetTickCount());
            int veh = 0;
            if (SafeInt(self, 0x2bc, &veh) && (veh & 0xff))
                InterlockedExchange(&g_carSeen, (LONG)GetTickCount());
        }

        // Log the whole call signature before anything is written.  Decoded against the
        // disassembly the five stack args are (rig, out*, delta, dt, extra), and arg1 is
        // the real output slot for this call - better than guessing self+0xac/self+0x27c.
        // branch and k are what actually decide how the call feels, so they are first.
        if (cfg.cameraProbe) {
            DWORD nowA = GetTickCount();
            // The yaw converter runs once per frame whether or not the mouse moves, so
            // only sample a call whose arguments differ from the previous one: that is a
            // frame where something is actually being looked at.
            unsigned* a = &f[2];
            bool moved = false;
            for (int q = 0; q < 5; ++q) if (a[q] != g_lastArgs[axis][q]) { moved = true; break; }
            for (int q = 0; q < 5; ++q) g_lastArgs[axis][q] = a[q];
            if (moved && (long)(nowA - g_argLogAt[axis]) > 2000) {
                g_argLogAt[axis] = nowA;
                uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
                float rest = 0.0f, tgt = 0.0f, c60 = 0.0f, acc = 0.0f, out = 0.0f;
                SafeF(cand, axis ? 0x08 : 0x00, &rest);
                SafeF(cand, g_targetOff[axis], &tgt);
                if (self) {
                    SafeF(self, 0x60, &c60);
                    SafeF(self, axis ? 0x78 : 0x74, &acc);   // the ramp accumulator
                }
                void* op = (void*)f[3];
                if (PlausiblePtr(op)) SafeF(op, 0, &out);
                float sc = axis ? v[2] : v[1];
                Log("CAM call axis=%d site=%08x(rva=0x%x) branch=%d k=%.4f self=%p rig=%p "
                    "delta=%.5f dt=%.5f extra=%.5f out=%.5f acc=%.4f "
                    "rig=(rest %.3f tgt %.3f time %.4f scale %.4f) cam60=%.4f",
                    axis, f[1], (unsigned)((uintptr_t)f[1] - exe), LookBranch(), sc * c60,
                    self, cand, AsFloat(f[4]), AsFloat(f[5]), AsFloat(f[6]), out, acc,
                    rest, tgt, v[0], sc, c60);
            }
        }

        // Diagnostics table lookup.  Scan every slot, not just the first g_rigN: a slot
        // retired as unreadable can come back, and a narrower bound used to hand the
        // same rig address a second slot on every call.
        LookRig* r = nullptr;
        for (LONG i = 0; i < 16; ++i)
            if (!g_rigs[i].dead && g_rigs[i].rig == cand) { r = &g_rigs[i]; break; }

        // The fix runs on the rig the engine is about to read, whether or not the table
        // has a slot for it - tracking is diagnostics only and must never gate the fix.
        if (!g_exeBase) g_exeBase = (uintptr_t)GetModuleHandleW(nullptr);
        ApplyLookDivisor((unsigned)((uintptr_t)f[1] - g_exeBase), self, AsFloat(f[5]));
        CamDeDamp(axis, cand, self, r);

        if (!r) {
            LONG i;
            if (g_rigN < 16) {
                i = InterlockedIncrement(&g_rigN) - 1;
            } else {
                // Some call sites build the rig as a fresh stack struct every frame, so
                // the table would fill with one-address-wonders and stop tracking the
                // persistent cameras.  Retire the least recently seen.
                i = -1;
                DWORD bestTick = 0xFFFFFFFFu;
                for (LONG k = 0; k < 16; ++k) {
                    if (g_rigs[k].dead) { i = k; break; }
                    if (g_rigs[k].tick < bestTick) { bestTick = g_rigs[k].tick; i = k; }
                }
                if (i < 0) return;
            }
            r = &g_rigs[i];
            r->rig = cand; r->self = self; r->axis = axis; r->slot = slotOf[s];
            r->dead = 0; r->miss = 0; r->raised = 0; r->haveLast = 0; r->calls = 0;
            r->wrote[0] = r->wrote[1] = Never();
            r->tick = GetTickCount();
            // A recycled stack address re-registers many times a second, so registration
            // is logged at most once every 2s per slot to keep the file readable.
            DWORD now = GetTickCount();
            if (r->lastLog == 0 || (long)(now - r->lastLog) > 2000) {
                r->lastLog = now;
                Log("LOOK rig#%ld axis=%d slot=%d rig=%p self=%p time=%.4f scale=%.4f/%.4f",
                    (long)i, axis, slotOf[s], cand, r->self, v[0], v[1], v[2]);
            }
        } else if (self) {
            r->self = self;
        }
        r->tick = GetTickCount();
        InterlockedIncrement(&r->calls);
        return;
    }
    LONG k = InterlockedIncrement(&g_rejectN[axis]);
    if (k <= 3)
        Log("CAM probe axis=%d no rig in frame #%ld EAX=%08x ECX=%08x EDX=%08x EBX=%08x "
            "ESI=%08x EDI=%08x EBP=%08x ret=%08x args=%08x %08x %08x %08x %08x",
            axis, (long)k, f[-1], f[-2], f[-3], f[-4], f[-7], f[-8], f[-6], f[1],
            f[2], f[3], f[4], f[5], f[6]);
}

// The stub only peeks at the frame, then continues into the trampoline with every
// register and flag exactly as the game left it.
void __declspec(naked) lookHookX(void)
{
    __asm {
        pushfd
        pushad
        lea eax, [esp+0x20]      // -> saved EFLAGS slot
        push eax                 // cdecl args go in right-to-left: frame first...
        push 0                   // ...axis last, so it lands at [esp+4]   (axis 0 = X)
        call CamProbe
        add  esp, 8
        popad
        popfd
        mov eax, oLookX
        jmp eax
    }
}

void __declspec(naked) lookHookY(void)
{
    __asm {
        pushfd
        pushad
        lea eax, [esp+0x20]
        push eax
        push 1                   // axis 1 = Y
        call CamProbe
        add  esp, 8
        popad
        popfd
        mov eax, oLookY
        jmp eax
    }
}

static uintptr_t g_codeLo, g_codeHi;
static bool FindCodeSection(uintptr_t base)
{
    IMAGE_DOS_HEADER* dh = (IMAGE_DOS_HEADER*)base;
    if (dh->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dh->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (DWORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sh) {
        if (!(sh->Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sh->Misc.VirtualSize) continue;
        g_codeLo = base + sh->VirtualAddress;
        g_codeHi = g_codeLo + sh->Misc.VirtualSize;
        return true;
    }
    return false;
}

static bool HasBytes(uintptr_t a, const unsigned char* b, int n)
{
    for (int i = 0; i < n; ++i) if (*(unsigned char*)(a + i) != b[i]) return false;
    return true;
}

static void TryHookCamera()
{
    if (g_camTried) return;
    g_camTried = true;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!FindCodeSection(base)) { Log("CAM: no executable section"); return; }

    // prologue: movss xmm0,[esp+0xc] / movss xmm1,[abs]
    static const unsigned char pro[] = { 0xF3,0x0F,0x10,0x44,0x24,0x0C, 0xF3,0x0F,0x10,0x0D };
    static const unsigned char mX[]  = { 0xF3,0x0F,0x10,0x47,0x04 };   // movss xmm0,[edi+4]  -> yaw
    static const unsigned char mY[]  = { 0xF3,0x0F,0x10,0x47,0x0C };   // movss xmm0,[edi+0xc]-> pitch
    struct Cand { uintptr_t addr; int axis; int starts; };
    Cand cand[12]; int n = 0;

    for (uintptr_t a = g_codeLo; a + 16 < g_codeHi; ++a) {
        if (!HasBytes(a, pro, 10)) continue;
        uintptr_t end = a + 8;
        while (end + 3 < g_codeHi && !HasBytes(end, (const unsigned char*)"\xCC\xCC\xCC", 3)) ++end;
        int axis = -1;
        for (uintptr_t b = a; b + 5 < end; ++b) {
            if (axis < 0 && HasBytes(b, mX, 5)) axis = 0;
            if (axis < 0 && HasBytes(b, mY, 5)) axis = 1;
        }
        if (axis < 0) { a = end; continue; }
        // A real function start is preceded by the int3 padding between functions;
        // a match inside a function body means we would be hijacking arbitrary code.
        int starts = (a > g_codeLo && *(unsigned char*)(a - 1) == 0xCC);
        if (n < 12) { cand[n].addr = a; cand[n].axis = axis; cand[n].starts = starts; ++n; }
        a = end;
    }
    for (int i = 0; i < n; ++i)
        Log("CAM cand#%d=%p rva=0x%x axis=%d funcStart=%d", i, (void*)cand[i].addr,
            (unsigned)(cand[i].addr - base), cand[i].axis, cand[i].starts);
    if (!n) { Log("CAM: no look functions found"); return; }

    for (int axis = 0; axis < 2; ++axis) {
        int pick = -1;
        for (int i = 0; i < n; ++i) {
            if (cand[i].axis != axis) continue;
            if (pick < 0 || cand[i].starts) { pick = i; if (cand[i].starts) break; }
        }
        if (pick < 0) { Log("CAM: no candidate for axis %d", axis); continue; }
        void* target = (void*)cand[pick].addr;
        void* detour = axis == 0 ? (void*)lookHookX : (void*)lookHookY;
        void** slot  = axis == 0 ? &oLookX : &oLookY;
        if (MH_CreateHook(target, detour, slot) != MH_OK) { Log("CAM: hook failed %p", target); *slot = nullptr; continue; }
        MH_EnableHook(target);
        Log("CAM: hooked %p (rva=0x%x funcStart=%d) as %s", target,
            (unsigned)(cand[pick].addr - base), cand[pick].starts, axis == 0 ? "yaw(X)" : "pitch(Y)");
    }
}

// ------------------------------------------------------- in-car smoothing patches
// Beyond the frametime divisor, the car camera passes its own state through a chain of
// first-order lerps of the shape `x += (target - x) * k`, where the engine deliberately
// caps k below 1.0. The three below are the ones whose cap is either hardcoded or taken
// from a constant with other users, so they are patched at the single instruction that
// loads it rather than by editing the constant. Each replacement is the same length as
// what it overwrites, so no branch displacement anywhere in the exe shifts, and each one
// verifies the eight bytes it is replacing before touching them and refuses to run on a
// different build.
//
//   0x6C63AA  mulss xmm0,[0xD58CBC]      k = cam60 * 0.1 for the hinge axis
//           -> movss xmm0,[0xD5780C]     (1.0), so k = 1.0 outright
//   0x6C6EC9  movss xmm0,[esi+0x2a8]     k = cam[0x2a8] * cam60 for the pivot
//           -> movss xmm0,[0xD5780C]     k = 1.0 * cam60
//   0x6C6866  mulss xmm5,[0xD5F008]      the auto-pitch approach, -0.75 = 25% per frame
//           -> operand only, to [0xD5780C] = +1.0
// 0xD5F008 and 0xD58CBC are NOT edited: the first is also the pedestal pitch sign flip at
// 0x6C8B7E, the second has ~98 readers. Only these three operands are redirected.
static void ApplyNoSmoothPatches()
{
    static unsigned char orig[3][8];
    static bool        saved[3] = { false, false, false };
    static bool        applied[3] = { false, false, false };
    static bool        gaveUp = false;

    // movss xmm0,[disp32]  ==  F3 0F 10 05 <disp32>
    static const unsigned char kMovss1[8] = { 0xF3, 0x0F, 0x10, 0x05, 0x0C, 0x78, 0xD5, 0x00 };
    static const unsigned char kFrom0[8]  = { 0xF3, 0x0F, 0x59, 0x05, 0xBC, 0x8C, 0xD5, 0x00 };
    static const unsigned char kFrom1[8]  = { 0xF3, 0x0F, 0x10, 0x86, 0xA8, 0x02, 0x00, 0x00 };
    // mulss xmm5,[disp32]  ==  F3 0F 59 2D <disp32>; operand sits four bytes in.
    static const unsigned char kFrom2[8]  = { 0xF3, 0x0F, 0x59, 0x2D, 0x08, 0xF0, 0xD5, 0x00 };
    static const unsigned char kTo2[8]    = { 0xF3, 0x0F, 0x59, 0x2D, 0x0C, 0x78, 0xD5, 0x00 };
    // Plain values, not pointers: an earlier version declared these as (const unsigned*)VA
    // and then dereferenced them, so the "address" was the four code bytes read as an integer,
    // VirtualProtect failed 487, and CarNoSmooth silently never applied anything.
    static const unsigned   kVa[3]   = { 0x6C63AA, 0x6C6EC9, 0x6C6866 };
    static const unsigned char* kExp[3] = { kFrom0, kFrom1, kFrom2 };
    static const unsigned char* kNew[3] = { kMovss1, kMovss1, kTo2 };
    static const char* const    kName[3] = { "hinge axis lerp", "pivot lerp", "auto pitch approach" };

    if (gaveUp) return;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);

    for (int i = 0; i < 3; ++i) {
        bool want = cfg.carNoSmooth;
        if (want == applied[i]) continue;
        unsigned char* p = (unsigned char*)(base + (kVa[i] - 0x400000));
        DWORD old;
        if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) {
            Log("CAM CarNoSmooth: VirtualProtect(%p) failed %u", (void*)p, (unsigned)GetLastError());
            gaveUp = true; return;
        }
        if (!saved[i]) { memcpy(orig[i], p, 8); saved[i] = true; }
        if (memcmp(orig[i], kExp[i], 8) != 0) {
            Log("CAM CarNoSmooth: %s at 0x%x has unexpected bytes, all smoothing patches aborted",
                kName[i], kVa[i]);
            DWORD t; VirtualProtect(p, 8, old, &t);
            gaveUp = true; return;
        }
        memcpy(p, want ? kNew[i] : kExp[i], 8);
        applied[i] = want;
        DWORD t;
        VirtualProtect(p, 8, old, &t);
        Log("CAM CarNoSmooth: %s %s (0x%x, k now %s)", kName[i], want ? "patched" : "restored",
            kVa[i], want ? "1.0" : "engine");
    }
}

// --------------------------------------------------------- in-car pitch gate patch
// The car camera update at 0x6C5EE0 has two pitch paths and picks between them at 0x6C6825:
// it asks the camera's owner (rig+0x28, minus 0x48) for a direction vector through vtable
// slot 0x4c, takes sqrt(v.x^2 + v.z^2), and only falls through to the mouse pitch converter
// at 0x6C6A35 when that length is below [0xE50DE4] = 0.001. Otherwise it drives cam+0x284
// itself from a speed curve and a 0x6C1120 angle, clamped to +-12 deg / +-80 deg, and the
// mouse never reaches pitch at all. Stationary the vector is zero so pitch works; the moment
// the car moves it is not, which is the "vertical movement is non existant as soon as player
// starts driving" report and the calls Y=0 in the log.
//
// The epsilon cannot simply be raised: 0x6C0E08 and 0x6C4D81 read the same constant as a
// genuine "is this vector zero" test in other camera code. So the branch itself is made
// unconditional - 0F 87 rel32 (ja) becomes E9 rel32 (jmp) plus a padding nop, same length,
// displacement moved by one because the opcode is a byte shorter. Restored when the key
// goes back to 0.
static void ApplyPitchGate()
{
    static const unsigned kVa = 0x6C6828;
    static unsigned char orig[6];
    static bool saved = false, applied = false, gaveUp = false;

    if (cfg.carPitchMouse == applied || gaveUp) return;

    unsigned char* p =
        (unsigned char*)((uintptr_t)GetModuleHandleW(nullptr) + (kVa - 0x400000));
    DWORD old;
    if (!VirtualProtect(p, sizeof orig, PAGE_EXECUTE_READWRITE, &old)) {
        Log("CAM CarPitchMouse: VirtualProtect(%p) failed %u", (void*)p, (unsigned)GetLastError());
        return;
    }

    if (cfg.carPitchMouse) {
        if (!saved) { memcpy(orig, p, sizeof orig); saved = true; }
        if (orig[0] == 0x0F && orig[1] == 0x87) {
            int rel = 0;
            memcpy(&rel, orig + 2, 4);
            ++rel;                                  // jmp is one byte shorter than ja
            unsigned char force[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
            memcpy(force + 1, &rel, 4);
            memcpy(p, force, sizeof force);
            applied = true;
            g_pitchGateOn = true;
            Log("CAM CarPitchMouse: in-car pitch forced onto the mouse (0x%x was %02X %02X %02X %02X %02X %02X)",
                kVa, orig[0], orig[1], orig[2], orig[3], orig[4], orig[5]);
        } else {
            gaveUp = true;
            Log("CAM CarPitchMouse: expected 0F 87 at 0x%x, found %02X %02X - disabled",
                kVa, orig[0], orig[1]);
        }
    } else {
        memcpy(p, orig, sizeof orig);
        applied = false;
        g_pitchGateOn = false;
        Log("CAM CarPitchMouse restored");
    }

    DWORD tmp;
    VirtualProtect(p, sizeof orig, old, &tmp);
}

// --------------------------------------------------------- engine constant overrides
// Two data-only levers. Both are floats in .data with exactly one reader each, neither
// of them touches code, and both are restored when their key goes back to 0 - so they can
// be flipped from the 2s ini reload while driving.
//
// 0xE50FA8 (25.0) the vehicle speed above which cam+0x2f0 starts ramping (reader
//   0x6C6252). That accumulator feeds two 0x6C91C0 curves at 0x6C62A5 / 0x6C62DD whose
//   outputs damp both look axes; below the threshold it decays to 0 and both curves emit
//   0, i.e. exactly the stationary behaviour. Raising the threshold holds the car at
//   "stationary" damping no matter how fast it goes.
//
// 0xE50E44 (0.1) how long the chase-cam machine at 0x6C1DE0 needs the input to stay
//   quiet before it latches engaged (reader 0x6C2190). Engagement sets obj+0x30 = 1 at
//   0x6C25D3, and while engaged the mouse-yaw integrator at 0x6C2031 is skipped in
//   favour of 0x6C0810, which pulls the view onto the vehicle heading. Raising the delay
//   past any achievable idle period keeps it disengaged. This machine must keep running:
//   skipping its call site kills in-car yaw outright, because it is also the only writer
//   of the yaw out-vector.
//
// --------------------------------------------------------- look spring stiffness
// The last smoother standing in the car. Measured live, both look converters take their
// branch=1 tail - out = rig_target * deg2rad * delta - which is exact, so nothing is damped
// between the mouse and cam+0x27c/0x280, and CarCam60, Smooth and RampTime are all inert on
// that path. What runs after is 0x6C1DE0: it authors a spring stiffness into [0x112A700+4]
// and integrates the look with 0x6DAAC0, which is second order. Its velocity is multiplied
// by a per-frame retention of [0xE50E28] = 0.333 and fed the target error scaled by k*dt, so
// each frame closes k*dt^2/(1-r) of what is left and the time constant is (1-r)/(k*dt). With
// the engine's k = 175 that is 0.62 s at 150 fps against 0.14 s at the 30 fps those numbers
// were authored for - a second, independent frame-rate penalty on the same cameras as the
// 59.94 divisor, and untouched by CarLookScale because the spring uses the real frametime,
// not the divisor.
//
// The same constant is also what drags the view back behind the car the moment the mouse
// stops, because the spring's target is the chase orientation. Raising it is therefore two
// opposite goods at once: crisp while steering, abrupt on release. So it is switched by
// phase instead of set once - see CarCentreIdle/CarHoldK/CarCentreK - using the camera's own
// movement accumulators to tell the two apart, and only while a vehicle camera is live.
//
// Only three of the seven candidates this started from are stiffness. 0x6C1DE0 hands two
// different things to two different callees out of the same block of constants: [esp+0x20]
// is the spring stiffness, which is 0xE50E10 while the chase cam is not latched (every frame
// of driving, since CarCentreDelay holds the latch off) and a lerp of 0xE50E20/0xE50E24 on
// the ramped path; [esp+0x24] is a *rate* for 0x6C0810, fed from 0xE50E0C, 0xE50E14 and a
// lerp of 0xE50E18/0xE50E1C. A previous build wrote all seven with one value, which also
// multiplied the latched pull rate about forty times and made the recenter a snap. The four
// rate constants are left strictly alone.
static void ApplySpringStiffness()
{
    static const unsigned int va[3]   = { 0xE50E10, 0xE50E20, 0xE50E24 };
    static const float        kEng[3] = { 175.0f, 10.0f, 120.0f };
    static float orig[3];
    static float cur = -1.0f;
    static bool  saved = false, gaveUp = false;
    static LONG  phase = -1;

    if (gaveUp) return;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!saved) {
        for (int i = 0; i < 3; ++i) orig[i] = *(float*)(base + (va[i] - 0x400000));
        for (int i = 0; i < 3; ++i) {
            if (orig[i] == kEng[i]) continue;
            gaveUp = true;
            Log("CAM CarSpringK: engine stiffness[%d] at 0x%x is %.6g, not %.6g - disabled",
                i, va[i], orig[i], kEng[i]);
            return;
        }
        saved = true;
    }

    // 0 = the engine's own values. phase is 0 while steering, 1 during the wait, 2 gliding.
    float want = 0.0f;
    long  ph   = 0;
    if (cfg.carSpringK > 0.0f) {
        want = cfg.carSpringK;
        LONG now = (LONG)GetTickCount();
        bool inCar = g_carSeen != 0 && (long)(now - g_carSeen) < 250;
        long idle  = g_lastSteer == 0 ? 0x7fffffff : (long)(now - g_lastSteer);
        if (cfg.carCentreIdle > 0.0f && inCar && idle > 120) {
            ph = idle >= (long)(cfg.carCentreIdle * 1000.0f) ? 2 : 1;
            want = (ph == 2 ? cfg.carCentreK : cfg.carHoldK);
            if (!(want > 0.0f)) want = orig[0];
        }
    }

    // Cap it by the frame time the spring is actually integrated with. The discretised spring
    // closes k*dt^2/(1-r) of the remaining error each frame, so one fixed k is only well
    // damped at one frame rate: 5000 closes 0.75 of the error per frame at 150fps, but 4.3 at
    // the 0.024s frame the log caught on 15:44:09, and past 1.0 it overshoots - which is the
    // jerk when new geometry steps in and costs a frame. Peak-hold with a decay, so a single
    // hitch caps for about a second and then relaxes again.
    static float peakDt = 0.0f;
    float dtObs = (float)g_dtMicro / 1e6f;
    float cap   = 0.0f;
    if (dtObs > 0.0002f && dtObs < 0.2f) {
        peakDt = dtObs > peakDt ? dtObs : peakDt * 0.85f;
        cap    = 0.5f / (peakDt * peakDt);
        if (want > cap) want = cap;
    }

    if (fabsf(want - cur) > 1e-3f) {
        for (int i = 0; i < 3; ++i) {
            float* p = (float*)(base + (va[i] - 0x400000));
            DWORD old;
            if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) {
                gaveUp = true;
                Log("CAM CarSpringK: VirtualProtect(%p) failed %u", (void*)p, (unsigned)GetLastError());
                return;
            }
            *p = want > 0.0f ? want : orig[i];
            DWORD tmp;
            VirtualProtect(p, 4, old, &tmp);
        }
        cur = want;
    }

    // A drive flips these constantly, so transitions are reported sparsely; the live value
    // is also in the e10= field of the per-second LOOK state line.
    if (phase != ph) {
        phase = ph;
        LONG now = (LONG)GetTickCount();
        static LONG loggedAt = 0;
        if ((long)(now - loggedAt) > 2000) {
            loggedAt = now;
            Log("CAM spring phase=%s k=%.0f (idle %ld ms, car=%d dt=%.5f cap=%.0f)",
                ph == 0 ? "steering" : ph == 1 ? "hold" : "glide",
                cur > 0.0f ? cur : orig[0],
                g_lastSteer == 0 ? -1 : (long)(now - g_lastSteer), g_carSeen != 0 ? 1 : 0,
                peakDt, cap);
        }
    }
}
static void ApplyDataLevers()
{
    static const unsigned int va[2] = { 0xE50FA8, 0xE50E44 };    static const char* const name[2] = { "CarSpeedThresh", "CarCentreDelay" };
    static float orig[2]    = { 0.0f, 0.0f };
    static bool  applied[2] = { false, false };
    const float want[2]     = { cfg.carSpeedThresh, cfg.carCentreDelay };

    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    for (int i = 0; i < 2; ++i) {
        float* p = (float*)(base + (va[i] - 0x400000));
        DWORD old;
        if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) {
            Log("CAM VirtualProtect(%p) failed %u", (void*)p, (unsigned)GetLastError());
            continue;
        }
        if (want[i] > 0.0f) {
            if (!applied[i]) { orig[i] = *p; applied[i] = true; }
            if (*p != want[i]) {
                *p = want[i];
                Log("CAM %s = %.6g (engine value %.6g)", name[i], want[i], orig[i]);
            }
        } else if (applied[i]) {
            *p = orig[i];
            applied[i] = false;
            Log("CAM %s restored to %.6g", name[i], orig[i]);
        }
        DWORD tmp;
        VirtualProtect(p, 4, old, &tmp);
    }

    // Data again, but a group rather than a pair, and the one that matters most.
    ApplySpringStiffness();

    // The levers that do touch code; driven from the same 2s reload so they can also be
    // flipped while driving.
    ApplyNoSmoothPatches();
    ApplyPitchGate();
}

// ------------------------------------------------------------- look delta filter
// The look caller contains a path that feeds both mouse deltas through this function
// and uses its return value as the delta the converters see.  Hooked and measured: in
// gameplay it is NEVER CALLED - the caller takes the earlier branch that reads the raw
// globals 0xe50f20 (yaw) / 0xe50ec0 (pitch) directly.  Kept because the gate that skips
// it is runtime state, so it may be live in some other situation, and because it is the
// only place in the exe that smooths a look delta.  State layout from the disassembly:
// +0x00 moving flag, +0x40 the accumulator it returns, +0x44 the per-frame coefficient,
// +0x48 a hold timer, +0x4c the saturation limit, +0x50 the timer reload, +0x54 a decay
// threshold.  All of those are zero in the image and authored at runtime.
typedef float (__thiscall *LookFilter_t)(void*, float, float);
static LookFilter_t oLookFilter   = nullptr;
static bool         g_filterTried = false;
static void*        g_filterState[2] = { nullptr, nullptr };
static DWORD        g_filterLogAt[2] = { 0, 0 };

static int FilterSlot(void* state)
{
    for (int i = 0; i < 2; ++i) if (g_filterState[i] == state) return i;
    for (int i = 0; i < 2; ++i) if (!g_filterState[i]) { g_filterState[i] = state; return i; }
    return -1;
}

struct LookFilterHook {
    static float __thiscall Detour(void* state, float dt, float value)
    {
        // Always run the real filter first: it owns the accumulator, the hold timer and
        // the moving flag, and leaving those stale would break the first frame after
        // FilterBypass is switched back off live.
        float engine = oLookFilter(state, dt, value);

        if (cfg.filterLog) {
            int s = FilterSlot(state);
            if (s >= 0 && (long)(GetTickCount() - g_filterLogAt[s]) > 1000) {
                g_filterLogAt[s] = GetTickCount();
                int flag = -1;
                float acc = 0, coeff = 0, hold = 0, limit = 0, reload = 0, thresh = 0;
                SafeInt(state, 0x00, &flag);
                SafeF(state, 0x40, &acc);   SafeF(state, 0x44, &coeff);
                SafeF(state, 0x48, &hold);  SafeF(state, 0x4c, &limit);
                SafeF(state, 0x50, &reload); SafeF(state, 0x54, &thresh);
                Log("FILTER #%d state=%p dt=%.5f raw=%.5f engine=%.5f ratio=%.4f%s "
                    "acc=%.5f coeff=%.6f hold=%.4f limit=%.4f reload=%.4f thresh=%.4f moving=%d",
                    s, state, dt, value, engine,
                    (value > 1e-6f || value < -1e-6f) ? engine / value : 0.0f,
                    cfg.filterBypass ? " BYPASSED" : "", acc, coeff, hold, limit, reload,
                    thresh, flag);
            }
        }
        return cfg.filterBypass ? value * cfg.filterGain : engine;
    }
};

static void TryHookLookFilter()
{
    if (g_filterTried) return;
    g_filterTried = true;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!g_codeLo && !FindCodeSection(base)) { Log("FILTER: no executable section"); return; }

    // push ebp / mov ebp,esp / and esp,-10h / movss xmm1,[ebp+0Ch] / xorps xmm0,xmm0 /
    // sub esp,18h / push esi.  Verified unique in .text on the 1.0 exe.
    static const unsigned char pro[] = {
        0x55,0x8B,0xEC,0x83,0xE4,0xF0,0xF3,0x0F,0x10,0x4D,0x0C,0x0F,0x57,0xC0,0x83,0xEC,0x18,0x56
    };
    uintptr_t hit = 0; int n = 0;
    for (uintptr_t a = g_codeLo; a + sizeof(pro) < g_codeHi; ++a) {
        if (!HasBytes(a, pro, sizeof pro)) continue;
        // A real function start sits behind the int3 padding between functions.
        if (a > g_codeLo && *(unsigned char*)(a - 1) != 0xCC) continue;
        hit = a; ++n;
    }
    if (n != 1) { Log("FILTER: %d prologue matches, expected 1 - not hooking", n); return; }

    if (MH_CreateHook((void*)hit, (void*)LookFilterHook::Detour, (void**)&oLookFilter) != MH_OK) {
        Log("FILTER: hook failed %p", (void*)hit); oLookFilter = nullptr; return;
    }
    MH_EnableHook((void*)hit);
    Log("FILTER: hooked %p (rva=0x%x) bypass=%d gain=%.2f", (void*)hit,
        (unsigned)(hit - base), cfg.filterBypass ? 1 : 0, cfg.filterGain);
}

static bool ReadRig(LookRig* r, float* v)
{
    for (int k = 0; k < 7; ++k)
        if (!SafeF(r->rig, g_rigOff[k], &v[k])) return false;
    return true;
}

// A rig whose memory has been gone for several consecutive polls stops being tracked.
// One failed read is not enough: this runs on the poll thread and races the game
// reusing a stack rig, and dropping on the first miss churned the whole table - which
// in turn evicted the live cameras and left the in-car rig unmodified.
static bool MissRig(LookRig* r)
{
    if (++r->miss < 3) return false;
    r->dead = 1;
    r->miss = 0;
    return true;
}

static void DumpCameras(const char* why)
{
    char cf[256];
    CamCfgText(cf, sizeof cf);
    Log("=== CAMERA DUMP via %s calls X=%ld Y=%ld rejects=%ld/%ld rigs=%ld last=%p/%p "
        "%s branch=%d writes=%ld ===",
        why, (long)g_axisCalls[0], (long)g_axisCalls[1],
        (long)g_rejectN[0], (long)g_rejectN[1], (long)g_rigN, g_lastRig[0], g_lastRig[1],
        cf, LookBranch(), (long)g_forceN);
    for (LONG i = 0; i < g_rigN; ++i) {
        LookRig* r = &g_rigs[i];
        if (r->dead) continue;
        // Re-read live: the whole point is to see these change between on foot and driving.
        float v[7];
        if (!ReadRig(r, v)) {
            if (MissRig(r))
                Log("  rig#%ld axis=%d rig=%p no longer readable, dropping", (long)i, r->axis, r->rig);
            continue;
        }
        r->miss = 0;
        float c60 = 0.0f;
        if (r->self) SafeF(r->self, 0x60, &c60);
        // k = scale x cam+60 is the smoothing coefficient the converter actually applies,
        // so it is the number worth reading; the two factors alone are misleading.
        Log("  rig#%ld axis=%d slot=%d self=%p range=%.4f scale=%.6f/%.6f k=%.4f/%.4f "
            "yaw=(%.2f->%.2f) pitch=(%.2f->%.2f) cam+60=%.4f raised=%ld calls=%ld",
            (long)i, r->axis, r->slot, r->self, v[4], v[5], v[6], v[5] * c60, v[6] * c60,
            v[0], v[1], v[2], v[3], c60, (long)r->raised, (long)r->calls);
        r->calls = 0;
    }
    g_axisCalls[0] = g_axisCalls[1] = 0;
}

// The 10 s dump is too coarse to tie a value to an action, so watch the rigs every
// poll and log the instant the engine retunes one (entering a car, aiming, ...).
static void WatchRigs()
{
    for (LONG i = 0; i < g_rigN; ++i) {
        LookRig* r = &g_rigs[i];
        if (r->dead) continue;
        float v[7];
        if (!ReadRig(r, v)) { MissRig(r); continue; }
        r->miss = 0;
        float c60 = 0.0f;
        if (r->self) SafeF(r->self, 0x60, &c60);
        if (r->haveLast) {
            // cam+0x60 drifts continuously while driving, so it is reported but never
            // treated as a retune on its own.
            bool diff = false;
            for (int k = 0; k < 7; ++k) if (v[k] != r->last[k]) { diff = true; break; }
            DWORD now = GetTickCount();
            if (diff && (r->lastLog == 0 || (long)(now - r->lastLog) > 2000)) {
                r->lastLog = now;
                Log("CAM CHANGE rig#%ld %p axis=%d: yaw=(%.3f->%.3f) pitch=(%.3f->%.3f) "
                    "range=%.4f scale=%.6f/%.6f cam+60=%.4f (was %.3f->%.3f / %.3f->%.3f range=%.4f scale=%.6f/%.6f cam+60=%.4f)",
                    (long)i, r->rig, r->axis, v[0], v[1], v[2], v[3], v[4], v[5], v[6], c60,
                    r->last[0], r->last[1], r->last[2], r->last[3], r->last[4], r->last[5], r->last[6],
                    r->last[7]);
            }
        }
        for (int k = 0; k < 7; ++k) r->last[k] = v[k];
        r->last[7] = c60;
        r->haveLast = 1;
    }
}

// F9-style capture: global key poll (independent of window focus / message delivery).
// Press Scroll Lock or Pause during gameplay with subtitles on screen -> 6s APT dump.
static void StartCapture(const char* why){
    g_capDsN = 0; g_capSvmN = 0; g_t2N = 0; g_t1N = 0;
    g_captureUntil = GetTickCount() + 6000;
    DumpCameras(why);
    Log("=== CAPTURE START 6s via %s (screen=%dx%d textScale=%.3f gdsNative=%ld) ===",
        why, g_screenW, g_screenH, g_textK, g_gdsNative);
}

static DWORD WINAPI CapturePollThread(LPVOID)
{
    bool prevScroll = false, prevPause = false;
    DWORD nextDump = GetTickCount() + 10000;
    DWORD nextReload = 0;
    // Sticky: once the camera keys matter they keep being polled, so turning a flag off
    // in the ini cannot strand the reload and make it impossible to turn back on.
    bool watchCfg = false;
    for (;;) {
        Sleep(60);
        // The spring's phase is a reaction to the last few frames, so it is refreshed on the
        // tick rather than on the 2s ini reload. Idempotent: it only writes when the value
        // actually changes.
        if (cfg.carSpringK > 0.0f) ApplySpringStiffness();
        if (watchCfg || NeedCameraHooks() || cfg.filterBypass || cfg.filterLog ||
            cfg.carPitchMouse || cfg.carNoSmooth || cfg.carSpringK > 0.0f ||
            cfg.carCentreIdle > 0.0f || cfg.carSpeedThresh > 0.0f ||
            cfg.carCentreDelay > 0.0f) {
            watchCfg = true;
            // Let the look be retuned while playing instead of per launch.
            if ((long)(GetTickCount() - nextReload) >= 0) {
                nextReload = GetTickCount() + 2000;
                char before[256], after[256];
                CamCfgText(before, sizeof before);
                LoadCameraCfg();
                CamCfgText(after, sizeof after);
                if (strcmp(before, after) != 0) Log("CAM cfg now %s", after);
                // Idempotent, so this only installs them the first time a flag that needs
                // them is switched on mid-session.
                if (NeedCameraHooks()) TryHookCamera();
                ApplyDataLevers();
            }
        }
        if (cfg.cameraProbe) {
            WatchRigs();
            if ((long)(GetTickCount() - nextDump) >= 0) {
                nextDump = GetTickCount() + 10000;
                DumpCameras("POLL");
            }
        }
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
        if (cfg.aptHooks) {
            TryHookApt();
            TryHookRenderers((uintptr_t)GetModuleHandleW(nullptr));
        }
        // DeDamp, CarCam60 and CarLookScale all work by rewriting camera or engine state
        // from inside these hooks.
        if (NeedCameraHooks()) TryHookCamera();
        if (cfg.filterBypass || cfg.filterLog) TryHookLookFilter();
        // Independent of the hooks. Idempotent, and running it here re-asserts the
        // overrides if the engine ever re-initialises its .data on a level load.
        ApplyDataLevers();
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
            TryDetour("Direct3DCreate9", p, (void*)hkDirect3DCreate9, (void**)&oDirect3DCreate9);
        }
        HMODULE hU = GetModuleHandleA("user32.dll");
        TryDetour("ChangeDisplaySettingsExW", (void*)GetProcAddress(hU, "ChangeDisplaySettingsExW"), (void*)hkChangeDisplaySettingsExW, nullptr);
        TryDetour("ChangeDisplaySettingsExA", (void*)GetProcAddress(hU, "ChangeDisplaySettingsExA"), (void*)hkChangeDisplaySettingsExA, nullptr);
    }

    if (cfg.rawInput) {
        HMODULE hDI = LoadLibraryA("dinput8.dll"); // proxy dinput8 exports the real DirectInput8Create
        if (hDI) {
            void* p = (void*)GetProcAddress(hDI, "DirectInput8Create");
            Log("DirectInput8Create at %p bytes=%02x %02x %02x %02x %02x %02x", p,
                p ? ((BYTE*)p)[0] : 0, p ? ((BYTE*)p)[1] : 0, p ? ((BYTE*)p)[2] : 0,
                p ? ((BYTE*)p)[3] : 0, p ? ((BYTE*)p)[4] : 0, p ? ((BYTE*)p)[5] : 0);
            TryDetour("DirectInput8Create", p, (void*)hkDirectInput8Create, (void**)&oDirectInput8Create);
        }
        if (cfg.cursorHooks) {
            // Patching user32 while another thread is inside one of these functions can
            // execute a half-written jump, and Modern Fixes hooks them too: wait until the
            // game is up before touching them.
            for (int i = 0; i < 100 && !g_hWnd; i++) Sleep(100);
            Sleep(500);
            HMODULE hU = GetModuleHandleA("user32.dll");
            TryDetour("GetCursorPos", (void*)GetProcAddress(hU, "GetCursorPos"), (void*)hkGetCursorPos, (void**)&oGetCursorPos);
            TryDetour("SetCursorPos", (void*)GetProcAddress(hU, "SetCursorPos"), (void*)hkSetCursorPos, (void**)&oSetCursorPos);
            TryDetour("ClipCursor", (void*)GetProcAddress(hU, "ClipCursor"), (void*)hkClipCursor, (void**)&oClipCursor);
        } else {
            HMODULE hU = GetModuleHandleA("user32.dll");
            BYTE* pc = (BYTE*)GetProcAddress(hU, "ClipCursor");
            BYTE* pg = (BYTE*)GetProcAddress(hU, "GetCursorPos");
            if (pc && pg)
                Log("cursor hooks off; user32 ClipCursor(%p)=%02x %02x %02x %02x %02x %02x "
                    "GetCursorPos(%p)=%02x %02x %02x %02x %02x %02x", pc,
                    pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pg,
                    pg[0], pg[1], pg[2], pg[3], pg[4], pg[5]);
        }
    }

    if (cfg.logFiles) {
        HMODULE hK = GetModuleHandleA("kernel32.dll");
        void* pw = (void*)GetProcAddress(hK, "CreateFileW");
        void* pa = (void*)GetProcAddress(hK, "CreateFileA");
        TryDetour("CreateFileW", pw, (void*)hkCreateFileW, (void**)&oCreateFileW);
        TryDetour("CreateFileA", pa, (void*)hkCreateFileA, (void**)&oCreateFileA);
        Log("CreateFile file-trace hooks: W=%p A=%p", pw, pa);
    }
    char camCfg[256];
    CamCfgText(camCfg, sizeof camCfg);
    Log("gf2fix loaded: borderless=%d raw=%d cursors=%d sens=%.2f textScale=%.2f "
        "probe=%d %s",
        cfg.borderless, cfg.rawInput, cfg.cursorHooks, cfg.sensitivity, cfg.textScale,
        cfg.cameraProbe, camCfg);
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
