// SQLite implementation of core::Store. See sqlite_store.hpp for the overall guarantees.
//
// Conventions
//  * Every statement is prepared, bound and finalized through the small RAII `Stmt` helper.
//  * Every sqlite failure is mapped to core::Error{ErrorKind::Storage, sqlite3_errmsg}. Nothing throws
//    out of this file (nlohmann::json use is wrapped in try/catch).
//  * Row contents (subjects, addresses, bodies, tokens) are never logged and never put into error text.
//  * Enums are persisted as their core::to_string text.
//  * Getters that return std::optional / vectors / InboxSyncState have no channel for an error, so a
//    failing read is reported as "absent" / empty / defaults.
#include "slowmail/storage/sqlite_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "schema.hpp"

namespace slowmail::storage {

using core::Error;
using core::ErrorKind;
using core::Status;

namespace {

constexpr const char* kMemoryPath = ":memory:";

Status storage_error(std::string message) {
    return Status(Error{ErrorKind::Storage, std::move(message), 0, std::nullopt, ""});
}

Status not_open_error() { return storage_error("database not open"); }

// RAII wrapper around sqlite3_stmt. Prepare and bind failures are latched: after the first failure
// every further call is a no-op and status() reports the failure (message captured from sqlite).
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (db_ == nullptr) {
            failed_ = true;
            message_ = "database not open";
            return;
        }
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) fail();
    }
    ~Stmt() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return !failed_; }

    // --- binding (1-based indices) ------------------------------------------
    void bind(int i, const std::string& v) {
        if (failed_) return;
        check(sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
    }
    void bind(int i, const char* v) {
        if (failed_) return;
        check(sqlite3_bind_text(stmt_, i, v, -1, SQLITE_TRANSIENT));
    }
    void bind(int i, std::int64_t v) {
        if (failed_) return;
        check(sqlite3_bind_int64(stmt_, i, v));
    }
    void bind(int i, int v) { bind(i, static_cast<std::int64_t>(v)); }
    void bind(int i, bool v) { bind(i, static_cast<std::int64_t>(v ? 1 : 0)); }
    void bind(int i, double v) {
        if (failed_) return;
        check(sqlite3_bind_double(stmt_, i, v));
    }
    void bind_null(int i) {
        if (failed_) return;
        check(sqlite3_bind_null(stmt_, i));
    }
    void bind_blob(int i, const std::string& v) {
        if (failed_) return;
        // sqlite treats a NULL pointer as SQL NULL; data() of an empty string is non-null in practice,
        // but be explicit and use a zero-length blob.
        if (v.empty()) {
            check(sqlite3_bind_zeroblob(stmt_, i, 0));
        } else {
            check(sqlite3_bind_blob(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        }
    }
    void bind(int i, const std::optional<std::string>& v) { v ? bind(i, *v) : bind_null(i); }
    void bind(int i, const std::optional<std::int64_t>& v) { v ? bind(i, *v) : bind_null(i); }
    void bind(int i, const std::optional<double>& v) { v ? bind(i, *v) : bind_null(i); }

    // --- stepping -------------------------------------------------------------
    // Returns true when a row is available. false means done or failed; check ok().
    bool step_row() {
        if (failed_) return false;
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc != SQLITE_DONE) fail();
        return false;
    }
    // Runs a statement that returns no rows.
    Status run() {
        while (step_row()) {
        }
        return status();
    }
    Status status() const { return failed_ ? storage_error(message_) : Status::success(); }

    // --- columns (0-based) ----------------------------------------------------
    bool is_null(int c) const { return sqlite3_column_type(stmt_, c) == SQLITE_NULL; }
    std::string text(int c) const {
        const unsigned char* p = sqlite3_column_text(stmt_, c);
        if (p == nullptr) return std::string();
        return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, c)));
    }
    std::string blob(int c) const {
        const void* p = sqlite3_column_blob(stmt_, c);
        if (p == nullptr) return std::string();
        return std::string(static_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, c)));
    }
    std::int64_t i64(int c) const { return sqlite3_column_int64(stmt_, c); }
    int i32(int c) const { return static_cast<int>(sqlite3_column_int64(stmt_, c)); }
    bool boolean(int c) const { return sqlite3_column_int64(stmt_, c) != 0; }
    double real(int c) const { return sqlite3_column_double(stmt_, c); }
    std::optional<std::string> opt_text(int c) const {
        if (is_null(c)) return std::nullopt;
        return text(c);
    }
    std::optional<std::int64_t> opt_i64(int c) const {
        if (is_null(c)) return std::nullopt;
        return i64(c);
    }
    std::optional<double> opt_real(int c) const {
        if (is_null(c)) return std::nullopt;
        return real(c);
    }

    int changes() const { return db_ ? sqlite3_changes(db_) : 0; }

