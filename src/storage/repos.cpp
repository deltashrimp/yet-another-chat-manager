#include "storage/repos.hpp"

#include "storage/db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>

namespace yacm::storage {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

UserRow row_from_statement(const Statement& st) {
    UserRow row;
    row.id = st.col_i64(0);
    row.username = st.col_str(1);
    row.first_name = st.col_str(2);
    row.last_name = st.col_str(3);
    row.is_bot = st.col_bool(4);
    row.first_seen_at = st.col_i64(5);
    return row;
}

}  // namespace

UserRepo::UserRepo(Database& db) : db_(db) {}

void UserRepo::upsert(const tg::User& user, int64_t now) {
    // usernames are unique (case-insensitive index). Telegram usernames can
    // move between accounts, so a stale row still holding this username
    // must release it first — otherwise the upsert hits the UNIQUE index
    // and every update from the new owner fails.
    if (!user.username.empty()) {
        Statement free_name(db_.raw(),
                            "UPDATE users SET username = NULL "
                            "WHERE lower(username) = ?1 AND id != ?2");
        free_name.bind(1, lower(user.username));
        free_name.bind(2, user.id);
        free_name.step();
    }

    Statement st(db_.raw(),
                 "INSERT INTO users (id, username, first_name, last_name, is_bot, "
                 "first_seen_at, updated_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6) "
                 "ON CONFLICT(id) DO UPDATE SET "
                 " username = excluded.username,"
                 " first_name = excluded.first_name,"
                 " last_name = excluded.last_name,"
                 " is_bot = excluded.is_bot,"
                 " updated_at = excluded.updated_at");
    st.bind(1, user.id);
    // No username -> NULL (the unique index skips NULLs; '' would make all
    // username-less users collide with each other).
    if (user.username.empty()) {
        st.bind_null(2);
    } else {
        st.bind(2, user.username);
    }
    st.bind(3, user.first_name);
    st.bind(4, user.last_name);
    st.bind(5, user.is_bot ? 1 : 0);
    st.bind(6, now);
    st.step();
}

std::optional<UserRow> UserRepo::find_by_username(const std::string& username) {
    if (username.empty()) return std::nullopt;
    Statement st(db_.raw(),
                 "SELECT id, username, first_name, last_name, is_bot, first_seen_at "
                 "FROM users WHERE lower(username) = ?1");
    st.bind(1, lower(username));
    if (st.step()) return row_from_statement(st);
    return std::nullopt;
}

std::optional<UserRow> UserRepo::find_by_id(int64_t id) {
    Statement st(db_.raw(),
                 "SELECT id, username, first_name, last_name, is_bot, first_seen_at "
                 "FROM users WHERE id = ?1");
    st.bind(1, id);
    if (st.step()) return row_from_statement(st);
    return std::nullopt;
}

void UserRepo::upsert_member(int64_t chat_id, int64_t user_id, const std::string& status,
                             int64_t now, std::optional<int64_t> joined_at) {
    Statement st(db_.raw(),
                 "INSERT INTO members (chat_id, user_id, status, joined_at, updated_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5) "
                 "ON CONFLICT(chat_id, user_id) DO UPDATE SET "
                 " status = excluded.status,"
                 " joined_at = COALESCE(members.joined_at, excluded.joined_at),"
                 " updated_at = excluded.updated_at");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.bind(3, status);
    if (joined_at) {
        st.bind(4, *joined_at);
    } else {
        st.bind_null(4);
    }
    st.bind(5, now);
    st.step();
}

void UserRepo::mark_member_active(int64_t chat_id, int64_t user_id, int64_t now) {
    // Only revives rows that say the user is gone; never touches a cached
    // creator/administrator/restricted status, and never inserts a guessed
    // status (a missing row makes role checks ask Telegram instead).
    Statement st(db_.raw(),
                 "UPDATE members SET status = 'member', updated_at = ?3 "
                 "WHERE chat_id = ?1 AND user_id = ?2 AND status IN ('left', 'kicked')");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.bind(3, now);
    st.step();
}

std::optional<std::string> UserRepo::member_status(int64_t chat_id, int64_t user_id) const {
    if (auto entry = member_entry(chat_id, user_id)) return entry->status;
    return std::nullopt;
}

