// HapWallpaper - fondo de pantalla animado ultraliviano.
// Reproduce un video en bucle detras de los iconos del escritorio usando
// Media Foundation (decodificacion por GPU). Se pausa solo cuando una ventana
// maximizada / a pantalla completa tapa el escritorio, al bloquear la sesion
// o con la pantalla apagada.
//
// Uso: HapWallpaper.exe [ruta_video|ruta.hap] [--debug] [--nopause] [--notaskbar] [--noautostart]
//   Sin ruta usa "fondo.hap" junto al .exe si existe (cuadros BC1, sin decodificador de
//   video) y si no "fondo.mp4" (Media Foundation). --debug escribe log.txt junto al .exe.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <string>
#include <cstdio>
#include <cwchar>

// GUIDs (definidos aqui para no depender de que mfuuid los traiga).
DEFINE_GUID(FV_CLSID_MFMediaEngineClassFactory, 0xb44392da, 0x499b, 0x446b, 0xa4, 0xcb, 0x00, 0x5f, 0xea, 0xd0, 0xe6, 0xd5);
DEFINE_GUID(FV_IID_IMFMediaEngineClassFactory, 0x4d645ace, 0x26aa, 0x4688, 0x9b, 0xe1, 0xdf, 0x35, 0x16, 0x99, 0x0b, 0x93);
DEFINE_GUID(FV_IID_IMFMediaEngineNotify, 0xfee7c112, 0xe776, 0x42b5, 0x9b, 0xbf, 0x00, 0x48, 0x52, 0x4e, 0x2b, 0xd5);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_CALLBACK, 0xc60381b8, 0x83a4, 0x41f8, 0xa3, 0xd0, 0xde, 0x05, 0x07, 0x68, 0x49, 0xa9);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_PLAYBACK_HWND, 0xd988879b, 0x67c9, 0x4d92, 0xba, 0xa7, 0x6e, 0xad, 0xd4, 0x46, 0x03, 0x9d);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_DXGI_MANAGER, 0x065702da, 0x1094, 0x486d, 0x86, 0x17, 0xee, 0x7c, 0xc4, 0xee, 0x46, 0x48);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 0x5066893c, 0x8cf9, 0x42bc, 0x8b, 0x8a, 0x47, 0x22, 0x12, 0xe5, 0x27, 0x26);
DEFINE_GUID(FV_IID_ID3D10Multithread, 0x9b7e4e00, 0x342c, 0x4106, 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0);
DEFINE_GUID(FV_GUID_CONSOLE_DISPLAY_STATE, 0x6fe69556, 0x704a, 0x47a0, 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47);

#define WM_TRAY (WM_APP + 1)
#define IDT_CHECK 1
#define IDM_TOGGLE 100
#define IDM_EXIT 101

static HINSTANCE g_inst;
static HWND g_msgWnd, g_videoWnd;
static IMFMediaEngine* g_engine;
static ID3D11Device* g_d3d;
static IMFDXGIDeviceManager* g_dxgiMgr;
static UINT g_resetToken;
static std::wstring g_videoPath, g_exeDir;
static bool g_debug, g_noAutostart, g_noAutoPause, g_userPaused, g_autoPaused, g_locked, g_displayOff;
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;

// Modo "frame server": el motor decodifica a la velocidad del video (60 fps) y un
// hilo propio presenta el ultimo cuadro en cada refresco del monitor (200 Hz).
// Asi G-Sync se queda en 200 Hz sin decodificar 200 cuadros por segundo.
static IDXGISwapChain1* g_swap;
static ID3D11Texture2D* g_frameTex;
static HANDLE g_frameWait, g_runEvent, g_rtThread;
static volatile LONG g_rtStop;
static UINT g_vidW, g_vidH;

static void Log(const wchar_t* fmt, ...) {
    if (!g_debug) return;
    FILE* f = _wfopen((g_exeDir + L"\\log.txt").c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- Media Engine
class Notify : public IMFMediaEngineNotify {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == FV_IID_IMFMediaEngineNotify) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) Log(L"Media Engine error %u hr=0x%08X", (unsigned)p1, (unsigned)p2);
        return S_OK;
    }
};

