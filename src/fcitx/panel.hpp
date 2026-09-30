#pragma once
#include "../panel/renderer.hpp"
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <functional>
#include <memory>
namespace rime_ime {
class Panel {
  public:
    using Select = std::function<void(fcitx::InputContext *, int)>;
    using Page = std::function<void(fcitx::InputContext *, bool)>;
    Panel(fcitx::Instance *, Select, Page);
    ~Panel();
    bool show(fcitx::InputContext *, const panel::Frame &, const nlohmann::json &appearance);
    void hide();
    const std::string &diagnostic() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
} // namespace rime_ime
