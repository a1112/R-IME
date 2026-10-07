#include "rime_ime/control.hpp"
#include <charconv>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include "../windows/platform.hpp"
#endif

using namespace rime_ime;
namespace {
constexpr std::size_t maximum_line = 136 * 1024 * 1024;
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require_arguments(bool condition, const char *message) {
    if (!condition)
        throw UsageError(message);
}
int integer(const std::string &text, int minimum, int maximum) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum ||
        value > maximum)
        throw UsageError("invalid integer argument: " + text);
    return value;
}
fs::path path(const std::string &text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}
Json error_reply(const std::string &code, const std::string &message, bool invalidated = false) {
    return {{"version", 1},
            {"ok", false},
            {"error", {{"code", code}, {"message", message}}},
            {"sessions_invalidated", invalidated}};
}
void output(const Json &reply) {
    // One complete UTF-8 JSON line, even when an OS error contains invalid bytes.
    std::cout << reply.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
}
Json parse(const std::string &data) {
    try {
        return Json::parse(data);
    } catch (const std::exception &error) {
        throw ControlError("invalid_request", error.what());
    }
}
std::string file_text(const std::string &file, std::size_t limit) {
    try {
        auto data = read_file(path(file), limit);
        if (data.starts_with("\xef\xbb\xbf"))
            data.erase(0, 3);
        return data;
    } catch (const std::exception &error) {
        throw ControlError("io_error", error.what());
    }
}
void save(const std::string &file, const std::string &data) {
    try {
        atomic_write(path(file), data);
    } catch (const std::exception &error) {
        throw ControlError("io_error", error.what());
    }
}
int rpc(ControlClient &client) {
    bool failed = false;
    // Bound the allocation before a hostile/mistaken line reaches JSON parsing.
    std::string line;
    bool oversized = false;
    auto process = [&] {
        try {
            if (oversized)
                throw ControlError("invalid_request", "JSON line exceeds 136 MiB");
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            output(client.call(parse(line)));
        } catch (const ControlError &error) {
            output(error_reply(error.code(), error.what(), error.sessions_invalidated()));
            failed = true;
        } catch (const std::exception &error) {
            output(error_reply("invalid_request", error.what()));
            failed = true;
        }
        line.clear();
        oversized = false;
    };
    for (char ch; std::cin.get(ch);) {
        if (ch == '\n')
            process();
        else if (!oversized) {
            if (line.size() == maximum_line) {
                oversized = true;
                line.clear();
            } else
                line.push_back(ch);
        }
    }
    if (!line.empty() || oversized)
        process();
    if (!std::cin.eof()) {
        output(error_reply("io_error", "cannot read standard input"));
        failed = true;
    }
    return failed ? 1 : 0;
}
void help() {
    std::cout << R"(R-IME local control API (UTF-8 JSON output)
Usage: r-ime-ctl [--socket PATH] [--timeout MS] COMMAND
  status                         Query engine status
  config get                     Read effective configuration
  config set POINTER JSON        Set one existing field, e.g. /general/ascii true
  config import FILE             Apply a complete config JSON file
  schema pinyin|flypy|ziranma     Set the persistent default input scheme
  mode chinese|ascii             Set the persistent default input mode
  phrases list                   List custom phrases as JSON
  phrases set CODE TEXT WEIGHT   Add/update a code+text pair (weight 0..1000000)
  phrases remove CODE TEXT       Remove a code+text pair
  phrases import FILE            Replace custom phrases from UTF-8 TSV
  phrases export FILE            Export custom phrases to UTF-8 TSV
  backup FILE                    Export config and user dictionary archive
  restore FILE                   Restore an R-IME backup archive
  rpc                            Persistent JSON-lines engine bridge via stdin/stdout
  --help                         Show this help
Timeouts: 2000 ms for reads/keys, 180000 ms for deployment/backup/restore.
--timeout overrides both defaults (1..600000 ms); operations are never replayed.
Mode/scheme commands update defaults, not another application's system IME.
Deployment/backup/restore can invalidate sessions. Recreate after invalidation.
Exit codes: 0 success; 1 operation failed; 2 invalid command-line arguments.
)";
}
int run(const std::vector<std::string> &arguments) {
    try {
        fs::path socket = Paths::from_environment().runtime / "engine.sock";
        int timeout = 2000, deployment_timeout = 180000;
        std::size_t position = 0;
        while (position < arguments.size() && arguments[position].starts_with("--")) {
            const auto option = arguments[position++];
            if (option == "--help") {
                require_arguments(arguments.size() == 1, "--help takes no other arguments");
                help();
                return 0;
            }
            require_arguments(position < arguments.size(), "missing option value");
            if (option == "--socket")
                socket = path(arguments[position++]);
            else if (option == "--timeout")
                deployment_timeout = timeout = integer(arguments[position++], 1, 600000);
            else
                throw UsageError("unknown option: " + option);
        }
        require_arguments(position < arguments.size(), "missing command; use --help");
        std::vector<std::string> args(arguments.begin() + position, arguments.end());
        auto count = [&](std::size_t expected) {
            require_arguments(args.size() == expected, "wrong argument count; use --help");
        };
        ControlClient client(socket, timeout, deployment_timeout);
        const auto &command = args[0];
        Json result;
        if (command == "status") {
            count(1);
            result = client.status();
        } else if (command == "config") {
            require_arguments(args.size() >= 2, "config requires get, set or import");
            if (args[1] == "get") {
                count(2);
                result = client.configuration();
            } else if (args[1] == "set") {
                count(4);
                result = client.set_config(args[2], parse(args[3]));
            } else if (args[1] == "import") {
                count(3);
                result = client.configure(parse(file_text(args[2], 1024 * 1024)));
            } else
                throw UsageError("unknown config command");
        } else if (command == "schema") {
            count(2);
            require_arguments(args[1] == "pinyin" || args[1] == "flypy" || args[1] == "ziranma",
                              "schema must be pinyin, flypy or ziranma");
            result = client.patch_config({{"input", {{"schema", "rime_ime_" + args[1]}}}});
        } else if (command == "mode") {
            count(2);
            require_arguments(args[1] == "chinese" || args[1] == "ascii",
                              "mode must be chinese or ascii");
            result = client.patch_config({{"general", {{"ascii", args[1] == "ascii"}}}});
        } else if (command == "phrases") {
            require_arguments(args.size() >= 2, "phrases requires a subcommand");
            if (args[1] == "list") {
                count(2);
                Json rows = Json::array();
                for (const auto &phrase : client.phrases())
                    rows.push_back(
                        {{"code", phrase.code}, {"text", phrase.text}, {"weight", phrase.weight}});
                result = {{"version", 1}, {"ok", true}, {"phrases", rows}};
            } else if (args[1] == "set") {
                count(5);
                result = client.upsert_phrase({args[2], args[3], integer(args[4], 0, 1000000)});
            } else if (args[1] == "remove") {
                count(4);
                result = client.remove_phrase(args[2], args[3]);
            } else if (args[1] == "import") {
                count(3);
                result = client.replace_phrases(file_text(args[2], 8 * 1024 * 1024));
            } else if (args[1] == "export") {
                count(3);
                save(args[2], format_phrases(client.phrases()));
                result = {{"version", 1}, {"ok", true}, {"file", args[2]}};
            } else
                throw UsageError("unknown phrases command");
        } else if (command == "backup") {
            count(2);
            const auto archive = client.backup();
            try {
                save(args[1], archive.dump() + "\n");
            } catch (const ControlError &error) {
                // The worker already flushed and reopened dictionaries before
                // the caller's export location failed.
                throw ControlError(error.code(), error.what(), true);
            }
            result = {
                {"version", 1}, {"ok", true}, {"file", args[1]}, {"sessions_invalidated", true}};
        } else if (command == "restore") {
            count(2);
            result = client.restore(parse(file_text(args[1], maximum_line)));
        } else if (command == "rpc") {
            count(1);
            return rpc(client);
        } else
            throw UsageError("unknown command: " + command);
        output(result);
        return 0;
    } catch (const UsageError &error) {
        output(error_reply("invalid_arguments", error.what()));
        return 2;
    } catch (const ControlError &error) {
        output(error_reply(error.code(), error.what(), error.sessions_invalidated()));
        return 1;
    } catch (const std::exception &error) {
        output(error_reply("io_error", error.what()));
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i)
        arguments.push_back(win::utf8(argv[i]));
    return run(arguments);
}
#else
int main(int argc, char **argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
