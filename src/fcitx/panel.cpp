#include "panel.hpp"
#include "vendor/waylandim_public.h"
#include "vendor/wl_output.h"
#include "vendor/wl_surface.h"
#include "vendor/zwp_input_method_v2.h"
#include "wayland-fractional-scale-v1-client-protocol.h"
#include "wayland-input-method-unstable-v1-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"
#include <algorithm>
#include <cairo-xcb.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fstream>
#include <linux/input-event-codes.h>
#include <map>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland_public.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb_public.h>

namespace rime_ime {
namespace {
bool system_dark() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string path = xdg ? xdg : std::string(home ? home : "") + "/.config";
    std::ifstream f(path + "/kdeglobals");
    std::string line;
    bool group = false;
    while (std::getline(f, line)) {
        if (line.starts_with("["))
            group = line == "[Colors:Window]";
        if (group && line.starts_with("BackgroundNormal=")) {
            int r = 255, g = 255, b = 255;
            char comma;
            std::istringstream in(line.substr(17));
            if (in >> r >> comma >> g >> comma >> b)
                return .2126 * r + .7152 * g + .0722 * b < 128;
        }
    }
    return false;
}
struct ShmBuffer {
    wl_buffer *buffer = nullptr;
    void *memory = MAP_FAILED;
    size_t bytes = 0;
    bool busy = false;
    std::function<void()> released;
    ~ShmBuffer() {
        if (buffer)
            wl_buffer_destroy(buffer);
        if (memory != MAP_FAILED)
            munmap(memory, bytes);
    }
};
} // namespace
struct Panel::Impl {
    fcitx::Instance *instance;
    Select select;
    Page page;
    panel::Renderer renderer;
    panel::Frame frame;
    panel::Layout layout;
    fcitx::TrackableObjectReference<fcitx::InputContext> active;
    std::string error;
    struct Wayland;
    struct XWindow;
    std::map<std::string, std::unique_ptr<Wayland>> wayland;
    std::map<std::string, std::unique_ptr<XWindow>> x11;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionCreated>> wayland_created;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::WaylandConnectionClosed>> wayland_closed;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::XCBConnectionCreated>> x_created;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::XCBConnectionClosed>> x_closed;
    fcitx::AddonInstance *waylandim = nullptr;
    Impl(fcitx::Instance *i, Select s, Page p);
    ~Impl();
    void clicked(double x, double y) {
        auto *ic = active.get();
        if (!ic || !ic->hasFocus())
            return;
        int index = layout.hit(x, y);
        if (index >= 0)
            select(ic, index);
        else if (layout.previous.contains(x, y) && frame.page > 0)
            page(ic, true);
        else if (layout.next.contains(x, y) && !frame.last_page)
            page(ic, false);
    }
    void hide();
    void scroll(double delta);
};

