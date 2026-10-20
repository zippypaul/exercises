// Versioned schema. Enums are stored as their core::to_string text.
// Rules for adding a migration: append a new entry with version = previous + 1, never edit an
// existing entry, and keep the SQL idempotent so re-running after a manual version downgrade is safe.
#include "schema.hpp"

namespace slowmail::storage {

const char* const kSettingsDdl =
    "CREATE TABLE IF NOT EXISTS settings ("
    "  key   TEXT PRIMARY KEY NOT NULL,"
    "  value TEXT NOT NULL"
    ");";

namespace {

const char* const kV1 = R"SQL(
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS account (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    tenant_id       TEXT NOT NULL,
    user_id         TEXT NOT NULL,
    display_address TEXT NOT NULL,
    signed_in       INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS inbox_sync_state (
    id                    INTEGER PRIMARY KEY CHECK (id = 1),
    delta_link            TEXT,
    resume_next_link      TEXT,
    last_complete_wall    INTEGER,
    last_complete_mono    REAL,
    last_complete_boot_id TEXT,
    last_attempt_wall     INTEGER,
    schema_version        INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS message_cache (
    id                TEXT PRIMARY KEY NOT NULL,
    conversation_id   TEXT NOT NULL DEFAULT '',
    subject           TEXT NOT NULL DEFAULT '',
    from_name         TEXT NOT NULL DEFAULT '',
    from_address      TEXT NOT NULL DEFAULT '',
    received_at_iso   TEXT NOT NULL DEFAULT '',
    received_at_epoch INTEGER NOT NULL DEFAULT 0,
    is_read           INTEGER NOT NULL DEFAULT 0,
    flag_status       TEXT NOT NULL DEFAULT 'notFlagged',
    body_preview      TEXT NOT NULL DEFAULT '',
    has_attachments   INTEGER NOT NULL DEFAULT 0,
    change_key        TEXT NOT NULL DEFAULT '',
    parent_folder_id  TEXT NOT NULL DEFAULT '',
    state             TEXT NOT NULL DEFAULT 'in_queue',
    state_reason      TEXT NOT NULL DEFAULT '',
    last_seen_at      INTEGER NOT NULL DEFAULT 0,
    tombstone         INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_message_cache_state_received
    ON message_cache (state, received_at_epoch DESC);

CREATE TABLE IF NOT EXISTS message_body (
    message_id       TEXT PRIMARY KEY NOT NULL
                     REFERENCES message_cache(id) ON DELETE CASCADE,
    text             TEXT NOT NULL DEFAULT '',
    was_html         INTEGER NOT NULL DEFAULT 0,
    truncated        INTEGER NOT NULL DEFAULT 0,
    attachments_json TEXT NOT NULL DEFAULT '[]'
);

CREATE TABLE IF NOT EXISTS session (
    id              TEXT PRIMARY KEY NOT NULL,
    started_at      INTEGER NOT NULL,
    ended_at        INTEGER,
    initial_sync_at INTEGER,
    state           TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS session_item (
    session_id         TEXT NOT NULL,
    message_id         TEXT NOT NULL,
    initial_state      TEXT NOT NULL,
    current_state      TEXT NOT NULL,
    suppression_reason TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (session_id, message_id)
);

CREATE TABLE IF NOT EXISTS command (
    id                 TEXT PRIMARY KEY NOT NULL,
    session_id         TEXT NOT NULL,
    message_id         TEXT NOT NULL,
    type               TEXT NOT NULL,
    payload_json       TEXT NOT NULL DEFAULT '{}',
    created_at         INTEGER NOT NULL,
    state              TEXT NOT NULL,
    attempts           INTEGER NOT NULL DEFAULT 0,
    last_error         TEXT NOT NULL DEFAULT '',
    server_result_json TEXT NOT NULL DEFAULT '',
    idempotency_key    TEXT UNIQUE          -- NULL (not '') when the caller supplied no key
);
CREATE INDEX IF NOT EXISTS idx_command_state_created ON command (state, created_at);
CREATE INDEX IF NOT EXISTS idx_command_message ON command (message_id, created_at);

CREATE TABLE IF NOT EXISTS session_event (
    id                    TEXT PRIMARY KEY NOT NULL,
    session_id            TEXT NOT NULL,
    message_id            TEXT NOT NULL,
    type                  TEXT NOT NULL,
    timestamp             INTEGER NOT NULL,
    confirmation          TEXT NOT NULL,
    display_snapshot_json TEXT NOT NULL DEFAULT '',
    command_id            TEXT NOT NULL DEFAULT '',
    failure_reason        TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_session_event_session_ts ON session_event (session_id, timestamp DESC);

CREATE TABLE IF NOT EXISTS token_blob (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    blob BLOB NOT NULL
);
)SQL";

// v2: the tombstone flag is derived from state (tombstone = 1 exactly when state = 'gone_on_server').
// Databases written by v1 code could hold tombstone = 1 on a message that had been restored to another
// state (hidden from list_queue and deleted by purge_tombstones). Repair them. Idempotent.
//
// Ordering note (no schema change needed): command, session_event and session_item are ordinary rowid
// tables whose rowid is assigned in insertion order and never reused (rows are only deleted by
// secure_wipe, which empties the file). Listings use it as the tie-breaker after created_at / timestamp.
// Writers must therefore update with INSERT ... ON CONFLICT DO UPDATE, never INSERT OR REPLACE.
const char* const kV2 = R"SQL(
UPDATE message_cache SET tombstone = CASE WHEN state = 'gone_on_server' THEN 1 ELSE 0 END
    WHERE tombstone != CASE WHEN state = 'gone_on_server' THEN 1 ELSE 0 END;
)SQL";

} // namespace

const std::vector<Migration>& migrations() {
    static const std::vector<Migration> kMigrations = {
        {1, kV1},
        {2, kV2},
    };
    return kMigrations;
}

int latest_schema_version() { return migrations().back().version; }

} // namespace slowmail::storage