static void InitD3D() {
    if (g_d3d) return;
    D3D_FEATURE_LEVEL fl;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_d3d, &fl, nullptr))) {
        Log(L"D3D11 sin aceleracion de video; se usa modo por defecto");
        g_d3d = nullptr; return;
    }
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_d3d->QueryInterface(FV_IID_ID3D10Multithread, (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiMgr)))
        g_dxgiMgr->ResetDevice(g_d3d, g_resetToken);
}

static bool CreateEngine(HWND hwnd, bool useDxgi) {
    IMFMediaEngineClassFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(FV_CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  FV_IID_IMFMediaEngineClassFactory, (void**)&factory);
    if (FAILED(hr)) { Log(L"No se pudo crear la fabrica: 0x%08X", (unsigned)hr); return false; }
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 4);
    Notify* notify = new Notify();
    attr->SetUnknown(FV_MF_MEDIA_ENGINE_CALLBACK, notify);
    // Con DXGI: modo frame server (sin ventana); sin DXGI: el motor dibuja en la ventana.
    if (useDxgi && g_dxgiMgr) attr->SetUnknown(FV_MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiMgr);
    else attr->SetUINT64(FV_MF_MEDIA_ENGINE_PLAYBACK_HWND, (UINT64)(ULONG_PTR)hwnd);
    attr->SetUINT32(FV_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 87 /* DXGI_FORMAT_B8G8R8A8_UNORM */);
    hr = factory->CreateInstance(0, attr, &g_engine);
    notify->Release(); attr->Release(); factory->Release();
    if (FAILED(hr)) { Log(L"CreateInstance fallo (dxgi=%d): 0x%08X", useDxgi, (unsigned)hr); g_engine = nullptr; return false; }

    g_engine->SetMuted(TRUE);
    g_engine->SetLoop(TRUE);
    g_engine->SetAutoPlay(TRUE);
    BSTR src = SysAllocString(g_videoPath.c_str());
    hr = g_engine->SetSource(src);
    SysFreeString(src);
    if (FAILED(hr)) Log(L"SetSource fallo: 0x%08X", (unsigned)hr);
    g_engine->Play();
    Log(L"Motor creado (dxgi=%d)", useDxgi);
    return true;
}

