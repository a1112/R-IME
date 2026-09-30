#include "client.hpp"
#include <algorithm>
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

  public:
    HWND window;
    std::wstring text;
    LONG start = 0, end = 0;
    bool readonly = false;
    bool deferLocks = false;
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
        *r = {100 + a * 10, 100, 110 + b * 10, 124};
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
static void installed_test() {
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
    hr(manager->Activate(&client), "activate TSF thread manager");
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
    key(VK_SPACE);
    check(store->text == L"你好", "installed profile commits Chinese via Windows dispatch");
    check(!IsWindowVisible(candidateWindow), "committing hides the candidate window");
    key('N');
    key(VK_ESCAPE);
    check(store->text == L"你好", "installed profile cancels without extra text");
    hr(profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id,
                                   nullptr, TF_IPPMF_FORPROCESS),
       "deactivate installed profile");
    keys.Reset();
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
    auto record = [&](bool success, const char *message) {
        if (argc == 4 && std::wstring(argv[1]) == L"--installed") {
            std::ofstream output{std::filesystem::path(argv[3]), std::ios::trunc};
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
        bool installed = argc >= 2 && std::wstring(argv[1]) == L"--installed";
        check(installed ? argc == 3 || argc == 4 : argc == 2,
              "usage: windows-tsf-test r-ime-tsf.dll | --installed <installation-directory> "
              "[result.json]");
        check(installed ? start_installed_worker(argv[2], ownedWorker) : rime_ime::start_engine(),
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
            installed_test();
            record(true, "Installed system profile, visible on-screen candidates, real Windows "
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
        for (int resource : {101, 102}) {
            auto icon = static_cast<HICON>(
                LoadImageW(dll, MAKEINTRESOURCEW(resource), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
            check(icon, "embedded R-IME icons decode successfully");
            DestroyIcon(icon);
        }
        HWND window =
            CreateWindowExW(0, L"EDIT", L"R-IME TSF integration", WS_OVERLAPPEDWINDOW, 100, 100,
                            600, 250, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
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
        hr(profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id, profile_id,
                                     nullptr,
                                     TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE |
                                         TF_IPPMF_ENABLEPROFILE),
           "activate test profile");
        pump();
        auto service = tracking->active;
        check(service != nullptr, "TSF activated the test text service");
        ComPtr<ITfKeyEventSink> keys;
        hr(service.As(&keys), "key sink");
        auto key = [&](WPARAM code) {
            BOOL eaten = FALSE;
            hr(keys->OnKeyDown(context.Get(), code, 1, &eaten), "OnKeyDown");
            pump();
            return eaten;
        };
        for (auto c : std::wstring(L"nihao"))
            check(key(c - L'a' + L'A'), "pinyin key consumed");
        check(!store->text.empty(), "inline preedit written via TSF edit session");
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
        hr(focus->OnSetThreadFocus(), "regain focus");
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
        store->deferLocks = true;
        check(!key('N'), "an unavailable synchronous edit lock must pass the key through");
        store->deferLocks = false;
        check(store->text == baseline, "deferred edit access cannot consume or commit a key");
        hr(profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, service_id,
                                       profile_id, nullptr, TF_IPPMF_FORPROCESS),
           "deactivate test profile");
        keys.Reset();
        focus.Reset();
        service.Reset();
        document->Pop(TF_POPF_ALL);
        context.Reset();
        document.Reset();
        manager->Deactivate();
        manager.Reset();
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
