// Private to slowmail_storage: versioned schema and forward-only migrations.
#pragma once
#include <vector>

namespace slowmail::storage {

struct Migration {
    int version;       // schema version reached after applying `sql`
    const char* sql;   // one or more statements; MUST be idempotent (IF NOT EXISTS)
};

// Ordered by ascending version, versions are contiguous starting at 1.
const std::vector<Migration>& migrations();

// Highest version in migrations().
int latest_schema_version();

// Key of the settings row that stores the applied schema version.
inline constexpr const char* kSchemaVersionKey = "schema_version";

// Bootstrap DDL for the settings table (needed before the version can be read).
extern const char* const kSettingsDdl;

} // namespace slowmail::storage
