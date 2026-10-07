#include "client.hpp"
#include <algorithm>
#include <ctffunc.h>
#include <ctfutb.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <msctf.h>
#include <rime_ime/config.hpp>
#include <stdexcept>
#include <string>
#include <textstor.h>
#include <thread>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static constexpr CLSID service_id = {
    0x1a5d0370, 0x40a3, 0x4865, {0x90, 0xa6, 0x32, 0x4e, 0x1b, 0xb1, 0x85, 0x5b}};
static constexpr GUID profile_id = {
    0x80b722f7, 0xfa50, 0x4d8e, {0xa0, 0x84, 0xe1, 0x2a, 0x5b, 0x90, 0x65, 0x85}};
static void check(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(what);
}
static void hr(HRESULT value, const char *what) {
    if (FAILED(value)) {
        std::cerr << what << ": HRESULT 0x" << std::hex << value << std::dec << '\n';
        throw std::runtime_error(what);
    }
}
// A host-side sink exercises the public TSF candidate protocol. The host may
// take over rendering at any point, including while a composition is active.
class CandidateHost final : public ITfUIElementSink {
    ULONG refs_ = 1;

  public:
    bool allowWindow = true;
    DWORD element = 0;
    int begins = 0, updates = 0, ends = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_ITfUIElementSink)
            *p = static_cast<ITfUIElementSink *>(this);
        if (!*p)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --refs_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id, BOOL *show) override {
        element = id;
        ++begins;
        *show = allowWindow;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
        if (id == element)
            ++updates;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
        if (id == element)
            ++ends;
        return S_OK;
    }
};
static void candidate_contract(const wchar_t *path) {
    hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialize contract host");
    auto dll = LoadLibraryW(path);
    check(dll, "load requested TSF DLL for public-interface contract");
    using GetFactory = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
    auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(dll, "DllGetClassObject"));
    check(getFactory, "class factory export");
    ComPtr<IClassFactory> factory;
    hr(getFactory(service_id, IID_PPV_ARGS(&factory)), "contract class factory");
    ComPtr<ITfTextInputProcessorEx> service;
    hr(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service)), "contract text service");
    ComPtr<ITfCandidateListUIElementBehavior> candidates;
    hr(service.As(&candidates), "TIP exposes Windows candidate-list behavior");
    UINT count = 99, selection = 99;
    hr(candidates->GetCount(&count), "candidate count before composition");
    check(count == 0, "inactive TIP exposes no stale candidate strings");
    check(candidates->GetSelection(&selection) == S_FALSE,
          "inactive candidate list has no selected item");
    check(FAILED(candidates->SetSelection(0)), "inactive candidate selection is rejected");
    ComPtr<ITfLangBarItem> bar;
    hr(service.As(&bar), "Windows input-mode language bar item");
    TF_LANGBARITEMINFO info{};
    hr(bar->GetInfo(&info), "input-mode language bar metadata");
    check(info.guidItem == GUID_LBI_INPUTMODE, "mode item uses Windows' canonical input-mode GUID");
    ComPtr<ITfDisplayAttributeProvider> attributes;
    hr(service.As(&attributes), "TIP exposes native Windows preedit display attributes");
    ComPtr<IEnumTfDisplayAttributeInfo> attributeEnum;
    hr(attributes->EnumDisplayAttributeInfo(&attributeEnum), "enumerate native preedit attributes");
    ComPtr<ITfDisplayAttributeInfo> attribute;
    ULONG fetched = 0;
    hr(attributeEnum->Next(1, &attribute, &fetched), "native input display attribute");
    check(fetched == 1 && attribute, "one input-style display attribute is available");
    TF_DISPLAYATTRIBUTE style{};
    hr(attribute->GetAttributeInfo(&style), "native preedit style");
    check(style.lsStyle == TF_LS_SOLID && !style.fBoldLine && style.bAttr == TF_ATTR_INPUT,
          "unconverted preedit uses a fine Windows input underline");
    GUID attributeId{};
    hr(attribute->GetGUID(&attributeId), "native preedit attribute identity");
    ComPtr<ITfDisplayAttributeInfo> byId;
    hr(attributes->GetDisplayAttributeInfo(attributeId, &byId),
       "lookup the enumerated style by GUID");
    check(attributes->GetDisplayAttributeInfo(GUID_NULL, byId.ReleaseAndGetAddressOf()) ==
              E_INVALIDARG,
          "display provider rejects unrelated attribute identities");
    attribute.Reset();
    attributeEnum.Reset();
    attributes.Reset();
    ComPtr<ITfFunctionProvider> provider;
    hr(service.As(&provider), "TIP exposes the Windows settings function provider");
    GUID providerType{};
    hr(provider->GetType(&providerType), "settings provider identity");
    check(providerType == service_id, "settings function provider identifies R-IME");
    ComPtr<IUnknown> function;
    hr(provider->GetFunction(GUID_NULL, IID_ITfFnConfigure, &function),
       "Windows obtains the input-method properties function");
    ComPtr<ITfFnConfigure> configure;
    hr(function.As(&configure), "input-method properties interface");
    BSTR displayName = nullptr;
    hr(configure->GetDisplayName(&displayName), "input-method properties display name");
    check(displayName && SysStringLen(displayName) > 0, "settings function has a readable name");
    SysFreeString(displayName);
    check(configure->Show(nullptr, 0x0804, GUID_NULL) == E_INVALIDARG,
          "settings invocation for an unrelated profile is rejected before launch");
    function.Reset();
    check(provider->GetFunction(profile_id, IID_ITfFnConfigure, &function) == E_NOINTERFACE &&
              !function,
          "settings provider rejects unsupported function namespace");
    configure.Reset();
    provider.Reset();
    bar.Reset();
    candidates.Reset();
    service.Reset();
    factory.Reset();
    FreeLibrary(dll);
    CoUninitialize();
}
class TrackingFactory final : public IClassFactory {
    ULONG refs_ = 1;
    ComPtr<IClassFactory> factory_;

