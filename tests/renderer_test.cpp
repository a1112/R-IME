#include "../src/panel/renderer.hpp"
#include <cmath>
#include <iostream>
#include <pango/pangocairo.h>
#include <stdexcept>
using namespace rime_ime::panel;
int main(int argc, char **argv) {
    try {
        Renderer renderer;
        Frame frame{{{"你好", ""},
                     {"这是一个需要自动换行的很长的候选词语，用来检查候选窗口的实际排版高度。",
                      "自定义短语"},
                     {"输入法", ""}},
                    "ni hao shi jie — a long preedit remains one line",
                    1};
        for (const auto *theme : {"light", "dark"})
            for (const auto *orientation : {"horizontal", "vertical"}) {
                renderer.configure({{"font_size", 16},
                                    {"theme", theme},
                                    {"orientation", orientation},
                                    {"font", "sans-serif"}},
                                   false);
                auto layout = renderer.layout(frame, 300);
                if (layout.width > 300 || layout.cells.size() != 3)
                    throw std::runtime_error("invalid layout geometry");
                auto *surface = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, std::ceil(layout.width), std::ceil(layout.height));
                auto *cr = cairo_create(surface);
                auto *pango = pango_cairo_create_layout(cr);
                auto *font = pango_font_description_from_string("sans-serif");
                pango_font_description_set_absolute_size(font, 16 * PANGO_SCALE);
                pango_layout_set_font_description(pango, font);
                pango_layout_set_text(pango, frame.items[1].text.c_str(), -1);
                pango_layout_set_width(pango, int((layout.cells[1].w - 40) * PANGO_SCALE));
                pango_layout_set_wrap(pango, PANGO_WRAP_WORD_CHAR);
                int height = 0;
                pango_layout_get_pixel_size(pango, nullptr, &height);
                if (layout.cells[1].h < height + 12 + 19)
                    throw std::runtime_error("long candidate overlaps comment");
                pango_font_description_free(font);
                g_object_unref(pango);
                renderer.paint(cr, frame, layout);
                if (cairo_status(cr) != CAIRO_STATUS_SUCCESS)
                    throw std::runtime_error("Cairo rendering failed");
                if (argc > 1) {
                    auto path =
                        std::string(argv[1]) + "/candidate-" + theme + "-" + orientation + ".png";
                    if (cairo_surface_write_to_png(surface, path.c_str()) != CAIRO_STATUS_SUCCESS)
                        throw std::runtime_error("cannot write preview");
                }
                cairo_destroy(cr);
                cairo_surface_destroy(surface);
            }
        renderer.configure({{"font_size", 40}, {"orientation", "vertical"}}, false);
        Frame overflow;
        overflow.selected = 8;
        for (int i = 0; i < 9; ++i)
            overflow.items.push_back({std::string(300, 'W'), "long phrase"});
        auto bounded = renderer.layout(overflow, 300, 400);
        if (bounded.height > 400 || bounded.max_scroll <= 0 ||
            bounded.hit(bounded.cells[8].x + 10, bounded.cells[8].y + 10) != 8)
            throw std::runtime_error("last candidate not reachable in bounded viewport");
        auto top = renderer.layout(overflow, 300, 400, 0);
        if (top.hit(top.cells[0].x + 10, top.cells[0].y + 10) != 0 ||
            top.hit(20, top.next.y + 5) != -1)
            throw std::runtime_error("scroll viewport hit testing failed");
        std::cout
            << "Pango long-candidate metrics, bounded scrolling and four rendered themes passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
