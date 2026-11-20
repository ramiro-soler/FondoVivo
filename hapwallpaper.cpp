// HapWallpaper - fondo de pantalla animado ultraliviano para Windows 11.
// Reproduce un video en bucle detras de los iconos del escritorio, en todos los
// monitores. Con un archivo HAP (cuadros BC1) no usa el decodificador de video: la
// placa lee los cuadros tal cual. Sin HAP usa Media Foundation (decodificacion por
// hardware). Presenta en cada refresco del monitor para que G-Sync/FreeSync no baje la
// frecuencia, y se pausa por monitor con ventanas maximizadas o a pantalla completa,
// con la sesion bloqueada o con la pantalla apagada.
//
// Uso: HapWallpaper.exe [ruta_video|ruta.hap] [--debug] [--nopause] [--notaskbar] [--noautostart]
//   Sin ruta usa "fondo.hap" junto al .exe si existe y si no "fondo.mp4".
//   --debug escribe log.txt junto al .exe (y tb_log.txt la DLL de la barra).

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
#include <vector>
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
static HWND g_msgWnd;
static ID3D11Device* g_d3d;
static IMFDXGIDeviceManager* g_dxgiMgr;
static UINT g_resetToken;
static CRITICAL_SECTION g_ctxLock; // el contexto inmediato se comparte entre los hilos de cada monitor
static std::wstring g_videoPath, g_exeDir;
static bool g_debug, g_noAutostart, g_noAutoPause, g_userPaused, g_fullscreenApp, g_locked, g_displayOff;
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;

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

static void Balloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, text, 255);
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ------------------------------------------------------------------ Un monitor
// Cada monitor tiene su ventana dentro de WorkerW, su swap chain y su hilo de render.
// El hilo presenta en cada refresco del monitor: G-Sync se queda a la frecuencia
// maxima aunque el video sea de 30 fps. En los refrescos en los que el cuadro no cambio
// no dibuja nada y le pasa a Windows un rectangulo sucio minimo.
struct Screen {
    HMONITOR mon = nullptr;
    RECT full = {}, work = {};
    HWND wnd = nullptr;
    IMFMediaEngine* engine = nullptr;      // solo en modo video
    bool engineOwnsWindow = false;          // sin D3D11: el motor dibuja en la ventana
    IDXGISwapChain1* swap = nullptr;
    HANDLE frameWait = nullptr, runEvent = nullptr, thread = nullptr;
    volatile LONG stop = 0;
    volatile bool shown = false;            // ya mostro al menos un cuadro
    bool autoPaused = false;
    ID3D11Texture2D* hapTex = nullptr;      // modo HAP: cuadro BC1 de este monitor
    ID3D11ShaderResourceView* hapSrv = nullptr;
    ID3D11Buffer* cb = nullptr;             // recorte para llenar la pantalla
};
static std::vector<Screen*> g_screens;

