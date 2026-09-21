#include "app/app_context.hpp"

#include "handlers/general.hpp"
#include "handlers/moderation.hpp"
#include "handlers/settings.hpp"
#include "version.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace yacm::app {
namespace {

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

AppContext::AppContext(config::Config cfg, std::shared_ptr<spdlog::logger> logger,
                       std::atomic<bool>& stop_flag)
    : config(std::move(cfg)),
      log(std::move(logger)),
      i18n(config.default_lang),
      db(config.db_path),
      version(YACM_VERSION),
      build_date(YACM_BUILD_DATE),
      stop_flag_(stop_flag) {}

AppContext::~AppContext() { shutdown(); }

void AppContext::init() {
    db.open();
    db.migrate();
    i18n.load_dir(config.i18n_dir);

    // A pre-injected client (tests) is respected; production builds the
    // real libcurl client.
    if (!client) {
        client = std::make_unique<tg::Client>(config.bot_token);
    }
    sender = std::make_unique<tg::Sender>(*client, log);
    users = std::make_unique<storage::UserRepo>(db);
    chats = std::make_unique<storage::ChatRepo>(db);
    warns = std::make_unique<storage::WarnRepo>(db);
    mutes = std::make_unique<storage::MuteRepo>(db);
    bans = std::make_unique<storage::BanRepo>(db);
    reports = std::make_unique<storage::ReportRepo>(db);
    answers = std::make_unique<storage::AnswerRepo>(db);
    audit = std::make_unique<storage::AuditRepo>(db);
    roles = std::make_unique<services::Roles>(config, *client, *users);
    bot_id = client->get_me().id;
    moderation = std::make_unique<services::ModerationService>(*client, bot_id, db, *mutes,
                                                               *bans, *audit);
    warn_service = std::make_unique<services::WarnService>(db, *chats, *warns, *audit, bot_id,
                                                           *moderation);

    welcome_message = read_file(config.welcome_message_path);
    if (welcome_message.empty()) {
        log->warn("welcome message file not found: {} (falling back to default)",
                  config.welcome_message_path);
        welcome_message = "Welcome!";
    }

    handlers::register_general(*this);
    handlers::register_moderation(*this);
    handlers::register_settings(*this);

    sender->start();
    log->info("ready: version={} build={} db={}", version, build_date, config.db_path);
}

void AppContext::run() {
    while (!stop_flag_.load()) {
        try {
            auto updates = client->get_updates(update_offset_, config.poll_timeout);
            for (auto& update : updates) {
                update_offset_ = std::max(update_offset_, update.update_id + 1);
                try {
                    process_update(update);
                } catch (const std::exception& e) {
                    log->error("update #{} failed: {}", update.update_id, e.what());
                }
            }
        } catch (const tg::TelegramError& e) {
            if (e.is_flood()) {
                const long wait = e.retry_after + 1;
                log->warn("getUpdates flood wait {}s", wait);
                std::this_thread::sleep_for(std::chrono::seconds(wait));
            } else {
                log->warn("getUpdates error: code={} desc={}", e.error_code, e.what());
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } catch (const std::exception& e) {
            // Transport-level failure (e.g. "curl error: SSL connect error"):
            // transient by nature, the loop simply retries after a backoff.
            log->warn("getUpdates network error (retrying in 5s): {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    log->info("poll loop exited");
}

void AppContext::process_update(const tg::Update& update) {
    if (update.message) {
        const tg::Message& m = *update.message;
        if (m.chat) {
            chats->ensure(*m.chat, unix_now(), config.default_lang);
            if (m.from) {
                users->upsert(*m.from, unix_now());
                const bool is_new_join = !m.new_chat_members.empty();
                users->upsert_member(m.chat->id, m.from->id, "member", unix_now(),
                                     is_new_join ? std::optional<int64_t>(unix_now())
                                                 : std::nullopt);
            }
            for (const auto& member : m.new_chat_members) {
                users->upsert(member, unix_now());
                users->upsert_member(m.chat->id, member.id, "member", unix_now(), unix_now());
            }
            if (m.left_chat_member) {
                users->upsert_member(m.chat->id, m.left_chat_member->id, "left", unix_now(),
                                     std::nullopt);
            }
        }
        if (m.is_command()) {
            // Unknown / malformed commands are silently ignored.
            router.dispatch(m);
        }
    }

    if (update.chat_member) {
        const auto& cm = *update.chat_member;
        if (cm.chat) {
            if (cm.from) users->upsert(*cm.from, unix_now());
            users->upsert_member(cm.chat->id, cm.user_id, cm.new_status, unix_now(),
                                 cm.new_status == "member" || cm.new_status == "administrator"
                                     ? std::optional<int64_t>(unix_now())
                                     : std::nullopt);
        }
    }
}

void AppContext::shutdown() {
    if (shutdown_done_) return;
    shutdown_done_ = true;

    stop_flag_.store(true);
    if (sender) sender->shutdown();
    db.close();
    log->info("shutdown complete");
}

}  // namespace yacm::app