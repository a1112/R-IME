#include "platform.hpp"
#include "tsf/guids.hpp"
#include <filesystem>
#include <iostream>
#include <msctf.h>
#include <sstream>
#include <stdexcept>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace rime_ime::tsf;
namespace {
void require(HRESULT result, const char *operation) {
    if (SUCCEEDED(result))
        return;
    std::ostringstream error;
    error << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(result)
          << ")";
    if (result == E_ACCESSDENIED || result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
        error << ". Windows TSF and machine COM registration require an elevated installer.";
    throw std::runtime_error(error.str());
}
std::wstring com_key() {
    return std::wstring(L"Software\\Classes\\CLSID\\") + service_guid;
}
void string_value(const std::wstring &path, const wchar_t *name, const std::wstring &value) {
    HKEY key{};
    auto result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE,
                                  nullptr, &key, nullptr);
    require(HRESULT_FROM_WIN32(result), "Create machine COM registration");
    result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    require(HRESULT_FROM_WIN32(result), "Write machine COM registration");
}
std::wstring registered_dll() {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, (com_key() + L"\\InprocServer32").c_str(), nullptr,
                     RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return value;
}
constexpr const GUID *categories[] = {&GUID_TFCAT_TIP_KEYBOARD, &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
                                      &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT};
void enable_user_layout(bool enable) {
    // Windows' modern keyboard list is maintained by Input.dll. Enabling only
    // the legacy ITfInputProcessorProfiles flag does not install a selectable TIP.
    HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!input)
        require(HRESULT_FROM_WIN32(GetLastError()), "Load Windows input service");
    using Install = BOOL(WINAPI *)(LPCWSTR, DWORD);
    auto install = reinterpret_cast<Install>(GetProcAddress(input, "InstallLayoutOrTip"));
    if (!install) {
        FreeLibrary(input);
        throw std::runtime_error("Windows InstallLayoutOrTip is unavailable");
    }
    std::wstring tip = std::wstring(L"0x0804:") + service_guid + profile_guid;
    // ILOT_UNINSTALL=1. Zero enables only this TIP, preserving the default and
    // every other keyboard layout; never use ILOT_DEFPROFILE/CLEANINSTALL.
    BOOL result = install(tip.c_str(), enable ? 0 : 1);
    DWORD error = GetLastError();
    FreeLibrary(input);
    if (!result)
        require(error ? HRESULT_FROM_WIN32(error) : E_FAIL,
                enable ? "Enable current-user Windows input-list entry"
                       : "Disable current-user Windows input-list entry");
}
struct Registration {
    ComPtr<ITfInputProcessorProfiles> profiles;
    ComPtr<ITfInputProcessorProfileMgr> manager;
    ComPtr<ITfCategoryMgr> category;
    Registration() {
        require(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&profiles)),
                "Create TSF profiles");
        require(profiles.As(&manager), "Get TSF profile manager");
        require(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&category)),
                "Create TSF category manager");
    }
    bool exists() {
        TF_INPUTPROCESSORPROFILE profile{};
        return SUCCEEDED(manager->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, language_id, service_id,
                                             profile_id, nullptr, &profile));
    }
    void enable_user() {
        require(profiles->EnableLanguageProfile(service_id, language_id, profile_id, TRUE),
                "Enable current-user R-IME language profile");
        enable_user_layout(true);
        // The modern API also tracks its enabled state. This enables and
        // activates only the short-lived registration process; it does not
        // change the user's default or another application's active profile.
        require(manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, language_id, service_id,
                                         profile_id, nullptr,
                                         TF_IPPMF_FORPROCESS | TF_IPPMF_ENABLEPROFILE |
                                             TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE),
                "Enable current-user modern TSF profile");
    }
    void install(const std::filesystem::path &dll) {
        if (!std::filesystem::is_regular_file(dll))
            throw std::runtime_error("The TSF DLL does not exist");
        auto previous = registered_dll();
        bool existing = exists(), createdProfile = false;
        try {
            string_value(com_key(), nullptr, display_name);
            string_value(com_key() + L"\\InprocServer32", nullptr,
                         std::filesystem::absolute(dll).wstring());
            string_value(com_key() + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
            auto path = std::filesystem::absolute(dll).wstring();
            require(manager->RegisterProfile(service_id, language_id, profile_id, display_name, 5,
                                             path.c_str(), static_cast<ULONG>(path.size()), 0,
                                             nullptr, 0, FALSE, 0),
                    "Register TSF language profile");
            createdProfile = !existing;
            for (auto id : categories)
                require(category->RegisterCategory(service_id, *id, service_id),
                        "Register TSF category");
            // Enable only this user's profile. Never set the default or activate
            // it globally: existing input-method preferences remain intact.
            require(profiles->EnableLanguageProfile(service_id, language_id, profile_id, TRUE),
                    "Enable current-user R-IME profile");
            if (!exists())
                throw std::runtime_error("Windows did not enumerate the new R-IME profile");
            // TSF loads the text service through the machine COM registration.
            // Remove only our own obsolete per-user class shadow, in this
            // helper's architecture view, after the machine install succeeds.
            auto shadow = RegDeleteTreeW(HKEY_CURRENT_USER, com_key().c_str());
            if (shadow != ERROR_FILE_NOT_FOUND)
                require(HRESULT_FROM_WIN32(shadow), "Remove obsolete R-IME per-user COM shadow");
        } catch (...) {
            if (createdProfile) {
                for (auto id : categories)
                    category->UnregisterCategory(service_id, *id, service_id);
                manager->UnregisterProfile(service_id, language_id, profile_id, 0);
            }
            if (previous.empty())
                RegDeleteTreeW(HKEY_LOCAL_MACHINE, com_key().c_str());
            else
                string_value(com_key() + L"\\InprocServer32", nullptr, previous);
            throw;
        }
    }
    void uninstall() {
        profiles->EnableLanguageProfile(service_id, language_id, profile_id, FALSE);
        // An absent profile is an idempotent success; errors removing an existing
        // registration are reported so uninstall cannot claim a false success.
        if (exists()) {
            for (auto id : categories)
                require(category->UnregisterCategory(service_id, *id, service_id),
                        "Unregister TSF category");
            require(manager->UnregisterProfile(service_id, language_id, profile_id, 0),
                    "Unregister TSF profile");
        }
        auto status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, com_key().c_str());
        if (status != ERROR_FILE_NOT_FOUND)
            require(HRESULT_FROM_WIN32(status), "Remove machine COM registration");
        status = RegDeleteTreeW(HKEY_CURRENT_USER, com_key().c_str());
        if (status != ERROR_FILE_NOT_FOUND)
            require(HRESULT_FROM_WIN32(status), "Remove obsolete R-IME per-user COM shadow");
    }
    void check() {
        if (!exists())
            throw std::runtime_error("R-IME is not registered in Windows TSF");
        TF_INPUTPROCESSORPROFILE profile{};
        require(manager->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR, language_id, service_id,
                                    profile_id, nullptr, &profile),
                "Query registered profile category");
        if (profile.catid != GUID_TFCAT_TIP_KEYBOARD)
            throw std::runtime_error(
                "R-IME is not registered in the Windows keyboard TIP category");
        BOOL enabled = FALSE;
        require(profiles->IsEnabledLanguageProfile(service_id, language_id, profile_id, &enabled),
                "Query profile enabled state");
        if (!enabled)
            throw std::runtime_error(
                "R-IME profile is registered but disabled for the current user");
        auto path = registered_dll();
        if (path.empty() || !std::filesystem::is_regular_file(path))
            throw std::runtime_error("R-IME machine COM server path is absent or invalid");
        ComPtr<ITfTextInputProcessorEx> service;
        require(CoCreateInstance(service_id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)),
                "Instantiate registered R-IME TSF service");
        std::wcout << L"R-IME keyboard profile registered, enabled, and machine COM-loadable: "
                   << path << L'\n';
    }
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        require(com, "Initialize COM");
        Registration registration;
        if (argc < 2)
            throw std::runtime_error("Usage: r-ime-register --register [r-ime-tsf.dll] | "
                                     "--unregister | --enable-user | --disable-user | --check");
        std::wstring action = argv[1];
        if (action == L"--register") {
            registration.install(argc >= 3 ? std::filesystem::path(argv[2])
                                           : rime_ime::win::module_directory() / L"r-ime-tsf.dll");
            std::cout << "R-IME registered. Existing default input method was preserved.\n";
        } else if (action == L"--unregister") {
            registration.uninstall();
            std::cout
                << "R-IME registration removed. User dictionaries and settings were preserved.\n";
        } else if (action == L"--enable-user") {
            registration.enable_user();
            std::cout
                << "R-IME added to the current user's Windows input list; default preserved.\n";
        } else if (action == L"--disable-user") {
            enable_user_layout(false);
            std::cout << "R-IME removed from the current user's Windows input list.\n";
        } else if (action == L"--check")
            registration.check();
        else
            throw std::runtime_error("Unknown registration command");
        registration.category.Reset();
        registration.manager.Reset();
        registration.profiles.Reset();
        CoUninitialize();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (SUCCEEDED(com))
            CoUninitialize();
        return 1;
    }
}
