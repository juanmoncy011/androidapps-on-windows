#include "sqlite_db.h"

#include <sqlite3.h>

namespace pkgmgr::detail {
namespace {

[[noreturn]] void fail(sqlite3* db, const std::string& what) {
    throw DbError(what + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
}

} // namespace

Statement::Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) fail(db_, "prepare");
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

Statement& Statement::bind(int index, const std::string& value) {
    if (sqlite3_bind_text(stmt_, index, value.data(), int(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        fail(db_, "bind");
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, sqlite3_int64(value)) != SQLITE_OK) fail(db_, "bind");
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    fail(db_, "step");
}

void Statement::run() {
    step();
    reset();
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

std::string Statement::text(int column) const {
    const auto* p = sqlite3_column_text(stmt_, column);
    return p ? std::string(reinterpret_cast<const char*>(p), std::size_t(sqlite3_column_bytes(stmt_, column)))
             : std::string();
}

std::int64_t Statement::int64(int column) const { return sqlite3_column_int64(stmt_, column); }

Database::Database(const std::filesystem::path& file) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(file.u8string().c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw DbError("cannot open " + file.u8string() + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);  // wait for other processes sharing the database
}

Database::~Database() { sqlite3_close(db_); }

void Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        throw DbError(msg);
    }
}

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (!done_) {
        try { db_.exec("ROLLBACK"); } catch (const DbError&) {}
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

} // namespace pkgmgr::detail
