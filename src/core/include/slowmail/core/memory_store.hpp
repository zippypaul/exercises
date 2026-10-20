#pragma once
#include <map>
#include <memory>
#include <mutex>

#include "slowmail/core/store.hpp"

namespace slowmail::core {

// In-memory Store for unit tests and the offline simulator. Not durable.
class MemoryStore final : public Store {
public:
    Status open() override;
    Status secure_wipe() override;
    Status begin() override;
    Status commit() override;
    Status rollback() override;

    std::optional<std::string> get_setting(const std::string& key) override;
    Status set_setting(const std::string& key, const std::string& value) override;

    std::optional<AccountInfo> get_account() override;
    Status put_account(const AccountInfo& a) override;

    InboxSyncState get_sync_state() override;
    Status put_sync_state(const InboxSyncState& s) override;

    Status upsert_message(const MessageSummary& m) override;
    Status set_message_state(const std::string& id, MessageState state, const std::string& reason) override;
    Status mark_gone(const std::string& id) override;
    std::optional<MessageSummary> get_message(const std::string& id) override;
    std::optional<MessageState> get_message_state(const std::string& id) override;
    std::vector<MessageSummary> list_messages_in_state(MessageState state) override;
    std::vector<MessageSummary> list_queue() override;
    Status put_body(const MessageBody& b) override;
    std::optional<MessageBody> get_body(const std::string& id) override;
    Status purge_tombstones() override;
    std::size_t count_messages() override;

    Status put_session(const SessionRecord& s) override;
    std::optional<SessionRecord> get_active_session() override;
    std::optional<SessionRecord> get_session(const std::string& id) override;
    Status put_session_item(const SessionItem& item) override;
    std::vector<SessionItem> list_session_items(const std::string& session_id) override;

    Status put_command(const Command& c) override;
    std::optional<Command> get_command(const std::string& id) override;
    std::vector<Command> list_commands(CommandState state) override;
    std::vector<Command> list_commands_for_message(const std::string& message_id) override;
    std::optional<Command> find_command_by_idempotency_key(const std::string& key) override;

    Status put_event(const SessionEvent& e) override;
    std::vector<SessionEvent> list_events(const std::string& session_id) override;

    std::optional<std::string> get_token_blob() override;
    Status put_token_blob(const std::string& blob) override;
    Status clear_token_blob() override;

    // Test hooks
    void fail_next_write(ErrorKind kind = ErrorKind::Storage) { fail_next_ = kind; }

private:
    struct CachedMessage { MessageSummary summary; MessageState state = MessageState::InQueue; std::string reason; };
    std::map<std::string, std::string> settings_;
    std::optional<AccountInfo> account_;
    InboxSyncState sync_;
    std::map<std::string, CachedMessage> messages_;
    std::map<std::string, MessageBody> bodies_;
    std::map<std::string, SessionRecord> sessions_;
    std::vector<SessionItem> items_;
    std::map<std::string, Command> commands_;
    std::vector<std::string> command_order_;
    std::vector<SessionEvent> events_;
    std::optional<std::string> token_blob_;
    std::optional<ErrorKind> fail_next_;
    struct Snapshot;                       // state captured by begin(), restored by rollback()
    std::shared_ptr<Snapshot> snapshot_;
    std::recursive_mutex mu_;
    Status maybe_fail();
};

} // namespace slowmail::core
