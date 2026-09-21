#include "storage/repos.hpp"

#include "storage/db.hpp"

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
    st.bind(2, user.username);
    st.bind(3, user.first_name);
    st.bind(4, user.last_name);
    st.bind(5, user.is_bot ? 1 : 0);
    st.bind(6, now);
    st.step();
}

std::optional<UserRow> UserRepo::find_by_username(const std::string& username) {
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

std::optional<std::string> UserRepo::member_status(int64_t chat_id, int64_t user_id) const {
    Statement st(db_.raw(), "SELECT status FROM members WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    if (st.step()) return st.col_str(0);
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

void ChatRepo::set_warn_action(int64_t chat_id, const std::string& action) {
    Statement st(db_.raw(),
                 "INSERT INTO chats (id, warn_action) VALUES (?1, ?2) "
                 "ON CONFLICT(id) DO UPDATE SET warn_action = excluded.warn_action");
    st.bind(1, chat_id);
    st.bind(2, action);
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
                 "       warn_action "
                 "FROM chats WHERE id = ?1");
    st.bind(1, chat_id);
    if (!st.step()) return s;  // unknown chat -> defaults
    s.language = st.col_str(0);
    s.rules_html = st.col_str(1);
    s.reports_enabled = st.col_bool(2);
    s.warn_limit = st.col_int(3);
    s.warn_action = st.col_str(4);
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

void WarnRepo::clear(int64_t chat_id, int64_t user_id) {
    Statement st(db_.raw(), "DELETE FROM warns WHERE chat_id = ?1 AND user_id = ?2");
    st.bind(1, chat_id);
    st.bind(2, user_id);
    st.step();
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
    // INSERT OR IGNORE: the UNIQUE (reporter_id, message_id) constraint
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

bool ReportRepo::already_reported(int64_t reporter_id, int64_t message_id) const {
    Statement st(db_.raw(),
                 "SELECT 1 FROM reports WHERE reporter_id = ?1 AND message_id = ?2 "
                 "LIMIT 1");
    st.bind(1, reporter_id);
    st.bind(2, message_id);
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

}  // namespace yacm::storage