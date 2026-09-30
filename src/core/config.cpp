#include "rime_ime/config.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#include <shlobj.h>
#include <windows.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace rime_ime {
namespace {
constexpr std::size_t archive_limit = 64 * 1024 * 1024;
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
fs::path env_path(const char *name, const fs::path &fallback) {
    auto value = std::getenv(name);
    if (value && *value && fs::path(value).is_absolute())
        return value;
    return fallback;
}
void no_symlink(const fs::path &path) {
    fs::path current;
    for (const auto &component : fs::absolute(path)) {
        current /= component;
        check(!fs::is_symlink(fs::symlink_status(current)),
              "symbolic link is not allowed: " + current.string());
    }
}
void private_directory(const fs::path &path) {
    no_symlink(path);
    fs::create_directories(path);
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
}
std::string encode_hex(const std::string &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
std::string decode_hex(const std::string &value) {
    check(value.size() % 2 == 0, "invalid backup encoding");
    std::string out;
    out.reserve(value.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        throw std::runtime_error("invalid backup encoding");
    };
    for (std::size_t i = 0; i < value.size(); i += 2)
        out += static_cast<char>((nibble(value[i]) << 4) | nibble(value[i + 1]));
    return out;
}
} // namespace
Paths Paths::from_environment() {
#ifdef _WIN32
    wchar_t override_path[32768]{};
    auto length = GetEnvironmentVariableW(L"RIME_USER_ROOT", override_path, 32768);
    fs::path root;
    if (length && length < 32768 && fs::path(override_path).is_absolute())
        root = override_path;
    else {
        PWSTR local{};
        check(
            SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)),
            "cannot find local app data");
        root = fs::path(local) / L"R-IME";
        CoTaskMemFree(local);
    }
    return {root / L"config" / L"config.json", root / L"data", root / L"cache", root / L"runtime"};
#else
    auto home = env_path("HOME", fs::current_path());
    auto runtime = env_path("XDG_RUNTIME_DIR", {});
    runtime = runtime.empty() ? fs::path("/tmp") / ("r-ime-" + std::to_string(getuid()))
                              : runtime / "r-ime";
    return {env_path("XDG_CONFIG_HOME", home / ".config") / "r-ime" / "config.json",
            env_path("XDG_DATA_HOME", home / ".local/share") / "r-ime",
            env_path("XDG_CACHE_HOME", home / ".cache") / "r-ime", runtime};