  public:
    ComPtr<ITfTextInputProcessorEx> active;
    explicit TrackingFactory(IClassFactory *factory) : factory_(factory) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_IClassFactory)
            *p = this;
        if (!*p)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID id, void **p) override {
        auto result = factory_->CreateInstance(outer, id, p);
        if (SUCCEEDED(result))
            static_cast<IUnknown *>(*p)->QueryInterface(
                IID_PPV_ARGS(active.ReleaseAndGetAddressOf()));
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        return factory_->LockServer(lock);
    }
};
// A real TSF ACP document store. All editing is performed by msctf ranges/edit
// sessions, rather than a mock ITfContext or an exported service test hook.
class Store final : public ITextStoreACP {
    ULONG refs_ = 1;
    ComPtr<ITextStoreACPSink> sink_;
    bool locked_ = false;
    DWORD pendingLock_ = 0;

  public:
    HWND window;
    std::wstring text;
    LONG start = 0, end = 0;
    bool readonly = false;
    bool deferLocks = false;
    bool failTextWrites = false;
    POINT layoutOrigin{100, 100};
    explicit Store(HWND w) : window(w) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_ITextStoreACP)
            *p = static_cast<ITextStoreACP *>(this);
        if (!*p)
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown *p, DWORD) override {
        return p->QueryInterface(id, reinterpret_cast<void **>(sink_.ReleaseAndGetAddressOf()));
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown *) override {
        sink_.Reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT *result) override {
        if (!sink_)
            return E_UNEXPECTED;
        if (deferLocks) {
            *result = (flags & TS_LF_SYNC) ? TS_E_SYNCHRONOUS : TS_S_ASYNC;
            if (!(flags & TS_LF_SYNC))
                pendingLock_ = flags;
            return S_OK;
        }
        if (locked_) {
            *result = TS_E_SYNCHRONOUS;
            return S_OK;
        }
        locked_ = true;
        *result = sink_->OnLockGranted(flags);
        locked_ = false;
        return S_OK;
    }
    bool hasDeferredLock() const {
        return pendingLock_ != 0;
    }
    void grantDeferredLock() {
        if (!pendingLock_)
            return;
        auto flags = pendingLock_;
        pendingLock_ = 0;
        locked_ = true;
        hr(sink_->OnLockGranted(flags), "grant pending application edit lock");
        locked_ = false;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS *s) override {
        s->dwDynamicFlags = readonly ? TS_SD_READONLY : 0;
        s->dwStaticFlags = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG a, LONG b, ULONG, LONG *x, LONG *y) override {
        *x = a;
        *y = b;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(ULONG, ULONG n, TS_SELECTION_ACP *s,
                                           ULONG *got) override {
        *got = n ? 1 : 0;
        if (n) {
            s->acpStart = start;
            s->acpEnd = end;
            s->style = {TS_AE_END, FALSE};
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG n, const TS_SELECTION_ACP *s) override {
        if (n) {
            start = s->acpStart;
            end = s->acpEnd;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(LONG a, LONG b, WCHAR *out, ULONG cap, ULONG *count,
                                      TS_RUNINFO *runs, ULONG runCap, ULONG *runCount,
                                      LONG *next) override {
        if (b < 0)
            b = static_cast<LONG>(text.size());
        if (a < 0 || b < a || b > static_cast<LONG>(text.size()))
            return TS_E_INVALIDPOS;
        ULONG n = std::min<ULONG>(b - a, cap);
        if (out && n)
            std::copy_n(text.data() + a, n, out);
        *count = n;
        *next = a + n;
        *runCount = 0;
        if (runCap && n) {
            runs[0] = {n, TS_RT_PLAIN};
            *runCount = 1;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG a, LONG b, const WCHAR *s, ULONG n,
                                      TS_TEXTCHANGE *change) override {
        if (failTextWrites)
            return E_FAIL;
        if (a < 0 || b < a || b > static_cast<LONG>(text.size()))
            return TS_E_INVALIDPOS;
        text.replace(a, b - a, s, n);
        *change = {a, b, a + static_cast<LONG>(n)};
        start = end = a + n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID *, const FORMATETC *,
                                                  BOOL *yes) override {
        *yes = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject *,
                                             TS_TEXTCHANGE *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR *s, ULONG n, LONG *a,
                                                    LONG *b, TS_TEXTCHANGE *change) override {
        if (a)
            *a = start;
        if (b)
            *b = end;
        if (flags & TS_IAS_QUERYONLY)
            return S_OK;
        LONG old = start;
        auto result = SetText(0, start, end, s, n, change);
        if (b)
            *b = old + n;
        return result;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject *, LONG *, LONG *,
                                                        TS_TEXTCHANGE *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID *,
                                                     DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID *,
                                                                  DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG halt, ULONG, const TS_ATTRID *,
                                                     DWORD, LONG *next, BOOL *found,
                                                     LONG *offset) override {
        *next = halt;
        *found = FALSE;
        *offset = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL *, ULONG *n) override {
        *n = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG *n) override {
        *n = static_cast<LONG>(text.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie *v) override {
        *v = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT *, DWORD,
                                              LONG *pos) override {
        *pos = end;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG a, LONG b, RECT *r,
                                         BOOL *clipped) override {
        *r = {layoutOrigin.x + a * 10, layoutOrigin.y, layoutOrigin.x + 10 + b * 10,
              layoutOrigin.y + 24};
        *clipped = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT *r) override {
        GetWindowRect(window, r);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND *w) override {
        *w = window;
        return S_OK;
    }
};
static ITfMessagePump *tsfMessagePump = nullptr;
static void pump() {
    MSG msg;
    auto peek = [&] {
        BOOL present = FALSE;
        if (tsfMessagePump) {
            hr(tsfMessagePump->PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE, &present),
               "TSF message pump");
            return present;
        }
        return PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
    };
    while (peek()) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
static bool start_installed_worker(const std::filesystem::path &directory, HANDLE &ownedProcess) {
    auto executable = std::filesystem::absolute(directory) / L"r-ime-engine.exe";
    if (!std::filesystem::is_regular_file(executable))
        return false;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    ownedProcess = process.hProcess;
    return true;
}
static void installed_test(bool uiLess) {
    hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialize");
    struct ComCleanup {
        ~ComCleanup() {
            CoUninitialize();
        }
    } cleanup;
    HWND previousForeground = GetForegroundWindow();
    struct WindowCleanup {
        HWND window = nullptr, previous = nullptr;
        ~WindowCleanup() {
            if (window)
                DestroyWindow(window);
            if (IsWindow(previous))
                SetForegroundWindow(previous);
        }
    } windowCleanup{nullptr, previousForeground};
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    hr(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&profiles)),
       "installed profile manager");
    TF_INPUTPROCESSORPROFILE registered{};
    hr(profiles->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id, nullptr,
                            &registered),
       "find installed system profile");
    ComPtr<ITfThreadMgr> manager;
    hr(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)),
       "create TSF thread manager");
    TfClientId client;
    if (uiLess) {
        ComPtr<ITfThreadMgrEx> extended;
        hr(manager.As(&extended), "UI-less TSF thread manager");
        hr(extended->ActivateEx(&client, TF_TMAE_UIELEMENTENABLEDONLY),
           "activate UI-less TSF host");
    } else
        hr(manager->Activate(&client), "activate TSF thread manager");
    ComPtr<ITfSource> threadSource;
    hr(manager.As(&threadSource), "installed candidate event source");
    ComPtr<CandidateHost> candidateHost;
    candidateHost.Attach(new CandidateHost);
    candidateHost->allowWindow = !uiLess;
    DWORD candidateCookie;
    hr(threadSource->AdviseSink(IID_ITfUIElementSink, candidateHost.Get(), &candidateCookie),
       "installed host candidate UI sink");
    ComPtr<ITfUIElementMgr> uiManager;
    hr(manager.As(&uiManager), "installed candidate UI manager");
    ComPtr<ITfMessagePump> messagePump;
    hr(manager.As(&messagePump), "query TSF message pump");
    tsfMessagePump = messagePump.Get();
    struct PumpCleanup {
        ~PumpCleanup() {
            tsfMessagePump = nullptr;
        }
    } pumpCleanup;
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.lpszClassName = L"R-IME.TSF.DocumentTest";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"R-IME installed TSF test",
                                  WS_OVERLAPPEDWINDOW, 100, 100, 600, 250, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    check(window, "create installed test document window");
    windowCleanup.window = window;
    ComPtr<Store> store;
    store.Attach(new Store(window));
    ComPtr<ITfDocumentMgr> document;
    hr(manager->CreateDocumentMgr(&document), "create installed test document");
    ComPtr<ITfContext> context;
    TfEditCookie cookie;
    hr(document->CreateContext(client, 0, store.Get(), &context, &cookie),
       "create real ACP context");
    hr(document->Push(context.Get()), "push installed test context");
    hr(manager->SetFocus(document.Get()), "focus installed TSF document");
    ComPtr<ITfDocumentMgr> previousDocument;
    hr(manager->AssociateFocus(window, document.Get(), &previousDocument),
       "associate installed document window focus");
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    SetFocus(window);
    pump();
    // Only this process changes its active profile. No temporary class factory,
    // profile registration, default-profile write, or session-wide switch.
    hr(profiles->ActivateProfile(
           TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id, nullptr,
           TF_IPPMF_FORPROCESS | TF_IPPMF_ENABLEPROFILE | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE),
       "activate installed profile in this test process");
    TF_INPUTPROCESSORPROFILE active{};
    hr(profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active),
       "query active installed profile");
    check(active.clsid == service_id && active.guidProfile == profile_id,
          "R-IME is the process's active keyboard profile");
    ComPtr<ITfFunctionProvider> installedFunctions;
    hr(manager->GetFunctionProvider(service_id, &installedFunctions),
       "Windows obtains the installed settings function provider");
    ComPtr<IUnknown> installedConfigure;
    hr(installedFunctions->GetFunction(GUID_NULL, IID_ITfFnConfigure, &installedConfigure),
       "installed provider exposes Windows input-method properties");
    installedConfigure.Reset();
    installedFunctions.Reset();
    ComPtr<ITfKeystrokeMgr> keys;
    hr(manager.As(&keys), "get Windows keystroke dispatcher");
    bool ready = false;
    for (int attempts = 0; attempts < 250; ++attempts) {
        pump();
        CLSID foreground{};
        BOOL threadFocus = FALSE;
        keys->GetForeground(&foreground);
        manager->IsThreadFocus(&threadFocus);
        if (GetForegroundWindow() == window && threadFocus && foreground == service_id) {
            ready = true;
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    check(ready, "Windows did not give the TSF test window foreground focus. Run interactively "
                 "or under the Windows debugger; background execution cannot claim desktop focus.");
    auto installedDll = GetModuleHandleW(L"r-ime-tsf.dll");
    check(installedDll != nullptr, "installed R-IME DLL loaded by Windows");
    for (int resource : {101, 102}) {
        auto icon = static_cast<HICON>(LoadImageW(installedDll, MAKEINTRESOURCEW(resource),
                                                  IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        check(icon != nullptr, "installed Chinese and English brand icons decode");
        DestroyIcon(icon);
    }
    ComPtr<ITfDocumentMgr> focusedDocument;
    ComPtr<ITfContext> focusedContext;
    hr(manager->GetFocus(&focusedDocument), "query actual focused document");
    if (focusedDocument)
        focusedDocument->GetTop(&focusedContext);
    check(focusedContext.Get() == context.Get(), "Windows focused the test ACP context");
    auto key = [&](WPARAM keycode) {
        BOOL interested = FALSE, eaten = FALSE;
        hr(keys->TestKeyDown(keycode, 1, &interested), "Windows TestKeyDown dispatch");
        check(interested, "installed input method claims the key through Windows");
        hr(keys->KeyDown(keycode, 1, &eaten), "Windows KeyDown dispatch");
        check(eaten, "installed input method consumes dispatched key");
        pump();
    };
    for (auto keycode : std::wstring(L"NIHAO"))
        key(keycode);
    check(!store->text.empty(), "installed DLL writes inline preedit through TSF");
    auto candidateWindow = FindWindowW(L"R-IME.CandidateWindow.v1", nullptr);
    if (uiLess)
        check(!candidateWindow || !IsWindowVisible(candidateWindow),
              "UI-less host denying visibility gets no input-method candidate window");
    else {
        check(candidateWindow && IsWindowVisible(candidateWindow),
              "installed candidate window is visible while composing");
        DWORD candidateProcess = 0;
        GetWindowThreadProcessId(candidateWindow, &candidateProcess);
        check(candidateProcess == GetCurrentProcessId(), "candidate belongs to this test host");
        RECT candidateBounds{};
        GetWindowRect(candidateWindow, &candidateBounds);
        check(candidateBounds.right > candidateBounds.left &&
                  candidateBounds.bottom > candidateBounds.top &&
                  MonitorFromRect(&candidateBounds, MONITOR_DEFAULTTONULL),
              "candidate window has a nonempty on-screen rectangle");
    }
    check(candidateHost->begins > 0 && candidateHost->updates > 0,
          "installed host receives candidate UI lifecycle");
    ComPtr<ITfUIElement> currentUi;
    hr(uiManager->GetUIElement(candidateHost->element, &currentUi),
       "installed host gets candidate UI");
    ComPtr<ITfCandidateListUIElementBehavior> currentCandidates;
    hr(currentUi.As(&currentCandidates), "installed candidate behavior");
    BSTR firstCandidate = nullptr;
    hr(currentCandidates->GetString(0, &firstCandidate), "installed host reads Chinese candidate");
    std::wstring firstText(firstCandidate, SysStringLen(firstCandidate));
    SysFreeString(firstCandidate);
    check(firstText == L"你好", "host UI receives the actual Rime Chinese candidate");
    key(VK_SPACE);
    check(store->text == L"你好", "installed profile commits Chinese via Windows dispatch");
    check(!IsWindowVisible(candidateWindow), "committing hides the candidate window");
    check(candidateHost->ends > 0, "installed commit ends the candidate UI element");
    currentCandidates.Reset();
    currentUi.Reset();
    if (uiLess) {
        candidateHost->allowWindow = true;
        for (auto code : std::wstring(L"NIHAO"))
            key(code);
        candidateWindow = FindWindowW(L"R-IME.CandidateWindow.v1", nullptr);
        check(candidateWindow && IsWindowVisible(candidateWindow),
              "UI-less host granting visibility permits the input method's own candidates");
        hr(uiManager->GetUIElement(candidateHost->element, &currentUi),
           "host retrieves allowed UI");
        hr(currentUi->Show(FALSE), "host hides active candidate window");
        check(!IsWindowVisible(candidateWindow), "active UI Show(FALSE) is respected");
        hr(currentUi->Show(TRUE), "host restores active candidate window");
        pump();
        check(IsWindowVisible(candidateWindow), "active UI Show(TRUE) is respected");
        key(VK_ESCAPE);
        currentUi.Reset();
    }
    key('N');
    key(VK_ESCAPE);
    check(store->text == L"你好", "installed profile cancels without extra text");
    hr(profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id,
                                   nullptr, TF_IPPMF_FORPROCESS),
       "deactivate installed profile");
    keys.Reset();
    threadSource->UnadviseSink(candidateCookie);
    candidateHost.Reset();
    uiManager.Reset();
    threadSource.Reset();
    document->Pop(TF_POPF_ALL);
    context.Reset();
    document.Reset();
    manager->Deactivate();
    manager.Reset();
    store.Reset();
    std::cout << "Installed Windows TSF profile: system COM activation, real Windows key dispatch, "
                 "inline preedit, Chinese commit and cancellation passed\n";
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--contract") {
        try {
            candidate_contract(argv[2]);
            std::cout << "Windows candidate-list and input-mode contracts passed\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    auto record = [&](bool success, const char *message) {
        bool installedResult = argc == 4 && (std::wstring(argv[1]) == L"--installed" ||
                                             std::wstring(argv[1]) == L"--installed-ui-less");
        bool hostResult = argc == 3 && std::wstring(argv[1]) != L"--contract" &&
                          std::wstring(argv[1]) != L"--installed" &&
                          std::wstring(argv[1]) != L"--installed-ui-less";
        if (installedResult || hostResult) {
            auto path = installedResult ? argv[3] : argv[2];
            std::ofstream output{std::filesystem::path(path), std::ios::trunc};
            output << nlohmann::json{{"success", success},
                                     {"message", message},
                                     {"bits", sizeof(void *) * 8}}
                          .dump(2)
                   << '\n';
            if (!output)
                throw std::runtime_error("Cannot write installed-test result file");
        }
    };
    auto root = std::filesystem::temp_directory_path() /
                ("r-ime-tsf-test-" + std::to_string(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"RIME_USER_ROOT", root.c_str());
    SetEnvironmentVariableW(L"RIME_TSF_TRACE", L"1");
    HANDLE ownedWorker = nullptr;
    auto shutdown = [&] {
        try {
            rime_ime::Client client(rime_ime::Paths::from_environment().runtime / "engine.sock");
            client.call({{"op", "shutdown"}}, 1000);
        } catch (...) {
        }
        if (ownedWorker) {
            if (WaitForSingleObject(ownedWorker, 5000) == WAIT_TIMEOUT) {
                // This is exactly the process created by this test, with its
                // unique RIME_USER_ROOT. Never terminate the user's real engine.
                TerminateProcess(ownedWorker, 1);
                WaitForSingleObject(ownedWorker, 5000);
            }
            CloseHandle(ownedWorker);
            ownedWorker = nullptr;
        }
    };
    struct ShutdownGuard {
        std::function<void()> action;
        ~ShutdownGuard() {
            action();
        }
    } workerGuard{shutdown};
    try {
        bool uiLess = argc >= 2 && std::wstring(argv[1]) == L"--installed-ui-less";
        bool installed = argc >= 2 && (std::wstring(argv[1]) == L"--installed" || uiLess);
        check(installed ? argc == 3 || argc == 4 : argc == 2 || argc == 3,
              "usage: windows-tsf-test r-ime-tsf.dll [result.json] | --installed "
              "<installation-directory> "
              "[result.json]");
        auto workerDirectory = installed ? std::filesystem::path(argv[2])
                                          : std::filesystem::absolute(argv[0]).parent_path();
        if (!installed && !std::filesystem::exists(workerDirectory / L"r-ime-engine.exe") &&
            std::filesystem::exists(workerDirectory.parent_path() / L"r-ime-engine.exe"))
            workerDirectory = workerDirectory.parent_path();
        // Retain the handle for every private test worker, including local DLL
        // tests, so failed startup/IPC cannot leave a process locking the build.
        check(start_installed_worker(workerDirectory, ownedWorker),
              "start native engine from requested installation");
        {
            rime_ime::Client worker(rime_ime::Paths::from_environment().runtime / "engine.sock");
            bool ready = false;
            for (int i = 0; i < 600; ++i) {
                try {
                    worker.call({{"op", "status"}}, 1000);
                    ready = true;
                    break;
                } catch (...) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            check(ready, "native worker readiness");
        }
        if (installed) {
            installed_test(uiLess);
            record(true,
                   uiLess ? "Installed UI-less host, public candidate list, host-controlled "
                            "visibility TRUE/FALSE, Chinese commit and cancellation passed"
                          : "Installed system profile, visible on-screen candidates, real Windows "
                            "key dispatch, Chinese commit and cancellation passed");
            return 0;
        }
        hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialize");
        HMODULE dll = LoadLibraryW(argv[1]);
        check(dll != nullptr, "TSF DLL must load");
        for (int resource : {101, 102}) {
            check(FindResourceW(dll, MAKEINTRESOURCEW(resource), RT_GROUP_ICON) != nullptr,
                  "TSF DLL contains its own Chinese and English R-IME icons");
        }
        using GetFactory = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
        auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(dll, "DllGetClassObject"));
        check(getFactory, "class factory export");
        ComPtr<IClassFactory> factory;
        hr(getFactory(service_id, IID_PPV_ARGS(&factory)), "get class factory");
        ComPtr<TrackingFactory> tracking;
        tracking.Attach(new TrackingFactory(factory.Get()));
        DWORD classCookie;
        hr(CoRegisterClassObject(service_id, tracking.Get(), CLSCTX_INPROC_SERVER,
                                 REGCLS_MULTIPLEUSE, &classCookie),
           "register test class");
        ComPtr<ITfInputProcessorProfileMgr> profiles;
        hr(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&profiles)),
           "profile manager");
        hr(profiles->RegisterProfile(service_id, 0x0804, profile_id, L"R-IME test", 10, argv[1],
                                     static_cast<ULONG>(wcslen(argv[1])), 0, nullptr, 0, TRUE,
                                     TF_RP_LOCALPROCESS),
           "test profile");
        ComPtr<ITfThreadMgr> manager;
        hr(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&manager)),
           "create real TSF manager");
        TfClientId client;
        hr(manager->Activate(&client), "activate manager");
        ComPtr<ITfMessagePump> messagePump;
        hr(manager.As(&messagePump), "unit host TSF message pump");
        tsfMessagePump = messagePump.Get();
        for (int resource : {101, 102}) {
            auto icon = static_cast<HICON>(
                LoadImageW(dll, MAKEINTRESOURCEW(resource), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
            check(icon, "embedded R-IME icons decode successfully");
            DestroyIcon(icon);
        }
        WNDCLASSW unitClass{};
        unitClass.hInstance = GetModuleHandleW(nullptr);
        unitClass.lpfnWndProc = DefWindowProcW;
        unitClass.lpszClassName = L"R-IME.TSF.UnitDocument";
        RegisterClassW(&unitClass);
        HWND window = CreateWindowExW(0, unitClass.lpszClassName, L"R-IME TSF integration",
                                      WS_OVERLAPPEDWINDOW, 100, 100, 600, 250, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        check(window, "create document window");
        ComPtr<Store> store;
        store.Attach(new Store(window));
        ComPtr<ITfDocumentMgr> document;
        hr(manager->CreateDocumentMgr(&document), "create document");
        ComPtr<ITfContext> context;
        TfEditCookie cookie;
        hr(document->CreateContext(client, 0, store.Get(), &context, &cookie),
           "create ACP context");
        hr(document->Push(context.Get()), "push context");
        hr(manager->SetFocus(document.Get()), "focus TSF document");
        ComPtr<ITfDocumentMgr> previousDocument;
        hr(manager->AssociateFocus(window, document.Get(), &previousDocument),
           "associate test document with real window focus");
        ShowWindow(window, SW_SHOWNORMAL);
        SetForegroundWindow(window);
        SetFocus(window);
        pump();
        hr(profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id,
                                     nullptr,
                                     TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE |
                                         TF_IPPMF_ENABLEPROFILE),
           "activate test profile");
        pump();
        bool foregroundReady = false;
        ComPtr<ITfKeystrokeMgr> dispatcher;
        hr(manager.As(&dispatcher), "unit Windows keystroke dispatcher");
        for (int attempt = 0; attempt < 250; ++attempt) {
            pump();
            CLSID foreground{};
            dispatcher->GetForeground(&foreground);
            BOOL threadFocus = FALSE;
            manager->IsThreadFocus(&threadFocus);
            if (tracking->active && threadFocus && GetForegroundWindow() == window &&
                foreground == service_id) {
                foregroundReady = true;
                break;
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        }
        auto service = tracking->active;
        check(service != nullptr, "TSF activated the test text service");
        check(foregroundReady,
              "Native TSF host requires a foreground desktop window; run interactively or under "
              "the Windows debugger. Background CTest cannot claim desktop foreground focus.");
        dispatcher.Reset();
        ComPtr<ITfFunctionProvider> functions;
        hr(manager->GetFunctionProvider(service_id, &functions),
           "thread manager publishes the Windows settings provider");
        ComPtr<IUnknown> configure;
        hr(functions->GetFunction(GUID_NULL, IID_ITfFnConfigure, &configure),
           "thread manager's provider exposes properties");
        configure.Reset();
        functions.Reset();
        ComPtr<ITfSource> threadSource;
        hr(manager.As(&threadSource), "thread event source");
        ComPtr<CandidateHost> candidateHost;
        candidateHost.Attach(new CandidateHost);
        DWORD candidateSinkCookie;
        hr(threadSource->AdviseSink(IID_ITfUIElementSink, candidateHost.Get(),
                                    &candidateSinkCookie),
           "subscribe host-side candidate UI control");
        ComPtr<ITfUIElementMgr> uiManager;
        hr(manager.As(&uiManager), "Windows candidate UI manager");
        ComPtr<ITfKeyEventSink> keys;
        hr(service.As(&keys), "key sink");
        auto key = [&](WPARAM code) {
            BOOL eaten = FALSE;
            hr(keys->OnKeyDown(context.Get(), code, 1, &eaten), "OnKeyDown");
            pump();
            if (!eaten && code >= 'A' && code <= 'Z' && store->text.empty()) {
                BOOL threadFocus = FALSE;
                manager->IsThreadFocus(&threadFocus);
                ComPtr<ITfLangBarItemButton> bar;
                service.As(&bar);
                BSTR mode = nullptr;
                DWORD barStatus = 0;
                if (bar) {
                    bar->GetText(&mode);
                    bar->GetStatus(&barStatus);
                }
                std::cerr << "Unconsumed initial letter: threadFocus=" << threadFocus
                          << " windowForeground=" << (GetForegroundWindow() == window)
                          << " barStatus=" << barStatus
                          << " ASCII=" << (mode && wcscmp(mode, L"A") == 0)
                          << " Shift=" << GetKeyState(VK_SHIFT)
                          << " Control=" << GetKeyState(VK_CONTROL)
                          << " Alt=" << GetKeyState(VK_MENU)
                          << " CapsLock=" << GetKeyState(VK_CAPITAL) << '\n';
                SysFreeString(mode);
                ComPtr<ITfCompartmentMgr> modes;
                manager.As(&modes);
                for (auto id : {GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION}) {
                    ComPtr<ITfCompartment> compartment;
                    modes->GetCompartment(id, &compartment);
                    VARIANT value;
                    VariantInit(&value);
                    auto result = compartment->GetValue(&value);
                    std::cerr << "Thread mode compartment "
                              << (id == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE ? "open" : "conversion")
                              << " HRESULT=" << std::hex << result << std::dec
                              << " type=" << value.vt
                              << " value=" << (value.vt == VT_I4 ? value.lVal : -1) << '\n';
                    VariantClear(&value);
                }
                TF_STATUS documentStatus{};
                context->GetStatus(&documentStatus);
                std::cerr << "Document dynamic status=" << documentStatus.dwDynamicFlags << '\n';
            }
            return eaten;
        };
        for (auto c : std::wstring(L"nihao"))
            check(key(c - L'a' + L'A'), "pinyin key consumed");
        check(!store->text.empty(), "inline preedit written via TSF edit session");
        {
            class ReadStyle final : public ITfEditSession {
                ULONG refs_ = 1;
                ComPtr<ITfContext> context_;
                bool &found_;

              public:
                ReadStyle(ITfContext *context, bool &found) : context_(context), found_(found) {}
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
                    if (!p)
                        return E_POINTER;
                    *p = nullptr;
                    if (id == IID_IUnknown || id == IID_ITfEditSession)
                        *p = this;
                    if (!*p)
                        return E_NOINTERFACE;
                    AddRef();
                    return S_OK;
                }
                ULONG STDMETHODCALLTYPE AddRef() override {
                    return ++refs_;
                }
                ULONG STDMETHODCALLTYPE Release() override {
                    auto count = --refs_;
                    if (!count)
                        delete this;
                    return count;
                }
                HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
                    TF_SELECTION selection{};
                    ULONG count = 0;
                    auto status =
                        context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count);
                    if (FAILED(status) || !count)
                        return E_FAIL;
                    ComPtr<ITfRange> range;
                    range.Attach(selection.range);
                    LONG moved = 0;
                    range->ShiftStart(cookie, -1, &moved, nullptr);
                    ComPtr<ITfProperty> property;
                    status = context_->GetProperty(GUID_PROP_ATTRIBUTE, &property);
                    if (FAILED(status))
                        return status;
                    VARIANT value;
                    VariantInit(&value);
                    status = property->GetValue(cookie, range.Get(), &value);
                    found_ = SUCCEEDED(status) && value.vt == VT_I4 && value.lVal != 0;
                    VariantClear(&value);
                    return status;
                }
            };
            bool found = false;
            ComPtr<ITfEditSession> reader;
            reader.Attach(new ReadStyle(context.Get(), found));
            HRESULT result;
            hr(context->RequestEditSession(client, reader.Get(), TF_ES_SYNC | TF_ES_READ, &result),
               "read native preedit formatting through TSF");
            hr(result, "native preedit formatting edit session");
            check(found, "preedit range carries the native display-attribute atom");
        }
        check(key(VK_SPACE), "space consumed");
        check(store->text == L"你好", "space commits exactly one Chinese candidate");
        for (auto c : std::wstring(L"nihao"))
            check(key(c - L'a' + L'A'), "pinyin before click");
        HWND candidate = FindWindowW(L"R-IME.CandidateWindow.v1", nullptr);
        check(candidate && IsWindowVisible(candidate), "native candidate window is visible");
        check((GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
              "candidate has no-activate style");
        check(SendMessageW(candidate, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE,
              "candidate rejects mouse activation");
        auto foreground = GetForegroundWindow();
        int click = MulDiv(20, GetDpiForWindow(candidate), 96);
        SendMessageW(candidate, WM_LBUTTONDOWN, 0, MAKELPARAM(click, click));
        SendMessageW(candidate, WM_LBUTTONUP, 0, MAKELPARAM(click, click));
        pump();
        check(store->text == L"你好你好", "candidate click commits through TSF");
        check(GetForegroundWindow() == foreground, "candidate click preserves foreground focus");
        SendMessageW(candidate, WM_LBUTTONUP, 0, MAKELPARAM(click, click));
        pump();
        check(store->text == L"你好你好", "repeated stale click never commits twice");
        auto beforeHostCommit = store->text;
        candidateHost->allowWindow = false;
        for (auto code : std::wstring(L"NIHAO"))
            check(key(code), "pinyin in host-rendered candidate mode");
        check(candidateHost->begins > 0 && candidateHost->updates > 0,
              "Windows host receives candidate lifecycle and updates");
        check(!IsWindowVisible(candidate), "host denial hides the input method's own window");
        ComPtr<ITfUIElement> ui;
        hr(uiManager->GetUIElement(candidateHost->element, &ui),
           "host retrieves current candidate UI");
        ComPtr<ITfCandidateListUIElementBehavior> hostCandidates;
        hr(ui.As(&hostCandidates), "host retrieves selectable candidate list");
        UINT candidateCount = 0, selected = 0;
        hr(hostCandidates->GetCount(&candidateCount), "host candidate count");
        check(candidateCount >= 2, "host sees real Rime candidate strings");
        hr(hostCandidates->SetSelection(1), "host selects second candidate without committing");
        pump();
        hr(hostCandidates->GetSelection(&selected), "host selected index");
        check(selected == 1, "host selection agrees with engine selection");
        BSTR chosen = nullptr;
        hr(hostCandidates->GetString(1, &chosen), "host selected candidate string");
        std::wstring chosenText(chosen, SysStringLen(chosen));
        SysFreeString(chosen);
        hr(hostCandidates->Finalize(), "host finalizes selected candidate");
        pump();
        check(store->text == beforeHostCommit + chosenText,
              "host finalization commits exactly selected text");
        check(candidateHost->ends > 0, "committing ends the TSF candidate UI lifecycle");
        check(!IsWindowVisible(candidate), "host finalization leaves no floating candidate window");
        hostCandidates.Reset();
        ui.Reset();
        candidateHost->allowWindow = true;
        for (auto code : std::wstring(L"NIHAO"))
            check(key(code),
                  "composition before an asynchronously deferred candidate finalization");
        hr(uiManager->GetUIElement(candidateHost->element, &ui),
           "candidate UI before deferred selection");
        hr(ui.As(&hostCandidates), "candidate behavior before deferred selection");
        store->deferLocks = true;
        hr(hostCandidates->Finalize(),
           "queue candidate finalization while application owns its lock");
        check(store->hasDeferredLock(), "application really deferred the selection edit lock");
        store->deferLocks = false;
        check(key('N'), "a synchronous newer key updates candidates before the deferred selection");
        auto newestPreedit = store->text;
        store->grantDeferredLock();
        pump();
        check(store->text == newestPreedit,
              "a queued stale selection cannot commit newer candidates");
        check(key(VK_ESCAPE), "cancel composition after stale selection was discarded");
        hostCandidates.Reset();
        ui.Reset();
        check(key('N'), "composition for layout tracking");
        RECT originalCandidate{};
        GetWindowRect(candidate, &originalCandidate);
        store->layoutOrigin.y += 90;
        ComPtr<ITfTextLayoutSink> layoutSink;
        hr(service.As(&layoutSink), "TIP subscribes to document layout changes");
        hr(layoutSink->OnLayoutChange(context.Get(), TF_LC_CHANGE, nullptr),
           "document scroll layout change");
        pump();
        RECT movedCandidate{};
        GetWindowRect(candidate, &movedCandidate);
        check(movedCandidate.top != originalCandidate.top,
              "candidate follows caret after a layout change");
        check(key(VK_ESCAPE), "cancel layout tracking composition");
        auto baseline = store->text;
        check(key('N'), "new composition");
        check(key(VK_ESCAPE), "escape consumed");
        check(store->text == baseline, "escape cancels without committing");
        check(key('N'), "composition before focus loss");
        ComPtr<ITfThreadFocusSink> focus;
        hr(service.As(&focus), "focus sink");
        hr(focus->OnKillThreadFocus(), "lose focus");
        pump();
        check(store->text == baseline, "focus loss removes preedit");
        hr(keys->OnSetFocus(TRUE), "Windows key sink regains keyboard focus");
        check(key('N'), "keyboard focus notification restores input after thread focus loss");
        hr(keys->OnSetFocus(FALSE), "Windows key sink loses keyboard focus");
        pump();
        check(store->text == baseline, "keyboard focus loss clears uncommitted composition");
        check(!key('N'), "inactive key sink passes letters through");
        hr(focus->OnSetThreadFocus(), "regain thread focus");
        ComPtr<ITfCompartmentMgr> compartments;
        hr(context.As(&compartments), "context compartments");
        ComPtr<ITfCompartment> disabled;
        hr(compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED, &disabled),
           "disabled compartment");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = 1;
        hr(disabled->SetValue(client, &value), "disable sensitive context");
        check(!key('N'), "protected input passes through");
        check(store->text == baseline, "protected input is never sent to document");
        value.lVal = 0;
        hr(disabled->SetValue(client, &value), "enable context");
        ComPtr<ITfCompartmentMgr> threadCompartments;
        hr(manager.As(&threadCompartments), "Windows thread mode compartments");
        ComPtr<ITfCompartment> openClose;
        hr(threadCompartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openClose),
           "Windows IME open/close mode");
        hr(openClose->SetValue(client, &value), "Windows closes IME");
        pump();
        check(!key('N'), "Windows closed IME mode passes letters through");
        value.lVal = 1;
        hr(openClose->SetValue(client, &value), "Windows opens IME");
        pump();
        check(key('N'), "worker remains usable after sensitive context");
        check(key(VK_RETURN), "raw Enter consumed");
        check(store->text == baseline + L"n", "Enter commits raw input");
        check(key('N'), "composition before Shift tap");
        auto shiftTap = [&] {
            BOOL eaten = FALSE;
            hr(keys->OnTestKeyDown(context.Get(), VK_SHIFT, 1, &eaten), "test Shift down");
            check(!eaten, "Shift down passes to application");
            hr(keys->OnTestKeyUp(context.Get(), VK_SHIFT, 1, &eaten), "test Shift up");
            check(eaten, "Shift tap is recognized");
            hr(keys->OnKeyUp(context.Get(), VK_SHIFT, 1, &eaten), "Shift up");
            pump();
            check(eaten, "Shift toggles mode");
        };
        shiftTap();
        check(store->text == baseline + L"nn", "Shift commits raw composition before toggling");
        check(!key('N'), "ASCII mode passes ordinary letters through");
        shiftTap();
        BYTE original[256]{}, control[256]{};
        GetKeyboardState(original);
        std::copy_n(original, 256, control);
        shiftTap();
        BYTE shiftState[256]{};
        std::copy_n(original, 256, shiftState);
        shiftState[VK_SHIFT] = 0x80;
        SetKeyboardState(shiftState);
        BOOL shapeToggle = key(VK_SPACE);
        SetKeyboardState(original);
        check(shapeToggle, "Shift+Space enables full-width input in English mode");
        auto beforeFullWidth = store->text;
        check(key('A') && key('1') && key(VK_SPACE),
              "full-width letters, digits and spaces consumed");
        check(store->text == beforeFullWidth + L"ａ１　",
              "full-width input produces Windows-style forms");
        SetKeyboardState(shiftState);
        shapeToggle = key(VK_SPACE);
        SetKeyboardState(original);
        check(shapeToggle && !key('N'), "Shift+Space restores half-width English pass-through");
        shiftTap();
        control[VK_CONTROL] = 0x80;
        SetKeyboardState(control);
        BOOL consumed = FALSE;
        keys->OnTestKeyDown(context.Get(), 'C', 1, &consumed);
        SetKeyboardState(original);
        check(!consumed, "Control+C passes through without composition");
        auto cfg = rime_ime::load_config(rime_ime::Paths::from_environment().config);
        cfg["shortcuts"]["cancel"] = "F8";
        cfg["shortcuts"]["toggle_ascii"] = "Control+j";
        {
            rime_ime::Client worker(rime_ime::Paths::from_environment().runtime / "engine.sock");
            worker.call({{"op", "configure"}, {"config", cfg}}, 180000);
        }
        baseline = store->text;
        check(key('N'), "composition after live settings change");
        check(key(VK_F8), "custom cancel binding refreshed live");
        check(store->text == baseline, "custom cancel removes preedit");
        SetKeyboardState(control);
        BOOL toggle = key('J');
        SetKeyboardState(original);
        check(toggle, "custom Control+j toggles without composition");
        check(!key('N'), "custom binding selected ASCII mode");
        SetKeyboardState(control);
        toggle = key('J');
        SetKeyboardState(original);
        check(toggle, "custom Control+j toggles back");
        store->readonly = true;
        check(!key('N'), "read-only context passes through");
        store->readonly = false;
        store->failTextWrites = true;
        check(!key('N'), "application rejecting preedit must not lose the key");
        store->failTextWrites = false;
        check(store->text == baseline, "failed preedit writes do not alter committed text");
        store->deferLocks = true;
        check(!key('N'), "an unavailable synchronous edit lock must pass the key through");
        store->deferLocks = false;
        check(store->text == baseline, "deferred edit access cannot consume or commit a key");
        hr(profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id,
                                       profile_id, nullptr, TF_IPPMF_FORPROCESS),
           "deactivate test profile");
        threadSource->UnadviseSink(candidateSinkCookie);
        uiManager.Reset();
        candidateHost.Reset();
        threadSource.Reset();
        layoutSink.Reset();
        openClose.Reset();
        threadCompartments.Reset();
        keys.Reset();
        focus.Reset();
        service.Reset();
        document->Pop(TF_POPF_ALL);
        context.Reset();
        document.Reset();
        manager->Deactivate();
        manager.Reset();
        tsfMessagePump = nullptr;
        messagePump.Reset();
        store.Reset();
        factory.Reset();
        CoRevokeClassObject(classCookie);
        tracking.Reset();
        profiles.Reset();
        disabled.Reset();
        compartments.Reset();
        DestroyWindow(window);
        CoUninitialize();
        FreeLibrary(dll);
        record(true, "Desktop TSF host: activation, candidate UI lifecycle/host selection, "
                     "stale selection protection, layout, focus, modes, protected input and "
                     "live shortcuts passed");
        std::cout << "TSF activation, preedit, commit, candidate click, no-focus-steal, cancel, "
                     "focus, protected input, Shift and live shortcuts passed\n";
        return 0;
    } catch (const std::exception &e) {
        try {
            record(false, e.what());
        } catch (...) {
        }
        std::cerr << e.what() << '\n';
        return 1;
    }
}
