#include "../src/windows/platform.hpp"
#include "client.hpp"
#include "rime_ime/config.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <windows.h>
using namespace rime_ime;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
Json raw_request(const Paths &paths, const Json &request) {
    const auto pipe_name = win::pipe_name(paths.runtime);
    HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(pipe_name.c_str(), 1000))
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    check(pipe != INVALID_HANDLE_VALUE, "open raw protocol test connection");
    struct ClosePipe {
        HANDLE pipe;
        ~ClosePipe() {
            CloseHandle(pipe);
        }
    } close{pipe};
    std::string bytes = request.dump();
    DWORD length = DWORD(bytes.size());
    const auto deadline = GetTickCount64() + 1000;
    check(win::transfer(pipe, &length, 4, true, deadline) &&
              win::transfer(pipe, bytes.data(), length, true, deadline) &&
              win::transfer(pipe, &length, 4, false, deadline),
          "raw protocol framing");
    check(length > 0 && length < 1024 * 1024, "raw protocol reply size");
    bytes.resize(length);
    check(win::transfer(pipe, bytes.data(), length, false, deadline), "raw protocol reply");
    return Json::parse(bytes);
}
int main(int argc, char **argv) {
    auto root =
        fs::temp_directory_path() / (L"r-ime-中文-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"RIME_USER_ROOT", root.c_str());
    try {
        auto paths = Paths::from_environment();
        check(paths.data.parent_path() == root, "Windows data isolation");
        check(start_engine(), "start native worker");
        Client client(paths.runtime / "engine.sock");
        Json status = Json::object();
        std::string readiness_error;
        for (int i = 0; i < 600; ++i) {
            try {
                status = client.call({{"op", "status"}}, 1000);
                break;
            } catch (const std::exception &error) {
                readiness_error = error.what();
                auto startup_error = paths.cache / "engine-error.txt";
                if (fs::is_regular_file(startup_error))
                    throw std::runtime_error(read_file(startup_error));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        check(status.value("ok", false), ("native worker readiness: " + readiness_error).c_str());
        auto mirror_failure = [&] {
            auto previous = client.call({{"op", "status"}}, 1000).at("config");
            auto changed = previous;
            changed["appearance"]["theme"] = "dark";
            auto mirror = paths.data / "rime" / "r-ime-effective.json";
            for (const auto &locked_file : {mirror, paths.config}) {
                HANDLE held = CreateFileW(locked_file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                check(held != INVALID_HANDLE_VALUE,
                      "hold effective configuration without delete sharing");
                bool rejected = false;
                bool invalidated = true;
                try {
                    client.call({{"op", "configure"}, {"config", changed}}, 180000);
                } catch (const RpcError &error) {
                    rejected = true;
                    invalidated = error.reply().value("sessions_invalidated", true);
                } catch (...) {
                    CloseHandle(held);
                    throw;
                }
                CloseHandle(held);
                check(rejected, "an occupied effective configuration causes an explicit failure");
                check(load_config(paths.config) == previous && load_config(mirror) == previous,
                      "failed mirror write preserves both configuration files");
                check(client.call({{"op", "status"}}, 1000)["config"] == previous,
                      "failed mirror write preserves the in-memory configuration");
                check(!invalidated, "a mirror pre-swap failure does not invalidate sessions");
            }
        };
        if (argc == 2 && std::string(argv[1]) == "--mirror-failure") {
            mirror_failure();
            client.call({{"op", "shutdown"}}, 1000);
            std::cout << "Configuration mirror failure preserves both files\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--latency") {
            // Real worker/IPC timings after readiness, in an isolated test user
            // directory. This does not measure TSF callback/UI latency or P95.
            // Synthetic compositions are cancelled without any candidate commit.
            std::cout << "Native worker/IPC latency sample; excludes TSF callbacks and UI; "
                         "not end-to-end P95.\n";
            auto timed = [&](Json request, const std::string &label) {
                auto start = std::chrono::steady_clock::now();
                auto reply = client.call(std::move(request), 1000);
                auto milliseconds = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - start)
                                        .count();
                std::cout << label << ": " << milliseconds
                          << " ms; handled=" << reply.value("handled", false)
                          << "; candidates=" << reply.value("candidates", Json::array()).size()
                          << std::endl;
                return std::pair{reply, milliseconds};
            };
            double slowest_first_key = 0;
            for (auto schema : {"rime_ime_pinyin", "rime_ime_flypy", "rime_ime_ziranma"}) {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    const auto label =
                        std::string(schema) + " session " + std::to_string(attempt + 1);
                    auto [created, setup_time] = timed({{"op", "create"}}, label + " create");
                    auto session = created.at("session");
                    if (std::string(schema) != "rime_ime_pinyin")
                        timed({{"op", "set_schema"}, {"session", session}, {"schema", schema}},
                              label + " schema");
                    std::string input = std::string(schema) == "rime_ime_flypy"     ? "nihc"
                                        : std::string(schema) == "rime_ime_ziranma" ? "nihk"
                                                                                    : "nihao";
                    bool first = true;
                    for (char key : input) {
                        auto [reply, elapsed] = timed({{"op", "key"},
                                                       {"session", session},
                                                       {"keycode", int(key)},
                                                       {"modifiers", 0}},
                                                      label + " key " + std::string(1, key));
                        check(reply.at("handled").get<bool>(),
                              "latency probe processes real pinyin");
                        if (first) {
                            slowest_first_key = std::max(slowest_first_key, elapsed);
                            first = false;
                        }
                    }
                    client.call({{"op", "reset"}, {"session", session}}, 1000);
                    client.call({{"op", "destroy"}, {"session", session}}, 1000);
                }
            }
            client.call({{"op", "shutdown"}}, 1000);
            client.disconnect();
            check(slowest_first_key < 40,
                  "a fresh session's first key exceeds the 40 ms TSF budget");
            std::cout << "Fresh session first-key latency: " << slowest_first_key
                      << " ms maximum\n";
            return 0;
        }
        auto snapshot = client.call({{"op", "configuration"}}, 1000);
        check(snapshot["config"] == status["config"] && snapshot["phrases"] == "" &&
                  snapshot["recovery_required"] == false && snapshot["diagnostic"] == "" &&
                  snapshot["version"] == 1 && snapshot["session"] == 0,
              "configuration snapshot contains the effective configuration and phrases");
        check(!raw_request(
                   paths,
                   {{"id", uint64_t{1}}, {"session", uint64_t{0}}, {"op", "configuration"}})["ok"]
                   .get<bool>(),
              "global operations require a protocol version");
        check(!raw_request(paths, {{"version", 0},
                                   {"id", uint64_t{1}},
                                   {"session", uint64_t{0}},
                                   {"op", "configure"},
                                   {"config", status["config"]}})["ok"]
                   .get<bool>(),
              "an unsupported protocol version cannot modify configuration");
        auto rejected_shutdown = raw_request(
            paths,
            {{"version", 0}, {"id", uint64_t{1}}, {"session", uint64_t{0}}, {"op", "shutdown"}});
        check(rejected_shutdown["ok"] == false &&
                  rejected_shutdown["sessions_invalidated"] == false &&
                  client.call({{"op", "status"}}, 1000)["config"] == status["config"],
              "an unsupported shutdown request cannot stop or invalidate the worker");
        mirror_failure();
        auto create = [&] {
            return client.call({{"op", "create"}}, 1000).at("session").get<uint64_t>();
        };
        auto type = [&](uint64_t s, const std::string &word) {
            Json r;
            for (char c : word)
                r = client.call(
                    {{"op", "key"}, {"session", s}, {"keycode", int(c)}, {"modifiers", 0}}, 1000);
            return r;
        };
        auto s = create();
        auto r = type(s, "nihao");
        check(r["candidates"][0]["text"] == "你好", "real pinyin candidates");
        check(client.call({{"op", "select"}, {"session", s}, {"index", 0}}, 1000)["commit"] ==
                  "你好",
              "candidate commit");
        auto call = [&](const std::string &op, Json fields = Json::object()) {
            fields["op"] = op;
            fields["session"] = s;
            return client.call(fields, 1000);
        };
        auto first = type(s, "shi");
        auto initial_preedit = first.at("preedit");
        check(first.at("candidates").size() == 5, "navigation test has five candidates");
        r = call("navigate", {{"backward", true}});
        check(r["highlighted"] == 0 && r["page"] == 0 && r["handled"] == true,
              "Up at the first candidate keeps the menu selection");
        for (int i = 1; i < 5; ++i) {
            r = call("navigate", {{"backward", false}});
            check(r["highlighted"] == i && r["page"] == 0 && r["preedit"] == initial_preedit,
                  "Down moves one candidate without changing composition");
        }
        r = call("navigate", {{"backward", false}});
        check(r["highlighted"] == 0 && r["page"] == 1, "Down crosses candidate page");
        r = call("navigate", {{"backward", true}});
        check(r["highlighted"] == 4 && r["page"] == 0, "Up returns to previous page end");
        auto chosen = first["candidates"][4]["text"];
        check(call("key", {{"keycode", 32}, {"modifiers", 0}})["commit"] == chosen,
              "Space commits the highlighted candidate");
        r = call("status");
        check(r.contains("full_shape") && !r["full_shape"].get<bool>() &&
                  r.contains("ascii_punctuation") && !r["ascii_punctuation"].get<bool>(),
              "snapshot exposes width and punctuation modes");
        type(s, "ni");
        r = call("set_ascii", {{"value", false}});
        check(r["preedit"] != "" && r["commit"] == "",
              "setting the current mode keeps composition");
        r = call("set_ascii", {{"value", true}});
        check(r["ascii"] == true && r["commit"] == "ni" && r["preedit"] == "",
              "switching to English commits the raw composition once");
        r = type(s, "a");
        check(!r["handled"].get<bool>() && r["commit"] == "", "English half-width passes through");
        r = call("set_full_shape", {{"value", true}});
        check(r["full_shape"] == true, "enable full-width mode");
        check(type(s, "a")["commit"] == "ａ", "English full-width letter");
        check(type(s, "1")["commit"] == "１", "English full-width digit");
        check(type(s, ".")["commit"] == "．", "English full-width punctuation");
        check(type(s, " ")["commit"] == "　", "English full-width space");
        r = call("key", {{"keycode", int('c')}, {"modifiers", 4}});
        check(!r["handled"].get<bool>(), "full-width mode preserves application shortcuts");
        call("set_ascii", {{"value", false}});
        check(type(s, "nihao")["candidates"][0]["text"] == "你好",
              "full-width keeps Chinese composition");
        r = type(s, "A");
        check(r["commit"] == "" && !r["preedit"].get<std::string>().empty(),
              ("a full-width uppercase key keeps the composition pending: " + r.dump()).c_str());
        r = call("key", {{"keycode", 32}, {"modifiers", 0}});
        check(r["commit"] == "你好Ａ" && r["preedit"] == "",
              ("Space confirms the composition with a full-width suffix: " + r.dump()).c_str());
        type(s, "nihao");
        check(call("key", {{"keycode", 0xff0d}, {"modifiers", 0}})["commit"] == "nihao",
              "Enter preserves the original raw code even in full-width mode");
        check(type(s, ".")["commit"] == "。", "Chinese punctuation is independent of width");
        check(type(s, "1")["commit"] == "１", "Chinese full-width digit");
        r = call("set_ascii_punctuation", {{"value", true}});
        check(r["ascii_punctuation"] == true, "English punctuation mode snapshot");
        check(type(s, ".")["commit"] == "．", "full-width English punctuation in Chinese mode");
        call("toggle_full_shape");
        r = type(s, ".");
        check(!r["handled"].get<bool>() && r["commit"] == "",
              "half-width English punctuation passes through");
        call("toggle_ascii_punctuation");
        check(type(s, ".")["commit"] == "。", "restore Chinese punctuation mode");
        auto other = create();
        type(s, "ni");
        check(client.call({{"op", "status"}, {"session", other}}, 1000)["preedit"] == "",
              "session isolation");
        Client stranger(paths.runtime / "engine.sock");
        bool denied = false;
        try {
            stranger.call({{"op", "status"}, {"session", s}}, 1000);
        } catch (const RpcError &) {
            denied = true;
        }
        check(denied, "session ownership");
        auto config = status.at("config");
        auto changed = config;
        changed["appearance"]["theme"] = "dark";
        auto before = client.call({{"op", "configuration"}}, 1000);
        auto before_preedit = client.call({{"op", "status"}, {"session", s}}, 1000)["preedit"];
        auto before_config_file = read_file(paths.config);
        const auto stale_marker = paths.data.parent_path() / ".r-ime-data-stage" / "stale-marker";
        atomic_write(stale_marker, "must remain untouched\n");
        auto reject_stale = [&](Json fields, const char *expected_error) {
            fields["op"] = "configure";
            fields["config"] = changed;
            fields["version"] = 1;
            fields["id"] = uint64_t{1};
            fields["session"] = uint64_t{0};
            auto rejected = raw_request(paths, fields);
            check(rejected["ok"] == false && rejected["error"] == expected_error &&
                      rejected["error_code"] == "conflict" &&
                      rejected["sessions_invalidated"] == false,
                  "stale writes return a conflict without session invalidation");
            check(read_file(stale_marker) == "must remain untouched\n" &&
                      read_file(paths.config) == before_config_file &&
                      client.call({{"op", "status"}, {"session", s}}, 1000)["preedit"] ==
                          before_preedit &&
                      client.call({{"op", "configuration"}}, 1000)["config"] == before["config"],
                  "a stale write preserves files, deployment staging and composition");
        };
        reject_stale({{"expected_config", changed}}, "stale configuration snapshot");
        reject_stale({{"expected_phrases", "stale\t旧短语\t100\n"}}, "stale phrase snapshot");
        fs::remove(stale_marker);
        fs::remove(stale_marker.parent_path());
        check(!client
                   .call({{"op", "configure"},
                          {"config", changed},
                          {"expected_config", before["config"]},
                          {"expected_phrases", before["phrases"]}},
                         180000)
                   .at("sessions_invalidated")
                   .get<bool>(),
              "appearance preserves sessions");
        check(!client.call({{"op", "status"}, {"session", s}}, 1000)["preedit"]
                   .get<std::string>()
                   .empty(),
              "appearance preserves composition");
        client.call({{"op", "reset"}, {"session", s}}, 1000);
        check(type(s, "hanyu")["candidates"][0]["text"] == "汉语",
              "Windows OpenCC simplified output");
        check(client.call({{"op", "toggle_simplified"}, {"session", s}},
                          1000)["candidates"][0]["text"] == "漢語",
              "Windows OpenCC traditional output");
        client.call({{"op", "reset"}, {"session", s}}, 1000);
        client.call({{"op", "toggle_simplified"}, {"session", s}}, 1000);
        for (auto schema : {"rime_ime_flypy", "rime_ime_ziranma"}) {
            client.call({{"op", "set_schema"}, {"session", s}, {"schema", schema}}, 1000);
            check(type(s, std::string(schema) == "rime_ime_flypy"
                              ? "nihc"
                              : "nihk")["candidates"][0]["text"] == "你好",
                  "double pinyin");
            client.call({{"op", "reset"}, {"session", s}}, 1000);
        }
        auto saved = client.call({{"op", "backup"}}, 180000).at("backup");
        auto legacy_config = changed;
        legacy_config["general"].erase("full_shape");
        auto legacy_configured =
            client.call({{"op", "configure"}, {"config", legacy_config}}, 180000);
        check(legacy_configured["config"] == changed &&
                  legacy_configured["sessions_invalidated"] == false &&
                  client.call({{"op", "status"}}, 1000)["config"] == changed,
              "legacy configure normalizes in-memory config immediately");
        client.call({{"op", "configure"},
                     {"config", config},
                     {"phrases", "ceshi\tWindows 定制短语\t100\n"}},
                    180000);
        s = create();
        check(type(s, "ceshi")["candidates"][0]["text"] == "Windows 定制短语",
              "custom phrase deployment");
        auto invalid = config;
        invalid["appearance"]["page_size"] = 99;
        denied = false;
        try {
            client.call({{"op", "configure"}, {"config", invalid}}, 180000);
        } catch (const RpcError &) {
            denied = true;
        }
        check(denied, "reject invalid deployment");
        atomic_write(paths.data / "phrases.tsv", "invalid phrase row\n");
        check(client.call({{"op", "configuration"}}, 1000)["phrases"] ==
                  "ceshi\tWindows 定制短语\t100\n",
              "configuration snapshots use deployed phrases instead of externally damaged data");
        auto legacy_backup = saved;
        legacy_backup["config"]["general"].erase("full_shape");
        auto restored = client.call({{"op", "restore"}, {"backup", legacy_backup}}, 180000);
        check(restored["config"] == changed,
              "legacy restore normalizes response config immediately");
        check(client.call({{"op", "status"}}, 1000)["config"] == changed, "backup restore config");
        auto sensitive = client.call({{"op", "create"}, {"sensitive", true}}, 1000).at("session");
        client.call({{"op", "set_ascii"}, {"session", sensitive}, {"value", true}}, 1000);
        client.call({{"op", "set_full_shape"}, {"session", sensitive}, {"value", true}}, 1000);
        check(!client
                   .call({{"op", "key"},
                          {"session", sensitive},
                          {"keycode", int('n')},
                          {"modifiers", 0}},
                         1000)
                   .at("handled")
                   .get<bool>(),
              "sensitive bypass");
        auto lost_session = create();
        type(lost_session, "ni");
        HANDLE locked_phrase =
            CreateFileW((paths.data / "rime" / "r-ime-phrases.tsv").c_str(), GENERIC_READ,
                        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked_phrase != INVALID_HANDLE_VALUE, "hold a backup file without read sharing");
        bool failed_backup = false;
        bool backup_invalidated = false;
        try {
            client.call({{"op", "backup"}}, 180000);
        } catch (const RpcError &error) {
            failed_backup = true;
            backup_invalidated = error.reply().value("sessions_invalidated", false);
        } catch (...) {
            CloseHandle(locked_phrase);
            throw;
        }
        CloseHandle(locked_phrase);
        check(failed_backup, "the real sharing violation rejects the backup");
        check(backup_invalidated,
              "a backup failure after engine replacement reports session invalidation");
        bool expired = false;
        try {
            client.call({{"op", "status"}, {"session", lost_session}}, 1000);
        } catch (const RpcError &) {
            expired = true;
        }
        check(expired, "the replaced engine expires previous input sessions");
        auto recovered_session = create();
        check(type(recovered_session, "nihao")["candidates"][0]["text"] == "你好",
              "failed backup still permits new input sessions");
        client.call({{"op", "shutdown"}}, 1000);
        client.disconnect();
        auto recovery_root = root / "recovery-probe";
        SetEnvironmentVariableW(L"RIME_USER_ROOT", recovery_root.c_str());
        auto recovery_paths = Paths::from_environment();
        atomic_write(recovery_paths.config, "{damaged");
        const std::string recovered_phrases = "ceshi\t旧的有效短语\t100\n";
        atomic_write(recovery_paths.data / "rime" / "r-ime-phrases.tsv", recovered_phrases);
        check(start_engine(), "start an isolated recovery worker");
        Client recovery_client(recovery_paths.runtime / "engine.sock");
        Json recovery_status = Json::object();
        for (int i = 0; i < 100; ++i) {
            try {
                recovery_status = recovery_client.call({{"op", "status"}}, 1000);
                break;
            } catch (const std::exception &) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        check(recovery_status.value("ok", false), "recovery worker readiness");
        auto recovery_snapshot = recovery_client.call({{"op", "configuration"}}, 1000);
        check(
            recovery_snapshot["recovery_required"] == true &&
                !recovery_snapshot["diagnostic"].get<std::string>().empty() &&
                recovery_snapshot["config"] == default_config() &&
                recovery_snapshot["phrases"] == recovered_phrases &&
                read_file(recovery_paths.config) == "{damaged",
            "configuration snapshots are readable during recovery without modifying damaged files");
        recovery_client.call({{"op", "shutdown"}}, 1000);
        recovery_client.disconnect();
        std::cout << "Windows native worker: pinyin, double pinyin, ownership, appearance, "
                     "phrases, backup/restore, validation, sensitive input passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        try {
            Client cleanup(Paths::from_environment().runtime / "engine.sock");
            cleanup.call({{"op", "shutdown"}}, 1000);
        } catch (...) {
        }
        return 1;
    }
}
