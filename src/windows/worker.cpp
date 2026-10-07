#include "../ipc/client.hpp"
#include "platform.hpp"
#include "rime_ime/engine.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
using namespace rime_ime;
namespace {
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
std::wstring quoted(const fs::path &path) {
    return L"\"" + path.wstring() + L"\"";
}
void copy_tree(const fs::path &from, const fs::path &to, bool databases_only = false) {
    fs::create_directories(to);
    if (!fs::exists(from))
        return;
    for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator();
         ++it) {
        require(!it->is_symlink(), "user data must not contain symlinks");
        auto relative = fs::relative(it->path(), from);
        // Windows LevelDB opens live logs without read sharing. Deployment only
        // needs dictionaries; copy user databases after closing the live engine.
        if (!databases_only && it->is_directory() &&
            it->path().filename().string().ends_with(".userdb")) {
            it.disable_recursion_pending();
            continue;
        }
        if (relative.begin()->string() == "build") {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (databases_only) {
            auto first = relative.begin()->string();
            if (first.find(".userdb") == std::string::npos) {
                if (it->is_directory())
                    it.disable_recursion_pending();
                continue;
            }
        }
        auto destination = to / relative;
        if (it->is_directory())
            fs::create_directories(destination);
        else if (it->is_regular_file() && it->path().filename() != L"LOCK") {
            fs::create_directories(destination.parent_path());
            fs::copy_file(it->path(), destination, fs::copy_options::overwrite_existing);
        }
    }
}
struct Worker {
    Paths paths = Paths::from_environment();
    fs::path base = win::install_directory(), shared = base / L"share" / L"rime-data",
             overlay = base / L"share" / L"r-ime" / L"rime";
    Json config = default_config();
    std::unique_ptr<Engine> engine;
    std::string startup_error;
    std::mutex mutex;
    bool deploying = false;
    uint64_t session_generation = 0;
    fs::path user() const {
        return paths.data / L"rime";
    }
    void start() {
        engine = std::make_unique<Engine>(shared, user(), config);
        ++session_generation;
    }
    Worker() {
        fs::create_directories(paths.runtime);
        fs::create_directories(paths.cache);
        auto old = paths.data.parent_path() / L".r-ime-data-old";
        if (fs::exists(old)) {
            if (!fs::exists(paths.data / L".transaction-committed")) {
                if (fs::exists(paths.data))
                    fs::rename(paths.data,
                               paths.data.parent_path() /
                                   (L"data.interrupted-" + std::to_wstring(GetCurrentProcessId())));
                fs::rename(old, paths.data);
            } else
                fs::remove_all(old);
        }
        try {
            if (fs::exists(user() / L"r-ime-effective.json")) {
                config = load_config(user() / L"r-ime-effective.json");
                save_config(paths.config, config);
                if (fs::exists(user() / L"r-ime-phrases.tsv"))
                    atomic_write(paths.data / L"phrases.tsv",
                                 read_file(user() / L"r-ime-phrases.tsv"));
            } else
                config = load_config(paths.config);
        } catch (const std::exception &error) {
            startup_error = error.what();
            return;
        }
        if (!fs::exists(user() / L"build" / L"rime_ime_pinyin.schema.yaml")) {
            prepare_schemas(overlay, user(), config);
            deploy_child(user());
            save_config(user() / L"r-ime-effective.json", config);
            save_config(paths.config, config);
            atomic_write(user() / L"r-ime-phrases.tsv", fs::exists(paths.data / L"phrases.tsv")
                                                            ? read_file(paths.data / L"phrases.tsv")
                                                            : "");
        }
        start();
    }
    void deploy_child(const fs::path &target) {
        auto executable = base / L"r-ime-engine.exe";
        auto command = quoted(executable) + L" --deploy " + quoted(shared) + L" " + quoted(target);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, base.c_str(), &startup, &process),
                "cannot start dictionary deployment");
        CloseHandle(process.hThread);
        DWORD result = WaitForSingleObject(process.hProcess, 180000), code = 1;
        if (result != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        require(result == WAIT_OBJECT_0 && code == 0,
                "Rime deployment failed; inspect " +
                    win::utf8((target / L"deployment-error.txt").wstring()));
    }
    Json response(const Json &request) {
        auto number = [&](const char *key) -> uint64_t {
            auto it = request.find(key);
            return it != request.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
        };
        return {{"version", 1}, {"id", number("id")}, {"session", number("session")}, {"ok", true}};
    }
    std::string deployed_phrases() const {
        const auto file = user() / L"r-ime-phrases.tsv";
        auto text = fs::exists(file) ? read_file(file, 8 * 1024 * 1024) : std::string{};
        (void)parse_phrases(text);
        return text;
    }
    void rename_data_directory(const fs::path &source, const fs::path &destination) {
        const auto root = fs::absolute(paths.data.parent_path()).lexically_normal();
        require(fs::absolute(source).lexically_normal().parent_path() == root &&
                    fs::absolute(destination).lexically_normal().parent_path() == root,
                "data swap must remain in the private user directory");
        const auto deadline = GetTickCount64() + 1000;
        for (;;) {
            // No REPLACE_EXISTING: a concurrent unexpected target must never
            // be overwritten during either the swap or its rollback.
            if (MoveFileExW(source.c_str(), destination.c_str(), 0))
                return;
            const std::error_code error(int(GetLastError()), std::system_category());
            const bool transient =
                error.category() == std::system_category() &&
                (error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION ||
                 error.value() == ERROR_LOCK_VIOLATION);
            // An existing target is a different conflict, not a temporary lock.
            // Do not replace it or retry indefinitely under permanent permissions.
            std::error_code target_error;
            bool target_exists = fs::exists(destination, target_error);
            auto now = GetTickCount64();
            if (!transient || target_exists || now >= deadline)
                throw fs::filesystem_error("data directory rename", source, destination, error);
            Sleep(DWORD(std::min<ULONGLONG>(25, deadline - now)));
        }
    }
    void swap_stage(const fs::path &stage, const Json &next, const std::string &phrases,
                    bool keep_latest) {
        const auto old = paths.data.parent_path() / L".r-ime-data-old";
        require(!fs::exists(old), "unfinished data swap requires restart");
        auto previous = config;
        engine.reset();
        bool moved = false, installed = false;
        try {
            if (keep_latest)
                copy_tree(user(), stage / L"rime", true);
            if (fs::exists(paths.data)) {
                rename_data_directory(paths.data, old);
                moved = true;
            }
            rename_data_directory(stage, paths.data);
            installed = true;
            config = next;
            start();
            save_config(paths.config, next);
            atomic_write(paths.data / L".transaction-committed", "1\n");
            if (moved) {
                std::error_code ignored;
                fs::remove_all(old, ignored);
            }
        } catch (...) {
            engine.reset();
            std::error_code ignored;
            if (installed)
                fs::remove_all(paths.data, ignored);
            if (moved)
                rename_data_directory(old, paths.data);
            config = previous;
            save_config(paths.config, previous);
            if (startup_error.empty())
                start();
            throw;
        }
    }
    Json dispatch(const Json &request, std::unique_lock<std::mutex> &lock) {
        const auto before = session_generation;
        const bool had_engine = bool(engine);
        try {
            return dispatch_impl(request, lock);
        } catch (const std::exception &error) {
            auto reply = response(request);
            reply["ok"] = false;
            reply["error"] = error.what();
            reply["sessions_invalidated"] = session_generation != before || (had_engine && !engine);
            return reply;
        }
    }
    Json dispatch_impl(const Json &request, std::unique_lock<std::mutex> &lock) {
        auto reply = response(request);
        require(request.contains("version") && request.at("version").is_number_integer() &&
                    request.at("version") == 1,
                "unsupported protocol version");
        auto op = request.at("op").get<std::string>();
        if (op == "configuration") {
            require(request.value("session", uint64_t{}) == 0,
                    "configuration snapshots require session zero");
            reply["config"] = config;
            reply["phrases"] = deployed_phrases();
            reply["recovery_required"] = !startup_error.empty();
            reply["diagnostic"] = startup_error;
            return reply;
        }
        if (op == "status" && request.value("session", uint64_t{}) == 0) {
            reply["config"] = config;
            reply["recovery_required"] = !startup_error.empty();
            reply["diagnostic"] = startup_error;
            return reply;
        }
        if (op == "shutdown")
            return reply;
        require(startup_error.empty() || op == "restore" ||
                    (op == "configure" && request.value("repair_config", false)),
                "configuration requires explicit repair or backup restore: " + startup_error);
        if (op == "configure" || op == "restore") {
            require(!deploying, "another configuration deployment is running");
            if (op == "configure") {
                std::string conflict;
                if (request.contains("expected_config") && request.at("expected_config") != config)
                    conflict = "stale configuration snapshot";
                else if (request.contains("expected_phrases") &&
                         request.at("expected_phrases").get<std::string>() != deployed_phrases())
                    conflict = "stale phrase snapshot";
                if (!conflict.empty()) {
                    reply["ok"] = false;
                    reply["error"] = conflict;
                    reply["error_code"] = "conflict";
                    reply["sessions_invalidated"] = false;
                    return reply;
                }
            }
            deploying = true;
            struct Guard {
                bool &value;
                ~Guard() {
                    value = false;
                }
            } guard{deploying};
            auto stage = paths.data.parent_path() / L".r-ime-data-stage";
            if (fs::exists(stage))
                fs::remove_all(stage);
            auto next = normalize_config(op == "configure" ? request.at("config")
                                                           : request.at("backup").at("config"));
            std::string phrases;
            if (op == "configure") {
                phrases = request.contains("phrases")
                              ? request.at("phrases").get<std::string>()
                              : (fs::exists(paths.data / L"phrases.tsv")
                                     ? read_file(paths.data / L"phrases.tsv")
                                     : "");
                parse_phrases(phrases);
            }
            auto deployed = op == "configure" ? deployed_phrases() : std::string{};
            if (engine && op == "configure" && next["input"] == config["input"] &&
                next["general"] == config["general"] &&
                next["appearance"]["page_size"] == config["appearance"]["page_size"] &&
                phrases == deployed) {
                save_config(user() / L"r-ime-effective.json", next);
                try {
                    save_config(paths.config, next);
                } catch (...) {
                    save_config(user() / L"r-ime-effective.json", config);
                    throw;
                }
                engine->update_config(next);
                config = next;
                reply["config"] = config;
                reply["sessions_invalidated"] = false;
                return reply;
            }
            try {
                if (op == "restore") {
                    Paths temporary{stage.parent_path() / L".r-ime-restore-config.json", stage,
                                    paths.cache, paths.runtime};
                    rime_ime::restore(temporary, request.at("backup"));
                    fs::remove(temporary.config);
                    phrases =
                        fs::exists(stage / L"phrases.tsv") ? read_file(stage / L"phrases.tsv") : "";
                } else
                    copy_tree(paths.data, stage);
                fs::remove(stage / L".transaction-committed");
                atomic_write(stage / L"phrases.tsv", phrases);
                prepare_schemas(overlay, stage / L"rime", next);
                save_config(stage / L"rime" / L"r-ime-effective.json", next);
                atomic_write(stage / L"rime" / L"r-ime-phrases.tsv", phrases);
                lock.unlock();
                try {
                    deploy_child(stage / L"rime");
                } catch (...) {
                    lock.lock();
                    throw;
                }
                lock.lock();
                if (!startup_error.empty() && fs::exists(paths.config))
                    atomic_write(fs::path(paths.config.wstring() + L".invalid"),
                                 read_file(paths.config));
                swap_stage(stage, next, phrases, op == "configure");
                startup_error.clear();
                reply["config"] = config;
                reply["sessions_invalidated"] = true;
                return reply;
            } catch (...) {
                std::error_code ignored;
                fs::remove_all(stage, ignored);
                throw;
            }
        }
        if (op == "backup") {
            require(!deploying, "dictionary deployment is running");
            engine.reset();
            try {
                reply["backup"] = rime_ime::backup(paths);
            } catch (...) {
                start();
                throw;
            }
            start();
            reply["sessions_invalidated"] = true;
            return reply;
        }
        require(bool(engine), "input engine unavailable");
        return engine->dispatch(request);
    }
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 2 &&
            (std::wstring(argv[1]) == L"--shutdown" || std::wstring(argv[1]) == L"--status")) {
            Client client(Paths::from_environment().runtime / L"engine.sock");
            try {
                auto reply = client.call(
                    {{"op", std::wstring(argv[1]) == L"--shutdown" ? "shutdown" : "status"}}, 2000);
                std::cout << reply.dump() << '\n';
                return 0;
            } catch (...) {
                return std::wstring(argv[1]) == L"--shutdown" ? 0 : 1;
            }
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--deploy") {
            try {
                deploy(argv[2], argv[3]);
                return 0;
            } catch (const std::exception &e) {
                atomic_write(fs::path(argv[3]) / L"deployment-error.txt", e.what());
                return 1;
            }
        }
        auto paths = Paths::from_environment();
        auto pipe_name = win::pipe_name(paths.runtime);
        auto mutex_name = L"Local\\" + pipe_name.substr(pipe_name.find_last_of(L'\\') + 1);
        HANDLE singleton = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
        if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (singleton)
                CloseHandle(singleton);
            return 0;
        }
        Worker worker;
        HANDLE stopping = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::wstring sddl = L"D:P(A;;GA;;;" + win::sid() + L")S:(ML;;NW;;;LW)";
        PSECURITY_DESCRIPTOR security{};
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                     &security, nullptr),
                "cannot create private pipe ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), security, FALSE};
        struct Task {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> finished;
        };
        std::vector<Task> threads;
        std::mutex handles_mutex;
        std::set<HANDLE> handles;
        bool first = true;
        while (WaitForSingleObject(stopping, 0) == WAIT_TIMEOUT) {
            for (auto it = threads.begin(); it != threads.end();) {
                if (it->finished->load()) {
                    it->thread.join();
                    it = threads.erase(it);
                } else
                    ++it;
            }
            HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
                                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                               (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                               PIPE_REJECT_REMOTE_CLIENTS,
                                           32, 65536, 65536, 0, &attributes);
            first = false;
            if (pipe == INVALID_HANDLE_VALUE) {
                require(GetLastError() == ERROR_PIPE_BUSY, "cannot create private named pipe");
                Sleep(100);
                continue;
            }
            OVERLAPPED overlap{};
            overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL connected = ConnectNamedPipe(pipe, &overlap);
            auto error = GetLastError();
            if (!connected && error == ERROR_IO_PENDING) {
                HANDLE events[]{stopping, overlap.hEvent};
                auto ready = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (ready == WAIT_OBJECT_0) {
                    CancelIoEx(pipe, &overlap);
                    DWORD transferred{};
                    GetOverlappedResult(pipe, &overlap, &transferred, TRUE);
                    CloseHandle(overlap.hEvent);
                    CloseHandle(pipe);
                    break;
                }
                connected = TRUE;
            } else if (!connected && error == ERROR_PIPE_CONNECTED)
                connected = TRUE;
            CloseHandle(overlap.hEvent);
            if (!connected) {
                CloseHandle(pipe);
                continue;
            }
            {
                std::lock_guard guard(handles_mutex);
                handles.insert(pipe);
            }
            auto finished = std::make_shared<std::atomic<bool>>(false);
            std::thread thread([&, pipe, finished] {
                std::set<uint64_t> sessions;
                uint64_t sequence = 0;
                while (WaitForSingleObject(stopping, 0) == WAIT_TIMEOUT) {
                    DWORD length{};
                    if (!win::transfer(pipe, &length, 4, false, GetTickCount64() + 300000) ||
                        length == 0 || length > win::maximum_frame)
                        break;
                    std::string wire(length, 0);
                    if (!win::transfer(pipe, wire.data(), length, false, GetTickCount64() + 30000))
                        break;
                    Json request, reply;
                    bool shutdown = false;
                    try {
                        request = Json::parse(wire);
                        reply = worker.response(request);
                        require(request.contains("version") &&
                                    request.at("version").is_number_integer() &&
                                    request.at("version") == 1,
                                "unsupported protocol version");
                        auto serial = request.at("id").get<uint64_t>();
                        require(serial > sequence, "stale request sequence");
                        sequence = serial;
                        auto session = request.value("session", uint64_t{});
                        require(!session || sessions.count(session),
                                "session belongs to another connection");
                        std::unique_lock lock(worker.mutex);
                        reply = worker.dispatch(request, lock);
                        if (reply.value("ok", false) && request["op"] == "create")
                            sessions.insert(reply.at("session").get<uint64_t>());
                        if (reply.value("ok", false) && request["op"] == "destroy")
                            sessions.erase(session);
                        shutdown = reply.value("ok", false) && request["op"] == "shutdown";
                    } catch (const std::exception &e) {
                        reply = worker.response(request.is_object() ? request : Json::object());
                        reply["ok"] = false;
                        reply["error"] = e.what();
                        reply["sessions_invalidated"] = false;
                    }
                    wire = reply.dump();
                    length = DWORD(wire.size());
                    if (length > win::maximum_frame ||
                        !win::transfer(pipe, &length, 4, true, GetTickCount64() + 30000) ||
                        !win::transfer(pipe, wire.data(), length, true, GetTickCount64() + 30000))
                        break;
                    if (shutdown) {
                        SetEvent(stopping);
                        break;
                    }
                }
                {
                    std::lock_guard lock(worker.mutex);
                    if (worker.engine)
                        for (auto session : sessions)
                            worker.engine->dispatch({{"version", 1},
                                                     {"id", ++sequence},
                                                     {"session", session},
                                                     {"op", "destroy"}});
                }
                {
                    std::lock_guard guard(handles_mutex);
                    handles.erase(pipe);
                    CloseHandle(pipe);
                }
                finished->store(true);
            });
            threads.push_back({std::move(thread), finished});
        }
        {
            std::lock_guard guard(handles_mutex);
            for (auto pipe : handles) {
                CancelIoEx(pipe, nullptr);
                DisconnectNamedPipe(pipe);
            }
        }
        for (auto &task : threads)
            task.thread.join();
        LocalFree(security);
        CloseHandle(stopping);
        ReleaseMutex(singleton);
        CloseHandle(singleton);
        return 0;
    } catch (const std::exception &e) {
        try {
            auto paths = Paths::from_environment();
            atomic_write(paths.cache / L"engine-error.txt", e.what());
        } catch (...) {
        }
        return 1;
    }
}
