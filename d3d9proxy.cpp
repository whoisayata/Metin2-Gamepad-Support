// d3d9.dll proxy - Adim 10: kademe tabanli zirh yonlendirme + gizleme.
//
// Hedef fonksiyon: exe+0x027C4C20 (pack manager)
//   __thiscall, ecx = manager, [ebp+8] = cikti, [ebp+12] = dosya adi
//
// config.txt komutlari:
//   maxtier = 6                 kademesi 6'dan buyuk zirhlar varsayilana doner
//   default warrior = warrior_4-1
//   model warrior_8_1 = warrior_4-1    (LOD otomatik)
//   keep = warrior_saja
//   hide = npc_pet/
//   log  = all | karakter | esya | pet | yok
//   <tam yol> = <tam yol>       (acik esleme)
//
// Karar sirasi:
//   1. hide deseni
//   2. acik esleme (tam yol)
//   3. model eslemesi (model adi)
//   4. keep listesi          -> dokunulmaz
//   5. kademe: novice veya <= maxtier -> dokunulmaz
//   6. digerleri             -> default <sinif>
//
// Derleme: Visual Studio 2022, Release | x86 (32-bit client icin)

#define WIN32_LEAN_AND_MEAN
// winsock2.h, windows.h'den ONCE gelmeli: aksi halde windows.h
// eski winsock.h'yi cekiyor ve tanimlar cakisiyor.
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")   // F10 paneli
#pragma comment(lib, "gdi32.lib")    // F10 paneli (GetStockObject)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "MinHook.h"

// >>> GAMEPAD KOPRUSU BASLANGIC (dinput8.dll ile SharedMemory haberlesmesi v3)
#include <cmath>
#include <algorithm>

#ifndef M2_D3D_OK
#define M2_D3D_OK ((HRESULT)0L)
#endif

