#include "../ipc/client.hpp"
#include "panel.hpp"
#include <chrono>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterfacemanager.h>
#include <rime_ime/config.hpp>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char **environ;
namespace rime_ime {
using namespace fcitx;
struct State : InputContextProperty {
    uint64_t session = 0, epoch = 0;
    bool shift_pending = false, ascii = false;
    Json snapshot = Json::object();
};
class Engine;
class PagedCandidates final : public CommonCandidateList {
  public:
    PagedCandidates(int page, bool last, std::function<void(bool)> turn)
        : page_(page), last_(last), turn_(std::move(turn)) {}
    bool hasPrev() const override {
        return page_ > 0;
    }
    bool hasNext() const override {
        return !last_;
    }
    bool usedNextBefore() const override {
        return page_ > 0;
    }
    int currentPage() const override {
        return page_;
    }
    int totalPages() const override {
        return last_ ? page_ + 1 : -1;
    }
    void prev() override {
        auto turn = turn_;
        if (hasPrev())
            turn(true);
    }
    void next() override {
        auto turn = turn_;
        if (hasNext())
            turn(false);
    }

  private:
    int page_;
    bool last_;
    std::function<void(bool)> turn_;
};
class Candidate final : public CandidateWord {
  public:
    Candidate(Engine *e, int i, std::string text, uint64_t revision)
        : CandidateWord(Text(std::move(text))), engine(e), index(i), revision_(revision) {}
    void select(InputContext *) const override;