struct Panel::Impl::Wayland {
    Impl *parent;
    wl_display *display;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    wl_seat *seat = nullptr;
    wl_pointer *pointer = nullptr;
    zwp_input_panel_v1 *panel = nullptr;
    wp_fractional_scale_manager_v1 *fractional_manager = nullptr;
    wp_viewporter *viewporter = nullptr;
    std::unique_ptr<fcitx::wayland::WlSurface> surface;
    zwp_input_panel_surface_v1 *panel_role = nullptr;
    zwp_input_popup_surface_v2 *popup_role = nullptr;
    wp_fractional_scale_v1 *fractional = nullptr;
    wp_viewport *viewport = nullptr;
    std::vector<std::unique_ptr<ShmBuffer>> buffers;
    std::map<uint32_t, std::unique_ptr<fcitx::wayland::WlOutput>> outputs;
    std::map<fcitx::wayland::WlOutput *, int> output_scales;
    std::vector<fcitx::ScopedConnection> signals;
    std::unique_ptr<fcitx::EventSource> repaint;
    fcitx::TrackableObjectReference<fcitx::InputContext> context;
    double scale = 1, px = 0, py = 0;
    bool pointer_inside = false, shown = false, pending_paint = false;
    fcitx::ScopedConnection surface_enter;
    Wayland(Impl *p, wl_display *d) : parent(p), display(d) {
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {
            [](void *data, wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
                auto &w = *static_cast<Wayland *>(data);
                if (!strcmp(interface, "wl_compositor"))
                    w.compositor = static_cast<wl_compositor *>(
                        wl_registry_bind(r, name, &wl_compositor_interface, std::min(version, 4u)));
                else if (!strcmp(interface, "wl_shm"))
                    w.shm = static_cast<wl_shm *>(wl_registry_bind(r, name, &wl_shm_interface, 1));
                else if (!strcmp(interface, "zwp_input_panel_v1"))
                    w.panel = static_cast<zwp_input_panel_v1 *>(
                        wl_registry_bind(r, name, &zwp_input_panel_v1_interface, 1));
                else if (!strcmp(interface, "wp_fractional_scale_manager_v1"))
                    w.fractional_manager = static_cast<wp_fractional_scale_manager_v1 *>(
                        wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1));
                else if (!strcmp(interface, "wp_viewporter"))
                    w.viewporter = static_cast<wp_viewporter *>(
                        wl_registry_bind(r, name, &wp_viewporter_interface, 1));
                else if (!strcmp(interface, "wl_output")) {
                    auto output = std::make_unique<fcitx::wayland::WlOutput>(
                        static_cast<wl_output *>(wl_registry_bind(r, name, &wl_output_interface,
                                                                  std::min(version, 2u))));
                    auto *raw = output.get();
                    w.output_scales[raw] = 1;
                    w.signals.emplace_back(raw->scale().connect(
                        [&w, raw](int32_t s) { w.output_scales[raw] = std::max(1, s); }));
                    w.outputs.emplace(name, std::move(output));
                } else if (!strcmp(interface, "wl_seat") && !w.seat) {
                    w.seat = static_cast<wl_seat *>(
                        wl_registry_bind(r, name, &wl_seat_interface, std::min(version, 5u)));
                    w.init_seat();
                }
            },
            [](void *data, wl_registry *, uint32_t name) {
                auto &w = *static_cast<Wayland *>(data);
                auto it = w.outputs.find(name);
                if (it != w.outputs.end()) {
                    w.output_scales.erase(it->second.get());
                    w.outputs.erase(it);
                }
            }};
        wl_registry_add_listener(registry, &listener, this);
        wl_display_flush(display);
    }
    ~Wayland() {
        hide();
        repaint.reset();
        signals.clear();
        outputs.clear();
        buffers.clear();
        if (pointer)
            wl_pointer_destroy(pointer);
        if (seat)
            wl_seat_destroy(seat);
        if (panel)
            zwp_input_panel_v1_destroy(panel);
        if (fractional_manager)
            wp_fractional_scale_manager_v1_destroy(fractional_manager);
        if (viewporter)
            wp_viewporter_destroy(viewporter);
        if (shm)
            wl_shm_destroy(shm);
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
    }
    void init_seat() {
        static const wl_seat_listener sl = {
            [](void *data, wl_seat *s, uint32_t capabilities) {
                auto &w = *static_cast<Wayland *>(data);
                if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !w.pointer) {
                    w.pointer = wl_seat_get_pointer(s);
                    w.init_pointer();
                } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && w.pointer) {
                    wl_pointer_destroy(w.pointer);
                    w.pointer = nullptr;
                }
            },
            [](void *, wl_seat *, const char *) {}};
        wl_seat_add_listener(seat, &sl, this);
    }
    void init_pointer() {
        static const wl_pointer_listener pl = {
            [](void *d, wl_pointer *, uint32_t, wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
                auto &w = *static_cast<Wayland *>(d);
                w.pointer_inside = w.surface && s == fcitx::wayland::rawPointer(w.surface.get());
                w.px = wl_fixed_to_double(x);
                w.py = wl_fixed_to_double(y);
            },
            [](void *d, wl_pointer *, uint32_t, wl_surface *) {
                static_cast<Wayland *>(d)->pointer_inside = false;
            },
            [](void *d, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
                auto &w = *static_cast<Wayland *>(d);
                w.px = wl_fixed_to_double(x);
                w.py = wl_fixed_to_double(y);
            },
            [](void *d, wl_pointer *, uint32_t, uint32_t, uint32_t button, uint32_t state) {
                auto &w = *static_cast<Wayland *>(d);
                if (w.pointer_inside && button == BTN_LEFT &&
                    state == WL_POINTER_BUTTON_STATE_RELEASED)
                    w.parent->clicked(w.px, w.py);
            },
            [](void *d, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) {
                auto &w = *static_cast<Wayland *>(d);
                if (w.pointer_inside && axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                    w.parent->scroll(wl_fixed_to_double(value) * 3);
            },
            [](void *, wl_pointer *) {},
            [](void *, wl_pointer *, uint32_t) {},
            [](void *, wl_pointer *, uint32_t, uint32_t) {},
            [](void *, wl_pointer *, uint32_t, int32_t) {},
            nullptr,
            nullptr};
        wl_pointer_add_listener(pointer, &pl, this);
    }
    void defer_paint() {
        if (!shown || repaint)
            return;
        repaint = parent->instance->eventLoop().addDeferEvent([this](fcitx::EventSource *) {
            repaint.reset();
            if (shown)
                paint();
            return true;
        });
    }
    bool show(fcitx::InputContext *ic) {
        if (!compositor || !shm)
            return false;
        if (context.get() != ic)
            hide();
        if (!surface) {
            if (ic->frontendName() != "wayland" && ic->frontendName() != "wayland_v2")
                return false;
            surface = std::make_unique<fcitx::wayland::WlSurface>(
                wl_compositor_create_surface(compositor));
            // Host classicUI recognizes the actual wrapper; null application userdata
            // prevents its pointer handler from treating this as one of its windows.
            surface->setUserData(nullptr);
            scale = std::max(1., ic->scaleFactor());
            surface_enter = surface->enter().connect([this](fcitx::wayland::WlOutput *output) {
                if (!fractional && output_scales.contains(output)) {
                    scale = output_scales[output];
                    defer_paint();
                }
            });
            if (ic->frontendName() == "wayland" && panel) {
                panel_role = zwp_input_panel_v1_get_input_panel_surface(
                    panel, fcitx::wayland::rawPointer(surface.get()));
                zwp_input_panel_surface_v1_set_overlay_panel(panel_role);
            } else if (ic->frontendName() == "wayland_v2" && parent->waylandim) {
                auto *im = parent->waylandim->call<fcitx::IWaylandIMModule::getInputMethodV2>(ic);
                if (im) {
                    popup_role = zwp_input_method_v2_get_input_popup_surface(
                        fcitx::wayland::rawPointer(im), fcitx::wayland::rawPointer(surface.get()));
                    static const zwp_input_popup_surface_v2_listener listener = {
                        [](void *, zwp_input_popup_surface_v2 *, int32_t, int32_t, int32_t,
                           int32_t) {}};
                    zwp_input_popup_surface_v2_add_listener(popup_role, &listener, this);
                }
            }
            if (!panel_role && !popup_role) {
                hide();
                return false;
            }
            if (fractional_manager && viewporter) {
                viewport = wp_viewporter_get_viewport(viewporter,
                                                      fcitx::wayland::rawPointer(surface.get()));
                fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
                    fractional_manager, fcitx::wayland::rawPointer(surface.get()));
                static const wp_fractional_scale_v1_listener listener = {
                    [](void *d, wp_fractional_scale_v1 *, uint32_t s) {
                        auto &w = *static_cast<Wayland *>(d);
                        w.scale = std::max(1., s / 120.);
                        w.defer_paint();
                    }};
                wp_fractional_scale_v1_add_listener(fractional, &listener, this);
            }
            context = ic->watch();
        }
        shown = true;
        return paint();
    }
    bool paint() {
        if (!shown || !surface)
            return false;
        buffers.erase(
            std::remove_if(buffers.begin(), buffers.end(), [](auto &b) { return !b->busy; }),
            buffers.end());
        if (buffers.size() >= 3) {
            pending_paint = true;
            return true;
        }
        pending_paint = false;
        double actual_scale = viewport ? scale : std::ceil(scale);
        int width = int(std::ceil(parent->layout.width * actual_scale)),
            height = int(std::ceil(parent->layout.height * actual_scale));
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
        if (width <= 0 || height <= 0 || stride <= 0)
            return false;
        auto b = std::make_unique<ShmBuffer>();
        b->bytes = size_t(stride) * height;
        int fd = memfd_create("r-ime-candidate", MFD_CLOEXEC);
        if (fd < 0)
            return false;
        if (ftruncate(fd, off_t(b->bytes)) < 0) {
            close(fd);
            return false;
        }
        b->memory = mmap(nullptr, b->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (b->memory == MAP_FAILED) {
            close(fd);
            return false;
        }
        auto *pool = wl_shm_create_pool(shm, fd, int(b->bytes));
        b->buffer =
            wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        b->released = [this] {
            if (pending_paint)
                defer_paint();
        };
        static const wl_buffer_listener listener = {[](void *d, wl_buffer *) {
            auto *b = static_cast<ShmBuffer *>(d);
            b->busy = false;
            if (b->released)
                b->released();
        }};
        wl_buffer_add_listener(b->buffer, &listener, b.get());
        auto *s = cairo_image_surface_create_for_data(static_cast<unsigned char *>(b->memory),
                                                      CAIRO_FORMAT_ARGB32, width, height, stride);
        auto *c = cairo_create(s);
        cairo_scale(c, actual_scale, actual_scale);
        parent->renderer.paint(c, parent->frame, parent->layout);
        cairo_destroy(c);
        cairo_surface_destroy(s);
        auto *raw = fcitx::wayland::rawPointer(surface.get());
        wl_surface_set_buffer_scale(raw, viewport ? 1 : int(actual_scale));
        if (viewport)
            wp_viewport_set_destination(viewport, int(parent->layout.width),
                                        int(parent->layout.height));
        wl_surface_attach(raw, b->buffer, 0, 0);
        wl_surface_damage(raw, 0, 0, INT32_MAX, INT32_MAX);
        b->busy = true;
        buffers.push_back(std::move(b));
        wl_surface_commit(raw);
        wl_display_flush(display);
        return true;
    }
    void hide() {
        shown = false;
        pointer_inside = false;
        pending_paint = false;
        repaint.reset();
        context = {};
        if (popup_role) {
            zwp_input_popup_surface_v2_destroy(popup_role);
            popup_role = nullptr;
        }
        if (panel_role) {
            zwp_input_panel_surface_v1_destroy(panel_role);
            panel_role = nullptr;
        }
        if (fractional) {
            wp_fractional_scale_v1_destroy(fractional);
            fractional = nullptr;
        }
        if (viewport) {
            wp_viewport_destroy(viewport);
            viewport = nullptr;
        }
        surface_enter.disconnect();
        surface.reset();
    }
};

