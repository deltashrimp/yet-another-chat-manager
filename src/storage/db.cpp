#include "storage/db.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace yacm::storage {
namespace {

std::string sqlite_message(sqlite3* db, std::string what) {
    return std::move(what) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(SQLITE_ERROR));
}

}  // namespace

// --- Statement -----------------------------------------------------------

Statement::Statement(sqlite3* db, const std::string& sql) : db_(db), sql_(sql) {
    const int rc = sqlite3_prepare_v2(db_, sql_.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw DbError(sqlite_message(db_, "failed to prepare statement"));
    }
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(sqlite_message(db_, "failed to step statement"));
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

int Statement::bind(int index, int64_t value) {
    return sqlite3_bind_int64(stmt_, index, value);
}

int Statement::bind(int index, const std::string& value) {
    return sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT);
}

int Statement::bind_null(int index) { return sqlite3_bind_null(stmt_, index); }

int64_t Statement::col_i64(int index) const { return sqlite3_column_int64(stmt_, index); }

int Statement::col_int(int index) const { return sqlite3_column_int(stmt_, index); }

bool Statement::col_bool(int index) const { return sqlite3_column_int(stmt_, index) != 0; }

std::string Statement::col_str(int index) const {
    const auto* text = sqlite3_column_text(stmt_, index);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

// --- Database ------------------------------------------------------------

Database::Database(std::string path) : path_(std::move(path)) {}

Database::~Database() { close(); }

Database::Database(Database&& other) noexcept
    : path_(std::move(other.path_)), db_(other.db_) {
    other.db_ = nullptr;
    other.path_.clear();
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        db_ = other.db_;
        other.db_ = nullptr;
        other.path_.clear();
    }
    return *this;
}

void Database::open() {
    if (db_) return;

    // sqlite3 does not create missing parent directories.
    const std::filesystem::path parent =
        std::filesystem::path(path_).parent_path();
    if (!parent.empty() && parent != ".") {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec && !std::filesystem::exists(parent)) {
            throw DbError("cannot create database directory '" + parent.string() +
                          "': " + ec.message() + " — set DB_PATH to a writable path in "
                          "./.env (e.g. DB_PATH=./data/bot.db), or grant write access "
                          "to the directory");
        }
    }

    const int rc = sqlite3_open_v2(path_.c_str(), &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        const std::string msg = sqlite_message(db_, "cannot open database: " + path_);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw DbError(msg +
                      " (check that the parent directory exists and is writable)");
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA busy_timeout=5000;");
    exec("PRAGMA synchronous=NORMAL;");
}

void Database::close() {
    if (!db_) return;
    try {
        exec("PRAGMA wal_checkpoint(PASSIVE);");
    } catch (const std::exception&) {
        // Best effort; proceed with close.
    }
    sqlite3_close(db_);
    db_ = nullptr;
}

void Database::exec(const std::string& sql) {
    if (!db_) throw DbError("database is not open");
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string message = "SQL error";
        if (errmsg) message = errmsg;
        if (errmsg) sqlite3_free(errmsg);
        throw DbError(message + " -- " + sql);
    }
}

void Database::migrate() {
    if (!table_exists("schema_version")) {
        exec("CREATE TABLE schema_version (version INTEGER NOT NULL);");
        exec("INSERT INTO schema_version (version) VALUES (0);");
    }

    const int64_t current = schema_version();
    for (const auto& [version, sql] : migrations()) {
        if (version <= current) continue;
        exec("BEGIN;");
        try {
            exec(sql);
            Statement st(db_, "UPDATE schema_version SET version = ?1 WHERE version < ?2");
            st.bind(1, version);
            st.bind(2, version);
            st.step();
            exec("COMMIT;");
        } catch (...) {
            exec("ROLLBACK;");
            throw;
        }
    }
}

int64_t Database::schema_version() const {
    Statement st(db_, "SELECT version FROM schema_version ORDER BY version DESC LIMIT 1");
    return st.step() ? st.col_i64(0) : 0;
}

bool Database::table_exists(const std::string& name) const {
    Statement st(db_,
                 "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1");
    st.bind(1, name);
    return st.step();
}

