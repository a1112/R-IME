#include "../../ipc/client.hpp"
#include "../platform.hpp"
#include "guids.hpp"
#include "panel.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctfutb.h>
#include <functional>
#include <initguid.h>
#include <inputscope.h>
#include <memory>
#include <msctf.h>
#include <olectl.h>
#include <rime_ime/config.hpp>
#include <shellapi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace rime_ime::tsf {
HINSTANCE module;
std::atomic<long> objects{0};
class Edit final : public ITfEditSession {
    ULONG refs_ = 1;
    std::function<HRESULT(TfEditCookie)> action_;

  public:
    explicit Edit(std::function<HRESULT(TfEditCookie)> action) : action_(std::move(action)) {
        ++objects;
    }
    ~Edit() {
        --objects;
    }
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
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        try {
            return action_(cookie);
        } catch (...) {
            return E_FAIL;
        }
    }
};
class Service final : public ITfTextInputProcessorEx,
                      public ITfKeyEventSink,
                      public ITfThreadMgrEventSink,
                      public ITfThreadFocusSink,
                      public ITfCompositionSink,
                      public ITfLangBarItemButton,
                      public ITfSource {
    ULONG refs_ = 1;
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfContext> context_;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfLangBarItemSink> barSink_;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    DWORD threadSink_ = TF_INVALID_COOKIE, focusSink_ = TF_INVALID_COOKIE;
    bool keySink_ = false, barAdded_ = false, shiftPending_ = false, ascii_ = false,
         focused_ = true;
    std::uint64_t session_ = 0, epoch_ = 0, revision_ = 0;
    Client client_{Paths::from_environment().runtime / "engine.sock"};
    Json config_ = default_config(), state_ = Json::object();
    Panel panel_{module, [this](int index, std::uint64_t revision) {
                     if (!context_ || revision != revision_ || !focused_)
                         return;
                     if (index == -1 && state_.value("page", 0) == 0)
                         return;
                     if (index == -2 && state_.value("last_page", true))
                         return;
                     request(context_.Get(), index < 0
                                                 ? Json{{"op", "page"}, {"backward", index == -1}}
                                                 : Json{{"op", "select"}, {"index", index}});
                 }};
    std::chrono::steady_clock::time_point nextConnection_{};
    std::filesystem::file_time_type configStamp_{};
    void refresh_config() {
        try {
            auto path = Paths::from_environment().config;
            auto stamp = std::filesystem::exists(path) ? std::filesystem::last_write_time(path)
                                                       : std::filesystem::file_time_type{};
            if (stamp != configStamp_) {
                config_ = load_config(path);
                configStamp_ = stamp;
            }
        } catch (...) {
        } // The worker retains the last valid deployed configuration.
    }
    bool matches(WPARAM key, const std::string &binding) {
        unsigned required = 0;
        size_t start = 0, next;
        while ((next = binding.find('+', start)) != std::string::npos) {
            auto modifier = binding.substr(start, next - start);
            start = next + 1;
            if (modifier == "Shift")
                required |= 1;
            if (modifier == "Control")
                required |= 2;
            if (modifier == "Alt")
                required |= 4;
            if (modifier == "Super")
                required |= 8;
        }
        unsigned actual = ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) |
                          ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
                          ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0) |
                          (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? 8 : 0);
        if (required != actual)
            return false;
        auto name = binding.substr(start);
        if (name == "Page_Up")
            return key == VK_PRIOR;
        if (name == "Page_Down")
            return key == VK_NEXT;
        if (name == "Escape")
            return key == VK_ESCAPE;
        if (name == "Tab")
            return key == VK_TAB;
        if (name.size() > 1 && name[0] == 'F')
            return key == VK_F1 + std::stoi(name.substr(1)) - 1;
        return name.size() == 1 &&
               key == static_cast<WPARAM>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    Json shortcut(WPARAM key) {
        auto binding = config_["shortcuts"]["toggle_ascii"].get<std::string>();
        if (binding != "Shift" && matches(key, binding))
            return {{"op", "toggle_ascii"}};
        if (!state_.value("preedit", std::string{}).empty()) {
            if (matches(key, config_["shortcuts"]["next_page"]))
                return {{"op", "page"}, {"backward", false}};
            if (matches(key, config_["shortcuts"]["previous_page"]))
                return {{"op", "page"}, {"backward", true}};
            if (matches(key, config_["shortcuts"]["cancel"]))
                return {{"op", "reset"}};
        }
        return Json();
    }
    static bool shift(WPARAM key) {
        return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT;
    }
    static bool chord() {
        return (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
               (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
    }
    bool compartment(ITfContext *context, REFGUID id) {
        ComPtr<ITfCompartmentMgr> manager;
        ComPtr<ITfCompartment> value;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&manager))) ||
            FAILED(manager->GetCompartment(id, &value)))
            return false;
        VARIANT data;
        VariantInit(&data);
        bool yes = false;
        if (SUCCEEDED(value->GetValue(&data)))
            yes = data.vt == VT_I4 && data.lVal != 0;
        VariantClear(&data);
        return yes;
    }
    bool blocked(ITfContext *context) {
        if (!context || !focused_)
            return true;
        TF_STATUS status{};
        if (SUCCEEDED(context->GetStatus(&status)) && (status.dwDynamicFlags & TS_SD_READONLY))
            return true;
        if (compartment(context, GUID_COMPARTMENT_KEYBOARD_DISABLED) ||
            compartment(context, GUID_COMPARTMENT_EMPTYCONTEXT))
            return true;
        ComPtr<ITfContextView> view;
        HWND window = nullptr;
        if (SUCCEEDED(context->GetActiveView(&view)) && SUCCEEDED(view->GetWnd(&window)) &&
            window) {
            wchar_t name[32]{};
            GetClassNameW(window, name, 32);
            if ((_wcsicmp(name, L"EDIT") == 0 || wcsstr(name, L"RichEdit")) &&
                (GetWindowLongPtrW(window, GWL_STYLE) & ES_PASSWORD))
                return true;
        }
        return false;
    }
    bool private_scope(ITfContext *context, TfEditCookie cookie) {
        TF_SELECTION selection{};
        ULONG count = 0;
        if (FAILED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count)) ||
            !count)
            return false;
        ComPtr<ITfRange> range;
        range.Attach(selection.range);
        ComPtr<ITfReadOnlyProperty> property;
        if (FAILED(context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property)))
            return false;
        VARIANT value;
        VariantInit(&value);
        bool protectedField = false;
        if (SUCCEEDED(property->GetValue(cookie, range.Get(), &value)) && value.vt == VT_UNKNOWN &&
            value.punkVal) {
            ComPtr<ITfInputScope> scope;
            if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&scope)))) {
                InputScope *scopes = nullptr;
                UINT length = 0;
                if (SUCCEEDED(scope->GetInputScopes(&scopes, &length))) {
                    for (UINT i = 0; i < length; ++i)
                        if (scopes[i] == IS_PASSWORD || scopes[i] == IS_PRIVATE)
                            protectedField = true;
                    CoTaskMemFree(scopes);
                }
            }
        }
        VariantClear(&value);
        return protectedField;
    }
    static HRESULT edit(ITfContext *context, TfClientId client,
                        std::function<HRESULT(TfEditCookie)> action, bool asyncFallback = true) {
        auto task = new Edit(std::move(action));
        HRESULT result = E_FAIL;
        HRESULT status =
            context->RequestEditSession(client, task, TF_ES_SYNC | TF_ES_READWRITE, &result);
        if (asyncFallback && (status == TF_E_SYNCHRONOUS || result == TF_E_SYNCHRONOUS))
            status = context->RequestEditSession(client, task,
                                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &result);
        task->Release();
        return FAILED(status) ? status : result;
    }
    void clear_composition(ITfContext *context, ITfComposition *composition) {
        if (!context || !composition)
            return;
        ComPtr<ITfContext> held = context;
        ComPtr<ITfComposition> old = composition;
        edit(context, clientId_, [held, old](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            HRESULT status = old->GetRange(&range);
            if (SUCCEEDED(status))
                status = range->SetText(cookie, 0, L"", 0);
            old->EndComposition(cookie);
            return status;
        });
    }
    void reset(bool releaseContext = true) {
        ++epoch_;
        ++revision_;
        shiftPending_ = false;
        panel_.hide();
        state_ = Json::object();
        auto oldComposition = composition_;
        composition_.Reset();
        clear_composition(context_.Get(), oldComposition.Get());
        if (session_)
            try {
                client_.call({{"op", "destroy"}, {"session", session_}}, 40);
            } catch (...) {
                client_.disconnect();
            }
        session_ = 0;
        if (releaseContext)
            context_.Reset();
    }
    bool ensure_session(ITfContext *context) {
        if (context_.Get() != context) {
            reset();
            context_ = context;
        }
        if (session_)
            return true;
        auto now = std::chrono::steady_clock::now();
        if (now < nextConnection_)
            return false;
        try {
            auto result = client_.call({{"op", "create"}, {"session", 0}}, 40);
            session_ = result.at("session").get<std::uint64_t>();
            if (result.value("ascii", false) != ascii_)
                result = client_.call({{"op", "toggle_ascii"}, {"session", session_}}, 40);
            state_ = std::move(result);
            return true;
        } catch (...) {
            client_.disconnect();
            start_engine();
            nextConnection_ = now + std::chrono::milliseconds(250);
            return false;
        }
    }
    static HRESULT selection_end(ITfContext *context, TfEditCookie cookie, ITfRange *range) {
        ComPtr<ITfRange> cursor;
        auto status = range->Clone(&cursor);
        if (FAILED(status))
            return status;
        cursor->Collapse(cookie, TF_ANCHOR_END);
        TF_SELECTION selection{cursor.Get(), {TF_AE_NONE, FALSE}};
        return context->SetSelection(cookie, 1, &selection);
    }
    HRESULT apply(ITfContext *context, TfEditCookie cookie, const Json &reply) {
        auto commit = wide(reply.value("commit", ""));
        auto preedit = wide(reply.value("preedit", ""));
        ComPtr<ITfRange> range;
        if (composition_) {
            HRESULT status = composition_->GetRange(&range);
            if (FAILED(status))
                return status;
            if (!commit.empty() || preedit.empty()) {
                status = range->SetText(cookie, 0, commit.data(), static_cast<LONG>(commit.size()));
                if (FAILED(status))
                    return status;
                selection_end(context, cookie, range.Get());
                auto old = composition_;
                composition_.Reset();
                old->EndComposition(cookie);
                range.Reset();
            }
        } else if (!commit.empty()) {
            ComPtr<ITfInsertAtSelection> insert;
            auto status = context->QueryInterface(IID_PPV_ARGS(&insert));
            if (FAILED(status))
                return status;
            status = insert->InsertTextAtSelection(cookie, 0, commit.data(),
                                                   static_cast<LONG>(commit.size()), &range);
            if (FAILED(status))
                return status;
            selection_end(context, cookie, range.Get());
            range.Reset();
        }
        if (!preedit.empty()) {
            if (!composition_) {
                ComPtr<ITfInsertAtSelection> insert;
                auto status = context->QueryInterface(IID_PPV_ARGS(&insert));
                if (FAILED(status))
                    return status;
                status = insert->InsertTextAtSelection(cookie, TF_IAS_QUERYONLY, L"", 0, &range);
                if (FAILED(status))
                    return status;
                ComPtr<ITfContextComposition> owner;
                status = context->QueryInterface(IID_PPV_ARGS(&owner));
                if (FAILED(status))
                    return status;
                status = owner->StartComposition(cookie, range.Get(), this, &composition_);
                if (FAILED(status) || !composition_)
                    return E_FAIL;
            } else if (!range)
                composition_->GetRange(&range);
            auto status =
                range->SetText(cookie, 0, preedit.data(), static_cast<LONG>(preedit.size()));
            if (FAILED(status))
                return status;
            ComPtr<ITfRange> cursor;
            range->Clone(&cursor);
            cursor->Collapse(cookie, TF_ANCHOR_START);
            auto bytes = reply.value("preedit", std::string{});
            size_t pos = std::min<size_t>(reply.value("cursor", 0), bytes.size());
            LONG count = static_cast<LONG>(wide(bytes.substr(0, pos)).size()), moved = 0;
            cursor->ShiftStart(cookie, count, &moved, nullptr);
            cursor->Collapse(cookie, TF_ANCHOR_START);
            TF_SELECTION selection{cursor.Get(), {TF_AE_NONE, FALSE}};
            context->SetSelection(cookie, 1, &selection);
        }
        state_ = reply;
        ascii_ = reply.value("ascii", false);
        ++revision_;
        if (barSink_)
            barSink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT);
        RECT anchor{};
        BOOL clipped = FALSE;
        HWND window = nullptr;
        ComPtr<ITfContextView> view;
        if (SUCCEEDED(context->GetActiveView(&view))) {
            view->GetWnd(&window);
            if (range) {
                ComPtr<ITfRange> caret;
                range->Clone(&caret);
                caret->Collapse(cookie, TF_ANCHOR_END);
                view->GetTextExt(cookie, caret.Get(), &anchor, &clipped);
            }
        }
        if (anchor.right <= anchor.left && anchor.bottom <= anchor.top) {
            GUITHREADINFO info{sizeof(info)};
            if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret) {
                anchor = info.rcCaret;
                MapWindowPoints(info.hwndCaret, nullptr, reinterpret_cast<POINT *>(&anchor), 2);
            } else if (view)
                view->GetScreenExt(&anchor);
        }
        panel_.show(reply, config_["appearance"], anchor, window, revision_);
        return S_OK;
    }
    HRESULT request(ITfContext *context, Json request, BOOL *eaten = nullptr) {
        if (eaten)
            *eaten = FALSE;
        if (blocked(context)) {
            reset();
            return S_OK;
        }
        if (!ensure_session(context))
            return S_OK;
        auto epoch = epoch_;
        ComPtr<ITfContext> held = context;
        ComPtr<ITfTextInputProcessorEx> self = this;
        auto handled = std::make_shared<bool>(false);
        HRESULT result = edit(context, clientId_,
                              [this, self, held, request = std::move(request), epoch,
                               handled](TfEditCookie cookie) mutable {
                                  if (epoch != epoch_ || held.Get() != context_.Get() || !focused_)
                                      return S_FALSE;
                                  if (blocked(held.Get()) || private_scope(held.Get(), cookie)) {
                                      reset();
                                      return S_FALSE;
                                  }
                                  request["session"] = session_;
                                  try {
                                      auto reply = client_.call(request, 40);
                                      *handled = reply.value("handled", false);
                                      return apply(held.Get(), cookie, reply);
                                  } catch (...) {
                                      // An ambiguous transport failure is never replayed into a new
                                      // session. Existing preedit is cancelled before pass-through.
                                      session_ = 0;
                                      client_.disconnect();
                                      ++epoch_;
                                      panel_.hide();
                                      state_ = Json::object();
                                      if (composition_) {
                                          ComPtr<ITfRange> range;
                                          composition_->GetRange(&range);
                                          if (range)
                                              range->SetText(cookie, 0, L"", 0);
                                          auto old = composition_;
                                          composition_.Reset();
                                          old->EndComposition(cookie);
                                      }
                                      start_engine();
                                      nextConnection_ = std::chrono::steady_clock::now() +
                                                        std::chrono::milliseconds(250);
                                      return E_FAIL;
                                  }
                              }, eaten == nullptr);
        if (eaten)
            *eaten = *handled;
        return S_OK;
    }
    bool intended(ITfContext *context, WPARAM key) {
        if (blocked(context))
            return false;
        refresh_config();
        if (!shortcut(key).is_null())
            return true;
        if (chord())
            return false;
        if (shift(key))
            return false;
        bool composing = !state_.value("preedit", std::string{}).empty();
        if (ascii_ && !composing)
            return false;
        if ((key >= 'A' && key <= 'Z') || (key >= VK_OEM_1 && key <= VK_OEM_3) ||
            (key >= VK_OEM_4 && key <= VK_OEM_8))
            return true;
        return composing && ((key >= '0' && key <= '9') || key == VK_SPACE || key == VK_BACK ||
                             key == VK_DELETE || key == VK_RETURN || key == VK_ESCAPE ||
                             key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT ||
                             key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT);
    }
    static std::uint32_t keycode(WPARAM key, LPARAM flags) {
        switch (key) {
        case VK_BACK:
            return 0xff08;
        case VK_RETURN:
            return 0xff0d;
        case VK_ESCAPE:
            return 0xff1b;
        case VK_DELETE:
            return 0xffff;
        case VK_LEFT:
            return 0xff51;
        case VK_UP:
            return 0xff52;
        case VK_RIGHT:
            return 0xff53;
        case VK_DOWN:
            return 0xff54;
        case VK_PRIOR:
            return 0xff55;
        case VK_NEXT:
            return 0xff56;
        case VK_HOME:
            return 0xff50;
        case VK_END:
            return 0xff57;
        case VK_SPACE:
            return 32;
        }
        BYTE state[256]{};
        GetKeyboardState(state);
        wchar_t text[8]{};
        int count = ToUnicodeEx(static_cast<UINT>(key), static_cast<UINT>((flags >> 16) & 0xff),
                                state, text, 8, 4, GetKeyboardLayout(0));
        if (count == 1 && text[0] < 128)
            return text[0];
        if (key >= 'A' && key <= 'Z')
            return static_cast<std::uint32_t>(key - 'A' + 'a');
        return 0;
    }
    void set_focus(ITfDocumentMgr *document) {
        ComPtr<ITfContext> next;
        if (document)
            document->GetTop(&next);
        if (next.Get() != context_.Get()) {
            reset();
            context_ = next;
        }
    }

  public:
    Service() {
        ++objects;
    }
    ~Service() {
        --objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_ITfTextInputProcessor ||
            id == IID_ITfTextInputProcessorEx)
            *p = static_cast<ITfTextInputProcessorEx *>(this);
        else if (id == IID_ITfKeyEventSink)
            *p = static_cast<ITfKeyEventSink *>(this);
        else if (id == IID_ITfThreadMgrEventSink)
            *p = static_cast<ITfThreadMgrEventSink *>(this);
        else if (id == IID_ITfThreadFocusSink)
            *p = static_cast<ITfThreadFocusSink *>(this);
        else if (id == IID_ITfCompositionSink)
            *p = static_cast<ITfCompositionSink *>(this);
        else if (id == IID_ITfLangBarItem || id == IID_ITfLangBarItemButton)
            *p = static_cast<ITfLangBarItemButton *>(this);
        else if (id == IID_ITfSource)
            *p = static_cast<ITfSource *>(this);
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
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr *manager, TfClientId client) override {
        return ActivateEx(manager, client, 0);
    }
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr *manager, TfClientId client, DWORD) override {
        if (!manager)
            return E_INVALIDARG;
        if (manager_)
            return E_UNEXPECTED;
        manager_ = manager;
        clientId_ = client;
        focused_ = true;
        ComPtr<ITfSource> source;
        ComPtr<ITfKeystrokeMgr> keys;
        HRESULT status = manager_->QueryInterface(IID_PPV_ARGS(&source));
        if (SUCCEEDED(status))
            status = source->AdviseSink(IID_ITfThreadMgrEventSink,
                                        static_cast<ITfThreadMgrEventSink *>(this), &threadSink_);
        if (SUCCEEDED(status))
            status = source->AdviseSink(IID_ITfThreadFocusSink,
                                        static_cast<ITfThreadFocusSink *>(this), &focusSink_);
        if (SUCCEEDED(status))
            status = manager_->QueryInterface(IID_PPV_ARGS(&keys));
        if (SUCCEEDED(status)) {
            status = keys->AdviseKeyEventSink(clientId_, this, TRUE);
            keySink_ = SUCCEEDED(status);
        }
        if (FAILED(status)) {
            Deactivate();
            return status;
        }
        ComPtr<ITfLangBarItemMgr> bar;
        if (SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&bar))))
            barAdded_ = SUCCEEDED(bar->AddItem(this));
        try {
            config_ = load_config(Paths::from_environment().config);
            ascii_ = config_["general"]["ascii"].get<bool>();
        } catch (...) {
            config_ = default_config();
        }
        try {
            client_.call({{"op", "status"}, {"session", 0}}, 40);
        } catch (...) {
            start_engine();
        }
        ComPtr<ITfDocumentMgr> document;
        if (SUCCEEDED(manager_->GetFocus(&document)))
            set_focus(document.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        reset();
        if (manager_) {
            ComPtr<ITfKeystrokeMgr> keys;
            if (keySink_ && SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&keys))))
                keys->UnadviseKeyEventSink(clientId_);
            ComPtr<ITfSource> source;
            if (SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&source)))) {
                if (threadSink_ != TF_INVALID_COOKIE)
                    source->UnadviseSink(threadSink_);
                if (focusSink_ != TF_INVALID_COOKIE)
                    source->UnadviseSink(focusSink_);
            }
            ComPtr<ITfLangBarItemMgr> bar;
            if (barAdded_ && SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&bar))))
                bar->RemoveItem(this);
        }
        barSink_.Reset();
        manager_.Reset();
        client_.disconnect();
        keySink_ = barAdded_ = false;
        threadSink_ = focusSink_ = TF_INVALID_COOKIE;
        clientId_ = TF_CLIENTID_NULL;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        if (!foreground)
            reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext *context, WPARAM key, LPARAM flags,
                                            BOOL *eaten) override {
        if (!eaten)
            return E_POINTER;
        refresh_config();
        if (shift(key)) {
            if (!(flags & (1LL << 30)))
                shiftPending_ = config_["shortcuts"]["toggle_ascii"] == "Shift" && !chord() &&
                                !blocked(context);
            *eaten = FALSE;
        } else {
            shiftPending_ = false;
            *eaten = intended(context, key);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext *context, WPARAM key, LPARAM,
                                          BOOL *eaten) override {
        if (!eaten)
            return E_POINTER;
        *eaten = shift(key) && shiftPending_ && !chord() && !blocked(context);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext *context, WPARAM key, LPARAM flags,
                                        BOOL *eaten) override {
        if (!eaten)
            return E_POINTER;
        *eaten = FALSE;
        try {
            refresh_config();
            if (shift(key)) {
                if (!(flags & (1LL << 30)))
                    shiftPending_ = config_["shortcuts"]["toggle_ascii"] == "Shift" && !chord() &&
                                    !blocked(context);
                return S_OK;
            }
            shiftPending_ = false;
            if (!intended(context, key)) {
                if (blocked(context))
                    reset();
                return S_OK;
            }
            auto command = shortcut(key);
            if (!command.is_null())
                return request(context, std::move(command), eaten);
            auto code = keycode(key, flags);
            if (!code)
                return S_OK;
            std::uint32_t mods = (GetKeyState(VK_SHIFT) & 0x8000) ? 1u : 0u;
            return request(context, {{"op", "key"}, {"keycode", code}, {"modifiers", mods}}, eaten);
        } catch (...) {
            reset();
            return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext *context, WPARAM key, LPARAM,
                                      BOOL *eaten) override {
        if (!eaten)
            return E_POINTER;
        *eaten = FALSE;
        if (shift(key)) {
            bool toggle = shiftPending_ && !chord();
            shiftPending_ = false;
            if (toggle)
                try {
                    return request(context, {{"op", "toggle_ascii"}}, eaten);
                } catch (...) {
                    reset();
                }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext *, REFGUID, BOOL *eaten) override {
        if (eaten)
            *eaten = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr *document) override {
        ComPtr<ITfDocumentMgr> owner;
        if (context_ && SUCCEEDED(context_->GetDocumentMgr(&owner)) && owner.Get() == document)
            reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr *focus, ITfDocumentMgr *) override {
        set_focus(focus);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext *) override {
        ComPtr<ITfDocumentMgr> document;
        if (manager_ && SUCCEEDED(manager_->GetFocus(&document)))
            set_focus(document.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext *context) override {
        if (context_.Get() == context)
            reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override {
        focused_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override {
        focused_ = false;
        reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie,
                                                      ITfComposition *composition) override {
        if (composition_.Get() == composition) {
            ComPtr<ITfRange> range;
            if (SUCCEEDED(composition->GetRange(&range)))
                range->SetText(cookie, 0, L"", 0);
            composition_.Reset();
            reset(false);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO *info) override {
        if (!info)
            return E_POINTER;
        *info = {
            service_id, language_bar_id, TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_SHOWNINTRAY, 0, {}};
        wcscpy_s(info->szDescription, L"R-IME 输入模式与设置");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD *status) override {
        if (!status)
            return E_POINTER;
        *status = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR *text) override {
        if (!text)
            return E_POINTER;
        *text = SysAllocString(L"R-IME · 中英文、方案与设置");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT, const RECT *) override {
        if (click == TF_LBI_CLK_LEFT && context_)
            return request(context_.Get(), {{"op", "toggle_ascii"}});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu *menu) override {
        if (!menu)
            return E_POINTER;
        menu->AddMenuItem(1, 0, nullptr, nullptr, ascii_ ? L"中文模式" : L"英文模式", 4, nullptr);
        menu->AddMenuItem(2, 0, nullptr, nullptr, L"简体 / 繁體", 7, nullptr);
        menu->AddMenuItem(3, 0, nullptr, nullptr, L"全拼", 2, nullptr);
        menu->AddMenuItem(4, 0, nullptr, nullptr, L"小鹤双拼", 4, nullptr);
        menu->AddMenuItem(5, 0, nullptr, nullptr, L"自然码双拼", 5, nullptr);
        return menu->AddMenuItem(6, 0, nullptr, nullptr, L"R-IME 设置…", 9, nullptr);
    }
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT id) override {
        try {
            if (id == 6) {
                auto exe = win::install_directory() / L"r-ime-settings.exe";
                ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(),
                              SW_SHOWNORMAL);
                return S_OK;
            }
            if (!context_)
                return S_OK;
            if (id == 1)
                return request(context_.Get(), {{"op", "toggle_ascii"}});
            if (id == 2)
                return request(context_.Get(), {{"op", "toggle_simplified"}});
            const char *schemas[] = {"rime_ime_pinyin", "rime_ime_flypy", "rime_ime_ziranma"};
            if (id >= 3 && id <= 5)
                return request(context_.Get(), {{"op", "set_schema"}, {"schema", schemas[id - 3]}});
        } catch (...) {
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIcon(HICON *icon) override {
        if (!icon)
            return E_POINTER;
        *icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(ascii_ ? 102 : 101),
                                              IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        return *icon ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetText(BSTR *text) override {
        if (!text)
            return E_POINTER;
        *text = SysAllocString(ascii_ ? L"A" : L"中");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID id, IUnknown *sink, DWORD *cookie) override {
        if (id != IID_ITfLangBarItemSink)
            return CONNECT_E_CANNOTCONNECT;
        if (!sink || !cookie)
            return E_INVALIDARG;
        if (barSink_)
            return CONNECT_E_ADVISELIMIT;
        auto result = sink->QueryInterface(IID_PPV_ARGS(&barSink_));
        if (SUCCEEDED(result))
            *cookie = 1;
        return result;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        if (cookie != 1 || !barSink_)
            return CONNECT_E_NOCONNECTION;
        barSink_.Reset();
        return S_OK;
    }
};
class Factory final : public IClassFactory {
    ULONG refs_ = 1;

  public:
    Factory() {
        ++objects;
    }
    ~Factory() {
        --objects;
    }
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
        if (outer)
            return CLASS_E_NOAGGREGATION;
        try {
            auto service = new Service;
            auto result = service->QueryInterface(id, p);
            service->Release();
            return result;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock)
            ++objects;
        else
            --objects;
        return S_OK;
    }
};
} // namespace rime_ime::tsf
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID id, void **result) {
    if (clsid != rime_ime::tsf::service_id)
        return CLASS_E_CLASSNOTAVAILABLE;
    try {
        auto factory = new rime_ime::tsf::Factory;
        auto status = factory->QueryInterface(id, result);
        factory->Release();
        return status;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {
    return rime_ime::tsf::objects.load() == 0 ? S_OK : S_FALSE;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        rime_ime::tsf::module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
