#pragma once

#include "services/roles.hpp"
#include "storage/db.hpp"
#include "storage/repos.hpp"
#include "telegram/client.hpp"
#include "util/parsers.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace yacm::services {

// Outcome of a moderation action: ok, or an i18n error key to report.
struct ModerationResult {
    bool ok = false;
    std::string error_key;  // i18n key when !ok
};

// Business logic for ban/unban/kick/mute/unmute (spec §5.2-5.4): §2
// hierarchy checks, bot-rights check, live Telegram calls, transactional
// DB writes and the audit log.
class ModerationService {
public:
    ModerationService(tg::TelegramClient& client, int64_t bot_id, storage::Database& db,
                      storage::MuteRepo& mutes, storage::BanRepo& bans,
                      storage::AuditRepo& audit);

    // Spec §2: cannot touch the chat owner, other admins or the bot itself;
    // the bot must be an administrator of the chat.
    // Fills result.error_key on failure.
    void check_target(int64_t chat_id, int64_t target_id, Role target_role,
                      ModerationResult& result) const;

    // duration_s: 0 = permanent ban, otherwise a temporary ban (tban) with
    // the same bounds as mutes.
    ModerationResult ban(int64_t chat_id, int64_t actor_id, int64_t target_id,
                         Role target_role, const std::string& reason, int64_t duration_s = 0);
    ModerationResult unban(int64_t chat_id, int64_t actor_id, int64_t target_id,
                           Role target_role);
    ModerationResult kick(int64_t chat_id, int64_t actor_id, int64_t target_id,
                          Role target_role, const std::string& reason);
    // Telegram treats restrictions shorter than 30 s or longer than 366 days
    // as permanent: longer mutes are rejected (errors.invalid_duration),
    // shorter ones are padded up to kMinMuteSeconds.
    static constexpr int64_t kMinMuteSeconds = 35;
    static constexpr int64_t kMaxMuteSeconds = 366LL * 24 * 3600;

    // until_s: duration in seconds, 0 = forever (until /unmute). The mutes
    // table stores the absolute expiry timestamp (0 = forever).
    ModerationResult mute(int64_t chat_id, int64_t actor_id, int64_t target_id,
                          Role target_role, int64_t until_s, const std::string& reason);
    ModerationResult unmute(int64_t chat_id, int64_t actor_id, int64_t target_id,
                            Role target_role);

    // Runs an automatic punishment (warn limit, antiflood).
    ModerationResult apply(int64_t chat_id, int64_t actor_id, int64_t target_id,
                           Role target_role, const util::Action& action,
                           const std::string& reason);

    // Clamps a temporary restriction to what Telegram honours: > max is
    // rejected (nullopt), shorter than min is padded. 0 stays 0 (forever).
    static std::optional<int64_t> clamp_duration(int64_t seconds);

private:
    bool bot_is_admin(int64_t chat_id) const;

    tg::TelegramClient& client_;
    int64_t bot_id_;
    storage::Database& db_;
    storage::MuteRepo& mutes_;
    storage::BanRepo& bans_;
    storage::AuditRepo& audit_;
};

}  // namespace yacm::services