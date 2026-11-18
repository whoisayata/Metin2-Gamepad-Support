#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <unknwn.h>
#include <mmsystem.h>
#include <xinput.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "Metin2UI.h"

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

// =========================================================================
// 1. DIRECTINPUT8 PROXY EXPORTLARI
// =========================================================================
typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DllCanUnloadNow)(void);
typedef HRESULT(WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT(WINAPI* PFN_DllRegisterServer)(void);
typedef HRESULT(WINAPI* PFN_DllUnregisterServer)(void);

static HMODULE g_realDInput8 = NULL;
static PFN_DirectInput8Create  g_origDirectInput8Create = NULL;
static PFN_DllCanUnloadNow     g_origDllCanUnloadNow = NULL;
static PFN_DllGetClassObject   g_origDllGetClassObject = NULL;
static PFN_DllRegisterServer   g_origDllRegisterServer = NULL;
static PFN_DllUnregisterServer g_origDllUnregisterServer = NULL;

static char g_iniPath[MAX_PATH] = { 0 };
static char g_logPath[MAX_PATH] = { 0 };

static void WriteDebugLog(const char* fmt, ...) {
    if (!g_logPath[0]) return;
    FILE* f = NULL;
    if (fopen_s(&f, g_logPath, "a") == 0 && f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

static void EnsureRealDInput8() {
    if (g_realDInput8) return;
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    strcat_s(sysPath, "\\dinput8.dll");
    g_realDInput8 = LoadLibraryA(sysPath);
    if (g_realDInput8) {
        g_origDirectInput8Create = (PFN_DirectInput8Create)GetProcAddress(g_realDInput8, "DirectInput8Create");
        g_origDllCanUnloadNow = (PFN_DllCanUnloadNow)GetProcAddress(g_realDInput8, "DllCanUnloadNow");
        g_origDllGetClassObject = (PFN_DllGetClassObject)GetProcAddress(g_realDInput8, "DllGetClassObject");
        g_origDllRegisterServer = (PFN_DllRegisterServer)GetProcAddress(g_realDInput8, "DllRegisterServer");
        g_origDllUnregisterServer = (PFN_DllUnregisterServer)GetProcAddress(g_realDInput8, "DllUnregisterServer");
    }
}

// =========================================================================
// 2. D3D9 SHARED MEMORY KOPRUSU (v3)
// =========================================================================
#pragma pack(push, 1)
struct GamepadBridgeData
{
    DWORD magic;              // 0x47504439 ('GPD9')
    DWORD version;            // 3
    volatile LONG hideCursor;
    volatile LONG invOpen;
    volatile LONG inGame;
    int invLeft;
    int invTop;
    int slotW;
    int slotH;
    BYTE slotHeight[45];
    int quickSlotX[8];
    int quickSlotY[8];
    DWORD lastUpdateTick;
};
#pragma pack(pop)

static HANDLE             g_gpMapFile = NULL;
static GamepadBridgeData* g_gpBridge = NULL;

static bool EnsureGamepadBridge() {
    if (g_gpBridge && g_gpBridge->magic == 0x47504439 && g_gpBridge->version == 3) return true;

    if (!g_gpMapFile) {
        g_gpMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Local\\Metin2GamepadBridge");
        if (!g_gpMapFile) {
            g_gpMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                0, sizeof(GamepadBridgeData), "Local\\Metin2GamepadBridge");
        }
    }
    if (!g_gpMapFile) return false;

    if (!g_gpBridge) {
        g_gpBridge = (GamepadBridgeData*)MapViewOfFile(g_gpMapFile, FILE_MAP_ALL_ACCESS,
            0, 0, sizeof(GamepadBridgeData));
        if (g_gpBridge && (g_gpBridge->magic != 0x47504439 || g_gpBridge->version != 3)) {
            memset(g_gpBridge, 0, sizeof(GamepadBridgeData));
            g_gpBridge->magic = 0x47504439;
            g_gpBridge->version = 3;
            g_gpBridge->slotW = 32;
            g_gpBridge->slotH = 32;
        }
    }
    return (g_gpBridge != NULL);
}

static void CloseGamepadBridge() {
    if (g_gpBridge) {
        InterlockedExchange(&g_gpBridge->hideCursor, 0);
        UnmapViewOfFile(g_gpBridge);
        g_gpBridge = NULL;
    }
    if (g_gpMapFile) {
        CloseHandle(g_gpMapFile);
        g_gpMapFile = NULL;
    }
}

// =========================================================================
// 3. GAMEPAD, HISTEREZISLI ANALOG HAREKET & METIN2 UI OVERLAY MOTORU
// =========================================================================
typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
static PFN_XInputGetState g_pfnXInputGetState = NULL;

static M2UI::AssetManager g_m2ui;
static ULONG_PTR          g_gdiplusToken = 0;
static bool               g_showCombatHUD = true;

static HMODULE g_hModule = NULL;
static HWND g_gameWnd = NULL;
static HWND g_uiWnd = NULL;
static HWND g_overlayWnd = NULL;
static volatile bool g_running = false;
static HANDLE g_thread = NULL;

static volatile bool  g_hideGameCursor = false;
static volatile bool  g_kbMouseMode = false;
static volatile int   g_cursorClientX = -1000;
static volatile int   g_cursorClientY = -1000;
static volatile DWORD g_lastSyntheticMouseTick = 0;

static volatile int  g_invGridLeft = 0;
static volatile int  g_invGridTop = 0;
static volatile int  g_slotSpanMap[45];

static bool g_invOpen = false;
static int  g_gridCol = 0; // 0..4
static int  g_gridRow = 0; // 0..8
static int  g_invTab = 0;  // 0..3

static int  g_invOffsetX = 26;
static int  g_invOffsetY = 210;

static float g_mouseSpeed = 14.0f;
static float g_camSpeedX = 4.5f;
static float g_camSpeedY = 3.0f;
static volatile bool g_camDragging = false;
static float g_camAccumX = 0.0f;
static float g_camAccumY = 0.0f;

static bool  g_modLayerUsedCombo = false;
static bool  g_heldKeys[256] = { false };

static DWORD g_curPadButtons = 0;
static float g_curLx = 0.0f, g_curLy = 0.0f;

struct MagpieTransform {
    bool active;
    HWND hwnd;
    float scale;
    float offsetX;
    float offsetY;
};

static BOOL CALLBACK FindMagpieWndProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char cls[128] = { 0 };
    if (GetClassNameA(hwnd, cls, sizeof(cls)) > 0) {
        if (strncmp(cls, "Window_Magpie_", 14) == 0) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc) && (rc.right - rc.left) > 800 && (rc.bottom - rc.top) > 600) {
                *(HWND*)lParam = hwnd;
                return FALSE;
            }
        }
    }
    return TRUE;
}

static MagpieTransform GetMagpieTransform() {
    MagpieTransform tr = { false, NULL, 1.0f, 0.0f, 0.0f };
    if (!g_gameWnd) return tr;

    HWND hMagpie = NULL;
    EnumWindows(FindMagpieWndProc, (LPARAM)&hMagpie);
    if (!hMagpie) return tr;

    RECT srcRc, dstRc;
    if (!GetClientRect(g_gameWnd, &srcRc) || !GetWindowRect(hMagpie, &dstRc)) return tr;

    int srcW = srcRc.right - srcRc.left;
    int srcH = srcRc.bottom - srcRc.top;
    float dstW = (float)(dstRc.right - dstRc.left);
    float dstH = (float)(dstRc.bottom - dstRc.top);
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.0f || dstH <= 0.0f) return tr;

    float sx = dstW / (float)srcW;
    float sy = dstH / (float)srcH;
    float scale = (sx < sy) ? sx : sy;
    if (scale <= 0.1f) return tr;

    tr.active = true;
    tr.hwnd = hMagpie;
    tr.scale = scale;
    tr.offsetX = (float)dstRc.left + (dstW - srcW * scale) * 0.5f;
    tr.offsetY = (float)dstRc.top + (dstH - srcH * scale) * 0.5f;
    return tr;
}

static bool IsGameOrMagpieForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == g_gameWnd) return true;
    MagpieTransform mp = GetMagpieTransform();
    return (mp.active && fg == mp.hwnd);
}

enum PadButton {
    PAD_UP = 1 << 0,
    PAD_DOWN = 1 << 1,
    PAD_LEFT = 1 << 2,
    PAD_RIGHT = 1 << 3,
    PAD_BTN1 = 1 << 4,  // Ucgen / Y
    PAD_BTN2 = 1 << 5,  // Yuvarlak / B
    PAD_BTN3 = 1 << 6,  // Carpi / A
    PAD_BTN4 = 1 << 7,  // Kare / X
    PAD_BTN5 = 1 << 8,  // L1
    PAD_BTN6 = 1 << 9,  // R1
    PAD_BTN7 = 1 << 10, // L2 (2. Katman Tusu)
    PAD_BTN8 = 1 << 11, // R2 (Serbest Imlec Modu)
    PAD_BTN9 = 1 << 12, // Select
    PAD_BTN10 = 1 << 13, // Start
    PAD_BTN11 = 1 << 14, // L3
    PAD_BTN12 = 1 << 15  // R3
};

#define MOD_FLAG_CTRL  0x010000
#define MOD_FLAG_ALT   0x020000
#define MOD_FLAG_SHIFT 0x040000
#define KEY_SPECIAL_LCLICK   0xFF01
#define KEY_SPECIAL_RCLICK   0xFF02
#define KEY_SPECIAL_PREVTAB  0xFF03
#define KEY_SPECIAL_NEXTTAB  0xFF04

enum GameAction {
    ACT_ATTACK = 0,
    ACT_PICKUP,
    ACT_SLOT1,
    ACT_SLOT2,
    ACT_SLOT3,
    ACT_SLOT4,
    ACT_DPAD_UP,
    ACT_DPAD_RIGHT,
    ACT_DPAD_DOWN,
    ACT_DPAD_LEFT,
    ACT_INVENTORY,
    ACT_HORSE_RIDE,
    ACT_MOD_LAYER,
    ACT_L2_ACT1,
    ACT_L2_ACT2,
    ACT_L2_ACT3,
    ACT_L2_ACT4,
    ACT_LCLICK,
    ACT_RCLICK,
    COMBAT_ACT_COUNT,

