#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fcitx;
namespace {
void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

// This is a minimal real frontend input context, not a mock of the addon or IPC.
class HostContext final : public InputContext {
public:
    explicit HostContext(InputContextManager &manager)
        : InputContext(manager, "r-ime-integration-host") {
        created();
        setEnablePreedit(true);
        setCapabilityFlags(CapabilityFlag::Preedit);
    }
    ~HostContext() override { destroy(); }
    const char *frontend() const override { return "r-ime-integration-host"; }
    std::vector<std::string> commits, preedits;
    int forwarded = 0;
    bool key(const std::string &name, bool release = false) {
        KeyEvent event(this, Key(name), release);
        keyEvent(event);
        return event.accepted();
    }
    void type(const std::string &text) {
        for (char letter : text) require(key(std::string(1, letter)), "letter was not consumed: " + std::string(1, letter));
    }
private:
    void commitStringImpl(const std::string &text) override { commits.push_back(text); }
    void forwardKeyImpl(const ForwardKeyEvent &) override { ++forwarded; }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void updatePreeditImpl() override { preedits.push_back(inputPanel().clientPreedit().toString()); }
};

class Scenario {
public:
    explicit Scenario(Instance &instance) : instance_(instance) {}
    int result = 1;
    void run() {
        step([this] {
            require(instance_.addonManager().addonInfo("r-ime") != nullptr, "actual r-ime addon configuration was not discovered");
            require(instance_.addonManager().addon("r-ime", true) != nullptr, "actual r-ime module failed to load");
            require(instance_.addonManager().loadedAddonNames() == std::vector<std::string>{"r-ime"}, "host loaded unrelated addons");
            require(instance_.inputMethodManager().entry("r-ime") != nullptr, "r-ime input method entry is absent");
            auto group = instance_.inputMethodManager().currentGroup();
            group.inputMethodList().clear();
            group.inputMethodList().emplace_back("r-ime");
            group.setDefaultInputMethod("r-ime");
            instance_.inputMethodManager().setGroup(std::move(group));
            context_ = std::make_unique<HostContext>(instance_.inputContextManager());
            context_->focusIn();
            instance_.setCurrentInputMethod(context_.get(), "r-ime", true);
            require(instance_.inputMethod(context_.get()) == "r-ime", "R-IME was not activated");
            context_->type("nihao");
            require(!context_->inputPanel().clientPreedit().empty(), "frontend did not receive client preedit");
            require(!context_->preedits.empty() && !context_->preedits.back().empty(), "preedit callback did not run");
            auto candidates = context_->inputPanel().candidateList();
            require(candidates && candidates->size() > 0, "real Rime candidates are absent");
            require(candidates->candidate(0).text().toString() == "你好", "unexpected real first candidate");
            later([this] { fallback_and_commit(); });
        });
    }
private:
    Instance &instance_;
    std::unique_ptr<HostContext> context_;
    std::unique_ptr<EventSourceTime> timer_;
    void step(const std::function<void()> &operation) {
        try { operation(); }
        catch (const std::exception &error) {
            std::cerr << "Fcitx host assertion failed: " << error.what() << '\n';
            instance_.exit();
        }
    }
    void later(std::function<void()> operation) {
        timer_ = instance_.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            now(CLOCK_MONOTONIC) + 10000, 0,
            [this, operation = std::move(operation)](EventSourceTime *, uint64_t) {
                timer_.reset();
                step(operation);
                return true;
            });
    }
    void fallback_and_commit() {
        require(!context_->inputPanel().customInputPanelCallback(), "headless fallback retained custom panel callback");
        require(context_->key("space"), "space did not select candidate");
        require(context_->commits == std::vector<std::string>{"你好"}, "space did not commit exactly once");
        require(context_->inputPanel().clientPreedit().empty(), "commit left stale preedit");
        context_->type("nihao");
        auto candidates = context_->inputPanel().candidateList();
        require(candidates && candidates->size(), "click selection had no candidates");
        candidates->candidate(0).select(context_.get());
        require(context_->commits == std::vector<std::string>{"你好", "你好"}, "candidate callback did not commit exactly once");
        // A stale callback must never deliver a second commit after its candidate
        // list has been replaced. Keep the old list alive as a GUI click could.
        candidates->candidate(0).select(context_.get());
        require(context_->commits.size() == 2, "repeated stale candidate click duplicated a commit");
        context_->type("ni");
        auto next_preedit = context_->inputPanel().clientPreedit().toString();
        candidates->candidate(0).select(context_.get());
        require(context_->commits.size() == 2 && context_->inputPanel().clientPreedit().toString() == next_preedit,
                "stale candidate from previous composition selected the new composition");
        context_->key("Shift_L");
        require(context_->key("Shift+Shift_L", true), "Shift tap was not handled");
        require(context_->commits.size() == 3 && context_->commits.back() == "ni", "Shift tap failed to commit raw composition exactly once");
        require(!context_->key("a"), "ASCII mode consumed an ordinary key");
        context_->key("Shift_L");
        context_->key("Shift+Shift_L", true);
        require(!context_->key("Control+c"), "Ctrl+C was swallowed without composition");
        context_->key("Shift_L");
        context_->key("Control+c");
        context_->key("Shift+Shift_L", true);
        context_->type("nihao");
        require(!context_->inputPanel().clientPreedit().empty(), "Shift chord incorrectly toggled ASCII");
        context_->focusOut();
        require(context_->inputPanel().clientPreedit().empty(), "focusOut did not cancel preedit");
        require(!context_->inputPanel().customInputPanelCallback(), "focusOut retained custom panel callback");
        require(context_->commits.size() == 3, "focusOut committed cancelled composition");
        context_->focusIn();
        require(!context_->key("space"), "new focus selected a stale candidate");
        require(context_->commits.size() == 3, "new focus duplicated old commit");
        later([this] { sensitive_and_context_isolation(); });
    }
    void sensitive_and_context_isolation() {
        context_->setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) | CapabilityFlag::Password);
        require(!context_->key("n"), "password input was consumed");
        require(context_->inputPanel().clientPreedit().empty(), "password input exposed preedit");
        require(!context_->inputPanel().customInputPanelCallback(), "password input installed custom panel callback");
        context_->setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) | CapabilityFlag::Sensitive);
        require(!context_->key("n"), "sensitive input was consumed");
        context_->setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) | CapabilityFlag::Disable);
        require(!context_->key("n"), "disabled input was consumed");
        context_->focusOut();

        // An unrelated, unfocused context belongs to its own frontend/UI. R-IME
        // activity must not overwrite its preedit or callback. This checks context
        // ownership; it does not substitute for testing another installed IM.
        HostContext other(instance_.inputContextManager());
        other.inputPanel().setClientPreedit(Text("other frontend preedit"));
        other.inputPanel().setCustomInputPanelCallback([](InputContext *) {});
        context_->setCapabilityFlags(CapabilityFlag::Preedit);
        context_->focusIn();
        instance_.setCurrentInputMethod(context_.get(), "r-ime", true);
        context_->type("nihao");
        require(other.inputPanel().clientPreedit().toString() == "other frontend preedit", "R-IME changed unrelated context preedit");
        require(bool(other.inputPanel().customInputPanelCallback()), "R-IME cleared unrelated context callback");
        context_->focusOut();
        require(context_->commits.size() == 3, "sensitive/context transitions produced unexpected commits");
        std::cout << "Real Fcitx addon host: preedit, Space, candidate click once, Shift tap/chord, focus cancellation, sensitive bypass, unrelated-context isolation, headless fallback passed\n";
        result = 0;
        instance_.exit();
    }
};
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    setupTestingEnvironment(argv[1], {argv[2]}, {argv[1]});
    setenv("FCITX_DATA_DIRS", argv[1], 1);
    Log::setLogRule("default=2");
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    char name[] = "r-ime-fcitx-host-test", disable[] = "--disable=all", enable[] = "--enable=r-ime";
    char *arguments[] = {name, disable, enable};
    Instance instance(3, arguments);
    instance.addonManager().registerDefaultLoader(nullptr);
    Scenario scenario(instance);
    instance.eventDispatcher().schedule([&scenario] { scenario.run(); });
    instance.exec();
    return scenario.result;
}