// Rectangulo del video (0..1) que llena la pantalla sin bordes, recortando al centro.
static void CoverRect(float vw, float vh, float sw, float sh, float out[4] /*x0,y0,x1,y1*/) {
    float va = vw / vh, sa = sw / sh;
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    if (va > sa) { float s = sa / va; out[0] = (1 - s) / 2; out[2] = out[0] + s; }
    else if (va < sa) { float s = va / sa; out[1] = (1 - s) / 2; out[3] = out[1] + s; }
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

static IMFMediaEngine* CreateEngine(HWND hwnd, bool useDxgi) {
    IMFMediaEngineClassFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(FV_CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  FV_IID_IMFMediaEngineClassFactory, (void**)&factory);
    if (FAILED(hr)) { Log(L"No se pudo crear la fabrica: 0x%08X", (unsigned)hr); return nullptr; }
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 4);
    Notify* notify = new Notify();
    attr->SetUnknown(FV_MF_MEDIA_ENGINE_CALLBACK, notify);
    // Con DXGI: modo frame server (sin ventana); sin DXGI: el motor dibuja en la ventana.
    if (useDxgi && g_dxgiMgr) attr->SetUnknown(FV_MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiMgr);
    else attr->SetUINT64(FV_MF_MEDIA_ENGINE_PLAYBACK_HWND, (UINT64)(ULONG_PTR)hwnd);
    attr->SetUINT32(FV_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 87 /* DXGI_FORMAT_B8G8R8A8_UNORM */);
    IMFMediaEngine* engine = nullptr;
    hr = factory->CreateInstance(0, attr, &engine);
    notify->Release(); attr->Release(); factory->Release();
    if (FAILED(hr)) { Log(L"CreateInstance fallo (dxgi=%d): 0x%08X", useDxgi, (unsigned)hr); return nullptr; }
    engine->SetMuted(TRUE);
    engine->SetLoop(TRUE);
    engine->SetAutoPlay(TRUE);
    BSTR src = SysAllocString(g_videoPath.c_str());
    hr = engine->SetSource(src);
    SysFreeString(src);
    if (FAILED(hr)) Log(L"SetSource fallo: 0x%08X", (unsigned)hr);
    engine->Play();
    return engine;
}

// ------------------------------------------------- Modo HAP (cuadros ya comprimidos)
// fondo.hap: cuadros DXT1/BC1 de HAP sin compresion extra (ffmpeg -c:v hap -compressor
// none -f rawvideo). La placa lee BC1 directamente: no se usa el decodificador de video,
// solo se sube el cuadro y se dibuja con un shader que lo escala para llenar cada monitor.
// Al final del archivo, preparar_video.ps1 agrega un bloque de 32 bytes con los datos
// del video. Sin el bloque se asume 30 fps y la resolucion del monitor principal.
#pragma pack(push, 1)
struct HapTrailer { char magic[8]; UINT width, height, fpsNum, fpsDen, frames, stride; };
#pragma pack(pop)
static const char kHapMagic[8] = { 'H', 'A', 'P', 'W', 'A', 'L', 'L', '1' };
static std::wstring g_hapPath;
static HANDLE g_hapFile, g_hapMap;
static bool g_hapReady; // archivo validado; cada hilo mapea y suelta su propia vista
static UINT g_hapFrames, g_hapStride, g_hapHdr, g_hapW, g_hapH, g_hapFpsNum = 30, g_hapFpsDen = 1;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_sampler;

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static void CloseHap() {
    if (g_hapMap) CloseHandle(g_hapMap);
    if (g_hapFile) CloseHandle(g_hapFile);
    g_hapMap = g_hapFile = nullptr;
    g_hapReady = false;
}