    ACT_INV_LCLICK = COMBAT_ACT_COUNT,
    ACT_INV_RCLICK,
    ACT_INV_PREVTAB,
    ACT_INV_NEXTTAB,
    ACT_INV_ATTACK,
    ACT_COUNT
};

struct ActionBind {
    int tabId;
    const char* iniKey;
    const wchar_t* labelW;
    DWORD padMask;
    DWORD keyCombo;
    bool  holdMode;
};

static ActionBind g_binds[ACT_COUNT] = {
    { 0, "Attack",      L"Saldırı (Space)",              PAD_BTN3,  VK_SPACE,            true  },
    { 0, "Pickup",      L"Eşya Topla (Z)",               PAD_BTN5,  'Z',                 false },
    { 0, "Slot1",       L"Hızlı Slot 1",                 PAD_BTN4,  '1',                 false },
    { 0, "Slot2",       L"Hızlı Slot 2",                 PAD_BTN1,  '2',                 false },
    { 0, "Slot3",       L"Hızlı Slot 3",                 PAD_BTN2,  '3',                 false },
    { 0, "Slot4",       L"Hızlı Slot 4",                 PAD_BTN6,  '4',                 false },
    { 0, "DpadUp",      L"D-Pad Yukarı (F1)",            PAD_UP,    VK_F1,               false },
    { 0, "DpadRight",   L"D-Pad Sağ (F2)",               PAD_RIGHT, VK_F2,               false },
    { 0, "DpadDown",    L"D-Pad Aşağı (F3)",             PAD_DOWN,  VK_F3,               false },
    { 0, "DpadLeft",    L"D-Pad Sol (F4)",               PAD_LEFT,  VK_F4,               false },
    { 0, "Inventory",   L"Envanter Aç / Kapat (I)",      PAD_BTN9,  'I',                 false },
    { 0, "HorseRide",   L"Ata İn / Bin (Ctrl+G)",        PAD_BTN10, 'G' | MOD_FLAG_CTRL, false },
    { 0, "ModLayer",    L"2. Katman Tuşu (Mod)",         PAD_BTN7,  0,                   true  },
    { 0, "L2_Act1",     L"2. Katman + Slot F1",          PAD_BTN4,  VK_F1,               false },
    { 0, "L2_Act2",     L"2. Katman + Slot F2",          PAD_BTN1,  VK_F2,               false },
    { 0, "L2_Act3",     L"2. Katman + Slot F3",          PAD_BTN2,  VK_F3,               false },
    { 0, "L2_Act4",     L"2. Katman + Slot F4",          PAD_BTN3,  VK_F4,               false },
    { 0, "LeftClick",   L"Genel Fare Sol Tık",           PAD_BTN11, KEY_SPECIAL_LCLICK,  false },
    { 0, "RightClick",  L"Genel Fare Sağ Tık",           PAD_BTN12, KEY_SPECIAL_RCLICK,  false },

    { 1, "InvLeftClick",  L"Envanter Sol Tık (Tut/Bırak)",   PAD_BTN4, KEY_SPECIAL_LCLICK,  false },
    { 1, "InvRightClick", L"Envanter Sağ Tık (Giy/Kullan)",  PAD_BTN3, KEY_SPECIAL_RCLICK,  false },
    { 1, "InvPrevTab",    L"Önceki Envanter Sayfası (I-IV)", PAD_BTN5, KEY_SPECIAL_PREVTAB, false },
    { 1, "InvNextTab",    L"Sonraki Envanter Sayfası (I-IV)",PAD_BTN6, KEY_SPECIAL_NEXTTAB, false },
    { 1, "InvAttack",     L"Envanter Açıkken Saldır (Space)",0,        VK_SPACE,            true  }
};

static int g_activeUiTab = 0;
static volatile int g_listeningAction = -1;
static volatile int g_listeningKey = -1;

struct PadState {
    bool connected;
    float lx, ly;
    float rx, ry;
    DWORD buttons;
};

static const wchar_t* PadMaskToShortWName(DWORD mask) {
    switch (mask) {
    case PAD_UP:    return L"▲";
    case PAD_DOWN:  return L"▼";
    case PAD_LEFT:  return L"◄";
    case PAD_RIGHT: return L"►";
    case PAD_BTN1:  return L"Y / ▲";
    case PAD_BTN2:  return L"B / ●";
    case PAD_BTN3:  return L"A / ✖";
    case PAD_BTN4:  return L"X / ■";
    case PAD_BTN5:  return L"L1";
    case PAD_BTN6:  return L"R1";
    case PAD_BTN7:  return L"L2";
    case PAD_BTN8:  return L"R2";
    case PAD_BTN9:  return L"SELECT";
    case PAD_BTN10: return L"START";
    case PAD_BTN11: return L"L3";
    case PAD_BTN12: return L"R3";
    default:        return L"-";
    }
}

static const wchar_t* PadMaskToFullWName(DWORD mask) {
    switch (mask) {
    case PAD_UP:    return L"D-Pad Yukarı";
    case PAD_DOWN:  return L"D-Pad Aşağı";
    case PAD_LEFT:  return L"D-Pad Sol";
    case PAD_RIGHT: return L"D-Pad Sağ";
    case PAD_BTN1:  return L"Tuş 1 (Üçgen / Y)";
    case PAD_BTN2:  return L"Tuş 2 (Yuvarlak / B)";
    case PAD_BTN3:  return L"Tuş 3 (Çarpı / A)";
    case PAD_BTN4:  return L"Tuş 4 (Kare / X)";
    case PAD_BTN5:  return L"Tuş 5 (L1)";
    case PAD_BTN6:  return L"Tuş 6 (R1)";
    case PAD_BTN7:  return L"Tuş 7 (L2)";
    case PAD_BTN8:  return L"Tuş 8 (R2)";
    case PAD_BTN9:  return L"Tuş 9 (Select)";
    case PAD_BTN10: return L"Tuş 10 (Start)";
    case PAD_BTN11: return L"Tuş 11 (L3)";
    case PAD_BTN12: return L"Tuş 12 (R3)";
    default:        return L"Atanmamış";
    }
}

static void KeyComboToWName(DWORD combo, wchar_t* buf, size_t sz) {
    WORD vk = (WORD)(combo & 0xFFFF);
    if (vk == 0) { wcscpy_s(buf, sz, L"[Katman Tuşu]"); return; }
    if (vk == KEY_SPECIAL_LCLICK) { wcscpy_s(buf, sz, L"Sol Tık"); return; }
    if (vk == KEY_SPECIAL_RCLICK) { wcscpy_s(buf, sz, L"Sağ Tık"); return; }
    if (vk == KEY_SPECIAL_PREVTAB) { wcscpy_s(buf, sz, L"[Önceki Sayfa]"); return; }
    if (vk == KEY_SPECIAL_NEXTTAB) { wcscpy_s(buf, sz, L"[Sonraki Sayfa]"); return; }

    wchar_t prefix[48] = L"";
    if (combo & MOD_FLAG_CTRL)  wcscat_s(prefix, L"CTRL + ");
    if (combo & MOD_FLAG_ALT)   wcscat_s(prefix, L"ALT + ");
    if (combo & MOD_FLAG_SHIFT) wcscat_s(prefix, L"SHIFT + ");

    wchar_t kname[32] = L"";
    if (vk == VK_SPACE) wcscpy_s(kname, L"SPACE");
    else if (vk >= VK_F1 && vk <= VK_F12) swprintf_s(kname, L"F%d", (vk - VK_F1) + 1);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) swprintf_s(kname, L"%c", (wchar_t)vk);
    else {
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(scan << 16, kname, 32) == 0) {
            swprintf_s(kname, L"VK_%02X", vk);
        }
    }
    swprintf_s(buf, sz, L"%s%s", prefix, kname);
}

static void SaveConfig() {
    if (!g_iniPath[0]) return;
    char buf[64];
    for (int i = 0; i < ACT_COUNT; i++) {
        sprintf_s(buf, "%u", g_binds[i].padMask);
        WritePrivateProfileStringA("PadButtons", g_binds[i].iniKey, buf, g_iniPath);
        sprintf_s(buf, "%u", g_binds[i].keyCombo);
        WritePrivateProfileStringA("KeyCombos", g_binds[i].iniKey, buf, g_iniPath);
    }
    sprintf_s(buf, "%d", g_invOffsetX);
    WritePrivateProfileStringA("Calibration", "InvOffsetX", buf, g_iniPath);
    sprintf_s(buf, "%d", g_invOffsetY);
    WritePrivateProfileStringA("Calibration", "InvOffsetY", buf, g_iniPath);
    sprintf_s(buf, "%d", (int)roundf(g_camSpeedX * 10.0f));
    WritePrivateProfileStringA("Settings", "CamSpeedX_X10", buf, g_iniPath);
    sprintf_s(buf, "%d", (int)roundf(g_camSpeedY * 10.0f));
    WritePrivateProfileStringA("Settings", "CamSpeedY_X10", buf, g_iniPath);
    WritePrivateProfileStringA("HUD", "ShowCombatHUD", g_showCombatHUD ? "1" : "0", g_iniPath);
}

static void LoadConfig() {
    if (!g_iniPath[0]) return;
    for (int i = 0; i < ACT_COUNT; i++) {
        g_binds[i].padMask = GetPrivateProfileIntA("PadButtons", g_binds[i].iniKey, g_binds[i].padMask, g_iniPath);
        g_binds[i].keyCombo = GetPrivateProfileIntA("KeyCombos", g_binds[i].iniKey, g_binds[i].keyCombo, g_iniPath);
    }
    g_invOffsetX = GetPrivateProfileIntA("Calibration", "InvOffsetX", 26, g_iniPath);
    g_invOffsetY = GetPrivateProfileIntA("Calibration", "InvOffsetY", 210, g_iniPath);

    int legacyCs = GetPrivateProfileIntA("Settings", "CamSpeedX10", 45, g_iniPath);
    int csX = GetPrivateProfileIntA("Settings", "CamSpeedX_X10", legacyCs, g_iniPath);
    int csY = GetPrivateProfileIntA("Settings", "CamSpeedY_X10", 30, g_iniPath);
    if (csX >= 5 && csX <= 350) g_camSpeedX = csX / 10.0f;
    if (csY >= 5 && csY <= 350) g_camSpeedY = csY / 10.0f;

    g_showCombatHUD = GetPrivateProfileIntA("HUD", "ShowCombatHUD", 1, g_iniPath) != 0;
}