static void GetGpLogPath(char* outPath, size_t sz)
{
    GetModuleFileNameA(nullptr, outPath, (DWORD)sz);
    char* slash = strrchr(outPath, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(outPath, sz, "gp_d3d9.log");
}

static void GpLog(const char* fmt, ...)
{
    char path[MAX_PATH] = { 0 };
    GetGpLogPath(path, MAX_PATH);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f)
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

#pragma pack(push, 1)
struct GamepadBridgeData
{
    DWORD magic;              // 0x47504439 ('GPD9')
    DWORD version;            // 3
    volatile LONG hideCursor; // dinput8.dll yazar: 1 = imleci gizle, 0 = goster
    volatile LONG invOpen;    // d3d9.dll yazar: 1 = ekranda envanter acik, 0 = kapali
    volatile LONG inGame;     // d3d9.dll yazar: 1 = oyun ici HUD aktif, 0 = giris/secim ekrani
    int invLeft;              // d3d9.dll yazar: Envanter 1. slot sol-ust X
    int invTop;               // d3d9.dll yazar: Envanter 1. slot sol-ust Y
    int slotW;                // Tek slot genisligi (32 px)
    int slotH;                // Tek slot yuksekligi (32 px)
    BYTE slotHeight[45];      // d3d9.dll yazar: 5 sutun x 9 satir (45 slot) -> 0..3
    int quickSlotX[8];        // d3d9.dll yazar: Alt bar 1-4 ve F1-F4 slot X koordinatlari
    int quickSlotY[8];        // d3d9.dll yazar: Alt bar 1-4 ve F1-F4 slot Y koordinatlari
    DWORD lastUpdateTick;
};
#pragma pack(pop)

static HANDLE             g_gpMapFile = nullptr;
static GamepadBridgeData* g_gpBridge = nullptr;
static volatile bool      g_gpThreadRun = false;
static HANDLE             g_gpThreadHandle = nullptr;
static HWND               g_gpGameHwnd = nullptr;

typedef HRESULT(WINAPI* D8_EndScene_t)(void*);
typedef HRESULT(WINAPI* D8_DrawPrimitive_t)(void*, UINT, UINT, UINT);
typedef HRESULT(WINAPI* D8_DrawIndexedPrimitive_t)(void*, UINT, UINT, UINT, UINT, UINT);
typedef BOOL(WINAPI* ShowCursor_t)(BOOL);
typedef HCURSOR(WINAPI* SetCursor_t)(HCURSOR);

static D8_EndScene_t             g_origD8EndScene = nullptr;
static D8_DrawPrimitive_t        g_origD8DP = nullptr;
static D8_DrawIndexedPrimitive_t g_origD8DIP = nullptr;
static ShowCursor_t              g_origShowCursor = nullptr;
static SetCursor_t               g_origSetCursor = nullptr;

struct Quad2DInfo { float x, y, w, h; };
static Quad2DInfo g_frameDipSlots[180] = {};
static int        g_frameDipSlotCount = 0;

static Quad2DInfo g_frameQuickSlots[32] = {};
static int        g_frameQuickSlotCount = 0;

static bool  g_frameSawInGame = false;
static bool  g_frameSawMenuScreen = false;
static int   g_invMissingFrames = 0;
static int   g_inGameMissingFrames = 0;
static float g_lastCursorQuadX = -9999.0f;
static float g_lastCursorQuadY = -9999.0f;

static BOOL CALLBACK GpFindGameWndProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd))
    {
        RECT rc;
        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) >= 640 && (rc.bottom - rc.top) >= 480)
        {
            *(HWND*)lParam = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND GetOwnGameWindow()
{
    if (g_gpGameHwnd && IsWindow(g_gpGameHwnd)) return g_gpGameHwnd;
    HWND found = nullptr;
    EnumWindows(GpFindGameWndProc, (LPARAM)&found);
    if (found) g_gpGameHwnd = found;
    return g_gpGameHwnd;
}

static bool ShouldHideCursorNow()
{
    return (g_gpBridge && g_gpBridge->hideCursor != 0 && g_gpBridge->inGame != 0);
}

static BOOL WINAPI Hook_ShowCursor(BOOL bShow)
{
    if (ShouldHideCursorNow() && bShow)
        return g_origShowCursor(FALSE);
    return g_origShowCursor(bShow);
}

static HCURSOR WINAPI Hook_SetCursor(HCURSOR hCursor)
{
    if (ShouldHideCursorNow() && hCursor != nullptr)
        return g_origSetCursor(nullptr);
    return g_origSetCursor(hCursor);
}

static bool ReadD3D8VBSafe(void* vb, UINT offset, UINT size, void* outBuf)
{
    if (!vb || !outBuf || size == 0 || size > 256) return false;
    __try
    {
        void** vtbl = *(void***)vb;
        if (!vtbl) return false;
        typedef HRESULT(WINAPI* Lock8_t)(void*, UINT, UINT, BYTE**, DWORD);
        typedef HRESULT(WINAPI* Unlock8_t)(void*);
        BYTE* pData = nullptr;
        if (SUCCEEDED(((Lock8_t)vtbl[11])(vb, offset, size, &pData, 0x10)) && pData)
        {
            memcpy(outBuf, pData, size);
            ((Unlock8_t)vtbl[12])(vb);
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

static bool ReadActiveD3D8Stream(void* dev, UINT vertexOffsetCount, BYTE* outBuf, UINT* outStride)
{
    if (!dev || !outBuf || !outStride) return false;
    __try
    {
        void** dvt = *(void***)dev;
        if (!dvt) return false;
        typedef HRESULT(WINAPI* GetStream8_t)(void*, UINT, void**, UINT*);
        typedef ULONG(WINAPI* Release_t)(void*);
        void* vb = nullptr;
        UINT st = 0;
        if (SUCCEEDED(((GetStream8_t)dvt[84])(dev, 0, &vb, &st)) && vb)
        {
            bool ok = false;
            if (st >= 16 && st <= 64)
            {
                ok = ReadD3D8VBSafe(vb, vertexOffsetCount * st, 4 * st, outBuf);
                if (ok) *outStride = st;
            }
            ((Release_t)(*(void***)vb)[2])(vb);
            return ok;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

static bool Process2DQuad(const void* pVerts, UINT stride, const char* tag)
{
    if (!g_gpBridge || !pVerts || stride < 16 || stride > 64) return false;

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    __try
    {
        for (int i = 0; i < 4; i++)
        {
            const float* f = (const float*)((const BYTE*)pVerts + i * stride);
            float vx = f[0];
            float vy = f[1];
            if (!std::isfinite(vx) || !std::isfinite(vy)) return false;
            if (vx < minX) minX = vx;
            if (vx > maxX) maxX = vx;
            if (vy < minY) minY = vy;
            if (vy > maxY) maxY = vy;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (minX < -4.0f || maxX > 3840.0f || minY < -4.0f || maxY > 2160.0f) return false;

    float w = maxX - minX;
    float h = maxY - minY;
    if (w < 16.0f || w > 900.0f || h < 16.0f || h > 900.0f) return false;

    bool isDip = (tag[0] == 'D' && tag[1] == 'I');

    int clientW = 1366, clientH = 768;
    HWND gameWnd = GetOwnGameWindow();
    if (gameWnd)
    {
        RECT rc;
        if (GetClientRect(gameWnd, &rc) && (rc.right - rc.left) > 300)
        {
            clientW = rc.right - rc.left;
            clientH = rc.bottom - rc.top;
        }
    }

    if ((!isDip && fabsf(w - 373.0f) <= 2.0f && fabsf(h - 368.0f) <= 2.0f) ||
        (isDip && fabsf(w - 166.0f) <= 2.0f && fabsf(h - 273.0f) <= 2.0f && minX < 120.0f))
    {
        g_frameSawMenuScreen = true;
    }

    // Oyun ici alt hizli erisim barindaki 32x32 kutular (1-4 ve F1-F4)
    if (isDip && fabsf(w - 32.0f) <= 1.2f && fabsf(h - 32.0f) <= 1.2f &&
        minY >= (float)(clientH - 55) && minY <= (float)clientH)
    {
        g_frameSawInGame = true;
        if (g_frameQuickSlotCount < 32)
        {
            g_frameQuickSlots[g_frameQuickSlotCount++] = { minX, minY, w, h };
        }
    }

    // 1) 32x32 slot kutularini ve 32x64 / 32x96 esya ikonlarini topla
    if (isDip && fabsf(w - 32.0f) <= 2.5f && h >= 28.0f && h <= 102.0f)
    {
        if (g_frameDipSlotCount < 180)
        {
            g_frameDipSlots[g_frameDipSlotCount++] = { minX, minY, w, h };
        }
    }

    // 2) Oyun el imlecini gizle
    if (ShouldHideCursorNow() && isDip &&
        fabsf(w - 32.0f) <= 0.6f && fabsf(h - 32.0f) <= 0.6f)
    {
        POINT pt;
        if (gameWnd && GetCursorPos(&pt))
        {
            ScreenToClient(gameWnd, &pt);
            float expectedX = (float)pt.x - 0.5f;
            float expectedY = (float)pt.y - 0.5f;
            if (fabsf(minX - expectedX) <= 14.0f && fabsf(minY - expectedY) <= 14.0f)
            {
                g_lastCursorQuadX = minX;
                g_lastCursorQuadY = minY;
                return true;
            }
            if (fabsf(minX - g_lastCursorQuadX) <= 4.0f && fabsf(minY - g_lastCursorQuadY) <= 4.0f)
                return true;
        }
    }

    return false;
}

static HRESULT WINAPI Hook_D8_DrawPrimitive(void* dev, UINT type, UINT startVertex, UINT primCount)
{
    if (g_gpBridge && primCount == 2)
    {
        BYTE buf[256];
        UINT st = 0;
        if (ReadActiveD3D8Stream(dev, startVertex, buf, &st))
        {
            if (Process2DQuad(buf, st, "DP"))
                return M2_D3D_OK;
        }
    }
    return g_origD8DP(dev, type, startVertex, primCount);
}

static HRESULT WINAPI Hook_D8_DrawIndexedPrimitive(void* dev, UINT type, UINT minIdx, UINT numVertices, UINT startIndex, UINT primCount)
{
    if (g_gpBridge && primCount == 2)
    {
        BYTE buf[256];
        UINT st = 0;
        if (ReadActiveD3D8Stream(dev, minIdx, buf, &st))
        {
            if (Process2DQuad(buf, st, "DIP"))
                return M2_D3D_OK;
        }
    }
    return g_origD8DIP(dev, type, minIdx, numVertices, startIndex, primCount);
}

static HRESULT WINAPI Hook_D8_EndScene(void* dev)
{
    static bool s_firstD8 = false;
    if (!s_firstD8) { s_firstD8 = true; GpLog("[GP-D3D8] EndScene ILK KARE YAKALANDI! dev=0x%p", dev); }

    if (g_gpBridge)
    {
        if (g_frameSawMenuScreen)
        {
            g_inGameMissingFrames = 0;
            InterlockedExchange(&g_gpBridge->inGame, 0);
        }
        else if (g_frameSawInGame)
        {
            g_inGameMissingFrames = 0;
            InterlockedExchange(&g_gpBridge->inGame, 1);
        }
        else if (++g_inGameMissingFrames >= 30)
        {
            InterlockedExchange(&g_gpBridge->inGame, 0);
        }

        // Alt hizli erisim barindaki 8 slotun (1-4 ve F1-F4) benzersiz X/Y koordinatlarini soldan saga sirala
        if (g_frameQuickSlotCount >= 8)
        {
            Quad2DInfo uniq[16];
            int uCount = 0;
            for (int i = 0; i < g_frameQuickSlotCount; i++)
            {
                bool exists = false;
                for (int j = 0; j < uCount; j++)
                {
                    if (fabsf(uniq[j].x - g_frameQuickSlots[i].x) < 4.0f)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists && uCount < 16)
                {
                    uniq[uCount++] = g_frameQuickSlots[i];
                }
            }

            if (uCount >= 8)
            {
                std::sort(uniq, uniq + uCount, [](const Quad2DInfo& a, const Quad2DInfo& b) {
                    return a.x < b.x;
                    });

                // 1-2-3-4 (4 bitisik 32px slot) + kucuk bosluk + F1-F2-F3-F4 (4 bitisik 32px slot) desenini bul
                int foundBase = -1;
                for (int i = 0; i <= uCount - 8; i++)
                {
                    bool firstFourContig = true;
                    for (int k = 0; k < 3; k++)
                    {
                        float d = uniq[i + k + 1].x - uniq[i + k].x;
                        if (fabsf(d - 32.0f) > 2.0f) { firstFourContig = false; break; }
                    }
                    if (!firstFourContig) continue;

                    float midGap = uniq[i + 4].x - uniq[i + 3].x;
                    if (midGap < 33.5f || midGap > 52.0f) continue;

                    bool secondFourContig = true;
                    for (int k = 4; k < 7; k++)
                    {
                        float d = uniq[i + k + 1].x - uniq[i + k].x;
                        if (fabsf(d - 32.0f) > 2.0f) { secondFourContig = false; break; }
                    }
                    if (secondFourContig)
                    {
                        foundBase = i;
                        break;
                    }
                }

                int startIdx = (foundBase >= 0) ? foundBase : ((uCount > 8) ? (uCount - 8) / 2 : 0);
                for (int s = 0; s < 8; s++)
                {
                    g_gpBridge->quickSlotX[s] = (int)roundf(uniq[startIdx + s].x);
                    g_gpBridge->quickSlotY[s] = (int)roundf(uniq[startIdx + s].y);
                }
            }
        }

        // Toplanan 32x32 DIP kutulari icinde 5 sutun x 9 satir (45 slot) envanter izgarasi ara
        bool foundGrid = false;
        float bestOx = 0.0f, bestOy = 0.0f;

        if (g_gpBridge->inGame != 0 && !g_frameSawMenuScreen && g_frameDipSlotCount >= 35)
        {
            for (int i = 0; i < g_frameDipSlotCount; i++)
            {
                if (fabsf(g_frameDipSlots[i].h - 32.0f) > 2.0f) continue;
                float ox = g_frameDipSlots[i].x;
                float oy = g_frameDipSlots[i].y;

                bool hit[45] = { false };
                int hitCount = 0;

                for (int k = 0; k < g_frameDipSlotCount; k++)
                {
                    if (fabsf(g_frameDipSlots[k].h - 32.0f) > 2.0f) continue;
                    float dx = g_frameDipSlots[k].x - ox;
                    float dy = g_frameDipSlots[k].y - oy;
                    if (dx < -1.5f || dx > 130.0f || dy < -1.5f || dy > 258.0f) continue;

                    int col = (int)roundf(dx / 32.0f);
                    int row = (int)roundf(dy / 32.0f);
                    if (col >= 0 && col < 5 && row >= 0 && row < 9)
                    {
                        if (fabsf(dx - col * 32.0f) <= 1.5f && fabsf(dy - row * 32.0f) <= 1.5f)
                        {
                            int idx = row * 5 + col;
                            if (!hit[idx])
                            {
                                hit[idx] = true;
                                hitCount++;
                            }
                        }
                    }
                }

                if (hitCount >= 35)
                {
                    foundGrid = true;
                    bestOx = ox;
                    bestOy = oy;
                    break;
                }
            }
        }

        static LONG s_prevInv = -1;
        if (foundGrid)
        {
            g_invMissingFrames = 0;
            g_gpBridge->invLeft = (int)roundf(bestOx);
            g_gpBridge->invTop = (int)roundf(bestOy);

            BYTE slotMap[45];
            memset(slotMap, 1, 45);
            for (int k = 0; k < g_frameDipSlotCount; k++)
            {
                float h = g_frameDipSlots[k].h;
                if (h < 48.0f) continue;

                float dx = g_frameDipSlots[k].x - bestOx;
                float dy = g_frameDipSlots[k].y - bestOy;
                int col = (int)roundf(dx / 32.0f);
                int row = (int)roundf(dy / 32.0f);
                int span = (int)roundf(h / 32.0f);

                if (col >= 0 && col < 5 && row >= 0 && row < 9 && span >= 2 && span <= 3)
                {
                    if (row + span > 9) span = 9 - row;
                    int idx = row * 5 + col;
                    if (span > slotMap[idx]) slotMap[idx] = (BYTE)span;
                }
            }
            memcpy(g_gpBridge->slotHeight, slotMap, 45);

            InterlockedExchange(&g_gpBridge->invOpen, 1);
            g_gpBridge->lastUpdateTick = GetTickCount();
            if (s_prevInv != 1)
            {
                s_prevInv = 1;
                GpLog("[INV-STATE] invOpen -> 1 (slot0_left=%d, slot0_top=%d)", g_gpBridge->invLeft, g_gpBridge->invTop);
            }
        }
        else
        {
            if (++g_invMissingFrames >= 5)
            {
                InterlockedExchange(&g_gpBridge->invOpen, 0);
                if (s_prevInv != 0)
                {
                    s_prevInv = 0;
                    GpLog("[INV-STATE] invOpen -> 0");
                }
            }
        }

        g_frameDipSlotCount = 0;
        g_frameQuickSlotCount = 0;
        g_frameSawInGame = false;
        g_frameSawMenuScreen = false;
    }

    return g_origD8EndScene(dev);
}

static DWORD WINAPI GamepadD3D9HookThread(LPVOID)
{
    GpLog("=== Gamepad D3D8 GetStream Hook Thread Baslatildi (v3) ===");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "M2DummyD3D8Wnd";
    RegisterClassA(&wc);
    HWND hDummy = CreateWindowA("M2DummyD3D8Wnd", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    HMODULE hD3D8 = nullptr;
    for (int i = 0; i < 50 && !hD3D8; ++i)
    {
        hD3D8 = GetModuleHandleA("d3d8.dll");
        if (!hD3D8) Sleep(100);
    }
    if (hD3D8 && hDummy)
    {
        typedef void* (WINAPI* Direct3DCreate8_t)(UINT);
        Direct3DCreate8_t pCreate8 = (Direct3DCreate8_t)GetProcAddress(hD3D8, "Direct3DCreate8");
        void* pD3D8 = pCreate8 ? pCreate8(220) : nullptr;
        if (pD3D8)
        {
            struct D3DPP8 {
                UINT BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount;
                UINT MultiSampleType, SwapEffect;
                HWND hDeviceWindow;
                BOOL Windowed, EnableAutoDepthStencil;
                UINT AutoDepthStencilFormat;
                DWORD Flags;
                UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
            } pp8 = {};
            pp8.Windowed = TRUE;
            pp8.SwapEffect = 1;
            pp8.hDeviceWindow = hDummy;
            pp8.BackBufferFormat = 22;

            void** d3d8Vtbl = *(void***)pD3D8;
            typedef HRESULT(WINAPI* CreateDevice8_t)(void*, UINT, UINT, HWND, DWORD, void*, void**);
            typedef ULONG(WINAPI* Release_t)(void*);
            void* pDev8 = nullptr;
            HRESULT hr8 = ((CreateDevice8_t)d3d8Vtbl[15])(pD3D8, 0, 1, hDummy, 0x20, &pp8, &pDev8);
            if (SUCCEEDED(hr8) && pDev8)
            {
                void** vt8 = *(void***)pDev8;
                MH_STATUS s35 = MH_CreateHook(vt8[35], &Hook_D8_EndScene, (void**)&g_origD8EndScene);
                MH_STATUS s70 = MH_CreateHook(vt8[70], &Hook_D8_DrawPrimitive, (void**)&g_origD8DP);
                MH_STATUS s71 = MH_CreateHook(vt8[71], &Hook_D8_DrawIndexedPrimitive, (void**)&g_origD8DIP);

                MH_EnableHook(vt8[35]);
                MH_EnableHook(vt8[70]);
                MH_EnableHook(vt8[71]);
                GpLog("[GP-D3D8] Kancalar aktif! s35=%d s70=%d s71=%d", (int)s35, (int)s70, (int)s71);
                ((Release_t)vt8[2])(pDev8);
            }
            ((Release_t)d3d8Vtbl[2])(pD3D8);
        }
    }

    if (hDummy) DestroyWindow(hDummy);

    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (u32 && !g_origShowCursor)
    {
        void* pShow = GetProcAddress(u32, "ShowCursor");
        void* pSet = GetProcAddress(u32, "SetCursor");
        if (pShow) { MH_CreateHook(pShow, &Hook_ShowCursor, (void**)&g_origShowCursor); MH_EnableHook(pShow); }
        if (pSet) { MH_CreateHook(pSet, &Hook_SetCursor, (void**)&g_origSetCursor);  MH_EnableHook(pSet); }
    }
    return 0;
}

static void InitGamepadBridge()
{
    if (g_gpBridge) return;
    char logPath[MAX_PATH] = { 0 };
    GetGpLogPath(logPath, MAX_PATH);
    DeleteFileA(logPath);

    g_gpMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(GamepadBridgeData), "Local\\Metin2GamepadBridge");
    if (!g_gpMapFile) return;
    g_gpBridge = (GamepadBridgeData*)MapViewOfFile(g_gpMapFile, FILE_MAP_ALL_ACCESS,
        0, 0, sizeof(GamepadBridgeData));
    if (g_gpBridge)
    {
        memset(g_gpBridge, 0, sizeof(GamepadBridgeData));
        g_gpBridge->magic = 0x47504439;
        g_gpBridge->version = 3;
        g_gpBridge->slotW = 32;
        g_gpBridge->slotH = 32;
    }
    if (!g_gpThreadHandle)
    {
        g_gpThreadRun = true;
        g_gpThreadHandle = CreateThread(nullptr, 0, GamepadD3D9HookThread, nullptr, 0, nullptr);
    }
}

static void ShutdownGamepadBridge()
{
    g_gpThreadRun = false;
    if (g_gpThreadHandle) { CloseHandle(g_gpThreadHandle); g_gpThreadHandle = nullptr; }
    if (g_gpBridge) { UnmapViewOfFile(g_gpBridge); g_gpBridge = nullptr; }
    if (g_gpMapFile) { CloseHandle(g_gpMapFile);    g_gpMapFile = nullptr; }
}
// <<< GAMEPAD KOPRUSU BITIS

extern "C" NTSYSAPI WORD NTAPI RtlCaptureStackBackTrace(
    DWORD FramesToSkip, DWORD FramesToCapture, PVOID* BackTrace, PDWORD BackTraceHash);

// ---------------------------------------------------------------------------
#define RVA_PACKGET    0x027C4C20
#define MAX_MAPPINGS   1024
#define MAX_MODELMAPS  512
#define MAX_KEEPS      256
#define MAX_DEFAULTS   16
#define MAX_HIDES      64
#define PATH_LEN       224
#define NAME_LEN       96
#define LOG_LIMIT      600
#define RECENT_COUNT   192

#define HIDDEN_BASE    "mod_hidden_none"

static HMODULE g_real = nullptr;
static char    g_gameDir[MAX_PATH] = {};
static char    g_modDir[MAX_PATH] = {};
static char    g_logPath[MAX_PATH] = {};
static BYTE*   g_exeBase = nullptr;
static bool    g_ready = false;
static void*   g_origPackGet = nullptr;

static LONG    g_logCount = 0;
static LONG    g_hitCount = 0;

// Kademesi bundan buyuk olan zirhlar varsayilana doner. -1 = kapali
static int g_maxTier = -1;

struct Mapping { char from[PATH_LEN]; char to[PATH_LEN]; };
static Mapping g_map[MAX_MAPPINGS];
static int     g_mapCount = 0;

struct ModelMap { char from[NAME_LEN]; char to[NAME_LEN]; };
static ModelMap g_modelMap[MAX_MODELMAPS];
static int      g_modelMapCount = 0;

// Genel dosya adi eslemesi: her yolda calisir, yol ve LOD korunur
static ModelMap g_fileMap[MAX_MODELMAPS];
static int      g_fileMapCount = 0;

static char g_keep[MAX_KEEPS][NAME_LEN];
static int  g_keepCount = 0;

struct DefaultModel { char className[NAME_LEN]; char model[NAME_LEN]; };
static DefaultModel g_default[MAX_DEFAULTS];
static int          g_defaultCount = 0;
// clamptier <sinif> = <model> : kademesi maxtier'i ASAN gercek zirhlar
// buna doner. default'tan AYRI, cunku default kademesiz (olay/kostum)
// zirhlar icin ve orada oyuncunun seviyesini bilmiyoruz.
static DefaultModel g_clampTier[MAX_DEFAULTS];
static int          g_clampTierCount = 0;

// Silah: tip (vnum'un binler basamagi) -> varsayilan model adi
struct WeaponDefault { int type; char model[NAME_LEN]; };
static WeaponDefault g_weapon[MAX_DEFAULTS];
static int           g_weaponCount = 0;

// Tip icindeki numara bunu asarsa varsayilana doner. -1 = kapali
static int g_maxWeapon = -1;

static char g_hide[MAX_HIDES][PATH_LEN];
static int  g_hideCount = 0;

// redirect <desen> = <tam hedef yol>
// Desen eslesen ve hedefle ayni uzantiya sahip istekler hedefe gider.
struct Redirect { char pattern[PATH_LEN]; char target[PATH_LEN]; char ext[12]; };
static Redirect g_redirect[MAX_HIDES];
static int      g_redirectCount = 0;

// Sac: numarasi bunu asan sac modelleri varsayilana doner. -1 = kapali
static int  g_maxHair = -1;
static char g_defaultHair[NAME_LEN] = {};

// trace <desen> : eslesen istekte cagri yigini loglanir (hook avi icin)
static char g_trace[8][PATH_LEN];
static int  g_traceCount = 0;

// dir = klasor yonlendirmesi (binek -> varsayilan at)
// Eslesen her istekte dosya adi korunur, klasor degistirilir.
struct DirRule { char pattern[PATH_LEN]; char target[PATH_LEN]; };
static DirRule g_dir[64];
static int     g_dirCount = 0;

// anim <kaynak> = <hedef> : dir icinde dosya adi cevirisi
static ModelMap g_anim[64];
static int      g_animCount = 0;
static char     g_animDefault[NAME_LEN] = {};   // eslesmeyen .gr2/.msa
static char     g_msmDefault[NAME_LEN] = {};    // eslesmeyen .msm

// Kendi kendine ogrenen binek listesi.
// Gercek binek ile pet'i ayirmanin tek guvenilir yolu: client
// npc_mount/<ad>/motlist.txt isterse o klasor gercekten binek.
// Pet'lerde o istek gelmez, zincir npc2/ veya npc_pet/'e gecer.
// Ogrenilenler mod_mounts.txt'ye yazilir, sonraki aciliste okunur.
#define MAX_MOUNTS 256
struct MountEntry { char name[NAME_LEN]; bool active; };
static MountEntry g_mount[MAX_MOUNTS];
static int  g_mountCount = 0;
static bool g_mountLearn = false;
static bool g_mountAll = false;

// Ana anahtar: kapaliyken hicbir yol degistirilmez.
static volatile LONG g_enabled = 1;

#define MAX_FINDSTR 4
#define MAX_DUMPPTR 4

// --- Nesne avi: iki katmanli dokum ---
// Render zinciri tekil mimari: karakter nesnesi parametre olarak
// gelmiyor, yoneticinin ICINDE "secili karakter" alani olarak
static char g_mountDir[PATH_LEN] = "d:/ymir work/npc/horse/";
static char g_mountMsm[PATH_LEN] = "d:/ymir work/npc/horse/horse_member.msm";
static void MFoldAdd(const char* f);

// Kostum sinifini bulmak icin: client kostumden hemen ONCE
// oyuncunun gercek silah modelini istiyor. O vnum'un binler
// hanesi sinifi veriyor (0 kilic, 1 hancer, 2 yay, 3 ciftel,
// 5 can, 6 pence, 7 yelpaze). Sadece loglamak icin tutuyoruz.
static char g_lastWeapon[16] = {};

static bool IsReadable(const void* addr, size_t size);   // ileri bildirim
static bool GrabStr(const unsigned char* p, char* out, size_t outSz); // ileri bildirim
static bool ProtReadable(DWORD prot);                                // ileri bildirim
static void PktRecheck();      // ileri bildirim: HotkeyThread cagiriyor, tanim asagida
static int  WeaponFamily(const char* vnum);             // ileri bildirim

// ---------------------------------------------------------------------------
// EKIPMAN TABLOSU
// exe+0x024E1170 ekipman kurucusu:  ecx = karakter nesnesi, arg1 = esya vnum.
// Model dosyasi = (vnum / 10) * 10.  Boylece kostumun sahibini tahmin etmek
// yerine nesne kimliginden biliyoruz.
#define MAX_CHARS 96
struct CharEntry
{
    DWORD obj;       // ecx
    int   fam;       // son gercek silahin ailesi (-1 bilinmiyor)
    char  weapon[8]; // son gercek silahin model numarasi (log icin)
    char  cls[16];   // sinifi: warrior / assassin / sura / shaman / wolfman
};
static CharEntry g_chars[MAX_CHARS];
static int       g_charCount = 0;
static DWORD     g_equipRva = 0;        // equipfn = <rva>

static LONG  g_recvDump = 0;
typedef int (__stdcall* RecvFn)(SOCKET, char*, int, int);
// equiplog = N : ekipman kurucusunun ilk N cagrisini ham olarak logla.
// fnlimit'ten bagimsiz. Amac: bu fonksiyon SADECE silah mi aliyor,
// yoksa zirh vnum'u da geciyor mu? Zirh vnum'u geciyorsa seviyeye
// uygun zirh icin ag katmanina hic ihtiyac kalmiyor.
static LONG  g_equipLog = 0;
static volatile LONG g_equipLogHits = 0;
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Paket isleyicisini aramak yerine alani ELEYEREK buluyoruz.
// Istemci paketi kendisi cozuyor; cozulmus seviye, karakter
// nesnesinin bir yerinde duruyor olmak zorunda, cunku oyun onu
// ismin ustunde yaziyor.
//
// 1. TUR SONUCU: seviye nesnenin ilk 0x800 baytinda DEGIL.
// Yontem dogru calisti -- isim tamponunu (+018 K, +019 a, +01A f...)
// ve dizgi uzunlugunu (+028 = 9 = "KafilunTR") buldu, ikisi de
// bildigimiz gercek alanlar. KafilunTR 62 seviye ama hicbir adayda
// 62 yok. Kalan adaylar (+23F, +5AF, +5EB...) oyuncuya gore degil
// ZAMANA gore kayiyordu: ayni anda loglanan farkli oyuncularda ayni
// degeri veriyorlar. Yani paylasilan sayaclar, seviye degil.
//
// 2. TUR: iki degisiklik.
//   a) dogrudan tarama 0x800 -> 0x1000
//   b) BIR KAT DERIN: nesnedeki her isaretci takip edilip onun
//      gosterdigi yerin ilk 0x200 bayti da taranir. Aday artik
//      "[+isaretci offseti]+alt offset" seklinde.
// Ve kayan sayaclari elemek icin ucuncu kosul:
//   3) deger bir oyuncudan digerine SIK SIK degismeli. Gercek
//      seviye neredeyse her oyuncuda farklidir; kayan sayac
//      ardarda gelen oyuncularda ayni kalir.
// 0x1000 -> 0x4000: seviye ilk 4 KB'ta cikmadi ama oyuncu
// seviyeyi ekranda SUREKLI goruyor, yani veri nesnede var.
// Nesne dusundugumden buyuk; arama alanini dort katina cikardim.
#define LVL_SPAN      0x4000     // dogrudan tarama
#define LVL_PTRS      128        // obj+0x000 .. obj+0x1FC isaretcileri
#define LVL_PTR_SPAN  0x200      // her isaretcinin ardindaki alan
#define LVL_W         3          // 0=bayt 1=word 2=dword
#define LVL_LO        1
#define LVL_HI        130        // sampiyon seviyesi 1'den basliyor

// dogrudan nesne
// isaretci ardi

// armorfn = <rva> : ZIRH SETTER. 0x024E60B0 dogrulandi: arg1 zirh
// vnum'u tasiyor -- 11xxx/12xxx gercek zirh, 41xxx zirh kostumu.
// Burada isimle birlikte logluyoruz ki kostumlu oyuncuda IKISININ
// BIRDEN gelip gelmedigini gorelim.
static DWORD g_armorRva = 0;
static LONG  g_armorLog = 0;
static volatile LONG g_armorHits = 0;

// ===================== PAKET TARAYICI =====================
// packet.h'tan dogrulanmis duzen (#pragma pack(1), dolgu yok):
//   +00 BYTE  header       HEADER_GC_CHAR_ADDITIONAL_INFO = 136
//   +01 DWORD dwVID
//   +05 char  name[25]     CHARACTER_NAME_MAX_LEN(24) + 1
//   +30 WORD  awPart[0]    ZIRH
//   +32 WORD  awPart[1]    SILAH
//   +34 WORD  awPart[2]    KAFA
//   +36 WORD  awPart[3]    SAC
//   +38 BYTE  bEmpire
//   +39 DWORD dwGuildID
//   +43 DWORD dwLevel      <-- aradigimiz
//   +47 short sAlignment
//   +49 BYTE  bPKMode
//   +50 DWORD dwMountVnum
//   = 54 bayt
#define PKT_SIZE        54
#define PKT_OFF_NAME     5
#define PKT_STACK_SPAN  0x8000
// Ismin etrafinda bakilan pencere. Gercek pakette zirh isim+25'te,
// seviye isim+38'de, binek isim+45'te. 8 geri + 64 ileri hepsini kapsar.
#define PKT_BACK         8
#define PKT_WIN        232      // isim-8 .. isim+223
#define PKT_MAXHIT      10

static LONG g_pktScan = 0;

// Ikinci ornek: ayni nesneyi birkac saniye sonra tekrar okuyup
// DEGISMEYEN alanlari ariyoruz. Ekran koordinati, animasyon sayaci,
// donus acisi -- hepsi degisir. Seviye degismez.
#define PKT_ECX_SPAN 0x2000   // ecx icinde nesne isaretcisi aranan alan
#define PKT_PEND 18
struct PktPend {
    char  nm[24];
    const BYTE* p;
    DWORD t, zirh;
    int   zofs;
    BYTE  snap[PKT_WIN];
    bool  bitti;
};
static PktPend g_pend[PKT_PEND];
static int     g_pendN = 0;

#define CALLSIN_MAX 6

#define PY_TABLE_MAX 6

#define HUNT_MAX_NAMES 12

// Bekleyen kayitlar THREAD YEREL. Kurucu fonksiyon ile hemen ardindan
// gelen dosya istekleri ayni is parcaciginda kosuyor; tek global
// kullanilinca kalabalikta baskasinin seviyesi/zirhi karisiyordu.
static __declspec(thread) LONG g_pendCostume = 0; // kostum vnum
static __declspec(thread) int  g_pendFam     = -1;
static __declspec(thread) char g_pendWeapon[8] = {};
static __declspec(thread) char g_pendName[28]  = {};

static CharEntry* FindChar(DWORD obj, bool create)
{
    for (int i = 0; i < g_charCount; ++i)
        if (g_chars[i].obj == obj) return &g_chars[i];
    if (!create) return nullptr;
    if (g_charCount < MAX_CHARS)
    {
        CharEntry* e = &g_chars[g_charCount++];
        e->obj = obj; e->fam = -1; e->weapon[0] = '\0';
        return e;
    }
    // Tablo dolu: en eskisini dondur (dairesel)
    static int rr = 0;
    rr = (rr + 1) % MAX_CHARS;
    CharEntry* e = &g_chars[rr];
    e->obj = obj; e->fam = -1; e->weapon[0] = '\0';
    return e;
}

// Nesnenin icindeki std::string ismi oku (+0x18 tampon, +0x28 uzunluk)
static void ReadCharName(void* obj, char* out, size_t outSize)
{
    out[0] = '\0';
    if (!IsReadable(obj, 0x30)) return;
    DWORD len = *(DWORD*)((BYTE*)obj + 0x28);
    if (len == 0 || len > 24) return;
    const char* buf = (const char*)obj + 0x18;
    DWORD kap = *(DWORD*)((BYTE*)obj + 0x2C);
    if (kap > 15)
    {
        // KAPASITE 16+ ise std::string yerel tamponu KULLANMAZ: +0x18'de
        // metnin kendisi degil ISARETCISI durur. Belirleyici olan ismin
        // uzunlugu degil KAPASITE: logda 4-11 harfli isimlerde kapasite
        // 31/47 cikti (nesne yeniden kullanilinca tampon buyuk kaliyor).
        // Eskiden uzunluga bakildigi icin bu karakterlerin ismi, dolayisiyla
        // seviyesi okunamiyordu -- ayni oyuncu bir gorunuste dogru, bir
        // gorunuste "seviye bilinmiyor" silahiyla cikiyordu.
        if (kap < len || kap > 0x1000) return;
        buf = *(const char**)((BYTE*)obj + 0x18);
        if (!IsReadable(buf, len + 1)) return;
    }
    for (DWORD i = 0; i < len && i + 1 < outSize; ++i)
    {
        char c = buf[i];
        if (c < 0x20 || c > 0x7E) { out[0] = '\0'; return; }
        out[i] = c; out[i + 1] = '\0';
    }
}

// Son gorulen karakter sinifi. Kostum ailesi bu sinifa imkansizsa
// eslestirme kaymis demektir; o zaman sinifin kendi silahini veriyoruz.
// Sura'nin pence tutmasi gibi sacmaliklari bu engelliyor.
static char g_lastClass[NAME_LEN] = {};

// Hangisi daha yeni? Bayat bilgi tazeyi ezmesin diye sira sayaci.
// Logda gorulen hata: taze bir can (saman silahi) geldi ama sinif
// bilgisi onceki karakterden kalma "wolfman"di; sinif duzeltmesi
// dogruyu yanlisa cevirdi. Artik sadece sinif DAHA YENIYSE duzeltiyoruz.
static LONG g_seq = 0;
static LONG g_classSeq = 0;
static LONG g_weaponSeq = 0;

// Sinif -> kullanabilecegi silah aileleri (-1 = liste sonu)
struct ClassFams { const char* name; int fam[3]; };
static const ClassFams k_classFams[] = {
    { "warrior",  { 0, 3, -1 } },   // kilic, ciftel
    { "assassin", { 1, 2, -1 } },   // hancer, yay
    { "sura",     { 0, 5, -1 } },   // kilic, yelpaze
    { "shaman",   { 4, 5, -1 } },   // can, yelpaze
    { "wolfman",  { 6, -1, -1 } },  // pence
};

// Aile bu sinif icin mumkun mu? Degilse sinifin birincil ailesi.
static int FixFamilyForClass(int fam, const char* cls)
{
    if (!*cls) return fam;
    const int famCount = (int)(sizeof(k_classFams) / sizeof(k_classFams[0]));
    for (int i = 0; i < famCount; ++i)
    {
        if (strcmp(k_classFams[i].name, cls) != 0) continue;
        for (int k = 0; k < 3; ++k)
        {
            if (k_classFams[i].fam[k] < 0) break;
            if (k_classFams[i].fam[k] == fam) return fam;   // uyumlu
        }
        return k_classFams[i].fam[0];                        // uyumsuz
    }
    return fam;
}
static LONG g_pairHits = 0;

// Listede olmayan / ailesi bilinmeyen kostumler icin son care
static char g_costumeDefault[PATH_LEN] = {};

// Aileye gore kostum hedefi: 0 kilic 1 hancer 2 yay 3 ciftel
// 4 can 5 yelpaze 6 pence   (config: costume <0-6> = <yol>)
static char g_costumeFam[7][PATH_LEN] = {};

// clamp <aile> = <vnum> : o ailede bu vnum'dan BUYUK her silah
// bunun gorunumunu alir. Oyuna sonradan eklenen, listemizde
// olmayan silahlari tek kuralla klasige cekiyor.
static int g_clampTop[7] = { -1, -1, -1, -1, -1, -1, -1 };

// ===================== SEVIYE SINIRLAMASI =====================
// Oyuncunun gorunen seviyesi = min(gercek seviye, maxlevel).
// maxlevel panel gelince oradan oynar, simdilik config'den.
static int g_maxLevel  = 99;
static int g_capLevel  = 80;   // tavan: mavi celik / triton basamagi
static int g_trustGear = 0;    // 1: sampiyon korumasi (asagida anlatiliyor)

// Silah tablosu. KESINLIKLE formulle uretilmiyor: vnum'lar
// Metin2'de duzensiz girilmis (Dolunay 290 seviye 30, Ruhsuz
// Bicak 4040 hancer, can seviye 10'da basliyor). Config'den
// satir satir okunuyor:  silah <aile> <seviye> <vnum+0>
#define WTAB_MAX   400
struct WRow { short fam; short lvl; int base; int mask; };   // mask: 1 savasci 2 ninja 4 sura 8 saman 16 lycan
static WRow g_wtab[WTAB_MAX];

// Esya isimleri (item_names'ten). Sadece log/dogrulama icin: hangi
// vnum'a yonlendirdigimizi isimle gorebilmek lazim.
struct IsimRow { int vnum; char ad[30]; };
static IsimRow g_isim[400];

// Kostum silahi -> aile (proto Value3). Kostumun hangi silah turu
// oldugunu kesin soyler; eskiden karakterin son gercek silahindan
// tahmin ediyorduk ve yanlis aile/seviye cikiyordu.
struct KSil { int vnum; int fam; };
static KSil g_ksil[400];
static int  g_ksilN = 0;

static int KSilFam(int vnum)
{
    for (int i = 0; i < g_ksilN; ++i)
        if (g_ksil[i].vnum == vnum) return g_ksil[i].fam;
    return -1;
}
static int     g_isimN = 0;

static const char* IsimBul(int vnum)
{
    for (int i = 0; i < g_isimN; ++i)
        if (g_isim[i].vnum == vnum) return g_isim[i].ad;
    return "?";
}
static int  g_wtabN = 0;

// Zirh tablosu:  zirh <sinif> <seviye> <vnum+0>
#define ATAB_MAX   240
struct ARow { char cls[16]; short lvl; int base; };
static ARow g_atab[ATAB_MAX];
static int  g_atabN = 0;

// zirhmodel = <vnum> <gr2> <dds>   (uzantisiz adlar)
#define AMOD_MAX 120
struct AMod { int vnum; char gr2[NAME_LEN]; char dds[NAME_LEN]; };
static AMod g_amod[AMOD_MAX];
static int  g_amodN = 0;
static int  g_zirhKirp = 1;      // zirhkirp = 0 -> zirh yonlendirmesini kapat
static int  g_hideCostume = 0;   // kostumgizle = 1 -> altindaki gercek zirh

static const AMod* AModOf(int vnum)
{
    for (int i = 0; i < g_amodN; ++i)
        if (g_amod[i].vnum == vnum) return &g_amod[i];
    return nullptr;
}
// Ayni sinifta seviyesi <= lvl olan EN YUKSEK zirhin vnum'u
// Ninja sinifinin iki adi var: tabloda "ninja", dosya yolunda ve
// pakette "assassin". Esitlemezsek o sinifin zirhlari hic kirpilmiyor
// (hedef=-1 donuyordu).
static bool SinifAyni(const char* a, const char* b)
{
    if (_stricmp(a, b) == 0) return true;
    bool an = (_stricmp(a, "ninja") == 0 || _stricmp(a, "assassin") == 0);
    bool bn = (_stricmp(b, "ninja") == 0 || _stricmp(b, "assassin") == 0);
    return an && bn;
}

static int ATargetFor(const char* cls, int lvl)
{
    int best = -1, bestLvl = -1;
    for (int i = 0; i < g_atabN; ++i)
    {
        if (!SinifAyni(g_atab[i].cls, cls)) continue;
        if (g_atab[i].base >= 30000) continue;   // kostum/etkinlik zirhi hedef olmasin
        if (g_atab[i].lvl > lvl) continue;
        if (g_atab[i].lvl > bestLvl) { bestLvl = g_atab[i].lvl; best = g_atab[i].base; }
    }
    return best;
}
static const char* AClassOf(int base)
{
    for (int i = 0; i < g_atabN; ++i)
        if (g_atab[i].base == base) return g_atab[i].cls;
    return nullptr;
}

static int WFamByName(const char* s)
{
    if (!_stricmp(s, "kilic"))   return 0;
    if (!_stricmp(s, "hancer"))  return 1;
    if (!_stricmp(s, "yay"))     return 2;
    if (!_stricmp(s, "ciftel"))  return 3;
    if (!_stricmp(s, "can"))     return 4;
    if (!_stricmp(s, "yelpaze")) return 5;
    if (!_stricmp(s, "pence"))   return 6;
    return -1;
}


static int WLevelOf(int base)
{
    int t = -1;
    for (int i = 0; i < g_wtabN; ++i)
        if (g_wtab[i].base == base) { t = g_wtab[i].lvl; break; }

    // Tablo tek yetkili. Olculen degerler sadece log/dogrulama icin
    // tutuluyor, karara karismiyor.
    return t;
}


// O ailede seviyesi <= lvl olan EN YUKSEK seviyeli silah.
// Esitlikte config sirasindaki ilki -- sirayi sen belirliyorsun.
// --- OYUNDA GERCEKTEN VAR OLAN SILAHLAR ---
// Tabloda oyunda bulunmayan vnum'lar var (1150, 7170 gibi). Oraya
// yonlendirince olmayan silah gorunuyor. Istemcinin KENDI istedigi
// her item/weapon/<vnum>.gr2 o silahin var oldugunun kanitidir;
// onlari biriktirip sadece var oldugu kanitlanmis silahlara
// yonlendiriyoruz.
static int  g_wvar[400];
static int  g_wvarN = 0;

static bool WVarMi(int base)
{
    for (int i = 0; i < g_wvarN; ++i) if (g_wvar[i] == base) return true;
    return false;
}

static void WVarEkle(int base)
{
    if (base <= 0 || WVarMi(base)) return;
    if (g_wvarN < 400) g_wvar[g_wvarN++] = base;
}

// --- ZIRH KADEME TABLOSU (gozlenen gercek modeller) ---
struct ZKad { char cls[12]; int sv; char model[NAME_LEN]; };
static ZKad g_zkad[64];
static int  g_zkadN = 0;

// Modelin tablodaki en dusuk seviyesi; tabloda yoksa -1 (kostum,
// zodyak, yansitma, tavan ustu zirh).
static int ZKadSeviye(const char* cls, const char* model)
{
    int best = -1;
    for (int i = 0; i < g_zkadN; ++i)
    {
        if (_stricmp(g_zkad[i].model, model) != 0) continue;
        if (!SinifAyni(g_zkad[i].cls, cls)) continue;
        if (best < 0 || g_zkad[i].sv < best) best = g_zkad[i].sv;
    }
    return best;
}

// Sinifin, verilen seviyede giyebilecegi en ust model.
static const char* ZKadHedef(const char* cls, int sv)
{
    const char* best = nullptr; int bsv = -1;
    for (int i = 0; i < g_zkadN; ++i)
    {
        if (!SinifAyni(g_zkad[i].cls, cls)) continue;
        if (g_zkad[i].sv > sv || g_zkad[i].sv < bsv) continue;
        bsv = g_zkad[i].sv; best = g_zkad[i].model;
    }
    return best;
}

// Silahin ailesi TABLODAN (proto SubType). Rakamlardan tahmin yok.
static int SinifMaske(const char* cls);
static void MaskeyiKarakterdenAl(void* ecx);

static int WFamOf(int base)
{
    for (int i = 0; i < g_wtabN; ++i)
        if (g_wtab[i].base == base) return g_wtab[i].fam;
    return -1;
}

// Klasik (+ basilabilen, standart) silah mi? 10000 alti vnum'lar.
// Ustundekiler ozel seri / etkinlik / basilamayan silahlar: kural
// geregi her zaman klasik karsiligina cevriliyor.
static bool WKlasik(int base) { return base > 0 && base < 10000; }

// Arti basilamayan LEVEL SILAHLARI (219xx serisi) klasik karsiligina +8
// olarak cevrilir: +8 parlamasi sayesinde level silahi kullanan oyuncu,
// klasik silah kullanandan ayirt edilebilir. Diger ozel seriler +0 kalir.
static int OzelRef(int base) { return (base >= 21900 && base < 22000) ? 8 : 0; }

static int SinifMaske(const char* cls)
{
    if (!cls || !cls[0]) return 31;
    if (!_stricmp(cls, "warrior")) return 1;
    if (!_stricmp(cls, "ninja") || !_stricmp(cls, "assassin")) return 2;
    if (!_stricmp(cls, "sura"))    return 4;
    if (!_stricmp(cls, "shaman"))  return 8;
    if (!_stricmp(cls, "wolfman")) return 16;
    return 31;
}

// O an islenen karakterin sinif maskesi (thread yerel). Hedef secimi
// bunu kullaniyor: savasciya sura kilici (Keskin Pala) gitmesin.
static __declspec(thread) int t_sinifMaske = 31;
static CRITICAL_SECTION g_lock;

// Islenen karakterin sinif maskesini KENDI kaydindan ayarla. Seviye
// onbellekten gelince maske bir onceki karakterinki kaliyordu
// (FAA34 savasciya sura kilici Keskin Pala gidiyordu).
static int LvlcMaskeEcx(void* ecx);   // asagida, onbellek tanimindan sonra
static void MaskeyiKarakterdenAl(void* ecx)
{
    char c[16] = {};
    EnterCriticalSection(&g_lock);
    CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, false);
    if (e && e->cls[0]) strcpy_s(c, e->cls);
    LeaveCriticalSection(&g_lock);
    // Karakter kaydi yoksa (tablo donup kayit silinmis olabilir) sinifi
    // isim onbelleginden al. Eskiden burada 31 (her sinif) kaliyor,
    // savasciya sura kilici Keskin Pala gidiyordu; ustelik ayni oyuncu
    // bir cagrida Triton, sonrakinde Keskin Pala aliyordu.
    // ONCE isimle tutulan onbellek (paketten okunan sinif), yoksa nesne
    // kaydi. Nesne kaydi adresle tutuluyor ve ayni adresi daha once
    // kullanan baska oyuncunun sinifini tasiyabiliyor.
    int m = LvlcMaskeEcx(ecx);
    t_sinifMaske = (m != 31) ? m : (c[0] ? SinifMaske(c) : 31);
}

static int WTargetFor(int fam, int lvl)
{
    int best = -1, bestLvl = -1;
    int varBest = -1, varLvl = -1;
    for (int i = 0; i < g_wtabN; ++i)
    {
        if (g_wtab[i].fam != fam) continue;
        // Bu sinif bu silahi kullanabiliyor mu? (proto AntiFlags)
        if (!(g_wtab[i].mask & t_sinifMaske)) continue;
        // HEDEF FILTRESI: sadece klasik silah vnum'lari (<10000).
        // 91000 gibi etkinlik/GM silahlarinin model dosyasi yok;
        // hedef secilince silah kayboluyor veya olmayan kilic cikiyordu.
        if (g_wtab[i].base >= 10000) continue;
        // Olculen seviyeyi de hesaba kat: tablo dusuk diyorsa bile
        // oyunda daha yuksek seviyede tasindigi olculduyse o gecerli.
        int sv = WLevelOf(g_wtab[i].base);
        if (sv < 0) sv = g_wtab[i].lvl;
        if (sv > lvl) continue;
        if (sv > bestLvl) { bestLvl = sv; best = g_wtab[i].base; }

    }
    // "Kanitlanmis silahi tercih et" kurali KALDIRILDI: oturumda henuz
    // gorulmemis ust seviye silahlar eleniyor ve 110. seviyedeki adam
    // en dusuk silaha dusuyordu (3199 -> 3039, 21950 -> 10).
    (void)varBest; (void)varLvl;
    return best;
}

// Zirhlarda basamak +0..+15, silahlarda +0..+9. Bu yuzden taban
// vnum'u "%10" ile bulmak zirhlarda yanlis sonuc veriyor
// (21242 -> 21240; dogrusu 21230). Tabanlari tablodan ariyoruz.
static int ATabanOf(int v)
{
    int best = -1;
    for (int i = 0; i < g_atabN; ++i)
    {
        int b = g_atab[i].base;
        if (b <= v && v - b < 16 && b > best) best = b;
    }
    return best > 0 ? best : v - (v % 10);
}

static int WTabanOf(int v)
{
    int best = -1;
    for (int i = 0; i < g_wtabN; ++i)
    {
        int b = g_wtab[i].base;
        if (b <= v && v - b < 10 && b > best) best = b;
    }
    return best > 0 ? best : v - (v % 10);
}

static int ALevelOf(int base)
{
    for (int i = 0; i < g_atabN; ++i)
        if (g_atab[i].base == base) return g_atab[i].lvl;
    return -1;
}

// ecx -> seviye onbellegi. Arama karakter basina bir kez.
// ecx serbest birakilip baskasina verilirse isim degisecegi icin
// ismi de sakliyoruz ve uyusmazsa yeniden cozuyoruz.
#define LVLC_MAX 512
struct LvlCache { void* ecx; char nm[28]; int lvl; const BYTE* isim; int maske;
                  DWORD silahCikis;      // bu karaktere en son verilen silah vnum'u
                  DWORD silahTakili;     // silah fonksiyonuna FIILEN giden son vnum
                  DWORD silahHam;        // oyunun verdigi son GERCEK silah vnum'u
                  DWORD kostumHam;       // oyunun verdigi son silah kostumu vnum'u (0 = yok)
                  DWORD ekArg[3];        // o cagrinin 2., 3. ve 4. argumani
                  DWORD zirhHam;         // oyunun verdigi son zirh / zirh kostumu vnum'u
                  DWORD zirhArg[3];      // zirh fonksiyonu cagrisinin 2.-4. argumani
                  DWORD a0c;             // nesnenin +A0C alanindaki silah kostumu (gizlerken sifirlanir, burada saklanir)
                  LONG  nesil; };        // ac/kapa sonrasi yeniden takildi mi
static LvlCache g_lvlc[LVLC_MAX];
static int      g_lvlcN = 0;

// Nesnenin onbellek kaydi. SADECE ecx ile aramak YANLIS: oyun karakter
// nesnelerini yeniden kullaniyor, ayni adreste daha once duran BASKA bir
// oyuncunun kaydi bulunuyordu (yanlis sinif -> o sinifa uygun silah yok ->
// esya hic cevrilmiyor, zodyak silah / kostum oldugu gibi gorunuyordu).
// Nesnedeki ISIM de tutmali.
static LvlCache* LvlcBul(void* ecx)
{
    char ad[28];
    ReadCharName(ecx, ad, sizeof(ad));
    if (!ad[0]) return nullptr;
    for (int i = 0; i < g_lvlcN; ++i)
        if (strcmp(g_lvlc[i].nm, ad) == 0) return &g_lvlc[i];
    return nullptr;
}
static DWORD LvlcSilahOku(void* ecx)
{
    LvlCache* c = LvlcBul(ecx);
    return (c && c->ecx == ecx) ? c->silahCikis : 0;
}
static void LvlcSilahYaz(void* ecx, DWORD v)
{
    LvlCache* c = LvlcBul(ecx);
    if (c && c->ecx == ecx) c->silahCikis = v;
}
// Seviye icin ALT SINIR: sunucu kimseye seviyesinin ustunde esya
// giydirmez, yani gercek esyasinin seviyesi oyuncunun seviyesinden buyuk
// olamaz. Okunan seviye bundan kucukse okuma yanlistir.
static void LvlcSeviyeTaban(void* ecx, int taban)
{
    LvlCache* c = LvlcBul(ecx);
    if (c && c->lvl > 0 && c->lvl < taban) c->lvl = taban;
}

// Onbellekteki sinif maskesi (paketin +56 alanindan ogrenilir).
// Bilinmiyorsa 31 = her sinif.
static LvlCache* LvlcBul(void* ecx);
static int LvlcMaskeEcx(void* ecx)
{
    LvlCache* c = LvlcBul(ecx);
    return (c && c->maske) ? c->maske : 31;
}
static int LvlcMaskeIsim(const char* nm)
{
    for (int i = 0; i < g_lvlcN; ++i)
        if (strcmp(g_lvlc[i].nm, nm) == 0) return g_lvlc[i].maske;
    return 0;
}
static void LvlcMaskeYaz(const char* nm, int m)
{
    if (!m || m == 31) return;
    for (int i = 0; i < g_lvlcN; ++i)
        if (strcmp(g_lvlc[i].nm, nm) == 0) { g_lvlc[i].maske = m; return; }
}
static int      g_lvlcSira = 0;   // tablo dolunca uzerine yazilacak sira

// Model cozulurken o an hangi karakterin ekipmani isleniyor.
static __declspec(thread) LONG g_pendLevel = 0;
static __declspec(thread) LONG g_pendArmor = 0;   // son zirh setter vnum'u
static __declspec(thread) void* g_pendEcx = nullptr; // o cagrinin karakteri
static __declspec(thread) LONG g_pendEsya = 0;       // son ekipman vnum'u
static volatile LONG g_zmHits   = 0;
static LONG g_eslemeLog = 0;
static LONG g_sampiyonLog = 0;   // sampiyonlog = N
static int  g_seviyeGoster = 0;  // seviyegoster = 99 -> gorunen seviye tavani
static LONG g_seviyeAra = 0;     // seviyeara = N -> nesnede seviye alanini ara
static volatile LONG g_degisHits = 0;
static int  g_kostumSilahGizle = 1;  // kostumsilahgizle = 1 -> kostum silahini gizle
static int  g_vnumDegis = 1;     // vnumdegis = 1 -> karakter bazli vnum degistirme
static int  g_alanYaz = 1;       // takili-silah alanini geri yaz (animasyon duzeltmesi; hep acik)
static int  g_zirhKostumGizle = 1;  // paneldeki "Zirh kostumunu gizle"
static int  g_bilinmeyenSv = 1;  // bilinmeyensv = N -> seviye okunamazsa bu kabul edilir
static volatile LONG g_svAraHits = 0;
static LONG g_paketDok = 0;      // paketdok = N
static int  g_yansKaldir = 1;    // yanskaldir = 1 -> yansitmayi sifirla
static LONG g_yansAra = 0;       // yansara = N
static volatile LONG g_yansHits = 0;
static LONG g_nesneDok = 0;      // nesnedok = N
static volatile LONG g_ndokHits = 0;
static LONG g_parcaLog = 0;      // parcalog = N
static volatile LONG g_parcaHits = 0;
static int  g_parcaYaz = 0;      // parcayaz = 1 -> gorunur parcalari kirp
static LONG g_zirhHepsi = 0;     // zirhhepsi = N
static volatile LONG g_zhHits = 0;
static LONG g_izleN = 0;         // izle = N -> model isteginin cagri yigini
static volatile LONG g_izleHits = 0;
static volatile LONG g_pdokHits = 0;
static int  g_kademeSv[21] = {};  // kademesv = <kademe> <seviye>
static int  g_kademeSvN = 0;
static volatile LONG g_sampHits = 0;
static volatile LONG g_eslemeHits = 0;

// vnum'dan silah tipini cikar. Klasik silahlarda binler hanesi
// tipi verir; 219XY serisinde son hane verir.
static int WeaponFamily(const char* vnum)
{
    size_t n = strlen(vnum);
    if (n == 5 && strncmp(vnum, "2190", 4) == 0)
    {
        // SADECE 21900-21909: yansitma MODEL dosyalari, son hane aile.
        // 21920, 21950 gibi digerleri yansitma ESYA vnum'lari; onlarda
        // son hane aile DEGIL. Oraya aile uydurunca bicakciya kilic
        // takiliyor ve karakter T-poza giriyordu.
        int y = vnum[4] - '0';
        return (y >= 0 && y <= 6) ? y : -1;
    }
    if (n == 5 && strncmp(vnum, "219", 3) == 0) return -1;   // bilinmiyor
    if (n < 4) return -1;
    int v = atoi(vnum);
    switch (v / 1000)
    {
    case 0: return 0;   // kilic
    case 1: return 1;   // hancer
    case 2: return 2;   // yay
    case 3: return 3;   // ciftel
    case 4: return 1;   // hancer (04xxx serisi)
    case 5: return 4;   // can
    case 6: return 6;   // pence
    case 7: return 5;   // yelpaze
    default: return -1;
    }
}

// probe = <desen> : eslesen yollari bir kez logla (vnum kesfi)
static char g_probe[8][PATH_LEN];
static int  g_probeCount = 0;
static LONG g_probeHits = 0;
// 12'den 30'a cikardim: ayni anda hem zirh hem binek deseni izleniyor,
// zirh istekleri sik geldigi icin 12'lik butceyi binek gelmeden bitiriyordu.
static const LONG TRACE_LIMIT = 30;

// Bos string ve ona isaret eden pointer (client'in beklediği iki bicim icin)

// hookfn = <rva> : o fonksiyonu hook'layip parametrelerini logla
#define MAX_FNHOOKS 6
struct FnHook { DWORD rva; void* orig; LONG count; };
static FnHook g_fnHook[MAX_FNHOOKS];
static int    g_fnHookCount = 0;
static LONG g_fnLogLimit = 10;   // fnlimit = N ile degisir
static LONG g_fnDumpLimit = 3;   // ilk N cagride ecx dokumu

static int g_logMask = 1 | 2 | 4;
// logdetay = 0 (varsayilan): loga sadece KARAR satirlari ve hatalar
// yazilir. logdetay = 1: eskisi gibi her dosya istegi / dokum de yazilir.
static int g_logDetay = 0;
static int g_lodEnKalite = 1;   // lodkalite = 0 -> LOD0 zorlamasi kapali

static __declspec(thread) char t_buffer[PATH_LEN];

static char g_recent[RECENT_COUNT][PATH_LEN];
static int  g_recentPos = 0;
// g_lock yukarida tanimli

// ---------------------------------------------------------------------------
// Log yazimi
// ESKI HALI HATALIYDI: her satir icin CreateFile/WriteFile/CloseHandle
// yapiyordu. Kalabalik yerde saniyede yuzlerce dosya istegi gelince
// bu, saniyede yuzlerce ac-kapa demek; ustelik FILE_APPEND_DATA her
// yazmada dosya sonuna konumlandigi icin log buyudukce yavasliyordu.
// Oyunun zamanla birkac FPS'e dusmesinin sebebi buydu.
//
// Yeni hali: dosya bir kez aciliyor, satirlar bellekte birikiyor,
// tampon dolunca veya belli araliklarla diske yaziliyor.
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static char   g_logBuf[64 * 1024];
static int    g_logLen = 0;
static DWORD  g_logLastFlush = 0;
static CRITICAL_SECTION g_logLock;
static bool   g_logLockReady = false;

static void LogFlushLocked()
{
    if (g_logLen <= 0) return;
    if (g_logFile == INVALID_HANDLE_VALUE)
    {
        g_logFile = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile == INVALID_HANDLE_VALUE) { g_logLen = 0; return; }
    }
    DWORD written = 0;
    WriteFile(g_logFile, g_logBuf, (DWORD)g_logLen, &written, nullptr);
    g_logLen = 0;
    g_logLastFlush = GetTickCount();
}

static void LogFlush()
{
    if (!g_logLockReady) return;
    EnterCriticalSection(&g_logLock);
    LogFlushLocked();
    LeaveCriticalSection(&g_logLock);
}

static void Log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (len < 0) return;
    if (len > (int)sizeof(line) - 3) len = (int)sizeof(line) - 3;
    line[len] = '\0';

    // HAFIF LOG: girintili satirlar (dosya istekleri, dokumler, tanilama)
    // sadece asagidaki etiketlerden birini tasiyorsa yazilir. Girintisiz
    // satirlar (acilis ozeti, F9, panel, kapanis) her zaman yazilir.
    if (!g_logDetay && line[0] == ' ')
    {
        static const char* const izin[] = {
            "[SEVIYE?]", "[PARCA>>]", "[YANS>>]", "[ZIRH>>]", "[ESYA>>]",
            "[kirp]", "[kostum]", "[BILINMEYEN", "[GUNCEL]", "[YUKLEME]", "[BINEK]", "[LOD]"
        };
        bool yaz = false;
        for (int i = 0; i < (int)(sizeof(izin) / sizeof(izin[0])); ++i)
            if (strstr(line, izin[i])) { yaz = true; break; }
        if (!yaz) return;
    }

    line[len++] = '\r';
    line[len++] = '\n';

    if (!g_logLockReady) return;
    EnterCriticalSection(&g_logLock);

    // TEKRAR ETMEYEN LOG: hafif modda, daha once birebir ayni yazilmis
    // karar satiri tekrar yazilmaz. Ayni oyuncu ayni silahla yuz kez
    // yeniden yuklense tek satir cikar; silahi/zirhi/seviyesi degisirse
    // satir da degisir ve yeniden yazilir.
    if (!g_logDetay && line[0] == ' ')
    {
        static DWORD gorulen[4096];
        static int   gorulenN = 0;
        DWORD h = 2166136261u;
        for (int i = 0; i < len - 2; ++i) { h ^= (BYTE)line[i]; h *= 16777619u; }
        if (h == 0) h = 1;
        if (gorulenN > 3000) { memset(gorulen, 0, sizeof(gorulen)); gorulenN = 0; }
        DWORD k = h & 4095;
        bool var = false;
        while (gorulen[k])
        {
            if (gorulen[k] == h) { var = true; break; }
            k = (k + 1) & 4095;
        }
        if (var) { LeaveCriticalSection(&g_logLock); return; }
        gorulen[k] = h;
        ++gorulenN;
    }

    if (g_logLen + len > (int)sizeof(g_logBuf))
        LogFlushLocked();

    if (len <= (int)sizeof(g_logBuf))
    {
        memcpy(g_logBuf + g_logLen, line, len);
        g_logLen += len;
    }

    // Cok uzun sure beklemesin diye saniyede bir de bosalt
    DWORD now = GetTickCount();
    if (now - g_logLastFlush > 1000) LogFlushLocked();

    LeaveCriticalSection(&g_logLock);
}

static bool IsGameProcess()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    name = name ? name + 1 : exe;
    return _stricmp(name, "metin2client.exe") == 0;
}

static bool IsReadable(const void* addr, size_t size)
{
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    BYTE* end = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    return ((BYTE*)addr + size) <= end;
}

static void NormalizeInto(const char* in, char* out, size_t outSize)
{
    size_t n = 0;
    for (const char* p = in; *p && n < outSize - 1; ++p, ++n)
    {
        char c = *p;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out[n] = c;
    }
    out[n] = '\0';
}

static void TrimInPlace(char* s)
{
    char* start = s;
    while (*start == ' ' || *start == '\t') ++start;
    char* end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
        end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
    if (start != s) memmove(s, start, (end - start) + 1);
}

static void ToLowerStr(char* s)
{
    for (char* p = s; *p; ++p)
    {
        if (*p == '\\') *p = '/';
        if (*p >= 'A' && *p <= 'Z') *p = *p - 'A' + 'a';
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GOMULU AYARLAR
// ---------------------------------------------------------------------------
// Ayarlar DLL'in icinde. Yaninda config.txt varsa O kullanilir
// (deneme yaparken ise yarar), yoksa asagidaki gomulu metin.
#include "gomulu_config.h"
static const int g_cfgParcaN = (int)(sizeof(g_cfgGomulu)/sizeof(g_cfgGomulu[0]));

static void LoadConfig()
{
    char cfgPath[MAX_PATH];
    sprintf_s(cfgPath, "%sconfig.txt", g_modDir);

    FILE* f = nullptr;
    bool dosyadan = (fopen_s(&f, cfgPath, "r") == 0 && f != nullptr);
    int parca = 0;
    const char* imlec = g_cfgGomulu[0];
    Log(dosyadan ? "ayarlar: config.txt (disaridan)" : "ayarlar: gomulu");

    char line[600];
    for (;;)
    {
        if (dosyadan)
        {
            if (!fgets(line, sizeof(line), f)) break;
        }
        else
        {
            while (!*imlec)
            {
                if (++parca >= g_cfgParcaN) break;
                imlec = g_cfgGomulu[parca];
            }
            if (!*imlec) break;
            const char* nl = strchr(imlec, '\n');
            size_t n = nl ? (size_t)(nl - imlec) : strlen(imlec);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, imlec, n);
            line[n] = '\0';
            imlec = nl ? nl + 1 : imlec + strlen(imlec);
        }

        // Satir sonu yorumunu kes:  "file 00270 = 00170   # aciklama"
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char* semi = strchr(line, ';');
        if (semi) *semi = '\0';

        TrimInPlace(line);
        if (!line[0]) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* left = line;
        char* right = eq + 1;
        TrimInPlace(left);
        TrimInPlace(right);
        if (!*left || !*right) continue;

        // maxtier = N
        if (_stricmp(left, "maxtier") == 0)
        {
            g_maxTier = atoi(right);
            continue;
        }

        // maxweapon = N
        if (_stricmp(left, "maxweapon") == 0)
        {
            g_maxWeapon = atoi(right);
            continue;
        }

        // weapon <tip> = <model>
        if (_strnicmp(left, "weapon", 6) == 0 && left[6] == ' ')
        {
            char* t = left + 7;
            TrimInPlace(t);
            if (*t && g_weaponCount < MAX_DEFAULTS && strlen(right) < NAME_LEN)
            {
                g_weapon[g_weaponCount].type = atoi(t);
                strcpy_s(g_weapon[g_weaponCount].model, right);
                ++g_weaponCount;
            }
            continue;
        }

        // log = ...
        if (_stricmp(left, "log") == 0)
        {
            if (_stricmp(right, "yok") == 0)           g_logMask = 0;
            else if (_stricmp(right, "karakter") == 0) g_logMask = 1;
            else if (_stricmp(right, "esya") == 0)     g_logMask = 2;
            else if (_stricmp(right, "pet") == 0)      g_logMask = 4;
            else                                        g_logMask = 7;
            continue;
        }

        // maxhair = N
        if (_stricmp(left, "maxhair") == 0)
        {
            g_maxHair = atoi(right);
            continue;
        }

        // defaulthair = <model>
        if (_stricmp(left, "defaulthair") == 0)
        {
            if (strlen(right) < NAME_LEN) strcpy_s(g_defaultHair, right);
            continue;
        }

        // petname = <rva>  (isim cizen fonksiyon; 0 = kapali)

        // equipfn = <rva>  : ekipman kurucusu. hookfn gibi kurulur, ek olarak
        // karakter<->esya eslemesini besler.
        if (_stricmp(left, "equipfn") == 0)
        {
            DWORD rva = (DWORD)strtoul(right, nullptr, 0);
            if (rva && g_fnHookCount < MAX_FNHOOKS)
            {
                g_equipRva = rva;
                g_fnHook[g_fnHookCount].rva = rva;
                g_fnHook[g_fnHookCount].count = 0;
                ++g_fnHookCount;
            }
            continue;
        }

        // hookfn = <rva>   (ornek: hookfn = 0x02669F40)
        if (_stricmp(left, "hookfn") == 0)
        {
            if (g_fnHookCount < MAX_FNHOOKS)
            {
                g_fnHook[g_fnHookCount].rva = (DWORD)strtoul(right, nullptr, 0);
                g_fnHook[g_fnHookCount].orig = nullptr;
                g_fnHook[g_fnHookCount].count = 0;
                ++g_fnHookCount;
            }
            continue;
        }

        // mountlearn = 1   (kendi kendine binek ogrenme)
        // pyown = <rva>,<rva>,...

        // pyrev = <rva>,<rva>,...

        // pyhook <ad> = <rva>

        // pyscan = isim1,isim2,...

        // mgrhook = <rva> : o fonksiyonun ecx'ini yonetici olarak yakala
        // objdump = <desen> : bu dosya istenirken iki katmanli dokum al

        // fnlimit = N  (kac cagri loglansin; dokum ilk N/3'unde)
        if (_stricmp(left, "fnlimit") == 0)
        {
            g_fnLogLimit = atoi(right);
            if (g_fnLogLimit < 1)  g_fnLogLimit = 1;
            if (g_fnLogLimit > 400) g_fnLogLimit = 400;
            g_fnDumpLimit = g_fnLogLimit / 3;
            if (g_fnDumpLimit < 1) g_fnDumpLimit = 1;
            continue;
        }

        if (_stricmp(left, "recvhook") == 0)
        {
            g_recvDump = atoi(right);
            continue;
        }

        // armorfn = <rva> : equipfn gibi, hook'u da kendisi kurar
        if (_stricmp(left, "armorfn") == 0)
        {
            DWORD rva = (DWORD)strtoul(right, nullptr, 0);
            if (rva && g_fnHookCount < MAX_FNHOOKS)
            {
                g_armorRva = rva;
                g_fnHook[g_fnHookCount].rva = rva;
                g_fnHook[g_fnHookCount].count = 0;
                ++g_fnHookCount;
            }
            continue;
        }

        if (_stricmp(left, "armorlog") == 0)
        {
            g_armorLog = atoi(right);
            continue;
        }

        if (_stricmp(left, "kostumgizle") == 0) { g_hideCostume = atoi(right); continue; }
        if (_stricmp(left, "zirhkirp")    == 0) { g_zirhKirp    = atoi(right); continue; }
        if (_stricmp(left, "sampiyonlog") == 0) { g_sampiyonLog = atoi(right); continue; }
        if (_stricmp(left, "seviyegoster")== 0) { g_seviyeGoster= atoi(right); continue; }
        if (_stricmp(left, "seviyeara")   == 0) { g_seviyeAra   = atoi(right); continue; }
        if (_stricmp(left, "paketdok")    == 0) { g_paketDok    = atoi(right); continue; }
        if (_stricmp(left, "izle")        == 0) { g_izleN       = atoi(right); continue; }
        if (_stricmp(left, "zirhhepsi")   == 0) { g_zirhHepsi   = atoi(right); continue; }
        if (_stricmp(left, "parcalog")    == 0) { g_parcaLog    = atoi(right); continue; }
        if (_stricmp(left, "nesnedok")    == 0) { g_nesneDok    = atoi(right); continue; }
        if (_stricmp(left, "yansara")     == 0) { g_yansAra     = atoi(right); continue; }
        if (_stricmp(left, "yanskaldir")  == 0) { g_yansKaldir  = atoi(right); continue; }
        if (_stricmp(left, "parcayaz")    == 0) { g_parcaYaz    = atoi(right); continue; }
        if (_stricmp(left, "bilinmeyensv")== 0) { g_bilinmeyenSv= atoi(right); continue; }
        if (_stricmp(left, "logdetay")    == 0) { g_logDetay    = atoi(right); continue; }
        if (_stricmp(left, "lodkalite")   == 0) { g_lodEnKalite = atoi(right); continue; }
        if (_stricmp(left, "vnumdegis")   == 0) { g_vnumDegis   = atoi(right); continue; }
        if (_stricmp(left, "kostumsilahgizle") == 0) { g_kostumSilahGizle = atoi(right); continue; }

        // kademesv = <kademe> <en az seviye>
        if (_stricmp(left, "kademesv") == 0)
        {
            int k = 0, sv = 0;
            if (sscanf_s(right, "%d %d", &k, &sv) == 2 && k >= 1 && k <= 20)
            {
                g_kademeSv[k] = sv;
                if (k > g_kademeSvN) g_kademeSvN = k;
            }
            continue;
        }

        // zirhmodel = <vnum> <gr2> <dds>
        if (_stricmp(left, "zirhmodel") == 0)
        {
            int v = 0; char g[NAME_LEN] = {}, d[NAME_LEN] = {};
            if (sscanf_s(right, "%d %63s %63s", &v,
                         g, (unsigned)sizeof(g), d, (unsigned)sizeof(d)) == 3)
            {
                if (v > 0 && g_amodN < AMOD_MAX)
                {
                    g_amod[g_amodN].vnum = v;
                    strcpy_s(g_amod[g_amodN].gr2, g);
                    strcpy_s(g_amod[g_amodN].dds, d);
                    ++g_amodN;
                }
            }
            continue;
        }

        if (_stricmp(left, "sampiyonguven") == 0) { g_trustGear = atoi(right); continue; }
        if (_stricmp(left, "eslemelog") == 0) { g_eslemeLog = atoi(right); continue; }
        if (_stricmp(left, "maxlevel") == 0) { g_maxLevel = atoi(right); continue; }
        if (_stricmp(left, "caplevel") == 0) { g_capLevel = atoi(right); continue; }

        // silah <aile> <seviye> <vnum>
        if (_stricmp(left, "silah") == 0)
        {
            char fam[16] = {}; int lvl = 0, base = 0;
            if (sscanf_s(right, "%15s %d %d", fam, (unsigned)sizeof(fam), &lvl, &base) == 3)
            {
                int f = WFamByName(fam);
                if (f >= 0 && lvl >= 0 && base > 0 && g_wtabN < WTAB_MAX)
                {
                    g_wtab[g_wtabN].fam  = (short)f;
                    g_wtab[g_wtabN].lvl  = (short)lvl;
                    g_wtab[g_wtabN].base = base;
                    g_wtab[g_wtabN].mask = 31;
                    ++g_wtabN;
                }
            }
            continue;
        }

        // zirhkademe = <sinif> <seviye> <model>
        if (_stricmp(left, "zirhkademe") == 0)
        {
            char c1[12] = {}, m1[NAME_LEN] = {}; int sv1 = 0;
            if (sscanf_s(right, "%11s %d %63s", c1, (unsigned)sizeof(c1), &sv1,
                         m1, (unsigned)sizeof(m1)) == 3 && g_zkadN < 64)
            {
                strcpy_s(g_zkad[g_zkadN].cls, c1);
                g_zkad[g_zkadN].sv = sv1;
                strcpy_s(g_zkad[g_zkadN].model, m1);
                ++g_zkadN;
            }
            continue;
        }

        // silahsinif = <vnum> <maske>
        if (_stricmp(left, "silahsinif") == 0)
        {
            int v = 0, m = 0;
            if (sscanf_s(right, "%d %d", &v, &m) == 2)
                for (int i = 0; i < g_wtabN; ++i)
                    if (g_wtab[i].base == v) g_wtab[i].mask = m;
            continue;
        }

        // kostumsilah = <vnum> <aile>
        if (_stricmp(left, "kostumsilah") == 0)
        {
            int v = 0, f = 0;
            if (sscanf_s(right, "%d %d", &v, &f) == 2 &&
                v > 0 && f >= 0 && f <= 6 && g_ksilN < 400)
            {
                g_ksil[g_ksilN].vnum = v;
                g_ksil[g_ksilN].fam  = f;
                ++g_ksilN;
            }
            continue;
        }

        // isim = <vnum> <ad>   (sadece log icin)
        if (_stricmp(left, "isim") == 0)
        {
            int v = 0; char ad[30] = {};
            if (sscanf_s(right, "%d %29[^\n]", &v, ad, (unsigned)sizeof(ad)) == 2 &&
                v > 0 && g_isimN < 400)
            {
                TrimInPlace(ad);
                g_isim[g_isimN].vnum = v;
                strcpy_s(g_isim[g_isimN].ad, ad);
                ++g_isimN;
            }
            continue;
        }

        // zirh <sinif> <seviye> <vnum>
        if (_stricmp(left, "zirh") == 0)
        {
            char cls[16] = {}; int lvl = 0, base = 0;
            if (sscanf_s(right, "%15s %d %d", cls, (unsigned)sizeof(cls), &lvl, &base) == 3)
            {
                if (lvl >= 0 && base > 0 && g_atabN < ATAB_MAX)
                {
                    strcpy_s(g_atab[g_atabN].cls, cls);
                    g_atab[g_atabN].lvl  = (short)lvl;
                    g_atab[g_atabN].base = base;
                    ++g_atabN;
                }
            }
            continue;
        }

        if (_stricmp(left, "equiplog") == 0)
        {
            g_equipLog = atoi(right);
            continue;
        }

        if (_stricmp(left, "mountall") == 0)
        {
            g_mountAll = (atoi(right) != 0);
            continue;
        }
        if (_stricmp(left, "mountlearn") == 0)
        {
            g_mountLearn = (atoi(right) != 0);
            continue;
        }
        if (_stricmp(left, "mountdir") == 0)
        {
            if (strlen(right) < PATH_LEN) strcpy_s(g_mountDir, right);
            continue;
        }
        // binekklasor = fire_tiger   (modeli duz npc/ altindan gelen binek)
        if (_stricmp(left, "binekklasor") == 0)
        {
            ToLowerStr(right);
            MFoldAdd(right);
            continue;
        }

        if (_stricmp(left, "mountmsm") == 0)
        {
            if (strlen(right) < PATH_LEN) strcpy_s(g_mountMsm, right);
            continue;
        }
        // clamp <0-6> = <vnum>
        if (_strnicmp(left, "clamp", 5) == 0 && left[5] == ' ')
        {
            char* t = left + 6;
            while (*t == ' ') ++t;
            int fam = atoi(t);
            if (fam >= 0 && fam <= 6) g_clampTop[fam] = atoi(right);
            continue;
        }

        // costume <0-6> = <tam yol>
        if (_strnicmp(left, "costume", 7) == 0 && left[7] == ' ')
        {
            char* t = left + 8;
            while (*t == ' ') ++t;
            int fam = atoi(t);
            if (fam >= 0 && fam <= 6 && strlen(right) < PATH_LEN)
                strcpy_s(g_costumeFam[fam], right);
            continue;
        }

        if (_stricmp(left, "costumedefault") == 0)
        {
            if (strlen(right) < PATH_LEN) strcpy_s(g_costumeDefault, right);
            continue;
        }

        // dir <desen> = <hedef klasor>   (dosya adi korunur)
        if (_strnicmp(left, "dir", 3) == 0 && left[3] == ' ')
        {
            char* pat = left + 4;
            while (*pat == ' ') ++pat;
            if (*pat && g_dirCount < 64 &&
                strlen(pat) < PATH_LEN && strlen(right) < PATH_LEN)
            {
                strcpy_s(g_dir[g_dirCount].pattern, pat);
                ToLowerStr(g_dir[g_dirCount].pattern);
                strcpy_s(g_dir[g_dirCount].target, right);
                ++g_dirCount;
            }
            continue;
        }

        // anim <kaynak> = <hedef>   (dir icinde dosya adi cevirisi)
        if (_strnicmp(left, "anim", 4) == 0 && left[4] == ' ')
        {
            char* src = left + 5;
            while (*src == ' ') ++src;
            if (*src && g_animCount < 64 &&
                strlen(src) < NAME_LEN && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_anim[g_animCount].from, src);
                ToLowerStr(g_anim[g_animCount].from);
                strcpy_s(g_anim[g_animCount].to, right);
                ++g_animCount;
            }
            continue;
        }

        // animdefault / msmdefault
        if (_stricmp(left, "animdefault") == 0)
        {
            if (strlen(right) < NAME_LEN) strcpy_s(g_animDefault, right);
            continue;
        }
        if (_stricmp(left, "msmdefault") == 0)
        {
            if (strlen(right) < NAME_LEN) strcpy_s(g_msmDefault, right);
            continue;
        }

        // probe = <desen>   (eslesen yollari bir kez logla)
        if (_stricmp(left, "probe") == 0)
        {
            if (g_probeCount < 8 && strlen(right) < PATH_LEN)
            {
                strcpy_s(g_probe[g_probeCount], right);
                ToLowerStr(g_probe[g_probeCount]);
                ++g_probeCount;
            }
            continue;
        }

        // trace = <desen>
        if (_stricmp(left, "trace") == 0)
        {
            if (g_traceCount < 8 && strlen(right) < PATH_LEN)
            {
                strcpy_s(g_trace[g_traceCount], right);
                ToLowerStr(g_trace[g_traceCount]);
                ++g_traceCount;
            }
            continue;
        }

        // redirect <desen> = <tam hedef yol>
        if (_strnicmp(left, "redirect", 8) == 0 && left[8] == ' ')
        {
            char* pat = left + 9;
            TrimInPlace(pat);
            if (*pat && g_redirectCount < MAX_HIDES &&
                strlen(pat) < PATH_LEN && strlen(right) < PATH_LEN)
            {
                strcpy_s(g_redirect[g_redirectCount].pattern, pat);
                ToLowerStr(g_redirect[g_redirectCount].pattern);
                strcpy_s(g_redirect[g_redirectCount].target, right);

                // Hedefin uzantisini sakla: sadece ayni uzantili istekler yakalanir
                const char* dot = strrchr(right, '.');
                if (dot && strlen(dot) < 12)
                    strcpy_s(g_redirect[g_redirectCount].ext, dot);
                else
                    g_redirect[g_redirectCount].ext[0] = '\0';

                ++g_redirectCount;
            }
            continue;
        }

        // hide = <desen>
        if (_strnicmp(left, "hide", 4) == 0 && (left[4] == '\0' || left[4] == ' '))
        {
            if (g_hideCount < MAX_HIDES && strlen(right) < PATH_LEN)
            {
                strcpy_s(g_hide[g_hideCount], right);
                ToLowerStr(g_hide[g_hideCount]);
                ++g_hideCount;
            }
            continue;
        }

        // keep = <model>
        if (_strnicmp(left, "keep", 4) == 0 && (left[4] == '\0' || left[4] == ' '))
        {
            if (g_keepCount < MAX_KEEPS && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_keep[g_keepCount], right);
                ToLowerStr(g_keep[g_keepCount]);
                ++g_keepCount;
            }
            continue;
        }

        // file <kaynak> = <hedef>   (her yolda calisir)
        if (_strnicmp(left, "file", 4) == 0 && left[4] == ' ')
        {
            char* src = left + 5;
            TrimInPlace(src);
            if (*src && g_fileMapCount < MAX_MODELMAPS &&
                strlen(src) < NAME_LEN && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_fileMap[g_fileMapCount].from, src);
                ToLowerStr(g_fileMap[g_fileMapCount].from);
                strcpy_s(g_fileMap[g_fileMapCount].to, right);
                ++g_fileMapCount;
            }
            continue;
        }

        // model <kaynak> = <hedef>
        if (_strnicmp(left, "model", 5) == 0 && left[5] == ' ')
        {
            char* src = left + 6;
            TrimInPlace(src);
            if (*src && g_modelMapCount < MAX_MODELMAPS &&
                strlen(src) < NAME_LEN && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_modelMap[g_modelMapCount].from, src);
                ToLowerStr(g_modelMap[g_modelMapCount].from);
                strcpy_s(g_modelMap[g_modelMapCount].to, right);
                ++g_modelMapCount;
            }
            continue;
        }

        // clamptier <sinif> = <model>
        if (_strnicmp(left, "clamptier", 9) == 0 && left[9] == ' ')
        {
            char* cls = left + 10;
            TrimInPlace(cls);
            if (*cls && g_clampTierCount < MAX_DEFAULTS &&
                strlen(cls) < NAME_LEN && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_clampTier[g_clampTierCount].className, cls);
                ToLowerStr(g_clampTier[g_clampTierCount].className);
                strcpy_s(g_clampTier[g_clampTierCount].model, right);
                ++g_clampTierCount;
            }
            continue;
        }

        // default <sinif> = <model>
        if (_strnicmp(left, "default", 7) == 0 && left[7] == ' ')
        {
            char* cls = left + 8;
            TrimInPlace(cls);
            if (*cls && g_defaultCount < MAX_DEFAULTS &&
                strlen(cls) < NAME_LEN && strlen(right) < NAME_LEN)
            {
                strcpy_s(g_default[g_defaultCount].className, cls);
                ToLowerStr(g_default[g_defaultCount].className);
                strcpy_s(g_default[g_defaultCount].model, right);
                ++g_defaultCount;
            }
            continue;
        }

        // acik esleme: tam yol = tam yol
        if (g_mapCount < MAX_MAPPINGS &&
            strlen(left) < PATH_LEN && strlen(right) < PATH_LEN)
        {
            NormalizeInto(left, g_map[g_mapCount].from, PATH_LEN);
            strcpy_s(g_map[g_mapCount].to, right);
            ++g_mapCount;
        }
    }

    if (f) fclose(f);
    Log("config: maxtier=%d maxhair=%d | %d yol, %d model, %d dosya, %d keep, "
        "%d varsayilan, %d gizleme, %d yonlendirme, %d klasor, %d anim, "
        "%d probe, %d trace, %d ogrenilmis binek",
        g_maxTier, g_maxHair, g_mapCount, g_modelMapCount, g_fileMapCount,
        g_keepCount, g_defaultCount, g_hideCount, g_redirectCount,
        g_dirCount, g_animCount, g_probeCount, g_traceCount, g_mountCount);

    // Tablolari acikca rapor et: bu tur 243 satir sessizce yuklenmemisti
    // (config parser '=' olmayan satiri atliyor, benim satirlarimda yoktu).
    // Bir daha sessiz kalmasin.
    Log("tablo: %d silah, %d zirh, %d zirhmodel | maxlevel=%d caplevel=%d "
        "sampiyonguven=%d kostumgizle=%d",
        g_wtabN, g_atabN, g_amodN, g_maxLevel, g_capLevel, g_trustGear, g_hideCostume);
    if (g_wtabN == 0) Log("  !! SILAH TABLOSU BOS -- seviye kurali CALISMAZ");
    if (g_atabN == 0) Log("  !! ZIRH TABLOSU BOS");
}

// Tum kural sayaclarini sifirlayip config'i bastan oku.
static void ReloadConfig()
{
    EnterCriticalSection(&g_lock);
    g_mapCount = g_modelMapCount = g_fileMapCount = 0;
    g_keepCount = g_defaultCount = g_hideCount = g_redirectCount = 0;
    g_wtabN = g_atabN = g_amodN = 0; g_zkadN = 0;
    g_clampTierCount = 0;
    g_weaponCount = g_dirCount = g_animCount = 0;
    g_traceCount = g_probeCount = 0;
    g_animDefault[0] = g_msmDefault[0] = '\0';
    g_costumeDefault[0] = '\0';
    for (int i = 0; i < 7; ++i) g_costumeFam[i][0] = '\0';
    LeaveCriticalSection(&g_lock);

    LoadConfig();
}

// ---------------------------------------------------------------------------
struct ParsedPath
{
    bool valid;
    int  prefixLen;
    char className[NAME_LEN];
    char modelName[NAME_LEN];
    char lodSuffix[16];
};

static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

static void ParsePath(const char* original, const char* norm, ParsedPath* out)
{
    out->valid = false;

    size_t len = strlen(norm);
    if (len < 8 || strcmp(norm + len - 4, ".gr2") != 0) return;

    const char* pc = strstr(norm, "/pc/");
    if (!pc) pc = strstr(norm, "/pc2/");
    if (!pc) pc = strstr(norm, "/pc3/");
    if (!pc) return;

    const char* afterPc = strchr(pc + 1, '/');
    if (!afterPc) return;
    const char* afterClass = strchr(afterPc + 1, '/');
    if (!afterClass) return;
    if (strchr(afterClass + 1, '/')) return;

    size_t clsLen = (size_t)(afterClass - (afterPc + 1));
    if (clsLen == 0 || clsLen >= NAME_LEN) return;
    memcpy(out->className, afterPc + 1, clsLen);
    out->className[clsLen] = '\0';

    const char* fileStart = afterClass + 1;
    size_t fileLen = strlen(fileStart) - 4;
    if (fileLen == 0 || fileLen >= NAME_LEN) return;

    char fileName[NAME_LEN];
    memcpy(fileName, fileStart, fileLen);
    fileName[fileLen] = '\0';

    out->lodSuffix[0] = '\0';
    if (fileLen > 7)
    {
        const char* lod = fileName + fileLen - 7;
        if (strncmp(lod, "_lod_", 5) == 0 && IsDigit(lod[5]) && IsDigit(lod[6]))
        {
            strcpy_s(out->lodSuffix, lod);
            fileName[fileLen - 7] = '\0';
        }
    }

    strcpy_s(out->modelName, fileName);

    out->prefixLen = (int)(fileStart - norm);
    if (out->prefixLen <= 0 || out->prefixLen >= (int)strlen(original)) return;

    out->valid = true;
}

// Zirh kademesi:  -1 = klasik kalip degil,  0 = novice,  1..N = kademe
static int GetTier(const char* model, const char* className)
{
    size_t cl = strlen(className);
    if (strncmp(model, className, cl) != 0) return -1;
    if (model[cl] != '_') return -1;

    const char* rest = model + cl + 1;
    if (!*rest) return -1;
    if (strcmp(rest, "novice") == 0) return 0;

    const char* p = rest;
    if (!IsDigit(*p)) return -1;

    int tier = 0;
    while (IsDigit(*p)) { tier = tier * 10 + (*p - '0'); ++p; }

    if (*p != '_' && *p != '-') return -1;
    ++p;
    if (!IsDigit(*p)) return -1;
    while (IsDigit(*p)) ++p;
    if (*p != '\0') return -1;

    return tier;
}

static bool IsKept(const char* model)
{
    for (int i = 0; i < g_keepCount; ++i)
        if (strcmp(g_keep[i], model) == 0) return true;
    return false;
}

static const char* FindModelMap(const char* model)
{
    for (int i = 0; i < g_modelMapCount; ++i)
        if (strcmp(g_modelMap[i].from, model) == 0)
            return g_modelMap[i].to;
    return nullptr;
}

static const char* FindClampTier(const char* className)
{
    for (int i = 0; i < g_clampTierCount; ++i)
        if (strcmp(g_clampTier[i].className, className) == 0)
            return g_clampTier[i].model;
    return nullptr;
}

static const char* FindDefault(const char* className)
{
    for (int i = 0; i < g_defaultCount; ++i)
        if (strcmp(g_default[i].className, className) == 0)
            return g_default[i].model;
    return nullptr;
}

// Desen bosluklarla ayrilmis parcalar icerebilir; hepsi eslesmeli.
// "!" ile baslayan parca: o metin yolda GECMEMELI.
static bool PatternMatches(const char* norm, const char* pattern)
{
    const char* p = pattern;
    while (*p)
    {
        while (*p == ' ') ++p;
        if (!*p) break;

        char part[PATH_LEN];
        int n = 0;
        while (*p && *p != ' ' && n < PATH_LEN - 1) part[n++] = *p++;
        part[n] = '\0';
        if (n == 0) continue;

        if (part[0] == '!')
        {
            if (part[1] && strstr(norm, part + 1)) return false;
        }
        else
        {
            if (!strstr(norm, part)) return false;
        }
    }
    return true;
}

static bool ShouldHide(const char* norm)
{
    // SADECE .gr2 gizlenir.
    //   .msm -> ana tanim dosyasi, gizlenirse client coker
    //   .txt -> motlist gibi hareket tanimlari, gizlenirse davranis bozulur
    //   .dds -> doku, gizlenirse model siyah/bozuk gorunur
    const char* dot = strrchr(norm, '.');
    if (!dot) return false;
    //   .avi -> acilis videosu. Yuklenemezse istemci atlayip geciyor,
    //   zararsiz. Ozel sunucularda bu dosyalar zaten sik siliniyor.
    bool izin = (_stricmp(dot, ".gr2") == 0) || (_stricmp(dot, ".avi") == 0);
    if (!izin) return false;

    for (int i = 0; i < g_hideCount; ++i)
        if (PatternMatches(norm, g_hide[i])) return true;
    return false;
}

// Gizleme icin, orijinalin uzantisini koruyan sahte yol uret
static void MakeHiddenPath(const char* norm, char* out, size_t outSize)
{
    const char* dot = strrchr(norm, '.');
    if (dot && strlen(dot) < 8)
        sprintf_s(out, outSize, "%s%s", HIDDEN_BASE, dot);
    else
        sprintf_s(out, outSize, "%s.gr2", HIDDEN_BASE);
}

static bool SeenBefore(const char* path)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < RECENT_COUNT; ++i)
    {
        if (g_recent[i][0] && strcmp(g_recent[i], path) == 0)
        {
            LeaveCriticalSection(&g_lock);
            return true;
        }
    }
    strcpy_s(g_recent[g_recentPos], path);
    g_recentPos = (g_recentPos + 1) % RECENT_COUNT;
    LeaveCriticalSection(&g_lock);
    return false;
}

