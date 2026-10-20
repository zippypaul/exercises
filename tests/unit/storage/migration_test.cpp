#include <gtest/gtest.h>

#include <sqlite3.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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
        ASSERT_EQ(s.schema_version(), 1);
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
        EXPECT_EQ(s.schema_version(), 1);
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
    EXPECT_EQ(s.schema_version(), 1);
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
    EXPECT_EQ(s.schema_version(), 1);
    EXPECT_EQ(s.get_setting("keep").value_or(""), "yes");
}