static bool InitHap() {
    if (g_hapReady) return true;
    if (g_hapPath.empty() || !g_d3d) return false;
    g_hapFile = CreateFileW(g_hapPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hapFile == INVALID_HANDLE_VALUE) { g_hapFile = nullptr; return false; }
    LARGE_INTEGER size; GetFileSizeEx(g_hapFile, &size);
    g_hapMap = CreateFileMappingW(g_hapFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    // Vista temporal solo para validar; se suelta antes de reproducir.
    const BYTE* p = g_hapMap ? (const BYTE*)MapViewOfFile(g_hapMap, FILE_MAP_READ, 0, 0, 0) : nullptr;
    bool ok = p && size.QuadPart > 40 && (p[3] & 0x0F) == 0x0B && (p[3] & 0xF0) == 0xA0; // DXT1, sin Snappy
    if (ok) {
        UINT s24 = p[0] | (p[1] << 8) | (p[2] << 16);
        g_hapHdr = s24 ? 4 : 8;
        UINT payload = s24 ? s24 : *(const UINT*)(p + 4);
        LONGLONG data = size.QuadPart;
        const HapTrailer* t = (const HapTrailer*)(p + size.QuadPart - sizeof(HapTrailer));
        if (!memcmp(t->magic, kHapMagic, 8)) {
            data -= sizeof(HapTrailer);
            g_hapW = t->width; g_hapH = t->height;
            g_hapFpsNum = t->fpsNum; g_hapFpsDen = t->fpsDen;
            ok = t->fpsNum && t->fpsDen && t->stride == g_hapHdr + payload && (LONGLONG)t->frames * t->stride == data;
        } else {
            // Archivo sin bloque final: se asume la resolucion del monitor principal.
            g_hapW = GetSystemMetrics(SM_CXSCREEN); g_hapH = GetSystemMetrics(SM_CYSCREEN);
            g_hapFpsNum = 30; g_hapFpsDen = 1;
        }
        g_hapStride = g_hapHdr + payload;
        ok = ok && g_hapW % 4 == 0 && g_hapH % 4 == 0 && payload == g_hapW * g_hapH / 2 && data % g_hapStride == 0;
        g_hapFrames = ok ? (UINT)(data / g_hapStride) : 0;
    }
    if (p) UnmapViewOfFile(p);
    if (!ok || !g_hapFrames) { Log(L"Archivo HAP invalido: %ls", g_hapPath.c_str()); CloseHap(); return false; }

    HRESULT hr = S_OK;
    if (!g_vs) {
        // El triangulo cubre la pantalla; uv recorta el video para llenarla (cb).
        static const char hlsl[] =
            "Texture2D t : register(t0); SamplerState s : register(s0);\n"
            "cbuffer c : register(b0) { float4 r; };\n"
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
            "V vs(uint i : SV_VertexID) {\n"
            "  float2 p = float2((i << 1) & 2, i & 2); V o;\n"
            "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "  o.uv = r.xy + p * (r.zw - r.xy); return o; }\n"
            "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr;
        hr = compile ? S_OK : E_FAIL;
        if (SUCCEEDED(hr)) hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, nullptr);
        if (SUCCEEDED(hr)) hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, nullptr);
        if (SUCCEEDED(hr)) hr = g_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        if (SUCCEEDED(hr)) hr = g_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        if (SUCCEEDED(hr)) {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = g_d3d->CreateSamplerState(&sd, &g_sampler);
        }
    }
    if (FAILED(hr)) { Log(L"No se pudo preparar HAP: 0x%08X", (unsigned)hr); CloseHap(); return false; }
    g_hapReady = true;
    Log(L"HAP: %u cuadros %ux%u a %u/%u fps", g_hapFrames, g_hapW, g_hapH, g_hapFpsNum, g_hapFpsDen);
    return true;
}

static bool CreateHapResources(Screen* s, UINT sw, UINT sh) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_hapW; td.Height = g_hapH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, &s->hapTex))) return false;
    if (FAILED(g_d3d->CreateShaderResourceView(s->hapTex, nullptr, &s->hapSrv))) return false;
    float r[4]; CoverRect((float)g_hapW, (float)g_hapH, (float)sw, (float)sh, r);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { r, 0, 0 };
    return SUCCEEDED(g_d3d->CreateBuffer(&bd, &init, &s->cb));
}

// --------------------------------------------------------------------- Render
static bool ShouldPause(Screen* s) {
    bool want = g_userPaused || s->autoPaused || g_fullscreenApp || g_locked || g_displayOff;
    // No pausar hasta haber mostrado un cuadro (si no, detras de la barra queda negro).
    return want && (s->shown || !s->swap);
}

