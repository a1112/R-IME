#include "client.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace rime_ime {
Client::Client(std::filesystem::path socket) : socket_(std::move(socket)) {}
Client::~Client() {
    disconnect();
}
void Client::disconnect() {
    if (fd_ >= 0)
        close(fd_);
    fd_ = -1;
    pending_.clear();
}
nlohmann::json Client::call(nlohmann::json request, int timeout_ms) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto wait = [&](short events) {
        for (;;) {
            int remaining =
                int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now())
                        .count());
            if (remaining <= 0)
                throw std::runtime_error("引擎响应超时");
            pollfd p{fd_, events, 0};
            int r = poll(&p, 1, remaining);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0 || !(p.revents & events))
                throw std::runtime_error("输入引擎连接中断");
            return;
        }
    };
    try {
        if (fd_ < 0) {
            auto path = socket_.string();
            if (path.size() >= sizeof(sockaddr_un::sun_path))
                throw std::runtime_error("引擎 socket 路径过长");
            struct stat st{};
            if (lstat(path.c_str(), &st) < 0 || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
                (st.st_mode & 0077))
                throw std::runtime_error("输入引擎未启动或 socket 权限无效");
            fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (fd_ < 0)
                throw std::runtime_error("无法创建输入引擎连接");
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            if (connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                if (errno != EINPROGRESS)
                    throw std::runtime_error("无法连接输入引擎");
                wait(POLLOUT);
                int error = 0;
                socklen_t n = sizeof(error);
                if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &n) < 0 || error)
                    throw std::runtime_error("无法连接输入引擎");
            }
            ucred peer{};
            socklen_t n = sizeof(peer);
            if (getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &peer, &n) < 0 || peer.uid != geteuid())
                throw std::runtime_error("输入引擎身份校验失败");
        }
        request["version"] = 1;
        request["id"] = ++sequence_;
        if (!request.contains("session"))
            request["session"] = 0;
        std::string bytes = request.dump() + "\n";
        size_t sent = 0;
        while (sent < bytes.size()) {
            wait(POLLOUT);
            auto n = send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (n <= 0)
                throw std::runtime_error("输入引擎发送失败");
            sent += size_t(n);
        }
        while (pending_.find('\n') == std::string::npos) {
            wait(POLLIN);
            char buffer[8192];
            auto n = recv(fd_, buffer, sizeof(buffer), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (n <= 0)
                throw std::runtime_error("输入引擎连接中断");
            pending_.append(buffer, size_t(n));
            if (pending_.size() > 136 * 1024 * 1024)
                throw std::runtime_error("输入引擎响应过大");
        }
        auto end = pending_.find('\n');
        auto reply = nlohmann::json::parse(pending_.substr(0, end));
        pending_.erase(0, end + 1);
        if (reply.value("version", 0) != 1 || reply.at("id") != request["id"] ||
            (request.value("op", std::string{}) != "create" &&
             reply.at("session") != request["session"]))
            throw std::runtime_error("输入引擎响应序列失效");
        if (!reply.value("ok", false))
            throw RpcError(reply.value("error", std::string("输入引擎操作失败")));
        return reply;
    } catch (const RpcError &) {
        throw;
    } catch (...) {
        disconnect();
        throw;
    }
}
bool start_engine() {
    // Spawn without a shell and without inheriting terminal input/output.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    char program[] = "r-ime-engine";
    char *argv[] = {program, nullptr};
    int result = posix_spawnp(&pid, program, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result == 0)
        std::thread([pid] {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    return result == 0;
}
} // namespace rime_ime
