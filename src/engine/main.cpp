#include "rime_ime/engine.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern char **environ;
using namespace rime_ime;
namespace {
volatile sig_atomic_t running = 1;
void stop(int) {
    running = 0;
}
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void private_dir(const fs::path &path) {
    for (fs::path current; const auto &component : fs::absolute(path)) {
        current /= component;
        require(!fs::is_symlink(fs::symlink_status(current)),
                "runtime path must not contain symbolic links");
    }
    fs::create_directories(path);
    struct stat stat{};
    require(lstat(path.c_str(), &stat) == 0 && S_ISDIR(stat.st_mode) && stat.st_uid == getuid(),
            "runtime directory must be owned by current user");
    require(chmod(path.c_str(), 0700) == 0, "cannot restrict runtime permissions");
}
struct Options {
    Paths paths = Paths::from_environment();
    fs::path shared = "/usr/share/rime-data", overlay = RIME_IME_DATA_DIR, user, socket;
    bool deploy_only = false;
    Options(int argc, char **argv) {
        user = paths.data / "rime";
        socket = paths.runtime / "engine.sock";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--deploy") {
                deploy_only = true;
                continue;
            }
            if (arg == "--help") {
                std::cout << "r-ime-engine [--shared-data DIR] [--overlay DIR] [--user-data DIR] "
                             "[--socket PATH]\n";
                std::exit(0);
            }
            require(i + 1 < argc, "missing value for " + arg);
            std::string value = argv[++i];
            if (arg == "--shared-data")
                shared = value;
            else if (arg == "--overlay")
                overlay = value;
            else if (arg == "--user-data")
                user = value;
            else if (arg == "--socket")
                socket = value;
            else
                throw std::runtime_error("unknown option: " + arg);
        }
    }
};
void deploy_child(const Options &options, const fs::path &directory,
                  const std::function<void()> &pump = {}) {
    // Keep this generation available even after an installed binary is replaced.
    const std::string executable = "/proc/" + std::to_string(getpid()) + "/exe";
    std::vector<std::string> args = {executable,      "--deploy",
                                     "--shared-data", options.shared.string(),
                                     "--user-data",   directory.string()};
    std::vector<char *> argv;
    for (auto &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid{};
    const auto log = directory / "r-ime-deploy.log";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 2, log.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    int result = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    require(result == 0, std::string("cannot launch deployment worker: ") + strerror(result));
    struct ChildGuard {
        pid_t pid;
        bool reaped = false;
        ~ChildGuard() {
            if (!reaped) {
                kill(pid, SIGKILL);
                int status;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
    } child{pid};
    int status{};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
        auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            child.reaped = true;
            break;
        }
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("cannot wait for deployment");
        if (!running || std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error(
                "Rime deployment cancelled or timed out; previous data retained");
        if (pump)
            pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(pump ? 2 : 20));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::string detail;
        try {
            detail = read_file(log);
            if (detail.size() > 3000)
                detail = detail.substr(detail.size() - 3000);
        } catch (...) {
        }
        throw std::runtime_error("Rime deployment failed; previous configuration retained. " +
                                 detail);
    }
    fs::remove(log);
}
bool user_database(const fs::path &path) {
    auto name = path.filename().string();
    return name.ends_with(".userdb") || name.find(".userdb.") != std::string::npos ||
           name == "user.yaml" || name == "installation.yaml";
}
void sync_tree(const fs::path &root) {
    std::vector<fs::path> directories{root};
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        require(!entry.is_symlink(), "cannot commit a data tree containing symlinks");
        if (entry.is_directory())
            directories.push_back(entry.path());
        else if (entry.is_regular_file()) {
            int fd = open(entry.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            require(fd >= 0, "cannot open data for flush");
            int result = fsync(fd);
            close(fd);
            require(result == 0, "cannot flush user data");
        }
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        int fd = open(it->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(fd >= 0, "cannot open data directory for flush");
        int result = fsync(fd);
        close(fd);
        require(result == 0, "cannot flush data directory");
    }
}
void copy_safe(const fs::path &from, const fs::path &to, bool skip_databases = false) {
    fs::create_directories(to);
    if (!fs::exists(from))
        return;
    for (auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator();
         ++it) {
        const auto &entry = *it;
        if (entry.path().filename() == "build" ||
            entry.path().filename() == ".transaction-committed" ||
            (skip_databases && user_database(entry.path()))) {
            if (entry.is_directory())
                it.disable_recursion_pending();
            continue;
        }
        require(!entry.is_symlink(), "Rime data must not contain symbolic links");
        auto relative = fs::relative(entry.path(), from);
        if (entry.is_directory())
            fs::create_directories(to / relative);
        else if (entry.is_regular_file() && entry.path().filename() != "LOCK")
            fs::copy_file(entry.path(), to / relative, fs::copy_options::overwrite_existing);
    }
}
Json reply_base(const Json &req) {
    return {{"version", 1},
            {"id", req.value("id", std::uint64_t{})},
            {"session", req.value("session", std::uint64_t{})},
            {"ok", true},
            {"handled", false},
            {"commit", ""},
            {"preedit", ""},
            {"cursor", 0},
            {"candidates", Json::array()},
            {"highlighted", 0},
            {"page", 0},
            {"last_page", true},
            {"ascii", false},
            {"simplified", true}};
}
void move_directory(const fs::path &from, const fs::path &to) {
    fs::rename(from, to);
    for (const auto &parent : {from.parent_path(), to.parent_path()}) {
        int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
}
bool recover_directory(const fs::path &target, const std::string &prefix) {
    if (!fs::exists(target.parent_path()))
        return false;
    std::vector<fs::path> previous;
    for (const auto &entry : fs::directory_iterator(target.parent_path()))
        if (entry.is_directory() && !entry.is_symlink() &&
            entry.path().filename().string().starts_with(prefix))
            previous.push_back(entry.path());
    require(previous.size() <= 1,
            "multiple interrupted deployments found; select a recovery directory manually");
    if (previous.empty())
        return false;
    if (fs::exists(target / ".transaction-committed")) {
        // The marker is durable only after config, phrases and the engine probe have
        // succeeded. A partially removed old tree is never used for rollback.
        fs::remove_all(previous.front());
        return true;
    }
    if (fs::exists(target)) {
        auto displaced = target.parent_path() /
                         (target.filename().string() + ".interrupted-" + std::to_string(getpid()));
        require(!fs::exists(displaced), "interrupted data recovery destination already exists");
        move_directory(target, displaced);
    }
    move_directory(previous.front(), target);
    std::cerr << "R-IME recovered the previous data directory after an interrupted swap\n";
    return true;
}
struct Worker {
    Options options;
    Json config;
    std::unique_ptr<Engine> engine;
    std::function<void()> pump_typing;
    bool deploying = false;
    std::string startup_error;
    explicit Worker(Options value) : options(std::move(value)), config(default_config()) {
        recover_directory(options.paths.data, ".r-ime-before-import-");
        recover_directory(options.user, ".rime-old-");
        private_dir(options.paths.data);
        // The deployed snapshot is authoritative at startup, including a crash in
        // rollback after the old directory was restored but before external files.
        // Explicit reload/configure RPCs are the only way to apply edited settings.
        if (fs::exists(options.user / "r-ime-effective.json")) {
            config = load_config(options.user / "r-ime-effective.json");
            save_config(options.paths.config, config);
            if (fs::exists(options.user / "r-ime-phrases.tsv"))
                atomic_write(options.paths.data / "phrases.tsv",
                             read_file(options.user / "r-ime-phrases.tsv"));
        } else
            try {
                config = load_config(options.paths.config);
            } catch (const std::exception &error) {
                startup_error = error.what();
                std::cerr << "R-IME recovery mode: " << startup_error << '\n';
                return;
            }
        try {
            configure(config);
        } catch (const std::exception &error) {
            // Failed upgrades must still permit the last deployed generation to start.
            if (fs::exists(options.user / "r-ime-effective.json"))
                config = load_config(options.user / "r-ime-effective.json");
            require(fs::exists(options.user / "build" / "rime_ime_pinyin.schema.yaml"),
                    std::string("first deployment failed: ") + error.what());
            engine.reset();
            engine = make_engine(config);
            std::cerr << "R-IME retained the previous deployment: " << error.what() << '\n';
        }
    }
    std::unique_ptr<Engine> make_engine(const Json &effective) {
        auto result = std::make_unique<Engine>(options.shared, options.user, effective);
        auto probe =
            result->dispatch({{"version", 1}, {"id", 1}, {"session", 0}, {"op", "create"}});
        require(probe.value("ok", false), "deployed scheme cannot create a session");
        result->dispatch(
            {{"version", 1}, {"id", 2}, {"session", probe["session"]}, {"op", "destroy"}});
        return result;
    }
    void recover_engine() noexcept {
        if (engine || !startup_error.empty())
            return;
        try {
            engine = make_engine(config);
        } catch (const std::exception &error) {
            std::cerr << "R-IME recovery unavailable: " << error.what() << '\n';
        }
    }
    bool configure(const Json &next, const std::string *phrases = nullptr) {
        validate_config(next);
        if (phrases)
            (void)parse_phrases(*phrases);
        std::string previous_phrases = fs::exists(options.paths.data / "phrases.tsv")
                                           ? read_file(options.paths.data / "phrases.tsv")
                                           : "";
        const std::string deployed_phrases = fs::exists(options.user / "r-ime-phrases.tsv")
                                                 ? read_file(options.user / "r-ime-phrases.tsv")
                                                 : "";
        if (engine && next["general"] == config["general"] && next["input"] == config["input"] &&
            next["appearance"]["page_size"] == config["appearance"]["page_size"] &&
            (phrases ? *phrases : previous_phrases) == deployed_phrases) {
            // Color/font/layout and shortcut edits never interrupt active composition or
            // rebuild dictionaries. Page size and learning are compiled schema options.
            save_config(options.user / "r-ime-effective.json", next);
            try {
                save_config(options.paths.config, next);
            } catch (...) {
                save_config(options.user / "r-ime-effective.json", config);
                throw;
            }
            engine->update_config(next);
            config = next;
            return false;
        }
        auto stage = options.user.parent_path() / (".rime-stage-" + std::to_string(getpid()));
        auto old = options.user.parent_path() / (".rime-old-" + std::to_string(getpid()));
        require(!fs::exists(stage) && !fs::exists(old),
                "unfinished deployment directory exists; recovery required");
        // Compile only immutable scheme sources in the child. The old engine keeps
        // serving keys while the child runs; user databases are copied after flush.
        bool moved = false, installed = false;
        try {
            copy_safe(options.user, stage, true);
            prepare_schemas(options.overlay, stage, next);
            if (phrases) {
                std::string tsv;
                for (const auto &p : parse_phrases(*phrases))
                    tsv += p.text + '\t' + p.code + '\t' + std::to_string(p.weight) + '\n';
                atomic_write(stage / "rime_ime_phrases.txt", tsv);
            }
            save_config(stage / "r-ime-effective.json", next);
            atomic_write(stage / "r-ime-phrases.tsv", phrases ? *phrases : previous_phrases);
            deploy_child(options, stage, pump_typing);
            engine.reset();
            if (fs::exists(options.user))
                for (const auto &entry : fs::directory_iterator(options.user))
                    if (user_database(entry.path())) {
                        require(!entry.is_symlink(), "user databases must not be symlinks");
                        if (entry.is_directory())
                            copy_safe(entry.path(), stage / entry.path().filename());
                        else if (entry.is_regular_file())
                            fs::copy_file(entry.path(), stage / entry.path().filename(),
                                          fs::copy_options::overwrite_existing);
                    }
            sync_tree(stage);
            if (fs::exists(options.user)) {
                move_directory(options.user, old);
                moved = true;
            }
            move_directory(stage, options.user);
            installed = true;
            auto candidate = make_engine(next);
            save_config(options.paths.config, next);
            if (phrases)
                atomic_write(options.paths.data / "phrases.tsv", *phrases);
            atomic_write(options.user / ".transaction-committed", "1\n");
            config = next;
            engine = std::move(candidate);
            std::error_code ignored;
            fs::remove_all(old, ignored);
            return true;
        } catch (...) {
            auto failure = std::current_exception();
            if (moved || installed)
                engine.reset();
            std::error_code ignored;
            if (installed)
                fs::remove_all(options.user, ignored);
            if (moved)
                fs::rename(old, options.user, ignored);
            fs::remove_all(stage, ignored);
            try {
                if (installed) {
                    save_config(options.paths.config, config);
                    if (phrases)
                        atomic_write(options.paths.data / "phrases.tsv", previous_phrases);
                }
            } catch (const std::exception &error) {
                std::cerr << "R-IME configuration rollback: " << error.what() << '\n';
            }
            recover_engine();
            std::rethrow_exception(failure);
        }
    }
    void restore_archive(const Json &archive) {
        // Validate, extract, and compile under a sibling directory. The live engine
        // and its exact binary database/build generation remain untouched until this
        // entire stage succeeds; validation failures therefore require no rollback.
        auto relative = options.user.lexically_relative(options.paths.data);
        require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
                "restore requires the engine user directory to be inside R-IME data");
        auto stage =
            options.paths.data.parent_path() / (".r-ime-import-" + std::to_string(getpid()));
        auto old =
            options.paths.data.parent_path() / (".r-ime-before-import-" + std::to_string(getpid()));
        require(!fs::exists(stage) && !fs::exists(old),
                "unfinished restore directory exists; recovery required");
        Paths staged{stage / "config.json", stage / "data", stage / "cache", stage / "runtime"};
        bool moved = false, installed = false;
        try {
            rime_ime::restore(staged, archive);
            auto next = load_config(staged.config);
            fs::remove(staged.data / ".transaction-committed");
            fs::remove(staged.data / relative / ".transaction-committed");
            atomic_write(staged.data / relative / "r-ime-phrases.tsv",
                         fs::exists(staged.data / "phrases.tsv")
                             ? read_file(staged.data / "phrases.tsv")
                             : "");
            prepare_schemas(options.overlay, staged.data / relative, next);
            save_config(staged.data / relative / "r-ime-effective.json", next);
            deploy_child(options, staged.data / relative, pump_typing);
            sync_tree(staged.data);
            engine.reset();
            if (fs::exists(options.paths.data)) {
                move_directory(options.paths.data, old);
                moved = true;
            }
            move_directory(staged.data, options.paths.data);
            installed = true;
            auto candidate = make_engine(next);
            save_config(options.paths.config, next);
            atomic_write(options.user / ".transaction-committed", "1\n");
            atomic_write(options.paths.data / ".transaction-committed", "1\n");
            config = next;
            engine = std::move(candidate);
            std::error_code ignored;
            fs::remove_all(old, ignored);
            fs::remove_all(stage, ignored);
        } catch (...) {
            auto failure = std::current_exception();
            std::error_code ignored;
            if (installed)
                engine.reset();
            if (installed)
                fs::remove_all(options.paths.data, ignored);
            if (moved)
                fs::rename(old, options.paths.data, ignored);
            fs::remove_all(stage, ignored);
            try {
                if (installed)
                    save_config(options.paths.config, config);
            } catch (const std::exception &error) {
                std::cerr << "R-IME restore rollback: " << error.what() << '\n';
            }
            recover_engine();
            std::rethrow_exception(failure);
        }
    }
    Json dispatch(const Json &request) {
        auto reply = reply_base(request);
        try {
            require(request.at("version") == 1, "unsupported protocol version");
            auto op = request.at("op").get<std::string>();
            const bool mutation =
                op == "configure" || op == "reload" || op == "backup" || op == "restore";
            require(!mutation || !deploying, "a configuration operation is already in progress");
            struct Guard {
                bool &busy;
                bool owns;
                Guard(bool &b, bool own) : busy(b), owns(own) {
                    if (owns)
                        busy = true;
                }
                ~Guard() {
                    if (owns)
                        busy = false;
                }
            } guard(deploying, mutation);
            if (!startup_error.empty()) {
                if (op == "status" && request.value("session", 0) == 0) {
                    reply["config"] = config;
                    reply["diagnostic"] =
                        "Configuration requires backup recovery: " + startup_error;
                    reply["recovery_required"] = true;
                    return reply;
                }
                require(op == "restore" ||
                            (op == "configure" && request.value("repair_config", false)),
                        "configuration is damaged; restore a valid backup first: " + startup_error);
            }
            if (op == "configure" || op == "reload") {
                if (!startup_error.empty() && fs::exists(options.paths.config))
                    atomic_write(options.paths.config.string() + ".invalid",
                                 read_file(options.paths.config));
                auto next =
                    op == "configure" ? request.at("config") : load_config(options.paths.config);
                std::string phrases;
                const std::string *ptr = nullptr;
                if (request.contains("phrases")) {
                    phrases = request.at("phrases");
                    ptr = &phrases;
                }
                reply["sessions_invalidated"] = configure(next, ptr);
                startup_error.clear();
                reply["config"] = config;
                return reply;
            }
            if (op == "backup") {
                engine.reset();
                try {
                    reply["backup"] = rime_ime::backup(options.paths);
                } catch (...) {
                    recover_engine();
                    throw;
                }
                engine = make_engine(config);
                reply["sessions_invalidated"] = true;
                return reply;
            }
            if (op == "restore") {
                if (!startup_error.empty() && fs::exists(options.paths.config))
                    atomic_write(options.paths.config.string() + ".invalid",
                                 read_file(options.paths.config));
                restore_archive(request.at("backup"));
                startup_error.clear();
                reply["config"] = config;
                reply["sessions_invalidated"] = true;
                return reply;
            }
            require(
                bool(engine),
                "engine unavailable after failed recovery; inspect the journal and restart R-IME");
            return engine->dispatch(request);
        } catch (const std::exception &error) {
            recover_engine();
            reply["ok"] = false;
            reply["error"] = error.what();
            reply["sessions_invalidated"] = true;
            return reply;
        }
    }
};
struct Client {
    std::string input, output;
    std::set<std::uint64_t> sessions;
    std::uint64_t last = 0, token = 0;
};
} // namespace
int main(int argc, char **argv) {
    try {
        Options options(argc, argv);
        if (options.deploy_only) {
            deploy(options.shared, options.user);
            return 0;
        }
        umask(0077);
        private_dir(options.socket.parent_path());
        int lock = open((options.socket.parent_path() / "engine.lock").c_str(),
                        O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0,
                "another R-IME engine is running");
        struct stat lock_stat{};
        require(fstat(lock, &lock_stat) == 0 && lock_stat.st_uid == getuid() &&
                    S_ISREG(lock_stat.st_mode),
                "invalid engine lock file");
        signal(SIGTERM, stop);
        signal(SIGINT, stop);
        signal(SIGPIPE, SIG_IGN);
        Worker worker(options);
        int server = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        require(server >= 0, "cannot create engine socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        auto name = options.socket.string();
        require(name.size() < sizeof(address.sun_path), "engine socket path is too long");
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        if (fs::exists(options.socket)) {
            struct stat st{};
            require(lstat(options.socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) &&
                        st.st_uid == getuid(),
                    "refusing to replace non-socket runtime file");
            fs::remove(options.socket);
        }
        require(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
                "cannot bind engine socket");
        chmod(options.socket.c_str(), 0600);
        require(listen(server, 16) == 0, "cannot listen on engine socket");
        std::map<int, Client> clients;
        std::set<int> busy;
        std::uint64_t connection_token = 0;
        std::function<void(int)> process_clients = [&](int timeout) {
            std::vector<pollfd> descriptors{{server, POLLIN, 0}};
            std::vector<std::uint64_t> tokens{0};
            for (const auto &[fd, c] : clients)
                if (!busy.contains(fd)) {
                    descriptors.push_back(
                        {fd, short(POLLIN | (c.output.empty() ? 0 : POLLOUT)), 0});
                    tokens.push_back(c.token);
                }
            int ready = poll(descriptors.data(), descriptors.size(), timeout);
            if (ready < 0) {
                if (errno == EINTR)
                    return;
                throw std::runtime_error("engine poll failed");
            }
            if (descriptors[0].revents & POLLIN) {
                int fd = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd >= 0) {
                    ucred peer{};
                    socklen_t size = sizeof(peer);
                    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
                        peer.uid == getuid() && clients.size() < 32) {
                        Client accepted;
                        accepted.token = ++connection_token;
                        clients.emplace(fd, std::move(accepted));
                    } else
                        close(fd);
                }
            }
            std::vector<std::pair<int, std::uint64_t>> closed;
            for (std::size_t i = 1; i < descriptors.size(); ++i) {
                auto descriptor = descriptors[i];
                if (!clients.contains(descriptor.fd) ||
                    clients.at(descriptor.fd).token != tokens[i] || busy.contains(descriptor.fd))
                    continue;
                auto &client = clients.at(descriptor.fd);
                bool dead = (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL));
                if (descriptor.revents & POLLIN) {
                    char buffer[8192];
                    auto count = recv(descriptor.fd, buffer, sizeof(buffer), 0);
                    if (count <= 0) {
                        if (count == 0 || (errno != EAGAIN && errno != EINTR))
                            dead = true;
                    } else
                        client.input.append(buffer, count);
                    if (client.input.size() > 140 * 1024 * 1024)
                        dead = true;
                    for (std::size_t newline;
                         !dead && (newline = client.input.find('\n')) != std::string::npos;) {
                        auto line = client.input.substr(0, newline);
                        client.input.erase(0, newline + 1);
                        Json response;
                        try {
                            auto request = Json::parse(line);
                            require(request.is_object(), "request must be an object");
                            response = reply_base(request);
                            require(request.at("id").is_number_unsigned() ||
                                        (request.at("id").is_number_integer() &&
                                         request.at("id").get<std::int64_t>() > 0),
                                    "invalid request id");
                            auto id = request.at("id").get<std::uint64_t>();
                            require(id > client.last, "stale request sequence");
                            client.last = id;
                            auto session = request.at("session").get<std::uint64_t>();
                            auto op = request.at("op").get<std::string>();
                            require(session == 0 || client.sessions.contains(session),
                                    "session belongs to another connection");
                            busy.insert(descriptor.fd);
                            try {
                                response = worker.dispatch(request);
                            } catch (...) {
                                busy.erase(descriptor.fd);
                                throw;
                            }
                            busy.erase(descriptor.fd);
                            if (response.value("ok", false) && op == "create")
                                client.sessions.insert(response["session"].get<std::uint64_t>());
                            if (response.value("ok", false) && op == "destroy")
                                client.sessions.erase(session);
                            // Keep stale IDs associated with their connection. Their next request
                            // gets an explicit expired-session error, never a different app's
                            // state.
                        } catch (const std::exception &error) {
                            if (!response.is_object())
                                response = reply_base(Json::object());
                            response["ok"] = false;
                            response["error"] = error.what();
                        }
                        client.output += response.dump() + "\n";
                        if (client.output.size() > 140 * 1024 * 1024)
                            dead = true;
                    }
                }
                if (!dead && !client.output.empty()) {
                    auto count = send(descriptor.fd, client.output.data(), client.output.size(),
                                      MSG_NOSIGNAL);
                    if (count > 0)
                        client.output.erase(0, count);
                    else if (errno != EAGAIN && errno != EINTR)
                        dead = true;
                }
                if (dead)
                    closed.emplace_back(descriptor.fd, tokens[i]);
            }
            for (auto [fd, token] : closed) {
                if (!clients.contains(fd) || clients.at(fd).token != token || busy.contains(fd))
                    continue;
                auto &client = clients.at(fd);
                for (auto session : client.sessions)
                    worker.dispatch({{"version", 1},
                                     {"id", ++client.last},
                                     {"session", session},
                                     {"op", "destroy"}});
                close(fd);
                clients.erase(fd);
            }
        };
        worker.pump_typing = [&] { process_clients(0); };
        while (running)
            process_clients(1000);
        for (const auto &[fd, _] : clients)
            close(fd);
        close(server);
        fs::remove(options.socket);
        close(lock);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "r-ime-engine: " << error.what() << '\n';
        return 1;
    }
}
