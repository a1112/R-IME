#include "../../ipc/client.hpp"
#include "../platform.hpp"
#include "candidate_ui.hpp"
#include "display_attributes.hpp"
#include "guids.hpp"
#include "panel.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctffunc.h>
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
                      public ITfTextLayoutSink,
                      public ITfCompartmentEventSink,
                      public ITfCompositionSink,
                      public ITfFunctionProvider,
                      public ITfFnConfigure,
                      public ITfDisplayAttributeProvider,
                      public ITfLangBarItemButton,
                      public ITfSource {
    ULONG refs_ = 1;
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfContext> context_;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfLangBarItemSink> barSink_;
    ComPtr<ITfUIElementMgr> uiManager_;
    ComPtr<ITfCompartment> openClose_, conversion_;
    TfClientId clientId_ = TF_CLIENTID_NULL;
    TfGuidAtom attributeAtom_ = TF_INVALID_GUIDATOM;
    DWORD threadSink_ = TF_INVALID_COOKIE, focusSink_ = TF_INVALID_COOKIE;
    DWORD layoutSink_ = TF_INVALID_COOKIE, openSink_ = TF_INVALID_COOKIE,
          conversionSink_ = TF_INVALID_COOKIE, uiId_ = TF_INVALID_COOKIE;
    bool keySink_ = false, barAdded_ = false, shiftPending_ = false, ascii_ = false,
         focused_ = true, fullShape_ = false, asciiPunctuation_ = false, syncingModes_ = false,
         uiLess_ = false, secure_ = false, barVisible_ = true, simplified_ = true,
         functionAdded_ = false;
    std::string schema_ = "rime_ime_pinyin";
    std::uint64_t session_ = 0, epoch_ = 0, revision_ = 0;
    Client client_{Paths::from_environment().runtime / "engine.sock"};
    Json config_ = default_config(), state_ = Json::object();
    CandidateUi candidates_{
        static_cast<ITfTextInputProcessorEx *>(this),
        [this](ITfDocumentMgr **document) {
            return context_ ? context_->GetDocumentMgr(document) : E_UNEXPECTED;
        },
        [this](UINT index, bool finalize) {
            if (!context_ || !focused_ || uiId_ == TF_INVALID_COOKIE)
                return E_UNEXPECTED;
            if (finalize)
                return request(context_.Get(), {{"op", "select"}, {"index", index}});
            auto count = state_.at("candidates").size();
            if (index >= count)
                return E_INVALIDARG;
            auto epoch = epoch_;
            auto frame = revision_;
            ComPtr<ITfContext> held = context_;
            ComPtr<ITfTextInputProcessorEx> self = this;
            return edit(held.Get(), clientId_,
                        [this, self, held, index, epoch, frame](TfEditCookie cookie) {
                            if (epoch != epoch_ || frame != revision_ ||
                                held.Get() != context_.Get() || blocked(held.Get()))
                                return S_FALSE;
                            try {
                                Json reply = state_;
                                int current = state_.value("highlighted", 0);
                                for (int remaining = std::abs(current - int(index)); remaining > 0;
                                     --remaining)
                                    reply = client_.call({{"op", "navigate"},
                                                          {"session", session_},
                                                          {"backward", current > int(index)}},
                                                         40);
                                return apply(held.Get(), cookie, reply);
                            } catch (...) {
                                reset(false);
                                return E_FAIL;
                            }
                        });
        },
        [this] {
            return context_ && focused_ ? request(context_.Get(), {{"op", "reset"}}) : E_UNEXPECTED;
        },
        [this](bool show) {
            if (!show) {
                panel_.hide();
                candidates_.visible(false);
            } else
                update_position();
        }};
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
    static void diagnostic(const char *operation, const char *reason) {
        wchar_t enabled[2]{};
        if (!GetEnvironmentVariableW(L"RIME_TSF_TRACE", enabled, 2) || enabled[0] != L'1')
            return;
        auto message = std::string("R-IME TSF ") + operation + ": " + reason + "\n";
        OutputDebugStringA(message.c_str());
    }
    void notify_bar() {
        if (barSink_)
            barSink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_STATUS);
    }
    static bool read_mode(ITfCompartment *compartment, LONG &value) {
        if (!compartment)
            return false;
        VARIANT data;
        VariantInit(&data);
        bool valid = SUCCEEDED(compartment->GetValue(&data)) && data.vt == VT_I4;
        if (valid)
            value = data.lVal;
        VariantClear(&data);
        return valid;
    }
    void sync_modes() {
        if (!openClose_ || !conversion_ || syncingModes_)
            return;
        syncingModes_ = true;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = ascii_ ? 0 : 1;
        openClose_->SetValue(clientId_, &value);
        LONG previous = 0;
        read_mode(conversion_.Get(), previous);
        value.lVal = (previous & ~(TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_FULLSHAPE |
                                   TF_CONVERSIONMODE_SYMBOL)) |
                     (ascii_ ? 0 : TF_CONVERSIONMODE_NATIVE) |
                     (fullShape_ ? TF_CONVERSIONMODE_FULLSHAPE : 0) |
                     (asciiPunctuation_ ? 0 : TF_CONVERSIONMODE_SYMBOL);
        conversion_->SetValue(clientId_, &value);
        syncingModes_ = false;
    }
    void end_candidates() {
        panel_.hide();
        if (uiId_ != TF_INVALID_COOKIE && uiManager_) {
            auto id = uiId_;
            uiId_ = TF_INVALID_COOKIE;
            uiManager_->EndUIElement(id);
        }
        candidates_.clear();
    }
    bool has_candidates() const {
        return state_.contains("candidates") && !state_["candidates"].empty();
    }
    bool panel_visible() const {
        bool visible = false;
        EnumThreadWindows(
            GetCurrentThreadId(),
            [](HWND window, LPARAM parameter) -> BOOL {
                wchar_t name[64]{};
                GetClassNameW(window, name, 64);
                if (wcscmp(name, L"R-IME.CandidateWindow.v1") == 0 && IsWindowVisible(window)) {
                    *reinterpret_cast<bool *>(parameter) = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&visible));
        return visible;
    }
    void position_candidates(ITfContext *context, TfEditCookie cookie) {
        if (!focused_ || secure_ || !candidates_.allowed() || !has_candidates()) {
            panel_.hide();
            candidates_.visible(false);
            return;
        }
        RECT anchor{};
        BOOL clipped = FALSE;
        HWND owner = nullptr;
        ComPtr<ITfContextView> view;
        ComPtr<ITfRange> caret;
        if (FAILED(context->GetActiveView(&view)))
            return;
        view->GetWnd(&owner);
        TF_SELECTION selection{};
        ULONG count = 0;
        if (SUCCEEDED(context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &count)) &&
            count) {
            caret.Attach(selection.range);
            caret->Collapse(cookie, TF_ANCHOR_START);
        }
        auto status = caret ? view->GetTextExt(cookie, caret.Get(), &anchor, &clipped) : E_FAIL;
        // A not-yet-laid-out or offscreen range is not the document's screen
        // rectangle. Wait for its layout callback rather than jumping windows.
        if (FAILED(status) || clipped ||
            (anchor.right == anchor.left && anchor.bottom == anchor.top)) {
            panel_.hide();
            candidates_.visible(false);
            return;
        }
        panel_.show(state_, config_["appearance"], anchor, owner, revision_);
        candidates_.visible(panel_visible());
    }
    void update_position() {
        if (!context_ || !has_candidates())
            return;
        auto epoch = epoch_;
        ComPtr<ITfContext> held = context_;
        ComPtr<ITfTextInputProcessorEx> self = this;
        auto task = new Edit([this, self, held, epoch](TfEditCookie cookie) {
            if (epoch == epoch_ && held.Get() == context_.Get())
                position_candidates(held.Get(), cookie);
            return S_OK;
        });
        HRESULT result;
        held->RequestEditSession(clientId_, task, TF_ES_ASYNCDONTCARE | TF_ES_READ, &result);
        task->Release();
    }
    void attach_context(ITfContext *context) {
        context_ = context;
        ComPtr<ITfSource> source;
        if (context_ && SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&source))))
            source->AdviseSink(IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink *>(this),
                               &layoutSink_);
        notify_bar();
    }
    HRESULT mode_command(const Json &command) {
        if (secure_)
            return S_FALSE;
        if (context_ && !blocked(context_.Get()))
            return request(context_.Get(), command);
        auto op = command.value("op", "");
        if (op == "toggle_ascii")
            ascii_ = !ascii_;
        else if (op == "set_ascii")
            ascii_ = command.value("value", false);
        else if (op == "toggle_full_shape")
            fullShape_ = !fullShape_;
        else if (op == "toggle_ascii_punctuation")
            asciiPunctuation_ = !asciiPunctuation_;
        else
            return S_FALSE;
        sync_modes();
        notify_bar();
        return S_OK;
    }
    HRESULT show_settings(HWND parent, bool wait) {
        if (secure_)
            return E_ACCESSDENIED;
        auto executable = win::install_directory() / L"r-ime-settings.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(executable, error))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        auto directory = executable.parent_path().wstring();
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        launch.hwnd = parent;
        launch.lpVerb = L"open";
        launch.lpFile = executable.c_str();
        launch.lpDirectory = directory.c_str();
        launch.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&launch)) {
            auto code = GetLastError();
            return code ? HRESULT_FROM_WIN32(code) : E_FAIL;
        }
        if (!launch.hProcess)
            return wait ? E_UNEXPECTED : S_OK;
        HRESULT result = S_OK;
        if (wait) {
            // The public properties function is modal to its caller, while
            // the settings window runs independently of the input engine.
            DWORD index = 0;
            result =
                CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                                         INFINITE, 1, &launch.hProcess, &index);
        }
        CloseHandle(launch.hProcess);
        return result;
    }
    void refresh_config() {
        try {
            auto path = Paths::from_environment().config;
            auto stamp = std::filesystem::exists(path) ? std::filesystem::last_write_time(path)
                                                       : std::filesystem::file_time_type{};
            if (stamp != configStamp_) {
                auto next = load_config(path);
                bool inputChanged =
                    next["input"] != config_["input"] || next["general"] != config_["general"] ||
                    next["appearance"]["page_size"] != config_["appearance"]["page_size"];
                if (inputChanged)
                    reset(false);
                if (next["input"]["schema"] != config_["input"]["schema"])
                    schema_ = next["input"]["schema"].get<std::string>();
                if (next["general"]["ascii"] != config_["general"]["ascii"])
                    ascii_ = next["general"]["ascii"].get<bool>();
                if (next["general"].value("full_shape", false) !=
                    config_["general"].value("full_shape", false))
                    fullShape_ = next["general"].value("full_shape", false);
                if (next["general"]["ascii_punctuation"] != config_["general"]["ascii_punctuation"])
                    asciiPunctuation_ = next["general"]["ascii_punctuation"].get<bool>();
                if (next["general"]["simplified"] != config_["general"]["simplified"])
                    simplified_ = next["general"]["simplified"].get<bool>();
                config_ = std::move(next);
                configStamp_ = stamp;
                sync_modes();
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
        if (key == VK_SPACE && (GetKeyState(VK_SHIFT) & 0x8000) && !chord())
            return {{"op", "toggle_full_shape"}};
        if (key == VK_OEM_PERIOD && (GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
            !((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000))
            return {{"op", "toggle_ascii_punctuation"}};
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
        if (!context || !focused_ || secure_)
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
    static void clear_attribute(ITfContext *context, TfEditCookie cookie, ITfRange *range) {
        ComPtr<ITfProperty> property;
        if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property)))
            property->Clear(cookie, range);
    }
    void clear_composition(ITfContext *context, ITfComposition *composition) {
        if (!context || !composition)
            return;
        ComPtr<ITfContext> held = context;
        ComPtr<ITfComposition> old = composition;
        edit(context, clientId_, [held, old](TfEditCookie cookie) {
            ComPtr<ITfRange> range;
            HRESULT status = old->GetRange(&range);
            if (SUCCEEDED(status)) {
                clear_attribute(held.Get(), cookie, range.Get());
                status = range->SetText(cookie, 0, L"", 0);
            }
            old->EndComposition(cookie);
            return status;
        });
    }
    void reset(bool releaseContext = true) {
        ++epoch_;
        ++revision_;
        shiftPending_ = false;
        end_candidates();
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
        if (releaseContext) {
            ComPtr<ITfSource> source;
            if (context_ && layoutSink_ != TF_INVALID_COOKIE &&
                SUCCEEDED(context_->QueryInterface(IID_PPV_ARGS(&source))))
                source->UnadviseSink(layoutSink_);
            layoutSink_ = TF_INVALID_COOKIE;
            context_.Reset();
        }
        notify_bar();
    }
    bool ensure_session(ITfContext *context) {
        if (context_.Get() != context) {
            reset();
            attach_context(context);
        }
        if (session_)
            return true;
        auto now = std::chrono::steady_clock::now();
        if (now < nextConnection_)
            return false;
        try {
            auto result = client_.call({{"op", "create"}, {"session", 0}}, 40);
            session_ = result.at("session").get<std::uint64_t>();
            if (result.value("schema", "") != schema_)
                result = client_.call(
                    {{"op", "set_schema"}, {"schema", schema_}, {"session", session_}}, 40);
            if (result.value("simplified", true) != simplified_)
                result = client_.call({{"op", "toggle_simplified"}, {"session", session_}}, 40);
            if (result.value("ascii", false) != ascii_)
                result = client_.call(
                    {{"op", "set_ascii"}, {"value", ascii_}, {"session", session_}}, 40);
            if (result.value("full_shape", false) != fullShape_)
                result = client_.call(
                    {{"op", "set_full_shape"}, {"value", fullShape_}, {"session", session_}}, 40);
            if (result.value("ascii_punctuation", false) != asciiPunctuation_)
                result = client_.call({{"op", "set_ascii_punctuation"},
                                       {"value", asciiPunctuation_},
                                       {"session", session_}},
                                      40);
            state_ = std::move(result);
            return true;
        } catch (const std::exception &error) {
            diagnostic("session setup failed", error.what());
            session_ = 0;
            state_ = Json::object();
            ++epoch_;
            ++revision_;
            end_candidates();
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
        status = cursor->Collapse(cookie, TF_ANCHOR_END);
        if (FAILED(status))
            return status;
        TF_SELECTION selection{cursor.Get(), {TF_AE_NONE, FALSE}};
        return context->SetSelection(cookie, 1, &selection);
    }
    HRESULT apply(ITfContext *context, TfEditCookie cookie, const Json &reply,
                  bool *committed = nullptr) {
        auto commit = wide(reply.value("commit", ""));
        auto preedit = wide(reply.value("preedit", ""));
        ComPtr<ITfRange> range;
        if (composition_) {
            HRESULT status = composition_->GetRange(&range);
            if (FAILED(status))
                return status;
            if (!commit.empty() || preedit.empty()) {
                clear_attribute(context, cookie, range.Get());
                status = range->SetText(cookie, 0, commit.data(), static_cast<LONG>(commit.size()));
                if (FAILED(status))
                    return status;
                if (committed && !commit.empty())
                    *committed = true;
                auto selectionStatus = selection_end(context, cookie, range.Get());
                auto old = composition_;
                composition_.Reset();
                status = old->EndComposition(cookie);
                if (FAILED(status) || FAILED(selectionStatus))
                    return FAILED(status) ? status : selectionStatus;
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
            if (committed)
                *committed = true;
            status = selection_end(context, cookie, range.Get());
            if (FAILED(status))
                return status;
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
            } else if (!range) {
                auto status = composition_->GetRange(&range);
                if (FAILED(status))
                    return status;
            }
            auto status =
                range->SetText(cookie, 0, preedit.data(), static_cast<LONG>(preedit.size()));
            if (FAILED(status))
                return status;
            if (attributeAtom_ != TF_INVALID_GUIDATOM) {
                ComPtr<ITfProperty> property;
                if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &property))) {
                    VARIANT value;
                    VariantInit(&value);
                    value.vt = VT_I4;
                    value.lVal = static_cast<LONG>(attributeAtom_);
                    property->SetValue(cookie, range.Get(), &value);
                }
            }
            ComPtr<ITfRange> cursor;
            status = range->Clone(&cursor);
            if (FAILED(status))
                return status;
            status = cursor->Collapse(cookie, TF_ANCHOR_START);
            if (FAILED(status))
                return status;
            auto bytes = reply.value("preedit", std::string{});
            size_t pos = std::min<size_t>(reply.value("cursor", 0), bytes.size());
            LONG count = static_cast<LONG>(wide(bytes.substr(0, pos)).size()), moved = 0;
            status = cursor->ShiftStart(cookie, count, &moved, nullptr);
            if (FAILED(status))
                return status;
            status = cursor->Collapse(cookie, TF_ANCHOR_START);
            if (FAILED(status))
                return status;
            TF_SELECTION selection{cursor.Get(), {TF_AE_NONE, FALSE}};
            status = context->SetSelection(cookie, 1, &selection);
            if (FAILED(status))
                return status;
        }
        state_ = reply;
        ascii_ = reply.value("ascii", false);
        fullShape_ = reply.value("full_shape", false);
        asciiPunctuation_ = reply.value("ascii_punctuation", false);
        simplified_ = reply.value("simplified", true);
        schema_ = reply.value("schema", schema_);
        ++revision_;
        sync_modes();
        notify_bar();
        if (!has_candidates())
            end_candidates();
        else {
            candidates_.update(reply);
            if (uiId_ == TF_INVALID_COOKIE && uiManager_) {
                BOOL show = !secure_;
                DWORD id = TF_INVALID_COOKIE;
                auto result = uiManager_->BeginUIElement(&candidates_, &show, &id);
                if (SUCCEEDED(result)) {
                    uiId_ = id;
                    candidates_.permission(show && !secure_);
                }
            }
            if (uiId_ != TF_INVALID_COOKIE && uiManager_)
                uiManager_->UpdateUIElement(uiId_);
            position_candidates(context, cookie);
        }
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
        auto frame = revision_;
        bool requireFrame = eaten == nullptr;
        ComPtr<ITfContext> held = context;
        ComPtr<ITfTextInputProcessorEx> self = this;
        auto handled = std::make_shared<bool>(false);
        HRESULT result = edit(
            context, clientId_,
            [this, self, held, request = std::move(request), epoch, frame, requireFrame,
             handled](TfEditCookie cookie) mutable {
                if (epoch != epoch_ || (requireFrame && frame != revision_) ||
                    held.Get() != context_.Get() || !focused_)
                    return S_FALSE;
                if (blocked(held.Get()) || private_scope(held.Get(), cookie)) {
                    reset();
                    return S_FALSE;
                }
                request["session"] = session_;
                try {
                    auto reply = client_.call(request, 40);
                    bool committed = false;
                    auto status = apply(held.Get(), cookie, reply, &committed);
                    *handled = reply.value("handled", false) && (SUCCEEDED(status) || committed);
                    if (FAILED(status))
                        reset(false);
                    return status;
                } catch (const std::exception &error) {
                    diagnostic("request failed", error.what());
                    // An ambiguous transport failure is never replayed into a new
                    // session. Existing preedit is cancelled before pass-through.
                    session_ = 0;
                    client_.disconnect();
                    ++epoch_;
                    end_candidates();
                    state_ = Json::object();
                    if (composition_) {
                        ComPtr<ITfRange> range;
                        composition_->GetRange(&range);
                        if (range) {
                            clear_attribute(held.Get(), cookie, range.Get());
                            range->SetText(cookie, 0, L"", 0);
                        }
                        auto old = composition_;
                        composition_.Reset();
                        old->EndComposition(cookie);
                    }
                    start_engine();
                    nextConnection_ =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                    return E_FAIL;
                }
            },
            eaten == nullptr);
        if (eaten)
            *eaten = *handled;
        return eaten ? S_OK : result;
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
        if (ascii_ && !fullShape_ && !composing)
            return false;
        if (fullShape_ && ((key >= '0' && key <= '9') || key == VK_SPACE ||
                           (key >= VK_NUMPAD0 && key <= VK_DIVIDE)))
            return true;
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
            attach_context(next.Get());
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
        else if (id == IID_ITfTextLayoutSink)
            *p = static_cast<ITfTextLayoutSink *>(this);
        else if (id == IID_ITfCompartmentEventSink)
            *p = static_cast<ITfCompartmentEventSink *>(this);
        else if (id == IID_ITfUIElement || id == IID_ITfCandidateListUIElement ||
                 id == IID_ITfCandidateListUIElementBehavior)
            *p = static_cast<ITfCandidateListUIElementBehavior *>(&candidates_);
        else if (id == IID_ITfCompositionSink)
            *p = static_cast<ITfCompositionSink *>(this);
        else if (id == IID_ITfFunctionProvider)
            *p = static_cast<ITfFunctionProvider *>(this);
        else if (id == IID_ITfFunction || id == IID_ITfFnConfigure)
            *p = static_cast<ITfFnConfigure *>(this);
        else if (id == IID_ITfDisplayAttributeProvider)
            *p = static_cast<ITfDisplayAttributeProvider *>(this);
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
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr *manager, TfClientId client,
                                         DWORD flags) override {
        if (!manager)
            return E_INVALIDARG;
        if (manager_)
            return E_UNEXPECTED;
        manager_ = manager;
        clientId_ = client;
        uiLess_ = (flags & TF_TMAE_UIELEMENTENABLEDONLY) != 0;
        secure_ = (flags & TF_TMAE_SECUREMODE) != 0;
        BOOL threadFocus = FALSE;
        manager_->IsThreadFocus(&threadFocus);
        focused_ = threadFocus != FALSE;
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
            fullShape_ = config_["general"].value("full_shape", false);
            asciiPunctuation_ = config_["general"]["ascii_punctuation"].get<bool>();
            simplified_ = config_["general"]["simplified"].get<bool>();
            schema_ = config_["input"]["schema"].get<std::string>();
        } catch (...) {
            config_ = default_config();
        }
        manager_->QueryInterface(IID_PPV_ARGS(&uiManager_));
        ComPtr<ITfCategoryMgr> category;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&category))))
            category->RegisterGUID(input_attribute_id, &attributeAtom_);
        ComPtr<ITfSourceSingle> single;
        if (SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&single))))
            functionAdded_ = SUCCEEDED(single->AdviseSingleSink(
                clientId_, IID_ITfFunctionProvider, static_cast<ITfFunctionProvider *>(this)));
        ComPtr<ITfCompartmentMgr> compartments;
        if (SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&compartments)))) {
            compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &openClose_);
            compartments->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                                         &conversion_);
            sync_modes();
            ComPtr<ITfSource> modeSource;
            if (openClose_ && SUCCEEDED(openClose_->QueryInterface(IID_PPV_ARGS(&modeSource))))
                modeSource->AdviseSink(IID_ITfCompartmentEventSink,
                                       static_cast<ITfCompartmentEventSink *>(this), &openSink_);
            modeSource.Reset();
            if (conversion_ && SUCCEEDED(conversion_->QueryInterface(IID_PPV_ARGS(&modeSource))))
                modeSource->AdviseSink(IID_ITfCompartmentEventSink,
                                       static_cast<ITfCompartmentEventSink *>(this),
                                       &conversionSink_);
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
        ComPtr<ITfSource> modeSource;
        if (openClose_ && openSink_ != TF_INVALID_COOKIE &&
            SUCCEEDED(openClose_->QueryInterface(IID_PPV_ARGS(&modeSource))))
            modeSource->UnadviseSink(openSink_);
        modeSource.Reset();
        if (conversion_ && conversionSink_ != TF_INVALID_COOKIE &&
            SUCCEEDED(conversion_->QueryInterface(IID_PPV_ARGS(&modeSource))))
            modeSource->UnadviseSink(conversionSink_);
        openSink_ = conversionSink_ = TF_INVALID_COOKIE;
        openClose_.Reset();
        conversion_.Reset();
        uiManager_.Reset();
        if (manager_) {
            ComPtr<ITfSourceSingle> single;
            if (functionAdded_ && SUCCEEDED(manager_->QueryInterface(IID_PPV_ARGS(&single))))
                single->UnadviseSingleSink(clientId_, IID_ITfFunctionProvider);
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
        functionAdded_ = false;
        threadSink_ = focusSink_ = TF_INVALID_COOKIE;
        clientId_ = TF_CLIENTID_NULL;
        attributeAtom_ = TF_INVALID_GUIDATOM;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnChange(REFGUID id) override {
        if (syncingModes_ || !manager_)
            return S_OK;
        LONG value = 0;
        bool ascii = ascii_, shape = fullShape_, punctuation = asciiPunctuation_;
        if (id == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE && read_mode(openClose_.Get(), value))
            ascii = value == 0;
        else if (id == GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION &&
                 read_mode(conversion_.Get(), value)) {
            ascii = !(value & TF_CONVERSIONMODE_NATIVE);
            shape = (value & TF_CONVERSIONMODE_FULLSHAPE) != 0;
            punctuation = !(value & TF_CONVERSIONMODE_SYMBOL);
        } else
            return S_OK;
        if (ascii != ascii_ || shape != fullShape_ || punctuation != asciiPunctuation_) {
            // The OS changed modes outside a key event. Cancel an old session
            // before the next key, so it cannot restore its stale mode snapshot.
            reset(false);
            ascii_ = ascii;
            fullShape_ = shape;
            asciiPunctuation_ = punctuation;
            sync_modes();
            notify_bar();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext *context, TfLayoutCode code,
                                             ITfContextView *) override {
        if (context_.Get() != context)
            return S_OK;
        if (code == TF_LC_DESTROY) {
            panel_.hide();
            candidates_.visible(false);
        } else
            update_position();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        focused_ = foreground != FALSE;
        if (!foreground)
            reset();
        else if (manager_) {
            ComPtr<ITfDocumentMgr> document;
            if (SUCCEEDED(manager_->GetFocus(&document)))
                set_focus(document.Get());
            notify_bar();
        }
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
            if (!state_.value("preedit", std::string{}).empty() && (key == VK_UP || key == VK_DOWN))
                return request(context, {{"op", "navigate"}, {"backward", key == VK_UP}}, eaten);
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
            if (SUCCEEDED(composition->GetRange(&range))) {
                if (context_)
                    clear_attribute(context_.Get(), cookie, range.Get());
                range->SetText(cookie, 0, L"", 0);
            }
            composition_.Reset();
            reset(false);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo **attributes) override {
        return enumerate_attributes(attributes);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid,
                                                      ITfDisplayAttributeInfo **info) override {
        return input_attribute(guid, info);
    }
    HRESULT STDMETHODCALLTYPE GetType(GUID *type) override {
        if (!type)
            return E_POINTER;
        *type = service_id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR *description) override {
        if (!description)
            return E_POINTER;
        *description = SysAllocString(L"R-IME 输入法功能");
        return *description ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetFunction(REFGUID guid, REFIID id, IUnknown **function) override {
        if (!function)
            return E_POINTER;
        *function = nullptr;
        if (guid != GUID_NULL || id != IID_ITfFnConfigure)
            return E_NOINTERFACE;
        if (secure_)
            return E_ACCESSDENIED;
        return QueryInterface(id, reinterpret_cast<void **>(function));
    }
    HRESULT STDMETHODCALLTYPE GetDisplayName(BSTR *name) override {
        if (!name)
            return E_POINTER;
        *name = SysAllocString(L"R-IME 设置");
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Show(HWND parent, LANGID language, REFGUID profile) override {
        if (language != language_id || profile != profile_id)
            return E_INVALIDARG;
        return show_settings(parent, true);
    }
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO *info) override {
        if (!info)
            return E_POINTER;
        *info = {service_id,
                 GUID_LBI_INPUTMODE,
                 TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_SHOWNINTRAY,
                 0,
                 {}};
        wcscpy_s(info->szDescription, L"R-IME 输入模式与设置");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD *status) override {
        if (!status)
            return E_POINTER;
        *status = (barVisible_ ? 0 : TF_LBI_STATUS_HIDDEN) |
                  (blocked(context_.Get()) ? TF_LBI_STATUS_DISABLED : 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override {
        barVisible_ = show != FALSE;
        notify_bar();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR *text) override {
        if (!text)
            return E_POINTER;
        *text = SysAllocString(ascii_ ? L"R-IME：英文模式" : L"R-IME：中文模式");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT, const RECT *) override {
        if (click == TF_LBI_CLK_LEFT)
            return mode_command({{"op", "toggle_ascii"}});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu *menu) override {
        if (!menu)
            return E_POINTER;
        auto item = [&](UINT id, const wchar_t *text, bool checked, bool enabled = true) {
            DWORD flags = (checked ? TF_LBMENUF_CHECKED : 0) | (enabled ? 0 : TF_LBMENUF_GRAYED);
            return menu->AddMenuItem(id, flags, nullptr, nullptr, text,
                                     static_cast<ULONG>(wcslen(text)), nullptr);
        };
        item(1, L"中文模式", !ascii_, !secure_);
        item(7, L"英文模式", ascii_, !secure_);
        item(8, L"全角输入  Shift+空格", fullShape_, !secure_);
        item(9, L"中文标点  Ctrl+.", !asciiPunctuation_, !secure_);
        item(2, L"简体中文", simplified_, !secure_ && context_ && !blocked(context_.Get()));
        auto schema = schema_;
        item(3, L"全拼", schema == "rime_ime_pinyin", !secure_ && context_);
        item(4, L"小鹤双拼", schema == "rime_ime_flypy", !secure_ && context_);
        item(5, L"自然码双拼", schema == "rime_ime_ziranma", !secure_ && context_);
        return item(6, L"R-IME 设置…", false, !secure_);
    }
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT id) override {
        try {
            if (id == 6) {
                return show_settings(nullptr, false);
            }
            if (id == 1)
                return mode_command({{"op", "set_ascii"}, {"value", false}});
            if (id == 7)
                return mode_command({{"op", "set_ascii"}, {"value", true}});
            if (id == 8)
                return mode_command({{"op", "toggle_full_shape"}});
            if (id == 9)
                return mode_command({{"op", "toggle_ascii_punctuation"}});
            if (!context_ || secure_)
                return S_FALSE;
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
        *icon = nullptr;
        const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
        void *pixels = nullptr;
        auto dc = CreateCompatibleDC(nullptr);
        auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap || !pixels) {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            return E_OUTOFMEMORY;
        }
        std::fill_n(static_cast<DWORD *>(pixels), width * height, 0);
        auto oldBitmap = SelectObject(dc, bitmap);
        auto font = CreateFontW(-height + 1, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        auto oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT bounds{0, 0, width, height};
        DrawTextW(dc, ascii_ ? L"A" : L"中", 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DWORD light = 1, bytes = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
        HIGHCONTRASTW contrast{sizeof(contrast)};
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        COLORREF color = contrast.dwFlags & HCF_HIGHCONTRASTON
                             ? GetSysColor(COLOR_WINDOWTEXT)
                             : (light ? RGB(32, 32, 32) : RGB(255, 255, 255));
        auto data = static_cast<DWORD *>(pixels);
        for (int i = 0; i < width * height; ++i) {
            DWORD alpha = data[i] & 0xff;
            data[i] = (alpha << 24) | ((GetRValue(color) * alpha / 255) << 16) |
                      ((GetGValue(color) * alpha / 255) << 8) | (GetBValue(color) * alpha / 255);
        }
        std::vector<BYTE> maskBits(((width + 15) / 16) * 2 * height, 0);
        auto mask = CreateBitmap(width, height, 1, 1, maskBits.data());
        ICONINFO iconInfo{TRUE, 0, 0, mask, bitmap};
        if (mask)
            *icon = CreateIconIndirect(&iconInfo);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        if (font)
            DeleteObject(font);
        if (mask)
            DeleteObject(mask);
        DeleteObject(bitmap);
        DeleteDC(dc);
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
