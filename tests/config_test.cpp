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
        auto bad = cfg;
        bad["appearance"]["page_size"] = 10;
        rejects([&] { validate_config(bad); });
        bad = cfg;
        bad["input"]["schema"] = "../../evil";
        rejects([&] { validate_config(bad); });
        bad = cfg;
        bad["general"]["learning"] = "true";
        rejects([&] { validate_config(bad); });
        save_config(paths.config, cfg);
        require(load_config(paths.config) == cfg, "config roundtrip");
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