static DWORD WINAPI RenderThread(LPVOID param) {
    Screen* s = (Screen*)param;
    ID3D11DeviceContext* ctx = nullptr;
    g_d3d->GetImmediateContext(&ctx);
    bool hap = g_hapReady && s->hapTex;
    bool waitNext = false, haveFrame = false;
    UINT64 frameVer = 0, bufVer[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
    IDXGISwapChain3* sc3 = nullptr;
    s->swap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3);
    DXGI_SWAP_CHAIN_DESC1 sd; s->swap->GetDesc1(&sd);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    LONGLONG hapLast = 0, hapAcc = 0;
    UINT hapCur = ~0u;
    const BYTE* hapView = nullptr;      // vista propia del archivo; se suelta al pausar
    MFVideoNormalizedRect src = { 0, 0, 1, 1 };
    bool srcReady = false;
    while (!s->stop) {
        if (WaitForSingleObject(s->runEvent, 0) == WAIT_TIMEOUT) {
            // En pausa: soltar la vista del archivo HAP (~1 GB) hasta que vuelva a reproducirse.
            if (hapView) {
                UnmapViewOfFile(hapView); hapView = nullptr;
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
            hapLast = 0;
            WaitForSingleObject(s->runEvent, INFINITE);
        }
        if (s->stop) break;
        if (hap && !hapView) {
            hapView = (const BYTE*)MapViewOfFile(g_hapMap, FILE_MAP_READ, 0, 0, 0);
            if (!hapView) { Sleep(100); continue; }
        }
        // Espera el proximo refresco (la cola del swap chain tiene lugar).
        if (waitNext && WaitForSingleObject(s->frameWait, 100) != WAIT_OBJECT_0) { waitNext = false; continue; }
        waitNext = false;
        if (hap) {
            // Reloj propio: solo avanza mientras se reproduce.
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (hapLast) { LONGLONG dt = now.QuadPart - hapLast; if (dt > qpf.QuadPart / 20) dt = qpf.QuadPart / 20; hapAcc += dt; }
            hapLast = now.QuadPart;
            UINT f = (UINT)((hapAcc * (LONGLONG)g_hapFpsNum / (qpf.QuadPart * (LONGLONG)g_hapFpsDen)) % g_hapFrames);
            if (f != hapCur) {
                EnterCriticalSection(&g_ctxLock);
                ctx->UpdateSubresource(s->hapTex, 0, nullptr, hapView + (size_t)f * g_hapStride + g_hapHdr, (g_hapW / 4) * 8, 0);
                LeaveCriticalSection(&g_ctxLock);
                hapCur = f; frameVer++; haveFrame = true;
            }
        } else {
            if (!srcReady) {
                DWORD vw = 0, vh = 0;
                if (FAILED(s->engine->GetNativeVideoSize(&vw, &vh)) || !vw || !vh) { Sleep(10); continue; }
                float r[4]; CoverRect((float)vw, (float)vh, (float)sd.Width, (float)sd.Height, r);
                src = { r[0], r[1], r[2], r[3] };
                srcReady = true;
                Log(L"Video %ux%u en monitor %ux%u", (unsigned)vw, (unsigned)vh, sd.Width, sd.Height);
            }
            LONGLONG pts;
            if (s->engine->OnVideoStreamTick(&pts) == S_OK) { haveFrame = true; frameVer++; }
        }
        if (!haveFrame) { Sleep(2); continue; }
        // Cada buffer del swap chain se redibuja solo cuando tiene un cuadro viejo (una vez
        // por buffer por cuadro de video, sin copias intermedias).
        UINT idx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
        if (idx > 3) idx = 0;
        bool drawn = false;
        if (!sc3 || bufVer[idx] != frameVer) {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(s->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
                if (hap) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (SUCCEEDED(g_d3d->CreateRenderTargetView(back, nullptr, &rtv))) {
                        D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
                        EnterCriticalSection(&g_ctxLock);
                        ctx->OMSetRenderTargets(1, &rtv, nullptr);
                        ctx->RSSetViewports(1, &vp);
                        ctx->IASetInputLayout(nullptr);
                        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ctx->VSSetShader(g_vs, nullptr, 0);
                        ctx->VSSetConstantBuffers(0, 1, &s->cb);
                        ctx->PSSetShader(g_ps, nullptr, 0);
                        ctx->PSSetShaderResources(0, 1, &s->hapSrv);
                        ctx->PSSetSamplers(0, 1, &g_sampler);
                        ctx->Draw(3, 0);
                        ID3D11RenderTargetView* none = nullptr;
                        ctx->OMSetRenderTargets(1, &none, nullptr);
                        LeaveCriticalSection(&g_ctxLock);
                        rtv->Release();
                        drawn = true;
                    }
                } else {
                    RECT dst = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
                    EnterCriticalSection(&g_ctxLock);
                    drawn = SUCCEEDED(s->engine->TransferVideoFrame(back, &src, &dst, nullptr));
                    LeaveCriticalSection(&g_ctxLock);
                }
                if (drawn) bufVer[idx] = frameVer;
                back->Release();
            }
        }
        // En los refrescos repetidos se presenta con un rectangulo sucio minimo: Windows
        // casi no recompone, pero el monitor sigue a su frecuencia maxima.
        RECT tiny = { 0, 0, 1, 1 };
        DXGI_PRESENT_PARAMETERS pp = {};
        if (!drawn && sc3) { pp.DirtyRectsCount = 1; pp.pDirtyRects = &tiny; }
        if (SUCCEEDED(s->swap->Present1(1, 0, &pp))) { waitNext = true; s->shown = true; }
        else Sleep(5);
    }
    if (hapView) UnmapViewOfFile(hapView);
    if (sc3) sc3->Release();
    ctx->Release();
    return 0;
}