#endif
}
Json default_config() {
    return {{"version", 1},
            {"general",
             {{"ascii", false},
              {"simplified", true},
              {"ascii_punctuation", false},
              {"learning", true}}},
            {"input", {{"schema", "rime_ime_pinyin"}}},
            {"appearance",
             {{"theme", "system"},
              {"font", ""},
              {"font_size", 16},
              {"page_size", 5},
              {"orientation", "horizontal"}}},
            {"shortcuts",
             {{"toggle_ascii", "Shift"},
              {"next_page", "Page_Down"},
              {"previous_page", "Page_Up"},
              {"cancel", "Escape"}}}};
}
void validate_config(const Json &cfg) {
    check(cfg.dump().size() <= 1024 * 1024, "configuration exceeds 1 MiB");
    check(cfg.is_object() && cfg.value("version", 0) == 1, "unsupported configuration version");
    const auto &g = cfg.at("general");
    for (auto key : {"ascii", "simplified", "ascii_punctuation", "learning"})
        check(g.at(key).is_boolean(), std::string("expected boolean: ") + key);
    const auto &a = cfg.at("appearance");
    check(a.at("page_size").is_number_integer(), "page size must be an integer");
    int page = a.at("page_size");
    check(page >= 3 && page <= 9, "candidate count must be 3–9");
    check(a.at("font_size").is_number_integer(), "font size must be an integer");
    int size = a.at("font_size");
    check(size >= 10 && size <= 40, "font size must be 10–40");
    check(a.at("font").is_string() && a.at("font").get<std::string>().size() <= 256,
          "invalid font name");
    auto theme = a.at("theme").get<std::string>();
    check(theme == "system" || theme == "light" || theme == "dark", "invalid theme");
    auto layout = a.at("orientation").get<std::string>();
    check(layout == "horizontal" || layout == "vertical", "invalid orientation");
    auto schema = cfg.at("input").at("schema").get<std::string>();
    check(schema == "rime_ime_pinyin" || schema == "rime_ime_flypy" || schema == "rime_ime_ziranma",
          "unsupported input scheme");
    const auto &shortcuts = cfg.at("shortcuts");
    std::set<std::string> used;
    const std::regex binding("((Control|Alt|Shift|Super)\\+)*(Shift|Page_Up|Page_Down|Escape|Tab|F["
                             "1-9]|F1[0-2]|[A-Za-z])");
    for (auto key : {"toggle_ascii", "next_page", "previous_page", "cancel"}) {
        auto value = shortcuts.at(key).get<std::string>();
        check(value.size() <= 64 && std::regex_match(value, binding),
              std::string("invalid shortcut: ") + key);
        check(used.insert(value).second, "shortcut conflict: " + value);
        check(value != "Alt+Tab" && value != "Control+Space" && value != "Super+Space",
              "shortcut conflicts with common desktop binding: " + value);
    }
}
std::string read_file(const fs::path &file, std::size_t limit) {
    check(fs::is_regular_file(file), "not a regular file: " + file.string());
    check(fs::file_size(file) <= limit, "file exceeds size limit: " + file.string());
    std::ifstream stream(file, std::ios::binary);
    check(bool(stream), "cannot open " + file.string());
    std::string result((std::istreambuf_iterator<char>(stream)), {});
    check(!stream.bad(), "cannot read " + file.string());
    check(result.size() <= limit, "file exceeds size limit");
    return result;
}
void atomic_write(const fs::path &file, const std::string &data) {
    no_symlink(file);
#ifdef _WIN32
    fs::create_directories(file.parent_path());
    wchar_t temporary[MAX_PATH]{};
    check(GetTempFileNameW(file.parent_path().c_str(), L"rim", 0, temporary) != 0,
          "cannot create temporary file");
    HANDLE handle = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    try {
        check(handle != INVALID_HANDLE_VALUE, "cannot open temporary file");
        std::size_t done = 0;
        while (done < data.size()) {
            DWORD written{};
            check(WriteFile(handle, data.data() + done,
                            DWORD(std::min<std::size_t>(data.size() - done, 1024 * 1024)), &written,
                            nullptr) &&
                      written,
                  "cannot write temporary file");
            done += written;
        }
        check(FlushFileBuffers(handle), "cannot flush temporary file");
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        check(MoveFileExW(temporary, file.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
              "cannot atomically replace file");
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        DeleteFileW(temporary);
        throw;
    }
#else
    if (fs::create_directories(file.parent_path()))
        fs::permissions(file.parent_path(), fs::perms::owner_all, fs::perm_options::replace);
    auto pattern = (file.parent_path() / (file.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    check(fd >= 0, "cannot create temporary file for " + file.string());
    try {
        std::size_t done = 0;
        while (done < data.size()) {
            auto count = ::write(fd, data.data() + done, data.size() - done);
            if (count < 0 && errno == EINTR)
                continue;
            check(count > 0, "cannot write " + file.string());
            done += count;
        }
        check(fsync(fd) == 0, "cannot flush " + file.string());
        close(fd);
        fd = -1;
        fs::rename(name.data(), file);
        int directory = open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) {
            fsync(directory);
            close(directory);
        }
    } catch (...) {
        if (fd >= 0)
            close(fd);
        std::error_code ignored;
        fs::remove(name.data(), ignored);
        throw;
    }
#endif
}
Json load_config(const fs::path &file) {
    if (!fs::exists(file))
        return default_config();
    auto result = Json::parse(read_file(file, 1024 * 1024));
    validate_config(result);
    return result;
}
void save_config(const fs::path &file, const Json &config) {
    validate_config(config);
    atomic_write(file, config.dump(2) + "\n");
}
std::vector<Phrase> parse_phrases(const std::string &tsv) {
    check(tsv.size() <= 8 * 1024 * 1024, "phrase file exceeds 8 MiB");
    std::vector<Phrase> result;
    std::istringstream input(tsv);
    std::string line;
    std::set<std::pair<std::string, std::string>> seen;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        auto tab = line.find('\t'),
             second = tab == std::string::npos ? tab : line.find('\t', tab + 1);
        check(tab != std::string::npos && second != std::string::npos &&
                  line.find('\t', second + 1) == std::string::npos,
              "phrase rows require code, text, weight separated by tabs");
        Phrase phrase{line.substr(0, tab), line.substr(tab + 1, second - tab - 1), 0};
        check(!phrase.code.empty() && phrase.code.size() <= 64 &&
                  std::all_of(phrase.code.begin(), phrase.code.end(),
                              [](char c) { return (c >= 'a' && c <= 'z') || c == '\''; }),
              "phrase code must contain lowercase letters or apostrophe");
        check(!phrase.text.empty() && phrase.text.size() <= 4096 &&
                  std::none_of(phrase.text.begin(), phrase.text.end(),
                               [](unsigned char c) { return c < 32 || c == 127; }),
              "invalid phrase text");
        // dump performs strict UTF-8 validation.
        (void)Json(phrase.text).dump();
        auto value = line.substr(second + 1);
        auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), phrase.weight);
        check(error == std::errc{} && end == value.data() + value.size() && phrase.weight >= 0 &&
                  phrase.weight <= 1000000,
              "phrase weight must be 0–1000000");
        check(seen.emplace(phrase.code, phrase.text).second, "duplicate phrase");
        result.push_back(std::move(phrase));
        check(result.size() <= 100000, "too many phrases");
    }
    return result;
}
std::string format_phrases(const std::vector<Phrase> &phrases) {
    std::string result;
    for (const auto &p : phrases)
        result += p.code + '\t' + p.text + '\t' + std::to_string(p.weight) + '\n';
    (void)parse_phrases(result);
    return result;
}
Json backup(const Paths &paths) {
    Json result = {{"format", "r-ime-backup"},
                   {"version", 1},
                   {"config", load_config(paths.config)},
                   {"files", Json::array()}};
    std::size_t total = 0, metadata = 0;
    if (fs::exists(paths.data))
        for (auto it = fs::recursive_directory_iterator(paths.data);
             it != fs::recursive_directory_iterator(); ++it) {
            const auto &entry = *it;
            if (entry.path().filename() == "build") {
                if (entry.is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            check(!entry.is_symlink(), "backup refuses symbolic links");
            if (!entry.is_regular_file())
                continue;
            auto relative = fs::relative(entry.path(), paths.data);
            metadata += relative.generic_string().size();
            check(metadata <= 1024 * 1024 && result["files"].size() < 10000,
                  "backup file metadata exceeds limit");
            if (entry.path().filename() == "LOCK")
                continue;
            auto bytes = read_file(entry.path(), archive_limit);
            total += bytes.size();
            check(total <= archive_limit, "backup exceeds 64 MiB");
            result["files"].push_back(
                {{"path", relative.generic_string()}, {"hex", encode_hex(bytes)}});
        }
    return result;
}
void restore(const Paths &paths, const Json &document) {
    check(document.at("format") == "r-ime-backup" && document.at("version") == 1,
          "unsupported backup format");
    validate_config(document.at("config"));
    check(document.at("files").is_array() && document["files"].size() <= 10000,
          "invalid backup file list");
    std::vector<std::pair<fs::path, std::string>> entries;
    std::set<std::string> seen;
    std::size_t total = 0, metadata = 0;
    for (const auto &file : document["files"]) {
        std::string raw = file.at("path");
        fs::path relative(raw);
        metadata += raw.size();
        check(raw.size() <= 4096 && metadata <= 1024 * 1024, "backup file metadata exceeds limit");
        check(!raw.empty() && raw.find('\\') == std::string::npos &&
                  raw.find(':') == std::string::npos && raw.find('\0') == std::string::npos &&
                  !relative.is_absolute() && raw == relative.lexically_normal().generic_string(),
              "invalid backup path");
        for (const auto &component : relative)
            check(component != ".." && component != "." && !component.empty(),
                  "unsafe backup path");
        check(seen.insert(raw).second, "duplicate backup path");
        std::string encoded = file.at("hex");
        check(encoded.size() <= archive_limit * 2, "backup file too large");
        auto bytes = decode_hex(encoded);
        total += bytes.size();
        check(total <= archive_limit, "backup exceeds 64 MiB");
        entries.emplace_back(relative, std::move(bytes));
    }
    no_symlink(paths.data);
    if (fs::create_directories(paths.data.parent_path()))
        fs::permissions(paths.data.parent_path(), fs::perms::owner_all, fs::perm_options::replace);
    auto stage = paths.data.parent_path() / (".r-ime-restore-" + std::to_string(getpid()));
    auto old = paths.data.parent_path() / (".r-ime-previous-" + std::to_string(getpid()));
    check(!fs::exists(stage) && !fs::exists(old), "restore staging directory already exists");
    private_directory(stage);
    bool moved = false, installed = false;
    try {
        for (const auto &[relative, bytes] : entries)
            atomic_write(stage / relative, bytes);
        if (fs::exists(stage / "phrases.tsv"))
            (void)parse_phrases(read_file(stage / "phrases.tsv"));
        if (fs::exists(paths.data)) {
            fs::rename(paths.data, old);
            moved = true;
        }
        fs::rename(stage, paths.data);
        installed = true;
        save_config(paths.config, document.at("config"));
        if (moved) {
            std::error_code ignored;
            fs::remove_all(old, ignored);
        }
    } catch (...) {
        std::error_code ec;
        if (installed)
            fs::remove_all(paths.data, ec);
        if (moved)
            fs::rename(old, paths.data, ec);
        fs::remove_all(stage, ec);
        // Configuration is written atomically only after the data swap; failed writes
        // leave the previous config intact.
        throw;
    }
}
} // namespace rime_ime