// ------------------------------------------------- Modo HAP (cuadros ya comprimidos)
// fondo.hap: cuadros DXT1/BC1 de HAP sin compresion extra (ffmpeg -c:v hap -compressor
// none -f rawvideo). La placa lee BC1 directamente: no se usa el decodificador de video,
// solo se sube el cuadro (1,8 MB) y se dibuja con un shader minimo.
// Al final del archivo, preparar_video.ps1 agrega un bloque de 32 bytes con los datos
// del video (sin el bloque se asume 30 fps, como los primeros archivos).
#pragma pack(push, 1)
struct HapTrailer { char magic[8]; UINT width, height, fpsNum, fpsDen, frames, stride; };
#pragma pack(pop)
static const char kHapMagic[8] = { 'H', 'A', 'P', 'W', 'A', 'L', 'L', '1' };
static std::wstring g_hapPath;
static HANDLE g_hapFile, g_hapMap;
static bool g_hapReady; // archivo validado; cada hilo de render mapea y suelta su propia vista
static UINT g_hapFrames, g_hapStride, g_hapHdr, g_hapW, g_hapH, g_hapFpsNum = 30, g_hapFpsDen = 1;
static ID3D11Texture2D* g_hapTex;
static ID3D11ShaderResourceView* g_hapSrv;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static bool InitHap(UINT w, UINT h) {
    if (g_hapReady && g_hapW == w && g_hapH == h) return true;
    if (g_hapReady) return false;
    g_hapFile = CreateFileW(g_hapPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hapFile == INVALID_HANDLE_VALUE) { g_hapFile = nullptr; return false; }
    LARGE_INTEGER size; GetFileSizeEx(g_hapFile, &size);
    g_hapMap = CreateFileMappingW(g_hapFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    // Vista temporal solo para validar; se suelta antes de reproducir.
    const BYTE* p = g_hapMap ? (const BYTE*)MapViewOfFile(g_hapMap, FILE_MAP_READ, 0, 0, 0) : nullptr;
    UINT payload = w * h / 2;
    bool ok = p && size.QuadPart > 8 && (p[3] & 0x0F) == 0x0B && (p[3] & 0xF0) == 0xA0; // DXT1, sin Snappy
    if (ok) {
        UINT s24 = p[0] | (p[1] << 8) | (p[2] << 16);
        g_hapHdr = s24 ? 4 : 8;
        UINT s = s24 ? s24 : *(const UINT*)(p + 4);
        g_hapStride = g_hapHdr + payload;
        LONGLONG data = size.QuadPart;
        const HapTrailer* t = size.QuadPart >= (LONGLONG)sizeof(HapTrailer)
            ? (const HapTrailer*)(p + size.QuadPart - sizeof(HapTrailer)) : nullptr;
        if (t && !memcmp(t->magic, kHapMagic, 8)) {
            data -= sizeof(HapTrailer);
            ok = t->width == w && t->height == h && t->stride == g_hapStride && t->fpsNum && t->fpsDen &&
                 (LONGLONG)t->frames * g_hapStride == data;
            if (ok) { g_hapFpsNum = t->fpsNum; g_hapFpsDen = t->fpsDen; }
        }
        ok = ok && s == payload && data % g_hapStride == 0;
        g_hapFrames = ok ? (UINT)(data / g_hapStride) : 0;
    }
    if (p) UnmapViewOfFile(p);
    if (!ok || !g_hapFrames) {
        Log(L"El archivo HAP no coincide con la pantalla %ux%u; se usa el video", w, h);
        if (g_hapMap) CloseHandle(g_hapMap);
        CloseHandle(g_hapFile); g_hapMap = g_hapFile = nullptr;
        g_hapPath.clear();
        return false;
    }
    g_hapReady = true; g_hapW = w; g_hapH = h;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_d3d->CreateTexture2D(&td, nullptr, &g_hapTex);
    if (SUCCEEDED(hr)) hr = g_d3d->CreateShaderResourceView(g_hapTex, nullptr, &g_hapSrv);

    static const char hlsl[] =
        "Texture2D t : register(t0);\n"
        "float4 vs(uint i : SV_VertexID) : SV_Position {\n"
        "  float2 p = float2((i << 1) & 2, i & 2);\n"
        "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
        "float4 ps(float4 p : SV_Position) : SV_Target {\n"
        "  return float4(t.Load(int3(p.xy, 0)).rgb, 1); }\n";
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    if (SUCCEEDED(hr) && compile) {
        hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, nullptr);
        if (SUCCEEDED(hr)) hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, nullptr);
        if (SUCCEEDED(hr)) hr = g_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        if (SUCCEEDED(hr)) hr = g_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (FAILED(hr)) { Log(L"No se pudo preparar HAP: 0x%08X", (unsigned)hr); g_hapPath.clear(); g_hapReady = false; return false; }
    Log(L"HAP: %u cuadros %ux%u a %u/%u fps", g_hapFrames, w, h, g_hapFpsNum, g_hapFpsDen);
    return true;
}

// No pausar hasta haber mostrado al menos un cuadro (si no, detras de la barra queda negro).
static volatile bool g_shown;
static bool ShouldPause() { return (g_userPaused || g_autoPaused || g_locked || g_displayOff) && (g_shown || !g_swap); }

