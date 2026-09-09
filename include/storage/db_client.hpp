#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace ddci::storage {

class StatementHandle {
public:
    explicit StatementHandle(sqlite3_stmt* stmt);
    ~StatementHandle();

    StatementHandle(const StatementHandle&) = delete;
    StatementHandle& operator=(const StatementHandle&) = delete;

    StatementHandle(StatementHandle&& other) noexcept;
    StatementHandle& operator=(StatementHandle&& other) noexcept;

    [[nodiscard]] core::Result<void> bind_text(int index, std::string_view value);
    [[nodiscard]] core::Result<void> bind_int64(int index, int64_t value);
    [[nodiscard]] core::Result<void> bind_double(int index, double value);
    [[nodiscard]] core::Result<void> bind_null(int index);

    [[nodiscard]] core::Result<bool> step();
    [[nodiscard]] core::Result<void> reset();

    [[nodiscard]] std::string_view column_text(int col) const;
    [[nodiscard]] int64_t column_int64(int col) const;
    [[nodiscard]] double column_double(int col) const;
    [[nodiscard]] bool column_is_null(int col) const;

    [[nodiscard]] sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_{nullptr};
};

class DatabaseClient {
public:
    explicit DatabaseClient(std::string db_path);
    ~DatabaseClient();

    DatabaseClient(const DatabaseClient&) = delete;
    DatabaseClient& operator=(const DatabaseClient&) = delete;

    DatabaseClient(DatabaseClient&& other) noexcept;
    DatabaseClient& operator=(DatabaseClient&& other) noexcept;

    [[nodiscard]] core::Result<void> execute(std::string_view sql);
    [[nodiscard]] core::Result<StatementHandle> prepare(std::string_view sql);
    [[nodiscard]] core::Result<void> init_schema();
    [[nodiscard]] core::Result<void> insert_l2_summary(
        std::string_view category, std::string_view content, double importance);

    void close();

    [[nodiscard]] bool is_open() const { return db_ != nullptr; }
    [[nodiscard]] const std::string& path() const { return db_path_; }

private:
    [[nodiscard]] core::Result<void> apply_pragmas();

    std::string db_path_;
    sqlite3* db_{nullptr};
};

}