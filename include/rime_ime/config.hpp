#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace rime_ime {
using Json = nlohmann::json;
namespace fs = std::filesystem;
inline std::string path_utf8(const fs::path &path) {
    auto text = path.u8string();
    return {text.begin(), text.end()};
}
struct Paths {
    fs::path config, data, cache, runtime;
    static Paths from_environment();
};
Json default_config();
// All invalid input and I/O failures throw std::runtime_error (JSON errors may
// derive from std::exception). Callers must display the exception, not overwrite.
void validate_config(const Json &config);
Json load_config(const fs::path &file);
void save_config(const fs::path &file, const Json &config);
void atomic_write(const fs::path &file, const std::string &data);
std::string read_file(const fs::path &file, std::size_t limit = 16 * 1024 * 1024);
struct Phrase {
    std::string code, text;
    int weight = 100;
};
std::vector<Phrase> parse_phrases(const std::string &tsv);
std::string format_phrases(const std::vector<Phrase> &phrases);
// Backup/restore must be invoked while the worker is stopped, or through its
// backup/restore operations, so LevelDB files cannot change during the snapshot.
Json backup(const Paths &paths);
void restore(const Paths &paths, const Json &document);
} // namespace rime_ime