static DWORD WINAPI RenderThread(LPVOID) {
    ID3D11DeviceContext* ctx = nullptr;
    g_d3d->GetImmediateContext(&ctx);
    bool waitNext = false, haveFrame = false;
    UINT64 frameVer = 0, bufVer[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
    IDXGISwapChain3* sc3 = nullptr;
    g_swap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    LONGLONG hapLast = 0, hapAcc = 0;
    UINT hapCur = ~0u;
    const BYTE* hapView = nullptr; // vista propia del archivo; se suelta al pausar
    while (!g_rtStop) {
        if (WaitForSingleObject(g_runEvent, 0) == WAIT_TIMEOUT) {
            // En pausa (ventana maximizada, juego, pantalla apagada): soltar los ~1 GB
            // de cuadros de la memoria del proceso hasta que vuelva a reproducirse.
            if (hapView) {
                UnmapViewOfFile(hapView); hapView = nullptr;
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
            WaitForSingleObject(g_runEvent, INFINITE);
        }
        if (g_rtStop) break;
        if (g_hapReady && !hapView) {
            hapView = (const BYTE*)MapViewOfFile(g_hapMap, FILE_MAP_READ, 0, 0, 0);
            if (!hapView) { Sleep(100); continue; }
        }
        // Espera el proximo refresco (la cola del swap chain tiene lugar).
        if (waitNext && WaitForSingleObject(g_frameWait, 100) != WAIT_OBJECT_0) { waitNext = false; continue; }
        waitNext = false;
        LONGLONG pts;
        bool hap = g_hapReady;
        if (hap) {
            // Reloj propio: solo avanza mientras se reproduce (saltos de pausa recortados).
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (hapLast) { LONGLONG dt = now.QuadPart - hapLast; if (dt > qpf.QuadPart / 20) dt = qpf.QuadPart / 20; hapAcc += dt; }
            hapLast = now.QuadPart;
            UINT f = (UINT)((hapAcc * (LONGLONG)g_hapFpsNum / (qpf.QuadPart * (LONGLONG)g_hapFpsDen)) % g_hapFrames);
            if (f != hapCur) {
                ctx->UpdateSubresource(g_hapTex, 0, nullptr, hapView + (size_t)f * g_hapStride + g_hapHdr, (g_hapW / 4) * 8, 0);
                hapCur = f; frameVer++; haveFrame = true;
            }
        } else if (!g_frameTex) {
            // Primer cuadro: crear la textura al tamano real del video.
            DWORD w = 0, h = 0;
            if (FAILED(g_engine->GetNativeVideoSize(&w, &h)) || !w || !h) { Sleep(10); continue; }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, &g_frameTex))) { Sleep(100); continue; }
            g_vidW = w; g_vidH = h;
            Log(L"Video %ux%u, presentando a la frecuencia del monitor", (unsigned)w, (unsigned)h);
        }
        if (!hap && g_engine->OnVideoStreamTick(&pts) == S_OK) { haveFrame = true; frameVer++; }
        if (!haveFrame) { Sleep(2); continue; }
        // El cuadro se convierte directo en el buffer del swap chain, y solo cuando ese
        // buffer tiene un cuadro viejo (una vez por buffer por cuadro de video, sin
        // copias intermedias). En los refrescos repetidos se presenta sin dibujar nada y
        // con un rectangulo sucio minimo: Windows casi no recompone, pero el monitor
        // sigue a 200 Hz.
        UINT idx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
        if (idx > 3) idx = 0;
        bool copied = false;
        if (!sc3 || bufVer[idx] != frameVer) {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
                DXGI_SWAP_CHAIN_DESC1 sd; g_swap->GetDesc1(&sd);
                if (hap) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (SUCCEEDED(g_d3d->CreateRenderTargetView(back, nullptr, &rtv))) {
                        D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
                        ctx->OMSetRenderTargets(1, &rtv, nullptr);
                        ctx->RSSetViewports(1, &vp);
                        ctx->IASetInputLayout(nullptr);
                        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ctx->VSSetShader(g_vs, nullptr, 0);
                        ctx->PSSetShader(g_ps, nullptr, 0);
                        ctx->PSSetShaderResources(0, 1, &g_hapSrv);
                        ctx->Draw(3, 0);
                        rtv->Release();
                        bufVer[idx] = frameVer; copied = true;
                    }
                } else {
                    RECT dst = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
                    if (SUCCEEDED(g_engine->TransferVideoFrame(back, nullptr, &dst, nullptr))) { bufVer[idx] = frameVer; copied = true; }
                }
                back->Release();
            }
        }
        RECT tiny = { 0, 0, 1, 1 };
        DXGI_PRESENT_PARAMETERS pp = {};
        if (!copied && sc3) { pp.DirtyRectsCount = 1; pp.pDirtyRects = &tiny; }
        if (SUCCEEDED(g_swap->Present1(1, 0, &pp))) { waitNext = true; g_shown = true; }
        else Sleep(5);
    }
    if (hapView) UnmapViewOfFile(hapView);
    if (sc3) sc3->Release();
    ctx->Release();
    return 0;
}

