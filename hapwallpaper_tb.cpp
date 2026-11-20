// HapWallpaperTB.dll - se carga dentro de Explorer mediante la API de diagnostico
// XAML (igual que TranslucentTB) y oculta el fondo ("BackgroundFill") y la linea
// superior ("BackgroundStroke") de la barra de tareas de Windows 11, poniendo su
// opacidad en 0. La transparencia de la ventana en si la aplica HapWallpaper.exe.

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#include <unordered_map>
#include <string>
#include <cstdio>
#include <cwchar>

// {C96D667B-21D9-420A-844C-874FCA567138}
static const CLSID CLSID_HapWallpaperTap = { 0xc96d667b, 0x21d9, 0x420a, { 0x84, 0x4c, 0x87, 0x4f, 0xca, 0x56, 0x71, 0x38 } };
static const IID FV_IID_IVisualTreeService3 = { 0x0e79c6e0, 0x85a0, 0x4be8, { 0xb4, 0x1a, 0x65, 0x5c, 0xf1, 0xfd, 0x19, 0xbd } };
static const IID FV_IID_IXamlDiagnostics = { 0x18c9e2b6, 0x3f43, 0x4116, { 0x9f, 0x2b, 0xff, 0x93, 0x5d, 0x77, 0x70, 0xd2 } };
static const IID FV_IID_IVisualTreeServiceCallback = { 0xaa7a8931, 0x80e4, 0x4fec, { 0x8f, 0x3b, 0x55, 0x3f, 0x87, 0xb4, 0x96, 0x6e } };
static const IID FV_IID_IVisualTreeServiceCallback2 = { 0xbad9eb88, 0xae77, 0x4397, { 0xb9, 0x48, 0x5f, 0xa2, 0xdb, 0x0a, 0x19, 0xea } };

static LONG g_objects;

static void Log(const wchar_t* fmt, ...) {
    // Log junto a la DLL.
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Log, &self);
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy(slash + 1, L"tb_log.txt");
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

class Watcher : public IVisualTreeServiceCallback2 {
    LONG m_ref = 1;
    IVisualTreeService3* m_svc;
    std::unordered_map<InstanceHandle, InstanceHandle> m_parent;
    std::unordered_map<InstanceHandle, std::wstring> m_type;
    unsigned m_opacityIndex = UINT_MAX;
    int m_hidden = 0;

    bool UnderTaskbarFrame(InstanceHandle h) {
        for (int i = 0; i < 64; i++) {
            auto it = m_parent.find(h);
            if (it == m_parent.end()) return false;
            h = it->second;
            auto t = m_type.find(h);
            if (t != m_type.end() && t->second == L"Taskbar.TaskbarFrame") return true;
        }
        return false;
    }

