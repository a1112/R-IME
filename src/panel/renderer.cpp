#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <pango/pangocairo.h>
namespace rime_ime::panel {
namespace {
void round(cairo_t *c, double x, double y, double w, double h, double r) {
    constexpr double pi = 3.141592653589793;
    cairo_new_sub_path(c);
    cairo_arc(c, x + w - r, y + r, r, -pi / 2, 0);
    cairo_arc(c, x + w - r, y + h - r, r, 0, pi / 2);
    cairo_arc(c, x + r, y + h - r, r, pi / 2, pi);
    cairo_arc(c, x + r, y + r, r, pi, pi * 1.5);
    cairo_close_path(c);
}
void color(cairo_t *c, double r, double g, double b, double a = 1) {
    cairo_set_source_rgba(c, r, g, b, a);
}
PangoLayout *text_layout(cairo_t *c, const std::string &text, const std::string &font, double size,
                         double width = -1, bool single_line = false) {
    auto *l = pango_cairo_create_layout(c);
    auto *f = pango_font_description_from_string(font.c_str());
    pango_font_description_set_absolute_size(f, size * PANGO_SCALE);
    pango_layout_set_font_description(l, f);
    pango_font_description_free(f);
    pango_layout_set_text(l, text.c_str(), int(text.size()));
    if (width >= 0) {
        pango_layout_set_width(l, int(width * PANGO_SCALE));
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_height(l, -3);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    }
    if (single_line) {
        pango_layout_set_height(l, -1);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(l, true);
    }
    return l;
}
void text(cairo_t *c, const std::string &s, const std::string &font, double size, double x,
          double y, double width = -1, bool single_line = false) {
    auto *l = text_layout(c, s, font, size, width, single_line);
    cairo_move_to(c, x, y);
    pango_cairo_show_layout(c, l);
    g_object_unref(l);
}
} // namespace
void Renderer::configure(const nlohmann::json &a, bool system_dark) {
    size_ = a.value("font_size", 16);
    font_ = a.value("font", std::string{});
    if (font_.empty())
        font_ = "sans-serif";
    auto theme = a.value("theme", std::string("system"));
    dark_ = theme == "dark" || (theme == "system" && system_dark);
    vertical_ = a.value("orientation", std::string("horizontal")) == "vertical";
}
Layout Renderer::layout(const Frame &f, double max_width, double max_height, double scroll) const {
    auto *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto *c = cairo_create(s);
    auto l = arrange(
        f.items, !f.preedit.empty(), vertical_, size_, max_width,
        [&](const std::string &t) {
            auto *p = text_layout(c, t, font_, size_);
            int w = 0;
            pango_layout_get_pixel_size(p, &w, nullptr);
            g_object_unref(p);
            return double(w);
        },
        [&](const std::string &t, double width) {
            auto *p = text_layout(c, t, font_, size_, width);
            int h = 0;
            pango_layout_get_pixel_size(p, nullptr, &h);
            g_object_unref(p);
            return double(h);
        });
    cairo_destroy(c);
    cairo_surface_destroy(s);
    const double top = f.preedit.empty() ? 10 : l.preedit.y + l.preedit.h + 6;
    const double original_bottom = l.previous.y - 6;
    l.height = std::min(l.height, std::max(120., max_height));
    l.previous.y = l.next.y = l.height - 28;
    l.viewport = {0, top, l.width, std::max(1., l.previous.y - 6 - top)};
    l.max_scroll = std::max(0., original_bottom - top - l.viewport.h);
    if (scroll < 0) {
        scroll = 0;
        if (f.selected >= 0 && f.selected < int(l.cells.size())) {
            auto cell = l.cells[f.selected];
            scroll = std::max(0., cell.y + cell.h - (top + l.viewport.h));
        }
    }
    l.scroll = std::clamp(scroll, 0., l.max_scroll);
    for (auto &cell : l.cells)
        cell.y -= l.scroll;
    return l;
}
void Renderer::paint(cairo_t *c, const Frame &f, const Layout &l) const {
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    color(c, 0, 0, 0, 0);
    cairo_paint(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);
    round(c, 1, 2, l.width - 2, l.height - 3, 10);
    color(c, 0, 0, 0, .12);
    cairo_fill(c);
    round(c, .5, .5, l.width - 1, l.height - 3, 10);
    double bg = dark_ ? .15 : .985;
    color(c, bg, bg, bg);
    cairo_fill_preserve(c);
    color(c, dark_ ? .32 : .80, dark_ ? .32 : .80, dark_ ? .32 : .80);
    cairo_set_line_width(c, 1);
    cairo_stroke(c);
    if (!f.preedit.empty()) {
        double fg = dark_ ? .8 : .38;
        color(c, fg, fg, fg);
        text(c, f.preedit, font_, size_ * .85, l.preedit.x, l.preedit.y, l.preedit.w, true);
    }
    cairo_save(c);
    cairo_rectangle(c, l.viewport.x, l.viewport.y, l.viewport.w, l.viewport.h);
    cairo_clip(c);
    for (size_t i = 0; i < f.items.size(); ++i) {
        auto r = l.cells[i];
        bool selected = int(i) == f.selected;
        if (selected) {
            round(c, r.x, r.y, r.w, r.h, 6);
            color(c, .20, .47, .96);
            cairo_fill(c);
        }
        double fg = selected ? 1 : (dark_ ? .94 : .12);
        color(c, fg, fg, fg, selected ? .8 : .55);
        text(c, std::to_string(i + 1), font_, size_ * .70, r.x + 7, r.y + 10);
        color(c, fg, fg, fg);
        text(c, f.items[i].text, font_, size_, r.x + size_ + 12, r.y + 6, r.w - size_ - 24);
        if (!f.items[i].comment.empty()) {
            color(c, fg, fg, fg, .62);
            text(c, f.items[i].comment, font_, size_ * .7, r.x + size_ + 12,
                 r.y + r.h - size_ * 1.1, r.w - size_ - 24, true);
        }
    }
    cairo_restore(c);
    double fg = dark_ ? .75 : .4;
    if (l.max_scroll > 0) {
        color(c, fg, fg, fg, .5);
        double track = l.viewport.h;
        double thumb = std::max(16., track * track / (track + l.max_scroll));
        cairo_rectangle(c, l.width - 5, l.viewport.y + (track - thumb) * l.scroll / l.max_scroll, 2,
                        thumb);
        cairo_fill(c);
    }
    color(c, fg, fg, fg);
    text(c, std::to_string(f.page + 1), font_, 11, 12, l.previous.y + 2);
    color(c, fg, fg, fg, f.page > 0 ? 1 : .25);
    text(c, "‹", font_, 18, l.previous.x + 7, l.previous.y - 3);
    color(c, fg, fg, fg, f.last_page ? .25 : 1);
    text(c, "›", font_, 18, l.next.x + 7, l.next.y - 3);
}
} // namespace rime_ime::panel