static void InitXInput() {
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3; i++) {
        HMODULE mod = LoadLibraryA(dlls[i]);
        if (mod) {
            g_pfnXInputGetState = (PFN_XInputGetState)GetProcAddress(mod, "XInputGetState");
            if (g_pfnXInputGetState) break;
        }
    }
}

static float ApplyRadialDeadzone(float& x, float& y, float dz) {
    float mag = sqrtf(x * x + y * y);
    if (mag < dz) {
        x = 0.0f;
        y = 0.0f;
        return 0.0f;
    }
    float normMag = (mag - dz) / (1.0f - dz);
    if (normMag > 1.0f) normMag = 1.0f;
    x = (x / mag) * normMag;
    y = (y / mag) * normMag;
    return normMag;
}

static PadState ReadGamepad() {
    PadState st = {};

    if (g_pfnXInputGetState) {
        XINPUT_STATE xst;
        ZeroMemory(&xst, sizeof(xst));
        if (g_pfnXInputGetState(0, &xst) == ERROR_SUCCESS) {
            st.connected = true;
            WORD b = xst.Gamepad.wButtons;
            if (b & XINPUT_GAMEPAD_DPAD_UP)        st.buttons |= PAD_UP;
            if (b & XINPUT_GAMEPAD_DPAD_DOWN)      st.buttons |= PAD_DOWN;
            if (b & XINPUT_GAMEPAD_DPAD_LEFT)      st.buttons |= PAD_LEFT;
            if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     st.buttons |= PAD_RIGHT;
            if (b & XINPUT_GAMEPAD_Y)              st.buttons |= PAD_BTN1;
            if (b & XINPUT_GAMEPAD_B)              st.buttons |= PAD_BTN2;
            if (b & XINPUT_GAMEPAD_A)              st.buttons |= PAD_BTN3;
            if (b & XINPUT_GAMEPAD_X)              st.buttons |= PAD_BTN4;
            if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  st.buttons |= PAD_BTN5;
            if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) st.buttons |= PAD_BTN6;
            if (xst.Gamepad.bLeftTrigger > 100)    st.buttons |= PAD_BTN7;
            if (xst.Gamepad.bRightTrigger > 100)   st.buttons |= PAD_BTN8;
            if (b & XINPUT_GAMEPAD_BACK)           st.buttons |= PAD_BTN9;
            if (b & XINPUT_GAMEPAD_START)          st.buttons |= PAD_BTN10;
            if (b & XINPUT_GAMEPAD_LEFT_THUMB)     st.buttons |= PAD_BTN11;
            if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    st.buttons |= PAD_BTN12;

            st.lx = xst.Gamepad.sThumbLX / 32767.0f;
            st.ly = -xst.Gamepad.sThumbLY / 32767.0f;
            ApplyRadialDeadzone(st.lx, st.ly, 0.20f);

            st.rx = xst.Gamepad.sThumbRX / 32767.0f;
            st.ry = -xst.Gamepad.sThumbRY / 32767.0f;
            ApplyRadialDeadzone(st.rx, st.ry, 0.14f);
            return st;
        }
    }

    JOYINFOEX ji;
    ji.dwSize = sizeof(ji);
    ji.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx(JOYSTICKID1, &ji) == JOYERR_NOERROR) {
        st.connected = true;
        st.lx = (ji.dwXpos - 32767.5f) / 32767.5f;
        st.ly = (ji.dwYpos - 32767.5f) / 32767.5f;
        ApplyRadialDeadzone(st.lx, st.ly, 0.22f);

        st.rx = (ji.dwZpos - 32767.5f) / 32767.5f;
        st.ry = (ji.dwRpos - 32767.5f) / 32767.5f;
        ApplyRadialDeadzone(st.rx, st.ry, 0.16f);

        if (ji.dwPOV != JOY_POVCENTERED) {
            DWORD pov = ji.dwPOV;
            if (pov >= 31500 || pov <= 4500)  st.buttons |= PAD_UP;
            if (pov >= 4500 && pov <= 13500) st.buttons |= PAD_RIGHT;
            if (pov >= 13500 && pov <= 22500) st.buttons |= PAD_DOWN;
            if (pov >= 22500 && pov <= 31500) st.buttons |= PAD_LEFT;
        }

        DWORD btn = ji.dwButtons;
        for (int i = 0; i < 12; i++) {
            if (btn & (1 << i)) st.buttons |= (PAD_BTN1 << i);
        }
    }
    return st;
}

static void SendRawKey(WORD vk, bool down) {
    if (vk >= 256) return;
    if (g_heldKeys[vk] == down) return;
    g_heldKeys[vk] = down;
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    inp.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    inp.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &inp, sizeof(INPUT));
}

static void ReleaseAllHeldKeys() {
    for (int vk = 1; vk < 256; vk++) {
        if (g_heldKeys[vk]) {
            SendRawKey((WORD)vk, false);
        }
    }
}

static bool IsPhysicalKeyboardPressed() {
    static const int checkKeys[] = {
        'W', 'A', 'S', 'D', 'Q', 'E', 'R', 'T', 'Z', 'X', 'C', 'V', 'B',
        'I', 'K', 'M', 'N', 'O', 'P', 'G', 'H', 'J', 'L',
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        VK_SPACE, VK_RETURN, VK_ESCAPE, VK_TAB,
        VK_F1, VK_F2, VK_F3, VK_F4, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT
    };
    for (int vk : checkKeys) {
        if (g_heldKeys[vk]) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
            return true;
        }
    }
    return false;
}

static void MoveMouseClient(int clientX, int clientY) {
    if (!g_gameWnd) return;
    g_lastSyntheticMouseTick = GetTickCount();
    g_cursorClientX = clientX;
    g_cursorClientY = clientY;
    POINT pt = { clientX, clientY };
    ClientToScreen(g_gameWnd, &pt);
    SetCursorPos(pt.x, pt.y);
}

static void ClickMouse(bool rightClick) {
    g_lastSyntheticMouseTick = GetTickCount();
    INPUT inp[2] = {};
    inp[0].type = INPUT_MOUSE;
    inp[0].mi.dwFlags = rightClick ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    inp[1].type = INPUT_MOUSE;
    inp[1].mi.dwFlags = rightClick ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(2, inp, sizeof(INPUT));
}

// =========================================================================
// 4. METIN2 ORIJINAL UI OVERLAY & CANLI SLOT PANELI
// =========================================================================
static void UpdateInvGridOrigin() {
    if (!g_gameWnd) return;
    if (g_gpBridge && g_gpBridge->invLeft > 0 && g_gpBridge->invTop > 0) {
        g_invGridLeft = g_gpBridge->invLeft;
        g_invGridTop = g_gpBridge->invTop;
        return;
    }
    RECT rc;
    GetClientRect(g_gameWnd, &rc);
    int invLeft = (rc.right - rc.left) - 176 - 5;
    int invTop = (rc.bottom - rc.top) - 505 - 36;
    g_invGridLeft = invLeft + g_invOffsetX - 16;
    g_invGridTop = invTop + g_invOffsetY - 16;
}

static void EnterInventoryMode();
static void ExitInventoryMode();

static void SyncBridgeFromD3D9() {
    if (!EnsureGamepadBridge() || !g_gpBridge) return;

    InterlockedExchange(&g_gpBridge->hideCursor, g_hideGameCursor ? 1 : 0);

    if (g_gpBridge->invLeft > 0 && g_gpBridge->invTop > 0) {
        g_invGridLeft = g_gpBridge->invLeft;
        g_invGridTop = g_gpBridge->invTop;
    }

    bool actualInvOpen = (g_gpBridge->invOpen != 0);
    if (actualInvOpen && !g_invOpen) EnterInventoryMode();
    else if (!actualInvOpen && g_invOpen) ExitInventoryMode();

    if (g_invOpen) {
        for (int i = 0; i < 45; i++) {
            BYTE sp = g_gpBridge->slotHeight[i];
            g_slotSpanMap[i] = (sp >= 1 && sp <= 3) ? (int)sp : 1;
        }
    }
}

static POINT GetInventorySlotCenter(int col, int row) {
    UpdateInvGridOrigin();
    POINT p = { g_invGridLeft + col * 32 + 16, g_invGridTop + row * 32 + 16 };
    return p;
}

static POINT GetInventoryTabCenter(int tabIdx) {
    UpdateInvGridOrigin();
    POINT p = { g_invGridLeft + 18 + tabIdx * 38, g_invGridTop - 12 };
    return p;
}

static int GetItemRootRow(int col, int row) {
    if (col < 0 || col > 4 || row < 0 || row > 8) return row;
    for (int offset = 0; offset <= 2; offset++) {
        int checkRow = row - offset;
        if (checkRow < 0) break;
        int span = g_slotSpanMap[checkRow * 5 + col];
        if (span >= 2 && checkRow + span - 1 >= row) {
            return checkRow;
        }
    }
    return row;
}

static int GetCurrentSelectedSpan() {
    int rootRow = GetItemRootRow(g_gridCol, g_gridRow);
    int span = g_slotSpanMap[rootRow * 5 + g_gridCol];
    if (span < 1) span = 1;
    if (span > 3) span = 3;
    if (rootRow + span > 9) span = 9 - rootRow;
    return span;
}

static bool IsActionMaskPressed(int actionIdx) {
    DWORD mask = g_binds[actionIdx].padMask;
    return (mask != 0) && ((g_curPadButtons & mask) != 0);
}