static void StopRender() {
    if (g_rtThread) {
        InterlockedExchange(&g_rtStop, 1);
        SetEvent(g_runEvent);
        WaitForSingleObject(g_rtThread, 2000);
        CloseHandle(g_rtThread); g_rtThread = nullptr;
    }
    g_shown = false;
    if (g_frameTex) { g_frameTex->Release(); g_frameTex = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_frameWait) { CloseHandle(g_frameWait); g_frameWait = nullptr; }
}

static bool StartRender(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    IDXGIDevice* dxdev = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    g_d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    if (dxdev) dxdev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = rc.right; sd.Height = rc.bottom;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // conserva el contenido de cada buffer
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = factory ? factory->CreateSwapChainForHwnd(g_d3d, hwnd, &sd, nullptr, nullptr, &g_swap) : E_FAIL;
    if (factory) factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxdev) dxdev->Release();
    if (FAILED(hr)) { Log(L"No se pudo crear el swap chain: 0x%08X", (unsigned)hr); g_swap = nullptr; return false; }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(g_swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        g_frameWait = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }
    if (!g_runEvent) g_runEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ShouldPause()) ResetEvent(g_runEvent); else SetEvent(g_runEvent);
    InterlockedExchange(&g_rtStop, 0);
    g_rtThread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
    SetThreadPriority(g_rtThread, THREAD_PRIORITY_ABOVE_NORMAL);
    Log(L"Presentacion propia %ldx%ld", rc.right, rc.bottom);
    return true;
}

static void DestroyEngine() {
    StopRender();
    if (g_engine) { g_engine->Shutdown(); g_engine->Release(); g_engine = nullptr; }
}

// ---------------------------------------------------------- Escritorio (WorkerW)
static BOOL CALLBACK FindWorkerProc(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
        *(HWND*)lp = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
    return TRUE;
}

static HWND FindWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR r;
    // Pide a Explorer que cree la capa WorkerW detras de los iconos.
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &r);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);
    HWND worker = nullptr;
    EnumWindows(FindWorkerProc, (LPARAM)&worker);
    if (!worker) worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); // Win11 24H2+
    return worker;
}

