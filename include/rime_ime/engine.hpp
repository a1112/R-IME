#pragma once
#include "config.hpp"
#include <memory>
namespace rime_ime {
// A process owns exactly one engine: librime has process-global state.
class Engine {
  public:
    Engine(fs::path shared_data, fs::path user_data, Json config);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Json dispatch(const Json &request);
    void update_config(const Json &config);
    void sync();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Builds three private schemes. Deployment is run in a fresh child process.
void prepare_schemas(const fs::path &overlay, const fs::path &user_data, const Json &config);
void deploy(const fs::path &shared_data, const fs::path &user_data);
} // namespace rime_ime
