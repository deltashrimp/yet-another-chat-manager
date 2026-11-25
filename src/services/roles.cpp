#include "services/roles.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace yacm::services {
namespace {

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int role_rank(Role role) { return static_cast<int>(role); }

bool meets(Role required, Role actual) { return role_rank(required) <= role_rank(actual); }

Roles::Roles(const config::Config& config, tg::TelegramClient& client, storage::UserRepo& users)
    : config_(config), client_(client), users_(users) {}

Role Roles::role_from_status(const std::string& status) {
    if (status == "creator") return Role::Owner;
    if (status == "administrator") return Role::Admin;
    return Role::Everyone;
}

std::string moderation_block(int64_t target_id, int64_t bot_id, Role target_role) {
    if (target_id == bot_id) return "errors.cannot_restrict_bot";
    if (target_role == Role::Owner) return "errors.cannot_restrict_owner";
    if (target_role == Role::Admin) return "errors.cannot_restrict_admin";
    return "";
}

Role Roles::resolve(int64_t chat_id, const tg::User& user) const {
    if (config_.bot_owner_id != 0 && user.id == config_.bot_owner_id) {
        return Role::BotOwner;
    }
    if (chat_id >= 0) return Role::Everyone;  // private chat

    const int64_t now = unix_now();
    const auto cached = users_.member_entry(chat_id, user.id);
    // A fresh cache entry is trusted as-is. Stale entries are re-checked:
    // promotions/demotions are only pushed to the bot (chat_member updates)
    // while it is an admin, so an old "administrator" row could otherwise
    // keep granting rights to someone who was demoted long ago.
    if (cached && now - cached->updated_at < kCacheTtlSeconds) {
        return role_from_status(cached->status);
    }

    // Cache miss / stale -> live call, write-through to the cache so the
    // next check (and any other command in this chat) hits the DB.
    try {
        const tg::ChatMember member = client_.get_chat_member(chat_id, user.id);
        users_.upsert_member(chat_id, user.id, member.status, now, std::nullopt);
        return role_from_status(member.status);
    } catch (const std::exception& e) {
        spdlog::debug("getChatMember failed for {} in {}: {}", user.id, chat_id, e.what());
    }
    // Fail closed: a stale privileged entry is not trusted when it cannot
    // be re-validated (the admin can simply retry the command).
    return Role::Everyone;
}

}  // namespace yacm::services