static RECT PrimaryMonitorRect() {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

static void Attach() {
    DestroyEngine();
    if (g_videoWnd && IsWindow(g_videoWnd)) DestroyWindow(g_videoWnd);
    g_videoWnd = nullptr;

    HWND worker = FindWorkerW();
    if (!worker) { Log(L"No se encontro WorkerW"); return; }
    RECT mon = PrimaryMonitorRect();
    POINT pts[2] = { { mon.left, mon.top }, { mon.right, mon.bottom } };
    MapWindowPoints(HWND_DESKTOP, worker, pts, 2);
    g_videoWnd = CreateWindowExW(WS_EX_NOACTIVATE, L"HapWallpaperVideo", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 pts[0].x, pts[0].y, pts[1].x - pts[0].x, pts[1].y - pts[0].y,
                                 worker, nullptr, g_inst, nullptr);
    if (!g_videoWnd) { Log(L"No se pudo crear la ventana de video"); return; }
    InitD3D();
    bool ok = false;
    if (!g_hapPath.empty() && g_d3d) {
        RECT rc; GetClientRect(g_videoWnd, &rc);
        ok = InitHap(rc.right, rc.bottom) && StartRender(g_videoWnd);
        if (!ok) StopRender();
        else Log(L"Reproduciendo HAP (sin decodificador de video)");
    }
    if (!ok) {
        ok = g_dxgiMgr && CreateEngine(g_videoWnd, true);
        if (ok && !StartRender(g_videoWnd)) { DestroyEngine(); ok = false; }
    }
    if (!ok) CreateEngine(g_videoWnd, false);
    Log(L"Adjuntado a WorkerW %p, rect %d,%d %dx%d", worker, pts[0].x, pts[0].y, pts[1].x - pts[0].x, pts[1].y - pts[0].y);
}

// ------------------------------------------------------------- Auto-pausa
struct CoverCtx { HMONITOR mon; RECT work; RECT full; HWND found; };

static BOOL CALLBACK CoverProc(HWND h, LPARAM lp) {
    CoverCtx* c = (CoverCtx*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return TRUE;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != c->mon) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    // Pausa con ventanas maximizadas o a pantalla completa: detras de la barra
    // transparente queda el ultimo cuadro congelado.
    bool covers = r.left <= c->work.left && r.top <= c->work.top && r.right >= c->work.right && r.bottom >= c->work.bottom;
    if (covers) { c->found = h; return FALSE; }
    return TRUE;
}

static HWND CoveringWindow() {
    CoverCtx c = {};
    c.mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(c.mon, &mi);
    c.work = mi.rcWork; c.full = mi.rcMonitor;
    EnumWindows(CoverProc, (LPARAM)&c);
    return c.found;
}

static bool FullscreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE s;
    if (FAILED(SHQueryUserNotificationState(&s))) return false;
    // QUNS_BUSY no se usa: Windows lo reporta aunque no haya nada en pantalla completa.
    return s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE;
}

static void UpdateTrayTip() {
    bool paused = g_userPaused || g_autoPaused || g_locked || g_displayOff;
    swprintf(g_nid.szTip, 128, L"HapWallpaper - %ls", g_userPaused ? L"pausado" : paused ? L"en pausa automatica" : L"reproduciendo");
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ApplyState() {
    bool pause = ShouldPause();
    if (g_runEvent) { if (pause) ResetEvent(g_runEvent); else SetEvent(g_runEvent); }
    if (g_engine) {
        BOOL isPaused = g_engine->IsPaused();
        if (pause && !isPaused) { g_engine->Pause(); Log(L"Pausa"); }
        else if (!pause && isPaused) { g_engine->Play(); Log(L"Reproduciendo"); }
    }
    UpdateTrayTip();
}

static void Check() {
    if (!g_videoWnd || !IsWindow(g_videoWnd)) { Log(L"Ventana perdida, reintentando"); Attach(); }
    HWND cover = CoveringWindow();
    bool autoPause = !g_noAutoPause && (cover != nullptr || FullscreenAppRunning());
    if (autoPause != g_autoPaused) {
        if (cover) { wchar_t t[128] = {}, c[64] = {}; GetWindowTextW(cover, t, 128); GetClassNameW(cover, c, 64); Log(L"Tapado por [%ls] \"%ls\"", c, t); }
        g_autoPaused = autoPause;
    }
    ApplyState();
}

// ------------------------------------------------------- Barra transparente
// Reemplazo de TranslucentTB: la ventana de la barra se vuelve transparente con
// SetWindowCompositionAttribute y HapWallpaperTB.dll (cargada en Explorer) oculta
// el fondo XAML y la linea superior.
struct AccentPolicy { int State, Flags, Color, Anim; };
struct WinCompAttrData { int Attr; void* Data; ULONG Size; };
typedef BOOL (WINAPI *PFN_SWCA)(HWND, WinCompAttrData*);
static bool g_taskbar = true;
static HHOOK g_tbHook;
static int g_hookTicks;
static DWORD g_injectedPid;
static int g_injectTries;
static HWINEVENTHOOK g_fgHook;

static void ClearTaskbarWindow(HWND h) {
    static PFN_SWCA set = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    static PFN_SWCA get = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowCompositionAttribute");
    if (!set) return;
    AccentPolicy cur = {};
    WinCompAttrData d = { 19 /* WCA_ACCENT_POLICY */, &cur, sizeof(cur) };
    if (get && get(h, &d) && cur.State == 2 && cur.Color == 0) return; // ya esta transparente
    AccentPolicy ap = { 2 /* ACCENT_ENABLE_TRANSPARENTGRADIENT */, 2, 0x00000000, 0 };
    d.Data = &ap;
    set(h, &d);
}

static void ApplyTaskbar() {
    if (!g_taskbar) return;
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) ClearTaskbarWindow(t);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr))) ClearTaskbarWindow(s);
}