    void Hide(InstanceHandle h, const wchar_t* name) {
        if (m_opacityIndex == UINT_MAX) {
            unsigned srcCount = 0, propCount = 0;
            PropertyChainSource* srcs = nullptr;
            PropertyChainValue* vals = nullptr;
            HRESULT hr = m_svc->GetPropertyValuesChain(h, &srcCount, &srcs, &propCount, &vals);
            if (FAILED(hr)) { Log(L"GetPropertyValuesChain 0x%08X", (unsigned)hr); return; }
            for (unsigned i = 0; i < propCount; i++) {
                if (vals[i].PropertyName && !wcscmp(vals[i].PropertyName, L"Opacity")) m_opacityIndex = vals[i].Index;
                SysFreeString(vals[i].Type); SysFreeString(vals[i].DeclaringType); SysFreeString(vals[i].ValueType);
                SysFreeString(vals[i].ItemType); SysFreeString(vals[i].Value); SysFreeString(vals[i].PropertyName);
            }
            for (unsigned i = 0; i < srcCount; i++) {
                SysFreeString(srcs[i].TargetType); SysFreeString(srcs[i].Name);
                SysFreeString(srcs[i].SrcInfo.FileName); SysFreeString(srcs[i].SrcInfo.Hash);
            }
            CoTaskMemFree(vals); CoTaskMemFree(srcs);
            if (m_opacityIndex == UINT_MAX) { Log(L"No se encontro la propiedad Opacity"); return; }
        }
        BSTR type = SysAllocString(L"Windows.Foundation.Double");
        BSTR value = SysAllocString(L"0");
        InstanceHandle v = 0;
        HRESULT hr = m_svc->CreateInstance(type, value, &v);
        SysFreeString(type); SysFreeString(value);
        if (SUCCEEDED(hr)) hr = m_svc->SetProperty(h, v, m_opacityIndex);
        m_hidden++;
        Log(L"Oculto %ls -> 0x%08X", name, (unsigned)hr);
    }

public:
    explicit Watcher(IVisualTreeService3* svc) : m_svc(svc) { m_svc->AddRef(); InterlockedIncrement(&g_objects); }
    ~Watcher() { m_svc->Release(); InterlockedDecrement(&g_objects); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == FV_IID_IVisualTreeServiceCallback || riid == FV_IID_IVisualTreeServiceCallback2) {
            *ppv = static_cast<IVisualTreeServiceCallback2*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    STDMETHODIMP OnVisualTreeChange(ParentChildRelation rel, VisualElement el, VisualMutationType mt) override {
        if (mt == Add) {
            m_parent[el.Handle] = rel.Parent;
            m_type[el.Handle] = el.Type ? el.Type : L"";
            if (el.Type && el.Name && !wcscmp(el.Type, L"Windows.UI.Xaml.Shapes.Rectangle") &&
                (!wcscmp(el.Name, L"BackgroundFill") || !wcscmp(el.Name, L"BackgroundStroke")) &&
                UnderTaskbarFrame(el.Handle)) {
                Hide(el.Handle, el.Name);
            }
        } else {
            m_parent.erase(el.Handle);
            m_type.erase(el.Handle);
        }
        return S_OK;
    }
    STDMETHODIMP OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override { return S_OK; }

    static DWORD WINAPI AdviseThread(LPVOID p) {
        Watcher* w = (Watcher*)p;
        // AdviseVisualTreeChange no puede llamarse desde el hilo de SetSite (se cuelga).
        HRESULT hr = w->m_svc->AdviseVisualTreeChange(w);
        Log(L"AdviseVisualTreeChange 0x%08X, elementos ocultos: %d", (unsigned)hr, w->m_hidden);
        w->Release();
        return 0;
    }
};

class Tap : public IObjectWithSite {
    LONG m_ref = 1;
    IUnknown* m_site = nullptr;
public:
    Tap() { InterlockedIncrement(&g_objects); }
    ~Tap() { if (m_site) m_site->Release(); InterlockedDecrement(&g_objects); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) { *ppv = static_cast<IObjectWithSite*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    STDMETHODIMP SetSite(IUnknown* site) override {
        if (m_site) { m_site->Release(); m_site = nullptr; }
        if (!site) return S_OK;
        m_site = site; m_site->AddRef();
        IVisualTreeService3* svc = nullptr;
        HRESULT hr = site->QueryInterface(FV_IID_IVisualTreeService3, (void**)&svc);
        Log(L"SetSite, IVisualTreeService3 0x%08X", (unsigned)hr);
        if (FAILED(hr)) return hr;
        Watcher* w = new Watcher(svc);
        svc->Release();
        HANDLE t = CreateThread(nullptr, 0, Watcher::AdviseThread, w, 0, nullptr);
        if (t) CloseHandle(t); else w->Release();
        // El watcher queda vivo mientras Explorer lo tenga registrado.
        return S_OK;
    }
    STDMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!m_site) { *ppv = nullptr; return E_FAIL; }
        return m_site->QueryInterface(riid, ppv);
    }
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Tap* t = new Tap();
        HRESULT hr = t->QueryInterface(riid, ppv);
        t->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_HapWallpaperTap) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

// ---- Arranque dentro de Explorer -------------------------------------------
// HapWallpaper.exe instala un hook de mensajes en el hilo de la barra; cuando Explorer
// carga esta DLL por el hook, se fija en memoria y se registra a si misma como TAP
// de diagnostico XAML del propio proceso (asi lo hace tambien TranslucentTB).
typedef HRESULT (WINAPI *PFN_IXDE)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID, PCWSTR);
static LONG g_started;
static HANDLE g_readyEvent;

static DWORD WINAPI InitThread(LPVOID) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&InitThread, &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_IXDE ixde = xaml ? (PFN_IXDE)GetProcAddress(xaml, "InitializeXamlDiagnosticsEx") : nullptr;
    if (!ixde) { Log(L"InitializeXamlDiagnosticsEx no disponible"); return 0; }
    HRESULT hr = E_FAIL;
    int n;
    for (n = 1; n < 100; n++) {
        wchar_t conn[48];
        swprintf(conn, 48, L"VisualDiagConnection%d", n);
        hr = ixde(conn, GetCurrentProcessId(), nullptr, path, CLSID_HapWallpaperTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
    }
    Log(L"Registrado en Explorer (conexion %d): 0x%08X", n, (unsigned)hr);
    if (SUCCEEDED(hr)) {
        wchar_t name[64];
        swprintf(name, 64, L"Local\\HapWallpaperTB_%u", (unsigned)GetCurrentProcessId());
        g_readyEvent = CreateEventW(nullptr, TRUE, TRUE, name); // HapWallpaper.exe lo usa para saber que ya esta
    } else {
        InterlockedExchange(&g_started, 0); // permite reintentar
    }
    return 0;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK FvGetMsgProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && InterlockedCompareExchange(&g_started, 1, 0) == 0) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = wcsrchr(exe, L'\\');
        if (base && !_wcsicmp(base + 1, L"explorer.exe")) {
            HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