static void RenderMetin2Overlay(int screenX, int screenY, int screenW, int screenH, float scale, float offsetX, float offsetY) {
    if (!g_overlayWnd || screenW <= 0 || screenH <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenW;
    bmi.bmiHeader.biHeight = -screenH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBmp) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);
    memset(pBits, 0, screenW * screenH * 4);

    HDC hdcGameWnd = g_gameWnd ? GetDC(g_gameWnd) : NULL;
    bool hasQuickSlots = (g_gpBridge && g_gpBridge->quickSlotX[0] > 0);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);

        bool inGame = (g_gpBridge && g_gpBridge->inGame != 0);

        // =================================================================
        // 1. SOL PANEL (TEMEL KONTROLLER): L-Stick + Eylemler
        // =================================================================
        if (inGame && !g_kbMouseMode && g_showCombatHUD) {
            int panelW = 208;
            int margin = 16;
            int yBottom = screenH - 54;

            int leftH = 164;
            int leftX = margin;
            int leftY = yBottom - leftH;

            g_m2ui.DrawThinBoardGold(g, leftX, leftY, panelW, leftH, 0.86f);
            g_m2ui.DrawM2Text(g, L"TEMEL KONTROLLER", (float)(leftX + 10), (float)(leftY + 6), (float)(panelW - 20), 18.0f,
                11.5f, Gdiplus::Color(255, 255, 215, 0), Gdiplus::StringAlignmentCenter, true);

            bool stickActive = (fabsf(g_curLx) > 0.15f || fabsf(g_curLy) > 0.15f);
            g_m2ui.DrawModernLStick(g, leftX + 37, leftY + 50, g_curLx, g_curLy);
            g_m2ui.DrawM2Text(g, L"Hareket (W/A/S/D)", (float)(leftX + 70), (float)(leftY + 34), 125.0f, 18.0f,
                11.5f, stickActive ? Gdiplus::Color(255, 255, 235, 110) : Gdiplus::Color(255, 245, 230, 184),
                Gdiplus::StringAlignmentNear, false);
            g_m2ui.DrawM2Text(g, L"Sol Analog", (float)(leftX + 70), (float)(leftY + 51), 125.0f, 16.0f,
                10.5f, Gdiplus::Color(200, 190, 175, 140), Gdiplus::StringAlignmentNear, false);

            g_m2ui.DrawActionButton(g, leftX + 12, leftY + 80, 52, 21,
                PadMaskToShortWName(g_binds[ACT_HORSE_RIDE].padMask), L"Ata İn / Bin", IsActionMaskPressed(ACT_HORSE_RIDE));
            g_m2ui.DrawActionButton(g, leftX + 12, leftY + 107, 52, 21,
                PadMaskToShortWName(g_binds[ACT_PICKUP].padMask), L"Eşya Topla (Z)", IsActionMaskPressed(ACT_PICKUP));
            g_m2ui.DrawActionButton(g, leftX + 12, leftY + 134, 52, 21,
                PadMaskToShortWName(g_binds[ACT_INVENTORY].padMask), L"Envanter Aç/Kapat", IsActionMaskPressed(ACT_INVENTORY));
        }

        // =================================================================
        // 2. ENVANTER ACIKSA: Orijinal slotactiveeffect + Alt Ipucu Kutusu
        // =================================================================
        if (inGame && g_invOpen && !g_kbMouseMode) {
            UpdateInvGridOrigin();
            int rootRow = GetItemRootRow(g_gridCol, g_gridRow);
            int span = GetCurrentSelectedSpan();

            int slotW = (int)roundf(32.0f * scale);
            int slotH = (int)roundf(32.0f * scale);
            int drawX = (int)roundf((offsetX - screenX) + (g_invGridLeft + g_gridCol * 32) * scale);
            int drawY = (int)roundf((offsetY - screenY) + (g_invGridTop + rootRow * 32) * scale);

            g_m2ui.DrawSlotActiveEffect(g, drawX, drawY, slotW, slotH, span);

            int barW = (int)roundf(188.0f * scale);
            int barH = (int)roundf(44.0f * scale);
            int barX = (int)roundf((offsetX - screenX) + (g_invGridLeft - 14) * scale);
            int barY = (int)roundf((offsetY - screenY) + (g_invGridTop + 9 * 32 + 8) * scale);

            g_m2ui.DrawThinBoardGold(g, barX, barY, barW, barH, 0.90f);

            wchar_t line1[128], line2[128];
            swprintf_s(line1, L"[%s] Kullan/Giy   [%s] Tut/Bırak",
                PadMaskToShortWName(g_binds[ACT_INV_RCLICK].padMask),
                PadMaskToShortWName(g_binds[ACT_INV_LCLICK].padMask));
            swprintf_s(line2, L"[R-Stick] Gezin  [%s/%s] Sayfa",
                PadMaskToShortWName(g_binds[ACT_INV_PREVTAB].padMask),
                PadMaskToShortWName(g_binds[ACT_INV_NEXTTAB].padMask));

            float fSize = 10.5f * (scale > 1.2f ? 1.25f : 1.0f);
            g_m2ui.DrawM2Text(g, line1, (float)(barX + 8), (float)(barY + 3), (float)(barW - 16), (float)(barH / 2 - 2), fSize);
            g_m2ui.DrawM2Text(g, line2, (float)(barX + 8), (float)(barY + barH / 2 - 1), (float)(barW - 16), (float)(barH / 2 - 2),
                fSize, Gdiplus::Color(255, 255, 215, 0));
        }

        // =================================================================
        // 3. ENVANTER KAPALI & HUD ACIKSA:
        //    Sol Elmas = D-Pad (F1-F4), Sag Elmas = Yuz Tuslari (1-4)
        // =================================================================
        if (inGame && !g_invOpen && !g_kbMouseMode && g_showCombatHUD) {
            bool l2Active = IsActionMaskPressed(ACT_MOD_LAYER);

            int panelW = 292;
            int margin = 16;
            int yBottom = screenH - 54;

            int rightH = 212;
            int rightX = screenW - panelW - margin;
            int rightY = yBottom - rightH;

            g_m2ui.DrawThinBoardGold(g, rightX, rightY, panelW, rightH, 0.86f);

            std::wstring layerTitle = l2Active ? L"2. KATMAN (L2 BASILI)" : L"SAVAŞ & HIZLI SLOTLAR";
            Gdiplus::Color titleCol = l2Active ? Gdiplus::Color(255, 115, 220, 255) : Gdiplus::Color(255, 255, 215, 0);
            g_m2ui.DrawM2Text(g, layerTitle, (float)(rightX + 10), (float)(rightY + 6), (float)(panelW - 20), 18.0f,
                11.5f, titleCol, Gdiplus::StringAlignmentCenter, true);

            g_m2ui.DrawActionButton(g, rightX + 68, rightY + 29, 54, 21,
                PadMaskToShortWName(g_binds[ACT_ATTACK].padMask), L"Saldırı (Space)", IsActionMaskPressed(ACT_ATTACK));

            int dSz = 36;
            int dOff = 44;
            int cy = rightY + 132;

            // SOL ELMAS: D-Pad (Yukari=F1, Sag=F2, Asagi=F3, Sol=F4)
            int leftCx = rightX + 76;
            Gdiplus::SolidBrush coreBr(Gdiplus::Color(225, 26, 20, 14));
            Gdiplus::Pen corePen(Gdiplus::Color(220, 165, 134, 72), 1.2f);
            g.FillRectangle(&coreBr, leftCx - 16, cy - 16, 32, 32);
            g.DrawRectangle(&corePen, leftCx - 16, cy - 16, 32, 32);

            g_m2ui.DrawM2Text(g, L"▲", (float)(leftCx - 8), (float)(cy - 18), 16.0f, 12.0f, 9.5f,
                IsActionMaskPressed(ACT_DPAD_UP) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"▼", (float)(leftCx - 8), (float)(cy + 6), 16.0f, 12.0f, 9.5f,
                IsActionMaskPressed(ACT_DPAD_DOWN) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"◄", (float)(leftCx - 17), (float)(cy - 6), 12.0f, 12.0f, 9.5f,
                IsActionMaskPressed(ACT_DPAD_LEFT) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"►", (float)(leftCx + 5), (float)(cy - 6), 12.0f, 12.0f, 9.5f,
                IsActionMaskPressed(ACT_DPAD_RIGHT) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);

            g_m2ui.DrawCleanSlotBox(g, leftCx - dSz / 2, cy - dOff - dSz / 2 + 4, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[4] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[4] : -1, hasQuickSlots,
                IsActionMaskPressed(ACT_DPAD_UP));
            g_m2ui.DrawCleanSlotBox(g, leftCx - dSz / 2, cy + dOff - dSz / 2 - 4, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[6] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[6] : -1, hasQuickSlots,
                IsActionMaskPressed(ACT_DPAD_DOWN));
            g_m2ui.DrawCleanSlotBox(g, leftCx - dOff - dSz / 2 + 4, cy - dSz / 2, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[7] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[7] : -1, hasQuickSlots,
                IsActionMaskPressed(ACT_DPAD_LEFT));
            g_m2ui.DrawCleanSlotBox(g, leftCx + dOff - dSz / 2 - 4, cy - dSz / 2, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[5] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[5] : -1, hasQuickSlots,
                IsActionMaskPressed(ACT_DPAD_RIGHT));

            // SAG ELMAS: Yuz Tuslari (Ust=Y/▲, Sag=B/●, Alt=A/✖, Sol=X/■)
            int rightCx = rightX + 216;
            g.FillRectangle(&coreBr, rightCx - 16, cy - 16, 32, 32);
            g.DrawRectangle(&corePen, rightCx - 16, cy - 16, 32, 32);

            int actTop = l2Active ? ACT_L2_ACT2 : ACT_SLOT2;
            int actRight = l2Active ? ACT_L2_ACT3 : ACT_SLOT3;
            int actBot = l2Active ? ACT_L2_ACT4 : ACT_SLOT4;
            int actLeft = l2Active ? ACT_L2_ACT1 : ACT_SLOT1;

            g_m2ui.DrawM2Text(g, L"Y", (float)(rightCx - 8), (float)(cy - 18), 16.0f, 12.0f, 9.5f,
                IsActionMaskPressed(actTop) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"A", (float)(rightCx - 8), (float)(cy + 6), 16.0f, 12.0f, 9.5f,
                IsActionMaskPressed(actBot) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"X", (float)(rightCx - 17), (float)(cy - 6), 12.0f, 12.0f, 9.5f,
                IsActionMaskPressed(actLeft) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);
            g_m2ui.DrawM2Text(g, L"B", (float)(rightCx + 5), (float)(cy - 6), 12.0f, 12.0f, 9.5f,
                IsActionMaskPressed(actRight) ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 255, 215, 0),
                Gdiplus::StringAlignmentCenter, false);

            int baseSlot = l2Active ? 4 : 0;
            g_m2ui.DrawCleanSlotBox(g, rightCx - dSz / 2, cy - dOff - dSz / 2 + 4, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[baseSlot + 1] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[baseSlot + 1] : -1, hasQuickSlots,
                IsActionMaskPressed(actTop));
            g_m2ui.DrawCleanSlotBox(g, rightCx - dSz / 2, cy + dOff - dSz / 2 - 4, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[baseSlot + 3] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[baseSlot + 3] : -1, hasQuickSlots,
                IsActionMaskPressed(actBot));
            g_m2ui.DrawCleanSlotBox(g, rightCx - dOff - dSz / 2 + 4, cy - dSz / 2, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[baseSlot + 0] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[baseSlot + 0] : -1, hasQuickSlots,
                IsActionMaskPressed(actLeft));
            g_m2ui.DrawCleanSlotBox(g, rightCx + dOff - dSz / 2 - 4, cy - dSz / 2, dSz,
                hdcGameWnd, hasQuickSlots ? g_gpBridge->quickSlotX[baseSlot + 2] : -1, hasQuickSlots ? g_gpBridge->quickSlotY[baseSlot + 2] : -1, hasQuickSlots,
                IsActionMaskPressed(actRight));
        }
    }

    if (hdcGameWnd) ReleaseDC(g_gameWnd, hdcGameWnd);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { screenX, screenY };
    SIZE szWnd = { screenW, screenH };
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_overlayWnd, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void UpdateInventoryOverlay() {
    if (!g_gameWnd) return;

    if (!g_overlayWnd) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = g_hModule;
        wc.lpszClassName = "M2Metin2ThemedOverlayV21";
        RegisterClassA(&wc);

        g_overlayWnd = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "M2Metin2ThemedOverlayV21", "",
            WS_POPUP,
            0, 0, 100, 100,
            NULL, NULL, g_hModule, NULL);
    }

    if (!g_overlayWnd) return;

    bool inGame = (g_gpBridge && g_gpBridge->inGame != 0);
    bool shouldShow = inGame && !g_kbMouseMode && IsGameOrMagpieForeground() && (g_invOpen || g_showCombatHUD);

    if (shouldShow) {
        MagpieTransform mp = GetMagpieTransform();
        int scrX = 0, scrY = 0, scrW = 0, scrH = 0;
        float scale = 1.0f, offX = 0.0f, offY = 0.0f;

        if (mp.active) {
            RECT dstRc;
            GetWindowRect(mp.hwnd, &dstRc);
            scrX = dstRc.left;
            scrY = dstRc.top;
            scrW = dstRc.right - dstRc.left;
            scrH = dstRc.bottom - dstRc.top;
            scale = mp.scale;
            offX = mp.offsetX;
            offY = mp.offsetY;
        }
        else {
            RECT rc;
            GetClientRect(g_gameWnd, &rc);
            POINT tl = { 0, 0 };
            ClientToScreen(g_gameWnd, &tl);
            scrX = tl.x;
            scrY = tl.y;
            scrW = rc.right - rc.left;
            scrH = rc.bottom - rc.top;
            scale = 1.0f;
            offX = (float)tl.x;
            offY = (float)tl.y;
        }

        if (!IsWindowVisible(g_overlayWnd)) {
            ShowWindow(g_overlayWnd, SW_SHOWNOACTIVATE);
        }
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, scrX, scrY, scrW, scrH, SWP_NOACTIVATE);
        RenderMetin2Overlay(scrX, scrY, scrW, scrH, scale, offX, offY);
    }
    else {
        if (IsWindowVisible(g_overlayWnd)) {
            ShowWindow(g_overlayWnd, SW_HIDE);
        }
    }
}

