#include "rime_ime/engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <rime_api.h>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace rime_ime {
namespace {
constexpr std::array<const char *, 3> schemas = {"rime_ime_pinyin", "rime_ime_flypy",
                                                 "rime_ime_ziranma"};
std::atomic<std::uint64_t> next_session{1};
std::atomic<std::uint64_t> next_generation{1};
void require(bool value, const std::string &error) {
    if (!value)
        throw std::runtime_error(error);
}
RimeTraits traits_for(const std::string &shared, const std::string &user) {
    RimeTraits traits{};
    RIME_STRUCT_INIT(RimeTraits, traits);
    traits.shared_data_dir = shared.c_str();
    traits.user_data_dir = user.c_str();
    traits.distribution_name = "R-IME";
    traits.distribution_code_name = "r-ime";
    traits.distribution_version = "0.2.0";
    traits.app_name = "rime.r-ime";
    traits.min_log_level = 2;
    traits.log_dir = "";
    return traits;
}
Json empty_reply(const Json &req) {
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
            {"full_shape", false},
            {"ascii_punctuation", false},
            {"simplified", true}};
}
template <class Api>
bool highlight_candidate(Api *api, RimeSessionId id, std::size_t index, bool backward) {
    // New librime exposes explicit selection independent of schema key bindings.
    // Older supported Linux headers keep their established selector behavior.
    if constexpr (requires { api->highlight_candidate; })
        if (RIME_API_AVAILABLE(api, highlight_candidate))
            return bool(api->highlight_candidate(id, index));
    return bool(api->process_key(id, backward ? 0xff52 : 0xff54, 0));
}
} // namespace
void prepare_schemas(const fs::path &overlay, const fs::path &user, const Json &cfg) {
    validate_config(cfg);
    fs::create_directories(user);
    YAML::Node defaults;
    for (auto schema : schemas) {
        YAML::Node item;
        item["schema"] = schema;
        defaults["patch"]["schema_list"].push_back(item);
    }
    defaults["patch"]["menu/page_size"] = cfg["appearance"]["page_size"].get<int>();
    atomic_write(user / "default.custom.yaml", YAML::Dump(defaults) + "\n");
    for (auto schema : schemas) {
        auto source = overlay / (std::string(schema) + ".schema.yaml");
        auto document = YAML::Load(read_file(source));
        require(document.IsMap(), "invalid schema overlay: " + source.string());
        // librime skips automatic .custom patches when a schema already has
        // a root __patch. Put product options in that explicit patch instead.
        document["__patch"]["menu/page_size"] = cfg["appearance"]["page_size"].get<int>();
        document["__patch"]["translator/enable_user_dict"] = cfg["general"]["learning"].get<bool>();
        document["__patch"]["ascii_composer/switch_key/Shift_L"] = "noop";
        document["__patch"]["ascii_composer/switch_key/Shift_R"] = "noop";
        atomic_write(user / source.filename(), YAML::Dump(document) + "\n");
        fs::remove(user / (std::string(schema) + ".custom.yaml"));
    }
    auto phrase_file = user.parent_path() / "phrases.tsv";
    std::string rime_phrases;
    if (fs::exists(phrase_file))
        for (const auto &phrase : parse_phrases(read_file(phrase_file)))
            rime_phrases +=
                phrase.text + '\t' + phrase.code + '\t' + std::to_string(phrase.weight) + '\n';
    atomic_write(user / "rime_ime_phrases.txt", rime_phrases);
}
void deploy(const fs::path &shared_data, const fs::path &user_data) {
    for (const auto *file :
         {"default.yaml", "key_bindings.yaml", "punctuation.yaml", "luna_pinyin.dict.yaml"})
        require(fs::is_regular_file(shared_data / file),
                std::string("missing Rime shared data: ") + file +
                    " (install rime-prelude and rime-data-luna-pinyin)");
    auto shared = path_utf8(shared_data), user = path_utf8(user_data);
    auto traits = traits_for(shared, user);
    auto api = rime_get_api();
    api->setup(&traits);
    api->deployer_initialize(&traits);
    try {
        require(api->deploy_config_file("default.yaml", "config_version"),
                "failed to deploy default Rime configuration");
        for (auto schema : schemas) {
            require(api->deploy_schema(
                        path_utf8(user_data / (std::string(schema) + ".schema.yaml")).c_str()),
                    std::string("failed to deploy schema: ") + schema);
            auto compiled =
                YAML::Load(read_file(user_data / "build" / (std::string(schema) + ".schema.yaml")));
            require(compiled["engine"]["processors"].IsSequence() &&
                        compiled["translator"]["dictionary"].IsScalar(),
                    std::string("incomplete compiled schema: ") + schema);
        }
        api->finalize();
    } catch (...) {
        api->finalize();
        throw;
    }
}
struct Engine::Impl {
    RimeApi *api = rime_get_api();
    std::string shared, user;
    Json config;
    struct Session {
        RimeSessionId rime;
        std::uint64_t last = 0;
        bool sensitive = false;
    };
    std::map<std::uint64_t, Session> sessions;
    std::uint64_t generation = next_generation++;
    Impl(fs::path s, fs::path u, Json c)
        : shared(path_utf8(s)), user(path_utf8(u)), config(normalize_config(std::move(c))) {
        auto traits = traits_for(shared, user);
        api->setup(&traits);
        api->initialize(&traits);
        // Rime releases dictionary caches when the last session disappears.
        // Keep an idle, non-learning session per scheme so focus changes never
        // reopen LevelDB or cold-load dictionaries in the key handling budget.
        try {
            for (const auto *schema : schemas) {
                auto warm = api->create_session();
                require(warm != 0 && api->select_schema(warm, schema), "cannot warm input scheme");
                api->set_option(warm, "_no_learning", true);
            }
        } catch (...) {
            api->cleanup_all_sessions();
            api->finalize();
            throw;
        }
    }
    ~Impl() {
        api->cleanup_all_sessions();
        api->finalize();
    }
    void options(Session &session) {
        const auto &g = config["general"];
        api->set_option(session.rime, "ascii_mode", g["ascii"].get<bool>());
        api->set_option(session.rime, "simplification", g["simplified"].get<bool>());
        api->set_option(session.rime, "full_shape", g.value("full_shape", false));
        api->set_option(session.rime, "ascii_punct", g["ascii_punctuation"].get<bool>());
        api->set_option(session.rime, "_no_learning",
                        session.sensitive || !g["learning"].get<bool>());
    }
    void snapshot(RimeSessionId id, Json &reply) {
        RIME_STRUCT(RimeCommit, commit);
        if (api->get_commit(id, &commit)) {
            reply["commit"] = reply["commit"].get<std::string>() + (commit.text ? commit.text : "");
            api->free_commit(&commit);
        }
        RIME_STRUCT(RimeContext, context);
        if (api->get_context(id, &context)) {
            reply["preedit"] = context.composition.preedit ? context.composition.preedit : "";
            reply["cursor"] = context.composition.cursor_pos;
            reply["highlighted"] = context.menu.highlighted_candidate_index;
            reply["page"] = context.menu.page_no;
            reply["last_page"] = bool(context.menu.is_last_page);
            for (int i = 0; i < context.menu.num_candidates; ++i) {
                const auto &candidate = context.menu.candidates[i];
                reply["candidates"].push_back(
                    {{"text", candidate.text ? candidate.text : ""},
                     {"comment", candidate.comment ? candidate.comment : ""}});
            }
            api->free_context(&context);
        }
        reply["ascii"] = bool(api->get_option(id, "ascii_mode"));
        reply["full_shape"] = bool(api->get_option(id, "full_shape"));
        reply["ascii_punctuation"] = bool(api->get_option(id, "ascii_punct"));
        reply["simplified"] = bool(api->get_option(id, "simplification"));
        char current[128]{};
        if (api->get_current_schema(id, current, sizeof(current)))
            reply["schema"] = current;
    }
    void raw_commit(RimeSessionId id, Json &reply) {
        const char *input = api->get_input(id);
        reply["commit"] = input ? input : "";
        api->clear_composition(id);
    }
    bool navigate(RimeSessionId id, bool backward) {
        RIME_STRUCT(RimeContext, context);
        if (!api->get_context(id, &context))
            return false;
        const auto &menu = context.menu;
        bool has_menu = menu.num_candidates > 0 && menu.page_size > 0;
        auto current = std::size_t(std::max(0, menu.page_no)) * std::size_t(menu.page_size) +
                       std::size_t(std::max(0, menu.highlighted_candidate_index));
        bool boundary = backward ? current == 0
                                 : menu.is_last_page &&
                                       menu.highlighted_candidate_index >= menu.num_candidates - 1;
        api->free_context(&context);
        if (!has_menu)
            return false;
        if (!boundary)
            (void)highlight_candidate(api, id, backward ? current - 1 : current + 1, backward);
        // Arrow keys stay with the candidate menu at its ends, too.
        return true;
    }
    Json run(const Json &req) {
        auto reply = empty_reply(req);
        reply["generation"] = generation;
        try {
            require(req.is_object() && req.at("version") == 1, "unsupported protocol version");
            require(req.at("id").is_number_unsigned() ||
                        (req.at("id").is_number_integer() && req.at("id").get<std::int64_t>() > 0),
                    "invalid request id");
            auto serial = req.at("id").get<std::uint64_t>();
            require(serial > 0, "request id must be positive");
            auto external = req.at("session").get<std::uint64_t>();
            auto op = req.at("op").get<std::string>();
            if (op == "create") {
                require(sessions.size() < 1024, "session limit reached");
                Session session{api->create_session(), serial, req.value("sensitive", false)};
                require(session.rime != 0, "cannot create Rime session");
                if (!api->select_schema(session.rime,
                                        config["input"]["schema"].get<std::string>().c_str())) {
                    api->destroy_session(session.rime);
                    throw std::runtime_error("configured Rime schema is unavailable");
                }
                options(session);
                external = next_session++;
                sessions.emplace(external, session);
                reply["session"] = external;
                snapshot(session.rime, reply);
                return reply;
            }
            if (op == "status" && external == 0) {
                reply["config"] = config;
                reply["engine_version"] = api->get_version();
                return reply;
            }
            auto found = sessions.find(external);
            require(found != sessions.end(), "unknown or expired session");
            auto &session = found->second;
            require(serial > session.last, "stale request sequence");
            session.last = serial;
            auto id = session.rime;
            if (op == "destroy") {
                api->destroy_session(id);
                sessions.erase(found);
                return reply;
            }
            if (op == "reset") {
                api->clear_composition(id);
                reply["handled"] = true;
            } else if (op == "key") {
                require(req.at("keycode").is_number_unsigned() ||
                            (req.at("keycode").is_number_integer() &&
                             req.at("keycode").get<std::int64_t>() >= 0),
                        "invalid keycode");
                auto key = req.at("keycode").get<std::uint32_t>(),
                     modifiers = req.at("modifiers").get<std::uint32_t>();
                if (session.sensitive || req.value("sensitive", false)) {
                    api->clear_composition(id);
                    api->set_option(id, "_no_learning", true);
                } else if (!(modifiers & (1u << 30))) {
                    const char *input = api->get_input(id);
                    bool composing = input && *input;
                    if ((modifiers & (4u | 8u | 64u)) && !composing)
                        reply["handled"] = false;
                    else if ((key == 0xff0d || key == 0xff8d) && composing) {
                        raw_commit(id, reply);
                        reply["handled"] = true;
                    } else
                        reply["handled"] = bool(api->process_key(id, static_cast<int>(key),
                                                                 static_cast<int>(modifiers)));
                }
            } else if (op == "select") {
                int index = req.at("index");
                RIME_STRUCT(RimeContext, context);
                bool valid = api->get_context(id, &context);
                int count = valid ? context.menu.num_candidates : 0;
                if (valid)
                    api->free_context(&context);
                require(index >= 0 && index < count, "candidate index out of range");
                reply["handled"] = bool(api->select_candidate_on_current_page(id, index));
            } else if (op == "page")
                reply["handled"] =
                    bool(api->process_key(id, req.value("backward", false) ? 0xff55 : 0xff56, 0));
            else if (op == "navigate")
                reply["handled"] = navigate(id, req.value("backward", false));
            else if (op == "toggle_ascii" || op == "set_ascii") {
                bool current = bool(api->get_option(id, "ascii_mode"));
                bool next = op == "set_ascii" ? req.at("value").get<bool>() : !current;
                if (current != next) {
                    raw_commit(id, reply);
                    api->set_option(id, "ascii_mode", next);
                }
                reply["handled"] = true;
            } else if (op == "toggle_full_shape" || op == "set_full_shape") {
                bool next = op == "set_full_shape" ? req.at("value").get<bool>()
                                                    : !api->get_option(id, "full_shape");
                api->set_option(id, "full_shape", next);
                reply["handled"] = true;
            } else if (op == "toggle_ascii_punctuation" || op == "set_ascii_punctuation") {
                bool next = op == "set_ascii_punctuation" ? req.at("value").get<bool>()
                                                           : !api->get_option(id, "ascii_punct");
                api->set_option(id, "ascii_punct", next);
                reply["handled"] = true;
            } else if (op == "toggle_simplified") {
                api->set_option(id, "simplification", !api->get_option(id, "simplification"));
                reply["handled"] = true;
            } else if (op == "set_schema") {
                auto schema = req.at("schema").get<std::string>();
                require(std::find(schemas.begin(), schemas.end(), schema) != schemas.end(),
                        "unsupported scheme");
                api->clear_composition(id);
                require(api->select_schema(id, schema.c_str()), "scheme unavailable");
                options(session);
                reply["handled"] = true;
            } else
                require(op == "status", "unknown operation");
            snapshot(id, reply);
        } catch (const std::exception &error) {
            reply["ok"] = false;
            reply["error"] = error.what();
        }
        return reply;
    }
};
Engine::Engine(fs::path shared, fs::path user, Json config)
    : impl_(std::make_unique<Impl>(std::move(shared), std::move(user), std::move(config))) {}
Engine::~Engine() = default;
Json Engine::dispatch(const Json &request) {
    return impl_->run(request);
}
void Engine::update_config(const Json &config) {
    impl_->config = normalize_config(config);
}
void Engine::sync() {
    impl_->api->sync_user_data();
    impl_->api->join_maintenance_thread();
    impl_->sessions.clear();
    impl_->generation = next_generation++;
}
} // namespace rime_ime
