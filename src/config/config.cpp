#include "config/config.hpp"

#include "util/parsers.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>

namespace yacm::config {
namespace {

const char* env(const char* key) {
    const char* v = std::getenv(key);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

// Parses a simple KEY=VALUE file (.env). Comments (#) and empty lines are
// skipped; values may be quoted.
std::map<std::string, std::string> load_dotenv(const std::string& path) {
    std::map<std::string, std::string> result;
    std::ifstream in(path);
    if (!in) return result;

    std::string line;
    while (std::getline(in, line)) {
        std::string t = util::trim(line);
        if (t.empty() || t.front() == '#') continue;

        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = util::trim(t.substr(0, eq));
        std::string value = util::trim(t.substr(eq + 1));
        if (key.empty()) continue;

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        result[key] = value;
    }
    return result;
}

// Resolution order: process environment > .env file > fallback defaults.
class ValueSource {
public:
    explicit ValueSource(const std::map<std::string, std::string>& file)
        : file_(file) {}

    std::optional<std::string> str(const char* key) const {
        if (const char* v = env(key)) return std::optional<std::string>(v);
        auto it = file_.find(key);
        if (it != file_.end() && !it->second.empty()) return it->second;
        return std::nullopt;
    }

    std::optional<int64_t> i64(const char* key) const {
        auto v = str(key);
        if (!v) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        long long value = std::strtoll(v->c_str(), &end, 10);
        if (errno != 0 || end == v->c_str() || *end != '\0') {
            throw ConfigError(std::string(key) + " must be an integer, got: " + *v);
        }
        return static_cast<int64_t>(value);
    }

    std::optional<double> f64(const char* key) const {
        auto v = str(key);
        if (!v) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(v->c_str(), &end);
        if (errno != 0 || end == v->c_str() || *end != '\0') {
            throw ConfigError(std::string(key) + " must be a number, got: " + *v);
        }
        return value;
    }

    // Integer in [min, max], or `fallback` when unset.
    int ranged(const char* key, int fallback, int min, int max) const {
        auto v = i64(key);
        if (!v) return fallback;
        if (*v < min || *v > max) {
            throw ConfigError(std::string(key) + " must be in [" + std::to_string(min) + ", " +
                              std::to_string(max) + "], got: " + std::to_string(*v));
        }
        return static_cast<int>(*v);
    }

private:
    const std::map<std::string, std::string>& file_;
};

}  // namespace

Config from_env(const std::string& dotenv_path) {
    const auto dotenv = load_dotenv(dotenv_path);
    const ValueSource src(dotenv);

    Config cfg;

    auto token = src.str("BOT_TOKEN");
    if (!token) throw ConfigError("BOT_TOKEN is not set (see .env.example)");
    cfg.bot_token = *token;

    if (auto v = src.i64("BOT_OWNER_ID")) cfg.bot_owner_id = *v;
    if (auto v = src.str("DB_PATH")) cfg.db_path = *v;
    if (auto v = src.str("DEFAULT_LANG")) cfg.default_lang = *v;
    if (auto v = src.str("LOG_LEVEL")) cfg.log_level = *v;
    if (auto v = src.str("I18N_DIR")) cfg.i18n_dir = *v;
    if (auto v = src.str("WELCOME_MESSAGE")) cfg.welcome_message_path = *v;
    if (auto v = src.i64("POLL_TIMEOUT")) {
        if (*v < 0 || *v > 1000000) {
            throw ConfigError("POLL_TIMEOUT is out of range: " + std::to_string(*v));
        }
        cfg.poll_timeout = static_cast<int>(*v);
    }

    AiConfig& ai = cfg.ai;
    if (auto v = src.str("AI_API_URL")) ai.api_url = *v;
    if (auto v = src.str("AI_API_KEY")) ai.api_key = *v;
    if (auto v = src.str("AI_MODEL")) ai.model = *v;
    if (auto v = src.str("AI_RULES")) ai.rules = *v;
    if (auto v = src.str("AI_BIO")) ai.bio = *v;
    ai.max_tokens = src.ranged("AI_MAX_TOKENS", ai.max_tokens, 1, 100000);
    ai.temperature = src.f64("AI_TEMPERATURE");
    ai.top_p = src.f64("AI_TOP_P");
    ai.frequency_penalty = src.f64("AI_FREQUENCY_PENALTY");
    ai.presence_penalty = src.f64("AI_PRESENCE_PENALTY");
    ai.timeout_s = src.ranged("AI_TIMEOUT", ai.timeout_s, 1, 600);
    ai.user_cooldown_s = src.ranged("AI_USER_COOLDOWN", ai.user_cooldown_s, 0, 86400);
    ai.chat_per_min = src.ranged("AI_CHAT_PER_MIN", ai.chat_per_min, 1, 1000);
    ai.queue_limit = src.ranged("AI_QUEUE_LIMIT", ai.queue_limit, 1, 10000);

    return cfg;
}

}  // namespace yacm::config