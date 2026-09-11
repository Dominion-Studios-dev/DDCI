#include "storage/db_client.hpp"

#include <sqlite3.h>

#include <sys/stat.h>

#include <algorithm>
#include <utility>

namespace ddci::storage {

StatementHandle::StatementHandle(sqlite3_stmt* stmt)
    : stmt_(stmt) {}

StatementHandle::~StatementHandle() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

StatementHandle::StatementHandle(StatementHandle&& other) noexcept
    : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

StatementHandle& StatementHandle::operator=(StatementHandle&& other) noexcept {
    if (this != &other) {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

core::Result<void> StatementHandle::bind_text(int index, std::string_view value) {
    int rc = sqlite3_bind_text(stmt_, index,
                               value.data(),
                               static_cast<int>(value.size()),
                               SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

core::Result<void> StatementHandle::bind_int64(int index, int64_t value) {
    int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

core::Result<void> StatementHandle::bind_double(int index, double value) {
    int rc = sqlite3_bind_double(stmt_, index, value);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

core::Result<void> StatementHandle::bind_null(int index) {
    int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

core::Result<bool> StatementHandle::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return core::Result<bool>(true);
    }
    if (rc == SQLITE_DONE) {
        return core::Result<bool>(false);
    }
    return core::Result<bool>::failure(core::ErrorCode::DatabaseError);
}

core::Result<void> StatementHandle::reset() {
    int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    rc = sqlite3_clear_bindings(stmt_);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

std::string_view StatementHandle::column_text(int col) const {
    const char* ptr = reinterpret_cast<const char*>(
        sqlite3_column_text(stmt_, col));
    int size = sqlite3_column_bytes(stmt_, col);
    return ptr ? std::string_view(ptr, static_cast<size_t>(size))
               : std::string_view{};
}

int64_t StatementHandle::column_int64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

double StatementHandle::column_double(int col) const {
    return sqlite3_column_double(stmt_, col);
}

bool StatementHandle::column_is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

DatabaseClient::DatabaseClient(std::string db_path)
    : db_path_(std::move(db_path)) {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return;
    }
    (void)::chmod(db_path_.c_str(), S_IRUSR | S_IWUSR);
}

DatabaseClient::~DatabaseClient() {
    close();
}

DatabaseClient::DatabaseClient(DatabaseClient&& other) noexcept
    : db_path_(std::move(other.db_path_))
    , db_(other.db_) {
    other.db_ = nullptr;
}

DatabaseClient& DatabaseClient::operator=(DatabaseClient&& other) noexcept {
    if (this != &other) {
        close();
        db_path_ = std::move(other.db_path_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void DatabaseClient::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

core::Result<void> DatabaseClient::apply_pragmas() {
    const char* const pragmas[] = {
        "PRAGMA journal_mode = WAL;",
        "PRAGMA synchronous = NORMAL;",
        "PRAGMA mmap_size = 268435456;",
    };
    for (const char* sql : pragmas) {
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) {
            return core::Result<void>::failure(core::ErrorCode::DatabaseError);
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DatabaseClient::execute(std::string_view sql) {
    if (!db_) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    int rc = sqlite3_exec(db_, sql.data(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }
    return core::Result<void>::success();
}

core::Result<StatementHandle> DatabaseClient::prepare(std::string_view sql) {
    if (!db_) {
        return core::Result<StatementHandle>::failure(
            core::ErrorCode::DatabaseError);
    }
    sqlite3_stmt* raw_stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
                                sql.data(),
                                static_cast<int>(sql.size()),
                                &raw_stmt,
                                nullptr);
    if (rc != SQLITE_OK) {
        if (raw_stmt) {
            sqlite3_finalize(raw_stmt);
        }
        return core::Result<StatementHandle>::failure(
            core::ErrorCode::DatabaseError);
    }
    return core::Result<StatementHandle>(
        StatementHandle(raw_stmt));
}

core::Result<void> DatabaseClient::insert_l2_summary(
    std::string_view category, std::string_view content, double importance) {
    if (!db_) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }

    auto prep_result =
        prepare("INSERT INTO l2_summaries (category, content, importance_score) "
                "VALUES (?1, ?2, ?3)");
    if (!prep_result.ok()) {
        return core::Result<void>::failure(prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    auto b1 = stmt.bind_text(1, category);
    if (!b1.ok()) return b1;

    auto b2 = stmt.bind_text(2, content);
    if (!b2.ok()) return b2;

    auto b3 = stmt.bind_double(3, importance);
    if (!b3.ok()) return b3;

    auto step_result = stmt.step();
    if (!step_result.ok()) {
        return core::Result<void>::failure(step_result.error());
    }
    return core::Result<void>::success();
}

core::Result<void> DatabaseClient::init_schema() {
    if (!db_) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }

    auto pragma_result = apply_pragmas();
    if (!pragma_result.ok()) {
        return pragma_result;
    }

    auto r1 = execute(
        "CREATE TABLE IF NOT EXISTS l2_summaries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  category TEXT NOT NULL DEFAULT '',"
        "  content TEXT NOT NULL,"
        "  importance_score REAL NOT NULL DEFAULT 0.0,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ")");
    if (!r1.ok()) return r1;

    auto r2 = execute(
        "CREATE TABLE IF NOT EXISTS l3_facts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  category TEXT NOT NULL DEFAULT '',"
        "  content TEXT NOT NULL,"
        "  importance_score REAL NOT NULL DEFAULT 0.0,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ")");
    if (!r2.ok()) return r2;

    auto r3 = execute(
        "CREATE INDEX IF NOT EXISTS idx_l2_category ON l2_summaries(category)");
    if (!r3.ok()) return r3;

    auto r4 = execute(
        "CREATE INDEX IF NOT EXISTS idx_l3_category ON l3_facts(category)");
    if (!r4.ok()) return r4;

    auto r5 = execute(
        "CREATE TABLE IF NOT EXISTS user_config ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL DEFAULT ''"
        ")");
    if (!r5.ok()) return r5;

    auto r6 = execute(
        "CREATE TABLE IF NOT EXISTS chat_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")");
    if (!r6.ok()) return r6;

    return core::Result<void>::success();
}

core::Result<void> DatabaseClient::set_user_config(std::string_view key,
                                                   std::string_view value) {
    if (!db_) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }

    auto prep_result = prepare(
        "INSERT INTO user_config (key, value) VALUES (?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!prep_result.ok()) {
        return core::Result<void>::failure(prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    auto b1 = stmt.bind_text(1, key);
    if (!b1.ok()) return b1;

    auto b2 = stmt.bind_text(2, value);
    if (!b2.ok()) return b2;

    auto step_result = stmt.step();
    if (!step_result.ok()) {
        return core::Result<void>::failure(step_result.error());
    }
    return core::Result<void>::success();
}

core::Result<std::string> DatabaseClient::get_user_config(
    std::string_view key) {
    if (!db_) {
        return core::Result<std::string>::failure(
            core::ErrorCode::DatabaseError);
    }

    auto prep_result = prepare("SELECT value FROM user_config WHERE key = ?1");
    if (!prep_result.ok()) {
        return core::Result<std::string>::failure(prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    auto b1 = stmt.bind_text(1, key);
    if (!b1.ok()) {
        return core::Result<std::string>::failure(b1.error());
    }

    auto step_result = stmt.step();
    if (!step_result.ok()) {
        return core::Result<std::string>::failure(step_result.error());
    }
    if (!step_result.value()) {
        return core::Result<std::string>("");
    }
    return core::Result<std::string>(std::string(stmt.column_text(0)));
}

core::Result<void> DatabaseClient::set_user_name(std::string_view name) {
    return set_user_config("user_name", name);
}

core::Result<std::string> DatabaseClient::get_user_name() {
    return get_user_config("user_name");
}

core::Result<void> DatabaseClient::log_message(std::string_view role,
                                               std::string_view content) {
    if (!db_) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }

    auto prep_result = prepare(
        "INSERT INTO chat_history (role, content) VALUES (?1, ?2)");
    if (!prep_result.ok()) {
        return core::Result<void>::failure(prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    auto b1 = stmt.bind_text(1, role);
    if (!b1.ok()) return b1;

    auto b2 = stmt.bind_text(2, content);
    if (!b2.ok()) return b2;

    auto step_result = stmt.step();
    if (!step_result.ok()) {
        return core::Result<void>::failure(step_result.error());
    }
    return core::Result<void>::success();
}

core::Result<std::vector<ChatMessage>> DatabaseClient::get_recent_history(
    int limit) {
    if (!db_) {
        return core::Result<std::vector<ChatMessage>>::failure(
            core::ErrorCode::DatabaseError);
    }
    if (limit <= 0) {
        return core::Result<std::vector<ChatMessage>>(
            std::vector<ChatMessage>{});
    }

    auto prep_result = prepare(
        "SELECT role, content FROM chat_history ORDER BY id DESC LIMIT ?1");
    if (!prep_result.ok()) {
        return core::Result<std::vector<ChatMessage>>::failure(
            prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    auto b1 = stmt.bind_int64(1, limit);
    if (!b1.ok()) {
        return core::Result<std::vector<ChatMessage>>::failure(b1.error());
    }

    std::vector<ChatMessage> rows;
    rows.reserve(static_cast<std::size_t>(limit));

    for (;;) {
        auto step_result = stmt.step();
        if (!step_result.ok()) {
            return core::Result<std::vector<ChatMessage>>::failure(
                step_result.error());
        }
        if (!step_result.value()) {
            break;
        }
        ChatMessage msg;
        msg.role = std::string(stmt.column_text(0));
        msg.content = std::string(stmt.column_text(1));
        rows.push_back(std::move(msg));
    }

    std::reverse(rows.begin(), rows.end());
    return core::Result<std::vector<ChatMessage>>(std::move(rows));
}

core::Result<void> DatabaseClient::clear_chat_history() {
    return execute("DELETE FROM chat_history");
}

}