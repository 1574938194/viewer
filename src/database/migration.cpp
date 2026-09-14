#include "database/migration.h"

#include "database/sqlite_database.h"

namespace nexus::db {

namespace {

// Base schema (design doc sections 16-21, 47, 73). Relations cascade so a
// resource delete removes its tags/actors/history in one statement.
const char* const kSchema[] = {
    "CREATE TABLE IF NOT EXISTS categories"
    "(id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",

    "CREATE TABLE IF NOT EXISTS resources("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "resource_type INTEGER NOT NULL,"
    "path TEXT UNIQUE NOT NULL,"
    "original_filename TEXT NOT NULL,"
    "title TEXT,"
    "code TEXT,"
    "category_id INTEGER REFERENCES categories(id),"
    "file_size INTEGER DEFAULT 0,"
    "width INTEGER DEFAULT 0,"
    "height INTEGER DEFAULT 0,"
    "duration INTEGER DEFAULT 0,"
    "content_hash TEXT DEFAULT '',"
    "added_time INTEGER DEFAULT 0,"
    "modified_time INTEGER DEFAULT 0,"
    "last_open_time INTEGER DEFAULT 0,"
    "total_open_count INTEGER DEFAULT 0,"
    "hot_score REAL DEFAULT 0,"
    "favorite INTEGER DEFAULT 0,"
    "thumbnail_key TEXT,"
    "missing INTEGER DEFAULT 0);",

    "CREATE TABLE IF NOT EXISTS tags"
    "(id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",

    "CREATE TABLE IF NOT EXISTS tag_alias"
    "(alias TEXT PRIMARY KEY,"
    "tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE);",

    "CREATE TABLE IF NOT EXISTS resource_tags("
    "resource_id INTEGER NOT NULL REFERENCES resources(id) ON DELETE CASCADE,"
    "tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
    "weight REAL DEFAULT 1.0,"
    "PRIMARY KEY(resource_id, tag_id));",

    "CREATE TABLE IF NOT EXISTS actors"
    "(id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);",

    "CREATE TABLE IF NOT EXISTS resource_actors("
    "resource_id INTEGER NOT NULL REFERENCES resources(id) ON DELETE CASCADE,"
    "actor_id INTEGER NOT NULL REFERENCES actors(id) ON DELETE CASCADE,"
    "PRIMARY KEY(resource_id, actor_id));",

    "CREATE TABLE IF NOT EXISTS open_history("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "resource_id INTEGER NOT NULL REFERENCES resources(id) ON DELETE CASCADE,"
    "open_time INTEGER NOT NULL);",

    "CREATE INDEX IF NOT EXISTS idx_open_history_resource_time"
    " ON open_history(resource_id, open_time);",

    "CREATE TABLE IF NOT EXISTS operation_log("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "operation TEXT NOT NULL,"
    "source_path TEXT,"
    "target_path TEXT,"
    "resource_id INTEGER,"
    "time INTEGER NOT NULL,"
    "success INTEGER NOT NULL);",

    // Duplicate detection is keyed by the cheap, deterministic fingerprint
    // available during import. Keep the lookup indexed for large libraries.
    "CREATE INDEX IF NOT EXISTS idx_resources_duplicate"
    " ON resources(file_size, original_filename COLLATE NOCASE);",

    "CREATE INDEX IF NOT EXISTS idx_resources_path_nocase"
    " ON resources(path COLLATE NOCASE);",

    "CREATE INDEX IF NOT EXISTS idx_operation_log_status_time"
    " ON operation_log(success, time DESC);",

};

// Version 7 makes the FTS5 index a mandatory part of the on-disk schema.
// Databases created by the earlier optional-FTS build are upgraded below.
constexpr int kCurrentVersion = 7;

}  // namespace

bool Migrate(Database& database)
{
    const int version = database.user_version();
    if (version < 0)
    {
        return false;
    }
    if (sqlite3_compileoption_used("ENABLE_FTS5") == 0)
    {
        // Search is deliberately FTS5-only. Do not silently fall back to a
        // CPU scan when an incompatible SQLite DLL is deployed.
        return false;
    }
    if (version >= kCurrentVersion)
    {
        Statement ftsCheck;
        if (!ftsCheck.Prepare(
                database.handle(),
                "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                "name='resource_fts' LIMIT 1;"))
        {
            return false;
        }
        if (ftsCheck.StepRow())
        {
            return true;
        }
        // A database with a newer version is not safe to downgrade. A v7
        // database without its mandatory index is repaired by this migration.
        if (version > kCurrentVersion)
        {
            return false;
        }
    }

    if (!database.Exec("BEGIN;"))
    {
        return false;
    }
    for (const char* sql : kSchema)
    {
        if (!database.Exec(sql))
        {
            database.Exec("ROLLBACK;");
            return false;
        }
    }

    // The column was added after v1 installations already existed. The
    // CREATE TABLE above covers fresh databases; inspect legacy schemas before
    // issuing ALTER TABLE so migration is idempotent.
    bool hasContentHash = false;
    Statement columns;
    if (!columns.Prepare(database.handle(), "PRAGMA table_info(resources);"))
    {
        database.Exec("ROLLBACK;");
        return false;
    }
    while (columns.StepRow())
    {
        if (columns.ColumnText(1) == "content_hash")
        {
            hasContentHash = true;
            break;
        }
    }
    if (!hasContentHash &&
        !database.Exec("ALTER TABLE resources ADD COLUMN "
                       "content_hash TEXT DEFAULT '';"))
    {
        database.Exec("ROLLBACK;");
        return false;
    }
    if (!database.Exec("CREATE INDEX IF NOT EXISTS idx_resources_content_hash "
                       "ON resources(file_size, content_hash) "
                       "WHERE content_hash <> '';"))
    {
        database.Exec("ROLLBACK;");
        return false;
    }
    const char* const ftsSchema[] = {
        "CREATE VIEW IF NOT EXISTS resource_search_content AS "
        "SELECT r.id AS id, COALESCE(r.title,'') AS title, "
        "COALESCE(r.code,'') AS code, "
        "COALESCE(r.original_filename,'') AS original_filename, "
        "COALESCE(c.name,'') AS category, "
        "COALESCE((SELECT group_concat(t.name,' ') FROM resource_tags rt "
        "JOIN tags t ON t.id=rt.tag_id WHERE rt.resource_id=r.id),'') AS tags, "
        "COALESCE((SELECT group_concat(a.name,' ') FROM resource_actors ra "
        "JOIN actors a ON a.id=ra.actor_id WHERE ra.resource_id=r.id),'') AS actors "
        "FROM resources r LEFT JOIN categories c ON c.id=r.category_id;",

        "CREATE VIRTUAL TABLE IF NOT EXISTS resource_fts USING fts5("
        "title, code, original_filename, category, tags, actors,"
        " tokenize='unicode61 remove_diacritics 2');",

        "CREATE TRIGGER IF NOT EXISTS resources_fts_ai AFTER INSERT ON resources "
        "BEGIN INSERT INTO resource_fts(rowid,title,code,original_filename,"
        "category,tags,actors) SELECT id,title,code,original_filename,category,"
        "tags,actors FROM resource_search_content WHERE id=NEW.id; END;",

        "CREATE TRIGGER IF NOT EXISTS resources_fts_au AFTER UPDATE OF title,code,"
        "original_filename,category_id ON resources BEGIN "
        "DELETE FROM resource_fts WHERE rowid=OLD.id; "
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content WHERE id=NEW.id; END;",

        "CREATE TRIGGER IF NOT EXISTS resources_fts_ad AFTER DELETE ON resources "
        "BEGIN DELETE FROM resource_fts WHERE rowid=OLD.id; END;",

        "CREATE TRIGGER IF NOT EXISTS resource_tags_fts_ai AFTER INSERT ON "
        "resource_tags BEGIN DELETE FROM resource_fts WHERE rowid=NEW.resource_id; "
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content WHERE id=NEW.resource_id; END;",

        "CREATE TRIGGER IF NOT EXISTS resource_tags_fts_ad AFTER DELETE ON "
        "resource_tags BEGIN DELETE FROM resource_fts WHERE rowid=OLD.resource_id; "
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content WHERE id=OLD.resource_id; END;",

        "CREATE TRIGGER IF NOT EXISTS resource_actors_fts_ai AFTER INSERT ON "
        "resource_actors BEGIN DELETE FROM resource_fts WHERE rowid=NEW.resource_id; "
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content WHERE id=NEW.resource_id; END;",

        "CREATE TRIGGER IF NOT EXISTS resource_actors_fts_ad AFTER DELETE ON "
        "resource_actors BEGIN DELETE FROM resource_fts WHERE rowid=OLD.resource_id; "
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content WHERE id=OLD.resource_id; END;",

        "DELETE FROM resource_fts;",
        "INSERT INTO resource_fts(rowid,title,code,original_filename,category,"
        "tags,actors) SELECT id,title,code,original_filename,category,tags,actors "
        "FROM resource_search_content;",
    };
    for (const char* sql : ftsSchema)
    {
        if (!database.Exec(sql))
        {
            database.Exec("ROLLBACK;");
            return false;
        }
    }
    if (!database.set_user_version(kCurrentVersion) || !database.Exec("COMMIT;"))
    {
        database.Exec("ROLLBACK;");
        return false;
    }
    return true;
}

}  // namespace nexus::db
