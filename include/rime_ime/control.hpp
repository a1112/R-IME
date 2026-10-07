#pragma once
#include <memory>
#include <optional>
#include <rime_ime/config.hpp>
#include <stdexcept>

namespace rime_ime {
class ControlError : public std::runtime_error {
    std::string code_;
    bool sessionsInvalidated_;

  public:
    ControlError(std::string code, std::string message, bool sessions_invalidated = false);
    const std::string &code() const noexcept;
    bool sessions_invalidated() const noexcept;
};

// One connection owns its input sessions. Call from one thread at a time.
// This client never starts an engine or retries a failed mutation.
class ControlClient {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit ControlClient(fs::path socket = Paths::from_environment().runtime / "engine.sock",
                           int request_timeout_ms = 2000, int deployment_timeout_ms = 180000);
    ~ControlClient();
    ControlClient(const ControlClient &) = delete;
    ControlClient &operator=(const ControlClient &) = delete;
    // Zero chooses the ordinary/deployment default according to the operation.
    Json call(Json request, int timeout_ms = 0);
    // Snapshot contains config, raw phrase TSV and recovery diagnostics.
    Json configuration();
    Json status();
    Json configure(Json config, std::optional<std::string> phrases = std::nullopt);
    Json patch_config(Json object);
    Json set_config(std::string pointer, Json value);
    std::vector<Phrase> phrases();
    Json replace_phrases(std::string tsv);
    Json upsert_phrase(Phrase phrase);
    Json remove_phrase(std::string code, std::string text);
    // Backup/restore may invalidate input sessions; never read live dictionary
    // databases directly to build a backup.
    Json backup();
    Json restore(Json archive);
    void disconnect();
};
} // namespace rime_ime