std::vector<std::pair<int, std::string>> Database::migrations() {
    return {
        {1, R"sql(
            CREATE TABLE users (
                id            INTEGER PRIMARY KEY,
                username      TEXT,
                first_name    TEXT NOT NULL DEFAULT '',
                last_name     TEXT NOT NULL DEFAULT '',
                is_bot        INTEGER NOT NULL DEFAULT 0,
                first_seen_at INTEGER NOT NULL,
                updated_at    INTEGER NOT NULL
            );
            CREATE UNIQUE INDEX idx_users_username ON users (lower(username))
                WHERE username IS NOT NULL;

            CREATE TABLE chats (
                id              INTEGER PRIMARY KEY,
                title           TEXT NOT NULL DEFAULT '',
                type            TEXT NOT NULL DEFAULT 'private',
                language        TEXT NOT NULL DEFAULT 'en',
                rules_html      TEXT,
                reports_enabled INTEGER NOT NULL DEFAULT 1,
                warn_limit      INTEGER NOT NULL DEFAULT 3,
                warn_action     TEXT NOT NULL DEFAULT 'mute'
            );

            CREATE TABLE warns (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id    INTEGER NOT NULL,
                user_id    INTEGER NOT NULL,
                reason     TEXT,
                created_by INTEGER NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX idx_warns_user ON warns (chat_id, user_id);

            CREATE TABLE mutes (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id    INTEGER NOT NULL,
                user_id    INTEGER NOT NULL,
                until      INTEGER NOT NULL DEFAULT 0, -- 0 = forever
                reason     TEXT,
                created_by INTEGER NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX idx_mutes_user ON mutes (chat_id, user_id);

            CREATE TABLE bans (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id    INTEGER NOT NULL,
                user_id    INTEGER NOT NULL,
                reason     TEXT,
                created_by INTEGER NOT NULL,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX idx_bans_user ON bans (chat_id, user_id);

            CREATE TABLE reports (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id     INTEGER NOT NULL,
                message_id  INTEGER NOT NULL,
                reporter_id INTEGER NOT NULL,
                target_id   INTEGER NOT NULL,
                created_at  INTEGER NOT NULL,
                UNIQUE (reporter_id, message_id)
            );
            CREATE INDEX idx_reports_chat ON reports (chat_id);

            CREATE TABLE audit (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id    INTEGER NOT NULL,
                actor_id   INTEGER NOT NULL,
                action     TEXT NOT NULL,
                target_id  INTEGER,
                details    TEXT,
                created_at INTEGER NOT NULL
            );
            CREATE INDEX idx_audit_chat ON audit (chat_id);

            CREATE TABLE members (
                chat_id    INTEGER NOT NULL,
                user_id    INTEGER NOT NULL,
                status     TEXT NOT NULL DEFAULT 'member',
                joined_at  INTEGER,
                updated_at INTEGER NOT NULL,
                PRIMARY KEY (chat_id, user_id)
            );
        )sql"},
        {2, R"sql(
            CREATE TABLE answers (
                chat_id INTEGER NOT NULL,
                command TEXT NOT NULL,
                enabled INTEGER NOT NULL DEFAULT 1,
                PRIMARY KEY (chat_id, command)
            );
        )sql"},
        {3, R"sql(
            -- Users without a username were stored as '' instead of NULL,
            -- so the unique username index made them collide.
            UPDATE users SET username = NULL WHERE username = '';

            -- message_id is only unique per chat: report dedup must include
            -- chat_id (the old UNIQUE (reporter_id, message_id) silently
            -- dropped legitimate reports from other chats).
            CREATE TABLE reports_v3 (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                chat_id     INTEGER NOT NULL,
                message_id  INTEGER NOT NULL,
                reporter_id INTEGER NOT NULL,
                target_id   INTEGER NOT NULL,
                created_at  INTEGER NOT NULL,
                UNIQUE (chat_id, reporter_id, message_id)
            );
            INSERT INTO reports_v3 (id, chat_id, message_id, reporter_id, target_id, created_at)
                SELECT id, chat_id, message_id, reporter_id, target_id, created_at FROM reports;
            DROP TABLE reports;
            ALTER TABLE reports_v3 RENAME TO reports;
            CREATE INDEX idx_reports_chat ON reports (chat_id);
        )sql"},
    };
}

// --- Transaction ---------------------------------------------------------

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN;"); }

Transaction::~Transaction() {
    if (!finished_) {
        try {
            db_.exec("ROLLBACK;");
        } catch (const std::exception&) {
            // Best effort; the connection is about to be used/closed anyway.
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT;");
    finished_ = true;
}

void Transaction::rollback() noexcept {
    if (finished_) return;
    try {
        db_.exec("ROLLBACK;");
    } catch (const std::exception&) {
        // Best effort.
    }
    finished_ = true;
}

}  // namespace yacm::storage