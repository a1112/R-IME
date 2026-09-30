#pragma once
#include "layout.hpp"
#include <cairo.h>
#include <nlohmann/json.hpp>
namespace rime_ime::panel {
struct Frame {
    std::vector<Item> items;
    std::string preedit;
    int selected = 0, page = 0;
    bool last_page = true;
};
class Renderer {
  public:
    void configure(const nlohmann::json &appearance, bool system_dark);
    Layout layout(const Frame &, double max_width = 720, double max_height = 420,
                  double scroll = -1) const;
    void paint(cairo_t *, const Frame &, const Layout &) const;

  private:
    double size_ = 16;
    std::string font_ = "sans-serif";
    bool dark_ = false, vertical_ = false;
};
} // namespace rime_ime::panel