struct Panel::Impl::XWindow {
    Impl *parent;
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    xcb_window_t window = 0;
    cairo_surface_t *canvas = nullptr;
    double scale = 1;
    bool shown = false;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::XCBEventFilter>> filter;
    XWindow(Impl *p, xcb_connection_t *c, int n, const std::string &name,
            fcitx::AddonInstance *module)
        : parent(p), connection(c) {
        auto it = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (int i = 0; i < n && it.rem; i++)
            xcb_screen_next(&it);
        screen = it.data;
        filter = module->call<fcitx::IXCBModule::addEventFilter>(
            name, [this](xcb_connection_t *, xcb_generic_event_t *e) {
                auto type = e->response_type & ~0x80;
                if (type == XCB_EXPOSE) {
                    auto *x = reinterpret_cast<xcb_expose_event_t *>(e);
                    if (x->window == window) {
                        paint();
                        return true;
                    }
                }
                if (type == XCB_BUTTON_RELEASE) {
                    auto *b = reinterpret_cast<xcb_button_release_event_t *>(e);
                    if (b->event == window) {
                        if (b->detail == 1)
                            parent->clicked(b->event_x / scale, b->event_y / scale);
                        else if (b->detail == 4 || b->detail == 5)
                            parent->scroll(b->detail == 4 ? -48 : 48);
                        return true;
                    }
                }
                return false;
            });
    }
    ~XWindow() {
        filter.reset();
        if (canvas)
            cairo_surface_destroy(canvas);
        if (window)
            xcb_destroy_window(connection, window);
    }
    bool show(fcitx::InputContext *ic) {
        if (!screen || ic->capabilityFlags().test(fcitx::CapabilityFlag::RelativeRect))
            return false;
        scale = std::max(1., ic->scaleFactor());
        auto rect = ic->cursorRect();
        panel::Rect monitor{0, 0, double(screen->width_in_pixels),
                            double(screen->height_in_pixels)};
        auto cookie = xcb_randr_get_monitors(connection, screen->root, 1);
        auto *reply = xcb_randr_get_monitors_reply(connection, cookie, nullptr);
        if (reply) {
            for (auto it = xcb_randr_get_monitors_monitors_iterator(reply); it.rem;
                 xcb_randr_monitor_info_next(&it)) {
                auto *m = it.data;
                if (rect.left() >= m->x && rect.left() < m->x + m->width && rect.top() >= m->y &&
                    rect.top() < m->y + m->height) {
                    monitor = {double(m->x), double(m->y), double(m->width), double(m->height)};
                    break;
                }
            }
            free(reply);
        }
        parent->layout =
            parent->renderer.layout(parent->frame, std::min(720., monitor.w / scale - 8),
                                    std::min(420., monitor.h / scale - 8));
        auto place = panel::anchor(rect.left(), rect.top(), rect.height(),
                                   std::ceil(parent->layout.width * scale),
                                   std::ceil(parent->layout.height * scale), monitor);
        if (!window) {
            window = xcb_generate_id(connection);
            uint32_t values[] = {screen->white_pixel, 1,
                                 XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_RELEASE};
            xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1, 1, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                              XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
                              values);
            // ICCCM input hint: never take keyboard focus.
            auto atom_cookie = xcb_intern_atom(connection, 0, 8, "WM_HINTS");
            auto *atom = xcb_intern_atom_reply(connection, atom_cookie, nullptr);
            if (atom) {
                uint32_t hints[9] = {1, 0, 0, 0, 0, 0, 0, 0, 0};
                xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, atom->atom,
                                    atom->atom, 32, 9, hints);
                free(atom);
            }
        }
        uint32_t geometry[] = {uint32_t(int(place.x)), uint32_t(int(place.y)), uint32_t(place.w),
                               uint32_t(place.h), XCB_STACK_MODE_ABOVE};
        xcb_configure_window(connection, window,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
                                 XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE,
                             geometry);
        // Shape the opaque X11 window as well; this works without a compositor.
        std::vector<xcb_rectangle_t> strips;
        int radius = std::min(int(std::ceil(10 * scale)), int(place.h / 2));
        for (int y = 0; y < int(place.h); ++y) {
            double dy = y < radius ? radius - y - .5
                                   : (y >= int(place.h) - radius ? y - (place.h - radius) + .5 : 0);
            int inset =
                dy > 0 ? int(std::ceil(radius - std::sqrt(std::max(0., radius * radius - dy * dy))))
                       : 0;
            strips.push_back(
                {int16_t(inset), int16_t(y), uint16_t(std::max(1, int(place.w) - 2 * inset)), 1});
        }
        xcb_shape_rectangles(connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                             XCB_CLIP_ORDERING_YX_BANDED, window, 0, 0, strips.size(),
                             strips.data());
        if (canvas) {
            cairo_surface_destroy(canvas);
            canvas = nullptr;
        }
        xcb_visualtype_t *visual = nullptr;
        for (auto d = xcb_screen_allowed_depths_iterator(screen); d.rem; xcb_depth_next(&d)) {
            for (auto v = xcb_depth_visuals_iterator(d.data); v.rem; xcb_visualtype_next(&v))
                if (v.data->visual_id == screen->root_visual)
                    visual = v.data;
        }
        if (!visual)
            return false;
        canvas = cairo_xcb_surface_create(connection, window, visual, int(place.w), int(place.h));
        xcb_map_window(connection, window);
        shown = true;
        paint();
        return true;
    }
    void paint() {
        if (!shown || !canvas)
            return;
        auto *c = cairo_create(canvas);
        cairo_scale(c, scale, scale);
        parent->renderer.paint(c, parent->frame, parent->layout);
        cairo_destroy(c);
        cairo_surface_flush(canvas);
        xcb_flush(connection);
    }
    void hide() {
        shown = false;
        if (window) {
            xcb_unmap_window(connection, window);
            xcb_flush(connection);
        }
    }
};

