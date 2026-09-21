#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace yacm::storage {

struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// RAII wrapper around a prepared SQLite statement.
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return stmt_; }

    // Executes the statement once. Returns true when a row is available.
    bool step();
    void reset();

    int bind(int index, int64_t value);
    int bind(int index, const std::string& value);
    int bind_null(int index);

    int64_t col_i64(int index) const;
    int col_int(int index) const;
    bool col_bool(int index) const;
    std::string col_str(int index) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    std::string sql_;
};

// SQLite database with WAL, foreign keys and busy timeout. Applies
// embedded migrations at open time.
class Database {
public:
    Database() = default;
    explicit Database(std::string path);
    ~Database();

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open();
    void close();  // WAL checkpoint + close

    sqlite3* raw() const { return db_; }
    [[nodiscard]] const std::string& path() const { return path_; }

    // Executes raw SQL (multiple statements allowed). Throws on error.
    void exec(const std::string& sql);

    void migrate();
    [[nodiscard]] int64_t schema_version() const;
    [[nodiscard]] bool table_exists(const std::string& name) const;

    static std::vector<std::pair<int, std::string>> migrations();

private:
    std::string path_;
    sqlite3* db_ = nullptr;
};

// RAII transaction: commits on scope exit unless rollback() was called
// (an exception during commit/rollback is swallowed on destruction).
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();
    void rollback() noexcept;

private:
    Database& db_;
    bool finished_ = false;
};

}  // namespace yacm::storage