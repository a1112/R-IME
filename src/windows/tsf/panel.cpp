#include "panel.hpp"
#include <algorithm>
#include <dwmapi.h>
#include <stdexcept>
#include <windowsx.h>
namespace rime_ime::tsf {
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0)
        throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), n);
    return result;
}
Panel::Panel(HINSTANCE module, Action action) : module_(module), action_(std::move(action)) {}
Panel::~Panel() {
    if (window_)
        DestroyWindow(window_);
    UnregisterClassW(L"R-IME.CandidateWindow.v1", module_);
    if (font_)
        DeleteObject(font_);
    if (labelFont_)
        DeleteObject(labelFont_);
}
void Panel::hide() {
    if (window_ && IsWindowVisible(window_)) {
        NotifyWinEvent(EVENT_OBJECT_IME_HIDE, window_, OBJID_CLIENT, CHILDID_SELF);
        ShowWindow(window_, SW_HIDE);
    }
    items_.clear();
    lastState_ = nlohmann::json();
    scroll_ = maximumScroll_ = 0;
    pressedAction_ = hovered_ = -99;
    ++revision_;
}
void Panel::show(const nlohmann::json &state, const nlohmann::json &appearance, RECT anchor,
                 HWND owner, std::uint64_t revision) {
    if (state.at("candidates").empty()) {
        hide();
        return;
    }
    if (!window_) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = module_;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = L"R-IME.CandidateWindow.v1";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.style = CS_DROPSHADOW;
        RegisterClassExW(&cls);
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  cls.lpszClassName, L"R-IME candidates", WS_POPUP, 0, 0, 1, 1,
                                  owner, nullptr, module_, this);
        if (!window_)
            return;
    }
    const bool wasVisible = IsWindowVisible(window_) != FALSE;
    lastState_ = state;
    lastAppearance_ = appearance;
    lastAnchor_ = anchor;
    lastOwner_ = owner;
    // The host's DPI awareness is respected; never change its process DPI mode.
    scale_ = owner ? GetDpiForWindow(owner) : 96;
    if (!scale_)
        scale_ = 96;
    auto px = [&](int n) { return MulDiv(n, scale_, 96); };
    std::string theme = appearance.value("theme", "system");
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    dark_ = theme == "dark" || (theme == "system" && !light);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    highContrast_ = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    accent_ = dark_ ? RGB(96, 205, 255) : RGB(0, 103, 192);
    DWORD color = 0; BOOL opaque = FALSE;
    if (!highContrast_ && SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
        accent_ = RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
    if (highContrast_)
        accent_ = GetSysColor(COLOR_HIGHLIGHT);
    BOOL darkFrame = dark_;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));
    auto face = wide(appearance.value("font", std::string{}));
    if (face.empty())
        face = L"Microsoft YaHei UI";
    const int fontSize = appearance.value("font_size", 16);
    if (!font_ || fontFace_ != face || fontSize_ != fontSize || fontDpi_ != scale_) {
        if (font_) DeleteObject(font_);
        if (labelFont_) DeleteObject(labelFont_);
        fontFace_ = face; fontSize_ = fontSize; fontDpi_ = scale_;
        auto make = [&](int size) {
            return CreateFontW(-px(size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
        };
        font_ = make(fontSize);
        labelFont_ = make(std::max(10, fontSize - 3));
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    int maxWidth = std::max(
        px(120), std::min<int>(px(640), monitor.rcWork.right - monitor.rcWork.left - px(16)));
    int maxHeight = std::max(
        px(80), std::min<int>(px(420), monitor.rcWork.bottom - monitor.rcWork.top - px(16)));
    HDC dc = GetDC(window_);
    auto oldFont = SelectObject(dc, font_);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    int gap = px(4), x = px(8), y = px(8), usedWidth = 0, rowHeight = 0;
    items_.clear();
    selected_ = state.value("highlighted", 0);
    revision_ = revision;
    bool vertical = vertical_ = appearance.value("orientation", "horizontal") == "vertical";
    previousPage_ = state.value("page", 0) > 0;
    nextPage_ = !state.value("last_page", true);
    int controls = (previousPage_ ? 1 : 0) + (nextPage_ ? 1 : 0);
    int controlWidth = vertical ? 0 : controls * px(28);
    int footer = vertical && controls ? px(30) : 0;
    int available = std::max(px(48), maxWidth - px(16) - controlWidth);
    hovered_ = -99;
    int count = static_cast<int>(state["candidates"].size());
    int verticalWidth = px(160);
    if (vertical)
        for (int i = 0; i < count; ++i) {
            auto text = wide(state["candidates"][i].value("text", ""));
            SIZE measured{};
            GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &measured);
            verticalWidth = std::max<int>(verticalWidth, measured.cx + px(44));
        }
    verticalWidth = std::min(available, verticalWidth);
    for (int i = 0; i < count; ++i) {
        auto text = wide(state["candidates"][i].value("text", ""));
        auto comment = wide(state["candidates"][i].value("comment", ""));
        SIZE size{};
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        int commentWidth = comment.empty() ? 0 : px(64);
        int width = std::min<int>(available, size.cx + px(40) + commentWidth);
        if (vertical)
            width = verticalWidth;
        commentWidth = std::min(commentWidth, width / 3);
        RECT measured{0, 0, std::max(1, width - px(40) - commentWidth), 0};
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &measured,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int row = std::max<int>(metrics.tmHeight, measured.bottom) + px(12);
        if (x + width > available + px(8) && x > px(8)) {
            x = px(8);
            y += rowHeight + gap;
            rowHeight = 0;
        }
        items_.push_back({{x, y, x + width, y + row}, std::move(text),
                          std::to_wstring(i + 1), std::move(comment), i});
        usedWidth = std::max(usedWidth, x + width);
        rowHeight = std::max(rowHeight, row);
        if (vertical) {
            x = px(8);
            y += row + gap;
            rowHeight = 0;
        } else
            x += width + gap;
    }
    int bottom = px(8);
    for (const auto &item : items_)
        bottom = std::max<int>(bottom, item.rect.bottom);
    width_ = std::min(maxWidth, std::max(px(100), usedWidth + px(8) + controlWidth));
    height_ = std::min(maxHeight, bottom + px(8) + footer);
    viewportTop_ = px(8);
    viewportBottom_ = height_ - px(8) - footer;
    maximumScroll_ = std::max(0, bottom - viewportBottom_);
    scroll_ = std::clamp(scroll_, 0, maximumScroll_);
    if (selected_ >= 0 && selected_ < count) {
        const auto &selected = items_[selected_].rect;
        if (selected.bottom - selected.top > viewportBottom_ - viewportTop_ ||
            selected.top - scroll_ < viewportTop_)
            scroll_ = selected.top - viewportTop_;
        else if (selected.bottom - scroll_ > viewportBottom_)
            scroll_ = selected.bottom - viewportBottom_;
        scroll_ = std::clamp(scroll_, 0, maximumScroll_);
    }
    int controlLeft = width_ - px(8) - controls * px(28);
    int controlTop = vertical ? height_ - footer : px(8);
    auto addControl = [&](int action, const wchar_t *text) {
        items_.push_back({{controlLeft, controlTop, controlLeft + px(28), controlTop + px(28)},
                          text, {}, {}, action});
        controlLeft += px(28);
    };
    if (previousPage_) addControl(-1, L"‹");
    if (nextPage_) addControl(-2, L"›");
    SelectObject(dc, oldFont);
    ReleaseDC(window_, dc);
    int left = std::clamp<int>(anchor.left, monitor.rcWork.left,
                               std::max(monitor.rcWork.left, monitor.rcWork.right - width_));
    int top = anchor.bottom + px(4);
    if (top + height_ > monitor.rcWork.bottom)
        top = anchor.top - height_ - px(4);
    top = std::clamp<int>(top, monitor.rcWork.top,
        std::max(monitor.rcWork.top, monitor.rcWork.bottom - height_));
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, px(16), px(16)), FALSE);
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    SetWindowPos(window_, HWND_TOP, left, top, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
    NotifyWinEvent(wasVisible ? EVENT_OBJECT_IME_CHANGE : EVENT_OBJECT_IME_SHOW,
                   window_, OBJID_CLIENT, CHILDID_SELF);
}
void Panel::paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window_, &ps);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width_, height_);
    auto oldBitmap = SelectObject(buffer, bitmap);
    COLORREF background = highContrast_ ? GetSysColor(COLOR_WINDOW) : dark_ ? RGB(44,44,44) : RGB(255,255,255),
             foreground = highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : dark_ ? RGB(255,255,255) : RGB(26,26,26),
             muted = highContrast_ ? foreground : dark_ ? RGB(181,181,181) : RGB(96,96,96);
    auto brush = CreateSolidBrush(background);
    RECT bounds{0, 0, width_, height_};
    FillRect(buffer, &bounds, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, highContrast_ ? foreground : dark_ ? RGB(72,72,72) : RGB(214,214,214));
    auto oldPen = SelectObject(buffer, pen);
    auto oldBrush = SelectObject(buffer, GetStockObject(NULL_BRUSH));
    RoundRect(buffer, 0, 0, width_, height_, MulDiv(16, scale_, 96), MulDiv(16, scale_, 96));
    SelectObject(buffer, oldBrush);
    SelectObject(buffer, oldPen);
    DeleteObject(pen);
    auto oldFont = SelectObject(buffer, font_);
    SetBkMode(buffer, TRANSPARENT);
    for (const auto &item : items_) {
        const bool candidate = item.action >= 0;
        int saved = SaveDC(buffer);
        RECT itemRect = item.rect;
        if (candidate) {
            OffsetRect(&itemRect, 0, -scroll_);
            IntersectClipRect(buffer, 0, viewportTop_, width_, viewportBottom_);
        }
        bool selected = item.action >= 0 && item.action == selected_;
        bool hovered = item.action == hovered_;
        if (selected || hovered) {
            auto selectedBrush = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) :
                dark_ ? (selected ? RGB(62,62,62) : RGB(55,55,55)) :
                        (selected ? RGB(240,240,240) : RGB(247,247,247)));
            auto previous = SelectObject(buffer, selectedBrush);
            auto previousPen = SelectObject(buffer, GetStockObject(NULL_PEN));
            RoundRect(buffer, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom,
                      MulDiv(8, scale_, 96), MulDiv(8, scale_, 96));
            SelectObject(buffer, previous);
            SelectObject(buffer, previousPen);
            DeleteObject(selectedBrush);
        }
        auto px = [&](int n){ return std::max(1, MulDiv(n, scale_, 96)); };
        if (selected && !highContrast_) {
            RECT indicator = vertical_ ?
                RECT{itemRect.left, itemRect.top + px(6), itemRect.left + px(3), itemRect.bottom - px(6)} :
                RECT{itemRect.left + px(8), itemRect.bottom - px(3), itemRect.right - px(8), itemRect.bottom};
            auto mark = CreateSolidBrush(accent_);
            FillRect(buffer, &indicator, mark); DeleteObject(mark);
        }
        COLORREF textColor = highContrast_ && (selected || hovered) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : foreground;
        SetTextColor(buffer, textColor);
        RECT text = itemRect;
        text.left += px(8);
        text.right -= px(8);
        if (candidate) {
            text.top += px(6);
            text.bottom -= px(6);
            RECT label = text;
            label.right = label.left + px(22);
            SelectObject(buffer, labelFont_);
            SetTextColor(buffer, highContrast_ ? textColor : muted);
            DrawTextW(buffer, item.label.data(), static_cast<int>(item.label.size()), &label,
                      DT_NOPREFIX | DT_SINGLELINE);
            text.left += px(22);
            if (!item.comment.empty()) {
                int commentWidth = std::min<int>(px(64), (itemRect.right - itemRect.left) / 3);
                RECT comment = text; comment.left = comment.right - commentWidth;
                DrawTextW(buffer, item.comment.data(), static_cast<int>(item.comment.size()), &comment,
                          DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
                text.right -= commentWidth;
            }
            SelectObject(buffer, font_);
            SetTextColor(buffer, textColor);
        }
        DrawTextW(buffer, item.text.data(), static_cast<int>(item.text.size()), &text,
                  DT_NOPREFIX | (candidate ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER | DT_CENTER));
        RestoreDC(buffer, saved);
    }
    if (maximumScroll_ > 0) {
        int viewport = viewportBottom_ - viewportTop_;
        int thumbHeight =
            std::max(MulDiv(20, scale_, 96), viewport * viewport / (viewport + maximumScroll_));
        int thumbTop = viewportTop_ + scroll_ * (viewport - thumbHeight) / maximumScroll_;
        RECT thumb{width_ - MulDiv(5, scale_, 96), thumbTop, width_ - MulDiv(2, scale_, 96),
                   thumbTop + thumbHeight};
        auto thumbBrush = CreateSolidBrush(dark_ ? RGB(105, 108, 115) : RGB(178, 183, 190));
        FillRect(buffer, &thumb, thumbBrush);
        DeleteObject(thumbBrush);
    }
    SelectObject(buffer, oldFont);
    BitBlt(dc, 0, 0, width_, height_, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window_, &ps);
}
int Panel::hit(POINT point) const {
    for (const auto &item : items_) {
        RECT hit = item.rect;
        if (item.action >= 0) {
            if (point.y < viewportTop_ || point.y >= viewportBottom_)
                continue;
            OffsetRect(&hit, 0, -scroll_);
        }
        if (item.action >= -2 && PtInRect(&hit, point))
            return item.action;
    }
    return -99;
}
LRESULT CALLBACK Panel::procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Panel *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        self->paint();
        return 0;
    case WM_MOUSEMOVE: {
        if (!self->trackingMouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            self->trackingMouse_ = TrackMouseEvent(&tracking) != FALSE;
        }
        int item = self->hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (item != self->hovered_) {
            self->hovered_ = item;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        self->trackingMouse_ = false;
        self->hovered_ = -99;
        self->pressedAction_ = -99;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        self->pressedAction_ = -99;
        return 0;
    case WM_LBUTTONDOWN:
        if (!self->trackingMouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            self->trackingMouse_ = TrackMouseEvent(&tracking) != FALSE;
        }
        self->pressedAction_ = self->hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        self->pressedRevision_ = self->revision_;
        return 0;
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        auto action = self->pressedAction_;
        self->pressedAction_ = -99;
        if (action != -99 && self->pressedRevision_ == self->revision_ &&
            self->hit(point) == action)
            self->action_(action, self->revision_);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (self->maximumScroll_ > 0) {
            int delta =
                MulDiv(GET_WHEEL_DELTA_WPARAM(w), MulDiv(72, self->scale_, 96), WHEEL_DELTA);
            self->scroll_ = std::clamp(self->scroll_ - delta, 0, self->maximumScroll_);
            InvalidateRect(window, nullptr, FALSE);
        } else if (GET_WHEEL_DELTA_WPARAM(w) > 0 ? self->previousPage_ : self->nextPage_)
            self->action_(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : -2, self->revision_);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DPICHANGED:
        if (IsWindowVisible(window) && !self->lastState_.is_null()) {
            auto state = self->lastState_, appearance = self->lastAppearance_;
            self->show(state, appearance, self->lastAnchor_, self->lastOwner_, self->revision_);
        }
        return 0;
    case WM_NCDESTROY:
        self->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, w, l);
    default:
        return DefWindowProcW(window, message, w, l);
    }
}
} // namespace rime_ime::tsf