std::optional<MemberEntry> UserRepo::member_entry(int64_t chat_id, int64_t user_id) const {
    Statement st(db_.raw(),
                 "SELECT status, updated_at FROM members WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    if (st.step()) return MemberEntry{st.col_str(0), st.col_i64(1)};
    return std::nullopt;
}

std::vector<UserRow> UserRepo::admins(int64_t chat_id) const {
    std::vector<UserRow> rows;
    Statement st(db_.raw(),
                 "SELECT u.id, u.username, u.first_name, u.last_name, u.is_bot, "
                 "       u.first_seen_at "
                 "FROM members m "
                 "JOIN users u ON u.id = m.user_id "
                 "WHERE m.chat_id = ?1 AND m.status IN ('creator', 'administrator') "
                 "ORDER BY (m.status = 'creator') DESC, u.username, u.first_name");
    st.bind(1, chat_id);
    while (st.step()) rows.push_back(row_from_statement(st));
    return rows;
}

ChatRepo::ChatRepo(Database& db) : db_(db) {}

void ChatRepo::ensure(const tg::Chat& chat, int64_t now, const std::string& default_lang) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, title, type, language) VALUES (?1, ?2, ?3, ?4) "
                 "ON CONFLICT(id) DO UPDATE SET title = excluded.title, "
                 " type = excluded.type");
    st.bind(1, chat.id);
    st.bind(2, chat.title);
    st.bind(3, chat.type);
    st.bind(4, default_lang.empty() ? "en" : default_lang);
    st.step();
    (void)now;
}

std::string ChatRepo::language(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT language FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_str(0) : std::string{};
}

void ChatRepo::set_language(int64_t chat_id, const std::string& language) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, language) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET language = excluded.language");
    st.bind(1, chat_id);
    st.bind(2, language);
    st.step();
}

void ChatRepo::set_rules(int64_t chat_id, const std::string& rules_html) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, rules_html) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET rules_html = excluded.rules_html");
    st.bind(1, chat_id);
    st.bind(2, rules_html);
    st.step();
}

void ChatRepo::clear_rules(int64_t chat_id) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, rules_html) VALUES (?1, NULL) "
                 "ON CONFLICT(id) DO UPDATE SET rules_html = NULL");
    st.bind(1, chat_id);
    st.step();
}

bool ChatRepo::has_rules(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT rules_html FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() && !st.col_str(0).empty();
}

std::string ChatRepo::rules_text(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT rules_html FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_str(0) : std::string{};
}

void ChatRepo::set_reports_enabled(int64_t chat_id, bool enabled) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, reports_enabled) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET reports_enabled = excluded.reports_enabled");
    st.bind(1, chat_id);
    st.bind(2, enabled ? 1 : 0);
    st.step();
}

bool ChatRepo::reports_enabled(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT reports_enabled FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() && st.col_bool(0);
}

void ChatRepo::set_warn_limit(int64_t chat_id, int limit) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, warn_limit) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET warn_limit = excluded.warn_limit");
    st.bind(1, chat_id);
    st.bind(2, limit);
    st.step();
}

void ChatRepo::set_warn_action(int64_t chat_id, const std::string& action,
                               int64_t duration_s) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, warn_action, warn_action_duration) VALUES (?1, ?2, ?3) "
                 "ON CONFLICT(id) DO UPDATE SET warn_action = excluded.warn_action, "
                 " warn_action_duration = excluded.warn_action_duration");
    st.bind(1, chat_id);
    st.bind(2, action);
    st.bind(3, duration_s);
    st.step();
}

int64_t ChatRepo::warn_action_duration(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT warn_action_duration FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_i64(0) : 0;
}

FloodSettings ChatRepo::flood_settings(int64_t chat_id) const {
    FloodSettings f;
    Statement st(db_.raw(),
                 "SELECT flood_limit, flood_timer_count, flood_timer_seconds, flood_action, "
                 "       flood_action_duration, flood_clear "
                 "FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    if (!st.step()) return f;
    f.limit = st.col_int(0);
    f.timer_count = st.col_int(1);
    f.timer_seconds = st.col_i64(2);
    f.action = st.col_str(3);
    f.action_duration = st.col_i64(4);
    f.clear = st.col_bool(5);
    return f;
}

void ChatRepo::set_flood_limit(int64_t chat_id, int limit) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, flood_limit) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET flood_limit = excluded.flood_limit");
    st.bind(1, chat_id);
    st.bind(2, static_cast<int64_t>(limit));
    st.step();
}

