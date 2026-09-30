#include "client.hpp"
#include "rime_ime/config.hpp"
#include <iostream>
#include <thread>
#include <windows.h>
using namespace rime_ime;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    auto root =
        fs::temp_directory_path() / (L"r-ime-中文-test-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"RIME_USER_ROOT", root.c_str());
    try {
        auto paths = Paths::from_environment();
        check(paths.data.parent_path() == root, "Windows data isolation");
        check(start_engine(), "start native worker");
        Client client(paths.runtime / "engine.sock");
        Json status;
        for (int i = 0; i < 600; ++i) {
            try {
                status = client.call({{"op", "status"}}, 1000);
                break;
            } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        check(status.value("ok", false), "native worker readiness");
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
        check(!client.call({{"op", "configure"}, {"config", changed}}, 180000)
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
        atomic_write(paths.data/"phrases.tsv","invalid phrase row\n");
        client.call({{"op", "restore"}, {"backup", saved}}, 180000);
        check(client.call({{"op", "status"}}, 1000)["config"] == changed, "backup restore config");
        auto sensitive = client.call({{"op", "create"}, {"sensitive", true}}, 1000).at("session");
        check(!client
                   .call({{"op", "key"},
                          {"session", sensitive},
                          {"keycode", int('n')},
                          {"modifiers", 0}},
                         1000)
                   .at("handled")
                   .get<bool>(),
              "sensitive bypass");
        client.call({{"op", "shutdown"}}, 1000);
        client.disconnect();
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