private:
    void fail() {
        failed_ = true;
        message_ = db_ != nullptr ? sqlite3_errmsg(db_) : "database not open";
    }
    void check(int rc) {
        if (rc != SQLITE_OK) fail();
    }

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    bool failed_ = false;
    std::string message_;
};

// core/types.hpp has no *_from_string for these two enums; invert to_string over the enumerators.
core::SessionState session_state_from_text(const std::string& s) {
    static const core::SessionState kAll[] = {
        core::SessionState::NotStarted, core::SessionState::AuthRequired, core::SessionState::ReadyCached,
        core::SessionState::SyncEligible, core::SessionState::Syncing, core::SessionState::SyncPartial,
        core::SessionState::Active, core::SessionState::Offline, core::SessionState::Ending,
        core::SessionState::Ended};
    for (auto v : kAll) {
        if (s == core::to_string(v)) return v;
    }
    return core::SessionState::NotStarted;
}

core::AttachmentKind attachment_kind_from_text(const std::string& s) {
    static const core::AttachmentKind kAll[] = {core::AttachmentKind::File, core::AttachmentKind::Item,
                                                core::AttachmentKind::Reference, core::AttachmentKind::Unknown};
    for (auto v : kAll) {
        if (s == core::to_string(v)) return v;
    }
    return core::AttachmentKind::Unknown;
}

std::string attachments_to_json(const std::vector<core::AttachmentMeta>& list) {
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& a : list) {
            nlohmann::json o = nlohmann::json::object();
            o["id"] = a.id;
            o["name"] = a.name;
            o["content_type"] = a.content_type;
            o["size"] = a.size;
            o["is_inline"] = a.is_inline;
            o["kind"] = std::string(core::to_string(a.kind));
            arr.push_back(std::move(o));
        }
        return arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception&) {
        return "[]";
    }
}

