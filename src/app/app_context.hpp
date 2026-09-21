#pragma once

#include "config/config.hpp"
#include "i18n/strings.hpp"
#include "router/router.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "services/warns.hpp"
#include "storage/db.hpp"
#include "storage/repos.hpp"
#include "telegram/client.hpp"
#include "telegram/sender.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace yacm::app {

// Composition root (manual DI container). Owns every component of the
// monolith and wires them together in init().
class AppContext {
public:
    AppContext(config::Config config, std::shared_ptr<spdlog::logger> log,
               std::atomic<bool>& stop_flag);
    ~AppContext();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    // Opens the database, applies migrations, starts the sender and
    // registers command handlers.
    void init();

    // Long-polling loop. Blocks until shutdown() is requested via signal.
    void run();

    // Stops polling, drains the outbound queue, closes the database.
    void shutdown();

    config::Config config;
    std::shared_ptr<spdlog::logger> log;
    i18n::Strings i18n;
    storage::Database db;
    std::unique_ptr<tg::TelegramClient> client;
    std::unique_ptr<tg::Sender> sender;
    std::unique_ptr<storage::UserRepo> users;
    std::unique_ptr<storage::ChatRepo> chats;
    std::unique_ptr<storage::WarnRepo> warns;
    std::unique_ptr<storage::MuteRepo> mutes;
    std::unique_ptr<storage::BanRepo> bans;
    std::unique_ptr<storage::ReportRepo> reports;
    std::unique_ptr<storage::AnswerRepo> answers;
    std::unique_ptr<storage::AuditRepo> audit;
    std::unique_ptr<services::Roles> roles;
    std::unique_ptr<services::ModerationService> moderation;
    std::unique_ptr<services::WarnService> warn_service;
    router::HandlerRegistry router;
    std::string welcome_message;
    std::string version;
    std::string build_date;
    int64_t bot_id = 0;  // own user id (for "cannot moderate the bot" checks)

private:
    void process_update(const tg::Update& update);

    std::atomic<bool>& stop_flag_;
    bool shutdown_done_ = false;
    int64_t update_offset_ = 0;
};

}  // namespace yacm::app