static void InjectTaskbar() {
    if (!g_taskbar) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    static DWORD lastPid;
    if (pid != lastPid) { lastPid = pid; g_injectTries = 0; } // Explorer se reinicio
    if (!pid || pid == g_injectedPid) return;

    // Ya cargada en este Explorer (por ejemplo, si HapWallpaper se reinicio)?
    wchar_t evName[64];
    swprintf(evName, 64, L"Local\\HapWallpaperTB_%u", (unsigned)pid);
    if (HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, evName)) {
        CloseHandle(ev);
        if (g_tbHook) { UnhookWindowsHookEx(g_tbHook); g_tbHook = nullptr; }
        g_injectedPid = pid;
        Log(L"Barra lista en Explorer (pid %u)", (unsigned)pid);
        return;
    }
    if (g_tbHook) {
        // Esperando a que la DLL se registre; si tarda demasiado, reintentar.
        if (++g_hookTicks < 5) { PostMessageW(tray, WM_NULL, 0, 0); return; }
        UnhookWindowsHookEx(g_tbHook); g_tbHook = nullptr;
    }
    if (g_injectTries >= 30) return;
    std::wstring dll = g_exeDir + L"\\HapWallpaperTB.dll";
    static HMODULE mod = nullptr;
    if (!mod) mod = LoadLibraryW(dll.c_str());
    HOOKPROC proc = mod ? (HOOKPROC)GetProcAddress(mod, "FvGetMsgProc") : nullptr;
    if (!proc) { Log(L"No se pudo cargar HapWallpaperTB.dll"); g_injectTries = 30; return; }
    g_injectTries++;
    DWORD tid = GetWindowThreadProcessId(tray, nullptr);
    g_tbHook = SetWindowsHookExW(WH_GETMESSAGE, proc, mod, tid);
    g_hookTicks = 0;
    PostMessageW(tray, WM_NULL, 0, 0);
    Log(L"Hook en la barra (pid %u, intento %d): %ls", (unsigned)pid, g_injectTries, g_tbHook ? L"ok" : L"fallo");
}

static void CALLBACK ForegroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { ApplyTaskbar(); }

// ------------------------------------------------------------- Bandeja / salida
static void AddTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_msgWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"HapWallpaper");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, g_userPaused ? L"Reanudar" : L"Pausar");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Salir");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_msgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_msgWnd, nullptr);
    DestroyMenu(m);
}

static void RestoreDesktop() {
    // Vuelve a aplicar el fondo estatico actual para que Explorer repinte la capa.
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wp, SPIF_SENDCHANGE);
}

static LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { Log(L"Explorer reiniciado"); AddTray(); Attach(); ApplyState(); return 0; }
    switch (msg) {
    case WM_TIMER: Check(); InjectTaskbar(); ApplyTaskbar(); return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) { g_userPaused = !g_userPaused; ApplyState(); }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TOGGLE) { g_userPaused = !g_userPaused; ApplyState(); }
        else if (LOWORD(wp) == IDM_EXIT) DestroyWindow(h);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = true;
        else if (wp == WTS_SESSION_UNLOCK) g_locked = false;
        ApplyState();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* ps = (POWERBROADCAST_SETTING*)lp;
            if (ps->PowerSetting == FV_GUID_CONSOLE_DISPLAY_STATE && ps->DataLength >= sizeof(DWORD)) {
                g_displayOff = *(DWORD*)ps->Data == 0;
                ApplyState();
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE: Attach(); ApplyState(); return 0;
    case WM_ENDSESSION:
        if (wp) { DestroyEngine(); }
        return 0;
    case WM_DESTROY:
        KillTimer(h, IDT_CHECK);
        if (g_tbHook) UnhookWindowsHookEx(g_tbHook);
        if (g_fgHook) UnhookWinEvent(g_fgHook);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyEngine();
        if (g_videoWnd && IsWindow(g_videoWnd)) DestroyWindow(g_videoWnd);
        RestoreDesktop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        if (!g_engine) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        return 1;
    }
    if (msg == WM_DESTROY && h == g_videoWnd) { DestroyEngine(); g_videoWnd = nullptr; }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"HapWallpaper_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe; g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    g_videoPath = g_exeDir + L"\\fondo.mp4";
    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--debug")) g_debug = true;
        else if (!wcscmp(argv[i], L"--nopause")) g_noAutoPause = true;
        else if (!wcscmp(argv[i], L"--notaskbar")) g_taskbar = false;
        else if (!wcscmp(argv[i], L"--noautostart")) g_noAutostart = true;
        else g_videoPath = argv[i];
    }
    LocalFree(argv);
    // fondo.hap junto al .exe (o un .hap pasado como argumento) tiene prioridad sobre el video.
    if (g_videoPath.size() > 4 && !_wcsicmp(g_videoPath.c_str() + g_videoPath.size() - 4, L".hap")) g_hapPath = g_videoPath;
    else if (g_videoPath == g_exeDir + L"\\fondo.mp4" && GetFileAttributesW((g_exeDir + L"\\fondo.hap").c_str()) != INVALID_FILE_ATTRIBUTES)
        g_hapPath = g_exeDir + L"\\fondo.hap";
    if (GetFileAttributesW(g_videoPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, (L"No se encontro el video:\n" + g_videoPath).c_str(), L"HapWallpaper", MB_ICONERROR);
        return 1;
    }
    Log(L"Inicio, video: %ls", g_videoPath.c_str());

    // Se anota solo en el inicio de Windows (HKCU\...\Run) con su ruta actual.
    if (!g_noAutostart) {
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            wchar_t cur[MAX_PATH * 2] = {};
            DWORD sz = sizeof(cur), type = 0;
            if (RegQueryValueExW(k, L"HapWallpaper", nullptr, &type, (BYTE*)cur, &sz) != ERROR_SUCCESS || cmd != cur)
                RegSetValueExW(k, L"HapWallpaper", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = MsgProc; wc.lpszClassName = L"HapWallpaperMsg";
    RegisterClassW(&wc);
    wc.lpfnWndProc = VideoProc; wc.lpszClassName = L"HapWallpaperVideo";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    // Ventana oculta (no message-only) para recibir TaskbarCreated y avisos de energia.
    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"HapWallpaperMsg", L"HapWallpaper", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    WTSRegisterSessionNotification(g_msgWnd, NOTIFY_FOR_THIS_SESSION);
    RegisterPowerSettingNotification(g_msgWnd, &FV_GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    AddTray();
    Attach();
    Check();
    InjectTaskbar();
    ApplyTaskbar();
    if (g_taskbar)
        g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundHook, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_msgWnd, IDT_CHECK, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    WTSUnRegisterSessionNotification(g_msgWnd);
    if (g_dxgiMgr) g_dxgiMgr->Release();
    if (g_d3d) g_d3d->Release();
    MFShutdown();
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
