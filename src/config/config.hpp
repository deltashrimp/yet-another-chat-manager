#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace yacm::config {

// AI assistant (OpenAI-compatible chat-completions API, e.g. Groq,
// OpenRouter, OpenAI, a local llama.cpp / Ollama server). Disabled while
// api_key is empty.
struct AiConfig {
    std::string api_url = "https://api.groq.com/openai/v1/chat/completions";
    std::string api_key;
    std::string model = "openai/gpt-oss-120b";
    // Inserted into the system prompt. Literal text or "file:/path" (the
    // file is re-read on /aireload).
    std::string rules;
    std::string bio;
    int max_tokens = 1000;
    // Sampling parameters are only sent when set (provider defaults otherwise).
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<double> frequency_penalty;
    std::optional<double> presence_penalty;
    int timeout_s = 60;
    int user_cooldown_s = 10;   // min seconds between AI requests of one user
    int chat_per_min = 10;      // max AI requests per chat per minute
    int queue_limit = 50;       // pending requests before "busy" is answered

    [[nodiscard]] bool enabled() const { return !api_key.empty() && !api_url.empty(); }
};

struct Config {
    std::string bot_token;
    int64_t bot_owner_id = 0;
    // Local default; Docker deployments should override with /data/bot.db.
    std::string db_path = "./data/bot.db";
    std::string default_lang = "en";
    std::string log_level = "info";
    std::string i18n_dir = "./i18n";
    std::string welcome_message_path = "./resources/welcome-message-public.md";
    int poll_timeout = 30;
    AiConfig ai;
};

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Reads configuration from environment variables and, as a fallback,
// from a KEY=VALUE file (`.env` by default). Process environment always
// takes precedence over file values. See .env.example.
// Throws ConfigError when required values are missing or invalid.
Config from_env(const std::string& dotenv_path = "./.env");

}  // namespace yacm::config