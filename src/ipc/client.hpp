#pragma once
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
namespace rime_ime {
class RpcError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class Client {
  public:
    explicit Client(std::filesystem::path socket);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    nlohmann::json call(nlohmann::json request, int timeout_ms = 40);
    void disconnect();
    bool connected() const {
        return fd_ >= 0;
    }

  private:
    std::filesystem::path socket_;
#ifdef _WIN32
    std::intptr_t fd_ = -1;
#else
    int fd_ = -1;
#endif
    uint64_t sequence_ = 0;
    std::string pending_;
};
bool start_engine();
} // namespace rime_ime