static const char* LogTag(const char* norm)
{
    const char* dot = strrchr(norm, '.');
    if (!dot) return nullptr;
    if (strcmp(dot, ".gr2") != 0 && strcmp(dot, ".msm") != 0) return nullptr;

    if (strstr(norm, "npc_pet/"))   return (g_logMask & 4) ? "PET  " : nullptr;
    if (strstr(norm, "npc_mount/")) return (g_logMask & 4) ? "BINEK" : nullptr;
    if (strstr(norm, "/npc/"))      return (g_logMask & 4) ? "NPC  " : nullptr;
    if (strstr(norm, "/npc2/"))     return (g_logMask & 4) ? "NPC2 " : nullptr;
    if (strstr(norm, "/item/"))
        return (g_logMask & 2) ? "ESYA " : nullptr;
    // Karakter govdesi: /pc/<sinif>/<model>.gr2  (alt klasor yoksa)
    const char* pc = strstr(norm, "/pc/");
    if (!pc) pc = strstr(norm, "/pc2/");
    if (!pc) pc = strstr(norm, "/pc3/");
    if (pc)
    {
        const char* afterPc = strchr(pc + 1, '/');
        if (!afterPc) return nullptr;
        const char* afterClass = strchr(afterPc + 1, '/');
        if (!afterClass) return nullptr;

        // hair/ alt klasoru: sac ve kafa kostumleri
        if (strncmp(afterClass + 1, "hair/", 5) == 0)
            return (g_logMask & 1) ? "SAC  " : nullptr;

        if (strchr(afterClass + 1, '/')) return nullptr;   // animasyon klasoru
        return (g_logMask & 1) ? "KARA " : nullptr;
    }

    return nullptr;
}

// Prefix + model + lod + .gr2 birlestir
static bool BuildPath(const char* original, const ParsedPath* pp,
    const char* model, char* out, size_t outSize)
{
    if (pp->prefixLen + strlen(model) + strlen(pp->lodSuffix) + 5 >= outSize)
        return false;

    memcpy(out, original, pp->prefixLen);
    out[pp->prefixLen] = '\0';
    strcat_s(out, outSize, model);
    strcat_s(out, outSize, pp->lodSuffix);
    strcat_s(out, outSize, ".gr2");
    return true;
}

// ---------------------------------------------------------------------------
// Python metot tablosu taramasi
// ---------------------------------------------------------------------------
// exe imajinin okunabilir bolgelerini gezip once isim metnini,
// sonra o metne isaret eden isaretciyi buluyoruz. PyMethodDef
// yapisi { const char* ml_name; PyCFunction ml_meth; ... } oldugu
// icin isaretciden 4 byte sonrasi fonksiyon adresi.
struct MemRange { BYTE* base; SIZE_T size; };

// ---------------------------------------------------------------------------
// Kisayol tuslari
// ---------------------------------------------------------------------------
// Oyunun Esc menusune gercek bir sekme eklemek, client'in Python
// arayuz dosyalarini degistirmeyi gerektiriyor; o dosyalar sifreli
// paketlerin icinde. Onun yerine tus kisayolu: oyun dosyalarina
// hic dokunmuyor, ayni isi goruyor.
//
//   F9  : config.txt'yi yeniden oku (oyunu kapatmadan kural degistir)
//   F10 : ayar panelini ac / kapa (kanallari tek tek acip kapatma)
//
// Tuslar sadece oyun penceresi onde iken dinlenir.
static bool GameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static volatile LONG g_shutdown = 0;   // DLL bosaltilirken 1 olur

