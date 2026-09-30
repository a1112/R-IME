#include "../src/windows/tsf/panel.hpp"
#include <iostream>
#include <stdexcept>
using namespace rime_ime::tsf;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        HWND owner =
            CreateWindowExW(0, L"STATIC", L"R-IME panel test", WS_OVERLAPPEDWINDOW, 100, 100, 300,
                            200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        int chosen = -99;
        std::uint64_t chosenRevision = 0;
        Panel panel(GetModuleHandleW(nullptr), [&](int item, std::uint64_t revision) {
            chosen = item;
            chosenRevision = revision;
        });
        nlohmann::json state = {{"candidates", nlohmann::json::array()},
                                {"highlighted", 8},
                                {"page", 0},
                                {"last_page", true}};
        std::string longText;
        for (int i = 0; i < 30; ++i)
            longText += "中华人民共和国";
        for (int i = 0; i < 9; ++i)
            state["candidates"].push_back({{"text", longText}});
        nlohmann::json appearance = {
            {"font_size", 40}, {"orientation", "vertical"}, {"theme", "light"}};
        panel.show(state, appearance, {1900, 1000, 1910, 1020}, owner, 17);
        auto window = FindWindowW(L"R-IME.CandidateWindow.v1", nullptr);
        check(window, "candidate window exists");
        auto dpi = GetDpiForWindow(owner);
        int point = MulDiv(20, dpi, 96);
        RECT rect{};
        GetWindowRect(window, &rect);
        check(rect.bottom - rect.top <= MulDiv(420, dpi, 96),
              "long candidates have a bounded scroll viewport");
        SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(point, point));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point, point));
        check(chosen == 8 && chosenRevision == 17,
              "selected long candidate is scrolled into view without shrinking text");
        for (int i = 0; i < 100; ++i)
            SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(point, point));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point, point));
        check(chosen == 0, "scrolling reaches the first candidate without paging");
        SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(point, point));
        panel.show(state, appearance, {100, 100, 110, 124}, owner, 18);
        chosen = -99;
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point, point));
        check(chosen == -99, "click pressed on an old frame cannot select a new candidate");
        panel.hide();
        chosen = -99;
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point, point));
        check(chosen == -99, "hidden candidate has no stale clickable entries");
        DestroyWindow(owner);
        std::cout << "Windows panel: long text wraps, selected item scrolls into view, all "
                     "candidates reachable, hidden callbacks cleared\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