static void MoveCursorToGridSlot(int col, int row) {
    SyncBridgeFromD3D9();
    int rootRow = GetItemRootRow(col, row);
    g_gridCol = col;
    g_gridRow = rootRow;

    int span = GetCurrentSelectedSpan();
    POINT p = GetInventorySlotCenter(g_gridCol, g_gridRow);
    p.y += (span - 1) * 16;

    MoveMouseClient(p.x, p.y);
    UpdateInventoryOverlay();
}

static void StepInventoryGrid(DWORD stepDir) {
    SyncBridgeFromD3D9();
    int rootRow = GetItemRootRow(g_gridCol, g_gridRow);

    if (stepDir & PAD_DOWN) {
        int span = GetCurrentSelectedSpan();
        int nextRow = rootRow + span;
        g_gridRow = (nextRow <= 8) ? nextRow : 0;
    }
    if (stepDir & PAD_UP) {
        if (rootRow > 0) {
            g_gridRow = GetItemRootRow(g_gridCol, rootRow - 1);
        }
        else {
            g_gridRow = GetItemRootRow(g_gridCol, 8);
        }
    }
    if (stepDir & PAD_LEFT) {
        g_gridCol = (g_gridCol > 0) ? g_gridCol - 1 : 4;
        g_gridRow = GetItemRootRow(g_gridCol, g_gridRow);
    }
    if (stepDir & PAD_RIGHT) {
        g_gridCol = (g_gridCol < 4) ? g_gridCol + 1 : 0;
        g_gridRow = GetItemRootRow(g_gridCol, g_gridRow);
    }

    MoveCursorToGridSlot(g_gridCol, g_gridRow);
}

static void SwitchInventoryTab(int newTab) {
    if (newTab < 0) newTab = 3;
    if (newTab > 3) newTab = 0;
    g_invTab = newTab;

    for (int i = 0; i < 45; i++) g_slotSpanMap[i] = 1;

    POINT tabPt = GetInventoryTabCenter(g_invTab);
    MoveMouseClient(tabPt.x, tabPt.y);
    Sleep(20);
    ClickMouse(false);
    Sleep(35);

    MoveCursorToGridSlot(g_gridCol, g_gridRow);
}

static void StopCameraDrag() {
    if (g_camDragging) {
        g_camDragging = false;
        g_camAccumX = 0.0f;
        g_camAccumY = 0.0f;
        g_lastSyntheticMouseTick = GetTickCount();
        INPUT inp = {};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(1, &inp, sizeof(INPUT));
    }
}

static void EnterInventoryMode() {
    if (g_invOpen) return;
    g_invOpen = true;
    StopCameraDrag();
    UpdateInvGridOrigin();
    SyncBridgeFromD3D9();
    if (!g_kbMouseMode) {
        MoveCursorToGridSlot(g_gridCol, g_gridRow);
    }
    UpdateInventoryOverlay();
}

static void ExitInventoryMode() {
    if (!g_invOpen) return;
    g_invOpen = false;
    SyncBridgeFromD3D9();
    UpdateInventoryOverlay();
}

static void SendCombo(DWORD combo, bool down) {
    WORD vk = (WORD)(combo & 0xFFFF);
    if (vk == 0) return;

    if (vk == KEY_SPECIAL_LCLICK) { if (down) ClickMouse(false); return; }
    if (vk == KEY_SPECIAL_RCLICK) { if (down) ClickMouse(true);  return; }
    if (vk == KEY_SPECIAL_PREVTAB) { if (down) SwitchInventoryTab(g_invTab - 1); return; }
    if (vk == KEY_SPECIAL_NEXTTAB) { if (down) SwitchInventoryTab(g_invTab + 1); return; }

    if (down) {
        if (combo & MOD_FLAG_CTRL)  SendRawKey(VK_LCONTROL, true);
        if (combo & MOD_FLAG_ALT)   SendRawKey(VK_LMENU, true);
        if (combo & MOD_FLAG_SHIFT) SendRawKey(VK_LSHIFT, true);
        SendRawKey(vk, true);
    }
    else {
        SendRawKey(vk, false);
        if (combo & MOD_FLAG_SHIFT) SendRawKey(VK_LSHIFT, false);
        if (combo & MOD_FLAG_ALT)   SendRawKey(VK_LMENU, false);
        if (combo & MOD_FLAG_CTRL)  SendRawKey(VK_LCONTROL, false);
    }
}

static void TapCombo(DWORD combo) {
    WORD vk = (WORD)(combo & 0xFFFF);
    if (vk == 0) return;
    if (vk >= KEY_SPECIAL_LCLICK && vk <= KEY_SPECIAL_NEXTTAB) {
        SendCombo(combo, true);
        return;
    }
    if (combo & MOD_FLAG_CTRL) { SendRawKey(VK_LCONTROL, true); Sleep(10); }
    if (combo & MOD_FLAG_ALT) { SendRawKey(VK_LMENU, true);    Sleep(10); }
    if (combo & MOD_FLAG_SHIFT) { SendRawKey(VK_LSHIFT, true);   Sleep(10); }

    SendRawKey(vk, true);
    Sleep(20);
    SendRawKey(vk, false);

    if (combo & MOD_FLAG_SHIFT) { Sleep(8); SendRawKey(VK_LSHIFT, false); }
    if (combo & MOD_FLAG_ALT) { Sleep(8); SendRawKey(VK_LMENU, false); }
    if (combo & MOD_FLAG_CTRL) { Sleep(8); SendRawKey(VK_LCONTROL, false); }
}

// =========================================================================
// 5. METIN2 ORIJINAL BOARD TASARIMLI F8 KONTROL MERKEZI (PER-PIXEL ALPHA)
// =========================================================================
static const int UI_WIN_W = 560;
static const int UI_WIN_H = 736;