static bool StartRender(Screen* s) {
    RECT rc; GetClientRect(s->wnd, &rc);
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
    HRESULT hr = factory ? factory->CreateSwapChainForHwnd(g_d3d, s->wnd, &sd, nullptr, nullptr, &s->swap) : E_FAIL;
    if (factory) factory->MakeWindowAssociation(s->wnd, DXGI_MWA_NO_ALT_ENTER);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxdev) dxdev->Release();
    if (FAILED(hr)) { Log(L"No se pudo crear el swap chain: 0x%08X", (unsigned)hr); s->swap = nullptr; return false; }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(s->swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        s->frameWait = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }
    s->runEvent = CreateEventW(nullptr, TRUE, ShouldPause(s) ? FALSE : TRUE, nullptr);
    s->stop = 0;
    s->thread = CreateThread(nullptr, 0, RenderThread, s, 0, nullptr);
    SetThreadPriority(s->thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return true;
}

static void DestroyScreen(Screen* s) {
    if (s->thread) {
        InterlockedExchange(&s->stop, 1);
        SetEvent(s->runEvent);
        WaitForSingleObject(s->thread, 2000);
        CloseHandle(s->thread);
    }
    if (s->runEvent) CloseHandle(s->runEvent);
    if (s->frameWait) CloseHandle(s->frameWait);
    if (s->swap) s->swap->Release();
    if (s->hapSrv) s->hapSrv->Release();
    if (s->hapTex) s->hapTex->Release();
    if (s->cb) s->cb->Release();
    if (s->engine) { s->engine->Shutdown(); s->engine->Release(); }
    if (s->wnd && IsWindow(s->wnd)) { SetWindowLongPtrW(s->wnd, GWLP_USERDATA, 0); DestroyWindow(s->wnd); }
    delete s;
}

