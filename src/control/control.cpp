#include "../ipc/client.hpp"
#include <algorithm>
#include <limits>
#include <rime_ime/control.hpp>
#include <set>

namespace rime_ime {
ControlError::ControlError(std::string code, std::string message, bool sessions_invalidated)
    : std::runtime_error(std::move(message)), code_(std::move(code)),
      sessionsInvalidated_(sessions_invalidated) {}
const std::string &ControlError::code() const noexcept {
    return code_;
}
bool ControlError::sessions_invalidated() const noexcept {
    return sessionsInvalidated_;
}
namespace {
template <class F> auto validate(F action) -> decltype(action()) {
    try {
        return action();
    } catch (const ControlError &) {
        throw;
    } catch (const std::exception &error) {
        throw ControlError("invalid_request", error.what());
    }
}
void require(bool condition, const char *message) {
    if (!condition)
        throw ControlError("invalid_request", message);
}
void validate_timeout(int timeout) {
    require(timeout > 0 && timeout <= 600000, "timeout must be 1–600000 milliseconds");
}
bool unsigned_integer(const Json &value) {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}
void existing_keys(const Json &current, const Json &patch) {
    if (!patch.is_object())
        return;
    require(current.is_object(), "an object cannot be added beneath a scalar configuration key");
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        require(current.contains(it.key()), "configuration edit contains an unknown key");
        existing_keys(current.at(it.key()), it.value());
    }
}
// Validate an edit before connecting without rejecting a valid new shortcut
// merely because it overlaps another *default* binding. Actual conflicts are
// checked again against the worker's complete current configuration.
void validate_edit(Json probe) {
    const auto defaults = default_config();
    if (probe.contains("shortcuts") && probe["shortcuts"].is_object()) {
        auto &shortcuts = probe["shortcuts"];
        std::set<std::string> fixed;
        for (auto it = shortcuts.begin(); it != shortcuts.end(); ++it)
            if (!defaults["shortcuts"].contains(it.key()) ||
                it.value() != defaults["shortcuts"][it.key()])
                fixed.insert(it.value().get<std::string>());
        int next = 1;
        for (auto it = shortcuts.begin(); it != shortcuts.end(); ++it) {
            if (!defaults["shortcuts"].contains(it.key()) ||
                it.value() != defaults["shortcuts"][it.key()])
                continue;
            std::string binding;
            do {
                binding = "F" + std::to_string(next++);
            } while (fixed.count(binding));
            it.value() = binding;
            fixed.insert(binding);
        }
    }
    validate_config(probe);
}
void usable(const Json &snapshot) {
    if (snapshot.value("recovery_required", false))
        throw ControlError("engine_error",
                           "configuration requires explicit repair or backup restore: " +
                               snapshot.value("diagnostic", std::string{}));
}
Json configure_snapshot(ControlClient &client, const Json &snapshot, Json config,
                        std::optional<std::string> phrases) {
    usable(snapshot);
    config = validate([&] { return normalize_config(std::move(config)); });
    Json request{{"op", "configure"},
                 {"config", std::move(config)},
                 {"expected_config", snapshot.at("config")},
                 {"expected_phrases", snapshot.at("phrases")}};
    if (phrases) {
        validate([&] { parse_phrases(*phrases); });
        request["phrases"] = std::move(*phrases);
    }
    return client.call(std::move(request));
}
std::vector<Phrase> stored_phrases(const Json &snapshot) {
    usable(snapshot);
    try {
        return parse_phrases(snapshot.at("phrases").get<std::string>());
    } catch (const std::exception &error) {
        throw ControlError("engine_error",
                           "stored phrases are invalid: " + std::string(error.what()));
    }
}
} // namespace
struct ControlClient::Impl {
    Client client;
    int ordinary, deployment;
    Impl(fs::path socket, int request, int deploy)
        : client(std::move(socket)), ordinary(request), deployment(deploy) {}
};
ControlClient::ControlClient(fs::path socket, int request, int deployment) : impl_(nullptr) {
    validate_timeout(request);
    validate_timeout(deployment);
    impl_ = std::make_unique<Impl>(std::move(socket), request, deployment);
}
ControlClient::~ControlClient() = default;
Json ControlClient::call(Json request, int timeout) {
    validate([&] {
        require(request.is_object(), "request must be a JSON object");
        require(request.contains("op") && request["op"].is_string() &&
                    !request["op"].get<std::string>().empty(),
                "request op must be a nonempty string");
        if (request.contains("version"))
            require(unsigned_integer(request["version"]) && request["version"] == 1,
                    "unsupported protocol version");
        if (request.contains("session"))
            require(unsigned_integer(request["session"]), "session must be a nonnegative integer");
        require(timeout >= 0 && timeout <= 600000,
                "timeout override must be 0–600000 milliseconds");
        // Reserve the exact metadata types that Client will put on the wire,
        // using the widest generated id. One extra byte covers Unix framing.
        // Rejecting locally must not disconnect an existing session owner.
        request["version"] = 1;
        request["id"] = std::numeric_limits<std::uint64_t>::max();
        if (!request.contains("session"))
            request["session"] = 0;
        constexpr std::size_t maximumFrame = 136 * 1024 * 1024;
        require(request.dump().size() <= maximumFrame - 1,
                "request frame exceeds the 136 MiB limit");
    });
    auto op = request["op"].get<std::string>();
    if (!timeout)
        timeout = op == "configure" || op == "restore" || op == "backup" ? impl_->deployment
                                                                         : impl_->ordinary;
    try {
        return impl_->client.call(std::move(request), timeout);
    } catch (const RpcError &error) {
        const auto &reply = error.reply();
        const auto code = reply.contains("error_code") && reply["error_code"] == "conflict"
                              ? "conflict"
                              : "engine_error";
        throw ControlError(code, error.what(),
                           error.reply().value("sessions_invalidated", false));
    } catch (const std::exception &error) {
        throw ControlError("transport_error", error.what(), true);
    }
}
Json ControlClient::configuration() {
    auto snapshot = call({{"op", "configuration"}});
    try {
        if (!snapshot.at("config").is_object() || !snapshot.at("phrases").is_string())
            throw std::runtime_error("missing configuration or phrase text");
        validate_config(snapshot["config"]);
        if (snapshot.contains("recovery_required") && !snapshot["recovery_required"].is_boolean())
            throw std::runtime_error("invalid recovery status");
        if (snapshot.contains("diagnostic") && !snapshot["diagnostic"].is_string())
            throw std::runtime_error("invalid diagnostic text");
    } catch (const std::exception &error) {
        throw ControlError("engine_error",
                           "invalid configuration response: " + std::string(error.what()));
    }
    return snapshot;
}
Json ControlClient::status() {
    return call({{"op", "status"}});
}
Json ControlClient::configure(Json config, std::optional<std::string> phrases) {
    config = validate([&] { return normalize_config(std::move(config)); });
    if (phrases)
        validate([&] { parse_phrases(*phrases); });
    auto snapshot = configuration();
    return configure_snapshot(*this, snapshot, std::move(config), std::move(phrases));
}
Json ControlClient::patch_config(Json patch) {
    validate([&] {
        require(patch.is_object(), "configuration patch must be an object");
        auto probe = default_config();
        existing_keys(probe, patch);
        probe.merge_patch(patch);
        validate_edit(std::move(probe));
    });
    auto snapshot = configuration();
    auto next = snapshot.at("config");
    usable(snapshot);
    validate([&] {
        existing_keys(next, patch);
        next.merge_patch(patch);
        validate_config(next);
    });
    return configure_snapshot(*this, snapshot, std::move(next), std::nullopt);
}
Json ControlClient::set_config(std::string pointer, Json value) {
    auto path = validate([&] {
        require(!pointer.empty(), "the configuration root cannot be replaced");
        Json::json_pointer path(pointer);
        auto probe = default_config();
        require(probe.contains(path), "configuration pointer does not identify an existing key");
        existing_keys(probe.at(path), value);
        probe.at(path) = value;
        validate_edit(std::move(probe));
        return path;
    });
    auto snapshot = configuration();
    auto next = snapshot.at("config");
    usable(snapshot);
    validate([&] {
        require(next.contains(path), "configuration pointer does not identify an existing key");
        existing_keys(next.at(path), value);
        next.at(path) = std::move(value);
        validate_config(next);
    });
    return configure_snapshot(*this, snapshot, std::move(next), std::nullopt);
}
std::vector<Phrase> ControlClient::phrases() {
    return stored_phrases(configuration());
}
Json ControlClient::replace_phrases(std::string tsv) {
    validate([&] { parse_phrases(tsv); });
    auto snapshot = configuration();
    return configure_snapshot(*this, snapshot, snapshot.at("config"), std::move(tsv));
}
Json ControlClient::upsert_phrase(Phrase phrase) {
    validate([&] { format_phrases({phrase}); });
    auto snapshot = configuration();
    auto entries = stored_phrases(snapshot);
    auto existing = std::find_if(entries.begin(), entries.end(), [&](const Phrase &entry) {
        return entry.code == phrase.code && entry.text == phrase.text;
    });
    if (existing == entries.end())
        entries.push_back(std::move(phrase));
    else
        *existing = std::move(phrase);
    auto tsv = validate([&] { return format_phrases(entries); });
    return configure_snapshot(*this, snapshot, snapshot.at("config"), std::move(tsv));
}
Json ControlClient::remove_phrase(std::string code, std::string text) {
    validate([&] { format_phrases({Phrase{code, text, 100}}); });
    auto snapshot = configuration();
    auto entries = stored_phrases(snapshot);
    auto existing = std::find_if(entries.begin(), entries.end(), [&](const Phrase &entry) {
        return entry.code == code && entry.text == text;
    });
    require(existing != entries.end(), "phrase does not exist");
    entries.erase(existing);
    auto tsv = validate([&] { return format_phrases(entries); });
    return configure_snapshot(*this, snapshot, snapshot.at("config"), std::move(tsv));
}
Json ControlClient::backup() {
    auto reply = call({{"op", "backup"}});
    if (!reply.contains("backup") || !reply["backup"].is_object())
        throw ControlError("engine_error", "backup response contains no archive",
                           reply.value("sessions_invalidated", true));
    return reply.at("backup");
}
Json ControlClient::restore(Json archive) {
    require(archive.is_object(), "backup archive must be an object");
    return call({{"op", "restore"}, {"backup", std::move(archive)}});
}
void ControlClient::disconnect() {
    impl_->client.disconnect();
}
} // namespace rime_ime