// GitHub kullanici adini ve bagis linkini buraya yaz:
static const wchar_t* URL_FEEDBACK = L"https://github.com/KULLANICI_ADIN/Metin2-Gamepad-Support/issues/new/choose";
static const wchar_t* URL_DONATE = L"https://buymeacoffee.com/KULLANICI_ADIN";

static int g_uiHoverId = 0;

static void AssignPadMaskWithConflictCheck(int targetIdx, DWORD newMask) {
    int targetTab = g_binds[targetIdx].tabId;
    bool targetIsL2 = (targetIdx >= ACT_L2_ACT1 && targetIdx <= ACT_L2_ACT4);

    for (int i = 0; i < ACT_COUNT; i++) {
        if (i == targetIdx) continue;
        if (g_binds[i].padMask != newMask) continue;

        bool globalConflict = (i == ACT_INVENTORY || targetIdx == ACT_INVENTORY);
        bool sameTabConflict = (g_binds[i].tabId == targetTab) &&
            (targetIsL2 == (i >= ACT_L2_ACT1 && i <= ACT_L2_ACT4)) &&
            (i != ACT_MOD_LAYER && targetIdx != ACT_MOD_LAYER);

        if (globalConflict || sameTabConflict) {
            int res = MessageBoxW(g_uiWnd,
                L"Bu kol tuşu başka bir eyleme atanmış durumda!\n\nÖnceki atamayı kaldırıp buraya atamak ister misiniz?",
                L"Tuş Çakışması Uyarısı", MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
            if (res == IDYES) {
                g_binds[i].padMask = 0;
            }
            else {
                return;
            }
        }
    }
    g_binds[targetIdx].padMask = newMask;
}

static int HitTestConfigUI(int mx, int my) {
    // Sag ust X butonu
    if (mx >= UI_WIN_W - 35 && mx <= UI_WIN_W - 18 && my >= 15 && my <= 32) return 1;

    // Envanter sekmesi stilindeki Savas / Genel & Envanter Tuslari sekmeleri
    if (my >= 42 && my <= 65) {
        if (mx >= 20 && mx <= 276) return 2;
        if (mx >= 284 && mx <= 540) return 3;
    }

    int visIdx = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (g_binds[i].tabId != g_activeUiTab) continue;
        int y = 76 + visIdx * 25;
        if (my >= y && my <= y + 21) {
            if (mx >= 220 && mx <= 375) return 100 + i;
            if (mx >= 382 && mx <= 510) {
                if (i != ACT_MOD_LAYER && !(g_binds[i].keyCombo >= KEY_SPECIAL_PREVTAB && g_binds[i].keyCombo <= KEY_SPECIAL_NEXTTAB))
                    return 200 + i;
            }
            if (mx >= 516 && mx <= 536) return 300 + i;
        }
        visIdx++;
    }

    // 1. Alt Satir: Yatay Hiz & HUD Butonu
    int ctrlY1 = 626;
    if (my >= ctrlY1 && my <= ctrlY1 + 21) {
        if (mx >= 20 && mx <= 90) return 10;
        if (mx >= 198 && mx <= 268) return 11;
        if (mx >= 310 && mx <= 540) return 12;
    }

    // 2. Alt Satir: Dikey Hiz & Kaydet & Kapat
    int ctrlY2 = 656;
    if (my >= ctrlY2 && my <= ctrlY2 + 21) {
        if (mx >= 20 && mx <= 90) return 13;
        if (mx >= 198 && mx <= 268) return 14;
        if (mx >= 310 && mx <= 540) return 15;
    }

    // 3. Alt Satir: Geri Bildirim & Destek Ol (Bagis)
    int ctrlY3 = 686;
    if (my >= ctrlY3 && my <= ctrlY3 + 21) {
        if (mx >= 20 && mx <= 268) return 16;
        if (mx >= 310 && mx <= 540) return 17;
    }

    return 0;
}

static void RepaintConfigUI() {
    if (!g_uiWnd || !IsWindowVisible(g_uiWnd)) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = UI_WIN_W;
    bmi.bmiHeader.biHeight = -UI_WIN_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBmp) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);
    memset(pBits, 0, UI_WIN_W * UI_WIN_H * 4);

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);

        // 1. Metin2 Tas Board
        g_m2ui.DrawBoard(g, 0, 0, UI_WIN_W, UI_WIN_H, 1.0f);

        // 2. TitleBar & X Butonu
        g_m2ui.DrawTitleBar(g, 13, 12, UI_WIN_W - 26, 23, L"Gamepad Kontrol Merkezi (F8)", g_uiHoverId == 1);

        // 3. Envanter Sekmesi Gorunumunde Sekmeler
        g_m2ui.DrawInventoryStyleTab(g, 20, 42, 256, 23, L"Savaş / Genel", g_activeUiTab == 0, g_uiHoverId == 2);
        g_m2ui.DrawInventoryStyleTab(g, 284, 42, 256, 23, L"Envanter Tuşları", g_activeUiTab == 1, g_uiHoverId == 3);

        // 4. Eylem Satirlari
        int visIdx = 0;
        for (int i = 0; i < ACT_COUNT; i++) {
            if (g_binds[i].tabId != g_activeUiTab) continue;
            int y = 76 + visIdx * 25;

            g_m2ui.DrawM2Text(g, g_binds[i].labelW, 22.0f, (float)(y - 1), 194.0f, 21.0f, 12.0f,
                Gdiplus::Color(255, 236, 232, 224), Gdiplus::StringAlignmentNear, false);

            std::wstring padTxt = (g_listeningAction == i) ? L">> KOL TUŞUNA BAS <<" : PadMaskToFullWName(g_binds[i].padMask);
            Gdiplus::Color padCol = (g_listeningAction == i) ? Gdiplus::Color(255, 115, 225, 255) : Gdiplus::Color(255, 255, 215, 0);
            g_m2ui.DrawM2Button(g, 220, y, 155, 21, padTxt, g_listeningAction == i, g_uiHoverId == 100 + i, padCol);

            wchar_t keyTxt[64];
            if (g_listeningKey == i) wcscpy_s(keyTxt, L">> KLAVYEYE BAS <<");
            else if (i == ACT_MOD_LAYER) wcscpy_s(keyTxt, L"[2. Katman Modu]");
            else KeyComboToWName(g_binds[i].keyCombo, keyTxt, 64);

            Gdiplus::Color keyCol = (g_listeningKey == i) ? Gdiplus::Color(255, 115, 225, 255) : Gdiplus::Color(255, 236, 232, 224);
            g_m2ui.DrawM2Button(g, 382, y, 128, 21, keyTxt, g_listeningKey == i, g_uiHoverId == 200 + i, keyCol);

            // Baslik cubugundaki X ile ayni ikon
            g_m2ui.DrawCloseIconButton(g, 518, y + 3, g_uiHoverId == 300 + i);

            visIdx++;
        }

        // 5. Bilgi Satirlari
        g_m2ui.DrawM2Text(g, L"Envanter Modu: Yalnızca Sağ Analog ile slotlar arası gezinilir (2-3 slotluk eşyalar otomatik atlanır).",
            20.0f, 566.0f, 520.0f, 18.0f, 11.5f, Gdiplus::Color(255, 195, 190, 180), Gdiplus::StringAlignmentCenter, false);
        g_m2ui.DrawM2Text(g, L"Not: Kol üzerindeki MODE / ANALOG ışığının açık olduğundan emin olun.",
            20.0f, 586.0f, 520.0f, 18.0f, 11.5f, Gdiplus::Color(255, 240, 205, 95), Gdiplus::StringAlignmentCenter, false);

        // 6. Alt Kamera Hizi & HUD Kontrol Butonlari
        int ctrlY1 = 626;
        g_m2ui.DrawM2Button(g, 20, ctrlY1, 70, 21, L"Yatay -", false, g_uiHoverId == 10);
        wchar_t bufX[48];
        swprintf_s(bufX, L"Yatay Hız: %.1f", g_camSpeedX);
        g_m2ui.DrawM2Text(g, bufX, 92.0f, (float)(ctrlY1 - 1), 104.0f, 21.0f, 12.0f,
            Gdiplus::Color(255, 236, 232, 224), Gdiplus::StringAlignmentCenter, false);
        g_m2ui.DrawM2Button(g, 198, ctrlY1, 70, 21, L"Yatay +", false, g_uiHoverId == 11);

        g_m2ui.DrawM2Button(g, 310, ctrlY1, 230, 21,
            g_showCombatHUD ? L"Savaş Tuş Paneli (HUD): Açık" : L"Savaş Tuş Paneli (HUD): Kapalı",
            g_showCombatHUD, g_uiHoverId == 12);

        int ctrlY2 = 656;
        g_m2ui.DrawM2Button(g, 20, ctrlY2, 70, 21, L"Dikey -", false, g_uiHoverId == 13);
        wchar_t bufY[48];
        swprintf_s(bufY, L"Dikey Hız: %.1f", g_camSpeedY);
        g_m2ui.DrawM2Text(g, bufY, 92.0f, (float)(ctrlY2 - 1), 104.0f, 21.0f, 12.0f,
            Gdiplus::Color(255, 236, 232, 224), Gdiplus::StringAlignmentCenter, false);
        g_m2ui.DrawM2Button(g, 198, ctrlY2, 70, 21, L"Dikey +", false, g_uiHoverId == 14);

        g_m2ui.DrawM2Button(g, 310, ctrlY2, 230, 21, L"Kaydet & Kapat (F8)", false, g_uiHoverId == 15);

        // 7. Topluluk Satiri: Geri Bildirim & Ikonlu Destek Ol (Bagis) Butonu
        int ctrlY3 = 686;
        g_m2ui.DrawM2Button(g, 20, ctrlY3, 248, 21, L"Geri Bildirim (Öneri / Sorun / Fikir)", false, g_uiHoverId == 16,
            Gdiplus::Color(255, 165, 215, 255));

        g_m2ui.DrawM2IconButton(g, 310, ctrlY3, 230, 21, L"donate_icon", L"Destek Ol (Bağış)", g_uiHoverId == 17,
            Gdiplus::Color(255, 255, 220, 95));
    }

    RECT rcWnd;
    GetWindowRect(g_uiWnd, &rcWnd);
    POINT ptSrc = { 0, 0 };
    POINT ptDst = { rcWnd.left, rcWnd.top };
    SIZE szWnd = { UI_WIN_W, UI_WIN_H };
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_uiWnd, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        int hit = HitTestConfigUI(mx, my);
        if (hit != g_uiHoverId) {
            g_uiHoverId = hit;
            RepaintConfigUI();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        int hit = HitTestConfigUI(mx, my);

        if (my >= 10 && my <= 35 && hit != 1) {
            ReleaseCapture();
            SendMessageA(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        if (hit == 1 || hit == 15) {
            g_listeningAction = -1; g_listeningKey = -1;
            SaveConfig(); ShowWindow(hwnd, SW_HIDE);
            if (g_gameWnd) SetForegroundWindow(g_gameWnd);
        }
        else if (hit == 2) {
            g_activeUiTab = 0; g_listeningAction = -1; g_listeningKey = -1;
            RepaintConfigUI();
        }
        else if (hit == 3) {
            g_activeUiTab = 1; g_listeningAction = -1; g_listeningKey = -1;
            RepaintConfigUI();
        }
        else if (hit >= 100 && hit < 100 + ACT_COUNT) {
            g_listeningKey = -1; g_listeningAction = hit - 100;
            RepaintConfigUI();
        }
        else if (hit >= 200 && hit < 200 + ACT_COUNT) {
            g_listeningAction = -1; g_listeningKey = hit - 200;
            RepaintConfigUI();
        }
        else if (hit >= 300 && hit < 300 + ACT_COUNT) {
            g_binds[hit - 300].padMask = 0;
            g_listeningAction = -1; g_listeningKey = -1;
            SaveConfig(); RepaintConfigUI();
        }
        else if (hit == 10) {
            if (g_camSpeedX > 0.5f) g_camSpeedX -= 0.5f;
            SaveConfig(); RepaintConfigUI();
        }
        else if (hit == 11) {
            if (g_camSpeedX < 35.0f) g_camSpeedX += 0.5f;
            SaveConfig(); RepaintConfigUI();
        }
        else if (hit == 12) {
            g_showCombatHUD = !g_showCombatHUD;
            SaveConfig(); RepaintConfigUI(); UpdateInventoryOverlay();
        }
        else if (hit == 13) {
            if (g_camSpeedY > 0.5f) g_camSpeedY -= 0.5f;
            SaveConfig(); RepaintConfigUI();
        }
        else if (hit == 14) {
            if (g_camSpeedY < 35.0f) g_camSpeedY += 0.5f;
            SaveConfig(); RepaintConfigUI();
        }
        else if (hit == 16) {
            ShellExecuteW(NULL, L"open", URL_FEEDBACK, NULL, NULL, SW_SHOWNORMAL);
        }
        else if (hit == 17) {
            ShellExecuteW(NULL, L"open", URL_DONATE, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        WORD vk = (WORD)wParam;
        if (vk == VK_F8 || vk == VK_ESCAPE) {
            g_listeningAction = -1; g_listeningKey = -1;
            SaveConfig(); ShowWindow(hwnd, SW_HIDE);
            if (g_gameWnd) SetForegroundWindow(g_gameWnd);
            return 0;
        }
        if (g_listeningKey >= 0 && g_listeningKey < ACT_COUNT) {
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return 0;
            DWORD combo = vk;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) combo |= MOD_FLAG_CTRL;
            if (GetAsyncKeyState(VK_MENU) & 0x8000) combo |= MOD_FLAG_ALT;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) combo |= MOD_FLAG_SHIFT;

            g_binds[g_listeningKey].keyCombo = combo;
            g_listeningKey = -1;
            SaveConfig(); RepaintConfigUI();
        }
        return 0;
    }
    case WM_CLOSE:
        g_listeningAction = -1; g_listeningKey = -1;
        SaveConfig(); ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void ToggleConfigUI() {
    if (!g_uiWnd) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = ConfigWndProc;
        wc.hInstance = g_hModule;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "M2GamepadMetin2BoardWndV5";
        RegisterClassA(&wc);

        int scrW = GetSystemMetrics(SM_CXSCREEN);
        int scrH = GetSystemMetrics(SM_CYSCREEN);

        g_uiWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            "M2GamepadMetin2BoardWndV5", "Metin2 Gamepad Kontrol Merkezi",
            WS_POPUP,
            (scrW - UI_WIN_W) / 2, (scrH - UI_WIN_H) / 2, UI_WIN_W, UI_WIN_H,
            NULL, NULL, g_hModule, NULL);
    }

    if (IsWindowVisible(g_uiWnd)) {
        g_listeningAction = -1; g_listeningKey = -1;
        SaveConfig(); ShowWindow(g_uiWnd, SW_HIDE);
        if (g_gameWnd) SetForegroundWindow(g_gameWnd);
    }
    else {
        ShowWindow(g_uiWnd, SW_SHOW);
        SetForegroundWindow(g_uiWnd);
        RepaintConfigUI();
    }
}

