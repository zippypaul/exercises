#pragma once
#include <optional>
#include <string>
#include <vector>

#include "slowmail/core/types.hpp"

namespace slowmail::core {

// Durable local store. Implemented by slowmail::storage::SqliteStore; tests use MemoryStore.
// Semantics every implementation must honour:
//  * every method is atomic on its own
//  * `transaction()` groups several calls; nested transactions are not required
//  * message ids are opaque strings; the store never interprets them
//  * nothing is ever hard-deleted from message_cache; use tombstone (GoneOnServer) instead,
//    except via purge_tombstones() which the app calls at session end
//  * "tombstoned" is derived from state, never stored independently: a message is tombstoned exactly
//    while its state is GoneOnServer. set_message_state(id, <any other state>) therefore un-tombstones
//    it (list_queue lists it again and purge_tombstones keeps it); an implementation that keeps a
//    separate tombstone flag must update it on every state change. upsert_message never changes it.
//  * list_commands / list_commands_for_message return commands in insertion order (FIFO) even when
//    created_at is equal, and updating a command via put_command keeps its position;
//    list_events is newest first, later insertions first on equal timestamps
class Store {
public:
    virtual ~Store() = default;

    // --- lifecycle -----------------------------------------------------------
    virtual Status open() = 0;              // creates/migrates schema
    virtual Status secure_wipe() = 0;       // sign-out: delete all rows AND the file(s); implementation documents its guarantees
    virtual Status begin() = 0;
    virtual Status commit() = 0;
    virtual Status rollback() = 0;

    // --- settings ------------------------------------------------------------
    virtual std::optional<std::string> get_setting(const std::string& key) = 0;
    virtual Status set_setting(const std::string& key, const std::string& value) = 0;

    // --- account -------------------------------------------------------------
    virtual std::optional<AccountInfo> get_account() = 0;
    virtual Status put_account(const AccountInfo& a) = 0;

    // --- inbox sync state ----------------------------------------------------
    virtual InboxSyncState get_sync_state() = 0;
    virtual Status put_sync_state(const InboxSyncState& s) = 0;

    // --- message cache -------------------------------------------------------
    // upsert keeps local-only columns (state) untouched unless `state` is provided.
    virtual Status upsert_message(const MessageSummary& m) = 0;
    virtual Status set_message_state(const std::string& id, MessageState state, const std::string& reason) = 0;
    virtual Status mark_gone(const std::string& id) = 0;                 // tombstone; state -> GoneOnServer
    virtual std::optional<MessageSummary> get_message(const std::string& id) = 0;
    virtual std::optional<MessageState> get_message_state(const std::string& id) = 0;
    virtual std::vector<MessageSummary> list_messages_in_state(MessageState state) = 0; // newest first by received_at
    virtual std::vector<MessageSummary> list_queue() = 0;                // state == InQueue, newest first
    virtual Status put_body(const MessageBody& b) = 0;                   // cache body text + attachment metadata
    virtual std::optional<MessageBody> get_body(const std::string& id) = 0;
    virtual Status purge_tombstones() = 0;
    virtual std::size_t count_messages() = 0;

    // --- sessions ------------------------------------------------------------
    virtual Status put_session(const SessionRecord& s) = 0;
    virtual std::optional<SessionRecord> get_active_session() = 0;       // state not Ended; at most one
    virtual std::optional<SessionRecord> get_session(const std::string& id) = 0;
    virtual Status put_session_item(const SessionItem& item) = 0;
    virtual std::vector<SessionItem> list_session_items(const std::string& session_id) = 0;

    // --- command journal -----------------------------------------------------
    virtual Status put_command(const Command& c) = 0;                    // insert or replace by id
    virtual std::optional<Command> get_command(const std::string& id) = 0;
    virtual std::vector<Command> list_commands(CommandState state) = 0;  // oldest first (FIFO)
    virtual std::vector<Command> list_commands_for_message(const std::string& message_id) = 0;
    virtual std::optional<Command> find_command_by_idempotency_key(const std::string& key) = 0;

    // --- session events (activity view) --------------------------------------
    virtual Status put_event(const SessionEvent& e) = 0;
    virtual std::vector<SessionEvent> list_events(const std::string& session_id) = 0; // newest first

    // --- tokens (opaque blob; encryption strategy lives in platform layer) -----
    virtual std::optional<std::string> get_token_blob() = 0;
    virtual Status put_token_blob(const std::string& blob) = 0;
    virtual Status clear_token_blob() = 0;
};

// RAII helper: commits on success(), rolls back on destruction otherwise.
class Transaction {
public:
    explicit Transaction(Store& s);
    ~Transaction();
    Status commit();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
private:
    Store& store_;
    bool done_ = false;       // true once committed, or when begin() failed (nothing to roll back)
    Status begin_status_;     // result of begin(); returned by commit() if begin() failed
};

} // namespace slowmail::core
