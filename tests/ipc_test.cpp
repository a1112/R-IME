#include "../src/ipc/client.hpp"
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
using nlohmann::json;
int main() {
    char tmp[] = "/tmp/r-ime-ipc-XXXXXX";
    auto *d = mkdtemp(tmp);
    if (!d)
        return 1;
    std::string path = std::string(d) + "/test.sock";
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    chmod(path.c_str(), 0600);
    listen(listener, 1);
    std::thread server([&] {
        int fd = accept(listener, nullptr, nullptr);
        std::string line;
        char c;
        while (read(fd, &c, 1) == 1 && c != '\n')
            line += c;
        auto q = json::parse(line);
        auto r =
            json{{"version", 1}, {"id", q["id"]}, {"session", q["session"]}, {"ok", true}}.dump() +
            "\n";
        write(fd, r.data(), r.size());
        line.clear();
        while (read(fd, &c, 1) == 1 && c != '\n')
            line += c;
        q = json::parse(line);
        r = json{{"version", 1},
                 {"id", q["id"].get<uint64_t>() - 1},
                 {"session", q["session"]},
                 {"ok", true}}
                .dump() +
            "\n";
        write(fd, r.data(), r.size());
        close(fd);
    });
    int result = 0;
    try {
        rime_ime::Client c(path);
        auto reply = c.call({{"session", 42}, {"op", "status"}}, 1000);
        if (!reply.value("ok", false))
            throw std::runtime_error("valid reply rejected");
        bool rejected = false;
        try {
            c.call({{"session", 42}, {"op", "status"}}, 1000);
        } catch (...) {
            rejected = true;
        }
        if (!rejected || c.connected())
            throw std::runtime_error("stale reply must disconnect before reuse");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    server.join();
    close(listener);
    unlink(path.c_str());
    rmdir(d);
    return result;
}