Panel::Impl::Impl(fcitx::Instance *i, Select s, Page p)
    : instance(i), select(std::move(s)), page(std::move(p)) {
    auto *m = i->addonManager().addon("wayland", true);
    waylandim = i->addonManager().addon("waylandim", true);
    if (m) {
        wayland_created = m->call<fcitx::IWaylandModule::addConnectionCreatedCallback>(
            [this](const std::string &name, wl_display *d, fcitx::FocusGroup *) {
                wayland.emplace("wayland:" + name, std::make_unique<Wayland>(this, d));
            });
        wayland_closed = m->call<fcitx::IWaylandModule::addConnectionClosedCallback>(
            [this](const std::string &name, wl_display *) { wayland.erase("wayland:" + name); });
    }
    auto *x = i->addonManager().addon("xcb", true);
    if (x) {
        x_created = x->call<fcitx::IXCBModule::addConnectionCreatedCallback>(
            [this, x](const std::string &name, xcb_connection_t *c, int screen,
                      fcitx::FocusGroup *) {
                x11.emplace("x11:" + name, std::make_unique<XWindow>(this, c, screen, name, x));
            });
        x_closed = x->call<fcitx::IXCBModule::addConnectionClosedCallback>(
            [this](const std::string &name, xcb_connection_t *) { x11.erase("x11:" + name); });
    }
}
Panel::Impl::~Impl() {
    wayland_created.reset();
    wayland_closed.reset();
    x_created.reset();
    x_closed.reset();
    wayland.clear();
    x11.clear();
}
void Panel::Impl::scroll(double delta) {
    layout = renderer.layout(frame, layout.width, layout.height, layout.scroll + delta);
    for (auto &[_, w] : wayland)
        if (w->shown)
            w->defer_paint();
    for (auto &[_, w] : x11)
        if (w->shown)
            w->paint();
}
void Panel::Impl::hide() {
    for (auto &[_, w] : wayland)
        w->hide();
    for (auto &[_, w] : x11)
        w->hide();
    active = {};
}
Panel::Panel(fcitx::Instance *i, Select s, Page p)
    : d_(std::make_unique<Impl>(i, std::move(s), std::move(p))) {}
Panel::~Panel() = default;
bool Panel::show(fcitx::InputContext *ic, const panel::Frame &frame,
                 const nlohmann::json &appearance) {
    if (!ic || !ic->hasFocus() || (frame.items.empty() && frame.preedit.empty())) {
        hide();
        return true;
    }
    if (d_->active.get() != ic)
        d_->hide();
    d_->active = ic->watch();
    d_->frame = frame;
    d_->renderer.configure(appearance, system_dark());
    d_->layout = d_->renderer.layout(frame);
    bool ok = false;
    auto display = ic->display();
    if (d_->wayland.contains(display))
        ok = d_->wayland[display]->show(ic);
    else if (d_->x11.contains(display))
        ok = d_->x11[display]->show(ic);
    if (!ok) {
        d_->hide();
        d_->error = "专属候选窗暂不可用，已使用 Fcitx 候选窗。请检查 KDE Wayland 输入法配置。";
    } else
        d_->error.clear();
    return ok;
}
void Panel::hide() {
    d_->hide();
}
const std::string &Panel::diagnostic() const {
    return d_->error;
}
} // namespace rime_ime
