#pragma once
// Small RAII wrapper over the SQLite C API.
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace aow::sql {

struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, const std::string& value);  // 1-based, like SQLite
    Statement& bind(int index, std::int64_t value);
    bool step();  // true while a row is available; throws on error
    void run();   // step() for statements that return no rows
    void reset();

    int parameterCount() const;
    std::string text(int column) const;  // 0-based
    std::int64_t int64(int column) const;
    bool isNull(int column) const;

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    explicit Database(const std::filesystem::path& file);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const char* sql);
    Statement prepare(const char* sql) { return Statement(db_, sql); }

private:
    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back on destruction unless commit() succeeded.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

} // namespace aow::sql