std::vector<core::AttachmentMeta> attachments_from_json(const std::string& text) {
    std::vector<core::AttachmentMeta> out;
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_array()) return out;
        for (const auto& o : j) {
            if (!o.is_object()) continue;
            core::AttachmentMeta a;
            a.id = o.value("id", std::string());
            a.name = o.value("name", std::string());
            a.content_type = o.value("content_type", std::string());
            a.size = o.value("size", std::int64_t{0});
            a.is_inline = o.value("is_inline", false);
            a.kind = attachment_kind_from_text(o.value("kind", std::string()));
            out.push_back(std::move(a));
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

constexpr const char* kMessageCols =
    "id, conversation_id, subject, from_name, from_address, received_at_iso, received_at_epoch, "
    "is_read, flag_status, body_preview, has_attachments, change_key, parent_folder_id";

core::MessageSummary message_from_row(const Stmt& s) {
    core::MessageSummary m;
    m.id = s.text(0);
    m.conversation_id = s.text(1);
    m.subject = s.text(2);
    m.from.name = s.text(3);
    m.from.address = s.text(4);
    m.received_at_iso = s.text(5);
    m.received_at_epoch = s.i64(6);
    m.is_read = s.boolean(7);
    m.flag_status = core::flag_status_from_string(s.text(8));
    m.body_preview = s.text(9);
    m.has_attachments = s.boolean(10);
    m.change_key = s.text(11);
    m.parent_folder_id = s.text(12);
    return m;
}

constexpr const char* kSessionCols = "id, started_at, ended_at, initial_sync_at, state";

core::SessionRecord session_from_row(const Stmt& s) {
    core::SessionRecord r;
    r.id = s.text(0);
    r.started_at = s.i64(1);
    r.ended_at = s.opt_i64(2);
    r.initial_sync_at = s.opt_i64(3);
    r.state = session_state_from_text(s.text(4));
    return r;
}

constexpr const char* kCommandCols =
    "id, session_id, message_id, type, payload_json, created_at, state, attempts, last_error, "
    "server_result_json, idempotency_key";

core::Command command_from_row(const Stmt& s) {
    core::Command c;
    c.id = s.text(0);
    c.session_id = s.text(1);
    c.message_id = s.text(2);
    c.type = core::command_type_from_string(s.text(3));
    c.payload_json = s.text(4);
    c.created_at = s.i64(5);
    c.state = core::command_state_from_string(s.text(6));
    c.attempts = s.i32(7);
    c.last_error = s.text(8);
    c.server_result_json = s.text(9);
    c.idempotency_key = s.text(10);  // NULL -> ""
    return c;
}

constexpr const char* kEventCols =
    "id, session_id, message_id, type, timestamp, confirmation, display_snapshot_json, command_id, failure_reason";

core::SessionEvent event_from_row(const Stmt& s) {
    core::SessionEvent e;
    e.id = s.text(0);
    e.session_id = s.text(1);
    e.message_id = s.text(2);
    e.type = core::event_type_from_string(s.text(3));
    e.timestamp = s.i64(4);
    e.confirmation = core::confirmation_state_from_string(s.text(5));
    e.display_snapshot_json = s.text(6);
    e.command_id = s.text(7);
    e.failure_reason = s.text(8);
    return e;
}

bool is_memory_path(const std::string& p) { return p.empty() || p == kMemoryPath; }

// unlink; ENOENT is fine.
Status unlink_if_exists(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        return storage_error(std::string("cannot remove database file: ") + std::strerror(errno));
    }
    return Status::success();
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

SqliteStore::SqliteStore(std::string path) : path_(std::move(path)) {}

SqliteStore::~SqliteStore() {
    if (db_ != nullptr) {
        // An open transaction at destruction is abandoned (rolled back by sqlite on close).
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Status SqliteStore::exec(const std::string& sql) {
    if (db_ == nullptr) return not_open_error();
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err != nullptr ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        return storage_error(std::move(msg));
    }
    return Status::success();
}

Status SqliteStore::open() {
    if (db_ != nullptr) return Status::success();  // idempotent
    const bool on_disk = !is_memory_path(path_);

    if (on_disk) {
        // Pre-create with 0600 so the file is never world-readable, even briefly (umask can only
        // remove bits). sqlite gives -wal/-shm the mode of the main file.
        const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) ::close(fd);
    }

    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(path_.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db != nullptr ? sqlite3_errmsg(db) : "cannot open database";
        if (db != nullptr) sqlite3_close_v2(db);
        return storage_error(std::move(msg));
    }
    db_ = db;
    tx_depth_ = 0;
    sqlite3_busy_timeout(db_, 5000);

    Status st = exec("PRAGMA journal_mode=WAL;");
    if (st.ok()) st = exec("PRAGMA synchronous=FULL;");
    if (st.ok()) st = exec("PRAGMA foreign_keys=ON;");
    if (st.ok()) st = exec("PRAGMA secure_delete=ON;");  // freed pages are zeroed (used by secure_wipe)

    if (st.ok() && on_disk) {
        if (::chmod(path_.c_str(), 0600) != 0) {
            st = storage_error(std::string("cannot set database file mode: ") + std::strerror(errno));
        } else {
            // Best effort: companions exist only while the WAL is live.
            (void)::chmod((path_ + "-wal").c_str(), 0600);
            (void)::chmod((path_ + "-shm").c_str(), 0600);
        }
    }
    if (st.ok()) st = migrate();

    if (!st.ok()) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        return st;
    }
    return Status::success();
}

Status SqliteStore::migrate() {
    Status st = exec(kSettingsDdl);
    if (!st.ok()) return st;

    const int current = schema_version();
    if (current > latest_schema_version()) {
        // Forward-only: never guess how to read a newer layout.
        return storage_error("database schema is newer than this application supports");
    }
    for (const Migration& m : migrations()) {
        if (m.version <= current) continue;
        st = exec("BEGIN IMMEDIATE;");
        if (!st.ok()) return st;
        st = exec(m.sql);
        if (st.ok()) {
            Stmt s(db_, "INSERT INTO settings(key, value) VALUES(?, ?) "
                        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
            s.bind(1, kSchemaVersionKey);
            s.bind(2, std::to_string(m.version));
            st = s.run();
        }
        if (st.ok()) {
            st = exec("COMMIT;");
        }
        if (!st.ok()) {
            (void)exec("ROLLBACK;");
            return st;
        }
    }
    return Status::success();
}

int SqliteStore::schema_version() {
    Stmt s(db_, "SELECT value FROM settings WHERE key = ?;");
    s.bind(1, kSchemaVersionKey);
    if (!s.step_row()) return 0;
    const std::string v = s.text(0);
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || n < 0) return 0;
    return static_cast<int>(n);
}