// Okunabilir koruma bayraklari -- BEYAZ LISTE.
// Ilk surumde kara liste kullandim ("NOACCESS degilse oku") ve oyun
// coktu: PAGE_EXECUTE gibi okunamayan bayraklar listeden gecti.
static bool ProtReadable(DWORD prot)
{
    DWORD p = prot & ~(DWORD)(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return p == PAGE_READONLY          || p == PAGE_READWRITE ||
           p == PAGE_WRITECOPY         || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// Bulus kaydi: tarama sirasinda SADECE adres topluyoruz. Log cagrisi
// __try icinde olmasin -- orada patlarsa log kilidi askida kalir.
struct HuntHit { int ni; BYTE* at; };

// Ilk harf tablosu. 1. denemede her bayt icin 11 ismin hepsini tek tek
// karsilastiriyordum: bayt basina 11 islem, yuz milyonlarca bayt. Tarama
// bitmedi. Simdi bayt basina tek tablo bakisi var; isim dongusune ancak
// ilk harf tutarsa giriliyor.

// Tarama ARTIK TEK SEFERDE YAPILMIYOR.
// Iki denemede de tarama bitmedi ve 25 sn'lik zaman butcesi bile
// devreye girmedi, cunku butceyi sadece bolgeler ARASINDA kontrol
// ediyordum: tek bir buyuk bolge icinde takilinca kontrol noktasina
// hic ulasilmiyordu. Simdi tarama durum makinesi: hotkey dongusunun
// her turunda (120 ms) en fazla 16 MB taraniyor, sonra cikiliyor.
// Boylece hicbir zaman bloklanmiyor ve ilerleme loglaniyor.
#define HUNT_CHUNK   (16u << 20)
#define HUNT_OVERLAP 320          // parca sinirinda dizgi kesilmesin

// ---------------------------------------------------------------------------
// F9 PANELI  (Metin2'nin kendi arayuz gorselleriyle)
// Oyunun ustunde duran kucuk bir pencere: her mudahale kanali icin bir
// ac/kapa dugmesi. Dugmeler dogrudan ayar degiskenlerini degistirir, dosyaya
// yazmaz. Gorseller ymir work\ui klasorunden alinip asagiya gomuludur
// (araclar/panel_gorsel.py): pano = pattern\board_*.tga, baslik =
// pattern\titlebar_*.tga, dugmeler = public.dds icindeki middle/large/close.
//
// Pencere "katmanli" (WS_EX_LAYERED): butun panel bellekte 32 bit bir
// resme cizilir ve saydamligiyla birlikte ekrana verilir; boylece panonun
// yuvarlak koseleri oyundaki gibi gorunur. Alt pencere (kontrol) yok, fare
// tiklamalarini kendimiz isliyoruz. WS_EX_NOACTIVATE: tiklaninca oyun odagi
// kaybetmez. Kendi is parcaciginda calisir, render thread'ine dokunmaz.
// >>> PANEL KOD BASLANGIC
#include "panel_gorsel.h"

// tip: 0 int, 1 bool, 2 LONG, 3 = "Klasik gorunum" (asagidaki grup birlikte)
struct PanelSec { const char* ad; const wchar_t* etiket; int tip; void* p; };
static PanelSec g_panelSec[] = {
    { "Klasik gorunum",        L"Klasik g\u00F6r\u00FCn\u00FCm",                 3, nullptr },
    { "Silah kostumunu gizle", L"Silah kost\u00FCm\u00FCn\u00FC gizle",          0, &g_kostumSilahGizle },
    { "Zirh kostumunu gizle",  L"Z\u0131rh kost\u00FCm\u00FCn\u00FC gizle",     0, &g_zirhKostumGizle },
    { "Binekleri ata cevir",   L"Binekleri ata \u00E7evir",                        1, &g_mountAll },
};
#define PANEL_N          ((int)(sizeof(g_panelSec) / sizeof(g_panelSec[0])))
#define PANEL_MSG_GOSTER (WM_APP + 1)
#define PANEL_MSG_KAPAT  (WM_APP + 2)

// Kontrol kimlikleri: 0..PANEL_N-1 ac/kapa dugmeleri, sonra bunlar.
#define PG_ID_ORJ    100
#define PG_ID_ILK    101
#define PG_ID_KAPAT  102

// Yerlesim (piksel)
#define PG_W       300
#define PG_SATIR_Y 44
#define PG_SATIR_H 24
#define PG_H       (PG_SATIR_Y + PANEL_N * PG_SATIR_H + 8 + 30 + 16)

static HWND    g_panelWnd = nullptr;
static int     g_panelIlk[16];        // panel ilk acildigindaki degerler
static HANDLE  g_panelThread = nullptr;
static HDC     g_pgDC  = nullptr;     // bellek DC'si
static HBITMAP g_pgBmp = nullptr;
static DWORD*  g_pgBuf = nullptr;     // PG_W x PG_H, ustten asagi, 0xAARRGGBB
static BYTE*   g_pgAlfa = nullptr;    // yazi cizilirken alfa yedegi
static HFONT   g_pgFont = nullptr;
static int     g_pgBasili = -1;       // fare basili tutulan kontrol
static int     g_pgUzerinde = -1;     // farenin uzerinde durdugu kontrol
static int     g_pgSonDurum = -1;     // en son cizilen durum (degisince yeniden ciz)

static int PanelOku(int i)
{
    void* p = g_panelSec[i].p;
    switch (g_panelSec[i].tip)
    {
    case 3:  return g_vnumDegis ? 1 : 0;
    case 1:  return *(bool*)p ? 1 : 0;
    case 2:  return *(volatile LONG*)p ? 1 : 0;
    default: return *(int*)p ? 1 : 0;
    }
}

static void PanelYaz(int i, int v)
{
    void* p = g_panelSec[i].p;
    switch (g_panelSec[i].tip)
    {
    case 3:
        // Klasik gorunum: butun cevirme kanallari birlikte acilir/kapanir.
        g_vnumDegis  = v ? 1 : 0;
        g_parcaYaz   = v ? 1 : 0;
        g_yansKaldir = v ? 1 : 0;
        g_zirhKirp   = v ? 1 : 0;
        InterlockedExchange(&g_enabled, v ? 1 : 0);
        break;
    case 1:  *(bool*)p = (v != 0); break;
    case 2:  InterlockedExchange((volatile LONG*)p, v ? 1 : 0); break;
    default: *(int*)p = v ? 1 : 0; break;
    }
}

// Kontrolun panel icindeki dikdortgeni.
static bool PgRect(int id, RECT* r)
{
    if (id >= 0 && id < PANEL_N)
    {
        r->left = PG_W - 24 - 61; r->top = PG_SATIR_Y + id * PG_SATIR_H + 1;
        r->right = r->left + 61;  r->bottom = r->top + 21;
        return true;
    }
    int by = PG_SATIR_Y + PANEL_N * PG_SATIR_H + 12;
    if (id == PG_ID_ORJ) { r->left = 52;  r->top = by; r->right = 52 + 88;  r->bottom = by + 21; return true; }
    if (id == PG_ID_ILK) { r->left = 160; r->top = by; r->right = 160 + 88; r->bottom = by + 21; return true; }
    if (id == PG_ID_KAPAT)
    {
        r->left = 8 + (PG_W - 15) - 15 - 4; r->top = 7 + 3;
        r->right = r->left + 15; r->bottom = r->top + 15;
        return true;
    }
    return false;
}

static int PgVurus(int x, int y)
{
    static const int ek[] = { PG_ID_KAPAT };
    RECT r;
    for (int i = 0; i < PANEL_N; ++i)
        if (PgRect(i, &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    for (int k = 0; k < 1; ++k)
        if (PgRect(ek[k], &r) && x >= r.left && x < r.right && y >= r.top && y < r.bottom) return ek[k];
    return -1;
}

// Kaynak resmi (duz alfa) hedefin (dx,dy) noktasina w x h alani dolduracak
// sekilde DOSEYEREK ustune koyar. w,h resimden kucukse kirpilmis olur.
static void PgDose(const PgResim& s, int dx, int dy, int w, int h)
{
    if (!g_pgBuf) return;
    for (int y = 0; y < h; ++y)
    {
        int ty = dy + y;
        if (ty < 0 || ty >= PG_H) continue;
        const DWORD* sr = s.px + (y % s.h) * s.w;
        DWORD* dr = g_pgBuf + ty * PG_W;
        for (int x = 0; x < w; ++x)
        {
            int tx = dx + x;
            if (tx < 0 || tx >= PG_W) continue;
            DWORD sp = sr[x % s.w];
            DWORD sa = sp >> 24;
            if (!sa) continue;
            if (sa == 255) { dr[tx] = sp; continue; }
            DWORD dp = dr[tx];
            DWORD da = dp >> 24;
            DWORD kalan = da * (255 - sa) / 255;       // hedefin katkisi
            DWORD oa = sa + kalan;
            if (!oa) continue;
            DWORD rr = (((sp >> 16) & 255) * sa + ((dp >> 16) & 255) * kalan) / oa;
            DWORD gg = (((sp >> 8)  & 255) * sa + ((dp >> 8)  & 255) * kalan) / oa;
            DWORD bb = (( sp        & 255) * sa + ( dp        & 255) * kalan) / oa;
            dr[tx] = (oa << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
}

static void PgKoy(const PgResim& s, int dx, int dy) { PgDose(s, dx, dy, s.w, s.h); }

static void PgYazi(const wchar_t* t, int l, int tp, int r, int b, UINT bicim, COLORREF renk)
{
    RECT rc = { l, tp, r, b };
    SetTextColor(g_pgDC, renk);
    DrawTextW(g_pgDC, t, -1, &rc, bicim | DT_SINGLELINE | DT_NOPREFIX);
}

// Butun paneli bellekteki resme cizer (pencereye dokunmaz).
static void PgBoya()
{
    if (!g_pgBuf) return;
    const int W = PG_W, H = PG_H;
    memset(g_pgBuf, 0, (size_t)W * H * 4);

    // --- Pano: ic dolgu, dort kenar, dort kose ---
    PgDose(pg_base,     32,     32,     W - 64, H - 64);
    PgDose(pg_l_top,    32,     0,      W - 64, 32);
    PgDose(pg_l_bottom, 32,     H - 32, W - 64, 32);
    PgDose(pg_l_left,   0,      32,     32,     H - 64);
    PgDose(pg_l_right,  W - 32, 32,     32,     H - 64);
    PgKoy(pg_c_lt, 0,      0);
    PgKoy(pg_c_rt, W - 32, 0);
    PgKoy(pg_c_lb, 0,      H - 32);
    PgKoy(pg_c_rb, W - 32, H - 32);

    // --- Baslik cubugu (8,7), genislik W-15 ---
    const int tx = 8, ty = 7, tw = W - 15;
    PgKoy(pg_t_left, tx, ty);
    PgDose(pg_t_center, tx + 32, ty, tw - 64, 32);
    PgKoy(pg_t_right, tx + tw - 32, ty);

    RECT r;
    // Kapat dugmesi
    PgRect(PG_ID_KAPAT, &r);
    PgKoy(g_pgBasili == PG_ID_KAPAT ? pg_close_3
          : g_pgUzerinde == PG_ID_KAPAT ? pg_close_2 : pg_close_1, r.left, r.top);

    // Ac/kapa dugmeleri: ACIK = basili gorunum; fare uzerindeyse parlak
    for (int i = 0; i < PANEL_N; ++i)
    {
        PgRect(i, &r);
        bool acik = PanelOku(i) != 0;
        PgKoy((acik || g_pgBasili == i) ? pg_mid_3
              : g_pgUzerinde == i ? pg_mid_2 : pg_mid_1, r.left, r.top);
    }
    // --- Yazilar. GDI yazi cizerken alfayi sifirliyor; once yedekle,
    //     sonra geri koy (yazi sadece opak zemine geliyor). ---
    if (g_pgAlfa)
        for (int i = 0; i < W * H; ++i) g_pgAlfa[i] = (BYTE)(g_pgBuf[i] >> 24);

    HGDIOBJ eskiFont = g_pgFont ? SelectObject(g_pgDC, g_pgFont) : nullptr;
    SetBkMode(g_pgDC, TRANSPARENT);

    PgYazi(L"G\u00F6rsel Mod", tx, ty + 1, tx + tw, ty + 22, DT_CENTER | DT_VCENTER,
           RGB(255, 232, 190));

    for (int i = 0; i < PANEL_N; ++i)
    {
        int sy = PG_SATIR_Y + i * PG_SATIR_H;
        PgYazi(g_panelSec[i].etiket, 22, sy, PG_W - 24 - 61 - 6, sy + PG_SATIR_H - 1,
               DT_LEFT | DT_VCENTER, RGB(232, 222, 200));
        PgRect(i, &r);
        bool acik = PanelOku(i) != 0;
        int k = (acik || g_pgBasili == i) ? 1 : 0;      // basili gorunumde 1 px kay
        PgYazi(acik ? L"A\u00E7\u0131k" : L"Kapal\u0131",
               r.left + k, r.top + k, r.right + k, r.bottom + k,
               DT_CENTER | DT_VCENTER,
               acik ? RGB(255, 255, 255) : RGB(190, 180, 160));
    }

    int ny = PG_SATIR_Y + PANEL_N * PG_SATIR_H + 8;
    PgYazi(L"Ayarlar birka\u00E7 saniyede uygulan\u0131r.",
           12, ny, PG_W - 12, ny + 15, DT_CENTER | DT_VCENTER, RGB(170, 160, 140));
    PgYazi(L"F9: paneli a\u00E7 / kapat",
           12, ny + 15, PG_W - 12, ny + 30, DT_CENTER | DT_VCENTER, RGB(170, 160, 140));

    if (eskiFont) SelectObject(g_pgDC, eskiFont);
    GdiFlush();

    if (g_pgAlfa)
        for (int i = 0; i < W * H; ++i)
            g_pgBuf[i] = (g_pgBuf[i] & 0x00FFFFFF) | ((DWORD)g_pgAlfa[i] << 24);
}

static int PgDurum()
{
    int d = 0;
    for (int i = 0; i < PANEL_N; ++i) if (PanelOku(i)) d |= 1 << i;
    return d | ((g_pgBasili + 1) << 16) | ((g_pgUzerinde + 1) << 24);
}

// Bellek resmini ve yazi tipini hazirlar. Basarisizsa false.
static bool PgHazirla()
{
    if (g_pgBuf) return true;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = PG_W;
    bi.bmiHeader.biHeight      = -PG_H;          // ustten asagi
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC ekran = GetDC(nullptr);
    g_pgDC = CreateCompatibleDC(ekran);
    void* bits = nullptr;
    g_pgBmp = CreateDIBSection(ekran, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, ekran);
    if (!g_pgDC || !g_pgBmp || !bits) return false;
    SelectObject(g_pgDC, g_pgBmp);
    g_pgBuf  = (DWORD*)bits;
    g_pgAlfa = (BYTE*)malloc((size_t)PG_W * PG_H);
    g_pgFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
    return true;
}

// Paneli cizip ekrana verir.
static void PgSun(HWND h)
{
    if (!PgHazirla()) return;
    PgBoya();
    g_pgSonDurum = PgDurum();

    // UpdateLayeredWindow "onceden carpilmis" alfa ister.
    for (int i = 0; i < PG_W * PG_H; ++i)
    {
        DWORD p = g_pgBuf[i], a = p >> 24;
        if (a == 255) continue;
        DWORD rr = ((p >> 16) & 255) * a / 255;
        DWORD gg = ((p >> 8)  & 255) * a / 255;
        DWORD bb = ( p        & 255) * a / 255;
        g_pgBuf[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
    SIZE  boy = { PG_W, PG_H };
    POINT sifir = { 0, 0 };
    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(h, nullptr, nullptr, &boy, g_pgDC, &sifir, 0, &bf, ULW_ALPHA);
}

static void PgTikla(HWND h, int id)
{
    if (id >= 0 && id < PANEL_N)
    {
        int v = PanelOku(id) ? 0 : 1;
        PanelYaz(id, v);
        Log("[PANEL] %s = %d", g_panelSec[id].ad, v);
    }
    else if (id == PG_ID_ORJ)
    {
        for (int i = 0; i < PANEL_N; ++i) PanelYaz(i, 0);
        Log("[PANEL] hepsi KAPALI (orijinal goruntu)");
    }
    else if (id == PG_ID_ILK)
    {
        // Panelin ilk acildigi andaki degerlere don.
        for (int i = 0; i < PANEL_N; ++i) PanelYaz(i, g_panelIlk[i]);
        Log("[PANEL] baslangic ayarlarina donuldu");
    }
    else if (id == PG_ID_KAPAT)
        ShowWindow(h, SW_HIDE);
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;                 // tiklama oyunun odagini almasin
    case WM_LBUTTONDOWN:
    {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int id = PgVurus(x, y);
        if (id < 0)
        {
            // Baslik cubugundan tutup surukleme
            if (y >= 7 && y < 32)
            {
                ReleaseCapture();
                SendMessageA(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        g_pgBasili = id;
        SetCapture(h);
        PgSun(h);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int basili = g_pgBasili;
        g_pgBasili = -1;
        ReleaseCapture();
        if (basili >= 0 && PgVurus(x, y) == basili) PgTikla(h, basili);
        if (IsWindowVisible(h)) PgSun(h);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        // Uzerine gelinen dugme parlasin (oyundaki gibi).
        int id = PgVurus((short)LOWORD(l), (short)HIWORD(l));
        if (id != g_pgUzerinde)
        {
            g_pgUzerinde = id;
            PgSun(h);
        }
        TRACKMOUSEEVENT tm = { sizeof(tm), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tm);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_pgUzerinde != -1) { g_pgUzerinde = -1; if (IsWindowVisible(h)) PgSun(h); }
        return 0;
    case WM_TIMER:
        // Ayar baska yerden degistiyse panel de guncellensin.
        if (IsWindowVisible(h) && PgDurum() != g_pgSonDurum) PgSun(h);
        return 0;
    case PANEL_MSG_GOSTER:
        if (IsWindowVisible(h))
            ShowWindow(h, SW_HIDE);
        else
        {
            // Oyun penceresinin sol ustune yerles.
            RECT r;
            HWND oyun = (HWND)w;
            if (oyun && oyun != h && GetWindowRect(oyun, &r))
                SetWindowPos(h, HWND_TOPMOST, r.left + 16, r.top + 90, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
            PgSun(h);
            ShowWindow(h, SW_SHOWNOACTIVATE);
            Log("[PANEL] gosterildi");
            LogFlush();
        }
        return 0;
    case PANEL_MSG_KAPAT:
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI PanelThread(LPVOID)
{
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = PanelProc;
    wc.hInstance     = inst;
    wc.lpszClassName = "GmtGorselPanel";
    wc.hCursor       = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    RegisterClassA(&wc);

    const DWORD exStil = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    HWND wnd = CreateWindowExA(exStil, "GmtGorselPanel", "Gorsel mod",
        WS_POPUP, 60, 120, PG_W, PG_H, nullptr, nullptr, inst, nullptr);
    if (!wnd || !PgHazirla())
    {
        Log("[PANEL] pencere OLUSTURULAMADI hata=%u", GetLastError());
        LogFlush();
        return 0;
    }

    // "Baslangica don" icin o anki (henuz dokunulmamis) degerleri sakla.
    for (int i = 0; i < PANEL_N && i < 16; ++i) g_panelIlk[i] = PanelOku(i);

    SetTimer(wnd, 1, 300, nullptr);
    g_panelWnd = wnd;

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_panelWnd = nullptr;
    return 0;
}
// <<< PANEL KOD BITIS

// F9: paneli ac / kapa. Pencere ilk basista olusturulur.
static void PanelAcKapa()
{
    HWND oyun = GetForegroundWindow();
    if (!g_panelThread)
    {
        g_panelThread = CreateThread(nullptr, 0, PanelThread, nullptr, 0, nullptr);
        for (int i = 0; i < 100 && !g_panelWnd; ++i) Sleep(10);
    }
    if (g_panelWnd)
        PostMessageA(g_panelWnd, PANEL_MSG_GOSTER, (WPARAM)oyun, 0);
    else
        Log("[PANEL] pencere 1 saniyede hazir olmadi (thread=%p)", g_panelThread);
}

static void GuncelKur();   // asagida, hook tablosundan sonra
static void ZirhUstKur();

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool prev9 = false, prev10 = false;
    while (!g_shutdown)
    {
        Sleep(120);
        if (g_shutdown) break;

        // Tamponu duzenli bosalt: oyun acikken log guncel kalsin.
        // Yazma artik burada, render thread'inde degil.
        LogFlush();

        // Ikinci ornek: 4 saniye once goruldugu yerde ne degismedi?
        PktRecheck();

        // Guncelleme fonksiyonunun girisi bulunduysa hook'unu kur.
        GuncelKur();
        ZirhUstKur();

        if (!GameHasFocus()) { prev9 = prev10 = false; continue; }

        // PANEL TUSU F9. F10 artik KULLANILMIYOR: Windows'ta F10 pencerenin
        // sistem menusunu acar; oyun "menu modu"na girip bir sonraki
        // fare/klavye hareketine kadar donuyordu (F10'daki donmanin sebebi).
        // Config'i yeniden okuma artik paneldeki "Config'e don" dugmesinde.
        //
        // 0x8000 = su an basili, 0x0001 = son sorgudan beri basildi. Ikincisi
        // olmadan 120 ms'den kisa dokunuslar hic gorulmuyordu.
        SHORT s9  = GetAsyncKeyState(VK_F9);
        bool now9 = (s9 & 0x8001) != 0;
        (void)prev10;

        if (now9 && !prev9)
        {
            Log("[F9] tus algilandi, panel ac/kapa");
            LogFlush();
            PanelAcKapa();
            LogFlush();
        }
        prev9 = now9;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Binek ogrenme
// ---------------------------------------------------------------------------
// Sadece mod_mounts.txt'den YUKLENENLER aktif.
// Bu oturumda ogrenilenler dosyaya yazilir ama devreye girmez:
// .msm istegi motlist.txt'den ONCE geliyor, yani ayni oturumda
// devreye girsek govde orijinal kalip animasyon/mesh degisir ve
// binek bozulur. Bir sonraki aciliste tam ve tutarli donusuyor.
// Bu oturumda npc_mount/ altinda gorulen klasorler + config'teki
// "binekklasor = <ad>" satirlari. Model bazen duz npc/<ad>/ altindan
// geliyor (kaplan: npc/fire_tiger/fire_tiger_white.msm), onu yakalamak icin.
static char g_mfold[64][NAME_LEN];
static int  g_mfoldN = 0;
static bool MFoldHas(const char* f)
{
    for (int i = 0; i < g_mfoldN; ++i) if (_stricmp(g_mfold[i], f) == 0) return true;
    return false;
}
static void MFoldAdd(const char* f)
{
    if (!*f || strlen(f) >= NAME_LEN || MFoldHas(f)) return;
    // Hedef at klasorlerini asla ekleme (dongu olur)
    if (!_stricmp(f,"horse") || !_stricmp(f,"horse2") || !_stricmp(f,"pony")) return;
    // Binek OLMAYAN klasorler (asagida BinekDegil)
    if (!_stricmp(f,"facility") || !_stricmp(f,"guild")) return;
    EnterCriticalSection(&g_lock);
    if (g_mfoldN < 64) strcpy_s(g_mfold[g_mfoldN++], f);
    LeaveCriticalSection(&g_lock);
}

static bool IsActiveMount(const char* folder)
{
    for (int i = 0; i < g_mountCount; ++i)
        if (g_mount[i].active && strcmp(g_mount[i].name, folder) == 0)
            return true;
    return false;
}

static void LearnMount(const char* folder)
{
    if (!*folder || strlen(folder) >= NAME_LEN) return;

    EnterCriticalSection(&g_lock);
    bool exists = false;
    for (int i = 0; i < g_mountCount; ++i)
        if (strcmp(g_mount[i].name, folder) == 0) { exists = true; break; }

    if (!exists && g_mountCount < MAX_MOUNTS)
    {
        strcpy_s(g_mount[g_mountCount].name, folder);
        g_mount[g_mountCount].active = false;   // bu oturumda pasif
        ++g_mountCount;

        char path[MAX_PATH];
        sprintf_s(path, "%smod_mounts.txt", g_gameDir);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") == 0 && f)
        {
            fprintf(f, "%s\n", folder);
            fclose(f);
        }
        LeaveCriticalSection(&g_lock);
        Log("  [OGREN] binek: %s  (bir sonraki aciliste devreye girecek)", folder);
        return;
    }
    LeaveCriticalSection(&g_lock);
}


// ---------------------------------------------------------------------------
// OGRENILEN ZIRH MODELLERI  (vnum -> .gr2 adi)
// ---------------------------------------------------------------------------
// Zirh dosya adlari tutarsiz: assassin_4-1 (tire) ama sura_4_1 (alt
// tire), warrior_6_1/8_1 gibi listede olmayanlar var. Config'teki
// tahmine yonlendirince olmayan dosya isteniyor ve karakter GORUNMEZ
// oluyor. O yuzden adi oyunun kendisinden ogreniyoruz: zirh setter
// vnum'u soyluyor, hemen ardindan gelen istek dosya adini soyluyor.
// Ogrenilenler mod_zirhmodel.txt'ye yazilir, sonraki aciliste hazir.
struct ZMod { int vnum; char gr2[NAME_LEN]; };
static ZMod g_zmod[256];
static int  g_zmodN = 0;

static const char* ZModBul(int vnum)
{
    for (int i = 0; i < g_zmodN; ++i)
        if (g_zmod[i].vnum == vnum) return g_zmod[i].gr2;
    return nullptr;
}

// "sura_4_1_lod_02" -> "sura_4_1"
static void ZModSade(const char* in, char* out, size_t n)
{
    strcpy_s(out, n, in);
    char* l = strstr(out, "_lod_");
    if (l) *l = '\0';
}

// "warrior_4-1" / "sura_5_1" gibi KADEME modeli mi?
// Yansitma ve kostum modelleri (warrior_easter_day_2026,
// sura_jinhon) bu kaliba uymuyor; onlari ogrenmiyoruz, yoksa
// 1. seviye zirhin modeli diye paskalya kostumu kaydediliyor.
static bool ZModKademe(const char* ad, const char* cls)
{
    size_t cl = strlen(cls);
    if (_strnicmp(ad, cls, cl) != 0 || ad[cl] != '_') return false;
    const char* p = ad + cl + 1;
    if (!IsDigit(p[0])) return false;
    ++p;
    if (IsDigit(p[0])) ++p;          // iki haneli kademe olabilir
    if (*p != '-' && *p != '_') return false;
    ++p;
    if (!IsDigit(*p)) return false;
    ++p;
    return *p == '\0';
}

static void ZModOgren(int vnum, const char* model, const char* cls)
{
    char ad[NAME_LEN];
    ZModSade(model, ad, sizeof(ad));
    if (!ad[0] || strlen(ad) >= NAME_LEN) return;
    if (!ZModKademe(ad, cls)) return;

    EnterCriticalSection(&g_lock);
    bool var = false;
    for (int i = 0; i < g_zmodN; ++i)
        if (g_zmod[i].vnum == vnum) { var = true; break; }

    if (!var && g_zmodN < 256)
    {
        g_zmod[g_zmodN].vnum = vnum;
        strcpy_s(g_zmod[g_zmodN].gr2, ad);
        ++g_zmodN;

        char path[MAX_PATH];
        sprintf_s(path, "%smod_zirhmodel.txt", g_gameDir);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") == 0 && f)
        {
            fprintf(f, "%d %s\n", vnum, ad);
            fclose(f);
        }
        Log("  [ogren] zirh %d -> %s", vnum, ad);
    }
    LeaveCriticalSection(&g_lock);
}

static void ZModYukle()
{
    char path[MAX_PATH];
    sprintf_s(path, "%smod_zirhmodel.txt", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;

    char line[128];
    while (fgets(line, sizeof(line), f) && g_zmodN < 256)
    {
        int v = 0; char ad[NAME_LEN] = {};
        if (sscanf_s(line, "%d %63s", &v, ad, (unsigned)sizeof(ad)) != 2) continue;
        if (v <= 0 || !ad[0] || ZModBul(v)) continue;
        g_zmod[g_zmodN].vnum = v;
        strcpy_s(g_zmod[g_zmodN].gr2, ad);
        ++g_zmodN;
    }
    fclose(f);
    Log("mod_zirhmodel.txt: %d zirh modeli ogrenilmis", g_zmodN);
}


// ---------------------------------------------------------------------------
// ZIRH KADEMELERI  (sinif + kademe -> gercek dosya adi)
// ---------------------------------------------------------------------------
// vnum -> model eslemesi YANSITMA yuzunden guvenilmez: sunucu gercek
// zirhin vnum'unu soyluyor ama istemci yansitilan zirhin modelini
// istiyor, ikisi farkli. O yuzden vnum'u tamamen birakip dosya adinin
// kendisine bakiyoruz: "warrior_8_1" -> sinif warrior, kademe 8.
// Kademeyi oyuncunun seviyesine gore asagi cekiyoruz ve ayni sinifin
// DAHA ONCE GERCEKTEN ISTENMIS bir modeline yonlendiriyoruz; boylece
// olmayan dosya adi uretip karakteri gorunmez yapmiyoruz.
struct KMod { char cls[20]; int kademe; int alt; char ad[NAME_LEN]; };
static KMod g_kmod[256];
static int  g_kmodN = 0;

// "warrior_8_1" -> cls=warrior kademe=8 alt=1
static bool KModAyir(const char* ad, char* cls, size_t clsN, int* kademe, int* alt)
{
    const char* u = strchr(ad, '_');
    if (!u) return false;
    size_t cl = (size_t)(u - ad);
    if (cl == 0 || cl >= clsN) return false;

    const char* p = u + 1;
    if (!IsDigit(*p)) return false;
    int k = 0;
    while (IsDigit(*p)) k = k * 10 + (*p++ - '0');
    if (*p != '-' && *p != '_') return false;
    ++p;
    if (!IsDigit(*p)) return false;
    int a = 0;
    while (IsDigit(*p)) a = a * 10 + (*p++ - '0');
    if (*p != '\0') return false;

    memcpy(cls, ad, cl); cls[cl] = '\0';
    *kademe = k; *alt = a;
    return true;
}

static void KModOgren(const char* ad)
{
    char cls[20]; int k = 0, a = 0;
    char sade[NAME_LEN];
    ZModSade(ad, sade, sizeof(sade));
    if (!KModAyir(sade, cls, sizeof(cls), &k, &a)) return;
    if (k < 1 || k > 20) return;

    EnterCriticalSection(&g_lock);
    bool var = false;
    for (int i = 0; i < g_kmodN; ++i)
        if (_stricmp(g_kmod[i].ad, sade) == 0) { var = true; break; }

    if (!var && g_kmodN < 256)
    {
        strcpy_s(g_kmod[g_kmodN].cls, cls);
        g_kmod[g_kmodN].kademe = k;
        g_kmod[g_kmodN].alt    = a;
        strcpy_s(g_kmod[g_kmodN].ad, sade);
        ++g_kmodN;

        char path[MAX_PATH];
        sprintf_s(path, "%smod_zirhmodel.txt", g_gameDir);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") == 0 && f) { fprintf(f, "%s\n", sade); fclose(f); }
        Log("  [ogren] zirh modeli %s (sinif %s, kademe %d)", sade, cls, k);
    }
    LeaveCriticalSection(&g_lock);
}

static void KModYukle()
{
    char path[MAX_PATH];
    sprintf_s(path, "%smod_zirhmodel.txt", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof(line), f) && g_kmodN < 256)
    {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!line[0] || line[0] == '#') continue;
        // Eski bicim ("11290 warrior_4-1") ile uyum: adi al
        char* sp = strrchr(line, ' ');
        const char* ad = sp ? sp + 1 : line;
        char cls[20]; int k = 0, a = 0;
        if (!KModAyir(ad, cls, sizeof(cls), &k, &a)) continue;
        bool var = false;
        for (int i = 0; i < g_kmodN; ++i)
            if (_stricmp(g_kmod[i].ad, ad) == 0) { var = true; break; }
        if (var) continue;
        strcpy_s(g_kmod[g_kmodN].cls, cls);
        g_kmod[g_kmodN].kademe = k;
        g_kmod[g_kmodN].alt    = a;
        strcpy_s(g_kmod[g_kmodN].ad, ad);
        ++g_kmodN;
    }
    fclose(f);
    Log("mod_zirhmodel.txt: %d zirh modeli bilinyor", g_kmodN);
}

// Config'teki ad ile oyunun istedigi ad bazen sadece ayiracta ayriliyor
// (warrior_4_1 <-> warrior_4-1). Ogrenilen adlar arasinda ayni yazimi
// ariyoruz; bulursak oyunun yazimini kullaniyoruz.
static const char* KModDuzelt(const char* ad)
{
    for (int i = 0; i < g_kmodN; ++i)
        if (_stricmp(g_kmod[i].ad, ad) == 0) return g_kmod[i].ad;

    for (int i = 0; i < g_kmodN; ++i)
    {
        const char* a = g_kmod[i].ad;
        if (strlen(a) != strlen(ad)) continue;
        bool ayni = true;
        for (const char* x = a, *y = ad; *x && ayni; ++x, ++y)
        {
            bool ayrac = (*x == '-' || *x == '_') && (*y == '-' || *y == '_');
            if (!ayrac && tolower(*x) != tolower(*y)) ayni = false;
        }
        if (ayni) return a;
    }
    return ad;
}

// Seviye -> izin verilen en yuksek kademe. config: kademesv = <kademe> <seviye>
static int KademeIcin(int seviye)
{
    int best = 1;
    for (int k = 1; k <= 20; ++k)
    {
        if (g_kademeSv[k] <= 0) continue;
        if (g_kademeSv[k] <= seviye && k > best) best = k;
    }
    return best;
}

// Ayni sinifta, kademesi sinirin altinda kalan EN YUKSEK bilinen model.
// Once ayni alt numarayi (zirhin rengi) tutturmaya calisiyoruz.
static const char* KModHedef(const char* cls, int enfazla, int alt)
{
    const char* bulunanAlt = nullptr; int bk1 = 0;
    const char* bulunan    = nullptr; int bk2 = 0;
    for (int i = 0; i < g_kmodN; ++i)
    {
        if (!SinifAyni(g_kmod[i].cls, cls)) continue;
        if (g_kmod[i].kademe > enfazla) continue;
        if (g_kmod[i].alt == alt && g_kmod[i].kademe > bk1)
        { bk1 = g_kmod[i].kademe; bulunanAlt = g_kmod[i].ad; }
        if (g_kmod[i].kademe > bk2)
        { bk2 = g_kmod[i].kademe; bulunan = g_kmod[i].ad; }
    }
    return bulunanAlt ? bulunanAlt : bulunan;
}


static void LoadMounts()
{
    char path[MAX_PATH];
    sprintf_s(path, "%smod_mounts.txt", g_gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;

    char line[NAME_LEN];
    while (fgets(line, sizeof(line), f) && g_mountCount < MAX_MOUNTS)
    {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!*line) continue;
        ToLowerStr(line);
        if (IsActiveMount(line)) continue;
        strcpy_s(g_mount[g_mountCount].name, line);
        g_mount[g_mountCount].active = true;    // dosyadan gelen aktif
        ++g_mountCount;
    }
    fclose(f);
    Log("mod_mounts.txt: %d binek aktif", g_mountCount);
}

// Klasor yolu kur: <hedefKlasor>/<cevrilmis ad><uzanti>
static bool BuildDirPath(const char* targetDir, const char* norm,
                         const char* slash, char* out)
{
    const char* fileStart = slash + 1;
    const char* dot = strrchr(fileStart, '.');
    if (!dot || dot == fileStart) return false;

    size_t stemLen = (size_t)(dot - fileStart);
    if (stemLen >= NAME_LEN) return false;

    char stem[NAME_LEN];
    memcpy(stem, fileStart, stemLen);
    stem[stemLen] = '\0';

    const char* dirStart = slash;
    while (dirStart > norm && *(dirStart - 1) != '/') --dirStart;
    size_t dirLen = (size_t)(slash - dirStart);
    bool isMesh = (dirLen > 0 && stemLen >= dirLen &&
                   strncmp(stem, dirStart, dirLen) == 0);

    const char* newStem = nullptr;
    for (int j = 0; j < g_animCount; ++j)
        if (strcmp(g_anim[j].from, stem) == 0) { newStem = g_anim[j].to; break; }

    if (!newStem)
    {
        if (_stricmp(dot, ".msm") == 0 && g_msmDefault[0])
            newStem = g_msmDefault;
        else if (isMesh && g_animDefault[0])
            newStem = g_animDefault;
    }
    if (!newStem) newStem = stem;

    if (strlen(targetDir) + strlen(newStem) + strlen(dot) + 1 >= PATH_LEN)
        return false;

    strcpy_s(out, PATH_LEN, targetDir);
    strcat_s(out, PATH_LEN, newStem);
    strcat_s(out, PATH_LEN, dot);
    return true;
}

static const char* ResolveFileNameIc(const char* original)
{
    // "Klasik gorunum" kapaliyken dosya kurallari (gizleme, sac, zirh, kirpma)
    // atlanir; ama yukleme ekrani ve "Binekleri ata cevir" bagimsiz calisir.
    const bool klasik = (g_enabled != 0);

    if (!g_ready || !original) return original;
    if (!IsReadable(original, 1)) return original;

    char norm[PATH_LEN];
    NormalizeInto(original, norm, sizeof(norm));

    // --- YUKLEME EKRANI ---
    // Oyun her yuklemede shared\common\ui\loading\loadingN.sub ister; o .sub
    // gosterilecek .jpg'yi adiyla soyler. Oyun klasorunde mod_yukleme\
    // varsa, icindeki .sub'lardan rastgele biri verilir; o .sub'un andigi
    // background_loading_*.jpg de ayni klasorden gelir. (DENEYSEL: istemcinin
    // paket disindaki dosyayi okuyup okumadigi bilinmiyor.)
    {
        const char* y = strstr(norm, "shared/common/ui/loading/");
        if (y)
        {
            static char adlar[32][64];
            static int  adN = -1;
            if (adN < 0)
            {
                adN = 0;
                char desen[MAX_PATH];
                sprintf_s(desen, "%smod_yukleme\\*.sub", g_gameDir);
                WIN32_FIND_DATAA fd;
                HANDLE fh = FindFirstFileA(desen, &fd);
                if (fh != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (adN < 32 && strlen(fd.cFileName) < 64)
                            strcpy_s(adlar[adN++], fd.cFileName);
                    } while (FindNextFileA(fh, &fd));
                    FindClose(fh);
                }
                Log("[YUKLEME] mod_yukleme klasorunde %d gorsel", adN);
            }
            const char* f = y + 25;
            if (adN > 0 && !strchr(f, '/'))
            {
                size_t fl = strlen(f);
                if (strncmp(f, "loading", 7) == 0 && IsDigit(f[7]) &&
                    fl > 4 && strcmp(f + fl - 4, ".sub") == 0)
                {
                    int sec = (int)(GetTickCount() / 7u) % adN;
                    _snprintf_s(t_buffer, PATH_LEN, _TRUNCATE, "%smod_yukleme\\%s",
                                g_gameDir, adlar[sec]);
                    Log("  [YUKLEME] %s  ==>  %s", original, t_buffer);
                    return t_buffer;
                }
                if (strncmp(f, "background_loading_", 19) == 0)
                {
                    _snprintf_s(t_buffer, PATH_LEN, _TRUNCATE, "%smod_yukleme\\%s",
                                g_gameDir, f);
                    Log("  [YUKLEME] %s  ==>  %s", original, t_buffer);
                    return t_buffer;
                }
            }
            Log("  [YUKLEME] %s", original);
        }
    }

    // --- GIRIS / SUNUCU SECIM ARKA PLANI ---
    // Oyun klasorunde mod_giris\login.jpg varsa, sunucu secim / giris
    // ekraninin arka plani (serverlist.jpg ya da intro/login/login.jpg) o olur.
    {
        if (strstr(norm, "ui/serverlist") || strstr(norm, "ui/login/login.jpg") ||
            strstr(norm, "intro/login/login.jpg"))
        {
            char hedef[MAX_PATH];
            sprintf_s(hedef, "%smod_giris\\login.jpg", g_gameDir);
            const bool var = (GetFileAttributesA(hedef) != INVALID_FILE_ATTRIBUTES);
            static LONG s_girisLog = 0;
            if (InterlockedIncrement(&s_girisLog) <= 8)
                Log("[GIRIS] %s  %s", original, var ? "==> mod_giris\\login.jpg" : "(mod_giris\\login.jpg yok)");
            if (var && strstr(norm, ".jpg"))
            {
                _snprintf_s(t_buffer, PATH_LEN, _TRUNCATE, "%s", hedef);
                return t_buffer;
            }
        }
    }

    // --- Probe: eslesen yollari bir kez logla (vnum kesfi icin) ---
    if (g_probeCount > 0)
    {
        for (int i = 0; i < g_probeCount; ++i)
        {
            if (!PatternMatches(norm, g_probe[i])) continue;
            // DIKKAT: SeenBefore cagirmiyoruz; cagirsaydik yol "gorulmus"
            // isaretlenir ve asil kural (redirect/gizle) loga dusmezdi.
            if (InterlockedIncrement(&g_probeHits) <= 400)
                Log("  [PROBE] %s", original);
            break;
        }
    }

    // --- Nesne avi tetigi ---
    // Zirh yuklenirken yoneticinin "secili karakter" alani gecerli
    // olmali; dokumu tam o anda aliyoruz.

    // Sac istegi de bir olay: bitisiklik olcusunu bozmasin diye
    // sirayi ilerletiyoruz.
    if (strstr(norm, "/hair/")) InterlockedIncrement(&g_seq);

    // --- Son gorulen karakter sinifini hatirla ---
    // "/pc/warrior/..." -> klasor adi DOGRUDAN /pc/'den sonra gelir.
    // Onceki surumde bir seviye derine inip "hair" / "effect"
    // okuyordum, o yuzden sinif kontrolu hic calismadi.
    {
        const char* cs = nullptr;
        const char* p2 = strstr(norm, "/pc/");
        if (p2) cs = p2 + 4;
        else if ((p2 = strstr(norm, "/pc2/")) != nullptr) cs = p2 + 5;
        else if ((p2 = strstr(norm, "/pc3/")) != nullptr) cs = p2 + 5;

        if (cs)
        {
            const char* ce = strchr(cs, '/');
            if (ce && (size_t)(ce - cs) < NAME_LEN)
            {
                char cand[NAME_LEN];
                size_t n = (size_t)(ce - cs);
                memcpy(cand, cs, n);
                cand[n] = '\0';
                // Sadece gercek sinif adlarini kabul et; pc/ altinda
                // "effect", "hair" gibi klasorler de var.
                const int fc = (int)(sizeof(k_classFams) / sizeof(k_classFams[0]));
                for (int i = 0; i < fc; ++i)
                    if (strcmp(k_classFams[i].name, cand) == 0)
                    {
                        strcpy_s(g_lastClass, cand);
                        g_classSeq = InterlockedIncrement(&g_seq);
                        break;
                    }
            }
        }
    }

    // --- Son gercek silahi hatirla (kostum sinifi icin SART) ---
    {
        // vnumdegis acikken KAPALI. Silahta kisi bazli kanal calisiyor:
        // kurucuda vnum'u degistirince her karakter KENDI dosyasini
        // istiyor, dolayisiyla onbellek cakismasi olmuyor. Dosya
        // kirpmasi ise oyuncudan bagimsiz calismak zorunda oldugu icin
        // herkese tavan silahini dayatiyordu (1. seviyeye Triton).
        const char* w = strstr(norm, "/item/weapon/");
        if (w)
        {
            const char* f = w + 13;
            if (!strchr(f, '/'))
            {
                // item/weapon/<numara>.gr2  -> son silah olarak sakla
                size_t n = 0;
                while (n < 15 && IsDigit(f[n])) ++n;
                if (n >= 4 && f[n] == '.')
                {
                    memcpy(g_lastWeapon, f, n);
                    g_lastWeapon[n] = '\0';
                    g_weaponSeq = InterlockedIncrement(&g_seq);
                }
            }
            else if (g_probeCount > 0 && _strnicmp(f, "costume/", 8) == 0 &&
                     g_lastWeapon[0])
            {
                if (InterlockedIncrement(&g_pairHits) <= 120)
                    Log("  [CIFT] kostum %s  <-  gercek silah %s",
                        f + 8, g_lastWeapon);
            }
        }
    }

    // --- 0. Yonlendirme (redirect) ---
    // Desen bosluklarla ayrilmis birden fazla parca icerebilir;
    // hepsi yolda geciyorsa eslesme sayilir.
    for (int i = 0; klasik && i < g_redirectCount; ++i)
    {
        if (!PatternMatches(norm, g_redirect[i].pattern)) continue;

        // Uzanti filtresi: hedefin uzantisiyla ayni mi?
        if (g_redirect[i].ext[0])
        {
            const char* dot = strrchr(norm, '.');
            if (!dot || _stricmp(dot, g_redirect[i].ext) != 0) continue;
        }

        strcpy_s(t_buffer, g_redirect[i].target);
        if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
            Log("  >> [yonlen] %s  ==>  %s", original, t_buffer);
        return t_buffer;
    }

    // --- 0b. Klasor yonlendirmesi (dir) ---
    for (int i = 0; klasik && i < g_dirCount; ++i)
    {
        if (!PatternMatches(norm, g_dir[i].pattern)) continue;
        const char* slash = strrchr(norm, '/');
        if (!slash) break;
        if (!BuildDirPath(g_dir[i].target, norm, slash, t_buffer)) break;

        if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
            Log("  >> [klasor] %s  ==>  %s", original, t_buffer);
        return t_buffer;
    }

    // --- 0c. Ogrenilen binekler ---
    // Client npc_mount/<ad>/motlist.txt isterse o klasor gercek
    // binek demektir; pet'lerde o istek hic gelmez. Ilk goruste
    // ogreniyoruz, sonraki her goruste ata ceviriyoruz.
    if ((g_mountLearn || g_mountAll) && !strstr(norm, "sound/"))
    {
        // Ayni yaratik iki yoldan yukleniyor: animasyonlari
        // npc_mount/, MODELI npc_pet/ altindan. Simdiye kadar sadece
        // npc_mount/ yonlendiriliyordu, bu yuzden binek at gibi
        // yuruyup kaplan gibi gorunuyordu. Artik ikisi de.
        // npc_pet/ tarafinda ASLA ogrenmiyoruz ve mountall'i
        // uygulamiyoruz; sadece daha once binek oldugu DOGRULANMIS
        // klasorlere dokunuyoruz, yoksa butun petler ata donerdi.
        bool petYolu = false;
        const char* m = strstr(norm, "npc_mount/");
        if (!m)
        {
            m = strstr(norm, "npc_pet/");
            if (m) { petYolu = true; }
        }
        if (m)
        {
            const char* fs = m + (petYolu ? 8 : 10);
            const char* sl = strchr(fs, '/');
            size_t flen = sl ? (size_t)(sl - fs) : 0;
            if (sl && flen > 0 && flen < NAME_LEN)
            {
                char folder[NAME_LEN];
                memcpy(folder, fs, flen);
                folder[flen] = '\0';

                // LONCA YAPILARI BINEK DEGIL: oyun lonca ocagi, tapinagi,
                // bankasi gibi yapilari npc_mount\facility\ ve npc\facility\
                // altindan yukluyor. "Butun binekleri ata cevir" bunlari da
                // ata ceviriyordu (Cennetin Gozu ocagi, Cagri tapinagi).
                bool binekDegil = !_stricmp(folder, "facility") ||
                                  !_stricmp(folder, "guild");

                if (!petYolu && !binekDegil)
                {
                    MFoldAdd(folder);
                    // Kalici kayit: pet yolundan gelen GOVDE modeli bir
                    // sonraki oturumda ilk karede yakalansin diye.
                    if (_stricmp(sl + 1, "motlist.txt") == 0) LearnMount(folder);
                }

                // mountall aciksa ogrenmeye gerek yok, dosya da yazmayiz
                if (!petYolu && !g_mountAll && !binekDegil &&
                    _stricmp(sl + 1, "motlist.txt") == 0)
                    LearnMount(folder);

                if (!binekDegil &&
                    (petYolu ? (IsActiveMount(folder) || MFoldHas(folder))
                             : (g_mountAll || IsActiveMount(folder) || MFoldHas(folder))))
                {
                    const char* dot = strrchr(sl + 1, '.');
                    if (dot && _stricmp(dot, ".msm") == 0)
                    {
                        strcpy_s(t_buffer, g_mountMsm);
                        // Hangi model ata cevrildi? (binek olmayan bir sey
                        // cevriliyorsa buradan gorulur; model basina bir satir)
                        Log("  [BINEK] %s  ==>  at", original);
                        if (!SeenBefore(norm) &&
                            InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                            Log("  >> [binek]  %s  ==>  %s", original, t_buffer);
                        return t_buffer;
                    }
                    if (BuildDirPath(g_mountDir, norm, sl, t_buffer))
                    {
                        if (!SeenBefore(norm) &&
                            InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                            Log("  >> [binek]  %s  ==>  %s", original, t_buffer);
                        return t_buffer;
                    }
                }
            }
        }
    }

    // --- 0d. Modeli duz npc/<ad>/ altindan gelen binekler (kaplan) ---
    if ((g_mountLearn || g_mountAll) && g_mfoldN > 0)
    {
        const char* m = strstr(norm, "/npc/");
        if (m)
        {
            const char* fs = m + 5;
            const char* sl = strchr(fs, '/');
            size_t flen = sl ? (size_t)(sl - fs) : 0;
            if (sl && flen > 0 && flen < NAME_LEN)
            {
                char folder[NAME_LEN];
                memcpy(folder, fs, flen);
                folder[flen] = '\0';
                if (_stricmp(folder, "facility") != 0 && _stricmp(folder, "guild") != 0 &&
                    (MFoldHas(folder) || IsActiveMount(folder)))
                {
                    // Govde .msm'i ata cevir; .gr2 mesh'leri ve .msa
                    // animasyonlari da at klasorunden gelsin. Sadece
                    // .msm cevrilirse iskelet at, mesh yaratik kalir
                    // ve model donuk/yatik gorunur.
                    const char* dot = strrchr(sl + 1, '.');
                    bool tamam = false;
                    if (dot && _stricmp(dot, ".msm") == 0)
                    {
                        strcpy_s(t_buffer, g_mountMsm);
                        tamam = true;
                    }
                    else
                    {
                        tamam = BuildDirPath(g_mountDir, norm, sl, t_buffer);
                    }
                    if (tamam)
                    {
                        if (dot && _stricmp(dot, ".msm") == 0)
                            Log("  [BINEK] %s  ==>  at (npc yolu)", original);
                        if (!SeenBefore(norm) &&
                            InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                            Log("  >> [binek-npc]  %s  ==>  %s", original, t_buffer);
                        return t_buffer;
                    }
                }
            }
        }
    }

    // Buradan sonrasi "Klasik gorunum" kurallari.
    if (!klasik) return original;

    // --- 1. Gizleme ---
    if (ShouldHide(norm))
    {
        MakeHiddenPath(norm, t_buffer, PATH_LEN);
        if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
            Log("  >> [gizle]  %s", original);
        return t_buffer;
    }

    // --- 2. Acik esleme (tam yol) ---
    for (int i = 0; i < g_mapCount; ++i)
    {
        if (strcmp(g_map[i].from, norm) == 0)
        {
            strcpy_s(t_buffer, g_map[i].to);
            if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                Log("  >> [acik]   %s  ==>  %s", original, t_buffer);
            return t_buffer;
        }
    }

    // --- Sac / kafa kostumu:  /pc*/<sinif>/hair/hair_<n>_<n>.gr2 ---
    if (g_maxHair >= 0 && g_defaultHair[0])
    {
        const char* h = strstr(norm, "/hair/hair_");
        if (h)
        {
            const char* numStart = h + 11;        // "/hair/hair_" = 11
            if (IsDigit(*numStart))
            {
                int hairNum = atoi(numStart);
                if (hairNum > g_maxHair)
                {
                    int prefixLen = (int)(h + 6 - norm);   // "/hair/" dahil
                    if (prefixLen > 0 &&
                        prefixLen + strlen(g_defaultHair) + 5 < PATH_LEN)
                    {
                        memcpy(t_buffer, original, prefixLen);
                        t_buffer[prefixLen] = '\0';
                        strcat_s(t_buffer, PATH_LEN, g_defaultHair);
                        strcat_s(t_buffer, PATH_LEN, ".gr2");

                        if (!SeenBefore(norm) &&
                            InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                            Log("  >> [sac %d>%d] %s  ==>  %s",
                                hairNum, g_maxHair, original, t_buffer);

                        return t_buffer;
                    }
                }
            }
        }
    }

    // --- Genel dosya adi eslemesi (her yolda calisir) ---
    if (g_fileMapCount > 0)
    {
        const char* slash = strrchr(norm, '/');
        if (slash)
        {
            const char* fileStart = slash + 1;
            const char* dot = strrchr(fileStart, '.');
            if (dot && dot > fileStart)
            {
                size_t baseLen = (size_t)(dot - fileStart);
                if (baseLen < NAME_LEN)
                {
                    char base[NAME_LEN];
                    memcpy(base, fileStart, baseLen);
                    base[baseLen] = '\0';

                    // LOD ekini ayir
                    char lod[16] = {};
                    if (baseLen > 7)
                    {
                        char* tail = base + baseLen - 7;
                        if (strncmp(tail, "_lod_", 5) == 0 &&
                            IsDigit(tail[5]) && IsDigit(tail[6]))
                        {
                            strcpy_s(lod, tail);
                            *tail = '\0';
                        }
                    }

                    for (int i = 0; i < g_fileMapCount; ++i)
                    {
                        if (strcmp(g_fileMap[i].from, base) != 0) continue;

                        // Hedefte '/' varsa TAM YOL kabul edilir.
                        // Boylece kostum klasorunden normal silah
                        // klasorune cikabiliyoruz.
                        if (strchr(g_fileMap[i].to, '/'))
                        {
                            if (strlen(g_fileMap[i].to) + 1 >= PATH_LEN) continue;
                            strcpy_s(t_buffer, g_fileMap[i].to);
                            if (!SeenBefore(norm) &&
                                InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                                Log("  >> [kostum] %s  ==>  %s", original, t_buffer);
                            return t_buffer;
                        }

                        int prefixLen = (int)(fileStart - norm);
                        if (prefixLen > 0 &&
                            prefixLen + strlen(g_fileMap[i].to) + strlen(lod)
                            + strlen(dot) + 1 < PATH_LEN)
                        {
                            memcpy(t_buffer, original, prefixLen);
                            t_buffer[prefixLen] = '\0';
                            strcat_s(t_buffer, PATH_LEN, g_fileMap[i].to);
                            strcat_s(t_buffer, PATH_LEN, lod);
                            strcat_s(t_buffer, PATH_LEN, dot);

                            if (!SeenBefore(norm) &&
                                InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                                Log("  >> [dosya]  %s  ==>  %s", original, t_buffer);

                            return t_buffer;
                        }
                        break;
                    }
                }
            }
        }
    }

    // Yansitma silah modeli (2190x): son hane aileyi veriyor. Bunu
    // karaktere yaziyoruz, boylece esya vnum'u tabloda olmasa da
    // dogru aileden silah verebiliyoruz.
    {
        const char* wy = strstr(norm, "/item/weapon/2190");
        if (wy && g_pendEcx)
        {
            const char* f1 = wy + 13;
            if (IsDigit(f1[4]))
            {
                int yf = f1[4] - '0';
                if (yf >= 0 && yf <= 6)
                {
                    EnterCriticalSection(&g_lock);
                    CharEntry* e = FindChar((DWORD)(DWORD_PTR)g_pendEcx, true);
                    if (e) e->fam = yf;
                    LeaveCriticalSection(&g_lock);
                }
            }
        }
    }

    // Istenen her gercek silah dosyasi, o silahin var oldugunun kaniti.
    {
        const char* w0 = strstr(norm, "/item/weapon/");
        if (w0)
        {
            const char* f0 = w0 + 13;
            const char* d0 = strrchr(f0, '.');
            if (!strchr(f0, '/') && d0 && _stricmp(d0, ".gr2") == 0 &&
                (d0 - f0) == 5)
            {
                bool rakam = true;
                for (const char* q = f0; q < d0; ++q)
                    if (!IsDigit(*q)) { rakam = false; break; }
                if (rakam)
                {
                    int vv = atoi(f0);
                    WVarEkle(vv - (vv % 10));
                }
            }
        }
    }

    // --- Aile ustu sinirlama (clamp) ---
    // vnumdegis acikken devre disi: karar artik kurucuda, karakter
    // bazinda veriliyor. Ikisi birden calisirsa dosya kirpmasi
    // herkese ayni silahi dayatiyor.
    // item/weapon/<vnum>.gr2 ve vnum ailesinin klasik tepesinden
    // buyukse, tepedeki klasik silaha cevir.
    {
        const char* w = strstr(norm, "/item/weapon/");
        // "Setter vnum degisimi" KAPALIYKEN dosya kirpmasi da kapali:
        // kutu kapatilinca orijinal goruntu istenir; ustelik bu kanal
        // dosyayi ilk isteyenin seviyesine gore cevirip sonucu HERKESE
        // yapistiriyor (onbellek).
        if (w && g_vnumDegis)
        {
            const char* f = w + 13;
            if (!strchr(f, '/'))
            {
                const char* dot = strrchr(f, '.');
                if (dot && _stricmp(dot, ".gr2") == 0)
                {
                    bool digits = (dot - f) >= 4;
                    for (const char* q = f; q < dot && digits; ++q)
                        if (!IsDigit(*q)) digits = false;

                    if (digits)
                    {
                        char vn[16] = {};
                        size_t vl = (size_t)(dot - f);
                        if (vl < sizeof(vn))
                        {
                            memcpy(vn, f, vl);
                            int fam = WeaponFamily(vn);
                            int v    = atoi(vn);
                            int base = v - (v % 10);
                            int ref  = v % 10;
                            int hedef = -1;
                            const char* neden = "";

                            // YANSITMA SILAHLARI (21900-21909): bunlar
                            // gercek silah degil, gorunum esyasi. Son hane
                            // aileyi veriyor (0 kilic, 1 hancer, 2 yay,
                            // 3 ciftel, 4 can). Oyuncunun seviyesine uygun
                            // klasik silaha cevir.
                            // 219xx icin ozel kural yok: proto onlari
                            // normal silah olarak veriyor.

                            // SEVIYE SINIRI (tablo). vnum tablomuzda yoksa
                            // DOKUNMUYORUZ: vnum'lar duzensiz, tahmin yanlis verir.
                            int silahLvl = WLevelOf(base);

                            // Bu dosyayi isteyen karakterin sinifi ve
                            // ailesi: tablodan (proto), rakamdan degil.
                            if (g_pendEcx) MaskeyiKarakterdenAl(g_pendEcx);
                            { int tf = WFamOf(base); if (tf >= 0) fam = tf; }

                            // Ozel seri / basilamayan silah: her zaman
                            // klasik karsiligi (kendi seviyesine kadar).
                            if (hedef < 0 && !WKlasik(base) && fam >= 0 && fam <= 6)
                            {
                                int oyuncu = g_pendLevel > 0 ? (int)g_pendLevel : g_bilinmeyenSv;
                                int s2 = oyuncu < g_maxLevel ? oyuncu : g_maxLevel;
                                if (s2 > g_capLevel) s2 = g_capLevel;
                                if (silahLvl >= 0 && silahLvl < s2) s2 = silahLvl;
                                int t = WTargetFor(fam, s2);
                                if (t > 0) { hedef = t; neden = "klasik karsilik"; }
                            }

                            if (hedef < 0 && fam >= 0 && fam <= 6 && silahLvl > 0)
                            {
                                // TAVAN KURALI: karar sadece silahin kendi
                                // seviyesine ve tavana bagli. Oyuncu
                                // seviyesini KULLANAMAYIZ: ayni dosyayi
                                // bircok oyuncu paylasiyor ve istemci ilk
                                // cozumu onbellege alip herkese uyguluyor.
                                // (25. seviyedeki adamin yaninda 41'lik
                                // birinin silahinin cikmasinin sebebi buydu.)
                                // Dosya istegi, o karakterin kurucusundan
                                // HEMEN SONRA geliyor; seviyesi elimizde.
                                // Tavan degil, seviye belirleyici.
                                // SEVIYE BILINMIYORSA: eskiden bilinmeyensv (1)
                                // kabul ediliyordu ve silah ailenin EN DUSUGUNE
                                // iniyordu (Gunes Kilici -> Genis Mizrak, hatta
                                // 30'luk Geyik Boynuzu Yay -> Yay). Oysa sunucu
                                // kimseye seviyesinin ustunde silah giydirmez:
                                // bu klasik silahi tasiyan oyuncu EN AZ silahin
                                // seviyesindedir. Alt sinir olarak onu aliyoruz;
                                // sonuc: tavan alti silaha dokunulmaz, tavan
                                // ustu silah tavandaki karsiligina iner.
                                // AMA sadece GERCEK esyada: yansitma silahi
                                // oyuncunun seviyesinden yuksek olabilir. Bu
                                // dosya, kurucunun az once verdigi gercek
                                // silahin (g_pendEsya) dosyasi degilse eski
                                // kural (bilinmeyensv) gecerli kalir.
                                bool gercekEsya = g_pendEsya > 0 &&
                                    WTabanOf((int)g_pendEsya) == base;
                                int oyuncu = g_pendLevel > 0
                                           ? (int)g_pendLevel
                                           : (gercekEsya && silahLvl > g_bilinmeyenSv
                                              ? silahLvl : g_bilinmeyenSv);
                                int sinir  = g_capLevel;
                                {
                                    int etkin = oyuncu < g_maxLevel
                                              ? oyuncu : g_maxLevel;
                                    // sampiyonguven = 1 ise: gercek ekipmani
                                    // seviyesinin izin verdiginden yuksekse
                                    // ekipmana guven (sampiyon seviye 1'e
                                    // donuyorsa adami baslangic silahina
                                    // dusurmeyelim). VARSAYILAN 0 = seviye
                                    // kurali kati uygulanir, senin istedigin bu.
                                    if (g_trustGear && silahLvl > etkin) etkin = silahLvl;
                                    if (etkin < sinir) sinir = etkin;
                                }
                                if (silahLvl > sinir)
                                {
                                    hedef = WTargetFor(fam, sinir);
                                    neden = "seviye";
                                }
                            }

                            // Tablomuzda olmayan silah (yansitma, yeni esya,
                            // ozel vnum): seviyesini bilmiyoruz. Eski kural
                            // burada SEVIYEYE BAKMADAN ailenin tepesine
                            // gonderiyordu -- 9. seviye karakterin elinde
                            // yuksek yay kalmasinin sebebi buydu.
                            // Artik oyuncunun seviyesine gore iniyor.
                            if (hedef < 0 && silahLvl < 0 && fam >= 0 && fam <= 6)
                            {
                                // Tablomuzda olmayan silah: seviyesini
                                // bilmiyoruz ve dosya paylasimli. Herkese
                                // guvenli olan en dusuk seviye silahi.
                                int t = WTargetFor(fam, 1);
                                if (t > 0 && t != base)
                                { hedef = t; neden = "liste-disi"; }
                            }

                            if (hedef > 0 && hedef != base)
                            {
                                int prefixLen = (int)(f - norm);
                                if (prefixLen > 0 && prefixLen + 16 < PATH_LEN)
                                {
                                    memcpy(t_buffer, original, prefixLen);
                                    t_buffer[prefixLen] = '\0';
                                    char tgt[16];
                                    sprintf_s(tgt, "%05d.gr2", hedef + ref);  // + korunur
                                    strcat_s(t_buffer, PATH_LEN, tgt);

                                    if (!SeenBefore(norm) &&
                                        InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                                        Log("  >> [kirp]   %s -> %s  (%s => %s, aile %d, "
                                            "%s, silah sv=%d, oyuncu sv=%ld)",
                                            original, t_buffer,
                                            IsimBul(base), IsimBul(hedef),
                                            fam, neden, silahLvl, g_pendLevel);
                                    return t_buffer;
                                }
                            }
                        }
                    }
                }
            }
        }
    }


    // --- Silah kademe kontrolu:  /item/weapon/<numara>.gr2 ---
    if (g_maxWeapon >= 0 && g_weaponCount > 0)
    {
        const char* w = strstr(norm, "/item/weapon/");
        if (w)
        {
            const char* fileStart = w + 13;          // "/item/weapon/" = 13
            // alt klasor (costume/ gibi) varsa dokunma
            if (!strchr(fileStart, '/'))
            {
                size_t fl = strlen(fileStart);
                if (fl > 4 && strcmp(fileStart + fl - 4, ".gr2") == 0)
                {
                    bool allDigit = true;
                    for (size_t i = 0; i < fl - 4; ++i)
                        if (!IsDigit(fileStart[i])) { allDigit = false; break; }

                    if (allDigit && fl - 4 > 0)
                    {
                        int vnum = atoi(fileStart);
                        int type = vnum / 1000;
                        int inner = vnum % 1000;

                        if (inner > g_maxWeapon)
                        {
                            for (int i = 0; i < g_weaponCount; ++i)
                            {
                                if (g_weapon[i].type != type) continue;

                                int prefixLen = (int)(fileStart - norm);
                                if (prefixLen > 0 &&
                                    prefixLen + strlen(g_weapon[i].model) + 5 < PATH_LEN)
                                {
                                    memcpy(t_buffer, original, prefixLen);
                                    t_buffer[prefixLen] = '\0';
                                    strcat_s(t_buffer, PATH_LEN, g_weapon[i].model);
                                    strcat_s(t_buffer, PATH_LEN, ".gr2");

                                    if (!SeenBefore(norm) &&
                                        InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                                        Log("  >> [silah %d>%d] %s  ==>  %s",
                                            inner, g_maxWeapon, original, t_buffer);

                                    return t_buffer;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    ParsedPath pp;
    ParsePath(original, norm, &pp);
    if (!pp.valid)
    {
        const char* tag = LogTag(norm);
        if (tag && !SeenBefore(norm))
        {
            LONG n = InterlockedIncrement(&g_logCount);
            if (n <= LOG_LIMIT) Log("  [%s] %s", tag, original);
            else if (n == LOG_LIMIT + 1) Log("  (log limiti doldu)");
        }
        return original;
    }

    // ZIRH MODEL ESLEMESI: vnum -> hangi .gr2 / .dds isteniyor.
    // Zirh modeli vnum'dan HESAPLANAMIYOR ve dosya adlari tutarsiz
    // (oyun warrior_4-1, warrior_6_1, warrior_8_1 istiyor; elimizdeki
    // listede bunlar yok). O yuzden tabloyu oyundan olcuyoruz.
    // --- MODEL ISTEGININ CAGIRANINI BUL ---
    // Zirh modelini seciren fonksiyon, bizim hook'ladigimiz zirh
    // setter'i DEGIL: setter 11200 (en dusuk zirh) diyor ama istenen
    // model warrior_nahan oluyor. Demek ki gorunumu baska bir fonksiyon
    // belirliyor. Onu bulmak icin istek anindaki cagri yiginini
    // doküyoruz; exe+RVA degerlerinden dogru fonksiyonu secip
    // hook'layacagiz.
    if (g_izleN > 0 && pp.className[0] && g_exeBase &&
        InterlockedIncrement(&g_izleHits) <= g_izleN)
    {
        void* cerceve[16] = {};
        WORD n = RtlCaptureStackBackTrace(1, 16, cerceve, nullptr);
        char sat[400]; sat[0] = '\0';
        for (WORD i = 0; i < n; ++i)
        {
            BYTE* a = (BYTE*)cerceve[i];
            if (a < g_exeBase || a > g_exeBase + 0x5000000) continue;
            char par[20];
            sprintf_s(par, "+%X ", (DWORD)(a - g_exeBase));
            strcat_s(sat, par);
        }
        Log("  [IZLE] %s  <-  %s", pp.modelName, sat);
    }

    // Zirh modeli adlarini ogren (vnum'a bakmadan: yansitma yuzunden
    // vnum ile model eslesmiyor, ama adin kendisi her zaman gercek).
    if (pp.className[0] && !strstr(pp.modelName, "novice"))
    {
        const char* dot0 = strrchr(norm, '.');
        if (dot0 && _stricmp(dot0, ".gr2") == 0)
        {
            KModOgren(pp.modelName);

            // vnum -> model eslemesi: SADECE tablodaki gercek zirhlar
            // icin ve sadece kademe kalibina uyan adlarda. Kostum ya da
            // yansitma takan karakterlerde model baska esyaya ait
            // oluyor; o yuzden vnum tabloda yoksa ogrenmiyoruz.
            // vnum -> model ogrenmesi KAPALI: yansitma yuzunden
            // "11200 -> warrior_8_1" gibi yanlis eslesme uretiyor.
        }

        // SINIF OGRENME: yolun kendisi sinifi soyluyor (pc/<sinif>/).
        // Bu bilgiyi karakterin nesnesine yaziyoruz; bir sonraki
        // ekipman cagrisinda vnum'u tabloda olmasa bile dogru sinifin
        // zirhini verebiliyoruz.
        if (g_pendEcx && strlen(pp.className) < 16)
        {
            EnterCriticalSection(&g_lock);
            CharEntry* e = FindChar((DWORD)(DWORD_PTR)g_pendEcx, true);
            if (e) strcpy_s(e->cls, pp.className);
            LeaveCriticalSection(&g_lock);
        }

        if (g_eslemeLog > 0 && g_pendArmor > 0 &&
            InterlockedIncrement(&g_zmHits) <= g_eslemeLog * 4)
            Log("  [ZIRHMODEL] vnum=%-6ld sinif=%-9s model=%s%s  (sv=%ld)",
                g_pendArmor, pp.className, pp.modelName,
                dot0 ? dot0 : "", g_pendLevel);
    }

    // ===== ZIRH MODEL BEYAZ LISTESI =====
    // Tek guvenilir veri: istemcinin ISTEDIGI dosya adi. Paket ve
    // nesne alanlari envanterdeki gercek esyayi gosteriyor, ekranda
    // baskasi cizilebiliyor. Dosya adi ise ekranda ne cizilecegini
    // kesin soyler.
    //
    // Kural: kademesi tavanin (maxtier) altinda kalan modeller gecer.
    // Kalibi tutmayan her sey (zodyak, nahan, jinhon, kostum...)
    // sinifin tavan modeline cevrilir. Boylece hicbir sey mavi celigin
    // ustunde gorunemez, verinin nereden geldigi onemli degil.
    bool govde = (strstr(pp.modelName, "novice") != nullptr);

    // ===== ZIRH: SEVIYEYE GORE MODEL =====
    // Kural: sinir = min(karakterin seviyesi, 99, tavan 80).
    // Istenen model tabloda (zirhkademe) ve seviyesi sinirin altindaysa
    // dokunma. Tabloda degilse (kostum, zodyak, tavan ustu zirh) ya da
    // seviyesi sinirin ustundeyse -> sinifin o seviyedeki modeli.
    // Tablodaki adlarin hepsi oyunun kendi istedigi dosyalar.
    // "Zirh kostumunu gizle" kapaliyken zirh modeli dosyalarina dokunma:
    // dosya kanali kostum modelini taniyamiyor, hepsini klasik zirha ceviriyor.
    if (!g_zirhKostumGizle && pp.className[0]) return original;

    if (g_zirhKirp && !govde && pp.className[0] && g_zkadN > 0)
    {
        char sade[NAME_LEN];
        ZModSade(pp.modelName, sade, sizeof(sade));

        int oyuncu = g_pendLevel > 0 ? (int)g_pendLevel : g_bilinmeyenSv;
        // Seviye okunamadiysa (isim yiginda var ama alan gecersiz) oyuncu en
        // dusuk seviye sayiliyor ve Aslan Plaka Kesis Plaka gorunuyordu.
        // Sunucu kimseye seviyesinin ustunde klasik zirh giydirmez: tablodaki
        // gercek zirhin seviyesi alt sinir olur (silah kurali ile ayni).
        if (g_pendLevel <= 0 && g_pendArmor > 0)
        {
            int al = ALevelOf(ATabanOf((int)g_pendArmor));
            if (al > oyuncu) oyuncu = al;
        }
        int sinir  = oyuncu < g_maxLevel ? oyuncu : g_maxLevel;
        if (sinir > g_capLevel) sinir = g_capLevel;

        int msv = ZKadSeviye(pp.className, sade);
        bool izinli = (msv >= 0 && msv <= sinir);

        if (!izinli)
        {
            const char* hedefAd = ZKadHedef(pp.className, sinir);
            if (hedefAd && _stricmp(hedefAd, sade) != 0 &&
                BuildPath(original, &pp, hedefAd, t_buffer, PATH_LEN))
            {
                if (!SeenBefore(norm) &&
                    InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
                    Log("  >> [zirh]   %s -> %s  (%s sv=%d, sinir=%d, %s)",
                        sade, hedefAd, g_pendName[0] ? g_pendName : "?",
                        oyuncu, sinir,
                        msv < 0 ? "tabloda yok" : "seviye ustu");
                return t_buffer;
            }
        }
    }

    // --- 3. Model eslemesi (LOD otomatik) ---
    const char* mapped = FindModelMap(pp.modelName);
    if (mapped && BuildPath(original, &pp, mapped, t_buffer, PATH_LEN))
    {
        if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
            Log("  >> [model]  %s  ==>  %s", original, t_buffer);
        return t_buffer;
    }

    // --- 4. keep ---
    if (IsKept(pp.modelName)) return original;

    // --- 5. Kademe kontrolu ---
    int tier = GetTier(pp.modelName, pp.className);

    if (tier == 0)   // novice
        return original;

    if (tier > 0 && (g_maxTier < 0 || tier <= g_maxTier))
    {
        if (!SeenBefore(norm) && (g_logMask & 1) &&
            InterlockedIncrement(&g_logCount) <= LOG_LIMIT)
            Log("  [kademe %d] %s", tier, original);
        return original;
    }

    // --- 6. Varsayilan ---
    // Kademesi bilinen zirh maxtier'i asiyorsa en yuksek KLASIK kademeye
    // kirpilir; boylece kademe 1..maxtier arasi cesitlilik korunur.
    // Kademesiz (olay/kostum) zirhlar eskisi gibi default'a duser.
    const char* def = nullptr;
    if (tier > 0) def = FindClampTier(pp.className);
    if (!def)     def = FindDefault(pp.className);
    if (def && BuildPath(original, &pp, def, t_buffer, PATH_LEN))
    {
        if (!SeenBefore(norm) && InterlockedIncrement(&g_hitCount) <= LOG_LIMIT)
        {
            if (tier > 0)
                Log("  >> [kademe %d>%d] %s  ==>  %s",
                    tier, g_maxTier, original, t_buffer);
            else
                Log("  >> [kural]  %s  ==>  %s", original, t_buffer);
        }
        return t_buffer;
    }

    if (!SeenBefore(norm) && (g_logMask & 1) &&
        InterlockedIncrement(&g_logCount) <= LOG_LIMIT)
        Log("  [YENI] %s  (sinif '%s' icin default yok)", original, pp.className);

    return original;
}

// Aday: ptr = -1 dogrudan nesne, degilse obj+ptr'deki isaretcinin ardi

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Sayi aramayi birakiyorum. Mantik su: oyun bu karakterin ismini,
// seviyesini ve lonca adini ismin ustunde YAZIYOR. Yazdigi her sey
// bir yerde METIN olarak duruyor. O halde sayi aramak yerine, bu
// nesneye bagli BUTUN metinleri dokelim.
//
// Iki kaynak:
//   a) nesnenin icine gomulu diziler  (isim +0x18'de boyle duruyor)
//   b) nesnedeki her isaretcinin gosterdigi diziler  (lonca adi,
//      unvan ve varsa "Lv 110" gibi bicimlenmis metin)
// Bellek taramasi yok, sadece tek nesneden isaretci takibi. Hizli,
// sinirli ve SEH ile korumali.

// addr'da yazdirilabilir, NUL ile biten bir dizi var mi?
static bool GrabStr(const unsigned char* p, char* out, size_t outSz)
{
    size_t i = 0;
    for (; i + 1 < outSz; ++i)
    {
        unsigned char c = p[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
        out[i] = (char)c;
    }
    out[i] = '\0';
    return i >= 3;              // en az 3 karakter
}

static bool GrabStrSafe(const unsigned char* p, char* out, size_t outSz)
{
    if (!IsReadable(p, 64)) return false;
    __try { return GrabStr(p, out, outSz); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Tek adaya bakar. Kendi SEH'i var: sayfa okunamazsa tarama durmaz.
static bool PktNameAt(const BYTE* p, const char* nm, size_t nmLen)
{
    __try { return memcmp(p, nm, nmLen + 1) == 0; }   // NUL dahil: parcali eslesme yok
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Bir isaretcinin hedefinde ismi arar. Tek SEH blogu, once ilk harf
// kontrolu: aday isaretci cok oldugu icin tam karsilastirmaya nadiren iniyor.
static int PktFindNameNear(const BYTE* t, const char* nm, size_t nmLen)
{
    static const int d[] = { 0, 1, 4, 5, 8 };
    __try
    {
        for (int k = 0; k < 5; ++k)
        {
            const BYTE* p = t + d[k];
            if (*p != (BYTE)nm[0]) continue;
            if (memcmp(p, nm, nmLen + 1) == 0) return d[k];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return -1;
}

// Tek DWORD'u guvenle okur.
static bool PktDword(const BYTE* p, DWORD* out)
{
    __try { *out = *(const DWORD*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ismin etrafindaki pencereyi guvenle kopyalar.
static bool PktGrab(const BYTE* p, BYTE* out)
{
    __try { memcpy(out, p - PKT_BACK, PKT_WIN); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// HotkeyThread'den cagriliyor (120 ms). Nesne serbest birakilmis
// olabilir: once pencereyi SEH ile okuyoruz, sonra ismin hala ayni
// adreste durdugunu dogruluyoruz. Durmuyorsa bellek baskasina
// gecmistir, karsilastirma yapmiyoruz.
static void PktRecheck()
{
    if (g_pktScan <= 0 || g_pendN == 0) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < g_pendN; ++i)
    {
        PktPend& e = g_pend[i];
        if (e.bitti || now - e.t < 4000) continue;
        e.bitti = true;

        BYTE w[PKT_WIN];
        if (!PktGrab(e.p, w)) { Log("  [SABIT] %s: nesne okunamiyor", e.nm); continue; }
        if (!PktNameAt(e.p, e.nm, strlen(e.nm)))
        { Log("  [SABIT] %s: bellek baskasina gecmis", e.nm); continue; }

        char l[600]; int o = 0;
        for (int k = 0; k + 4 <= PKT_WIN; ++k)
        {
            DWORD a = *(const DWORD*)(e.snap + k);
            if (a != *(const DWORD*)(w + k)) continue;   // degismis: seviye degil
            if (a < 1 || a > 250) continue;              // seviye bu aralikta
            if (o < 520) o += sprintf_s(l + o, sizeof(l) - o, "+%d=%u ", k - PKT_BACK, a);
        }
        Log("  [SABIT] %-14s zirh=%-6u isim%+d  %s", e.nm, e.zirh, e.zofs, o ? l : "yok");
    }
    LogFlush();
}

// ecx'in seviyesini cozer ve onbellege alir.
// Dogrulama: nesnenin +108'indeki zirh, setter'in verdigi vnum ile
// AYNI olmali. Bu olmazsa yanlis nesneye bakmis olabilirim, yazmam.
// SEVIYE OKUNAMADI tanilamasi. Isim basina BIR satir (sayacla degil,
// isimle tekillestirilir), boylece hangi karakterde ve neden
// okunamadigi logda kaybolmaz.
static void SeviyeYok(const char* nm, void* ecx, const char* neden, DWORD zirh)
{
    static char gorulen[128][28];
    static int  gorulenN = 0;
    for (int i = 0; i < gorulenN; ++i)
        if (strcmp(gorulen[i], nm) == 0) return;
    if (gorulenN >= 128) return;
    strcpy_s(gorulen[gorulenN++], nm);
    Log("  [SEVIYE?] %-24s %s  (zirh=%u ecx=%08X thread=%u)",
        nm, neden, zirh, (DWORD)(DWORD_PTR)ecx, GetCurrentThreadId());
}

static int LevelFor(void* ecx, const char* nm, DWORD zirh)
{
    if (ecx && nm && !nm[0])
    {
        // Isim nesneden okunamadi: uzunluk alanini dok.
        static volatile LONG isimsiz = 0;
        if (InterlockedIncrement(&isimsiz) <= 20 && IsReadable(ecx, 0x30))
            Log("  [SEVIYE?] isim okunamadi  ecx=%08X uzunluk=%u kapasite=%u",
                (DWORD)(DWORD_PTR)ecx,
                *(DWORD*)((BYTE*)ecx + 0x28), *(DWORD*)((BYTE*)ecx + 0x2C));
    }
    if (!ecx || !nm || !nm[0]) return 0;

    // Onbellek sadece SEVIYEYI hizlandirir; erken DONMEZ. Erken
    // donunce paketi bulamiyor, dolayisiyla gorunur parcalari
    // yamalayamiyorduk: bir karakterin ilk paketi disindaki butun
    // paketleri (zirhi tasiyan paket dahil) atliyorduk.
    int onbellek = 0;
    for (int i = 0; i < g_lvlcN; ++i)
        if (strcmp(g_lvlc[i].nm, nm) == 0 && g_lvlc[i].lvl > 0)
        { onbellek = g_lvlc[i].lvl; break; }

    size_t nmLen = strlen(nm);
    if (nmLen < 3 || nmLen > 24) return 0;

    // SEVIYESI BULUNAMAYAN ISIMLER (NPC, binek, henuz paketi gelmemis karakter):
    // yigin taramasi (32 KB, onlarca SEH) ayni isim icin 250 ms icinde tekrarlanmaz
    // (kisa tutuldu: yeni yuklenen karakterin ikinci cagrisi paketi gorebilir).
    // Seviyesi onbellekte olan karakterlerde bu kisa devre devreye girmez (paket
    // yamasi icin tarama gerekli); sadece "hic bulunamayan" isimler hizlanir.
    DWORD isimHash = 2166136261u;
    for (const char* q = nm; *q; ++q) { isimHash ^= (BYTE)*q; isimHash *= 16777619u; }
    static struct { DWORD h; DWORD t; } basarisiz[256];
    if (!onbellek)
    {
        DWORD ims = isimHash & 255;
        if (basarisiz[ims].h == isimHash && GetTickCount() - basarisiz[ims].t < 250)
            return 0;
    }

    volatile char probe = 0; (void)probe;
    const BYTE* sp = (const BYTE*)((DWORD_PTR)&probe & ~(DWORD_PTR)3);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(sp, &mbi, sizeof(mbi))) return 0;
    const BYTE* hi = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
    if (hi > sp + PKT_STACK_SPAN) hi = sp + PKT_STACK_SPAN;

    int lvl = 0, samp = 0;
    const BYTE* bulunan = nullptr;
    bool siki = false;                // seviye zirh dogrulamali okundu mu
    int  okunanMaske = 0;             // bu cagrida +56'dan okunan sinif
    const DWORD* bulSlot = nullptr;   // paketi gosteren yigin gozu (tanilama)
    int isimHit = 0;                  // isim kac adayda eslesti (tanilama)
    for (const DWORD* q = (const DWORD*)sp;
         (const BYTE*)(q + 1) <= hi && !lvl; ++q)
    {
        DWORD v = *q;
        if (v < 0x00010000 || v > 0x7FFE0000) continue;
        const BYTE* t = (const BYTE*)(DWORD_PTR)v;
        int k = PktFindNameNear(t, nm, nmLen);
        if (k < 0) continue;
        ++isimHit;
        bulSlot = q;
        const BYTE* isim = t + k;
        DWORD z = 0, L = 0;
        // Dogrulama: paketteki zirh, setter'in soyledigi zirhla ayni mi.
        // zirh = 0 ise (silah tarafindan geliyoruz) dogrulayacak bir sey
        // yok, isim eslesmesiyle yetiniyoruz.
        if (zirh && (!PktDword(isim + 108, &z) || z != zirh)) continue;
        if (!PktDword(isim + 160, &L)) continue;
        if (L < 1 || L > 120) continue;     // sunucu tavani 120
        lvl = (int)L;
        bulunan = isim;
        siki = (zirh != 0);                 // zirhla dogrulanmis okuma

        // +164 = SAMPIYON SEVIYESI (0 = sampiyon degil). Dogrulandi:
        // Ronc sampiyon 11 -> +160=120 (gercek), +164=11
        // P1s  sampiyon 1  -> +160=120 (gercek), +164=1
        DWORD S = 0;
        if (PktDword(isim + 164, &S) && S >= 1 && S <= 120) samp = (int)S;

        // +56 = IRK/SINIF (dogrulandi: 0/4 warrior, 1/5 assassin,
        // 2/6 sura, 3/7 shaman, 8/9 wolfman). Sinifi MODEL YUKLENMEDEN
        // once ogrendigimiz icin kostum ve yansitma ilk karede
        // kirpilabiliyor; eskiden model cizildikten sonra ogrendigimiz
        // icin ilk goruste tavan ustu zirh gecip gidiyordu.
        DWORD R = 0;
        if (PktDword(isim + 56, &R) && R <= 9)
        {
            const char* cl = nullptr;
            switch (R % 4)
            {
            case 0: cl = "warrior";  break;
            case 1: cl = "assassin"; break;
            case 2: cl = "sura";     break;
            case 3: cl = "shaman";   break;
            }
            if (R >= 8) cl = "wolfman";
            if (cl) t_sinifMaske = SinifMaske(cl);
            if (cl) okunanMaske = t_sinifMaske;
            if (cl)
            {
                EnterCriticalSection(&g_lock);
                CharEntry* ce = FindChar((DWORD)(DWORD_PTR)ecx, true);
                if (ce) strcpy_s(ce->cls, cl);
                LeaveCriticalSection(&g_lock);
            }
        }
    }

    // Siki gecis bos dondu: zirh dogrulamasini birakip sadece isimle
    // bir daha dene. Seviyenin hic okunamamasinin ana sebebi buydu --
    // bekleyen zirh baska karaktere aitse dogrulama hep patliyordu.
    if (!lvl && zirh)
    {
        for (const DWORD* q = (const DWORD*)sp;
             (const BYTE*)(q + 1) <= hi && !lvl; ++q)
        {
            DWORD v = *q;
            if (v < 0x00010000 || v > 0x7FFE0000) continue;
            const BYTE* t = (const BYTE*)(DWORD_PTR)v;
            int k = PktFindNameNear(t, nm, nmLen);
            if (k < 0) continue;
            bulSlot = q;
            const BYTE* isim = t + k;
            DWORD L = 0;
            if (!PktDword(isim + 160, &L)) continue;
            if (L < 1 || L > 120) continue;     // sunucu tavani 120
            lvl = (int)L;
            bulunan = isim;
            DWORD S = 0;
            if (PktDword(isim + 164, &S) && S >= 1 && S <= 120) samp = (int)S;
        }
    }

    if (!lvl && onbellek) lvl = onbellek;

    // SEVIYE SABIT KALSIN: logda ayni oyuncu bir cagrida 120, sonrakinde
    // 99; bir baskasi 76 sonra 149 okundu. Zirh dogrulamasi olmadan,
    // yalnizca isim eslesmesiyle bulunan tampon baska bir yapi olabiliyor
    // ve +160'i seviye degil. Boyle bir okuma, daha once ogrenilmis
    // seviyeyi EZMEZ. (Seviye degisince silah da degisiyor, istemci esyayi
    // yeniden takiyor ve animasyon basa sariyordu.)
    if (lvl && onbellek && !siki) lvl = onbellek;

    // "Gorunen seviye" yamasi pakete 99 yaziyor; sonraki okumada o 99 geri
    // geliyor (logda 120 -> 99). Gercek seviye onbellekte duruyorsa onu koru.
    if (g_seviyeGoster > 0 && lvl == g_seviyeGoster && onbellek > lvl) lvl = onbellek;

    // Hicbir yoldan cozulemedi: sebebiyle birlikte bir kez yaz.
    if (!lvl)
    {
        basarisiz[isimHash & 255].h = isimHash;
        basarisiz[isimHash & 255].t = GetTickCount();
    }
    if (!lvl)
        SeviyeYok(nm, ecx,
                  isimHit ? "isim yiginda VAR ama seviye alani gecersiz"
                          : "yiginda bu isimle paket YOK", zirh);

    // KAYNAK TANILAMASI: paket yiginda nerede duruyor ve o noktaya
    // kadar hangi exe adresleri var? Bunlar arasinda paketi isleyen
    // fonksiyonun donus adresi olmali; onu bulunca yigin taramasi
    // yerine dogrudan o fonksiyon hooklanir (kesin okuma).
    {
        static volatile LONG kaynakHit = 0;
        if (bulunan && bulSlot && g_exeBase &&
            InterlockedIncrement(&kaynakHit) <= 12)
        {
            char sat[400]; sat[0] = '\0';
            int n = 0;
            for (const DWORD* q = (const DWORD*)sp; q <= bulSlot + 4 && n < 22; ++q)
            {
                if ((const BYTE*)(q + 1) > hi) break;
                const BYTE* a = (const BYTE*)(DWORD_PTR)*q;
                if (a < g_exeBase + 0x1000 || a > g_exeBase + 0x5000000) continue;
                char par[24];
                sprintf_s(par, "%X:+%X ", (DWORD)((const BYTE*)q - sp),
                          (DWORD)(a - g_exeBase));
                strcat_s(sat, par);
                ++n;
            }
            Log("  [KAYNAK] %-16s zirh=%u goz=+%X | %s",
                nm, zirh, (DWORD)((const BYTE*)bulSlot - sp), sat);
        }
    }

    int kayitIdx = -1;
    for (int i = 0; i < g_lvlcN; ++i)
        if (strcmp(g_lvlc[i].nm, nm) == 0) { kayitIdx = i; break; }
    bool kayitli = kayitIdx >= 0;

    // ONBELLEK DUZELTMESI (FPS):
    // 1) Kayitli isim yeni bir nesneyle (ecx) geri geldiyse kaydi
    //    tazele. Eskiden ecx ilk gorusteki haliyle kaliyordu;
    //    LevelCached (ecx + isim arar) o karakter icin bir daha hic
    //    tutmuyor, her ekipman cagrisinda yigin taramasi yapiliyordu.
    if (lvl && kayitli)
    {
        // Yeni nesne = yeni yukleme: onceki silah kaydi gecersiz.
        if (g_lvlc[kayitIdx].ecx != ecx)
        {
            g_lvlc[kayitIdx].silahCikis  = 0;
            g_lvlc[kayitIdx].silahTakili = 0;
            g_lvlc[kayitIdx].silahHam    = 0;
            g_lvlc[kayitIdx].kostumHam   = 0;
            g_lvlc[kayitIdx].zirhHam     = 0;
            g_lvlc[kayitIdx].a0c         = 0;
        }
        g_lvlc[kayitIdx].ecx = ecx;
        g_lvlc[kayitIdx].lvl = lvl;
        if (bulunan) g_lvlc[kayitIdx].isim = bulunan;
        if (okunanMaske) g_lvlc[kayitIdx].maske = okunanMaske;
        // Bu cagrida sinif okunamadiysa onbellektekini kullan.
        else if (g_lvlc[kayitIdx].maske) t_sinifMaske = g_lvlc[kayitIdx].maske;
    }

    // 2) Tablo dolunca en eski kaydin uzerine yaz. Eskiden dolduktan
    //    sonra gelen hicbir karakter onbellege girmiyordu: hem her
    //    cagrida yigin taramasi, hem de paket yamasi hic calismiyordu.
    if (lvl && !kayitli)
    {
        int yer = g_lvlcN;
        if (yer >= LVLC_MAX)
        {
            yer = g_lvlcSira;
            g_lvlcSira = (g_lvlcSira + 1) % LVLC_MAX;
        }
        g_lvlc[yer].ecx  = ecx;
        strcpy_s(g_lvlc[yer].nm, nm);
        g_lvlc[yer].lvl  = lvl;
        g_lvlc[yer].isim = bulunan;
        g_lvlc[yer].maske = okunanMaske;
        g_lvlc[yer].silahCikis = 0;
        g_lvlc[yer].silahTakili = 0;
        g_lvlc[yer].silahHam = 0;
        g_lvlc[yer].kostumHam = 0;
        g_lvlc[yer].zirhHam = 0;
        g_lvlc[yer].a0c = 0;
        g_lvlc[yer].ekArg[0] = g_lvlc[yer].ekArg[1] = g_lvlc[yer].ekArg[2] = 0;
        g_lvlc[yer].nesil = 0;
        if (yer == g_lvlcN) ++g_lvlcN;
    }

    // PAKET YAMASI HER GORUSTE: eskiden asagidaki blok sadece karakter
    // onbellege ILK girdiginde calisiyordu. Karakter esya degistirince
    // ya da tekrar gorununce gelen yeni paket yamalanmadan geciyordu.
    // Artik paket bu cagrida gercekten bulunduysa (bulunan) her
    // seferinde calisir. Yama ayni pakete tekrar uygulansa da sonuc
    // degismez (deger zaten sinirin altinda kalir).
    if (lvl && bulunan)
    {

        // SAMPIYON AVI: seviyenin cevresindeki alanlari dok. Sampiyon
        // karakterlerin seviyesi oyunda MAVI yaziyor, yani istemcide
        // bir bayrak var. Hangi alan oldugunu bilmiyoruz; sampiyon ve
        // normal karakterleri yan yana koyup farki ariyoruz.
        // SAMPIYON = 120'nin ustu. Ekipman icin en yuksek seviye
        // sayilir, yani zirhi/silahi kirpilmaz (zaten tavanin ustunde).
        // Goruntude ise mavi sampiyon seviyesi yerine duz 99 yaziyoruz.

        // --- GORUNEN SEVIYEYI KIRP ---
        // Istemcinin kendi kopyasina yaziyoruz: +160 gercek seviye,
        // +164 sampiyon seviyesi (0 = sampiyon degil). 99 ustu herkes
        // 99 gorunur, sampiyon mavisi de normal 99'a doner.
        // Ekipman karari bu yazmadan ONCE okunan gercek seviyeye gore
        // veriliyor (lvl degiskeni), yani kirpma bozulmuyor.
        if (g_seviyeGoster > 0)
        {
            BYTE* yaz = (BYTE*)bulunan;
            DWORD eski = 0;
            if (VirtualProtect(yaz + 160, 8, PAGE_READWRITE, &eski))
            {
                DWORD* sv  = (DWORD*)(yaz + 160);
                DWORD* sam = (DWORD*)(yaz + 164);
                if (*sv > (DWORD)g_seviyeGoster) *sv = (DWORD)g_seviyeGoster;
                if (samp > 0)
                {
                    *sam = 0;                       // mavi sampiyon rengi kalksin
                    *sv  = (DWORD)g_seviyeGoster;   // duz 99
                }
                VirtualProtect(yaz + 160, 8, eski, &eski);
            }
        }

        // --- GORUNUR PARCALAR ---
        // Istemci modeli bu alanlardan kuruyor: +25 zirh, +29 silah,
        // +33 kafa, +37 sac, +41..+53 kostum/kanat. Setter'in gordugu
        // vnum envanterdeki GERCEK esya; ekranda gorunen ise bu
        // parcalar. Kirpmayi buraya yaziyoruz.
        if (bulunan)
        {
            DWORD pz = 0, ps = 0;
            bool okZ = PktDword(bulunan + 25, &pz);
            bool okS = PktDword(bulunan + 29, &ps);

            if (g_parcaLog > 0 &&
                InterlockedIncrement(&g_parcaHits) <= g_parcaLog)
            {
                // Parcalari +25/+29'da aramak yanlisti (hepsi 0 cikti ve
                // oraya yazinca oyun coktu). Gercek zirh +108'de
                // oldugunu BILIYORUZ; gorunur parcalar da buyuk ihtimalle
                // komsulugunda. Bu araligi dokuyoruz.
                char sat[400]; sat[0] = '\0';
                for (int o = 96; o <= 156; o += 4)
                {
                    DWORD d = 0; char par[20];
                    if (PktDword(bulunan + o, &d)) sprintf_s(par, "%d:%u ", o, d);
                    else                            sprintf_s(par, "%d:? ", o);
                    strcat_s(sat, par);
                }
                Log("  [PARCA] %-16s sv=%-4d %s", nm, lvl, sat);
            }

            if (g_parcaYaz && lvl > 0)
            {
                // DOGRULANAN ALANLAR (dokumle):
                //   +108 gorunur zirh, +112 gorunur silah,
                //   +128 zirh kostumu / yansitmasi (49xxx)
                // Millionnier: +108=11209 (en dusuk zirh) ama +128=49006;
                // ekranda gorunen o kostumdu. Kirpmayi buraya yaziyoruz.
                int sinir = lvl < g_maxLevel ? lvl : g_maxLevel;
                if (sinir > g_capLevel) sinir = g_capLevel;

                const char* cl = nullptr;
                DWORD R2 = 0;
                if (PktDword(bulunan + 56, &R2) && R2 <= 15)
                {
                    switch (R2 % 4)
                    {
                    case 0: cl = "warrior";  break;
                    case 1: cl = "assassin"; break;
                    case 2: cl = "sura";     break;
                    case 3: cl = "shaman";   break;
                    }
                    if (R2 >= 8) cl = "wolfman";
                }

                // +56 beklenmedik bir deger verdiyse sinifi karakterin
                // kendi kaydindan al (model yolundan ogrenilmisti).
                // Sinif bos kalinca zirh kirpmasi komple atlaniyordu --
                // zodyak zirhlarin gecmesinin sebebi buydu.
                char clYedek[16] = {};
                if (!cl)
                {
                    EnterCriticalSection(&g_lock);
                    CharEntry* ce3 = FindChar((DWORD)(DWORD_PTR)ecx, false);
                    if (ce3 && ce3->cls[0]) strcpy_s(clYedek, ce3->cls);
                    LeaveCriticalSection(&g_lock);
                    if (clYedek[0]) cl = clYedek;

                    if (g_parcaLog > 0 &&
                        InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 3)
                        Log("  [SINIF?] %-16s +56=%u -> %s",
                            nm, R2, cl ? cl : "BILINMIYOR");
                }

                if (cl)
                {
                    t_sinifMaske = SinifMaske(cl);
                    LvlcMaskeYaz(nm, t_sinifMaske);
                }
                else
                {
                    // Sinif bu pakette de bilinmiyor: onbellekte varsa onu
                    // kullan, yoksa silah secimi sinifsiz yapilmasin diye
                    // asagida silaha dokunmuyoruz (sinifBilinmiyor).
                    int m = LvlcMaskeIsim(nm);
                    t_sinifMaske = m ? m : 31;
                }
                bool sinifBilinmiyor = (t_sinifMaske == 31);

                DWORD pz = 0, ps = 0, pk = 0;
                PktDword(bulunan + 108, &pz);
                PktDword(bulunan + 112, &ps);
                PktDword(bulunan + 128, &pk);

                // ARALIK KONTROLU (COKME DUZELTMESI): bu alanlar esya vnum'u
                // degilse dokunma. Silah alaninda cok buyuk bir deger
                // geldiginde asagidaki "%05d" 8 baytlik tampona sigmiyor ve
                // oyun kapaniyordu (dokumde dogrulandi: c0000409, bu satir).
                // Zirh alaninda 1 gibi degerler de zirh degil (Fly55'e
                // "zirh 1 -> 11820" diye zirh giydirmistik).
                if (pz < 1000 || pz >= 100000) pz = 0;
                if (ps < 10   || ps >= 100000) ps = 0;
                if (sinifBilinmiyor) ps = 0;   // sinifsiz silah secme

                BYTE* yz = (BYTE*)bulunan;
                DWORD eski = 0;
                if (VirtualProtect(yz + 104, 32, PAGE_READWRITE, &eski))
                {
                    // --- ZIRH KOSTUMU: kaldir, altindaki zirh gorunsun
                    if (false)   // +128 AURA, zirh kostumu degil (proto: COSTUME_AURA)
                    {
                        *(DWORD*)(yz + 128) = 0;
                        if (g_parcaLog > 0 &&
                            InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 3)
                            Log("  [PARCA>>] %-16s kostum %u -> 0", nm, pk);
                    }

                    // --- ZIRH: tavan/seviye ustuyse indir
                    if (pz)
                    {
                        int zb = ATabanOf((int)pz);
                        int zl = ALevelOf(zb);
                        int hzLog = cl ? ATargetFor(cl, sinir) : -1;
                        if (g_parcaLog > 0 &&
                            InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 4)
                            Log("  [ZKARAR] %-16s sv=%-4d zirh=%u taban=%d zsv=%d "
                                "sinif=%s sinir=%d hedef=%d",
                                nm, lvl, pz, zb, zl, cl ? cl : "YOK", sinir, hzLog);
                    }

                    // Sinif hala bos ise, GORUNUR zirhin kendi vnum'undan
                    // sinifi cikarmayi dene (tablodaysa sinifini biliyoruz).
                    if (!cl && pz)
                    {
                        const char* c2 = AClassOf((int)pz - ((int)pz % 10));
                        if (c2) cl = c2;
                    }

                    // "Zirh kostumunu gizle" kapaliysa kostume (41xxx) dokunma.
                    if (cl && pz && !((pz >= 41000 && pz < 42000) && !g_zirhKostumGizle))
                    {
                        int zb = ATabanOf((int)pz);
                        int zl = ALevelOf(zb);
                        if (zl < 0 || zl > sinir)
                        {
                            int hz = ATargetFor(cl, sinir);
                            if (hz > 0 && hz != zb)
                            {
                                *(DWORD*)(yz + 108) = (DWORD)hz;
                                if (g_parcaLog > 0 &&
                                    InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 3)
                                    Log("  [PARCA>>] %-16s sv=%-4d zirh %u -> %d",
                                        nm, lvl, pz, hz);
                            }
                        }
                    }

                    // --- SILAH: yansitma (219xx) ve tavan ustu
                    if (ps)
                    {
                        int sb2 = WTabanOf((int)ps);
                        int ref = (int)ps - sb2;
                        int sf  = WFamOf(sb2);            // aile: tablodan (proto)
                        int sl2 = WLevelOf(sb2);

                        if (sf < 0)
                        {
                            char mdl[16];
                            sprintf_s(mdl, "%05d", sb2);
                            sf = WeaponFamily(mdl);
                        }

                        // Silah yuvasinda KOSTUM (40xxx) varsa aile proto'dan
                        // gelir ve kostum, bu karakterin seviyesindeki klasik
                        // silaha cevrilir (kurucudaki kuralin aynisi; iki
                        // kanal ayni sonucu uretsin diye).
                        bool kostumS = (ps >= 40000 && ps < 41000);
                        if (kostumS)
                            sf = g_kostumSilahGizle ? KSilFam((int)ps) : -1;

                        int hs = -1;
                        if (sf >= 0 && sf <= 6)
                        {
                            if (kostumS)
                            {
                                hs = WTargetFor(sf, sinir);
                                ref = 0;
                            }
                            else if (!WKlasik(sb2))
                            {
                                // Ozel seri / basilamayan silah: klasik
                                // karsiligi, silahin kendi seviyesine kadar.
                                int s2 = (sl2 >= 0 && sl2 < sinir) ? sl2 : sinir;
                                hs = WTargetFor(sf, s2);
                                ref = OzelRef(sb2);
                            }
                            else if (sl2 < 0 || sl2 > sinir)
                                hs = WTargetFor(sf, sinir);
                        }

                        if (hs > 0 && hs != sb2)
                        {
                            *(DWORD*)(yz + 112) = (DWORD)(hs + ref);
                            if (g_parcaLog > 0 &&
                                InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 3)
                                Log("  [PARCA>>] %-16s sv=%-4d silah %u (%s) -> %d (%s)",
                                    nm, lvl, ps, IsimBul(sb2), hs + ref, IsimBul(hs));
                        }
                    }

                    VirtualProtect(yz + 104, 32, eski, &eski);
                }

                // --- YANSITMA: kaldir ---
                // Yansitma vnum'lari +172..+200 araliginda duruyor
                // (Millionnier: 180=21325 zirh, 188=369 silah; NWA:
                // 184/192). Konum karaktere gore birkac bayt kayiyor, o
                // yuzden sabit offset yerine degerin TURUNE bakiyoruz:
                // tablodaki zirh/silah ya da kostum ise yansitmadir.
                // Sifirlayinca yansitma kalkar, altindaki gercek (ve
                // zaten kirpilmis) esya gorunur.
                if (g_yansKaldir)
                {
                    DWORD eski2 = 0;
                    if (VirtualProtect(yz + 172, 32, PAGE_READWRITE, &eski2))
                    {
                        for (int o = 172; o <= 200; o += 4)
                        {
                            DWORD d = *(DWORD*)(yz + o);
                            if (d < 10 || d > 100000) continue;
                            bool silahMi = WLevelOf(WTabanOf((int)d)) >= 0 ||
                                           KSilFam((int)d) >= 0;
                            bool zirhMi  = ALevelOf(ATabanOf((int)d)) >= 0 ||
                                           (d >= 41000 && d < 42000);
                            if (!silahMi && !zirhMi) continue;
                            *(DWORD*)(yz + o) = 0;
                            if (g_parcaLog > 0 &&
                                InterlockedIncrement(&g_parcaHits) <= g_parcaLog * 4)
                                Log("  [YANS>>] %-16s +%d  %u (%s) -> 0",
                                    nm, o, d, silahMi ? "silah" : "zirh");
                        }
                        VirtualProtect(yz + 172, 32, eski2, &eski2);
                    }
                }
            }
        }

        // --- YANSITMA ALANI AVI ---
        // Gorunur yuvalar +108..+132 arasinda ve dogru okunuyor; ama
        // yansitma bunlarin hicbirinde degil (Millionnier: +108=11209
        // ama ekranda warrior_8_1). Paketin geri kalaninda esya vnum'u
        // olabilecek her degeri dokuyoruz.
        if (g_yansAra > 0 && bulunan &&
            InterlockedIncrement(&g_yansHits) <= g_yansAra)
        {
            char sat[700]; sat[0] = '\0';
            for (int o = 136; o <= 400; o += 2)
            {
                DWORD d = 0;
                if (!PktDword(bulunan + o, &d)) break;
                if (o == 160 || o == 164) continue;          // seviye, sampiyon
                if (d < 10 || d > 100000) continue;
                char par[24];
                sprintf_s(par, "%d:%u ", o, d);
                if (strlen(sat) + strlen(par) < sizeof(sat) - 1)
                    strcat_s(sat, par);
            }
            Log("  [YANS?] %-16s sv=%-4d %s", nm, lvl, sat);
        }

        // PAKET DOKUMU: sinifi paketten okuyabilirsek, karakteri ilk
        // gordugumuz anda zirhini dogru cevirebiliriz. Simdilik sinifi
        // ancak model yuklendikten SONRA ogreniyoruz, o yuzden ilk
        // goruste kostum/yansitma kirpilmadan geciyor.
        if (g_paketDok > 0 && bulunan &&
            InterlockedIncrement(&g_pdokHits) <= g_paketDok)
        {
            char d1[400]; d1[0] = '\0';
            for (int o = 0; o <= 100; o += 4)
            {
                DWORD d = 0; char par[20];
                if (PktDword(bulunan + o, &d)) sprintf_s(par, "%d:%X ", o, d);
                else                            sprintf_s(par, "%d:? ", o);
                strcat_s(d1, par);
            }
            Log("  [PDOK] %-16s sv=%-4d %s", nm, lvl, d1);
        }

        if (g_sampiyonLog > 0 && bulunan &&
            InterlockedIncrement(&g_sampHits) <= g_sampiyonLog)
        {
            char sat[400]; sat[0] = '\0';
            for (int o = 140; o <= 200; o += 4)
            {
                DWORD d = 0;
                char par[24];
                if (PktDword(bulunan + o, &d)) sprintf_s(par, "%d:%08X ", o, d);
                else                            sprintf_s(par, "%d:-------- ", o);
                strcat_s(sat, par);
            }
            Log("  [SAMP] %-16s sv=%-4d sampiyon=%-4d %s", nm, lvl, samp, sat);
        }
    }
    return lvl;
}

static int LevelCached(void* ecx, const char* nm)
{
    if (!ecx || !nm || !nm[0]) return 0;
    for (int i = 0; i < g_lvlcN; ++i)
        if (g_lvlc[i].ecx == ecx && strcmp(g_lvlc[i].nm, nm) == 0)
            return g_lvlc[i].lvl;
    return 0;
}



// ---------------------------------------------------------------------------
// GUNCELLEME FONKSIYONU (+24E8480)  --  ANIMASYON TITREMESININ KAYNAGI
//
// Ne oldugu (loglarla dogrulandi): her karakter nesnesi icin surekli calisan,
// ARGUMAN ALMAYAN bir fonksiyon (NPC ve binekler icin de cagriliyor). Nesnenin
// icinde duran "olmasi gereken silah" ile "su an takili silah"i karsilastiriyor;
// farkliysa silah fonksiyonunu (equipfn) cagiriyor: +24E8782 gercek silah,
// +24E879A kostum, +24E880B kostumsuz karakterin silahi.
//
// Sorun: biz equipfn'de silahi degistiriyoruz. Takili olan (cevrilmis) ile
// olmasi gereken (orijinal) hic esit olmuyor; fonksiyon her calistiginda silahi
// yeniden taktiriyor, animasyon basa sariyor, yetenek sesleri tekrarlaniyor.
// Kanit: "Setter vnum degisimi" kapatilinca titreme bitiyor; acilinca silahlar
// geri gelmiyor (cunku o zaman ikisi esit, equipfn hic cagrilmiyor).
//
// Cozum iki parca:
//  1) ATLAMA: equipfn bu fonksiyonun icinden, o karaktere ZATEN takili olan
//     (bizim verdigimiz) silahla tekrar cagrilirsa, cagri hic yapilmaz ve
//     "degisiklik yok" (0) dondurulur. Oyunun, silah ayniyken zaten yaptigi
//     seyin aynisi. Bunun icin equipfn'in yigindan kac bayt arguman temizledigi
//     calisma aninda OLCULUR (donus adresi bir kereligine degistirilerek).
//  2) YENIDEN TAKTIRMA: kutu acildiginda takili silah orijinalse, fonksiyonun
//     girisinde equipfn dogrudan cagrilip cevrilmis silah taktirilir.
//
// Giris adresi calisma aninda bulunup GuncelKur() ile hooklanir (asagida).
static volatile DWORD g_guncelRva   = 0;   // hook kurulu giris (0 = yok)
static volatile DWORD g_guncelAday  = 0;   // bulunan, henuz kurulmamis giris
static volatile LONG  g_guncelDurum = 0;   // 0 bekliyor, 1 kuruldu/denendi

// --- Trambolin yardimcilari (ayri test programinda dogrulandi) ---
static int g_fnArgBytes[MAX_FNHOOKS] = { -1, -1, -1, -1, -1, -1 };  // ret N'deki N
static volatile LONG g_olcMesgul = 0;
static DWORD g_olcIdx = 0, g_olcRet = 0, g_olcEsp = 0;
static volatile LONG g_atlananCagri = 0;
static volatile LONG g_zorlananCagri = 0;

// Olculen fonksiyon "ret N" ile BURAYA doner: yigin isaretcisinden N'yi
// hesaplar, kaydeder ve asil donus adresine atlar.
__declspec(naked) static void OlcumDonus()
{
    __asm
    {
        push eax
        pushfd
        lea  eax, [esp + 8]
        sub  eax, [g_olcEsp]
        sub  eax, 4
        mov  ecx, [g_olcIdx]
        mov  [g_fnArgBytes + ecx * 4], eax
        mov  dword ptr [g_olcMesgul], 0
        popfd
        pop  eax
        jmp  dword ptr [g_olcRet]
    }
}

// thiscall bir fonksiyonu n argumanla cagirir (argumanlari cagrilan temizler).
__declspec(naked) static DWORD __cdecl ThisCagir(void* fn, void* self, DWORD* args, int n)
{
    __asm
    {
        push ebp
        mov  ebp, esp
        push esi
        push edi
        mov  esi, [ebp + 16]
        mov  ecx, [ebp + 20]
    dongu:
        test ecx, ecx
        jz   bitti
        dec  ecx
        push dword ptr [esi + ecx * 4]
        jmp  dongu
    bitti:
        mov  ecx, [ebp + 12]
        call dword ptr [ebp + 8]
        lea  esp, [ebp - 8]
        pop  edi
        pop  esi
        pop  ebp
        ret
    }
}

// Bir silah vnum'unu bu karakter icin cevir. 0 = degisiklik yok.
static DWORD SilahCevir(void* ecx, DWORD vnum, int lvl)
{
    int sb  = WTabanOf((int)vnum);
    int ref = (int)vnum - sb;
    int fam = WFamOf(sb);
    if (fam < 0 || fam > 6) return 0;
    int svS = WLevelOf(sb);
    if (svS < 0) return 0;
    if (WKlasik(sb) && svS > lvl) lvl = svS;      // imkansiz seviye: alt sinir
    int sinir = lvl < g_maxLevel ? lvl : g_maxLevel;
    if (sinir > g_capLevel) sinir = g_capLevel;

    MaskeyiKarakterdenAl(ecx);
    int hedef = -1;
    if (!WKlasik(sb))
    {
        int s2 = (svS < sinir) ? svS : sinir;
        hedef = WTargetFor(fam, s2);
        ref = OzelRef(sb);
    }
    else if (svS > sinir)
        hedef = WTargetFor(fam, sinir);
    if (hedef > 0 && hedef != sb) return (DWORD)(hedef + ref);
    return 0;
}

// --- CAGRI SONRASI: "takili silah" alanini oyunun bekledigi degere geri yaz ---
// equipfn, verilen vnum'u nesnenin icine "su an takili silah" diye yazar.
// Biz vnum'u degistirdigimiz icin orada cevrilmis silah (T) durur; guncelleme
// fonksiyonu bunu "olmasi gereken" orijinal silahla (V) karsilastirir, esit
// bulmaz ve silahi surekli yeniden taktirir. Cagridan ONCE nesnenin kopyasini
// alip, cagridan SONRA "T'ye donusmus" alanlari bulur ve oraya V yazariz.
// Boylece oyunun karsilastirmasi tutar ve silaha bir daha dokunmaz; ekranda
// ise cevrilmis silah kalir (model coktan yuklendi).
#define SON_BOY 0x4000
static volatile LONG g_sonMesgul = 0;
static DWORD g_sonRet = 0;
static void* g_sonEcx = nullptr;
static DWORD g_sonV = 0, g_sonT = 0;
static DWORD g_sonOnce[SON_BOY / 4];
static int   g_sonOnceN = 0;
static int   g_sonMod = 0;            // 0 silah alani yaz, 1 zirh alanini ogren

// --- ZIRHI ANINDA DEGISTIRME ---
// armorfn (+24E60B0) alt seviye: dogrudan cagrilinca model degisiyor ama
// animasyon durumu yenilenmiyor (T-poz, donma). Onu cagiran ust fonksiyon
// (+24E0940, tek argumani zirh vnum'u) hem zirhi degistiriyor hem durumu
// yeniliyor. Giris calisma aninda dogrulanip hooklanir; arguman bayti
// olculur; "this"in karakter nesnesi oldugu dogrulanir; nesnedeki "takili
// zirh" alani ogrenilir. Dordu de tamamsa zirh aninda degistirilir, biri
// eksikse zirha dokunulmaz.
static volatile DWORD g_zirhUstRva   = 0;
static volatile DWORD g_zirhUstAday  = 0;
static volatile LONG  g_zirhUstDurum = 0;
static volatile LONG  g_zirhUstThis  = 0;   // 1: this = karakter nesnesi (dogrulandi)
static int            g_zirhAlan     = -1;  // nesnede "takili zirh" alaninin ofseti

static int NesneKopyala(void* ecx, DWORD* out)
{
    int n = 0;
    __try
    {
        const DWORD* p = (const DWORD*)ecx;
        for (; n < SON_BOY / 4; ++n) out[n] = p[n];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

static bool NesneYaz(void* ecx, int ofs, DWORD v)
{
    __try { *(DWORD*)((BYTE*)ecx + ofs) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

extern "C" void __cdecl SonraIsle()
{
    static DWORD sonra[SON_BOY / 4];
    void* ecx = g_sonEcx;
    DWORD V = g_sonV, T = g_sonT;
    int n = NesneKopyala(ecx, sonra);
    if (n > g_sonOnceN) n = g_sonOnceN;

    int ofs[8], k = 0;
    for (int i = 0; i < n; ++i)
        if (sonra[i] == T && g_sonOnce[i] != T)
        {
            if (k < 8) ofs[k] = i * 4;
            ++k;
        }
    // ZIRH: sadece alani OGREN, hicbir sey yazma.
    if (g_sonMod == 1)
    {
        if (g_zirhAlan < 0 && k == 1)
        {
            g_zirhAlan = ofs[0];
            Log("  [GUNCEL] takili-zirh alani ogrenildi: +%X", g_zirhAlan);
        }
        else if (g_zirhAlan < 0)
        {
            static volatile LONG zu = 0;
            if (InterlockedIncrement(&zu) <= 6)
                Log("  [GUNCEL] takili-zirh alani ogrenilemedi: %d eslesme (T=%u)", k, T);
        }
        InterlockedExchange(&g_sonMesgul, 0);
        return;
    }

    // Alan bir kez ogrenilince (tek eslesme) sadece ORAYA yazilir. Cagri
    // alani degistirmediyse bile (zaten T duruyorsa) duzeltilir: kostumlu
    // karakterde gercek silah cagrisi kostumden SONRA gelirse boyle olur.
    static int ogrenilen = -1;
    char ad[28];
    ReadCharName(ecx, ad, sizeof(ad));
    if (ogrenilen < 0 && k == 1) ogrenilen = ofs[0];

    if (ogrenilen >= 0)
    {
        int i = ogrenilen / 4;
        if (i < n && sonra[i] == T)
        {
            NesneYaz(ecx, ogrenilen, V);
            Log("  [GUNCEL] %-16s alan +%X: %u yerine %u yazildi", ad[0] ? ad : "?",
                ogrenilen, T, V);
        }
    }
    else
    {
        static volatile LONG uy = 0;
        if (InterlockedIncrement(&uy) <= 10)
            Log("  [GUNCEL] takili-silah alani ogrenilemedi: %d eslesme (V=%u T=%u)", k, V, T);
    }
    InterlockedExchange(&g_sonMesgul, 0);
}

__declspec(naked) static void SonraDonus()
{
    __asm
    {
        pushad
        pushfd
        call SonraIsle
        popfd
        popad
        jmp  dword ptr [g_sonRet]
    }
}

// Guncelleme fonksiyonunun girisi. Normalde hicbir sey yapmaz (her nesne
// icin surekli cagrildigindan hafif kalmali). Paneldeki silah ayarlarindan
// biri degistikten sonraki 6 saniye boyunca, her karaktere oyunun verdigi
// ORIJINAL silahi (ve varsa kostumu) kendi hook'umuzdan gecirerek yeniden
// taktirir: ayar aciksa cevrilmis, kapaliysa orijinal silah gorunur.
//
// ZAMAN PENCERESI YOK: ayar her degistiginde "nesil" artar; her nesne, oyun onu
// BIR SONRAKI guncelledigi anda (ne zaman olursa olsun) bir kez yeniden taktirilir.
// Eskiden yalnizca ilk 6 saniye vardi; o sirada ekranda olmayan / guncellenmeyen
// karakterler kaciyordu. Nesne basina "bu nesilde islendi mi" kontrolu isaretci
// tabanli kucuk bir tabloyla O(1): her kare her nesne icin agir is yapilmaz.
static LONG  g_nesil = 0;
static DWORD g_nesilNesne[4096];
static LONG  g_nesilDeger[4096];
static DWORD g_nesilBekle[4096];   // != 0: bu zamana (GetTickCount) kadar tekrar deneme
static LONG  g_nesilYeniden = 0, g_nesilKayitYok = 0, g_nesilEcx = 0, g_nesilBos = 0;

static void GuncelIsle(void* ecx)
{
    static int sonDurum = -1;
    int simdi = (g_vnumDegis ? 1 : 0) | (g_kostumSilahGizle ? 2 : 0) | (g_alanYaz ? 4 : 0) |
                (g_zirhKostumGizle ? 8 : 0);
    if (sonDurum < 0) sonDurum = simdi;
    if (simdi != sonDurum)
    {
        Log("[GUNCEL] ayar degisti (nesil %ld -> %ld); onceki nesil ozeti: "
            "yeniden takilan=%ld, kayit yok=%ld, ecx uyusmaz=%ld, ekipmani bos=%ld",
            g_nesil, g_nesil + 1, g_nesilYeniden, g_nesilKayitYok, g_nesilEcx, g_nesilBos);
        g_nesilYeniden = g_nesilKayitYok = g_nesilEcx = g_nesilBos = 0;
        sonDurum = simdi;
        ++g_nesil;
    }
    if (!g_nesil || !ecx) return;

    DWORD anahtar = (DWORD)(DWORD_PTR)ecx;
    DWORD yuva = (anahtar >> 4) & 4095;
    if (g_nesilNesne[yuva] == anahtar && g_nesilDeger[yuva] == g_nesil) return;   // bu nesilde islendi
    // Gecici nedenle atlanan nesne (kayit yeni nesneye gecmemis / ekipman henuz
    // gelmemis) 250 ms sonra tekrar denenir; eskiden "islendi" sayilip bir sonraki
    // ac-kapaya kadar eski gorunumde kaliyordu.
    const DWORD simdiMs = GetTickCount();
    const bool ayniNesne = (g_nesilNesne[yuva] == anahtar);
    if (ayniNesne && g_nesilBekle[yuva] && (int)(simdiMs - g_nesilBekle[yuva]) < 0) return;
    const bool ilkDeneme = !(ayniNesne && g_nesilBekle[yuva]);
    g_nesilNesne[yuva] = anahtar;
    g_nesilDeger[yuva] = g_nesil;
    g_nesilBekle[yuva] = 0;

    // OLU KARAKTER: nesnede +0x77C = 1 (NeferTWETY ve oIALFAIo'da 1, 98 canli
    // karakterde 0; 15.11 dokumunden). Olu karakter yeniden takilinca T-poz ve
    // animasyon / ses bozuluyordu: atla, 250 ms sonra tekrar bak (dirilince takilir).
    {
        DWORD olu = 0;
        __try { olu = *(volatile DWORD*)((BYTE*)ecx + 0x77C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { olu = 0; }
        if (olu == 1)
        {
            if (ilkDeneme)
            {
                char adO[28]; ReadCharName(ecx, adO, sizeof(adO));
                Log("[GUNCEL] olu sayildi, atlandi: %s", adO[0] ? adO : "?");
            }
            g_nesilDeger[yuva] = 0;
            g_nesilBekle[yuva] = simdiMs + 250; if (!g_nesilBekle[yuva]) g_nesilBekle[yuva] = 1;
            return;
        }
    }

    int eq = -1;
    for (int i = 0; i < g_fnHookCount; ++i)
        if (g_equipRva && g_fnHook[i].rva == g_equipRva) eq = i;
    if (eq < 0 || !g_fnHook[eq].orig) return;
    int nb = g_fnArgBytes[eq];
    if (nb < 4 || nb > 16 || (nb & 3)) return;         // olculmedi / desteklenmiyor

    LvlCache* c = LvlcBul(ecx);
    if (!c)             { ++g_nesilKayitYok; return; }
    if (c->ecx != ecx)
    {
        if (ilkDeneme) ++g_nesilEcx;
        g_nesilDeger[yuva] = 0; g_nesilBekle[yuva] = simdiMs + 250; if (!g_nesilBekle[yuva]) g_nesilBekle[yuva] = 1;
        return;
    }
    if (c->nesil == g_nesil) return;
    // Silahi OLMAYAN karakterde de zirh yeniden takilabilsin diye silah sart degil.
    if (!c->silahHam && !c->zirhHam)
    {
        if (ilkDeneme) ++g_nesilBos;
        g_nesilDeger[yuva] = 0; g_nesilBekle[yuva] = simdiMs + 250; if (!g_nesilBekle[yuva]) g_nesilBekle[yuva] = 1;
        return;
    }
    // KARE BASINA SINIR: ayar degisince ~80 karakter ayni karede yeniden
    // takilirsa model yuklemeleri takilma / T-poz / LAG yapiyordu. Her 16 ms
    // penceresinde en fazla 4 karakter; kalanlar sonraki guncellemede takilir.
    {
        static DWORD pencere = 0; static int kullanilan = 0;
        if (simdiMs - pencere >= 16) { pencere = simdiMs; kullanilan = 0; }
        if (kullanilan >= 4)
        {
            g_nesilDeger[yuva] = 0; g_nesilBekle[yuva] = 0;   // sonraki cagrida tekrar dene
            return;
        }
        ++kullanilan;
    }
    c->nesil = g_nesil;
    ++g_nesilYeniden;
    {
        static volatile LONG yl = 0;
        if (InterlockedIncrement(&yl) <= 600)
            Log("[GUNCEL] takildi: %s (zirh %u)", c->nm, c->zirhHam);
    }

    DWORD ham = c->silahHam, kostum = c->kostumHam, zirh = c->zirhHam;

    // Silah kostumu bu karaktere bizim hook'tan gecmeden takilmis olabilir
    // (ALPHABANKK, xCanto: nesnede +A0C'de duruyor, kostumHam hep 0). O zaman
    // nesneden okuyup ayni yoldan (hooklu giris) yeniden verelim: gizleme acikken
    // cevrilir / gercek silah gosterilir.
    // vMUHTAR / ALPHABANKK / xCanto tipi: kostum equipfn ile verilmiyor, silah
    // modeli yuklenirken nesnenin +A0C alanindan okunuyor (kostum vnum'u orada,
    // paketteki yansitma silah yuvasiyla ayni deger). Gizleme aciksa alani sifirlayip
    // gercek silahi yeniden taktiririz; kapaliysa alani geri yazariz.
    if (!kostum)
    {
        DWORD k = 0;
        __try { k = *(volatile DWORD*)((BYTE*)ecx + 0xA0C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { k = 0; }
        if (k >= 40000 && k < 41000) c->a0c = k;
        if (c->a0c)
            NesneYaz(ecx, 0xA0C, g_kostumSilahGizle ? 0 : c->a0c);
    }

    // ZIRHI YENIDEN TAKTIRMA KAPALI (g_zirhZorla = 0).
    // Denendi (14.11): armorfn dogrudan cagrilinca model yeniden kuruluyor
    // ama animasyon durumu yenilenmiyor -> karakter hareket edene kadar
    // T-pozda kaliyor, birkac ac/kapadan sonra oyun dondu. armorfn alt
    // seviye bir fonksiyon; dogru giris, onu cagiran ve ardindan durumu
    // yenileyen ust fonksiyon. O bulunana kadar zirh ayarlari yalnizca yeni
    // yuklenen karakterde gecerli.
    //
    // YENI YOL: ust fonksiyon (+24E0940). Dort kosul da saglanmissa cagrilir:
    // hook kurulu, tek argumanli oldugu olculmus (4 bayt), this'in karakter
    // nesnesi oldugu dogrulanmis, "takili zirh" alani ogrenilmis. Fonksiyon
    // "zirh zaten ayni" deyip cikmasin diye once o alan sifirlanir.
    if (zirh && g_zirhUstRva && g_zirhUstThis && g_zirhAlan >= 0)
    {
        int uz = -1;
        for (int i = 0; i < g_fnHookCount; ++i)
            if (g_fnHook[i].rva == g_zirhUstRva) uz = i;
        if (uz >= 0 && g_fnHook[uz].orig && g_fnArgBytes[uz] == 4)
        {
            DWORD zarg[1] = { zirh };
            NesneYaz(ecx, g_zirhAlan, 0);
            ThisCagir(g_fnHook[uz].orig, ecx, zarg, 1);
        }
    }

    // SILAH: HOOKLU giristen cagir; cevirme / alan duzeltme kurallari aynen islesin.
    if (ham)
    {
        void* hooklu = g_exeBase + g_equipRva;
        DWORD args[4] = { ham, c->ekArg[0], c->ekArg[1], c->ekArg[2] };
        c->silahTakili = 0;                            // atlama devreye girmesin
        ThisCagir(hooklu, ecx, args, nb / 4);
        if (kostum)
        {
            args[0] = kostum;
            ThisCagir(hooklu, ecx, args, nb / 4);
        }
    }
    // Ayrintili satir yalniz ilk 10 nesne icin; ozet ayar degisince yazilir.
    if (InterlockedIncrement(&g_zorlananCagri) <= 10)
        Log("  [GUNCEL] %-16s yeniden takildi (zirh %u, silah %u, kostum %u)",
            c->nm, zirh, ham, kostum);
}

extern "C" DWORD __cdecl LogFnCall(int idx, void* ecx, void* a1, void* a2, void* a3,
                                   void* retAdr, DWORD* cerceve)
{
    DWORD yeniVnum = 0;   // 0 = degisiklik yok
    if (!g_ready || idx < 0 || idx >= g_fnHookCount) return 0;

    if (g_guncelRva && g_fnHook[idx].rva == g_guncelRva)
    {
        GuncelIsle(ecx);
        return 0;
    }

    // equipfn'in yigindan kac bayt arguman temizledigini BIR KEZ olc:
    // donus adresini OlcumDonus ile degistir; fonksiyon "ret N" ile oraya
    // donunce N hesaplanir ve asil adrese gecilir.
    if (cerceve && ((g_equipRva && g_fnHook[idx].rva == g_equipRva) ||
                    (g_armorRva && g_fnHook[idx].rva == g_armorRva) ||
                    (g_zirhUstRva && g_fnHook[idx].rva == g_zirhUstRva)))
    {
        if (g_fnArgBytes[idx] < 0)
        {
            if (InterlockedCompareExchange(&g_olcMesgul, 1, 0) == 0)
            {
                g_olcIdx = (DWORD)idx;
                g_olcRet = cerceve[1];
                g_olcEsp = (DWORD)(DWORD_PTR)&cerceve[1];
                cerceve[1] = (DWORD)(DWORD_PTR)&OlcumDonus;
            }
        }
        else
        {
            static volatile LONG yazildi[MAX_FNHOOKS] = {};
            if (InterlockedCompareExchange(&yazildi[idx], 1, 0) == 0)
                Log("[GUNCEL] exe+0x%08X arguman bayti olculdu: %d",
                    (DWORD)g_fnHook[idx].rva, g_fnArgBytes[idx]);
        }
    }

    // Zirhi degistiren UST fonksiyon: sadece dogrula (this karakter nesnesi mi)
    // ve ilk cagrilari yaz. Vnum'a burada dokunmuyoruz; cevirme armorfn'de.
    if (g_zirhUstRva && g_fnHook[idx].rva == g_zirhUstRva)
    {
        LvlCache* zc = ecx ? LvlcBul(ecx) : nullptr;
        if (zc && zc->ecx == ecx) InterlockedExchange(&g_zirhUstThis, 1);
        static volatile LONG zh = 0;
        if (InterlockedIncrement(&zh) <= 8)
        {
            char ad[28];
            ReadCharName(ecx, ad, sizeof(ad));
            Log("  [GUNCEL] zirh-ust cagrisi: %-16s arg=%u (this %s)",
                ad[0] ? ad : "?", (DWORD)(DWORD_PTR)a1,
                (zc && zc->ecx == ecx) ? "= karakter" : "bilinmiyor");
        }
        return 0;
    }

    // Silah fonksiyonu, guncelleme fonksiyonunun icindeki bilinen uc
    // noktadan (+24E8782 / +24E879A / +24E880B) cagrildiysa girisini bul.
    if (!g_guncelAday && g_equipRva && g_fnHook[idx].rva == g_equipRva &&
        g_exeBase && cerceve)
    {
        DWORD site = (DWORD)((BYTE*)retAdr - g_exeBase);
        if (site >= 0x024E8700 && site <= 0x024E8900)
        {
            static volatile LONG deneme = 0;
            LONG dn = InterlockedIncrement(&deneme);
            DWORD fEbp = cerceve[0], fRet = 0, a[4] = {};
            BYTE  op[8] = {};
            bool ok = fEbp > 0x10000 &&
                      PktDword((const BYTE*)(DWORD_PTR)fEbp + 4, &fRet) &&
                      fRet > (DWORD)(DWORD_PTR)g_exeBase + 0x1000 &&
                      fRet < (DWORD)(DWORD_PTR)g_exeBase + 0x5000000;
            DWORD giris = 0;
            if (ok)
            {
                for (int k = 0; k < 4; ++k)
                    PktDword((const BYTE*)(DWORD_PTR)fEbp + 8 + 4 * k, &a[k]);
                DWORD d0 = 0, d1 = 0;
                if (PktDword((const BYTE*)(DWORD_PTR)fRet - 8, &d0) &&
                    PktDword((const BYTE*)(DWORD_PTR)fRet - 4, &d1))
                {
                    memcpy(op, &d0, 4); memcpy(op + 4, &d1, 4);
                    if (op[3] == 0xE8)                       // call rel32
                    {
                        DWORD hedef = fRet + d1;
                        DWORD hr = hedef - (DWORD)(DWORD_PTR)g_exeBase;
                        DWORD pro = 0;
                        // Giris, cagri noktasini ICERMELI ve 55 8B EC ile baslamali.
                        if (hr <= site && site - hr < 0x3000 &&
                            PktDword((const BYTE*)(DWORD_PTR)hedef, &pro) &&
                            (pro & 0x00FFFFFF) == 0x00EC8B55)
                            giris = hr;
                    }
                }
            }
            if (giris)
            {
                g_guncelAday = giris;
                Log("  [GUNCEL] giris BULUNDU +%X (nokta +%X, cagiran donus +%X) "
                    "arg: %u %u %u %u",
                    giris, site, fRet - (DWORD)(DWORD_PTR)g_exeBase,
                    a[0], a[1], a[2], a[3]);
            }
            else if (dn <= 6)
                Log("  [GUNCEL] giris bulunamadi #%ld nokta=+%X ebp=%08X donus=%08X "
                    "komut=%02X %02X %02X %02X %02X %02X %02X %02X",
                    dn, site, fEbp, fRet,
                    op[0], op[1], op[2], op[3], op[4], op[5], op[6], op[7]);
        }
    }


    // --- ZIRH SETTER (log limitinden bagimsiz) ---
    if (g_armorRva && g_fnHook[idx].rva == g_armorRva && ecx && g_armorLog > 0)
    {
        DWORD v = (DWORD)(DWORD_PTR)a1;
        if (v >= 1000 && v < 100000)
        {
            LONG n = InterlockedIncrement(&g_armorHits);
            if (n <= g_armorLog)
            {
                char nm[28];
                ReadCharName(ecx, nm, sizeof(nm));
                const char* tur = "?";
                if      (v >= 11000 && v < 20000) tur = "GERCEK ZIRH";
                else if (v >= 20000 && v < 21000) tur = "binek/at";
                else if (v >= 41000 && v < 42000) tur = "ZIRH KOSTUMU";
                else if (v >= 45000 && v < 46000) tur = "aksesuar";
                Log("  [ZIRH #%ld] vnum=%-6u %-13s ecx=%08X  %s",
                    n, v, tur, (DWORD)(DWORD_PTR)ecx, nm[0] ? nm : "(isim yok)");
            }

            // Seviyeyi coz ve bu karakterin modelleri cozulurken
            // kullanilmak uzere pending'e yaz.
            {
                char nm3[28];
                ReadCharName(ecx, nm3, sizeof(nm3));
                int lvl = LevelFor(ecx, nm3, v);



                // === KARAKTER BAZLI DEGISTIRME ===
                // Model dosyasini degil, kurucuya giden VNUM'u
                // degistiriyoruz. Bu cagri her karakter icin ayri
                // yapildigi icin herkese kendi seviyesine uyan zirh
                // verilebiliyor; dosya yonlendirmesinde bu mumkun
                // degildi (ayni dosya herkese gidiyordu).
                // Bu karakterin oyunun verdigi zirhini hatirla (panelden
                // ayar degisince yeniden taktirmak icin).
                {
                    LvlCache* zc = LvlcBul(ecx);
                    if (zc && zc->ecx == ecx)
                    {
                        zc->zirhHam    = v;
                        zc->zirhArg[0] = (DWORD)(DWORD_PTR)a2;
                        zc->zirhArg[1] = (DWORD)(DWORD_PTR)a3;
                        zc->zirhArg[2] = cerceve ? cerceve[5] : 0;
                    }
                }

                // TANILAMA: armorfn'i cagiran UST fonksiyonun girisi ve
                // argumanlari. Zirhi aninda degistirmek icin armorfn degil
                // o fonksiyon cagrilmali (animasyon durumunu da yeniliyor).
                // Cagiranin cercevesinden donus adresini alip oncesindeki
                // "call" komutunun hedefini okuyoruz.
                if (g_exeBase && cerceve && retAdr)
                {
                    static DWORD gorulen[8]; static int gorulenN = 0;
                    DWORD site = (DWORD)((BYTE*)retAdr - g_exeBase);
                    bool var = false;
                    for (int i = 0; i < gorulenN; ++i) if (gorulen[i] == site) var = true;
                    if (!var && gorulenN < 8)
                    {
                        gorulen[gorulenN++] = site;
                        DWORD fEbp = cerceve[0], fRet = 0, d0 = 0, d1 = 0, a[4] = {};
                        DWORD giris = 0;
                        if (fEbp > 0x10000 &&
                            PktDword((const BYTE*)(DWORD_PTR)fEbp + 4, &fRet) &&
                            fRet > (DWORD)(DWORD_PTR)g_exeBase + 0x1000 &&
                            fRet < (DWORD)(DWORD_PTR)g_exeBase + 0x5000000 &&
                            PktDword((const BYTE*)(DWORD_PTR)fRet - 8, &d0) &&
                            PktDword((const BYTE*)(DWORD_PTR)fRet - 4, &d1))
                        {
                            for (int k = 0; k < 4; ++k)
                                PktDword((const BYTE*)(DWORD_PTR)fEbp + 8 + 4 * k, &a[k]);
                            if ((d0 >> 24) == 0xE8)
                                giris = fRet + d1 - (DWORD)(DWORD_PTR)g_exeBase;
                        }
                        Log("  [GUNCEL] zirh cagirani: nokta=+%X ust-giris=+%X "
                            "(nokta-giris=%d) ust-arg: %u %u %u %u  zirh=%u",
                            site, giris, (int)(site - giris), a[0], a[1], a[2], a[3], v);

                        // Ust fonksiyonun ilk argumani bu zirh vnum'uysa ve
                        // giris cagri noktasini iceriyorsa: aradigimiz fonksiyon.
                        DWORD pro = 0;
                        if (!g_zirhUstAday && giris && a[0] == v &&
                            giris <= site && site - giris < 0x2000 &&
                            PktDword(g_exeBase + giris, &pro) &&
                            (pro & 0x00FFFFFF) == 0x00EC8B55)
                            g_zirhUstAday = giris;
                    }
                }

                // Zirh kostumu (41xxx) "Zirh kostumunu gizle"ye, gercek zirh
                // "Klasik gorunum"e baglidir: ikisi birbirinden bagimsiz.
                // Klasik kapaliyken kostum gizleniyorsa altindaki zirh bilinmedigi
                // icin seviyeye uygun klasik zirh gosterilir.
                bool zKostum = (v >= 41000 && v < 42000);
                bool zYap    = zKostum ? (g_zirhKostumGizle != 0) : (g_vnumDegis != 0);

                if (zYap && lvl > 0)
                {
                    int gercek = ATabanOf((int)v);
                    const char* cls = AClassOf(gercek);
                    int zlvl = ALevelOf(gercek);

                    // Sinif tabloda yoksa karakterin kendi sinifindan.
                    char clsBuf[16] = {};
                    if (!cls)
                    {
                        EnterCriticalSection(&g_lock);
                        CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, false);
                        if (e && e->cls[0]) strcpy_s(clsBuf, e->cls);
                        LeaveCriticalSection(&g_lock);
                        if (clsBuf[0]) cls = clsBuf;
                    }

                    // Imkansiz seviye (silahtaki kuralin aynisi): gercek
                    // zirhin seviyesi oyuncununkinden buyuk olamaz.
                    if (gercek < 30000 && zlvl > 0 && zlvl > lvl)
                    {
                        lvl = zlvl;
                        LvlcSeviyeTaban(ecx, zlvl);
                    }

                    if (cls)
                    {
                        int sinir = lvl < g_maxLevel ? lvl : g_maxLevel;
                        if (sinir > g_capLevel) sinir = g_capLevel;
                        // zlvl < 0: tabloda olmayan zirh (zodyak, yeni
                        // esya). Seviyesini bilmiyoruz, kurala gore
                        // seviyesinin izin verdigi EN YUKSEK zirhi
                        // veriyoruz.
                        if (zlvl > sinir || zlvl < 0)
                        {
                            int hedef = ATargetFor(cls, sinir);
                            if (hedef > 0 && hedef != gercek)
                            {
                                yeniVnum = (DWORD)hedef;
                                if (g_eslemeLog > 0 &&
                                    InterlockedIncrement(&g_degisHits) <= 200000)
                                    Log("  [ZIRH>>] %-16s sv=%-4d %u -> %u "
                                        "(zirh sv=%d, sinir %d)",
                                        nm3, lvl, v, yeniVnum, zlvl, sinir);
                            }
                        }
                    }
                }

                // "Takili zirh" alanini ogren: armorfn cevrilmis vnum'la
                // donunce nesnede o degere donusen alan (hicbir sey yazilmaz).
                if (yeniVnum && g_zirhAlan < 0 && cerceve &&
                    cerceve[1] != (DWORD)(DWORD_PTR)&OlcumDonus &&
                    InterlockedCompareExchange(&g_sonMesgul, 1, 0) == 0)
                {
                    g_sonMod = 1;
                    g_sonOnceN = NesneKopyala(ecx, g_sonOnce);
                    g_sonEcx = ecx;
                    g_sonV   = v;
                    g_sonT   = yeniVnum;
                    g_sonRet = cerceve[1];
                    cerceve[1] = (DWORD)(DWORD_PTR)&SonraDonus;
                }

                g_pendLevel = (LONG)lvl;
                g_pendArmor = (LONG)v;
                g_pendEcx   = ecx;

                // ZIRH ESLEME: vnum -> model dosyasi eslemesini oyundan
                // ogreniyoruz. Zirh modeli vnum'dan hesaplanamiyor
                // (pc\<sinif>\<ad>.gr2 ve adlar tutarsiz), o yuzden
                // gozlemle tablo kuruyoruz. Model satiri hemen sonra gelir.
                if (g_eslemeLog > 0 &&
                    InterlockedIncrement(&g_eslemeHits) <= g_eslemeLog)
                {
                    int zb = (int)v - ((int)v % 10);
                    Log("  [ZIRH] %-16s sv=%-4d vnum=%-6u taban=%-6d tablo-sv=%d",
                        nm3[0] ? nm3 : "?", lvl, v, zb, ALevelOf(zb));
                }
            }
        }
    }

    // --- EKIPMAN ESLEMESI (log limitinden bagimsiz, her zaman calisir) ---
    if (g_equipRva && g_fnHook[idx].rva == g_equipRva && ecx)
    {
        DWORD vnum = (DWORD)(DWORD_PTR)a1;
        if (vnum >= 10 && vnum < 100000)
        {
            char model[8];
            sprintf_s(model, "%05u", (vnum / 10) * 10);

            // TANILAMA (gecici): kostumu gizlenmeyen karakterlerin tum silah
            // cagrilari, hicbir kosul uygulanmadan.
            {
                static volatile LONG iz2 = 0;
                if (iz2 < 300)
                {
                    char ad2[28]; ReadCharName(ecx, ad2, sizeof(ad2));
                    if (strcmp(ad2, "ALPHABANKK") == 0 || strcmp(ad2, "xCanto") == 0 || strcmp(ad2, "vMUHTAR") == 0)
                    {
                        LvlCache* c2 = LvlcBul(ecx);
                        Log("[IZLE2 %ld] %s vnum=%u nokta=+%X kayit=%s ecx=%s ham=%u kostumHam=%u klasik=%d silahGizle=%d",
                            InterlockedIncrement(&iz2), ad2, vnum,
                            (DWORD)((BYTE*)retAdr - g_exeBase),
                            c2 ? "var" : "YOK", (c2 && c2->ecx == ecx) ? "ayni" : "FARKLI",
                            c2 ? c2->silahHam : 0, c2 ? c2->kostumHam : 0,
                            (int)g_vnumDegis, (int)g_kostumSilahGizle);
                        // Ek argumanlar + nesne icinde kostum vnum'u (40000-41999) nerede?
                        static DWORD tamp2[SON_BOY / 4];
                        int n2 = NesneKopyala(ecx, tamp2);
                        char bul[200] = {}; int bl = 0;
                        for (int i = 0; i < n2 && bl < 170; ++i)
                            if (tamp2[i] >= 40000 && tamp2[i] < 42000)
                                bl += sprintf_s(bul + bl, sizeof(bul) - bl, " +%X=%u", i * 4, tamp2[i]);
                        Log("[IZLE2 arg] %s a2=%u a3=%u a4=%u | nesnede kostum:%s", ad2,
                            (DWORD)(DWORD_PTR)a2, (DWORD)(DWORD_PTR)a3,
                            cerceve ? cerceve[5] : 0, bul[0] ? bul : " yok");
                    }
                }
            }

            // --- HAM EKIPMAN DOKUMU ---
            // vnum = taban + parlaklik. taban esyayi, son hane
            // +kac oldugunu veriyor. Silah mi zirh mi, aile
            // tablosundan anlasiliyor.
            if (g_equipLog > 0)
            {
                LONG en = InterlockedIncrement(&g_equipLogHits);
                if (en <= g_equipLog)
                {
                    char nm[28];
                    ReadCharName(ecx, nm, sizeof(nm));
                    int  fam = WeaponFamily(model);
                    Log("  [EKIP #%ld] vnum=%-6u taban=%s +%u  aile=%-2d  %s",
                        en, vnum, model, vnum % 10, fam,
                        nm[0] ? nm : "(isim yok)");
                }
            }

            if (vnum >= 40000 && vnum < 41000)
            {
                // SILAH kostumu: sahibi bu nesne. Ailesini simdi sabitle.
                // 41xxx ve ustu zirh kostumu; bekleyen kaydi bozmasin diye
                // disarida birakiyoruz.
                EnterCriticalSection(&g_lock);
                CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, false);
                g_pendFam = e ? e->fam : -1;
                if (e && e->weapon[0]) strcpy_s(g_pendWeapon, e->weapon);
                else                   g_pendWeapon[0] = '\0';
                LeaveCriticalSection(&g_lock);
                ReadCharName(ecx, g_pendName, sizeof(g_pendName));
                g_pendCostume = (LONG)vnum;
                g_pendLevel   = (LONG)LevelCached(ecx, g_pendName);

                // === KOSTUM SILAHI: KARAKTER BAZLI ===
                // Eskiden kostum DOSYASI yonlendiriliyordu. Istemci dosyayi
                // yola gore onbellege aldigi icin ayni kostumu giyen herkes,
                // onu ilk giyenin seviyesine/ailesine gore secilmis silahi
                // goruyordu (62. seviyede Triton; ayni kostum bir yerde
                // Iblisin Yelpazesi, baska yerde duz Yelpaze).
                // Artik kostum vnum'u burada, BU karakterin seviyesine ve
                // kostumun proto ailesine gore klasik silaha cevriliyor.
                // Ayni karakter her gorunuste ayni silahi alir; kostum
                // dosyasi hic istenmez, dolayisiyla onbellek karismaz.
                if (g_kostumSilahGizle && !g_vnumDegis)
                {
                    // KLASIK GORUNUM KAPALI + KOSTUMU GIZLE: kostumun yerine
                    // karakterin GERCEK silahi (oyunun verdigi, cevrilmemis
                    // vnum) gosterilir. Ikisi birbirinden bagimsiz calisir.
                    // Gercek silah bilinmiyorsa (sadece kostum giyili) kostume dokunma.
                    LvlCache* kc = LvlcBul(ecx);
                    DWORD ham = (kc && kc->ecx == ecx) ? kc->silahHam : 0;
                    if (ham)
                    {
                        yeniVnum = ham;
                        g_pendCostume = 0;
                        if (g_eslemeLog > 0 &&
                            InterlockedIncrement(&g_degisHits) <= 200000)
                            Log("  [ESYA>>] %-16s kostum %u -> gercek silah %u",
                                g_pendName[0] ? g_pendName : "?", vnum, ham);
                    }
                }
                else if (g_vnumDegis && g_kostumSilahGizle)
                {
                    int sv = (int)g_pendLevel;
                    if (!sv)
                    {
                        sv = LevelFor(ecx, g_pendName, 0);
                        g_pendLevel = (LONG)sv;
                    }
                    int kf = KSilFam((int)vnum);     // proto Value3
                    if (kf < 0) kf = g_pendFam;      // yoksa karakterin bilinen ailesi
                    if (sv > 0 && kf >= 0 && kf <= 6)
                    {
                        int sinir = sv < g_maxLevel ? sv : g_maxLevel;
                        if (sinir > g_capLevel) sinir = g_capLevel;
                        MaskeyiKarakterdenAl(ecx);
                        int hs = WTargetFor(kf, sinir);

                        // GERCEK SILAHLA AYNI VNUM (ANIMASYON DUZELTMESI):
                        // Istemci her guncellemede once gercek silahi, sonra
                        // kostumu veriyor (+24E8480 fonksiyonu, uc cagri).
                        // Biz gercek silahi 279'a (+9 korunuyor), kostumu
                        // 270'e ceviriyorduk: ikisi de silah yuvasina gittigi
                        // icin istemci her guncellemede 279 <-> 270 arasinda
                        // silahi sokup takiyor, animasyon basa sariyordu.
                        // Bu karaktere az once verilen silah ayni ailedense
                        // kostum cagrisi da BIREBIR onu alir; ikinci cagri
                        // "zaten ustunde" olur ve hicbir sey degismez.
                        DWORD onceki = LvlcSilahOku(ecx);
                        if (onceki)
                        {
                            int ob = WTabanOf((int)onceki);
                            if (WFamOf(ob) == kf) hs = (int)onceki;
                        }

                        if (hs > 0)
                        {
                            yeniVnum = (DWORD)hs;
                            g_pendCostume = 0;       // dosya kanali devreye girmesin
                            if (g_eslemeLog > 0 &&
                                InterlockedIncrement(&g_degisHits) <= 200000)
                                Log("  [ESYA>>] %-16s sv=%-4d kostum %u -> %d (%s, aile %d, sinir %d)",
                                    g_pendName[0] ? g_pendName : "?", sv,
                                    vnum, hs, IsimBul(hs), kf, sinir);
                        }
                        else
                            Log("  .. [kostum] %-16s vnum=%u CEVRILEMEDI: hedef silah yok "
                                "(sv=%d aile=%d sinif maskesi=%d)",
                                g_pendName[0] ? g_pendName : "?", vnum, sv, kf, t_sinifMaske);
                    }
                    else
                        Log("  .. [kostum] %-16s vnum=%u CEVRILEMEDI: sv=%d aile=%d",
                            g_pendName[0] ? g_pendName : "?", vnum, sv, kf);
                }
            }
            else
            {
                // Bu cagri da bir karaktere ait: seviyeyi ONUN nesnesinden
                // tazele. Yoksa silah kirpmasi bir onceki karakterin
                // seviyesiyle hesaplaniyordu (9. seviyede 90. seviye yay).
                ReadCharName(ecx, g_pendName, sizeof(g_pendName));
                {
                    int sv = LevelCached(ecx, g_pendName);
                    if (!sv) sv = LevelFor(ecx, g_pendName, (DWORD)g_pendArmor);
                    g_pendLevel = (LONG)sv;
                }

                // mod_silahsv.txt artik YAZILMIYOR: olculen seviyeler hicbir
                // karara girmiyordu (tablo tek yetkili), dosya her oturumda
                // ayni silah icin satir satir buyuyordu.

                if (g_eslemeLog > 0 &&
                    InterlockedIncrement(&g_eslemeHits) <= g_eslemeLog * 3)
                {
                    int sb = (int)vnum - ((int)vnum % 10);
                    Log("  [SILAH] %-16s sv=%-4ld vnum=%-6u taban=%-6d tablo-sv=%d",
                        g_pendName[0] ? g_pendName : "?", g_pendLevel,
                        vnum, sb, WLevelOf(sb));
                }

                int fam = WeaponFamily(model);

                // === KARAKTER BAZLI DEGISTIRME (silah) ===
                g_pendEsya = (LONG)vnum;

                // Tablolarimizda HIC olmayan esya vnum'lari: ne silah
                // ne zirh olarak taniyoruz, o yuzden dokunamiyoruz.
                // Bir kez logla, karsiligini item database'den ogrenip
                // tabloya ekleyelim.
                if (vnum >= 20000 && WLevelOf((int)vnum - ((int)vnum % 10)) < 0 &&
                    !AClassOf((int)vnum - ((int)vnum % 10)) &&
                    !(vnum >= 21900 && vnum < 22000))
                {
                    int bv = (int)vnum - ((int)vnum % 10);
                    static int bilinen[64]; static int bilinenN = 0;
                    bool var = false;
                    for (int i = 0; i < bilinenN; ++i)
                        if (bilinen[i] == bv) { var = true; break; }
                    if (!var && bilinenN < 64)
                    {
                        bilinen[bilinenN++] = bv;
                        Log("  [BILINMEYEN ESYA] vnum=%u taban=%d  %s (sv=%ld)",
                            vnum, bv, g_pendName[0] ? g_pendName : "?", g_pendLevel);
                    }
                }

                // IMKANSIZ SEVIYE: logda oyuna giriste bir grup oyuncu sv=1
                // okundu (Zodyak kilicli, 105'lik zirhli oyuncular) ve
                // hepsine baslangic silahi verildi; birkac saniye sonra
                // dogru seviye gelince silah yeniden degisti. Klasik bir
                // silahin seviyesi oyuncunun seviyesinden buyuk olamaz;
                // okunan seviye kucukse silahin seviyesini alt sinir al.
                {
                    int sbT = WTabanOf((int)vnum);
                    int svT = WLevelOf(sbT);
                    if (WKlasik(sbT) && svT > 0 && g_pendLevel > 0 &&
                        svT > (int)g_pendLevel)
                    {
                        g_pendLevel = (LONG)svT;
                        LvlcSeviyeTaban(ecx, svT);
                    }
                }

                if (g_vnumDegis && g_pendLevel > 0)
                {
                    int sinir = (int)g_pendLevel < g_maxLevel
                              ? (int)g_pendLevel : g_maxLevel;
                    if (sinir > g_capLevel) sinir = g_capLevel;

                    // --- YANSITMA ESYALARI ---
                    // 21900-21909 : SILAH yansitmasi, son hane aile
                    //               (0 kilic 1 hancer 2 yay 3 ciftel 4 can)
                    // 21910-21999 : ZIRH yansitmasi. Bunlari silah sanip
                    //               kilica cevirdigimiz icin karakterler
                    //               T-poza giriyor ve zirhlar tavanin
                    //               ustunde kaliyordu.
                    if (false)
                    {
                        // SILAH yansitmasi (paket dokumu: 21920 silah
                        // yuvasinda, +112'de cikti). Zirha cevirmek
                        // T-poza sebep oluyordu.
                        int sf2 = -1;
                        EnterCriticalSection(&g_lock);
                        CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, false);
                        if (e) sf2 = e->fam;
                        LeaveCriticalSection(&g_lock);
                        if (sf2 >= 0 && sf2 <= 6)
                        {
                            int t = WTargetFor(sf2, sinir);
                            if (t > 0) yeniVnum = (DWORD)t;
                        }
                    }
                    else
                    {
                        int ownFam = fam;   // esyanin KENDI ailesi (vnum'dan)
                        if (fam < 0)
                        {
                            EnterCriticalSection(&g_lock);
                            CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, false);
                            if (e) fam = e->fam;
                            LeaveCriticalSection(&g_lock);
                        }

                        MaskeyiKarakterdenAl(ecx);
                        int sb  = WTabanOf((int)vnum);
                        int ref = (int)vnum - sb;
                        int tf  = WFamOf(sb);
                        if (tf >= 0) fam = tf;

                        // ARAC (kazma 29101, olta vb.): silah tablosunda yok ve
                        // vnum'dan da aile cikmiyor. Eskiden karakterin kendi
                        // ailesine dusup klasik silaha cevriliyordu (kazma
                        // yerine kilic gorunuyordu). Yansitma (219xx) haric.
                        bool arac = (tf < 0 && ownFam < 0 &&
                                     !(vnum >= 21900 && vnum < 22000));

                        if (!arac && fam >= 0 && fam <= 6)
                        {
                            int svS = WLevelOf(sb);
                            int hedef = -1;
                            if (!WKlasik(sb))
                            {
                                // Ozel seri / basilamayan silah: her zaman
                                // klasik karsiligi. Silahin kendi seviyesi
                                // de sinir (21920 sv20 -> sv20'ye kadar).
                                int s2 = (svS >= 0 && svS < sinir) ? svS : sinir;
                                hedef = WTargetFor(fam, s2);
                                ref = OzelRef(sb);
                            }
                            else if (svS > sinir || svS < 0)
                                hedef = WTargetFor(fam, sinir);
                            if (hedef > 0 && hedef != sb)
                                yeniVnum = (DWORD)(hedef + ref);
                        }
                    }

                    if (yeniVnum && g_eslemeLog > 0 &&
                        InterlockedIncrement(&g_degisHits) <= 200000)
                        Log("  [ESYA>>] %-16s sv=%-4ld %u -> %u (sinir %d)",
                            g_pendName[0] ? g_pendName : "?", g_pendLevel,
                            vnum, yeniVnum, sinir);
                }

                // Bu karaktere fiilen giden silah vnum'unu hatirla (kostum
                // cagrisi ayni vnum'u kullansin diye).
                // SADECE karar verilebildiyse (seviye biliniyor). Karar
                // verilemeyen cagride orijinal vnum'u yazinca kostum de
                // orijinal silaha (Zodyak kilic vb.) donuyordu.
                if (fam >= 0 && fam <= 6 && g_vnumDegis && g_pendLevel > 0)
                {
                    int sbK = WTabanOf((int)(yeniVnum ? yeniVnum : vnum));
                    int svK = WLevelOf(sbK);
                    bool uygun = WKlasik(sbK) && svK >= 0 && svK <= g_capLevel;
                    LvlcSilahYaz(ecx, uygun ? (yeniVnum ? yeniVnum : vnum) : 0);
                }
                else
                    LvlcSilahYaz(ecx, 0);

                if (fam >= 0)
                {
                    // Gercek silah: bu nesnenin ailesini guncelle.
                    EnterCriticalSection(&g_lock);
                    CharEntry* e = FindChar((DWORD)(DWORD_PTR)ecx, true);
                    e->fam = fam;
                    strcpy_s(e->weapon, model);
                    LeaveCriticalSection(&g_lock);
                }
            }

            // === TEKRARLAYAN CAGRIYI ATLA (animasyon titremesi) ===
            // Bu cagri guncelleme fonksiyonunun icinden geliyorsa ve bu
            // karaktere zaten AYNI silahi verdiysek, silahi yeniden
            // taktirmanin anlami yok: cagriyi hic yapma, 0 dondur.
            {
                DWORD cikis = yeniVnum ? yeniVnum : vnum;
                DWORD site = (g_exeBase && retAdr)
                           ? (DWORD)((BYTE*)retAdr - g_exeBase) : 0;
                bool gNokta = site >= 0x024E8700 && site <= 0x024E8900;
                bool kostumV = (vnum >= 40000 && vnum < 41000);
                LvlCache* c = LvlcBul(ecx);
                if (c && c->ecx == ecx)
                {
                    int nb = g_fnArgBytes[idx];
                    // TANILAMA (gecici): SEYITHAN'in her silah cagrisini sirayla yaz.
                    static volatile LONG izN = 0;
                    const bool izle = (strcmp(c->nm, "vMUHTAR") == 0 || strcmp(c->nm, "SEYITHAN") == 0) && izN < 400;
                    const bool atla = gNokta && g_vnumDegis && c->silahTakili == cikis &&
                                      nb >= 4 && nb <= 16 && !(nb & 3);
                    if (izle)
                        Log("[IZLE %ld] vnum=%u cikis=%u %s nokta=+%X takili=%u ham=%u kostumHam=%u cikisKayit=%u klasik=%d silahGizle=%d",
                            InterlockedIncrement(&izN), vnum, cikis, atla ? "ATLANDI" : "verildi",
                            site, c->silahTakili, c->silahHam, c->kostumHam, c->silahCikis,
                            (int)g_vnumDegis, (int)g_kostumSilahGizle);
                    if (atla)
                    {
                        InterlockedIncrement(&g_atlananCagri);
                        return 0xFFFFFFFF;          // trambolin: cagriyi atla
                    }
                    c->silahTakili = cikis;
                    // Yalnizca SILAH vnum'lari "son silah" sayilir (bu
                    // fonksiyona zirh / aksesuar vnum'u da gelebilir).
                    if (kostumV) c->kostumHam = vnum;
                    // Tabloda olmayan klasik silah (SEYITHAN'in 393'u gibi) da sayilir:
                    // silahHam 0 kalinca kostum cagrisi "takili silah" alanina gercek
                    // silahi yazamiyor (titreme) ve kostum gizleme / yeniden takma
                    // bu karakterde hic calismiyordu.
                    else if (WLevelOf(WTabanOf((int)vnum)) >= 0 || vnum < 10000) c->silahHam = vnum;
                    c->ekArg[0] = (DWORD)(DWORD_PTR)a2;
                    c->ekArg[1] = (DWORD)(DWORD_PTR)a3;
                    c->ekArg[2] = cerceve ? cerceve[5] : 0;

                    // Vnum'u degistirdiysek: cagri donunce "takili silah"
                    // alanini oyunun verdigi degere (vnum) geri yaz.
                    // Bu cagrida olcum yapiliyorsa (donus adresi zaten
                    // degistirildi) bir sonrakine birak.
                    // KOSTUM cagrisinda geri yazilan deger kostum vnum'u
                    // DEGIL, gercek silahin vnum'u olmali. Alana kostum
                    // numarasi yazinca oyun silah turunu ondan cikarmaya
                    // calisiyor, bulamiyor ve karakter T-pozda kaliyordu
                    // (ozellikle yeni yuklenen kostumlu karakterlerde).
                    // Gercek silah bilinmiyorsa alana hic dokunmuyoruz.
                    DWORD geriV = kostumV ? c->silahHam : vnum;
                    if (g_alanYaz && g_vnumDegis && yeniVnum && cikis != vnum &&
                        geriV && geriV != cikis &&
                        cerceve && nb >= 4 &&
                        cerceve[1] != (DWORD)(DWORD_PTR)&OlcumDonus &&
                        InterlockedCompareExchange(&g_sonMesgul, 1, 0) == 0)
                    {
                        g_sonMod = 0;
                        g_sonOnceN = NesneKopyala(ecx, g_sonOnce);
                        g_sonEcx = ecx;
                        g_sonV   = geriV;
                        g_sonT   = cikis;
                        g_sonRet = cerceve[1];
                        cerceve[1] = (DWORD)(DWORD_PTR)&SonraDonus;
                    }
                }
            }
        }
        else
        {
            // Silah cikarildi / gecersiz vnum: "takili" kaydi artik gecersiz.
            LvlCache* c = LvlcBul(ecx);
            if (c && c->ecx == ecx) c->silahTakili = 0;
        }
    }

    return yeniVnum;
}
#define MAKE_FN_HOOK(IDX)                       \
__declspec(naked) static void FnHook##IDX()     \
{                                               \
    __asm push ebp                              \
    __asm mov  ebp, esp                         \
    __asm pushad                                \
    __asm pushfd                                \
    __asm push ebp                              \
    __asm mov  eax, [ebp+4]                     \
    __asm push eax                              \
    __asm mov  eax, [ebp+16]                    \
    __asm push eax                              \
    __asm mov  eax, [ebp+12]                    \
    __asm push eax                              \
    __asm mov  eax, [ebp+8]                     \
    __asm push eax                              \
    __asm push ecx                              \
    __asm push IDX                              \
    __asm call LogFnCall                        \
    __asm add  esp, 28                          \
    __asm cmp  eax, 0FFFFFFFFh                  \
    __asm je   atla                             \
    __asm mov  edx, [ebp+8]                     \
    __asm test eax, eax                         \
    __asm cmovne edx, eax                       \
    __asm mov  [ebp+8], edx                     \
    __asm popfd                                 \
    __asm popad                                 \
    __asm mov  esp, ebp                         \
    __asm pop  ebp                              \
    __asm jmp  [g_fnHook + IDX*12 + 4]          \
    __asm atla:                                 \
    __asm popfd                                 \
    __asm popad                                 \
    __asm mov  esp, ebp                         \
    __asm pop  ebp                              \
    __asm pop  edx                              \
    __asm add  esp, [g_fnArgBytes + IDX*4]      \
    __asm xor  eax, eax                         \
    __asm jmp  edx                              \
}

MAKE_FN_HOOK(0)
MAKE_FN_HOOK(1)
MAKE_FN_HOOK(2)
MAKE_FN_HOOK(3)
MAKE_FN_HOOK(4)
MAKE_FN_HOOK(5)

static void* const g_fnDetours[MAX_FNHOOKS] = {
    &FnHook0, &FnHook1, &FnHook2, &FnHook3, &FnHook4, &FnHook5
};

// Bulunan guncelleme fonksiyonu girisine hook kur. HotkeyThread'den
// cagrilir (oyunun cagri zinciri disinda). Bir kez denenir.
static void GuncelKur()
{
    DWORD rva = g_guncelAday;
    if (!rva || g_guncelRva) return;
    if (InterlockedCompareExchange(&g_guncelDurum, 1, 0) != 0) return;
    if (g_fnHookCount >= MAX_FNHOOKS)
    { Log("[GUNCEL] hook yeri kalmadi"); return; }

    int i = g_fnHookCount;
    void* fn = g_exeBase + rva;
    BYTE* b = (BYTE*)fn;
    if (!IsReadable(fn, 8) || !(b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC))
    { Log("[GUNCEL] +%X prologue uyusmadi, hook KURULMADI", rva); return; }

    g_fnHook[i].rva   = rva;
    g_fnHook[i].orig  = nullptr;
    g_fnHook[i].count = 0;
    if (MH_CreateHook(fn, g_fnDetours[i], &g_fnHook[i].orig) != MH_OK)
    { Log("[GUNCEL] +%X hook olusturulamadi", rva); return; }
    g_fnHookCount = i + 1;          // once sayac: detour cagrilinca gecerli olsun
    g_guncelRva   = rva;
    if (MH_EnableHook(fn) == MH_OK)
        Log("[GUNCEL] hook AKTIF exe+0x%08X", rva);
    else
    { g_guncelRva = 0; Log("[GUNCEL] +%X hook etkinlestirilemedi", rva); }
    LogFlush();
}

// Zirhi degistiren ust fonksiyonun hook'u (ayni yontem). Bir kez denenir.
static void ZirhUstKur()
{
    DWORD rva = g_zirhUstAday;
    if (!rva || g_zirhUstRva) return;
    if (InterlockedCompareExchange(&g_zirhUstDurum, 1, 0) != 0) return;
    if (g_fnHookCount >= MAX_FNHOOKS)
    { Log("[GUNCEL] zirh-ust: hook yeri kalmadi"); return; }

    int i = g_fnHookCount;
    void* fn = g_exeBase + rva;
    BYTE* b = (BYTE*)fn;
    if (!IsReadable(fn, 8) || !(b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC))
    { Log("[GUNCEL] zirh-ust +%X prologue uyusmadi, hook KURULMADI", rva); return; }

    g_fnHook[i].rva   = rva;
    g_fnHook[i].orig  = nullptr;
    g_fnHook[i].count = 0;
    if (MH_CreateHook(fn, g_fnDetours[i], &g_fnHook[i].orig) != MH_OK)
    { Log("[GUNCEL] zirh-ust +%X hook olusturulamadi", rva); return; }
    g_fnHookCount = i + 1;
    g_zirhUstRva  = rva;
    if (MH_EnableHook(fn) == MH_OK)
        Log("[GUNCEL] zirh-ust hook AKTIF exe+0x%08X", rva);
    else
    { g_zirhUstRva = 0; Log("[GUNCEL] zirh-ust +%X hook etkinlestirilemedi", rva); }
    LogFlush();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ZIRH / KARAKTER MODELLERINDE HEP EN IYI KALITE (LOD0)
// Oyun uzaktaki karakterler icin "<model>_lod_01.gr2", "_lod_02.gr2" gibi cok
// dusuk poligonlu dosyalar istiyor. Mesafe ayari oyunun icinde var, bu yuzden
// bu dosyalara gerek yok: sonuc yolunda "_lod_NN" eki varsa atilir ve ana
// (LOD0) model verilir. Yalnizca pc / pc2 / pc3 altinda, sinif klasorunun
// HEMEN altindaki .gr2 dosyalari (govde/zirh) etkilenir; hair\ ve animasyon
// alt klasorleri, npc, esya dokunulmaz. Hem cevrilmis hem cevrilmemis
// yollar icin gecerli (cevirme sonucu da LOD eki tasiyabilir).
// lodkalite = 0 (config) ile kapatilir.
static __declspec(thread) char t_lodBuf[PATH_LEN];

static const char* LodKaldir(const char* yol)
{
    size_t n = strlen(yol);
    if (n < 16 || n >= PATH_LEN) return yol;
    if (_stricmp(yol + n - 4, ".gr2") != 0) return yol;
    const char* l = yol + n - 11;                       // "_lod_NN.gr2" = 11 karakter
    if (strncmp(l, "_lod_", 5) != 0 || !IsDigit(l[5]) || !IsDigit(l[6])) return yol;

    char norm[PATH_LEN];
    NormalizeInto(yol, norm, sizeof(norm));
    const char* pc = strstr(norm, "/pc/");
    if (!pc) pc = strstr(norm, "/pc2/");
    if (!pc) pc = strstr(norm, "/pc3/");
    if (!pc) return yol;
    const char* a = strchr(pc + 1, '/');
    if (!a) return yol;
    const char* b = strchr(a + 1, '/');
    if (!b || strchr(b + 1, '/')) return yol;           // alt klasor (hair, animasyon) degil

    memcpy(t_lodBuf, yol, n - 11);
    strcpy_s(t_lodBuf + (n - 11), sizeof(t_lodBuf) - (n - 11), ".gr2");
    static volatile LONG lodSayac = 0;
    if (InterlockedIncrement(&lodSayac) <= 4)
        Log("  [LOD] %s  ==>  %s", yol, t_lodBuf);
    return t_lodBuf;
}

extern "C" const char* __cdecl ResolveFileName(const char* original)
{
    const char* r = ResolveFileNameIc(original);
    if (!g_lodEnKalite || !g_ready || !r || !IsReadable(r, 1)) return r;
    return LodKaldir(r);
}

__declspec(naked) static void Hook_PackGet()
{
    __asm
    {
        push ebp
        mov  ebp, esp
        pushad
        pushfd

        mov  eax, [ebp + 12]
        push eax
        call ResolveFileName
        add  esp, 4
        mov[ebp + 12], eax

        popfd
        popad
        mov  esp, ebp
        pop  ebp
        jmp[g_origPackGet]
    }
}

static int g_erkenBekleme = 0;

#include "giris_gorsel.h"

// Gomulu giris gorselini oyun klasorune cikarir (yoksa ya da boyutu farkliysa),
// boylece son kullanicinin ayrica dosya kopyalamasi gerekmez.
static void GirisGorseliCikar()
{
    char dir[MAX_PATH], yol[MAX_PATH];
    sprintf_s(dir, "%smod_giris", g_gameDir);
    sprintf_s(yol, "%s\\login.jpg", dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(yol, GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow == (DWORD)sizeof(g_girisJpg))
        return;
    CreateDirectoryA(dir, nullptr);
    HANDLE h = CreateFileA(yol, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("[GIRIS] gorsel yazilamadi"); return; }
    DWORD yz = 0;
    WriteFile(h, g_girisJpg, (DWORD)sizeof(g_girisJpg), &yz, nullptr);
    CloseHandle(h);
    Log("[GIRIS] gomulu gorsel cikarildi (%lu bayt)", (unsigned long)yz);
}
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    // Eskiden sabit 4 sn beklenirdi; giris ekrani dosyalari bu sure icinde
    // yuklendigi icin kacirilirdi. Hedef fonksiyonun prologue'u (55 8B EC)
    // gorunene kadar (en cok 4 sn) 5 ms'de bir yokla.
    {
        BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
        int t = 0;
        for (; t < 800; ++t)
        {
            BYTE* q = base + RVA_PACKGET;
            if (IsReadable(q, 8) && q[0] == 0x55 && q[1] == 0x8B && q[2] == 0xEC) break;
            Sleep(5);
        }
        Sleep(20);
        g_erkenBekleme = t * 5;
    }

    g_exeBase = (BYTE*)GetModuleHandleA(nullptr);
    GirisGorseliCikar();

    Log("");
    Log("########## ZIRH YONLENDIRME ##########");
    Log("exe base: %p  (hook beklemesi %d ms)", g_exeBase, g_erkenBekleme);
    Log("derleme: %s %s", __DATE__, __TIME__);   // hangi d3d9.dll surumunun log'u oldugu belli olsun

    LoadConfig();
    if (g_mountLearn && !g_mountAll) LoadMounts();
    KModYukle();

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MH_Initialize basarisiz: %s", MH_StatusToString(status));
        return 0;
    }

    InitGamepadBridge();

    void* target = g_exeBase + RVA_PACKGET;
    if (!IsReadable(target, 8)) { Log("Hedef okunamadi"); return 0; }

    BYTE* p = (BYTE*)target;
    if (!(p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC))
    {
        Log("Prologue uyusmadi: %02X %02X %02X - IPTAL", p[0], p[1], p[2]);
        return 0;
    }

    if (MH_CreateHook(target, &Hook_PackGet, &g_origPackGet) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        Log("Hook kurulamadi");
        return 0;
    }

    // Guncelleme fonksiyonunu listeye ekle. Asagidaki dongu prologue'u
    // (55 8B EC) dogrulamadan hook kurmuyor; adres fonksiyon baslangici
    // degilse atlanir ve logda "ATLANDI" yazar.
    // (Isim etiketi adaylari 0x024E75D0 / 0x024E76C0 denendi: biri metin
    // atama, digeri irk/vnum ayari cikti, seviye yazisi degil. Cikarildi.)
    // (Guncelleme fonksiyonunun hook'u artik burada degil: girisi calisma
    //  aninda bulunup GuncelKur() ile kuruluyor.)

    // --- hookfn: config'de belirtilen fonksiyonlari hook'la ---
    for (int i = 0; i < g_fnHookCount; ++i)
    {
        void* fn = g_exeBase + g_fnHook[i].rva;
        if (!IsReadable(fn, 8))
        {
            Log("hookfn exe+0x%08X: okunamadi", g_fnHook[i].rva);
            continue;
        }

        BYTE* b = (BYTE*)fn;
        if (!(b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC))
        {
            Log("hookfn exe+0x%08X: prologue uyusmadi (%02X %02X %02X) - ATLANDI",
                g_fnHook[i].rva, b[0], b[1], b[2]);
            continue;
        }

        if (MH_CreateHook(fn, g_fnDetours[i], &g_fnHook[i].orig) == MH_OK &&
            MH_EnableHook(fn) == MH_OK)
            Log("hookfn exe+0x%08X: aktif", g_fnHook[i].rva);
        else
            Log("hookfn exe+0x%08X: kurulamadi", g_fnHook[i].rva);
    }

    g_ready = true;
    Log("Hook aktif (exe+0x%08X)", RVA_PACKGET);

    Log("");

    return 0;
}

// ---------------------------------------------------------------------------
static HMODULE Real()
{
    if (!g_real)
    {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\d3d9.dll");
        g_real = LoadLibraryA(path);
    }
    return g_real;
}

template <typename T>
static T RealFn(const char* name)
{
    HMODULE module = Real();
    return module ? (T)GetProcAddress(module, name) : nullptr;
}

extern "C" {

    void* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion)
    {
        auto fn = RealFn<void* (WINAPI*)(UINT)>("Direct3DCreate9");
        void* d3d = fn ? fn(sdkVersion) : nullptr;
        return d3d;
    }

    HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdkVersion, void** out)
    {
        auto fn = RealFn<HRESULT(WINAPI*)(UINT, void**)>("Direct3DCreate9Ex");
        return fn ? fn(sdkVersion, out) : E_FAIL;
    }

    int WINAPI Proxy_D3DPERF_BeginEvent(DWORD color, LPCWSTR name)
    {
        auto fn = RealFn<int (WINAPI*)(DWORD, LPCWSTR)>("D3DPERF_BeginEvent");
        return fn ? fn(color, name) : 0;
    }

    int WINAPI Proxy_D3DPERF_EndEvent(void)
    {
        auto fn = RealFn<int (WINAPI*)(void)>("D3DPERF_EndEvent");
        return fn ? fn() : 0;
    }

    DWORD WINAPI Proxy_D3DPERF_GetStatus(void)
    {
        auto fn = RealFn<DWORD(WINAPI*)(void)>("D3DPERF_GetStatus");
        return fn ? fn() : 0;
    }

    BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame(void)
    {
        auto fn = RealFn<BOOL(WINAPI*)(void)>("D3DPERF_QueryRepeatFrame");
        return fn ? fn() : FALSE;
    }

    void WINAPI Proxy_D3DPERF_SetMarker(DWORD color, LPCWSTR name)
    {
        auto fn = RealFn<void (WINAPI*)(DWORD, LPCWSTR)>("D3DPERF_SetMarker");
        if (fn) fn(color, name);
    }

    void WINAPI Proxy_D3DPERF_SetOptions(DWORD options)
    {
        auto fn = RealFn<void (WINAPI*)(DWORD)>("D3DPERF_SetOptions");
        if (fn) fn(options);
    }

    void WINAPI Proxy_D3DPERF_SetRegion(DWORD color, LPCWSTR name)
    {
        auto fn = RealFn<void (WINAPI*)(DWORD, LPCWSTR)>("D3DPERF_SetRegion");
        if (fn) fn(color, name);
    }

} // extern "C"

static HANDLE g_hotkeyThread = nullptr;

// ---------------------------------------------------------------------------
// Temiz kapanis
// Hook canliyken DLL bellekten cikarsa, hooklanan adresteki atlama
// artik var olmayan koda gider ve oyun coker. Bu yuzden boşaltmada
// hooklari kaldirip is parcaciklarini durduruyoruz.
static void Shutdown()
{
    g_ready = false;                      // hooklar artik is yapmasin
    InterlockedExchange(&g_shutdown, 1);  // hotkey dongusu dursun

    // Panel penceresinin proseduru bu DLL'in icinde: DLL cikmadan once
    // pencereyi kapat, yoksa bir sonraki mesajda oyun coker.
    if (g_panelWnd) PostMessageA(g_panelWnd, PANEL_MSG_KAPAT, 0, 0);
    if (g_panelThread)
    {
        WaitForSingleObject(g_panelThread, 500);
        CloseHandle(g_panelThread);
        g_panelThread = nullptr;
    }

    if (g_hotkeyThread)
    {
        WaitForSingleObject(g_hotkeyThread, 500);
        CloseHandle(g_hotkeyThread);
        g_hotkeyThread = nullptr;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    Log("kapanis: hooklar kaldirildi");
    LogFlush();
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&g_lock);
    ShutdownGamepadBridge();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;

        GetModuleFileNameA(module, g_gameDir, MAX_PATH);
        char* slash = strrchr(g_gameDir, '\\');
        if (slash) *(slash + 1) = '\0';
        sprintf_s(g_logPath, "%sproxy_log.txt", g_gameDir);
        sprintf_s(g_modDir, "%smod\\", g_gameDir);

        if (IsGameProcess())
        {
            // LOG HER OYUN ACILISINDA TEMIZ BASLAR: eski proxy_log.txt silinir.
            // Silinemezse (baska surec aciksa) bos olarak yeniden yaratilmayi
            // dene; o da olmazsa eski log kalir ve uzerine eklenir. Bu noktada
            // henuz hicbir Log() cagrisi yapilmadi, yani yeni oturumun satirini
            // silme riski yok.
            if (!DeleteFileA(g_logPath))
            {
                HANDLE lf = CreateFileA(g_logPath, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
            }

            HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            g_hotkeyThread = CreateThread(nullptr, 0, HotkeyThread,
                                          nullptr, 0, nullptr);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // reserved != NULL  ->  surec sonlaniyor. Isletim sistemi zaten
        // her seyi topluyor; bu asamada kilit beklemek kilitlenmeye
        // yol acabilir, o yuzden dokunmuyoruz.
        if (reserved == nullptr)
            Shutdown();
    }
    return TRUE;
}