static void DestroyScreens() {
    for (Screen* s : g_screens) DestroyScreen(s);
    g_screens.clear();
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

static bool g_hapWarned;

static void AttachScreen(HWND worker, HMONITOR mon) {
    Screen* s = new Screen();
    s->mon = mon;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    s->full = mi.rcMonitor; s->work = mi.rcWork;
    POINT pts[2] = { { mi.rcMonitor.left, mi.rcMonitor.top }, { mi.rcMonitor.right, mi.rcMonitor.bottom } };
    MapWindowPoints(HWND_DESKTOP, worker, pts, 2);
    s->wnd = CreateWindowExW(WS_EX_NOACTIVATE, L"HapWallpaperVideo", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             pts[0].x, pts[0].y, pts[1].x - pts[0].x, pts[1].y - pts[0].y,
                             worker, nullptr, g_inst, nullptr);
    if (!s->wnd) { Log(L"No se pudo crear la ventana de video"); delete s; return; }
    SetWindowLongPtrW(s->wnd, GWLP_USERDATA, (LONG_PTR)s);
    g_screens.push_back(s);
    RECT rc; GetClientRect(s->wnd, &rc);

    bool ok = false;
    if (g_hapReady) {
        ok = CreateHapResources(s, rc.right, rc.bottom) && StartRender(s);
        if (ok) Log(L"Monitor %ldx%ld: HAP (sin decodificador de video)", rc.right, rc.bottom);
    }
    if (!ok && g_dxgiMgr && GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        s->engine = CreateEngine(s->wnd, true);
        ok = s->engine && StartRender(s);
        if (ok) Log(L"Monitor %ldx%ld: video con Media Foundation", rc.right, rc.bottom);
    }
    if (!ok && !s->swap && GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (s->engine) { s->engine->Shutdown(); s->engine->Release(); }
        s->engine = CreateEngine(s->wnd, false);
        s->engineOwnsWindow = s->engine != nullptr;
    }
}

static BOOL CALLBACK EnumMonProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    AttachScreen((HWND)lp, mon);
    return TRUE;
}

static void Attach() {
    DestroyScreens();
    HWND worker = FindWorkerW();
    if (!worker) { Log(L"No se encontro WorkerW"); return; }
    InitD3D();
    if (!g_hapPath.empty() && !InitHap() && !g_hapWarned) {
        g_hapWarned = true;
        bool mp4 = GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        Balloon(L"HapWallpaper", mp4 ? L"No se pudo usar el archivo .hap; se reproduce el video MP4. Volve a generarlo con preparar_video.ps1."
                                     : L"No se pudo usar el archivo .hap. Volve a generarlo con preparar_video.ps1.");
    }
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)worker);
    Log(L"Adjuntado a WorkerW %p, %u monitores", worker, (unsigned)g_screens.size());
}

// ------------------------------------------------------------- Auto-pausa
struct CoverCtx { HMONITOR mon; RECT work; HWND found; };

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
    // Pausa con ventanas maximizadas o a pantalla completa en ese monitor: detras de la
    // barra transparente queda el ultimo cuadro congelado.
    bool covers = r.left <= c->work.left && r.top <= c->work.top && r.right >= c->work.right && r.bottom >= c->work.bottom;
    if (covers) { c->found = h; return FALSE; }
    return TRUE;
}

static HWND CoveringWindow(Screen* s) {
    CoverCtx c = { s->mon, s->work, nullptr };
    EnumWindows(CoverProc, (LPARAM)&c);
    return c.found;
}

static bool FullscreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    // QUNS_BUSY no se usa: Windows lo reporta aunque no haya nada en pantalla completa.
    return st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}

