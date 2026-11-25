#include "services/moderation.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <nlohmann/json.hpp>

namespace yacm::services {
namespace {

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A complete mute: no messages/media/polls at all.
const nlohmann::json kMutePermissions = {
    {"can_send_messages", false},
};

// Restores full sending rights on /unmute.
const nlohmann::json kUnmutePermissions = {
    {"can_send_messages", true},
    {"can_send_audios", true},
    {"can_send_documents", true},
    {"can_send_photos", true},
    {"can_send_videos", true},
    {"can_send_video_notes", true},
    {"can_send_voice_notes", true},
    {"can_send_polls", true},
    {"can_send_other_messages", true},
    {"can_add_web_page_previews", true},
    {"can_change_info", true},
    {"can_invite_users", true},
    {"can_pin_messages", true},
};

}  // namespace

ModerationService::ModerationService(tg::TelegramClient& client, int64_t bot_id,
                                     storage::Database& db, storage::MuteRepo& mutes,
                                     storage::BanRepo& bans, storage::AuditRepo& audit)
    : client_(client), bot_id_(bot_id), db_(db), mutes_(mutes), bans_(bans), audit_(audit) {}

void ModerationService::check_target(int64_t chat_id, int64_t target_id, Role target_role,
                                     ModerationResult& result) const {
    result = ModerationResult{};
    const std::string block = moderation_block(target_id, bot_id_, target_role);
    if (!block.empty()) {
        result.error_key = block;
        return;
    }
    if (!bot_is_admin(chat_id)) {
        result.error_key = "errors.not_enough_rights";
        return;
    }
    result.ok = true;
}

bool ModerationService::bot_is_admin(int64_t chat_id) const {
    try {
        const tg::ChatMember me = client_.get_chat_member(chat_id, bot_id_);
        return me.status == "administrator" || me.status == "creator";
    } catch (const std::exception& e) {
        spdlog::warn("cannot check bot rights in {}: {}", chat_id, e.what());
        return false;  // conservative: assume no rights
    }
}

ModerationResult ModerationService::ban(int64_t chat_id, int64_t actor_id, int64_t target_id,
                                        Role target_role, const std::string& reason) {
    ModerationResult r;
    check_target(chat_id, target_id, target_role, r);
    if (!r.ok) return r;

    try {
        client_.ban_chat_member(chat_id, target_id, reason);
    } catch (const std::exception& e) {
        spdlog::warn("banChatMember failed for {} in {}: {}", target_id, chat_id, e.what());
        r.ok = false;
        r.error_key = "errors.moderation_failed";
        return r;
    }

    const int64_t now = unix_now();
    storage::Transaction tx(db_);
    bans_.set(chat_id, target_id, reason, actor_id, now);
    audit_.log(chat_id, actor_id, "ban", target_id, reason, now);
    tx.commit();
    return r;
}

ModerationResult ModerationService::unban(int64_t chat_id, int64_t actor_id,
                                          int64_t target_id, Role target_role) {
    ModerationResult r;
    check_target(chat_id, target_id, target_role, r);
    if (!r.ok) return r;

    try {
        client_.unban_chat_member(chat_id, target_id);
    } catch (const std::exception& e) {
        spdlog::warn("unbanChatMember failed for {} in {}: {}", target_id, chat_id, e.what());
        r.ok = false;
        r.error_key = "errors.moderation_failed";
        return r;
    }

    const int64_t now = unix_now();
    storage::Transaction tx(db_);
    bans_.clear(chat_id, target_id);
    audit_.log(chat_id, actor_id, "unban", target_id, "", now);
    tx.commit();
    return r;
}

ModerationResult ModerationService::kick(int64_t chat_id, int64_t actor_id, int64_t target_id,
                                         Role target_role, const std::string& reason) {
    ModerationResult r;
    check_target(chat_id, target_id, target_role, r);
    if (!r.ok) return r;

    try {
        client_.ban_chat_member(chat_id, target_id, reason);
        client_.unban_chat_member(chat_id, target_id);  // kick, not a lasting ban
    } catch (const std::exception& e) {
        spdlog::warn("kick failed for {} in {}: {}", target_id, chat_id, e.what());
        r.ok = false;
        r.error_key = "errors.moderation_failed";
        return r;
    }

    const int64_t now = unix_now();
    storage::Transaction tx(db_);
    audit_.log(chat_id, actor_id, "kick", target_id, reason, now);
    tx.commit();
    return r;
}

ModerationResult ModerationService::mute(int64_t chat_id, int64_t actor_id, int64_t target_id,
                                         Role target_role, int64_t until_s,
                                         const std::string& reason) {
    if (until_s < 0) until_s = 0;
    ModerationResult r;
    check_target(chat_id, target_id, target_role, r);
    if (!r.ok) return r;

    if (until_s > kMaxMuteSeconds) {
        // Telegram silently turns such a restriction into a permanent one.
        r.ok = false;
        r.error_key = "errors.invalid_duration";
        return r;
    }
    // Telegram also treats "less than 30 seconds from now" as forever; pad
    // short mutes so request latency can't turn "/mute 30s" into a ban on
    // speaking for life.
    if (until_s > 0 && until_s < kMinMuteSeconds) until_s = kMinMuteSeconds;

    const int64_t now = unix_now();
    const int64_t until = until_s > 0 ? now + until_s : 0;  // absolute; 0 = forever
    try {
        client_.restrict_chat_member(chat_id, target_id, static_cast<long>(until),
                                     kMutePermissions);
    } catch (const std::exception& e) {
        spdlog::warn("restrictChatMember (mute) failed for {} in {}: {}", target_id, chat_id,
                     e.what());
        r.ok = false;
        r.error_key = "errors.moderation_failed";
        return r;
    }

    storage::Transaction tx(db_);
    mutes_.set(chat_id, target_id, until, reason, actor_id, now);
    audit_.log(chat_id, actor_id, "mute", target_id, reason, now);
    tx.commit();
    return r;
}

ModerationResult ModerationService::unmute(int64_t chat_id, int64_t actor_id,
                                           int64_t target_id, Role target_role) {
    ModerationResult r;
    check_target(chat_id, target_id, target_role, r);
    if (!r.ok) return r;

    try {
        client_.restrict_chat_member(chat_id, target_id, 0, kUnmutePermissions);
    } catch (const std::exception& e) {
        spdlog::warn("restrictChatMember (unmute) failed for {} in {}: {}", target_id, chat_id,
                     e.what());
        r.ok = false;
        r.error_key = "errors.moderation_failed";
        return r;
    }

    const int64_t now = unix_now();
    storage::Transaction tx(db_);
    mutes_.clear(chat_id, target_id);
    audit_.log(chat_id, actor_id, "unmute", target_id, "", now);
    tx.commit();
    return r;
}

}  // namespace yacm::services