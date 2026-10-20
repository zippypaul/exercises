#include <gtest/gtest.h>

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "slowmail/storage/sqlite_store.hpp"

using namespace slowmail;
using storage::SqliteStore;

namespace {

struct Dir {
    std::string path;
    Dir() {
        char tmpl[] = "/tmp/slowmail_migration_XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        path = p != nullptr ? p : "";
    }
    ~Dir() {
        for (const char* s : {"/mail.db", "/mail.db-wal", "/mail.db-shm"}) ::unlink((path + s).c_str());
        ::rmdir(path.c_str());
    }
    std::string db() const { return path + "/mail.db"; }
};

} // namespace

TEST(Migration, DowngradedVersionIsMigratedForwardIdempotentlyWithoutDataLoss) {
    Dir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_EQ(s.schema_version(), 2);
        core::MessageSummary m;
        m.id = "keep-me";
        m.subject = "hello";
        ASSERT_TRUE(s.upsert_message(m).ok());
        ASSERT_TRUE(s.put_token_blob("tok").ok());
        ASSERT_TRUE(s.set_setting("schema_version", "0").ok());
        EXPECT_EQ(s.schema_version(), 0);
    }
    for (int round = 0; round < 3; ++round) {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok()) << "round " << round;
        EXPECT_EQ(s.schema_version(), 2);
        ASSERT_TRUE(s.get_message("keep-me").has_value());
        EXPECT_EQ(s.get_message("keep-me")->subject, "hello");
        EXPECT_EQ(s.get_token_blob().value_or(""), "tok");
        EXPECT_EQ(s.count_messages(), 1u);
    }
}

TEST(Migration, FreshDatabaseGetsAllTablesFromEmptyFile) {
    Dir dir;
    // An empty pre-existing file (e.g. created by touch) must be treated as a new database.
    FILE* f = std::fopen(dir.db().c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fclose(f);
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_EQ(s.schema_version(), 2);
    EXPECT_TRUE(s.set_setting("a", "b").ok());
}

TEST(Migration, MissingVersionRowWithExistingTablesIsRecovered) {
    Dir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_TRUE(s.set_setting("keep", "yes").ok());
    }
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(dir.db().c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, "DELETE FROM settings WHERE key='schema_version';", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_EQ(s.schema_version(), 2);
    EXPECT_EQ(s.get_setting("keep").value_or(""), "yes");
}

// v1 -> v2: repairs tombstone flags left inconsistent with state by the F4 bug.
TEST(Migration, V2RepairsTombstoneFlagsInconsistentWithState) {
    Dir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        for (const char* id : {"restored", "gone", "plain"}) {
            core::MessageSummary m;
            m.id = id;
            ASSERT_TRUE(s.upsert_message(m).ok());
        }
        ASSERT_TRUE(s.mark_gone("gone").ok());
    }
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(dir.db().c_str(), &db), SQLITE_OK);
        // Reproduce the v1 bug: tombstone stuck at 1 on a restored message; and a gone one lacking it.
        ASSERT_EQ(sqlite3_exec(db,
                               "UPDATE message_cache SET tombstone = 1 WHERE id = 'restored';"
                               "UPDATE message_cache SET tombstone = 0 WHERE id = 'gone';"
                               "UPDATE settings SET value = '1' WHERE key = 'schema_version';",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(db);
    }
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_EQ(s.schema_version(), 2);
    std::vector<std::string> q;
    for (const auto& m : s.list_queue()) q.push_back(m.id);
    std::sort(q.begin(), q.end());
    EXPECT_EQ(q, (std::vector<std::string>{"plain", "restored"}));
    ASSERT_TRUE(s.purge_tombstones().ok());
    EXPECT_TRUE(s.get_message("restored").has_value());
    EXPECT_TRUE(s.get_message("plain").has_value());
    EXPECT_FALSE(s.get_message("gone").has_value());
}
