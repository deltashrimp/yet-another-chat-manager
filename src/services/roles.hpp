#pragma once

#include "config/config.hpp"
#include "storage/repos.hpp"
#include "telegram/client.hpp"
#include "telegram/types.hpp"

#include <cstdint>

namespace yacm::services {

enum class Role {
    Everyone = 0,
    Admin = 1,
    Owner = 2,
    BotOwner = 3,
};

int role_rank(Role role);
// true when `actual` satisfies the `required` level.
bool meets(Role required, Role actual);

// Resolves a user's role in a chat. bot_owner is global. For group chats
// the `members` DB cache is consulted first (populated by the update
// processor and by write-through below); only on a cache miss is a live
// getChatMember issued, and its result is cached.
class Roles {
public:
    Roles(const config::Config& config, tg::TelegramClient& client, storage::UserRepo& users);

    // Cached member statuses older than this are re-validated live.
    static constexpr int64_t kCacheTtlSeconds = 10 * 60;

    Role resolve(int64_t chat_id, const tg::User& user) const;

    // "creator" -> Owner, "administrator" -> Admin, everything else ->
    // Everyone (member/restricted/left/kicked are never admins).
    static Role role_from_status(const std::string& status);

private:
    const config::Config& config_;
    tg::TelegramClient& client_;
    storage::UserRepo& users_;
};

// Spec §2 hierarchy restrictions for moderation actions. Returns an i18n
// error key ("errors.cannot_restrict_*") when the target may not be
// moderated, or an empty string when the target is fair game.
std::string moderation_block(int64_t target_id, int64_t bot_id, Role target_role);

}  // namespace yacm::services