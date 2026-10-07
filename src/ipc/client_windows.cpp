#include "../windows/platform.hpp"
#include "client.hpp"
#include "rime_ime/config.hpp"
namespace rime_ime {
Client::Client(std::filesystem::path socket) : socket_(std::move(socket)) {}
Client::~Client() {
    disconnect();
}
void Client::disconnect() {
    if (fd_ != -1)
        CloseHandle(reinterpret_cast<HANDLE>(fd_));
    fd_ = -1;
    pending_.clear();
}
nlohmann::json Client::call(nlohmann::json request, int timeout_ms) {
    auto end = GetTickCount64() + std::max(timeout_ms, 1);
    auto serial = ++sequence_;
    request["version"] = 1;
    request["id"] = serial;
    if (!request.contains("session"))
        request["session"] = 0;
    try {
        if (fd_ == -1) {
            auto name = win::pipe_name(socket_.parent_path());
            HANDLE pipe = CreateFileW(
                name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
                WaitNamedPipeW(name.c_str(), win::remaining(end)))
                pipe = CreateFileW(
                    name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                    nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
                throw std::runtime_error("cannot connect to R-IME named pipe");
            fd_ = reinterpret_cast<std::intptr_t>(pipe);
        }
        auto handle = reinterpret_cast<HANDLE>(fd_);
        auto wire = request.dump();
        DWORD size = DWORD(wire.size());
        if (size > win::maximum_frame)
            throw std::runtime_error("request too large");
        if (!win::transfer(handle, &size, 4, true, end) ||
            !win::transfer(handle, wire.data(), size, true, end) ||
            !win::transfer(handle, &size, 4, false, end))
            throw std::runtime_error("R-IME pipe timeout or disconnect");
        if (size == 0 || size > win::maximum_frame)
            throw std::runtime_error("invalid reply length");
        std::string data(size, 0);
        if (!win::transfer(handle, data.data(), size, false, end))
            throw std::runtime_error("R-IME reply timeout");
        auto reply = nlohmann::json::parse(data);
        if (reply.at("version") != 1 || reply.at("id") != serial ||
            (request["op"] != "create" && reply.at("session") != request["session"]))
            throw std::runtime_error("stale R-IME reply");
        if (!reply.value("ok", false))
            throw RpcError(reply.value("error", std::string("R-IME request failed")), reply);
        return reply;
    } catch (const RpcError &) {
        throw;
    } catch (...) {
        disconnect();
        throw;
    }
}
bool start_engine() {
    auto executable = win::install_directory() / L"r-ime-engine.exe";
    if (!std::filesystem::exists(executable))
        return false;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
} // namespace rime_ime
