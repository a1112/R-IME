#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

// SDDL declarations require the Windows SDK partition macros above.
#include <sddl.h>
namespace rime_ime::win {
inline std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()),
                                nullptr, 0);
    if (!n)
        throw std::runtime_error("invalid UTF-8");
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), n);
    return result;
}
inline std::string utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr,
                                nullptr);
    std::string result(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), n, nullptr,
                        nullptr);
    return result;
}
inline std::wstring sid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("cannot query user token");
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    BOOL ok = GetTokenInformation(token, TokenUser, data.data(), size, &size);
    CloseHandle(token);
    LPWSTR value{};
    if (!ok ||
        !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &value))
        throw std::runtime_error("cannot read user SID");
    std::wstring result(value);
    LocalFree(value);
    return result;
}
inline std::wstring pipe_name(const std::filesystem::path &runtime) {
    // Stable across x86/x64 and independent of std::hash implementation.
    uint64_t hash = 14695981039346656037ull;
    for (auto c : runtime.wstring()) {
        hash ^= uint16_t(towlower(c));
        hash *= 1099511628211ull;
    }
    return L"\\\\.\\pipe\\r-ime-" + sid() + L"-" + std::to_wstring(hash);
}
inline std::filesystem::path module_directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path();
}
inline std::filesystem::path install_directory() {
    auto path = module_directory();
    if (path.filename() == L"x86")
        path = path.parent_path();
    return path;
}
inline DWORD remaining(ULONGLONG end) {
    auto now = GetTickCount64();
    return now >= end ? 0 : DWORD(end - now);
}
inline bool transfer(HANDLE pipe, void *data, DWORD bytes, bool writing, ULONGLONG end) {
    auto *current = static_cast<char *>(data);
    while (bytes) {
        OVERLAPPED overlap{};
        overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlap.hEvent)
            return false;
        DWORD count{};
        BOOL ok = writing ? WriteFile(pipe, current, bytes, &count, &overlap)
                          : ReadFile(pipe, current, bytes, &count, &overlap);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(overlap.hEvent, remaining(end)) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(pipe, &overlap, &count, FALSE);
            else {
                CancelIoEx(pipe, &overlap);
                GetOverlappedResult(pipe, &overlap, &count, TRUE);
                ok = FALSE;
            }
        }
        CloseHandle(overlap.hEvent);
        if (!ok || !count)
            return false;
        current += count;
        bytes -= count;
    }
    return true;
}
inline constexpr DWORD maximum_frame = 136 * 1024 * 1024;
} // namespace rime_ime::win