  private:
    Engine *engine;
    int index;
    uint64_t revision_;
};
class Engine final : public InputMethodEngineV2 {
  public:
    explicit Engine(Instance *instance)
        : instance_(instance), paths_(Paths::from_environment()),
          client_(paths_.runtime / "engine.sock"),
          factory_([](InputContext &) { return new State; }),
          panel_(
              instance, [this](InputContext *ic, int index) { choose(ic, index); },
              [this](InputContext *ic, bool backward) {
                  operation(ic, {{"op", "page"}, {"backward", backward}});
              }) {
        config_ = default_config();
        refresh_config();
        instance_->inputContextManager().registerProperty("r-ime-state", &factory_);
        add_action("r-ime-ascii", "中 / A",
                   [this](InputContext *ic) { operation(ic, {{"op", "toggle_ascii"}}); });
        add_action("r-ime-simplified", "简体 / 繁體",
                   [this](InputContext *ic) { operation(ic, {{"op", "toggle_simplified"}}); });
        const std::pair<const char *, const char *> schemes[] = {
            {"rime_ime_pinyin", "全拼"},
            {"rime_ime_flypy", "小鹤双拼"},
            {"rime_ime_ziranma", "自然码双拼"}};
        for (auto &[schema, label] : schemes)
            add_action(std::string("r-ime-") + schema, label, [this, schema](InputContext *ic) {
                operation(ic, {{"op", "set_schema"}, {"schema", schema}});
            });
        add_action("r-ime-settings", "R-IME 设置", [](InputContext *) {
            pid_t pid = 0;
            char cmd[] = "r-ime-settings";
            char *args[] = {cmd, nullptr};
            if (posix_spawnp(&pid, cmd, nullptr, nullptr, args, environ) == 0)
                std::thread([pid] {
                    int status;
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                    }
                }).detach();
        });
        watchers_.push_back(instance_->watchEvent(
            EventType::InputContextFocusOut, EventWatcherPhase::PreInputMethod, [this](Event &e) {
                auto *ic = static_cast<InputContextEvent &>(e).inputContext();
                if (active_.get() == ic) {
                    clear(ic, true);
                    active_ = {};
                }
            }));
        watchers_.push_back(instance_->watchEvent(
            EventType::InputContextDestroyed, EventWatcherPhase::PreInputMethod, [this](Event &e) {
                auto *ic = static_cast<InputContextEvent &>(e).inputContext();
                if (active_.get() == ic) {
                    clear(ic, true);
                    active_ = {};
                }
            }));
        watchers_.push_back(
            instance_->watchEvent(EventType::InputContextCursorRectChanged,
                                  EventWatcherPhase::PostInputMethod, [this](Event &e) {
                                      auto *ic = static_cast<InputContextEvent &>(e).inputContext();
                                      if (active_.get() == ic)
                                          show_panel(ic);
                                  }));
        launch();
    }
    ~Engine() {
        watchers_.clear();
        fallback_.reset();
        if (auto *ic = active_.get())
            ic->inputPanel().setCustomInputPanelCallback({});
        panel_.hide();
        factory_.unregister();
    }
    void activate(const InputMethodEntry &, InputContextEvent &e) override {
        auto *ic = e.inputContext();
        active_ = ic->watch();
        refresh_config();
        for (auto &a : actions_)
            ic->statusArea().addAction(StatusGroup::InputMethod, a.get());
        if (sensitive(ic))
            return;
        operation(ic, {{"op", "status"}});
    }
    void deactivate(const InputMethodEntry &, InputContextEvent &e) override {
        clear(e.inputContext(), true);
        if (active_.get() == e.inputContext())
            active_ = {};
    }
    void reset(const InputMethodEntry &, InputContextEvent &e) override {
        clear(e.inputContext(), true);
    }
    std::string subModeLabelImpl(const InputMethodEntry &, InputContext &ic) override {
        return ic.propertyFor(&factory_)->ascii ? "A" : "中";
    }
    std::string subModeIconImpl(const InputMethodEntry &, InputContext &) override {
        return "r-ime";
    }
    void keyEvent(const InputMethodEntry &, KeyEvent &e) override {
        auto *ic = e.inputContext();
        auto *s = ic->propertyFor(&factory_);
        if (sensitive(ic)) {
            clear(ic, true);
            return;
        }
        refresh_config();
        const auto sym = e.key().sym();
        const bool shift = sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
        auto shortcut = config_["shortcuts"]["toggle_ascii"].get<std::string>();
        if (shift && shortcut == "Shift") {
            if (!e.isRelease()) {
                s->shift_pending =
                    e.key().states().toInteger() == 0 || e.key().states() == KeyState::Shift;
                return;
            }
            const bool tap = s->shift_pending;
            s->shift_pending = false;
            if (tap && operation(ic, {{"op", "toggle_ascii"}}))
                e.filterAndAccept();
            return;
        }
        if (!e.isRelease())
            s->shift_pending = false;
        if (e.isRelease())
            return;
        if (shortcut != "Shift" && e.key().check(Key(shortcut))) {
            if (operation(ic, {{"op", "toggle_ascii"}}))
                e.filterAndAccept();
            return;
        }
        Json request = {{"op", "key"},
                        {"keycode", uint32_t(sym)},
                        {"modifiers", uint32_t(e.key().states().toInteger() & 0xffff)}};
        if (!s->snapshot.value("preedit", std::string{}).empty()) {
            if (e.key().check(Key(config_["shortcuts"]["next_page"].get<std::string>())))
                request = {{"op", "page"}, {"backward", false}};
            else if (e.key().check(Key(config_["shortcuts"]["previous_page"].get<std::string>())))
                request = {{"op", "page"}, {"backward", true}};
            else if (e.key().check(Key(config_["shortcuts"]["cancel"].get<std::string>())))
                request = {{"op", "reset"}};
        }
        if (operation(ic, std::move(request)))
            e.filterAndAccept();
    }
    void choose(InputContext *ic, int index, uint64_t revision = 0) {
        if (revision && ic->propertyFor(&factory_)->snapshot.value("id", uint64_t{}) != revision)
            return;
        operation(ic, {{"op", "select"}, {"index", index}});
    }