void ChatRepo::set_flood_timer(int64_t chat_id, int count, int64_t seconds) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, flood_timer_count, flood_timer_seconds) "
                 "VALUES (?1, ?2, ?3) "
                 "ON CONFLICT(id) DO UPDATE SET flood_timer_count = excluded.flood_timer_count, "
                 " flood_timer_seconds = excluded.flood_timer_seconds");
    st.bind(1, chat_id);
    st.bind(2, static_cast<int64_t>(count));
    st.bind(3, seconds);
    st.step();
}

void ChatRepo::set_flood_action(int64_t chat_id, const std::string& action,
                                int64_t duration_s) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, flood_action, flood_action_duration) VALUES (?1, ?2, ?3) "
                 "ON CONFLICT(id) DO UPDATE SET flood_action = excluded.flood_action, "
                 " flood_action_duration = excluded.flood_action_duration");
    st.bind(1, chat_id);
    st.bind(2, action);
    st.bind(3, duration_s);
    st.step();
}

bool ChatRepo::ai_enabled(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT ai_enabled FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_bool(0) : true;
}

void ChatRepo::set_ai_enabled(int64_t chat_id, bool enabled) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, ai_enabled) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET ai_enabled = excluded.ai_enabled");
    st.bind(1, chat_id);
    st.bind(2, static_cast<int64_t>(enabled ? 1 : 0));
    st.step();
}

bool ChatRepo::ai_autoreply(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT ai_autoreply FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() && st.col_bool(0);
}

void ChatRepo::set_ai_autoreply(int64_t chat_id, bool enabled) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, ai_autoreply) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET ai_autoreply = excluded.ai_autoreply");
    st.bind(1, chat_id);
    st.bind(2, static_cast<int64_t>(enabled ? 1 : 0));
    st.step();
}

void ChatRepo::set_flood_clear(int64_t chat_id, bool clear) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, flood_clear) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET flood_clear = excluded.flood_clear");
    st.bind(1, chat_id);
    st.bind(2, static_cast<int64_t>(clear ? 1 : 0));
    st.step();
}

int ChatRepo::warn_limit(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT warn_limit FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_int(0) : 3;
}

