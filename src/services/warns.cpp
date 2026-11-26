#include "services/warns.hpp"

#include <chrono>

namespace yacm::services {
namespace {

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

WarnService::WarnService(storage::Database& db, storage::ChatRepo& chats,
                         storage::WarnRepo& warns, storage::AuditRepo& audit, int64_t bot_id,
                         services::ModerationService& moderation)
    : db_(db), chats_(chats), warns_(warns), audit_(audit), bot_id_(bot_id),
      moderation_(moderation) {}

WarnOutcome WarnService::warn(int64_t chat_id, int64_t actor_id, int64_t target_id,
                              Role target_role, const std::string& reason) {
    WarnOutcome out;
    const std::string block = moderation_block(target_id, bot_id_, target_role);
    if (!block.empty()) {
        out.error_key = block;
        return out;
    }

    const int64_t now = unix_now();
    {
        storage::Transaction tx(db_);
        warns_.add(chat_id, target_id, reason, actor_id, now);
        audit_.log(chat_id, actor_id, "warn", target_id, reason, now);
        tx.commit();
    }

    out.ok = true;
    out.count = warns_.count(chat_id, target_id);
    out.limit = chats_.warn_limit(chat_id);
    if (out.count < out.limit) return out;

    // Limit reached: run the chat's configured action (spec §5.6), then
    // reset the warns so the user is not left "overloaded" after the
    // punishment expires.
    const util::Action action =
        util::action_from_db(chats_.warn_action(chat_id), chats_.warn_action_duration(chat_id));
    const ModerationResult ar = apply_limit_action(chat_id, actor_id, target_id, target_role,
                                                   action, reason);
    if (!ar.ok) {
        // The warn stays on record; the moderator sees why the action failed
        // (e.g. not enough rights), and the next /warn will retry the action.
        out.ok = false;
        out.error_key = ar.error_key;
        return out;
    }

    storage::Transaction tx(db_);
    warns_.clear(chat_id, target_id);
    audit_.log(chat_id, actor_id, "warn_reset", target_id, action.describe(), unix_now());
    tx.commit();

    out.count = 0;
    out.fired_action = action.name();
    out.fired_duration = action.duration_s;
    return out;
}

WarnOutcome WarnService::reset(int64_t chat_id, int64_t actor_id, int64_t target_id,
                               Role target_role) {
    WarnOutcome out;
    const std::string block = moderation_block(target_id, bot_id_, target_role);
    if (!block.empty()) {
        out.error_key = block;
        return out;
    }

    storage::Transaction tx(db_);
    out.removed = warns_.clear(chat_id, target_id);
    audit_.log(chat_id, actor_id, "resetwarn", target_id, std::to_string(out.removed),
               unix_now());
    tx.commit();

    out.ok = true;
    out.count = 0;
    out.limit = chats_.warn_limit(chat_id);
    return out;
}

WarnOutcome WarnService::unwarn(int64_t chat_id, int64_t actor_id, int64_t target_id,
                                Role target_role) {
    WarnOutcome out;
    const std::string block = moderation_block(target_id, bot_id_, target_role);
    if (!block.empty()) {
        out.error_key = block;
        return out;
    }

    const int64_t now = unix_now();
    storage::Transaction tx(db_);
    warns_.remove_last(chat_id, target_id);
    audit_.log(chat_id, actor_id, "unwarn", target_id, "", now);
    tx.commit();

    out.ok = true;
    out.count = warns_.count(chat_id, target_id);
    out.limit = chats_.warn_limit(chat_id);
    return out;
}

int WarnService::count(int64_t chat_id, int64_t target_id) const {
    return warns_.count(chat_id, target_id);
}

ModerationResult WarnService::apply_limit_action(int64_t chat_id, int64_t actor_id,
                                                 int64_t target_id, Role target_role,
                                                 const util::Action& action,
                                                 const std::string& reason) const {
    return moderation_.apply(chat_id, actor_id, target_id, target_role, action, reason);
}

int WarnService::warn_limit(int64_t chat_id) const { return chats_.warn_limit(chat_id); }

std::vector<storage::WarnRow> WarnService::list(int64_t chat_id, int64_t target_id) const {
    return warns_.list(chat_id, target_id);
}

}  // namespace yacm::services