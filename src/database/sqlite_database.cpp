#include "database/sqlite_database.h"

#include <Windows.h>


#include "util/string_util.h"

namespace nexus::db {

Statement::~Statement()
{
    if (stmt_ != nullptr)
    {
        sqlite3_finalize(stmt_);
    }
}

bool Statement::Prepare(sqlite3* db, const char* sql)
{
    if (stmt_ != nullptr)
    {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    return sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
}

void Statement::BindInt(int index, std::int64_t value)
{
    sqlite3_bind_int64(stmt_, index, value);
}

void Statement::BindText(int index, const std::string& value)
{
    // SQLITE_TRANSIENT lets SQLite copy the buffer immediately.
    sqlite3_bind_text(stmt_, index, value.c_str(),
                      static_cast<int>(value.size()), reinterpret_cast<sqlite3_destructor_type>(-1));
}

void Statement::BindDouble(int index, double value)
{
    sqlite3_bind_double(stmt_, index, value);
}

bool Statement::StepRow()
{
    return sqlite3_step(stmt_) == SQLITE_ROW;
}

bool Statement::StepDone()
{
    return sqlite3_step(stmt_) == SQLITE_DONE;
}

void Statement::Reset()
{
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

std::int64_t Statement::ColumnInt(int index)
{
    return sqlite3_column_int64(stmt_, index);
}

std::string Statement::ColumnText(int index)
{
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    if (text == nullptr)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(sqlite3_column_bytes(stmt_, index)));
}

double Statement::ColumnDouble(int index)
{
    return sqlite3_column_double(stmt_, index);
}

Database::~Database()
{
    Close();
}

bool Database::Open(const std::filesystem::path& file)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    if (sqlite3_open16(file.c_str(), &db_) != SQLITE_OK)
    {
        Close();
        return false;
    }
    // UI and the import worker share this connection; WAL readers/writers
    // may briefly collide, so wait instead of failing with SQLITE_BUSY.
    sqlite3_busy_timeout(db_, 5000);
    // Recommended configuration (design doc section 22).
    static const char* const kPragmas[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "PRAGMA foreign_keys=ON;",
        "PRAGMA temp_store=MEMORY;",
    };
    for (const char* pragma : kPragmas)
    {
        if (!Exec(pragma))
        {
            return false;
        }
    }
    return true;
}

void Database::Close()
{
    if (db_ != nullptr)
    {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

bool Database::Exec(const char* sql)
{
    char* error = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (error != nullptr)
    {
        sqlite3_free(error);
    }
    return rc == SQLITE_OK;
}

int Database::user_version()
{
    Statement statement;
    if (!statement.Prepare(db_, "PRAGMA user_version;"))
    {
        return -1;
    }
    if (!statement.StepRow())
    {
        return -1;
    }
    return static_cast<int>(statement.ColumnInt(0));
}

bool Database::set_user_version(int version)
{
    char sql[64] = {};
    sprintf_s(sql, "PRAGMA user_version = %d;", version);
    return Exec(sql);
}

bool Database::BackupTo(const std::filesystem::path& destination)
{
    if (db_ == nullptr || destination.empty())
    {
        return false;
    }
    std::error_code directoryError;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, directoryError);
    }
    if (directoryError)
    {
        return false;
    }
    const std::filesystem::path temporary =
        destination.wstring() + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    std::error_code cleanupError;
    std::filesystem::remove(temporary, cleanupError);
    sqlite3* backupDatabase = nullptr;
    if (sqlite3_open16(temporary.c_str(), &backupDatabase) != SQLITE_OK)
    {
        if (backupDatabase != nullptr)
        {
            sqlite3_close_v2(backupDatabase);
        }
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    sqlite3_backup* backup = sqlite3_backup_init(
        backupDatabase, "main", db_, "main");
    if (backup == nullptr)
    {
        sqlite3_close_v2(backupDatabase);
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    int result = SQLITE_OK;
    do
    {
        result = sqlite3_backup_step(backup, 64);
        if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
        {
            sqlite3_sleep(10);
        }
    } while (result == SQLITE_BUSY || result == SQLITE_LOCKED);
    const int finish = sqlite3_backup_finish(backup);
    const bool success = (result == SQLITE_DONE || result == SQLITE_OK) &&
                         finish == SQLITE_OK;
    sqlite3_close_v2(backupDatabase);
    if (!success)
    {
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

}  // namespace nexus::db
