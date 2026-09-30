#include "panel.hpp"
#include <algorithm>
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
}
void Panel::hide() {
    if (window_)
        ShowWindow(window_, SW_HIDE);
    items_.clear();
    scroll_ = maximumScroll_ = 0;
    pressedAction_ = -99;
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
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  cls.lpszClassName, L"R-IME candidates", WS_POPUP, 0, 0, 1, 1,
                                  owner, nullptr, module_, this);
        if (!window_)
            return;
    }
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
    if (font_)
        DeleteObject(font_);
    auto face = wide(appearance.value("font", std::string{}));
    if (face.empty())
        face = L"Microsoft YaHei UI";
    font_ = CreateFontW(-px(appearance.value("font_size", 16)), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
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
    bool vertical = appearance.value("orientation", "horizontal") == "vertical";
    int count = static_cast<int>(state["candidates"].size());
    for (int i = 0; i < count; ++i) {
        auto text = std::to_wstring(i + 1) + L"  " + wide(state["candidates"][i].value("text", ""));
        SIZE size{};
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        int width = std::min<int>(maxWidth - px(16), size.cx + px(24));
        if (vertical)
            width = maxWidth - px(16);
        RECT measured{0, 0, width - px(24), 0};
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &measured,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int row = std::max<int>(metrics.tmHeight, measured.bottom) + px(20);
        if (x + width + px(8) > maxWidth && x > px(8)) {
            x = px(8);
            y += rowHeight + gap;
            rowHeight = 0;
        }
        items_.push_back({{x, y, x + width, y + row}, std::move(text), i});
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
    width_ = std::max(px(180), usedWidth + px(8));
    height_ = std::min(maxHeight, bottom + px(38));
    viewportTop_ = px(8);
    viewportBottom_ = height_ - px(38);
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
    std::wstring status = state.value("ascii", false) ? L"A" : L"中";
    status += L"   " + std::to_wstring(state.value("page", 0) + 1);
    items_.push_back({{px(12), height_ - px(30), width_ - px(86), height_ - px(6)}, status, -10});
    if (state.value("page", 0) > 0)
        items_.push_back(
            {{width_ - px(76), height_ - px(32), width_ - px(44), height_ - px(4)}, L"‹", -1});
    if (!state.value("last_page", true))
        items_.push_back(
            {{width_ - px(40), height_ - px(32), width_ - px(8), height_ - px(4)}, L"›", -2});
    SelectObject(dc, oldFont);
    ReleaseDC(window_, dc);
    int left = std::clamp<int>(anchor.left, monitor.rcWork.left,
                               std::max(monitor.rcWork.left, monitor.rcWork.right - width_));
    int top = anchor.bottom + px(4);
    if (top + height_ > monitor.rcWork.bottom)
        top = anchor.top - height_ - px(4);
    top = std::max<int>(monitor.rcWork.top, top);
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, px(20), px(20)), FALSE);
    SetWindowLongPtrW(window_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    SetWindowPos(window_, HWND_TOPMOST, left, top, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}
void Panel::paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window_, &ps);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width_, height_);
    auto oldBitmap = SelectObject(buffer, bitmap);
    COLORREF background = dark_ ? RGB(38, 39, 43) : RGB(250, 250, 252),
             foreground = dark_ ? RGB(239, 240, 242) : RGB(40, 43, 48);
    auto brush = CreateSolidBrush(background);
    RECT bounds{0, 0, width_, height_};
    FillRect(buffer, &bounds, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, dark_ ? RGB(75, 76, 81) : RGB(211, 213, 218));
    auto oldPen = SelectObject(buffer, pen);
    auto oldBrush = SelectObject(buffer, GetStockObject(NULL_BRUSH));
    RoundRect(buffer, 0, 0, width_, height_, MulDiv(20, scale_, 96), MulDiv(20, scale_, 96));
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
        if (selected) {
            auto selectedBrush = CreateSolidBrush(RGB(48, 119, 232));
            auto previous = SelectObject(buffer, selectedBrush);
            auto previousPen = SelectObject(buffer, GetStockObject(NULL_PEN));
            RoundRect(buffer, itemRect.left, itemRect.top, itemRect.right, itemRect.bottom,
                      MulDiv(12, scale_, 96), MulDiv(12, scale_, 96));
            SelectObject(buffer, previous);
            SelectObject(buffer, previousPen);
            DeleteObject(selectedBrush);
        }
        SetTextColor(buffer, selected ? RGB(255, 255, 255) : foreground);
        RECT text = itemRect;
        text.left += MulDiv(12, scale_, 96);
        text.right -= MulDiv(12, scale_, 96);
        if (candidate) {
            text.top += MulDiv(10, scale_, 96);
            text.bottom -= MulDiv(10, scale_, 96);
        }
        DrawTextW(buffer, item.text.data(), static_cast<int>(item.text.size()), &text,
                  DT_NOPREFIX | (candidate ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER));
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
    case WM_LBUTTONDOWN:
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
        } else
            self->action_(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : -2, self->revision_);
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