// Transactions: begin() issues BEGIN IMMEDIATE only for the outermost level. Nested begin() calls
// are a savepoint-free counter: they succeed, and only the matching outermost commit() issues COMMIT.
// rollback() always rolls back the WHOLE transaction (there are no savepoints) and resets the depth to
// zero, so a later commit() of an outer level fails with ErrorKind::Invalid instead of silently
// committing partial work. rollback() with no open transaction is a harmless no-op (cleanup paths).
Status SqliteStore::begin() {
    if (db_ == nullptr) return not_open_error();
    if (tx_depth_ > 0) {
        ++tx_depth_;
        return Status::success();
    }
    Status st = exec("BEGIN IMMEDIATE;");
    if (st.ok()) tx_depth_ = 1;
    return st;
}

Status SqliteStore::commit() {
    if (db_ == nullptr) return not_open_error();
    if (tx_depth_ == 0) {
        return Status(Error{ErrorKind::Invalid, "commit without an open transaction", 0, std::nullopt, ""});
    }
    if (tx_depth_ > 1) {
        --tx_depth_;
        return Status::success();
    }
    Status st = exec("COMMIT;");
    if (st.ok()) {
        tx_depth_ = 0;
    } else if (sqlite3_get_autocommit(db_) != 0) {
        tx_depth_ = 0;  // sqlite already ended the transaction; do not leave a phantom open level
    }
    return st;
}

Status SqliteStore::rollback() {
    if (db_ == nullptr) return Status::success();
    if (tx_depth_ == 0) return Status::success();
    tx_depth_ = 0;
    if (sqlite3_get_autocommit(db_) != 0) return Status::success();  // already rolled back by sqlite
    return exec("ROLLBACK;");
}

// secure_wipe guarantees: all rows deleted with secure_delete=ON (freed pages zeroed), WAL truncated,
// file VACUUMed, connection closed, then db, db-wal and db-shm unlinked. Any open transaction is rolled
// back first. Errors during the row/VACUUM phase do not stop the close+unlink phase (deleting the
// files is the point); the first error is still reported. On flash media no physical-erase guarantee
// exists beyond what the filesystem provides (see docs/THREAT_MODEL.md).
Status SqliteStore::secure_wipe() {
    Status first = Status::success();
    auto note = [&first](const Status& s) {
        if (first.ok() && !s.ok()) first = s;
    };

    if (db_ != nullptr) {
        note(rollback());
        note(exec("PRAGMA secure_delete=ON;"));
        note(exec("BEGIN IMMEDIATE;"));
        static const char* const kTables[] = {"message_body", "message_cache", "session_item", "session",
                                              "command",      "session_event", "token_blob",   "account",
                                              "inbox_sync_state", "settings"};
        bool deleted = true;
        for (const char* t : kTables) {
            Status st = exec(std::string("DELETE FROM ") + t + ";");
            if (!st.ok()) {
                note(st);
                deleted = false;
                break;
            }
        }
        if (deleted) {
            note(exec("COMMIT;"));
        } else {
            (void)exec("ROLLBACK;");
        }
        if (!is_memory_path(path_)) {
            note(exec("PRAGMA wal_checkpoint(TRUNCATE);"));
        }
        note(exec("VACUUM;"));
        sqlite3_close_v2(db_);
        db_ = nullptr;
        tx_depth_ = 0;
    }

    if (!is_memory_path(path_)) {
        note(unlink_if_exists(path_));
        note(unlink_if_exists(path_ + "-wal"));
        note(unlink_if_exists(path_ + "-shm"));
    }
    return first;
}

// ---------------------------------------------------------------------------
// settings / account / sync state
// ---------------------------------------------------------------------------

std::optional<std::string> SqliteStore::get_setting(const std::string& key) {
    Stmt s(db_, "SELECT value FROM settings WHERE key = ?;");
    s.bind(1, key);
    if (s.step_row()) return s.text(0);
    return std::nullopt;
}