static void UpdateTrayTip() {
    unsigned playing = 0;
    for (Screen* s : g_screens) if (!ShouldPause(s)) playing++;
    if (g_userPaused) swprintf(g_nid.szTip, 128, L"HapWallpaper - pausado");
    else swprintf(g_nid.szTip, 128, L"HapWallpaper - %u de %u monitores reproduciendo%ls", playing, (unsigned)g_screens.size(),
                  g_hapReady ? L" (HAP)" : L"");
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ApplyState() {
    for (Screen* s : g_screens) {
        bool pause = ShouldPause(s);
        if (s->runEvent) { if (pause) ResetEvent(s->runEvent); else SetEvent(s->runEvent); }
        if (s->engine) {
            BOOL isPaused = s->engine->IsPaused();
            if (pause && !isPaused) s->engine->Pause();
            else if (!pause && isPaused) s->engine->Play();
        }
    }
    UpdateTrayTip();
}

static void Check() {
    bool lost = g_screens.empty();
    for (Screen* s : g_screens) if (!IsWindow(s->wnd)) lost = true;
    if (lost) { Log(L"Ventanas perdidas, reintentando"); Attach(); }
    g_fullscreenApp = !g_noAutoPause && FullscreenAppRunning();
    for (Screen* s : g_screens) {
        HWND cover = g_noAutoPause ? nullptr : CoveringWindow(s);
        bool ap = cover != nullptr;
        if (ap != s->autoPaused) {
            if (cover) { wchar_t t[128] = {}, c[64] = {}; GetWindowTextW(cover, t, 128); GetClassNameW(cover, c, 64); Log(L"Monitor tapado por [%ls] \"%ls\"", c, t); }
            s->autoPaused = ap;
        }
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
    case WM_DISPLAYCHANGE: Attach(); ApplyState(); return 0; // monitores o resolucion cambiaron
    case WM_ENDSESSION:
        if (wp) DestroyScreens();
        return 0;
    case WM_DESTROY:
        KillTimer(h, IDT_CHECK);
        if (g_tbHook) UnhookWindowsHookEx(g_tbHook);
        if (g_fgHook) UnhookWinEvent(g_fgHook);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyScreens();
        RestoreDesktop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        Screen* s = (Screen*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!s || (!s->swap && !s->engineOwnsWindow)) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"HapWallpaper_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitializeCriticalSection(&g_ctxLock);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe; g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    g_videoPath = g_exeDir + L"\\fondo.mp4";
    bool explicitPath = false;
    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--debug")) g_debug = true;
        else if (!wcscmp(argv[i], L"--nopause")) g_noAutoPause = true;
        else if (!wcscmp(argv[i], L"--notaskbar")) g_taskbar = false;
        else if (!wcscmp(argv[i], L"--noautostart")) g_noAutostart = true;
        else { g_videoPath = argv[i]; explicitPath = true; }
    }
    LocalFree(argv);
    // La DLL de la barra solo escribe tb_log.txt si existe este evento.
    HANDLE debugEvent = g_debug ? CreateEventW(nullptr, TRUE, TRUE, L"Local\\HapWallpaperDebug") : nullptr;

    // fondo.hap junto al .exe (o un .hap pasado como argumento) tiene prioridad sobre el video.
    if (g_videoPath.size() > 4 && !_wcsicmp(g_videoPath.c_str() + g_videoPath.size() - 4, L".hap")) g_hapPath = g_videoPath;
    else if (!explicitPath && GetFileAttributesW((g_exeDir + L"\\fondo.hap").c_str()) != INVALID_FILE_ATTRIBUTES)
        g_hapPath = g_exeDir + L"\\fondo.hap";
    bool haveVideo = GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_hapPath.empty() && !haveVideo) {
        MessageBoxW(nullptr, (L"No se encontro el video:\n" + g_videoPath +
                              L"\n\nPone fondo.hap o fondo.mp4 junto a HapWallpaper.exe (ver preparar_video.ps1).").c_str(),
                    L"HapWallpaper", MB_ICONERROR);
        return 1;
    }
    Log(L"Inicio, hap: %ls, video: %ls", g_hapPath.empty() ? L"-" : g_hapPath.c_str(), haveVideo ? g_videoPath.c_str() : L"-");

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
    CloseHap();
    if (g_sampler) g_sampler->Release();
    if (g_vs) g_vs->Release();
    if (g_ps) g_ps->Release();
    if (g_dxgiMgr) g_dxgiMgr->Release();
    if (g_d3d) g_d3d->Release();
    MFShutdown();
    CoUninitialize();
    if (debugEvent) CloseHandle(debugEvent);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
