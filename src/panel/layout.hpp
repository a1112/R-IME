#pragma once
#include <functional>
#include <string>
#include <vector>
namespace rime_ime::panel {
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
struct Item {
    std::string text, comment;
};
struct Layout {
    double width = 0, height = 0;
    Rect preedit, previous, next, viewport;
    double scroll = 0, max_scroll = 0;
    std::vector<Rect> cells;
    int hit(double x, double y) const;
};
using Measure = std::function<double(const std::string &)>;
using MeasureHeight = std::function<double(const std::string &, double)>;
Layout arrange(const std::vector<Item> &items, bool preedit, bool vertical, double font_size,
               double max_width, const Measure &measure, const MeasureHeight &height = {});
Rect anchor(double x, double y, double cursor_height, double width, double height, Rect screen);
} // namespace rime_ime::panel