Status SqliteStore::set_setting(const std::string& key, const std::string& value) {
    Stmt s(db_, "INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    s.bind(1, key);
    s.bind(2, value);
    return s.run();
}

std::optional<core::AccountInfo> SqliteStore::get_account() {
    Stmt s(db_, "SELECT tenant_id, user_id, display_address, signed_in FROM account WHERE id = 1;");
    if (!s.step_row()) return std::nullopt;
    core::AccountInfo a;
    a.tenant_id = s.text(0);
    a.user_id = s.text(1);
    a.display_address = s.text(2);
    a.signed_in = s.boolean(3);
    return a;
}

Status SqliteStore::put_account(const core::AccountInfo& a) {
    Stmt s(db_,
           "INSERT INTO account(id, tenant_id, user_id, display_address, signed_in) VALUES(1, ?, ?, ?, ?) "
           "ON CONFLICT(id) DO UPDATE SET tenant_id = excluded.tenant_id, user_id = excluded.user_id, "
           "display_address = excluded.display_address, signed_in = excluded.signed_in;");
    s.bind(1, a.tenant_id);
    s.bind(2, a.user_id);
    s.bind(3, a.display_address);
    s.bind(4, a.signed_in);
    return s.run();
}

core::InboxSyncState SqliteStore::get_sync_state() {
    core::InboxSyncState st;
    Stmt s(db_,
           "SELECT delta_link, resume_next_link, last_complete_wall, last_complete_mono, last_complete_boot_id, "
           "last_attempt_wall, schema_version FROM inbox_sync_state WHERE id = 1;");
    if (!s.step_row()) return st;
    st.delta_link = s.opt_text(0);
    st.resume_next_link = s.opt_text(1);
    st.last_complete_wall = s.opt_i64(2);
    st.last_complete_mono = s.opt_real(3);
    st.last_complete_boot_id = s.opt_text(4);
    st.last_attempt_wall = s.opt_i64(5);
    st.schema_version = s.i32(6);
    return st;
}

Status SqliteStore::put_sync_state(const core::InboxSyncState& st) {
    Stmt s(db_,
           "INSERT INTO inbox_sync_state(id, delta_link, resume_next_link, last_complete_wall, last_complete_mono, "
           "last_complete_boot_id, last_attempt_wall, schema_version) VALUES(1, ?, ?, ?, ?, ?, ?, ?) "
           "ON CONFLICT(id) DO UPDATE SET delta_link = excluded.delta_link, "
           "resume_next_link = excluded.resume_next_link, last_complete_wall = excluded.last_complete_wall, "
           "last_complete_mono = excluded.last_complete_mono, last_complete_boot_id = excluded.last_complete_boot_id, "
           "last_attempt_wall = excluded.last_attempt_wall, schema_version = excluded.schema_version;");
    s.bind(1, st.delta_link);
    s.bind(2, st.resume_next_link);
    s.bind(3, st.last_complete_wall);
    s.bind(4, st.last_complete_mono);
    s.bind(5, st.last_complete_boot_id);
    s.bind(6, st.last_attempt_wall);
    s.bind(7, st.schema_version);
    return s.run();
}

// ---------------------------------------------------------------------------
// message cache
// ---------------------------------------------------------------------------

// Inserts new rows as InQueue/not-tombstoned. For an existing id only server-sourced columns and
// last_seen_at are refreshed; state, state_reason and tombstone are preserved (a message that is
// GoneOnServer is never resurrected by a late delta entry).
Status SqliteStore::upsert_message(const core::MessageSummary& m) {
    Stmt s(db_,
           "INSERT INTO message_cache(id, conversation_id, subject, from_name, from_address, received_at_iso, "
           "received_at_epoch, is_read, flag_status, body_preview, has_attachments, change_key, parent_folder_id, "
           "state, state_reason, last_seen_at, tombstone) "
           "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'in_queue', '', CAST(strftime('%s','now') AS INTEGER), 0) "
           "ON CONFLICT(id) DO UPDATE SET conversation_id = excluded.conversation_id, subject = excluded.subject, "
           "from_name = excluded.from_name, from_address = excluded.from_address, "
           "received_at_iso = excluded.received_at_iso, received_at_epoch = excluded.received_at_epoch, "
           "is_read = excluded.is_read, flag_status = excluded.flag_status, body_preview = excluded.body_preview, "
           "has_attachments = excluded.has_attachments, change_key = excluded.change_key, "
           "parent_folder_id = excluded.parent_folder_id, last_seen_at = excluded.last_seen_at;");
    s.bind(1, m.id);
    s.bind(2, m.conversation_id);
    s.bind(3, m.subject);
    s.bind(4, m.from.name);
    s.bind(5, m.from.address);
    s.bind(6, m.received_at_iso);
    s.bind(7, m.received_at_epoch);
    s.bind(8, m.is_read);
    s.bind(9, core::to_string(m.flag_status));
    s.bind(10, m.body_preview);
    s.bind(11, m.has_attachments);
    s.bind(12, m.change_key);
    s.bind(13, m.parent_folder_id);
    return s.run();
}

// Unknown id -> ErrorKind::NotFound (a state change on a message we never cached is a caller bug).
Status SqliteStore::set_message_state(const std::string& id, core::MessageState state, const std::string& reason) {
    Stmt s(db_, "UPDATE message_cache SET state = ?, state_reason = ? WHERE id = ?;");
    s.bind(1, core::to_string(state));
    s.bind(2, reason);
    s.bind(3, id);
    Status st = s.run();
    if (st.ok() && s.changes() == 0) {
        return Status(Error{ErrorKind::NotFound, "message not in local cache", 0, std::nullopt, ""});
    }
    return st;
}

// Unknown id -> success no-op: delta sync may report removal of items that were never cached, and the
// call must stay idempotent.
Status SqliteStore::mark_gone(const std::string& id) {
    Stmt s(db_, "UPDATE message_cache SET state = 'gone_on_server', tombstone = 1 WHERE id = ?;");
    s.bind(1, id);
    return s.run();
}

std::optional<core::MessageSummary> SqliteStore::get_message(const std::string& id) {
    const std::string sql = std::string("SELECT ") + kMessageCols + " FROM message_cache WHERE id = ?;";
    Stmt s(db_, sql.c_str());
    s.bind(1, id);
    if (s.step_row()) return message_from_row(s);
    return std::nullopt;
}

std::optional<core::MessageState> SqliteStore::get_message_state(const std::string& id) {
    Stmt s(db_, "SELECT state FROM message_cache WHERE id = ?;");
    s.bind(1, id);
    if (s.step_row()) return core::message_state_from_string(s.text(0));
    return std::nullopt;
}

std::vector<core::MessageSummary> SqliteStore::list_messages_in_state(core::MessageState state) {
    std::vector<core::MessageSummary> out;
    const std::string sql = std::string("SELECT ") + kMessageCols +
                            " FROM message_cache WHERE state = ? ORDER BY received_at_epoch DESC, id;";
    Stmt s(db_, sql.c_str());
    s.bind(1, core::to_string(state));
    while (s.step_row()) out.push_back(message_from_row(s));
    return out;
}

std::vector<core::MessageSummary> SqliteStore::list_queue() {
    std::vector<core::MessageSummary> out;
    const std::string sql = std::string("SELECT ") + kMessageCols +
                            " FROM message_cache WHERE state = 'in_queue' AND tombstone = 0 "
                            "ORDER BY received_at_epoch DESC, id;";
    Stmt s(db_, sql.c_str());
    while (s.step_row()) out.push_back(message_from_row(s));
    return out;
}

// message_body has a foreign key to message_cache: the message must be upserted first, otherwise
// this fails with a Storage error ("FOREIGN KEY constraint failed").
Status SqliteStore::put_body(const core::MessageBody& b) {
    Stmt s(db_,
           "INSERT INTO message_body(message_id, text, was_html, truncated, attachments_json) VALUES(?, ?, ?, ?, ?) "
           "ON CONFLICT(message_id) DO UPDATE SET text = excluded.text, was_html = excluded.was_html, "
           "truncated = excluded.truncated, attachments_json = excluded.attachments_json;");
    s.bind(1, b.id);
    s.bind(2, b.text);
    s.bind(3, b.was_html);
    s.bind(4, b.truncated);
    s.bind(5, attachments_to_json(b.attachments));
    return s.run();
}

std::optional<core::MessageBody> SqliteStore::get_body(const std::string& id) {
    Stmt s(db_, "SELECT message_id, text, was_html, truncated, attachments_json FROM message_body WHERE message_id = ?;");
    s.bind(1, id);
    if (!s.step_row()) return std::nullopt;
    core::MessageBody b;
    b.id = s.text(0);
    b.text = s.text(1);
    b.was_html = s.boolean(2);
    b.truncated = s.boolean(3);
    b.attachments = attachments_from_json(s.text(4));
    return b;
}

Status SqliteStore::purge_tombstones() {
    Status st = begin();  // nested-safe: joins a caller's transaction if one is open
    if (!st.ok()) return st;
    {
        Stmt a(db_, "DELETE FROM message_body WHERE message_id IN (SELECT id FROM message_cache WHERE tombstone = 1);");
        st = a.run();
    }
    if (st.ok()) {
        Stmt b(db_, "DELETE FROM message_cache WHERE tombstone = 1;");
        st = b.run();
    }
    if (!st.ok()) {
        (void)rollback();
        return st;
    }
    return commit();
}

std::size_t SqliteStore::count_messages() {
    Stmt s(db_, "SELECT COUNT(*) FROM message_cache;");
    if (s.step_row()) return static_cast<std::size_t>(s.i64(0));
    return 0;
}

// ---------------------------------------------------------------------------
// sessions
// ---------------------------------------------------------------------------

Status SqliteStore::put_session(const core::SessionRecord& r) {
    Stmt s(db_,
           "INSERT INTO session(id, started_at, ended_at, initial_sync_at, state) VALUES(?, ?, ?, ?, ?) "
           "ON CONFLICT(id) DO UPDATE SET started_at = excluded.started_at, ended_at = excluded.ended_at, "
           "initial_sync_at = excluded.initial_sync_at, state = excluded.state;");
    s.bind(1, r.id);
    s.bind(2, r.started_at);
    s.bind(3, r.ended_at);
    s.bind(4, r.initial_sync_at);
    s.bind(5, core::to_string(r.state));
    return s.run();
}

std::optional<core::SessionRecord> SqliteStore::get_active_session() {
    const std::string sql = std::string("SELECT ") + kSessionCols +
                            " FROM session WHERE state != 'ended' ORDER BY started_at DESC, id DESC LIMIT 1;";
    Stmt s(db_, sql.c_str());
    if (s.step_row()) return session_from_row(s);
    return std::nullopt;
}

std::optional<core::SessionRecord> SqliteStore::get_session(const std::string& id) {
    const std::string sql = std::string("SELECT ") + kSessionCols + " FROM session WHERE id = ?;";
    Stmt s(db_, sql.c_str());
    s.bind(1, id);
    if (s.step_row()) return session_from_row(s);
    return std::nullopt;
}

Status SqliteStore::put_session_item(const core::SessionItem& item) {
    Stmt s(db_,
           "INSERT INTO session_item(session_id, message_id, initial_state, current_state, suppression_reason) "
           "VALUES(?, ?, ?, ?, ?) ON CONFLICT(session_id, message_id) DO UPDATE SET "
           "initial_state = excluded.initial_state, current_state = excluded.current_state, "
           "suppression_reason = excluded.suppression_reason;");
    s.bind(1, item.session_id);
    s.bind(2, item.message_id);
    s.bind(3, core::to_string(item.initial_state));
    s.bind(4, core::to_string(item.current_state));
    s.bind(5, item.suppression_reason);
    return s.run();
}

// Insertion order (rowid); an update of an existing (session, message) pair keeps its position.
std::vector<core::SessionItem> SqliteStore::list_session_items(const std::string& session_id) {
    std::vector<core::SessionItem> out;
    Stmt s(db_,
           "SELECT session_id, message_id, initial_state, current_state, suppression_reason "
           "FROM session_item WHERE session_id = ? ORDER BY rowid;");
    s.bind(1, session_id);
    while (s.step_row()) {
        core::SessionItem it;
        it.session_id = s.text(0);
        it.message_id = s.text(1);
        it.initial_state = core::message_state_from_string(s.text(2));
        it.current_state = core::message_state_from_string(s.text(3));
        it.suppression_reason = s.text(4);
        out.push_back(std::move(it));
    }
    return out;
}

// ---------------------------------------------------------------------------
// command journal
// ---------------------------------------------------------------------------

// Insert or replace by id. An empty idempotency_key is stored as NULL so any number of key-less
// commands can coexist; a non-empty key already owned by a DIFFERENT command id violates the UNIQUE
// constraint and returns a Storage error (callers use find_command_by_idempotency_key first).
Status SqliteStore::put_command(const core::Command& c) {
    Stmt s(db_,
           "INSERT INTO command(id, session_id, message_id, type, payload_json, created_at, state, attempts, "
           "last_error, server_result_json, idempotency_key) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
           "ON CONFLICT(id) DO UPDATE SET session_id = excluded.session_id, message_id = excluded.message_id, "
           "type = excluded.type, payload_json = excluded.payload_json, created_at = excluded.created_at, "
           "state = excluded.state, attempts = excluded.attempts, last_error = excluded.last_error, "
           "server_result_json = excluded.server_result_json, idempotency_key = excluded.idempotency_key;");
    s.bind(1, c.id);
    s.bind(2, c.session_id);
    s.bind(3, c.message_id);
    s.bind(4, core::to_string(c.type));
    s.bind(5, c.payload_json);
    s.bind(6, c.created_at);
    s.bind(7, core::to_string(c.state));
    s.bind(8, c.attempts);
    s.bind(9, c.last_error);
    s.bind(10, c.server_result_json);
    if (c.idempotency_key.empty()) {
        s.bind_null(11);
    } else {
        s.bind(11, c.idempotency_key);
    }
    return s.run();
}

std::optional<core::Command> SqliteStore::get_command(const std::string& id) {
    const std::string sql = std::string("SELECT ") + kCommandCols + " FROM command WHERE id = ?;";
    Stmt s(db_, sql.c_str());
    s.bind(1, id);
    if (s.step_row()) return command_from_row(s);
    return std::nullopt;
}

std::vector<core::Command> SqliteStore::list_commands(core::CommandState state) {
    std::vector<core::Command> out;
    const std::string sql =
        std::string("SELECT ") + kCommandCols + " FROM command WHERE state = ? ORDER BY created_at ASC, id;";
    Stmt s(db_, sql.c_str());
    s.bind(1, core::to_string(state));
    while (s.step_row()) out.push_back(command_from_row(s));
    return out;
}

std::vector<core::Command> SqliteStore::list_commands_for_message(const std::string& message_id) {
    std::vector<core::Command> out;
    const std::string sql =
        std::string("SELECT ") + kCommandCols + " FROM command WHERE message_id = ? ORDER BY created_at ASC, id;";
    Stmt s(db_, sql.c_str());
    s.bind(1, message_id);
    while (s.step_row()) out.push_back(command_from_row(s));
    return out;
}

std::optional<core::Command> SqliteStore::find_command_by_idempotency_key(const std::string& key) {
    if (key.empty()) return std::nullopt;  // empty key means "no key"
    const std::string sql = std::string("SELECT ") + kCommandCols + " FROM command WHERE idempotency_key = ?;";
    Stmt s(db_, sql.c_str());
    s.bind(1, key);
    if (s.step_row()) return command_from_row(s);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// session events
// ---------------------------------------------------------------------------

Status SqliteStore::put_event(const core::SessionEvent& e) {
    Stmt s(db_,
           "INSERT INTO session_event(id, session_id, message_id, type, timestamp, confirmation, "
           "display_snapshot_json, command_id, failure_reason) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
           "ON CONFLICT(id) DO UPDATE SET session_id = excluded.session_id, message_id = excluded.message_id, "
           "type = excluded.type, timestamp = excluded.timestamp, confirmation = excluded.confirmation, "
           "display_snapshot_json = excluded.display_snapshot_json, command_id = excluded.command_id, "
           "failure_reason = excluded.failure_reason;");
    s.bind(1, e.id);
    s.bind(2, e.session_id);
    s.bind(3, e.message_id);
    s.bind(4, core::to_string(e.type));
    s.bind(5, e.timestamp);
    s.bind(6, core::to_string(e.confirmation));
    s.bind(7, e.display_snapshot_json);
    s.bind(8, e.command_id);
    s.bind(9, e.failure_reason);
    return s.run();
}

std::vector<core::SessionEvent> SqliteStore::list_events(const std::string& session_id) {
    std::vector<core::SessionEvent> out;
    const std::string sql = std::string("SELECT ") + kEventCols +
                            " FROM session_event WHERE session_id = ? ORDER BY timestamp DESC, id DESC;";
    Stmt s(db_, sql.c_str());
    s.bind(1, session_id);
    while (s.step_row()) out.push_back(event_from_row(s));
    return out;
}

// ---------------------------------------------------------------------------
// token blob
// ---------------------------------------------------------------------------

std::optional<std::string> SqliteStore::get_token_blob() {
    Stmt s(db_, "SELECT blob FROM token_blob WHERE id = 1;");
    if (s.step_row()) return s.blob(0);
    return std::nullopt;
}

Status SqliteStore::put_token_blob(const std::string& blob) {
    Stmt s(db_, "INSERT INTO token_blob(id, blob) VALUES(1, ?) ON CONFLICT(id) DO UPDATE SET blob = excluded.blob;");
    s.bind_blob(1, blob);
    return s.run();
}

Status SqliteStore::clear_token_blob() {
    Stmt s(db_, "DELETE FROM token_blob;");
    return s.run();
}

} // namespace slowmail::storage