// =========================================================================
// 6. ANA PENCERE & WORKER THREAD
// =========================================================================
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if ((rc.right - rc.left) >= 640 && (rc.bottom - rc.top) >= 480) {
            g_gameWnd = hwnd;
            UpdateInvGridOrigin();
            return FALSE;
        }
    }
    return TRUE;
}

static DWORD WINAPI GamepadWorker(LPVOID) {
    timeBeginPeriod(1);

    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipInput, NULL);
    g_m2ui.Init(L"C:\\Users\\Baki\\Desktop\\ymir work\\ui");

    InitXInput();
    LoadConfig();
    for (int i = 0; i < 45; i++) g_slotSpanMap[i] = 1;

    DWORD prevButtons = 0;
    bool prevF8 = false, prevF11 = false;

    DWORD navHoldDir = 0;
    DWORD navNextRepeatTick = 0;
    DWORD lastOverlayTick = 0;

    POINT lastPhysicalScreenPt = { -9999, -9999 };

    while (g_running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!g_gameWnd || !IsWindow(g_gameWnd)) {
            EnumWindows(EnumWindowsProc, 0);
            Sleep(100);
            continue;
        }

        SyncBridgeFromD3D9();

        POINT curScreenPt;
        GetCursorPos(&curScreenPt);
        POINT curClient = curScreenPt;
        ScreenToClient(g_gameWnd, &curClient);
        g_cursorClientX = curClient.x;
        g_cursorClientY = curClient.y;

        bool curF8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (curF8 && !prevF8) {
            if (IsGameOrMagpieForeground() || (g_uiWnd && GetForegroundWindow() == g_uiWnd)) {
                StopCameraDrag();
                g_hideGameCursor = false;
                SyncBridgeFromD3D9();
                ReleaseAllHeldKeys();
                ToggleConfigUI();
            }
        }
        prevF8 = curF8;

        bool curF11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (curF11 && !prevF11 && IsGameOrMagpieForeground()) {
            RECT rc;
            GetClientRect(g_gameWnd, &rc);
            int invLeft = (rc.right - rc.left) - 176 - 5;
            int invTop = (rc.bottom - rc.top) - 505 - 36;
            g_invOffsetX = curClient.x - invLeft;
            g_invOffsetY = curClient.y - invTop;
            SaveConfig();
            UpdateInvGridOrigin();
            UpdateInventoryOverlay();
            MessageBeep(MB_ICONASTERISK);
        }
        prevF11 = curF11;

        PadState st = ReadGamepad();
        DWORD pressed = st.buttons & ~prevButtons;
        DWORD released = ~st.buttons & prevButtons;

        g_curPadButtons = st.buttons;
        g_curLx = st.lx;
        g_curLy = st.ly;

        if (g_uiWnd && IsWindowVisible(g_uiWnd)) {
            g_hideGameCursor = false;
            SyncBridgeFromD3D9();
            UpdateInventoryOverlay();
            if (g_listeningAction >= 0 && pressed != 0) {
                for (int bit = 0; bit < 16; bit++) {
                    if (pressed & (1 << bit)) {
                        int target = g_listeningAction;
                        g_listeningAction = -1;
                        AssignPadMaskWithConflictCheck(target, (1 << bit));
                        SaveConfig();
                        RepaintConfigUI();
                        break;
                    }
                }
            }
            prevButtons = st.buttons;
            Sleep(8);
            continue;
        }

        if (!IsGameOrMagpieForeground()) {
            StopCameraDrag();
            g_hideGameCursor = false;
            SyncBridgeFromD3D9();
            UpdateInventoryOverlay();
            ReleaseAllHeldKeys();
            lastPhysicalScreenPt = curScreenPt;
            Sleep(30);
            continue;
        }

        if (!st.connected) {
            StopCameraDrag();
            g_hideGameCursor = false;
            g_kbMouseMode = true;
            SyncBridgeFromD3D9();
            UpdateInventoryOverlay();
            ReleaseAllHeldKeys();
            lastPhysicalScreenPt = curScreenPt;
            Sleep(50);
            continue;
        }

        bool syntheticRecent = (GetTickCount() - g_lastSyntheticMouseTick) < 70;
        bool physMouseMovedOrClicked = false;
        if (!syntheticRecent && lastPhysicalScreenPt.x != -9999) {
            int dx = abs(curScreenPt.x - lastPhysicalScreenPt.x);
            int dy = abs(curScreenPt.y - lastPhysicalScreenPt.y);
            bool physClick = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            if (dx >= 4 || dy >= 4 || physClick) {
                physMouseMovedOrClicked = true;
            }
        }
        lastPhysicalScreenPt = curScreenPt;

        bool physKbPressed = IsPhysicalKeyboardPressed();

        bool gamepadActiveInput = (st.buttons != 0) ||
            (fabsf(st.lx) > 0.15f) || (fabsf(st.ly) > 0.15f) ||
            (fabsf(st.rx) > 0.20f) || (fabsf(st.ry) > 0.20f);

        bool prevKbMouseMode = g_kbMouseMode;
        if (gamepadActiveInput) {
            g_kbMouseMode = false;
        }
        else if (physMouseMovedOrClicked || physKbPressed) {
            g_kbMouseMode = true;
        }

        if (prevKbMouseMode != g_kbMouseMode) {
            UpdateInventoryOverlay();
        }

        bool isInGame = (g_gpBridge && g_gpBridge->inGame != 0);
        bool manualFreeCursor = ((st.buttons & PAD_BTN8) != 0);
        g_hideGameCursor = isInGame && !manualFreeCursor && !g_kbMouseMode;
        SyncBridgeFromD3D9();

        // -----------------------------------------------------------------
        // 1. SIFIR SAG-SOL SALLANMALI HISTEREZISLI 8-YON HAREKET
        // -----------------------------------------------------------------
        DWORD now = GetTickCount();
        static int s_lockedDir8 = -1;
        float lMag = sqrtf(st.lx * st.lx + st.ly * st.ly);
        bool curW = false, curS = false, curA = false, curD = false;

        if (lMag > 0.22f) {
            float deg = atan2f(st.ly, st.lx) * (180.0f / 3.14159265f);
            if (deg < 0.0f) deg += 360.0f;

            int candidateDir = ((int)roundf(deg / 45.0f)) % 8;

            if (s_lockedDir8 < 0) {
                s_lockedDir8 = candidateDir;
            }
            else if (candidateDir != s_lockedDir8) {
                float lockedCenter = s_lockedDir8 * 45.0f;
                float diff = fabsf(deg - lockedCenter);
                if (diff > 180.0f) diff = 360.0f - diff;

                if (diff > 27.0f) {
                    s_lockedDir8 = candidateDir;
                }
            }

            switch (s_lockedDir8) {
            case 0: curD = true; break;                // Sag
            case 1: curD = true; curS = true; break;   // Guneydogu
            case 2: curS = true; break;                // Guney
            case 3: curA = true; curS = true; break;   // Guneybati
            case 4: curA = true; break;                // Bati
            case 5: curA = true; curW = true; break;   // Kuzeybati
            case 6: curW = true; break;                // Kuzey
            case 7: curD = true; curW = true; break;   // Kuzeydogu
            }
        }
        else {
            s_lockedDir8 = -1;
        }

        SendRawKey('W', curW);
        SendRawKey('S', curS);
        SendRawKey('A', curA);
        SendRawKey('D', curD);

        // -----------------------------------------------------------------
        // 2. Sag Analog -> Kamera Dondurme
        // -----------------------------------------------------------------
        if (manualFreeCursor) {
            if (g_camDragging) StopCameraDrag();
            if (st.rx != 0.0f || st.ry != 0.0f) {
                g_lastSyntheticMouseTick = GetTickCount();
                POINT cur;
                GetCursorPos(&cur);
                SetCursorPos(cur.x + (int)(st.rx * (g_mouseSpeed * 0.45f)),
                    cur.y + (int)(st.ry * (g_mouseSpeed * 0.45f)));
            }
        }
        else if (g_invOpen) {
            if (g_camDragging) StopCameraDrag();
        }
        else {
            if (st.rx != 0.0f || st.ry != 0.0f) {
                if (!g_camDragging) {
                    g_camDragging = true;
                    g_camAccumX = 0.0f;
                    g_camAccumY = 0.0f;

                    RECT rc;
                    GetClientRect(g_gameWnd, &rc);
                    MoveMouseClient((rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2);

                    g_lastSyntheticMouseTick = GetTickCount();
                    INPUT inp = {};
                    inp.type = INPUT_MOUSE;
                    inp.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                    SendInput(1, &inp, sizeof(INPUT));
                }

                g_camAccumX += st.rx * (g_camSpeedX * 0.65f);
                g_camAccumY += st.ry * (g_camSpeedY * 0.55f);

                int dx = (int)g_camAccumX;
                int dy = (int)g_camAccumY;
                if (dx != 0 || dy != 0) {
                    g_camAccumX -= (float)dx;
                    g_camAccumY -= (float)dy;
                    g_lastSyntheticMouseTick = GetTickCount();

                    INPUT mv = {};
                    mv.type = INPUT_MOUSE;
                    mv.mi.dx = dx;
                    mv.mi.dy = dy;
                    mv.mi.dwFlags = MOUSEEVENTF_MOVE;
                    SendInput(1, &mv, sizeof(INPUT));
                }
            }
            else {
                if (g_camDragging) StopCameraDrag();
            }
        }

        // 3. Gamepad ile Envanter Ac / Kapat Tusu
        DWORD invMask = g_binds[ACT_INVENTORY].padMask;
        if (invMask && (pressed & invMask)) {
            TapCombo(g_binds[ACT_INVENTORY].keyCombo);
        }

        // 4. Envanter Grid Modu vs Savas Modu
        if (now - lastOverlayTick >= 33) {
            lastOverlayTick = now;
            UpdateInventoryOverlay();
        }

        if (g_invOpen) {
            DWORD navDir = 0;
            if (!manualFreeCursor) {
                if (st.ry < -0.45f) navDir |= PAD_UP;
                else if (st.ry > 0.45f) navDir |= PAD_DOWN;

                if (st.rx < -0.45f) navDir |= PAD_LEFT;
                else if (st.rx > 0.45f) navDir |= PAD_RIGHT;
            }

            DWORD stepDir = 0;
            if (navDir == 0) {
                navHoldDir = 0;
            }
            else if (navDir != navHoldDir) {
                navHoldDir = navDir;
                stepDir = navDir;
                navNextRepeatTick = now + 240;
            }
            else if (now >= navNextRepeatTick) {
                stepDir = navHoldDir;
                navNextRepeatTick = now + 95;
            }

            if (stepDir != 0) {
                StepInventoryGrid(stepDir);
            }

            for (int i = COMBAT_ACT_COUNT; i < ACT_COUNT; i++) {
                DWORD mask = g_binds[i].padMask;
                if (!mask || mask == invMask) continue;

                if (g_binds[i].holdMode) {
                    if (pressed & mask)  SendCombo(g_binds[i].keyCombo, true);
                    if (released & mask) SendCombo(g_binds[i].keyCombo, false);
                }
                else {
                    if (pressed & mask) {
                        TapCombo(g_binds[i].keyCombo);
                    }
                }
            }
        }
        else {
            DWORD modMask = g_binds[ACT_MOD_LAYER].padMask;
            bool modHeld = (modMask != 0) && ((st.buttons & modMask) != 0);

            if (modMask && (pressed & modMask)) {
                g_modLayerUsedCombo = false;
            }

            for (int i = 0; i < COMBAT_ACT_COUNT; i++) {
                if (i == ACT_INVENTORY || i == ACT_MOD_LAYER) continue;
                DWORD mask = g_binds[i].padMask;
                if (!mask || mask == modMask) continue;

                bool isL2Action = (i >= ACT_L2_ACT1 && i <= ACT_L2_ACT4);
                if (modHeld && !isL2Action && i != ACT_ATTACK) continue;
                if (!modHeld && isL2Action) continue;

                if (g_binds[i].holdMode) {
                    if (pressed & mask) {
                        SendCombo(g_binds[i].keyCombo, true);
                        if (isL2Action) g_modLayerUsedCombo = true;
                    }
                    if (released & mask) {
                        SendCombo(g_binds[i].keyCombo, false);
                    }
                }
                else {
                    if (pressed & mask) {
                        TapCombo(g_binds[i].keyCombo);
                        if (isL2Action) g_modLayerUsedCombo = true;
                    }
                }
            }

            if (modMask && (released & modMask) && !g_modLayerUsedCombo) {
                for (int i = 0; i < COMBAT_ACT_COUNT; i++) {
                    if (i == ACT_INVENTORY || i == ACT_MOD_LAYER) continue;
                    if (i >= ACT_L2_ACT1 && i <= ACT_L2_ACT4) continue;
                    if (g_binds[i].padMask == modMask) {
                        TapCombo(g_binds[i].keyCombo);
                    }
                }
            }
        }

        prevButtons = st.buttons;
        Sleep(4);
    }

    StopCameraDrag();
    ReleaseAllHeldKeys();
    CloseGamepadBridge();

    g_m2ui.Cleanup();
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }

    timeEndPeriod(1);
    return 0;
}

