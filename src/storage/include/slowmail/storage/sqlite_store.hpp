#pragma once
#include <string>

#include "slowmail/core/store.hpp"

struct sqlite3;

namespace slowmail::storage {

// SQLite implementation of core::Store.
// * schema lives in schema.cpp with a `schema_version` setting and forward-only migrations
// * WAL journal mode, synchronous=FULL (durability over speed; E Ink device, tiny write volume)
// * the database file is created with mode 0600; encryption at rest is NOT implemented (see docs/THREAT_MODEL.md)
// * secure_wipe(): deletes all rows, VACUUMs, closes, unlinks db + -wal + -shm files
class SqliteStore final : public core::Store {
public:
    explicit SqliteStore(std::string path);   // ":memory:" supported for tests
    ~SqliteStore() override;

    core::Status open() override;
    core::Status secure_wipe() override;
    core::Status begin() override;
    core::Status commit() override;
    core::Status rollback() override;

    std::optional<std::string> get_setting(const std::string& key) override;
    core::Status set_setting(const std::string& key, const std::string& value) override;

    std::optional<core::AccountInfo> get_account() override;
    core::Status put_account(const core::AccountInfo& a) override;

    core::InboxSyncState get_sync_state() override;
    core::Status put_sync_state(const core::InboxSyncState& s) override;

    core::Status upsert_message(const core::MessageSummary& m) override;
    core::Status set_message_state(const std::string& id, core::MessageState state, const std::string& reason) override;
    core::Status mark_gone(const std::string& id) override;
    std::optional<core::MessageSummary> get_message(const std::string& id) override;
    std::optional<core::MessageState> get_message_state(const std::string& id) override;
    std::vector<core::MessageSummary> list_messages_in_state(core::MessageState state) override;
    std::vector<core::MessageSummary> list_queue() override;
    core::Status put_body(const core::MessageBody& b) override;
    std::optional<core::MessageBody> get_body(const std::string& id) override;
    core::Status purge_tombstones() override;
    std::size_t count_messages() override;

    core::Status put_session(const core::SessionRecord& s) override;
    std::optional<core::SessionRecord> get_active_session() override;
    std::optional<core::SessionRecord> get_session(const std::string& id) override;
    core::Status put_session_item(const core::SessionItem& item) override;
    std::vector<core::SessionItem> list_session_items(const std::string& session_id) override;

    core::Status put_command(const core::Command& c) override;
    std::optional<core::Command> get_command(const std::string& id) override;
    std::vector<core::Command> list_commands(core::CommandState state) override;
    std::vector<core::Command> list_commands_for_message(const std::string& message_id) override;
    std::optional<core::Command> find_command_by_idempotency_key(const std::string& key) override;

    core::Status put_event(const core::SessionEvent& e) override;
    std::vector<core::SessionEvent> list_events(const std::string& session_id) override;

    std::optional<std::string> get_token_blob() override;
    core::Status put_token_blob(const std::string& blob) override;
    core::Status clear_token_blob() override;

    int schema_version();
    const std::string& path() const { return path_; }

private:
    std::string path_;
    sqlite3* db_ = nullptr;
    int tx_depth_ = 0;
    core::Status exec(const std::string& sql);
    core::Status migrate();
};

} // namespace slowmail::storage
