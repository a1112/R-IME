#include "rime_ime/config.hpp"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
using namespace rime_ime;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "invalid input was accepted");
}
int main() {
    try {
        auto root = fs::temp_directory_path() / ("r-ime-config-test-" + std::to_string(getpid()));
        fs::create_directories(root);
        Paths paths{root / "config.json", root / "data", root / "cache", root / "runtime"};
        auto cfg = default_config();
        validate_config(cfg);
        require(cfg["appearance"]["page_size"] == 5, "default candidate count");
        require(cfg["general"].value("full_shape", true) == false, "default half-width input");
        auto bad = cfg;
        bad["appearance"]["page_size"] = 10;
        rejects([&] { validate_config(bad); });
        bad = cfg;
        bad["input"]["schema"] = "../../evil";
        rejects([&] { validate_config(bad); });
        bad = cfg;
        bad["general"]["learning"] = "true";
        rejects([&] { validate_config(bad); });
        bad = cfg;
        bad["general"]["full_shape"] = "true";
        rejects([&] { validate_config(bad); });
        auto legacy = cfg;
        legacy["general"].erase("full_shape");
        validate_config(legacy);
        atomic_write(paths.config, legacy.dump());
        require(load_config(paths.config) == cfg, "legacy configuration migrates to half-width");
        save_config(paths.config, cfg);
        require(load_config(paths.config) == cfg, "config roundtrip");
        auto full_shape = cfg;
        full_shape["general"]["full_shape"] = true;
        save_config(paths.config, full_shape);
        require(load_config(paths.config) == full_shape, "full-width preference roundtrip");
        save_config(paths.config, cfg);
        rejects([&] { save_config(paths.config, bad); });
        require(load_config(paths.config) == cfg, "invalid save damaged last config");
        auto phrases = parse_phrases("# comment\nrime\t定制输入法\t100\nnihao\t你好\t9\n");
        require(phrases.size() == 2, "phrase count");
        require(parse_phrases(format_phrases(phrases))[0].text == "定制输入法", "phrase roundtrip");
        rejects([] { parse_phrases("bad code\t词语\t1\n"); });
        rejects([] { parse_phrases("ok\t词语\t-1\n"); });
        rejects([] { parse_phrases("ok\t词语\t1extra\n"); });
        rejects([] { parse_phrases("ok\t词语\t1\nok\t词语\t2\n"); });
        fs::create_directories(paths.data / "rime");
        atomic_write(paths.data / "phrases.tsv", format_phrases(phrases));
        atomic_write(paths.data / "rime" / "binary.userdb", std::string("a\0b", 3));
        auto archive = backup(paths);
        atomic_write(paths.data / "phrases.tsv", "");
        restore(paths, archive);
        require(read_file(paths.data / "rime" / "binary.userdb") == std::string("a\0b", 3),
                "binary restore");
        auto full_shape_archive = archive;
        full_shape_archive["config"]["general"]["full_shape"] = true;
        restore(paths, full_shape_archive);
        require(load_config(paths.config) == full_shape, "restore preserves full-width preference");
        auto legacy_archive = archive;
        legacy_archive["config"]["general"].erase("full_shape");
        restore(paths, legacy_archive);
        require(load_config(paths.config) == cfg, "legacy backup migrates to half-width");
        auto evil = archive;
        evil["files"][0]["path"] = "../escape";
        rejects([&] { restore(paths, evil); });
        require(read_file(paths.data / "phrases.tsv") == format_phrases(phrases),
                "failed restore damaged data");
        evil = archive;
        evil["files"].push_back(evil["files"][0]);
        rejects([&] { restore(paths, evil); });
        atomic_write(paths.config, "{");
        rejects([&] { load_config(paths.config); });
        fs::remove_all(root);
        std::cout << "config, phrases, binary backup, traversal rejection passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
