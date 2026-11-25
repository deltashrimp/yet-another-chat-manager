#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "telegram/types.hpp"

namespace yacm::storage {

class Database;

struct UserRow {
    int64_t id = 0;
    std::string username;
    std::string first_name;
    std::string last_name;
    bool is_bot = false;
    int64_t first_seen_at = 0;
};

// A cached `members` row: status plus when it was last confirmed.
struct MemberEntry {
    std::string status;
    int64_t updated_at = 0;
};

class UserRepo {
public:
    explicit UserRepo(Database& db);

    // Inserts or refreshes a user seen in an update.
    void upsert(const tg::User& user, int64_t now);

    std::optional<UserRow> find_by_username(const std::string& username);
    std::optional<UserRow> find_by_id(int64_t id);

    // Tracks member presence/permissions in a chat (cache for role checks).
    void upsert_member(int64_t chat_id, int64_t user_id, const std::string& status,
                       int64_t now, std::optional<int64_t> joined_at);

    // Cached status of a member in a chat ("creator" | "administrator" |
    // "member" | "restricted" | "left" | "kicked"), or nullopt when the bot
    // has never seen that member in the chat.
    std::optional<std::string> member_status(int64_t chat_id, int64_t user_id) const;

    // Same as member_status(), plus the time the status was last written.
    std::optional<MemberEntry> member_entry(int64_t chat_id, int64_t user_id) const;

    // The user just posted in the chat: a cached "left"/"kicked" row flips
    // back to "member". Any other cached status (notably creator/admin) is
    // left untouched and no row is created for an unknown user.
    void mark_member_active(int64_t chat_id, int64_t user_id, int64_t now);

    // Users cached as admins (creator/administrator) of a chat, creator
    // first. Used as a fallback when the live administrator list is
    // unavailable.
    std::vector<UserRow> admins(int64_t chat_id) const;

private:
    Database& db_;
};

// Snapshot of per-chat settings (steps 4-8 read most of these).
struct ChatSettings {
    std::string language = "en";
    std::string rules_html;  // empty = not set
    bool reports_enabled = true;
    int warn_limit = 3;
    std::string warn_action = "mute";
};

class ChatRepo {
public:
    explicit ChatRepo(Database& db);

    // Inserts or updates chat metadata (title, type, language).
    void ensure(const tg::Chat& chat, int64_t now, const std::string& default_lang);

    // Returns "" when the chat has no stored language.
    std::string language(int64_t chat_id) const;
    void set_language(int64_t chat_id, const std::string& language);

    void set_rules(int64_t chat_id, const std::string& rules_html);
    void clear_rules(int64_t chat_id);
    bool has_rules(int64_t chat_id) const;
    // Rules HTML, "" when not set.
    std::string rules_text(int64_t chat_id) const;

    void set_reports_enabled(int64_t chat_id, bool enabled);
    bool reports_enabled(int64_t chat_id) const;

    void set_warn_limit(int64_t chat_id, int limit);
    void set_warn_action(int64_t chat_id, const std::string& action);
    int warn_limit(int64_t chat_id) const;
    std::string warn_action(int64_t chat_id) const;

    // All settings at once; schema defaults for a chat the bot has not seen.
    ChatSettings settings(int64_t chat_id) const;

private:
    Database& db_;
};

// A single warn record (for /warnlist and /info).
struct WarnRow {
    int64_t id = 0;
    int64_t user_id = 0;
    std::string reason;
    int64_t created_at = 0;
};

class WarnRepo {
public:
    explicit WarnRepo(Database& db);

    void add(int64_t chat_id, int64_t user_id, const std::string& reason,
             int64_t created_by, int64_t created_at);

    // Removes the most recent warn of the user; false when there were none.
    bool remove_last(int64_t chat_id, int64_t user_id);

    // Warns of a user, newest first.
    std::vector<WarnRow> list(int64_t chat_id, int64_t user_id) const;

    int count(int64_t chat_id, int64_t user_id) const;

    // Removes all warns (warn-limit reset in step 4).
    void clear(int64_t chat_id, int64_t user_id);

private:
    Database& db_;
};

// Active mutes, one row per (chat, user): `until` 0 = forever.
class MuteRepo {
public:
    explicit MuteRepo(Database& db);

    // Replaces any previous active mute of the user.
    void set(int64_t chat_id, int64_t user_id, int64_t until, const std::string& reason,
             int64_t created_by, int64_t created_at);
    void clear(int64_t chat_id, int64_t user_id);

    // Until timestamp (0 = forever); nullopt when not muted.
    std::optional<int64_t> until(int64_t chat_id, int64_t user_id) const;

private:
    Database& db_;
};

// Active bans, one row per (chat, user).
class BanRepo {
public:
    explicit BanRepo(Database& db);

    void set(int64_t chat_id, int64_t user_id, const std::string& reason,
             int64_t created_by, int64_t created_at);
    void clear(int64_t chat_id, int64_t user_id);

    // Ban reason; nullopt when the user is not banned.
    std::optional<std::string> reason(int64_t chat_id, int64_t user_id) const;

private:
    Database& db_;
};

// Reports of messages (step 6). Spec §3: one report per (reporter, message)
// — a repeated report of the same message by the same user is ignored.
// The table's UNIQUE (chat_id, reporter_id, message_id) constraint enforces
// that (message ids are only unique within one chat).
class ReportRepo {
public:
    explicit ReportRepo(Database& db);

    // Records a report. No-op when the reporter already reported the
    // message (INSERT OR IGNORE; the UNIQUE constraint is the backstop).
    void add(int64_t chat_id, int64_t message_id, int64_t reporter_id,
             int64_t target_id, int64_t created_at);

    // True when the reporter already reported this message of this chat.
    bool already_reported(int64_t chat_id, int64_t reporter_id, int64_t message_id) const;

    // True when the reporter already reported this target user in this
    // chat (any message) — blocks spam re-reports about the same user.
    bool already_reported_target(int64_t chat_id, int64_t reporter_id,
                                 int64_t target_id) const;

private:
    Database& db_;
};

// Per-chat, per-command "success answers" toggles (step 8, spec §6.2).
// An absent row means answers are enabled for that command.
class AnswerRepo {
public:
    explicit AnswerRepo(Database& db);

    void set(int64_t chat_id, const std::string& command, bool enabled);

    // True unless the chat explicitly disabled answers for this command.
    bool enabled(int64_t chat_id, const std::string& command) const;

private:
    Database& db_;
};

class AuditRepo {
public:
    explicit AuditRepo(Database& db);

    void log(int64_t chat_id, int64_t actor_id, const std::string& action,
             std::optional<int64_t> target_id, const std::string& details,
             int64_t created_at);

private:
    Database& db_;
};

}  // namespace yacm::storage