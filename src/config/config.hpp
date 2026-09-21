#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace yacm::config {

struct Config {
    std::string bot_token;
    int64_t bot_owner_id = 0;
    // Local default; Docker deployments should override with /data/bot.db.
    std::string db_path = "./data/bot.db";
    std::string default_lang = "en";
    std::string log_level = "info";
    std::string i18n_dir = "./i18n";
    std::string welcome_message_path = "./resources/welcome-message.md";
    int poll_timeout = 30;
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