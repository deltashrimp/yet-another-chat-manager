#pragma once

#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "storage/db.hpp"
#include "storage/repos.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace yacm::services {

// Outcome of a warn / unwarn action.
struct WarnOutcome {
    bool ok = false;
    std::string error_key;          // i18n key when !ok
    int count = 0;                  // warns the user has now (0 after an auto-action)
    int limit = 0;                  // chat warn limit (default 3)
    // When the warn pushed the user to the limit, the chat's configured
    // action ("ban" | "kick" | "mute") fired and the warns were reset to 0.
    std::string fired_action;
};

// /warn /unwarn /warnlist business logic (spec §5.5): hierarchy check,
// transactional DB writes + audit log. Warns carry no Telegram rights
// requirement (they are a bot-internal counter). Reaching the chat's warn
// limit triggers the configured automatic action (spec §5.6) and resets
// the user's warns.
class WarnService {
public:
    WarnService(storage::Database& db, storage::ChatRepo& chats, storage::WarnRepo& warns,
                storage::AuditRepo& audit, int64_t bot_id,
                services::ModerationService& moderation);

    WarnOutcome warn(int64_t chat_id, int64_t actor_id, int64_t target_id, Role target_role,
                     const std::string& reason);
    WarnOutcome unwarn(int64_t chat_id, int64_t actor_id, int64_t target_id,
                       Role target_role);

    int count(int64_t chat_id, int64_t target_id) const;
    int warn_limit(int64_t chat_id) const;
    std::vector<storage::WarnRow> list(int64_t chat_id, int64_t target_id) const;

private:
    // Fires the configured warn-limit action; returns it via out.
    ModerationResult apply_limit_action(int64_t chat_id, int64_t actor_id, int64_t target_id,
                                        Role target_role, const std::string& action,
                                        const std::string& reason) const;

    storage::Database& db_;
    storage::ChatRepo& chats_;
    storage::WarnRepo& warns_;
    storage::AuditRepo& audit_;
    int64_t bot_id_;
    services::ModerationService& moderation_;
};

}  // namespace yacm::services