#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <windows.h>
namespace rime_ime::tsf {
std::wstring wide(const std::string &text);
class Panel final {
  public:
    using Action = std::function<void(int, std::uint64_t)>;
    explicit Panel(HINSTANCE module, Action action);
    ~Panel();
    void show(const nlohmann::json &state, const nlohmann::json &appearance, RECT anchor,
              HWND owner, std::uint64_t revision);
    void hide();

  private:
    struct Item {
        RECT rect;
        std::wstring text;
        std::wstring label;
        std::wstring comment;
        int action;
    };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    int hit(POINT point) const;
    void paint();
    HWND window_ = nullptr;
    HINSTANCE module_;
    HFONT font_ = nullptr;
    HFONT labelFont_ = nullptr;
    std::wstring fontFace_;
    int fontSize_ = 0, fontDpi_ = 0;
    std::vector<Item> items_;
    Action action_;
    std::uint64_t revision_ = 0;
    std::uint64_t pressedRevision_ = 0;
    int pressedAction_ = -99;
    int selected_ = 0, width_ = 0, height_ = 0, scale_ = 96;
    int hovered_ = -99;
    int scroll_ = 0, maximumScroll_ = 0, viewportTop_ = 0, viewportBottom_ = 0;
    bool dark_ = false;
    bool vertical_ = false, highContrast_ = false, trackingMouse_ = false;
    bool previousPage_ = false, nextPage_ = false;
    COLORREF accent_ = RGB(0, 103, 192);
    nlohmann::json lastState_, lastAppearance_;
    RECT lastAnchor_{};
    HWND lastOwner_ = nullptr;
};
} // namespace rime_ime::tsf
