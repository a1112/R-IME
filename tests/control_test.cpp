#include <iostream>
#include <rime_ime/control.hpp>
#include <type_traits>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
using namespace rime_ime;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F>
static void rejects(const std::string &code, F action, bool invalidated = false) {
    try {
        action();
    } catch (const ControlError &error) {
        if (error.code() != code)
            throw std::runtime_error("expected " + code + ", received " + error.code() + ": " +
                                     error.what());
        require(error.sessions_invalidated() == invalidated,
                "error reports the connection's session invalidation state");
        return;
    }
    throw std::runtime_error("expected a ControlError with code " + code);
}
int main() {
    try {
        static_assert(!std::is_copy_constructible_v<ControlClient>);
        static_assert(!std::is_copy_assignable_v<ControlClient>);
        const auto socket = fs::temp_directory_path() /
                            ("r-ime-control-offline-" + std::to_string(getpid())) / "runtime" /
                            "engine.sock";
        ControlClient client(socket, 30, 40);
        ControlError example("example", "readable failure", true);
        require(example.code() == "example" && std::string(example.what()) == "readable failure",
                "control errors preserve code and description");
        require(example.sessions_invalidated(), "control errors preserve invalidation metadata");
        for (auto request :
             {Json::array(), Json("status"), Json(), Json::object(), Json{{"op", ""}},
              Json{{"op", 7}}, Json{{"op", "status"}, {"version", 2}},
              Json{{"op", "status"}, {"version", "1"}}, Json{{"op", "status"}, {"session", -1}},
              Json{{"op", "status"}, {"session", 0.5}}})
            rejects("invalid_request", [&] { client.call(request); });
        rejects("invalid_request", [&] { client.call({{"op", "status"}}, -1); });
        rejects("invalid_request", [&] { client.call({{"op", "status"}}, 600001); });
        rejects("invalid_request",
                [&] { client.call({{"op", "status"}, {"extra", std::string("\xff", 1)}}); });
        rejects("invalid_request", [&] {
            client.call({{"op", "status"}, {"payload", std::string(136 * 1024 * 1024 + 1, 'x')}});
        });
        rejects("invalid_request", [&] { ControlClient invalid(socket, 0, 40); });
        rejects("invalid_request", [&] { ControlClient invalid(socket, 30, 600001); });
        auto config = default_config();
        config["appearance"]["page_size"] = 99;
        rejects("invalid_request", [&] { client.configure(config); });
        rejects("invalid_request", [&] { client.configure(default_config(), "invalid row"); });
        rejects("invalid_request", [&] { client.patch_config(Json::array()); });
        rejects("invalid_request", [&] { client.patch_config({{"unknown", 1}}); });
        rejects("invalid_request",
                [&] { client.patch_config({{"appearance", {{"unknown", 1}}}}); });
        rejects("invalid_request",
                [&] { client.patch_config({{"appearance", {{"page_size", 99}}}}); });
        rejects("invalid_request", [&] { client.set_config("", Json::object()); });
        rejects("invalid_request", [&] { client.set_config("/", 1); });
        rejects("invalid_request", [&] { client.set_config("/appearance/~2bad", 1); });
        rejects("invalid_request", [&] { client.set_config("/appearance/unknown", 1); });
        rejects("invalid_request", [&] { client.set_config("/appearance/page_size", 99); });
        rejects("invalid_request", [&] { client.replace_phrases("bad code\t中文\t1\n"); });
        rejects("invalid_request", [&] { client.upsert_phrase({"bad code", "中文", 1}); });
        rejects("invalid_request", [&] { client.upsert_phrase({"ok", "中文", -1}); });
        rejects("invalid_request", [&] { client.remove_phrase("ok", ""); });
        rejects("invalid_request", [&] { client.restore(Json::array()); });
        rejects("transport_error", [&] { client.status(); }, true);
        rejects(
            "transport_error",
            [&] { client.call({{"op", "status"}, {"version", 1}, {"session", 0}}); }, true);
        rejects(
            "transport_error",
            [&] { client.patch_config({{"shortcuts", {{"toggle_ascii", "Page_Down"}}}}); }, true);
        client.disconnect();
        require(!fs::exists(socket.parent_path()),
                "offline SDK validation creates no data or engine");
        std::cout << "Control SDK: offline validation, error codes, deadlines and no implicit "
                     "startup passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
