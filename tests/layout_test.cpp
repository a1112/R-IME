#include "../src/panel/layout.hpp"
#include <iostream>
#include <stdexcept>
using namespace rime_ime::panel;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        auto measure = [](const std::string &text) { return text.size() * 8.; };
        auto l = arrange({{"hello", ""}, {"a long candidate that must wrap", ""}, {"third", ""}},
                         true, false, 16, 240, measure);
        require(l.cells.size() == 3, "all candidates must have hit regions");
        require(l.width <= 240, "panel must fit available screen width");
        for (size_t i = 0; i < l.cells.size(); i++) {
            auto r = l.cells[i];
            require(r.w > 0 && r.h > 0, "candidate must have nonempty bounds");
            require(l.hit(r.x + r.w / 2, r.y + r.h / 2) == int(i),
                    "hit test must select correct candidate");
            require(r.x + r.w <= l.width && r.y + r.h <= l.height, "candidate must fit panel");
        }
        require(l.hit(-1, -1) == -1, "outside click must not select");
        auto v = arrange({{"one", ""}, {"two", ""}}, false, true, 16, 300, measure);
        require(v.cells[1].y > v.cells[0].y, "vertical candidates must stack");
        auto a = anchor(1910, 1070, 20, 300, 100, {0, 0, 1920, 1080});
        require(a.x + a.w <= 1920 && a.y + a.h <= 1080 && a.y < 1070,
                "bottom edge must flip popup above cursor");
        auto b = anchor(-1800, 30, 20, 300, 100, {-1920, 0, 1920, 1080});
        require(b.x < 0 && b.x >= -1920, "negative-origin screen coordinates must be preserved");
        std::cout << "panel layout tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