extern "C" {
    HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
        EnsureRealDInput8();
        if (!g_origDirectInput8Create) return E_FAIL;
        return g_origDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    }
    HRESULT WINAPI Proxy_DllCanUnloadNow(void) {
        EnsureRealDInput8();
        if (!g_origDllCanUnloadNow) return S_FALSE;
        return g_origDllCanUnloadNow();
    }
    HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
        EnsureRealDInput8();
        if (!g_origDllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
        return g_origDllGetClassObject(rclsid, riid, ppv);
    }
    HRESULT WINAPI Proxy_DllRegisterServer(void) {
        EnsureRealDInput8();
        if (!g_origDllRegisterServer) return E_FAIL;
        return g_origDllRegisterServer();
    }
    HRESULT WINAPI Proxy_DllUnregisterServer(void) {
        EnsureRealDInput8();
        if (!g_origDllUnregisterServer) return E_FAIL;
        return g_origDllUnregisterServer();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);

        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* name = strrchr(exe, '\\');
        name = name ? name + 1 : exe;

        strcpy_s(g_iniPath, exe);
        char* slash = strrchr(g_iniPath, '\\');
        if (slash) *(slash + 1) = '\0';
        strcpy_s(g_logPath, g_iniPath);
        strcat_s(g_iniPath, "gamepad.ini");
        strcat_s(g_logPath, "gamepad_debug.log");

        if (_stricmp(name, "metin2client.exe") == 0 ||
            _stricmp(name, "metin2client.bin") == 0) {
            DeleteFileA(g_logPath);
            WriteDebugLog("=== Metin2 Gamepad Proxy Baslatildi ===");
            g_running = true;
            g_thread = CreateThread(NULL, 0, GamepadWorker, NULL, 0, NULL);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        if (g_thread) {
            WaitForSingleObject(g_thread, 500);
            CloseHandle(g_thread);
        }
        CloseGamepadBridge();
        if (g_realDInput8) FreeLibrary(g_realDInput8);
    }
    return TRUE;
}