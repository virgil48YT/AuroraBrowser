// Aurora — schema migrations.
//
// Migrations are versioned and applied in order. The current schema version
// is stored in the database (PRAGMA user_version). Each migration runs inside
// its own transaction so a failure leaves the database at the previous,
// consistent version rather than half-migrated.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::storage {

// A migration brings the schema from version N to N+1.
struct Migration {
    int from_version;
    std::string description;
    std::function<DbError(Database&)> apply;
};

class Migrator {
public:
    // Returns the built-in migration list for the browser profile schema.
    static std::vector<Migration> builtin_migrations();

    // Apply all pending migrations to reach the latest version. Returns the
    // final version (or the error code on failure).
    static DbError migrate(Database& db);
};

} // namespace aurora::storage
