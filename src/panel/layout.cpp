#include "layout.hpp"
#include <algorithm>
#include <cmath>
namespace rime_ime::panel {
int Layout::hit(double x, double y) const {
    if (viewport.h > 0 && !viewport.contains(x, y))
        return -1;
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].contains(x, y))
            return int(i);
    return -1;
}
Layout arrange(const std::vector<Item> &items, bool show_preedit, bool vertical, double font,
               double max_width, const Measure &measure, const MeasureHeight &height) {
    Layout l;
    const double padding = 10, gap = 4, line = std::ceil(font * 1.5),
                 width = std::max(120., max_width);
    double x = padding, y = padding, row = 0, right = 0;
    if (show_preedit) {
        l.preedit = {padding, y, width - 2 * padding, line};
        y += line + 6;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const double available = width - 2 * padding;
        const double text_width = measure(items[i].text);
        double w = std::min(available, std::max(52., text_width + font + 24));
        if (vertical)
            w = available;
        double h =
            (height ? height(items[i].text, std::max(1., w - font - 24))
                    : line * std::max(1., std::ceil(text_width / std::max(1., w - font - 24)))) +
            12;
        if (!items[i].comment.empty())
            h += line * .8;
        if (x > padding && (vertical || x + w > width - padding)) {
            x = padding;
            y += row + gap;
            row = 0;
        }
        l.cells.push_back({x, y, w, h});
        right = std::max(right, x + w);
        row = std::max(row, h);
        x += w + gap;
    }
    l.width = std::min(width, std::max(120., right + padding));
    if (show_preedit)
        l.preedit.w = l.width - 2 * padding;
    y += row + 6;
    l.previous = {l.width - 62, y, 24, 20};
    l.next = {l.width - 34, y, 24, 20};
    l.height = y + 28;
    return l;
}
Rect anchor(double x, double y, double cursor_height, double width, double height, Rect screen) {
    width = std::min(width, screen.w);
    height = std::min(height, screen.h);
    double top = y + cursor_height + 4;
    if (top + height > screen.y + screen.h)
        top = y - height - 4;
    return {std::clamp(x, screen.x, screen.x + screen.w - width),
            std::clamp(top, screen.y, screen.y + screen.h - height), width, height};
}
} // namespace rime_ime::panel
