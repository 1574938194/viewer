#ifndef NEXUS_SRC_DATABASE_SQLITE_DATABASE_H_
#define NEXUS_SRC_DATABASE_SQLITE_DATABASE_H_

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace nexus::db {

// Thin RAII wrapper over a prepared statement.
class Statement
{
public:
    Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    bool Prepare(sqlite3* db, const char* sql);

    void BindInt(int index, std::int64_t value);
    void BindText(int index, const std::string& value);
    void BindDouble(int index, double value);

    // Returns true when a row is available.
    bool StepRow();

    // Returns true on SQLITE_DONE (statement finished without a row).
    bool StepDone();

    void Reset();

    std::int64_t ColumnInt(int index);
    std::string ColumnText(int index);
    double ColumnDouble(int index);

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// Owns the SQLite connection and the schema version (design doc section 63).
class Database
{
public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    bool Open(const std::filesystem::path& file);

    void Close();

    sqlite3* handle() const { return db_; }

    bool Exec(const char* sql);

    int user_version();

    bool set_user_version(int version);

    bool BackupTo(const std::filesystem::path& destination);

private:
    sqlite3* db_ = nullptr;
};

}  // namespace nexus::db

#endif  // NEXUS_SRC_DATABASE_SQLITE_DATABASE_H_