std::string ChatRepo::warn_action(int64_t chat_id) const {
    Statement st(db_.raw(), "SELECT warn_action FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    return st.step() ? st.col_str(0) : std::string("mute");
}

ChatSettings ChatRepo::settings(int64_t chat_id) const {
    ChatSettings s;
    Statement st(db_.raw(),
                 "SELECT language, COALESCE(rules_html, ''), reports_enabled, warn_limit, "
                 "       warn_action, warn_action_duration, ai_enabled, ai_autoreply "
                 "FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    if (!st.step()) return s;  // unknown chat -> defaults
    s.language = st.col_str(0);
    s.rules_html = st.col_str(1);
    s.reports_enabled = st.col_bool(2);
    s.warn_limit = st.col_int(3);
    s.warn_action = st.col_str(4);
    s.warn_action_duration = st.col_i64(5);
    s.ai_enabled = st.col_bool(6);
    s.ai_autoreply = st.col_bool(7);
    s.flood = flood_settings(chat_id);
    return s;
}

// --- WarnRepo ------------------------------------------------------------

WarnRepo::WarnRepo(Database& db) : db_(db) {}

void WarnRepo::add(int64_t chat_id, int64_t user_id, const std::string& reason,
                   int64_t created_by, int64_t created_at) {
    Statement st(db_.raw(),
                 "INSERT INTO warns (chat_id, user_id, reason, created_by, created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5)");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.bind(3, reason);
    st.bind(4, created_by);
    st.bind(5, created_at);
    st.step();
}

bool WarnRepo::remove_last(int64_t chat_id, int64_t user_id) {
    int64_t id = 0;
    {
        Statement st(db_.raw(),
                     "SELECT id FROM warns WHERE chat_id = ?1 AND user_id = ?2 "
                     "ORDER BY id DESC LIMIT 1");
        st.bind(1, chat_id);
        st.bind(2, user_id);
        if (!st.step()) return false;
        id = st.col_i64(0);
    }
    Statement st(db_.raw(), "DELETE FROM warns WHERE id = ?1");
    st.bind(1, id);
    st.step();
    return true;
}

std::vector<WarnRow> WarnRepo::list(int64_t chat_id, int64_t user_id) const {
    std::vector<WarnRow> rows;
    Statement st(db_.raw(),
                 "SELECT id, user_id, reason, created_at FROM warns "
                 "WHERE chat_id = ?1 AND user_id = ?2 ORDER BY id DESC");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    while (st.step()) {
        rows.push_back(WarnRow{st.col_i64(0), st.col_i64(1), st.col_str(2), st.col_i64(3)});
    }
    return rows;
}

int WarnRepo::count(int64_t chat_id, int64_t user_id) const {
    Statement st(db_.raw(),
                 "SELECT COUNT(*) FROM warns WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    return st.step() ? st.col_int(0) : 0;
}

int WarnRepo::clear(int64_t chat_id, int64_t user_id) {
    Statement st(db_.raw(), "DELETE FROM warns WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.step();
    return sqlite3_changes(db_.raw());
}

// --- MuteRepo ------------------------------------------------------------

MuteRepo::MuteRepo(Database& db) : db_(db) {}

void MuteRepo::set(int64_t chat_id, int64_t user_id, int64_t until, const std::string& reason,
                   int64_t created_by, int64_t created_at) {
    {
        Statement st(db_.raw(), "DELETE FROM mutes WHERE chat_id = ?1 AND user_id = ?2");
        st.bind(1, chat_id);
        st.bind(2, user_id);
        st.step();
    }
    Statement st(db_.raw(),
                 "INSERT INTO mutes (chat_id, user_id, until, reason, created_by, created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.bind(3, until);
    st.bind(4, reason);
    st.bind(5, created_by);
    st.bind(6, created_at);
    st.step();
}

void MuteRepo::clear(int64_t chat_id, int64_t user_id) {
    Statement st(db_.raw(), "DELETE FROM mutes WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.step();
}

std::optional<int64_t> MuteRepo::until(int64_t chat_id, int64_t user_id) const {
    Statement st(db_.raw(),
                 "SELECT until FROM mutes WHERE chat_id = ?1 AND user_id = ?2 "
                 "ORDER BY id DESC LIMIT 1");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    if (st.step()) return st.col_i64(0);
    return std::nullopt;
}

// --- BanRepo -------------------------------------------------------------

BanRepo::BanRepo(Database& db) : db_(db) {}

void BanRepo::set(int64_t chat_id, int64_t user_id, const std::string& reason,
                  int64_t created_by, int64_t created_at) {
    {
        Statement st(db_.raw(), "DELETE FROM bans WHERE chat_id = ?1 AND user_id = ?2");
        st.bind(1, chat_id);
        st.bind(2, user_id);
        st.step();
    }
    Statement st(db_.raw(),
                 "INSERT INTO bans (chat_id, user_id, reason, created_by, created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5)");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.bind(3, reason);
    st.bind(4, created_by);
    st.bind(5, created_at);
    st.step();
}

void BanRepo::clear(int64_t chat_id, int64_t user_id) {
    Statement st(db_.raw(), "DELETE FROM bans WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.step();
}

std::optional<std::string> BanRepo::reason(int64_t chat_id, int64_t user_id) const {
    Statement st(db_.raw(),
                 "SELECT reason FROM bans WHERE chat_id = ?1 AND user_id = ?2 "
                 "ORDER BY id DESC LIMIT 1");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    if (st.step()) return st.col_str(0);
    return std::nullopt;
}

// --- ReportRepo -----------------------------------------------------------

ReportRepo::ReportRepo(Database& db) : db_(db) {}

void ReportRepo::add(int64_t chat_id, int64_t message_id, int64_t reporter_id,
                     int64_t target_id, int64_t created_at) {
    // INSERT OR IGNORE: the UNIQUE (chat_id, reporter_id, message_id) constraint
    // silently drops a repeated report (spec §3).
    Statement st(db_.raw(),
                 "INSERT OR IGNORE INTO reports (chat_id, message_id, reporter_id, "
                 "target_id, created_at) VALUES (?1, ?2, ?3, ?4, ?5)");
    st.bind(1, chat_id);
    st.bind(2, message_id);
    st.bind(3, reporter_id);
    st.bind(4, target_id);
    st.bind(5, created_at);
    st.step();
}

bool ReportRepo::already_reported(int64_t chat_id, int64_t reporter_id,
                                  int64_t message_id) const {
    // message_id is only unique within a chat, so the chat is part of the key.
    Statement st(db_.raw(),
                 "SELECT 1 FROM reports WHERE chat_id = ?1 AND reporter_id = ?2 "
                 "AND message_id = ?3 LIMIT 1");
    st.bind(1, chat_id);
    st.bind(2, reporter_id);
    st.bind(3, message_id);
    return st.step();
}

bool ReportRepo::already_reported_target(int64_t chat_id, int64_t reporter_id,
                                         int64_t target_id) const {
    Statement st(db_.raw(),
                 "SELECT 1 FROM reports WHERE chat_id = ?1 AND reporter_id = ?2 "
                 "AND target_id = ?3 LIMIT 1");
    st.bind(1, chat_id);
    st.bind(2, reporter_id);
    st.bind(3, target_id);
    return st.step();
}

// --- AnswerRepo -----------------------------------------------------------

AnswerRepo::AnswerRepo(Database& db) : db_(db) {}

void AnswerRepo::set(int64_t chat_id, const std::string& command, bool enabled) {
    Statement st(db_.raw(),
                 "INSERT INTO answers (chat_id, command, enabled) VALUES (?1, ?2, ?3) "
                 "ON CONFLICT(chat_id, command) DO UPDATE SET enabled = excluded.enabled");
    st.bind(1, chat_id);
    st.bind(2, command);
    st.bind(3, enabled ? 1 : 0);
    st.step();
}

bool AnswerRepo::enabled(int64_t chat_id, const std::string& command) const {
    Statement st(db_.raw(),
                 "SELECT enabled FROM answers WHERE chat_id = ?1 AND command = ?2");
    st.bind(1, chat_id);
    st.bind(2, command);
    if (st.step()) return st.col_bool(0);
    return true;  // no override = enabled (default)
}

// --- AuditRepo -----------------------------------------------------------

AuditRepo::AuditRepo(Database& db) : db_(db) {}

void AuditRepo::log(int64_t chat_id, int64_t actor_id, const std::string& action,
                    std::optional<int64_t> target_id, const std::string& details,
                    int64_t created_at) {
    Statement st(db_.raw(),
                 "INSERT INTO audit (chat_id, actor_id, action, target_id, details, "
                 "created_at) "
                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    st.bind(1, chat_id);
    st.bind(2, actor_id);
    st.bind(3, action);
    if (target_id) {
        st.bind(4, *target_id);
    } else {
        st.bind_null(4);
    }
    st.bind(5, details);
    st.bind(6, created_at);
    st.step();
}

// --- GlobalBanRepo ------------------------------------------------------

GlobalBanRepo::GlobalBanRepo(Database& db) : db_(db) {}

bool GlobalBanRepo::add(int64_t user_id, const std::string& reason, int64_t created_by,
                        int64_t created_at) {
    Statement st(db_.raw(),
                 "INSERT OR IGNORE INTO global_bans (user_id, reason, created_by, created_at) "
                 "VALUES (?1, ?2, ?3, ?4)");
    st.bind(1, user_id);
    st.bind(2, reason);
    st.bind(3, created_by);
    st.bind(4, created_at);
    st.step();
    return sqlite3_changes(db_.raw()) > 0;
}

bool GlobalBanRepo::remove(int64_t user_id) {
    Statement st(db_.raw(), "DELETE FROM global_bans WHERE user_id = ?1");
    st.bind(1, user_id);
    st.step();
    return sqlite3_changes(db_.raw()) > 0;
}

bool GlobalBanRepo::is_banned(int64_t user_id) const {
    Statement st(db_.raw(), "SELECT 1 FROM global_bans WHERE user_id = ?1");
    st.bind(1, user_id);
    return st.step();
}

// --- BotSettingsRepo ----------------------------------------------------

BotSettingsRepo::BotSettingsRepo(Database& db) : db_(db) {}

std::optional<std::string> BotSettingsRepo::get(const std::string& key) const {
    Statement st(db_.raw(), "SELECT value FROM bot_settings WHERE key = ?1");
    st.bind(1, key);
    if (!st.step()) return std::nullopt;
    return st.col_str(0);
}

void BotSettingsRepo::set(const std::string& key, const std::string& value) {
    Statement st(db_.raw(),
                 "INSERT INTO bot_settings (key, value) VALUES (?1, ?2) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    st.bind(1, key);
    st.bind(2, value);
    st.step();
}

bool BotSettingsRepo::flag(const std::string& key, bool fallback) const {
    const auto v = get(key);
    return v ? *v == "1" : fallback;
}

void BotSettingsRepo::set_flag(const std::string& key, bool value) {
    set(key, value ? "1" : "0");
}

}  // namespace yacm::storage