  private:
    bool sensitive(InputContext *ic) {
        auto flags = ic->capabilityFlags();
        return flags.test(CapabilityFlag::Password) || flags.test(CapabilityFlag::Sensitive) ||
               flags.test(CapabilityFlag::Disable);
    }
    void launch() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_launch_ > std::chrono::seconds(3)) {
            start_engine();
            last_launch_ = now;
        }
    }
    void diagnostic(const std::string &message) {
        if (message == last_diagnostic_)
            return;
        last_diagnostic_ = message;
        try {
            atomic_write(paths_.cache / "diagnostic.json", Json{{"message", message}}.dump());
        } catch (...) {
        }
    }
    void refresh_config() {
        try {
            auto stamp = std::filesystem::exists(paths_.config)
                             ? std::filesystem::last_write_time(paths_.config)
                             : std::filesystem::file_time_type{};
            if (stamp != config_stamp_) {
                config_ = load_config(paths_.config);
                config_stamp_ = stamp;
            }
        } catch (const std::exception &e) {
            diagnostic(e.what());
        }
    }
    void add_action(const std::string &id, const std::string &label,
                    std::function<void(InputContext *)> callback) {
        auto action = std::make_unique<SimpleAction>();
        action->setShortText(label);
        action->setIcon("r-ime");
        action->registerAction(id, &instance_->userInterfaceManager());
        connections_.emplace_back(action->connect<SimpleAction::Activated>(std::move(callback)));
        actions_.push_back(std::move(action));
    }
    bool operation(InputContext *ic, Json request) {
        if (!ic || !ic->hasFocus() || sensitive(ic))
            return false;
        auto *s = ic->propertyFor(&factory_);
        try {
            if (!s->session || s->epoch != epoch_) {
                auto created = client_.call({{"op", "create"}, {"session", 0}}, 40);
                s->session = created.at("session");
                s->epoch = epoch_;
            }
            request["session"] = s->session;
            Json reply;
            try {
                reply = client_.call(request, 40);
            } catch (const RpcError &e) {
                if (std::string(e.what()) != "unknown or expired session")
                    throw;
                // The worker explicitly rejected this before processing the key. Recreate
                // after a successful deployment; transport failures never enter this path.
                auto created = client_.call({{"op", "create"}, {"session", 0}}, 40);
                s->session = created.at("session");
                request["session"] = s->session;
                reply = client_.call(request, 40);
            }
            // This is deliberately synchronous and bounded: no accepted key is replayed
            // after disconnect and no response is delivered to a different input field.
            if (!ic->hasFocus() || s->epoch != epoch_)
                return false;
            s->snapshot = reply;
            s->ascii = reply.value("ascii", false);
            auto commit = reply.value("commit", std::string{});
            if (!commit.empty())
                ic->commitString(commit);
            update(ic);
            return reply.value("handled", false);
        } catch (const std::exception &e) {
            ++epoch_;
            client_.disconnect();
            s->session = 0;
            s->snapshot = Json::object();
            s->shift_pending = false;
            panel_.hide();
            ic->inputPanel().reset();
            ic->updatePreedit();
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            diagnostic(e.what());
            launch();
            return false;
        }
    }
    void clear(InputContext *ic, bool destroy) {
        if (!ic)
            return;
        auto *s = ic->propertyFor(&factory_);
        if (s->session && s->epoch == epoch_ && client_.connected()) {
            try {
                client_.call({{"op", destroy ? "destroy" : "reset"}, {"session", s->session}}, 15);
            } catch (...) {
                ++epoch_;
                client_.disconnect();
            }
        }
        s->session = 0;
        s->shift_pending = false;
        s->snapshot = Json::object();
        if (active_.get() == ic)
            panel_.hide();
        ic->inputPanel().reset();
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
    void update(InputContext *ic) {
        auto *s = ic->propertyFor(&factory_);
        auto &r = s->snapshot;
        auto &input = ic->inputPanel();
        const bool was_custom = bool(input.customInputPanelCallback());
        input.reset();
        if (!was_custom)
            ic->updateUserInterface(UserInterfaceComponent::InputPanel, true);
        auto preedit = r.value("preedit", std::string{});
        // Fcitx handles focus-out before the engine's reset callback. The plan
        // requires cancellation, so never let its default path commit preedit.
        Text text(preedit, TextFormatFlags(TextFormatFlag::Underline) | TextFormatFlag::DontCommit);
        text.setCursor(r.value("cursor", 0));
        if (ic->capabilityFlags().test(CapabilityFlag::Preedit) && ic->isPreeditEnabled())
            input.setClientPreedit(text);
        else
            input.setPreedit(text);
        auto reference = ic->watch();
        auto list = std::make_unique<PagedCandidates>(
            r.value("page", 0), r.value("last_page", true), [this, reference](bool backward) {
                if (auto *context = reference.get())
                    operation(context, {{"op", "page"}, {"backward", backward}});
            });
        auto candidates = r.value("candidates", Json::array());
        list->setPageSize(config_["appearance"]["page_size"].get<int>());
        list->setLayoutHint(config_["appearance"]["orientation"] == "vertical"
                                ? CandidateLayoutHint::Vertical
                                : CandidateLayoutHint::Horizontal);
        KeyList keys;
        for (int i = 0; i < int(candidates.size()); ++i) {
            list->append<Candidate>(this, i, candidates[i].value("text", std::string{}),
                                    r.value("id", uint64_t{}));
            keys.emplace_back(static_cast<KeySym>(FcitxKey_1 + i));
        }
        list->setSelectionKey(keys);
        if (!candidates.empty())
            list->setGlobalCursorIndex(
                std::clamp(r.value("highlighted", 0), 0, int(candidates.size()) - 1));
        input.setCandidateList(std::move(list));
        // reset() clears custom callbacks; reinstall only on the R-IME context.
        input.setCustomInputPanelCallback([this](InputContext *context) { show_panel(context); });
        ic->updatePreedit();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    }
    void show_panel(InputContext *ic) {
        if (active_.get() != ic || !ic->hasFocus())
            return;
        auto &s = ic->propertyFor(&factory_)->snapshot;
        panel::Frame frame;
        for (auto &candidate : s.value("candidates", Json::array()))
            frame.items.push_back({candidate.value("text", std::string{}),
                                   candidate.value("comment", std::string{})});
        if (!ic->capabilityFlags().test(CapabilityFlag::Preedit) || !ic->isPreeditEnabled())
            frame.preedit = s.value("preedit", std::string{});
        frame.selected = s.value("highlighted", 0);
        frame.page = s.value("page", 0);
        frame.last_page = s.value("last_page", true);
        if (!panel_.show(ic, frame, config_["appearance"])) {
            diagnostic(panel_.diagnostic());
            auto reference = ic->watch();
            fallback_ = instance_->eventLoop().addDeferEvent([this, reference](EventSource *) {
                fallback_.reset();
                if (auto *context = reference.get(); context && context == active_.get()) {
                    context->inputPanel().setCustomInputPanelCallback({});
                    context->updateUserInterface(UserInterfaceComponent::InputPanel);
                }
                return true;
            });
        } else if (!frame.items.empty() || !frame.preedit.empty())
            diagnostic("");
    }
    Instance *instance_;
    Paths paths_;
    Client client_;
    Json config_;
    std::filesystem::file_time_type config_stamp_{};
    uint64_t epoch_ = 1;
    FactoryFor<State> factory_;
    Panel panel_;
    TrackableObjectReference<InputContext> active_;
    std::vector<std::unique_ptr<SimpleAction>> actions_;
    std::vector<ScopedConnection> connections_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>> watchers_;
    std::unique_ptr<EventSource> fallback_;
    std::chrono::steady_clock::time_point last_launch_{};
    std::string last_diagnostic_;
};
void Candidate::select(InputContext *ic) const {
    engine->choose(ic, index, revision_);
}
class Factory : public AddonFactory {
    AddonInstance *create(AddonManager *manager) override {
        return new Engine(manager->instance());
    }
};
} // namespace rime_ime
FCITX_ADDON_FACTORY(rime_ime::Factory);
