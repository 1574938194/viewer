#ifndef NEXUS_SRC_DATABASE_MIGRATION_H_
#define NEXUS_SRC_DATABASE_MIGRATION_H_

namespace nexus::db {

class Database;

// Brings the schema up to the latest version via PRAGMA user_version
// (design doc section 63).
bool Migrate(Database& database);

}  // namespace nexus::db

#endif  // NEXUS_SRC_DATABASE_MIGRATION_H_
