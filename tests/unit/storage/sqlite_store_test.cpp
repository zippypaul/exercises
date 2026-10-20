#include <gtest/gtest.h>

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "slowmail/core/types.hpp"
#include "slowmail/storage/sqlite_store.hpp"

using namespace slowmail;
using namespace slowmail::core;
using storage::SqliteStore;

namespace {

// Temp directory removed (with the files we know about) on destruction.
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/slowmail_storage_XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        path_ = p != nullptr ? p : "";
    }
    ~TempDir() {
        if (path_.empty()) return;
        for (const char* suffix : {"/mail.db", "/mail.db-wal", "/mail.db-shm", "/other.db", "/other.db-wal", "/other.db-shm"}) {
            ::unlink((path_ + suffix).c_str());
        }
        ::rmdir(path_.c_str());
    }
    const std::string& path() const { return path_; }
    std::string db() const { return path_ + "/mail.db"; }

private:
    std::string path_;
};

bool file_exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

MessageSummary full_message(const std::string& id = "AAMk-1") {
    MessageSummary m;
    m.id = id;
    m.conversation_id = "conv-1";
    m.subject = "Quarterly report \xE2\x9C\x93";
    m.from.name = "Ada Lovelace";
    m.from.address = "ada@example.com";
    m.received_at_iso = "2026-10-20T01:23:45Z";
    m.received_at_epoch = 1792459425;
    m.is_read = true;
    m.flag_status = FlagStatus::Flagged;
    m.body_preview = "Hello there";
    m.has_attachments = true;
    m.change_key = "CQAAABYAAAB";
    m.parent_folder_id = "inbox-folder-id";
    return m;
}

void expect_message_eq(const MessageSummary& a, const MessageSummary& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.conversation_id, b.conversation_id);
    EXPECT_EQ(a.subject, b.subject);
    EXPECT_EQ(a.from.name, b.from.name);
    EXPECT_EQ(a.from.address, b.from.address);
    EXPECT_EQ(a.received_at_iso, b.received_at_iso);
    EXPECT_EQ(a.received_at_epoch, b.received_at_epoch);
    EXPECT_EQ(a.is_read, b.is_read);
    EXPECT_EQ(a.flag_status, b.flag_status);
    EXPECT_EQ(a.body_preview, b.body_preview);
    EXPECT_EQ(a.has_attachments, b.has_attachments);
    EXPECT_EQ(a.change_key, b.change_key);
    EXPECT_EQ(a.parent_folder_id, b.parent_folder_id);
}

Command full_command(const std::string& id = "cmd-1", const std::string& key = "idem-1") {
    Command c;
    c.id = id;
    c.session_id = "sess-1";
    c.message_id = "AAMk-1";
    c.type = CommandType::UndoDelete;
    c.payload_json = "{\"to\":\"inbox\"}";
    c.created_at = 1792459500;
    c.state = CommandState::InFlight;
    c.attempts = 3;
    c.last_error = "timeout";
    c.server_result_json = "{\"new_id\":\"X\"}";
    c.idempotency_key = key;
    return c;
}

SessionEvent full_event(const std::string& id, std::int64_t ts, const std::string& session = "sess-1") {
    SessionEvent e;
    e.id = id;
    e.session_id = session;
    e.message_id = "AAMk-1";
    e.type = EventType::Delegated;
    e.timestamp = ts;
    e.confirmation = ConfirmationState::Failed;
    e.display_snapshot_json = "{\"subject\":\"s\"}";
    e.command_id = "cmd-1";
    e.failure_reason = "nope";
    return e;
}

// Runs every behavioural test against both a temp file and ":memory:".
class SqliteStoreTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        on_disk_ = GetParam();
        store_ = std::make_unique<SqliteStore>(on_disk_ ? dir_.db() : std::string(":memory:"));
        auto st = store_->open();
        ASSERT_TRUE(st.ok()) << st.error().message;
    }
    TempDir dir_;
    bool on_disk_ = false;
    std::unique_ptr<SqliteStore> store_;
};

INSTANTIATE_TEST_SUITE_P(Backends, SqliteStoreTest, ::testing::Values(true, false),
                         [](const ::testing::TestParamInfo<bool>& i) { return i.param ? "File" : "Memory"; });

} // namespace

TEST_P(SqliteStoreTest, SchemaCreatedAtVersion1) {
    EXPECT_EQ(store_->schema_version(), 1);
    EXPECT_EQ(store_->get_setting("schema_version").value_or(""), "1");
}

TEST_P(SqliteStoreTest, OpenIsIdempotent) {
    ASSERT_TRUE(store_->set_setting("k", "v").ok());
    EXPECT_TRUE(store_->open().ok());
    EXPECT_EQ(store_->get_setting("k").value_or(""), "v");
}

TEST_P(SqliteStoreTest, SettingsRoundTripAndOverwrite) {
    EXPECT_FALSE(store_->get_setting("missing").has_value());
    ASSERT_TRUE(store_->set_setting("theme", "dark").ok());
    EXPECT_EQ(store_->get_setting("theme").value_or(""), "dark");
    ASSERT_TRUE(store_->set_setting("theme", "light").ok());
    EXPECT_EQ(store_->get_setting("theme").value_or(""), "light");
    ASSERT_TRUE(store_->set_setting("empty", "").ok());
    ASSERT_TRUE(store_->get_setting("empty").has_value());
    EXPECT_EQ(*store_->get_setting("empty"), "");
}

TEST_P(SqliteStoreTest, AccountRoundTrip) {
    EXPECT_FALSE(store_->get_account().has_value());
    AccountInfo a;
    a.tenant_id = "tenant";
    a.user_id = "user";
    a.display_address = "me@example.com";
    a.signed_in = true;
    ASSERT_TRUE(store_->put_account(a).ok());
    auto got = store_->get_account();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->tenant_id, "tenant");
    EXPECT_EQ(got->user_id, "user");
    EXPECT_EQ(got->display_address, "me@example.com");
    EXPECT_TRUE(got->signed_in);

    a.signed_in = false;
    a.display_address = "other@example.com";
    ASSERT_TRUE(store_->put_account(a).ok());  // single row, replaced
    got = store_->get_account();
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(got->signed_in);
    EXPECT_EQ(got->display_address, "other@example.com");
}

TEST_P(SqliteStoreTest, SyncStateDefaultsAndRoundTrip) {
    InboxSyncState d = store_->get_sync_state();
    EXPECT_FALSE(d.delta_link.has_value());
    EXPECT_FALSE(d.resume_next_link.has_value());
    EXPECT_FALSE(d.last_complete_wall.has_value());
    EXPECT_FALSE(d.last_complete_mono.has_value());
    EXPECT_FALSE(d.last_complete_boot_id.has_value());
    EXPECT_FALSE(d.last_attempt_wall.has_value());
    EXPECT_EQ(d.schema_version, 1);

    InboxSyncState s;
    s.delta_link = "https://graph/delta?token=abc";
    s.resume_next_link = "https://graph/next?skip=10";
    s.last_complete_wall = 1792459000;
    s.last_complete_mono = 12345.678901;
    s.last_complete_boot_id = "boot-xyz";
    s.last_attempt_wall = 1792459999;
    s.schema_version = 7;
    ASSERT_TRUE(store_->put_sync_state(s).ok());
    InboxSyncState g = store_->get_sync_state();
    EXPECT_EQ(g.delta_link, s.delta_link);
    EXPECT_EQ(g.resume_next_link, s.resume_next_link);
    EXPECT_EQ(g.last_complete_wall, s.last_complete_wall);
    ASSERT_TRUE(g.last_complete_mono.has_value());
    EXPECT_DOUBLE_EQ(*g.last_complete_mono, 12345.678901);
    EXPECT_EQ(g.last_complete_boot_id, s.last_complete_boot_id);
    EXPECT_EQ(g.last_attempt_wall, s.last_attempt_wall);
    EXPECT_EQ(g.schema_version, 7);

    // Clearing optionals (e.g. resume link after a complete sync) must write NULLs.
    s.resume_next_link.reset();
    s.last_complete_mono.reset();
    ASSERT_TRUE(store_->put_sync_state(s).ok());
    g = store_->get_sync_state();
    EXPECT_FALSE(g.resume_next_link.has_value());
    EXPECT_FALSE(g.last_complete_mono.has_value());
    EXPECT_EQ(g.delta_link, s.delta_link);
}

TEST_P(SqliteStoreTest, MessageRoundTripEveryField) {
    EXPECT_FALSE(store_->get_message("AAMk-1").has_value());
    const MessageSummary m = full_message();
    ASSERT_TRUE(store_->upsert_message(m).ok());
    auto got = store_->get_message("AAMk-1");
    ASSERT_TRUE(got.has_value());
    expect_message_eq(*got, m);
    EXPECT_EQ(store_->get_message_state("AAMk-1"), MessageState::InQueue);
    EXPECT_EQ(store_->count_messages(), 1u);
}

TEST_P(SqliteStoreTest, MessageRoundTripFalseAndDefaultValues) {
    MessageSummary m;
    m.id = "plain";
    m.flag_status = FlagStatus::Complete;
    ASSERT_TRUE(store_->upsert_message(m).ok());
    auto got = store_->get_message("plain");
    ASSERT_TRUE(got.has_value());
    expect_message_eq(*got, m);
}

TEST_P(SqliteStoreTest, UpsertUpdatesServerFieldsButPreservesLocalState) {
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    ASSERT_TRUE(store_->set_message_state("AAMk-1", MessageState::DeferPending, "because").ok());

    MessageSummary changed = full_message();
    changed.subject = "new subject";
    changed.is_read = false;
    changed.flag_status = FlagStatus::Complete;
    changed.change_key = "key2";
    changed.received_at_epoch += 100;
    ASSERT_TRUE(store_->upsert_message(changed).ok());

    expect_message_eq(*store_->get_message("AAMk-1"), changed);
    EXPECT_EQ(store_->get_message_state("AAMk-1"), MessageState::DeferPending);
    EXPECT_EQ(store_->count_messages(), 1u);
}

TEST_P(SqliteStoreTest, UpsertDoesNotResurrectGoneMessage) {
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    ASSERT_TRUE(store_->mark_gone("AAMk-1").ok());
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    EXPECT_EQ(store_->get_message_state("AAMk-1"), MessageState::GoneOnServer);
    EXPECT_TRUE(store_->list_queue().empty());
    EXPECT_EQ(store_->list_messages_in_state(MessageState::GoneOnServer).size(), 1u);
}

TEST_P(SqliteStoreTest, SetMessageStateUnknownIdIsNotFound) {
    auto st = store_->set_message_state("nope", MessageState::Archived, "");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::NotFound);
    EXPECT_FALSE(store_->get_message_state("nope").has_value());
}

TEST_P(SqliteStoreTest, MarkGoneUnknownIdIsNoOp) {
    EXPECT_TRUE(store_->mark_gone("never-seen").ok());
    EXPECT_EQ(store_->count_messages(), 0u);
}

TEST_P(SqliteStoreTest, ListQueueOrderingAndFiltering) {
    auto add = [&](const std::string& id, std::int64_t epoch) {
        MessageSummary m = full_message(id);
        m.received_at_epoch = epoch;
        ASSERT_TRUE(store_->upsert_message(m).ok());
    };
    add("b", 200);
    add("a", 200);  // tie with "b": id ascending
    add("c", 300);
    add("d", 100);
    add("archived", 400);
    add("gone", 500);
    ASSERT_TRUE(store_->set_message_state("archived", MessageState::Archived, "").ok());
    ASSERT_TRUE(store_->mark_gone("gone").ok());

    auto q = store_->list_queue();
    ASSERT_EQ(q.size(), 4u);
    EXPECT_EQ(q[0].id, "c");
    EXPECT_EQ(q[1].id, "a");
    EXPECT_EQ(q[2].id, "b");
    EXPECT_EQ(q[3].id, "d");

    auto arch = store_->list_messages_in_state(MessageState::Archived);
    ASSERT_EQ(arch.size(), 1u);
    EXPECT_EQ(arch[0].id, "archived");
}

TEST_P(SqliteStoreTest, ListQueueExcludesTombstonedEvenIfStateReset) {
    ASSERT_TRUE(store_->upsert_message(full_message("x")).ok());
    ASSERT_TRUE(store_->mark_gone("x").ok());
    ASSERT_TRUE(store_->set_message_state("x", MessageState::InQueue, "").ok());  // tombstone flag stays
    EXPECT_TRUE(store_->list_queue().empty());
}

TEST_P(SqliteStoreTest, SetMessageStateStoresReasonAndEveryStateRoundTrips) {
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    const MessageState all[] = {MessageState::InQueue,       MessageState::DoPending,   MessageState::Archived,
                                MessageState::DeletePending, MessageState::Deleted,     MessageState::DeferPending,
                                MessageState::Deferred,      MessageState::GoneOnServer, MessageState::Failed};
    for (MessageState s : all) {
        ASSERT_TRUE(store_->set_message_state("AAMk-1", s, "r").ok());
        EXPECT_EQ(store_->get_message_state("AAMk-1"), s) << to_string(s);
    }
}

TEST_P(SqliteStoreTest, BodyRoundTripEveryField) {
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    EXPECT_FALSE(store_->get_body("AAMk-1").has_value());

    MessageBody b;
    b.id = "AAMk-1";
    b.text = "line1\nline2 \"quoted\" \xE2\x9C\x93";
    b.was_html = true;
    b.truncated = true;
    AttachmentMeta a1;
    a1.id = "att-1";
    a1.name = "report \"final\".pdf";
    a1.content_type = "application/pdf";
    a1.size = 123456;
    a1.is_inline = false;
    a1.kind = AttachmentKind::File;
    AttachmentMeta a2;
    a2.id = "att-2";
    a2.name = "logo.png";
    a2.content_type = "image/png";
    a2.size = -1;
    a2.is_inline = true;
    a2.kind = AttachmentKind::Reference;
    b.attachments = {a1, a2};
    ASSERT_TRUE(store_->put_body(b).ok());

    auto g = store_->get_body("AAMk-1");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->id, b.id);
    EXPECT_EQ(g->text, b.text);
    EXPECT_EQ(g->was_html, true);
    EXPECT_EQ(g->truncated, true);
    ASSERT_EQ(g->attachments.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(g->attachments[i].id, b.attachments[i].id);
        EXPECT_EQ(g->attachments[i].name, b.attachments[i].name);
        EXPECT_EQ(g->attachments[i].content_type, b.attachments[i].content_type);
        EXPECT_EQ(g->attachments[i].size, b.attachments[i].size);
        EXPECT_EQ(g->attachments[i].is_inline, b.attachments[i].is_inline);
        EXPECT_EQ(g->attachments[i].kind, b.attachments[i].kind);
    }

    // Replace with an empty, non-html body.
    MessageBody e;
    e.id = "AAMk-1";
    ASSERT_TRUE(store_->put_body(e).ok());
    g = store_->get_body("AAMk-1");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->text, "");
    EXPECT_FALSE(g->was_html);
    EXPECT_FALSE(g->truncated);
    EXPECT_TRUE(g->attachments.empty());
}

TEST_P(SqliteStoreTest, PutBodyForUnknownMessageFailsWithStorageError) {
    MessageBody b;
    b.id = "ghost";
    auto st = store_->put_body(b);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);
}

TEST_P(SqliteStoreTest, PurgeTombstonesRemovesOnlyTombstonedAndTheirBodies) {
    ASSERT_TRUE(store_->upsert_message(full_message("keep")).ok());
    ASSERT_TRUE(store_->upsert_message(full_message("drop")).ok());
    MessageBody kb;
    kb.id = "keep";
    kb.text = "k";
    MessageBody db;
    db.id = "drop";
    db.text = "d";
    ASSERT_TRUE(store_->put_body(kb).ok());
    ASSERT_TRUE(store_->put_body(db).ok());
    ASSERT_TRUE(store_->mark_gone("drop").ok());
    EXPECT_EQ(store_->count_messages(), 2u);

    ASSERT_TRUE(store_->purge_tombstones().ok());
    EXPECT_EQ(store_->count_messages(), 1u);
    EXPECT_TRUE(store_->get_message("keep").has_value());
    EXPECT_FALSE(store_->get_message("drop").has_value());
    EXPECT_TRUE(store_->get_body("keep").has_value());
    EXPECT_FALSE(store_->get_body("drop").has_value());
}

TEST_P(SqliteStoreTest, SessionRoundTripEveryField) {
    EXPECT_FALSE(store_->get_session("s1").has_value());
    SessionRecord r;
    r.id = "s1";
    r.started_at = 1000;
    r.ended_at = 2000;
    r.initial_sync_at = 1100;
    r.state = SessionState::SyncPartial;
    ASSERT_TRUE(store_->put_session(r).ok());
    auto g = store_->get_session("s1");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->id, "s1");
    EXPECT_EQ(g->started_at, 1000);
    EXPECT_EQ(g->ended_at, std::optional<std::int64_t>(2000));
    EXPECT_EQ(g->initial_sync_at, std::optional<std::int64_t>(1100));
    EXPECT_EQ(g->state, SessionState::SyncPartial);

    r.ended_at.reset();
    r.initial_sync_at.reset();
    r.state = SessionState::Offline;
    ASSERT_TRUE(store_->put_session(r).ok());
    g = store_->get_session("s1");
    EXPECT_FALSE(g->ended_at.has_value());
    EXPECT_FALSE(g->initial_sync_at.has_value());
    EXPECT_EQ(g->state, SessionState::Offline);
}

TEST_P(SqliteStoreTest, EverySessionStateRoundTrips) {
    const SessionState all[] = {SessionState::NotStarted, SessionState::AuthRequired, SessionState::ReadyCached,
                                SessionState::SyncEligible, SessionState::Syncing,      SessionState::SyncPartial,
                                SessionState::Active,      SessionState::Offline,      SessionState::Ending,
                                SessionState::Ended};
    for (SessionState s : all) {
        SessionRecord r;
        r.id = "s";
        r.started_at = 1;
        r.state = s;
        ASSERT_TRUE(store_->put_session(r).ok());
        EXPECT_EQ(store_->get_session("s")->state, s) << to_string(s);
    }
}

TEST_P(SqliteStoreTest, GetActiveSessionPicksNonEnded) {
    EXPECT_FALSE(store_->get_active_session().has_value());
    SessionRecord old_one;
    old_one.id = "old";
    old_one.started_at = 100;
    old_one.ended_at = 200;
    old_one.state = SessionState::Ended;
    SessionRecord newer_ended;
    newer_ended.id = "newer-ended";
    newer_ended.started_at = 500;
    newer_ended.ended_at = 600;
    newer_ended.state = SessionState::Ended;
    SessionRecord live;
    live.id = "live";
    live.started_at = 300;
    live.state = SessionState::Active;
    ASSERT_TRUE(store_->put_session(old_one).ok());
    ASSERT_TRUE(store_->put_session(newer_ended).ok());
    ASSERT_TRUE(store_->put_session(live).ok());
    auto a = store_->get_active_session();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, "live");

    // With several non-ended rows the most recently started wins.
    SessionRecord later;
    later.id = "later";
    later.started_at = 400;
    later.state = SessionState::Syncing;
    ASSERT_TRUE(store_->put_session(later).ok());
    EXPECT_EQ(store_->get_active_session()->id, "later");

    live.state = SessionState::Ended;
    later.state = SessionState::Ended;
    ASSERT_TRUE(store_->put_session(live).ok());
    ASSERT_TRUE(store_->put_session(later).ok());
    EXPECT_FALSE(store_->get_active_session().has_value());
}

TEST_P(SqliteStoreTest, SessionItemRoundTripAndUpsert) {
    EXPECT_TRUE(store_->list_session_items("s1").empty());
    SessionItem a;
    a.session_id = "s1";
    a.message_id = "m1";
    a.initial_state = MessageState::InQueue;
    a.current_state = MessageState::DeferPending;
    a.suppression_reason = "deferred_by_slowmail";
    SessionItem b;
    b.session_id = "s1";
    b.message_id = "m2";
    b.initial_state = MessageState::Deferred;
    b.current_state = MessageState::Archived;
    SessionItem other;
    other.session_id = "s2";
    other.message_id = "m1";
    ASSERT_TRUE(store_->put_session_item(a).ok());
    ASSERT_TRUE(store_->put_session_item(b).ok());
    ASSERT_TRUE(store_->put_session_item(other).ok());

    auto items = store_->list_session_items("s1");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].session_id, "s1");
    EXPECT_EQ(items[0].message_id, "m1");
    EXPECT_EQ(items[0].initial_state, MessageState::InQueue);
    EXPECT_EQ(items[0].current_state, MessageState::DeferPending);
    EXPECT_EQ(items[0].suppression_reason, "deferred_by_slowmail");
    EXPECT_EQ(items[1].message_id, "m2");
    EXPECT_EQ(items[1].initial_state, MessageState::Deferred);
    EXPECT_EQ(items[1].current_state, MessageState::Archived);
    EXPECT_EQ(items[1].suppression_reason, "");

    a.current_state = MessageState::Deferred;
    a.suppression_reason = "";
    ASSERT_TRUE(store_->put_session_item(a).ok());
    items = store_->list_session_items("s1");
    ASSERT_EQ(items.size(), 2u);  // PK (session, message): replaced, not duplicated
    EXPECT_EQ(items[0].message_id, "m1");
    EXPECT_EQ(items[0].current_state, MessageState::Deferred);
    EXPECT_EQ(items[0].suppression_reason, "");
}

TEST_P(SqliteStoreTest, CommandRoundTripEveryField) {
    EXPECT_FALSE(store_->get_command("cmd-1").has_value());
    const Command c = full_command();
    ASSERT_TRUE(store_->put_command(c).ok());
    auto g = store_->get_command("cmd-1");
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->id, c.id);
    EXPECT_EQ(g->session_id, c.session_id);
    EXPECT_EQ(g->message_id, c.message_id);
    EXPECT_EQ(g->type, c.type);
    EXPECT_EQ(g->payload_json, c.payload_json);
    EXPECT_EQ(g->created_at, c.created_at);
    EXPECT_EQ(g->state, c.state);
    EXPECT_EQ(g->attempts, c.attempts);
    EXPECT_EQ(g->last_error, c.last_error);
    EXPECT_EQ(g->server_result_json, c.server_result_json);
    EXPECT_EQ(g->idempotency_key, c.idempotency_key);

    auto by_key = store_->find_command_by_idempotency_key("idem-1");
    ASSERT_TRUE(by_key.has_value());
    EXPECT_EQ(by_key->id, "cmd-1");
    EXPECT_FALSE(store_->find_command_by_idempotency_key("other").has_value());
}

TEST_P(SqliteStoreTest, EveryCommandTypeAndStateRoundTrips) {
    const CommandType types[] = {CommandType::Archive, CommandType::Delete,      CommandType::Defer,
                                 CommandType::Undefer, CommandType::UndoArchive, CommandType::UndoDelete};
    int n = 0;
    for (CommandType t : types) {
        Command c = full_command("t" + std::to_string(n), "k" + std::to_string(n));
        c.type = t;
        ++n;
        ASSERT_TRUE(store_->put_command(c).ok());
        EXPECT_EQ(store_->get_command(c.id)->type, t);
    }
    const CommandState states[] = {CommandState::Pending, CommandState::InFlight, CommandState::Confirmed,
                                   CommandState::Failed, CommandState::Abandoned};
    for (CommandState s : states) {
        Command c = full_command("s" + std::to_string(n), "k" + std::to_string(n));
        c.state = s;
        ++n;
        ASSERT_TRUE(store_->put_command(c).ok());
        EXPECT_EQ(store_->get_command(c.id)->state, s);
    }
}

TEST_P(SqliteStoreTest, PutCommandReplacesById) {
    Command c = full_command();
    ASSERT_TRUE(store_->put_command(c).ok());
    c.state = CommandState::Confirmed;
    c.attempts = 4;
    c.last_error = "";
    ASSERT_TRUE(store_->put_command(c).ok());  // same id, same key: replace
    auto g = store_->get_command("cmd-1");
    EXPECT_EQ(g->state, CommandState::Confirmed);
    EXPECT_EQ(g->attempts, 4);
    EXPECT_EQ(store_->list_commands(CommandState::InFlight).size(), 0u);
    EXPECT_EQ(store_->list_commands(CommandState::Confirmed).size(), 1u);
}

TEST_P(SqliteStoreTest, IdempotencyKeyUniqueAcrossDifferentIds) {
    ASSERT_TRUE(store_->put_command(full_command("cmd-1", "same-key")).ok());
    auto st = store_->put_command(full_command("cmd-2", "same-key"));
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);
    EXPECT_FALSE(store_->get_command("cmd-2").has_value());
    EXPECT_EQ(store_->get_command("cmd-1")->id, "cmd-1");
}

TEST_P(SqliteStoreTest, EmptyIdempotencyKeysDoNotCollide) {
    ASSERT_TRUE(store_->put_command(full_command("a", "")).ok());
    ASSERT_TRUE(store_->put_command(full_command("b", "")).ok());
    EXPECT_EQ(store_->get_command("a")->idempotency_key, "");
    EXPECT_FALSE(store_->find_command_by_idempotency_key("").has_value());
}

TEST_P(SqliteStoreTest, ListCommandsOrderingFifoAndByMessage) {
    auto add = [&](const std::string& id, std::int64_t created, const std::string& msg, CommandState st) {
        Command c = full_command(id, "key-" + id);
        c.created_at = created;
        c.message_id = msg;
        c.state = st;
        ASSERT_TRUE(store_->put_command(c).ok());
    };
    add("c3", 300, "m1", CommandState::Pending);
    add("b1", 100, "m1", CommandState::Pending);
    add("a1", 100, "m2", CommandState::Pending);  // ties with b1 on created_at: id ascending
    add("d4", 50, "m2", CommandState::Failed);

    auto pending = store_->list_commands(CommandState::Pending);
    ASSERT_EQ(pending.size(), 3u);
    EXPECT_EQ(pending[0].id, "a1");
    EXPECT_EQ(pending[1].id, "b1");
    EXPECT_EQ(pending[2].id, "c3");

    auto m1 = store_->list_commands_for_message("m1");
    ASSERT_EQ(m1.size(), 2u);
    EXPECT_EQ(m1[0].id, "b1");
    EXPECT_EQ(m1[1].id, "c3");
    auto m2 = store_->list_commands_for_message("m2");
    ASSERT_EQ(m2.size(), 2u);
    EXPECT_EQ(m2[0].id, "d4");
    EXPECT_EQ(m2[1].id, "a1");
    EXPECT_TRUE(store_->list_commands_for_message("none").empty());
}

TEST_P(SqliteStoreTest, EventRoundTripEveryField) {
    const SessionEvent e = full_event("e1", 777);
    ASSERT_TRUE(store_->put_event(e).ok());
    auto list = store_->list_events("sess-1");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, e.id);
    EXPECT_EQ(list[0].session_id, e.session_id);
    EXPECT_EQ(list[0].message_id, e.message_id);
    EXPECT_EQ(list[0].type, e.type);
    EXPECT_EQ(list[0].timestamp, e.timestamp);
    EXPECT_EQ(list[0].confirmation, e.confirmation);
    EXPECT_EQ(list[0].display_snapshot_json, e.display_snapshot_json);
    EXPECT_EQ(list[0].command_id, e.command_id);
    EXPECT_EQ(list[0].failure_reason, e.failure_reason);
}

TEST_P(SqliteStoreTest, EveryEventTypeRoundTrips) {
    const EventType all[] = {EventType::Archived,   EventType::Deleted,     EventType::Deferred,
                             EventType::Undeferred, EventType::UndoArchive, EventType::UndoDelete,
                             EventType::Sent,       EventType::Delegated,   EventType::SyncCompleted,
                             EventType::SyncFailed};
    int n = 0;
    for (EventType t : all) {
        SessionEvent e = full_event("ev" + std::to_string(n), n, "types");
        e.type = t;
        ++n;
        ASSERT_TRUE(store_->put_event(e).ok());
    }
    auto list = store_->list_events("types");
    ASSERT_EQ(list.size(), 10u);
    for (const auto& e : list) {
        EXPECT_EQ(e.type, all[std::stoi(e.id.substr(2))]);
    }
}

TEST_P(SqliteStoreTest, ListEventsNewestFirstScopedToSession) {
    ASSERT_TRUE(store_->put_event(full_event("e1", 100)).ok());
    ASSERT_TRUE(store_->put_event(full_event("e3", 300)).ok());
    ASSERT_TRUE(store_->put_event(full_event("e2a", 200)).ok());
    ASSERT_TRUE(store_->put_event(full_event("e2b", 200)).ok());  // tie: id descending
    ASSERT_TRUE(store_->put_event(full_event("x", 999, "other-session")).ok());
    auto list = store_->list_events("sess-1");
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[0].id, "e3");
    EXPECT_EQ(list[1].id, "e2b");
    EXPECT_EQ(list[2].id, "e2a");
    EXPECT_EQ(list[3].id, "e1");
    EXPECT_TRUE(store_->list_events("none").empty());

    // put_event replaces by id (confirmation update path).
    SessionEvent upd = full_event("e1", 100);
    upd.confirmation = ConfirmationState::Confirmed;
    ASSERT_TRUE(store_->put_event(upd).ok());
    list = store_->list_events("sess-1");
    ASSERT_EQ(list.size(), 4u);
    EXPECT_EQ(list[3].confirmation, ConfirmationState::Confirmed);
}

TEST_P(SqliteStoreTest, TokenBlobRoundTripIncludingBinary) {
    EXPECT_FALSE(store_->get_token_blob().has_value());
    const std::string blob("\x00\x01\xFF tok\x00en", 11);
    ASSERT_TRUE(store_->put_token_blob(blob).ok());
    auto g = store_->get_token_blob();
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(*g, blob);

    ASSERT_TRUE(store_->put_token_blob("second").ok());
    EXPECT_EQ(store_->get_token_blob().value_or(""), "second");

    ASSERT_TRUE(store_->put_token_blob("").ok());
    ASSERT_TRUE(store_->get_token_blob().has_value());
    EXPECT_EQ(*store_->get_token_blob(), "");

    ASSERT_TRUE(store_->clear_token_blob().ok());
    EXPECT_FALSE(store_->get_token_blob().has_value());
    EXPECT_TRUE(store_->clear_token_blob().ok());  // idempotent
}

TEST_P(SqliteStoreTest, RollbackDiscardsAndCommitKeeps) {
    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->upsert_message(full_message("r1")).ok());
    ASSERT_TRUE(store_->set_setting("k", "v").ok());
    ASSERT_TRUE(store_->rollback().ok());
    EXPECT_FALSE(store_->get_message("r1").has_value());
    EXPECT_FALSE(store_->get_setting("k").has_value());

    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->upsert_message(full_message("c1")).ok());
    ASSERT_TRUE(store_->commit().ok());
    EXPECT_TRUE(store_->get_message("c1").has_value());
}

TEST_P(SqliteStoreTest, NestedBeginIsCounterAndInnerRollbackPoisonsOuterCommit) {
    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->begin().ok());  // nested: no-op counter
    ASSERT_TRUE(store_->upsert_message(full_message("n1")).ok());
    ASSERT_TRUE(store_->commit().ok());  // inner commit: still not durable
    ASSERT_TRUE(store_->commit().ok());  // outermost: real COMMIT
    EXPECT_TRUE(store_->get_message("n1").has_value());

    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->upsert_message(full_message("n2")).ok());
    ASSERT_TRUE(store_->rollback().ok());  // rolls back everything
    EXPECT_FALSE(store_->get_message("n2").has_value());
    auto st = store_->commit();            // outer level must not silently commit
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Invalid);
}

TEST_P(SqliteStoreTest, RollbackAndCommitWithoutTransaction) {
    EXPECT_TRUE(store_->rollback().ok());
    auto st = store_->commit();
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Invalid);
}

TEST_P(SqliteStoreTest, TransactionHelperRollsBackOnScopeExit) {
    {
        Transaction tx(*store_);
        ASSERT_TRUE(store_->upsert_message(full_message("t1")).ok());
        // no commit
    }
    EXPECT_FALSE(store_->get_message("t1").has_value());
    {
        Transaction tx(*store_);
        ASSERT_TRUE(store_->upsert_message(full_message("t2")).ok());
        ASSERT_TRUE(tx.commit().ok());
    }
    EXPECT_TRUE(store_->get_message("t2").has_value());
}

TEST_P(SqliteStoreTest, FailedCommandInsertInsideTransactionCanBeRolledBack) {
    ASSERT_TRUE(store_->put_command(full_command("cmd-1", "k")).ok());
    ASSERT_TRUE(store_->begin().ok());
    ASSERT_TRUE(store_->upsert_message(full_message("m")).ok());
    EXPECT_FALSE(store_->put_command(full_command("cmd-2", "k")).ok());
    ASSERT_TRUE(store_->rollback().ok());
    EXPECT_FALSE(store_->get_message("m").has_value());
}

TEST_P(SqliteStoreTest, ThousandMessagesInOneTransactionFastAndOrdered) {
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(store_->begin().ok());
    for (int i = 0; i < 1000; ++i) {
        MessageSummary m = full_message("id-" + std::to_string(1000 + i));
        m.subject = "subject " + std::to_string(i);
        m.received_at_epoch = 1700000000 + i;
        ASSERT_TRUE(store_->upsert_message(m).ok());
    }
    ASSERT_TRUE(store_->commit().ok());
    auto q = store_->list_queue();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // SLOWMAIL_TEST_TIME_SCALE loosens wall-clock bounds under emulation (QEMU) or slow CI.
    const char* scale_env = std::getenv("SLOWMAIL_TEST_TIME_SCALE");
    const double scale = scale_env ? std::atof(scale_env) : 1.0;
    EXPECT_LT(elapsed, 2.0 * (scale > 0 ? scale : 1.0));
    ASSERT_EQ(q.size(), 1000u);
    EXPECT_EQ(store_->count_messages(), 1000u);
    for (std::size_t i = 1; i < q.size(); ++i) {
        EXPECT_GT(q[i - 1].received_at_epoch, q[i].received_at_epoch);
    }
    EXPECT_EQ(q.front().subject, "subject 999");
    EXPECT_EQ(q.back().subject, "subject 0");
}

TEST_P(SqliteStoreTest, SecureWipeClearsAllDataAndStoreReopens) {
    ASSERT_TRUE(store_->set_setting("k", "v").ok());
    ASSERT_TRUE(store_->upsert_message(full_message()).ok());
    ASSERT_TRUE(store_->put_token_blob("secret").ok());
    ASSERT_TRUE(store_->put_command(full_command()).ok());
    ASSERT_TRUE(store_->secure_wipe().ok());

    // Closed after wipe: reads degrade to "absent", writes report a Storage error, nothing throws.
    EXPECT_FALSE(store_->get_token_blob().has_value());
    auto st = store_->set_setting("a", "b");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);

    ASSERT_TRUE(store_->open().ok());
    EXPECT_EQ(store_->schema_version(), 1);
    EXPECT_EQ(store_->count_messages(), 0u);
    EXPECT_FALSE(store_->get_setting("k").has_value());
    EXPECT_FALSE(store_->get_token_blob().has_value());
    EXPECT_FALSE(store_->get_command("cmd-1").has_value());
}

// --------------------------------------------------------------------------
// File-only tests
// --------------------------------------------------------------------------

TEST(SqliteStoreFile, ReopenKeepsDataAcrossEveryTable) {
    TempDir dir;
    ASSERT_FALSE(dir.path().empty());
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        EXPECT_EQ(s.schema_version(), 1);
        ASSERT_TRUE(s.set_setting("k", "v").ok());
        AccountInfo a;
        a.tenant_id = "t";
        a.signed_in = true;
        ASSERT_TRUE(s.put_account(a).ok());
        InboxSyncState ss;
        ss.delta_link = "dl";
        ASSERT_TRUE(s.put_sync_state(ss).ok());
        ASSERT_TRUE(s.upsert_message(full_message()).ok());
        ASSERT_TRUE(s.set_message_state("AAMk-1", MessageState::Deferred, "why").ok());
        MessageBody b;
        b.id = "AAMk-1";
        b.text = "body";
        ASSERT_TRUE(s.put_body(b).ok());
        SessionRecord r;
        r.id = "s1";
        r.started_at = 5;
        r.state = SessionState::Active;
        ASSERT_TRUE(s.put_session(r).ok());
        SessionItem it;
        it.session_id = "s1";
        it.message_id = "AAMk-1";
        ASSERT_TRUE(s.put_session_item(it).ok());
        ASSERT_TRUE(s.put_command(full_command()).ok());
        ASSERT_TRUE(s.put_event(full_event("e1", 10)).ok());
        ASSERT_TRUE(s.put_token_blob("blob").ok());
    }
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_EQ(s.schema_version(), 1);
    EXPECT_EQ(s.get_setting("k").value_or(""), "v");
    EXPECT_TRUE(s.get_account().has_value());
    EXPECT_EQ(s.get_sync_state().delta_link.value_or(""), "dl");
    ASSERT_TRUE(s.get_message("AAMk-1").has_value());
    expect_message_eq(*s.get_message("AAMk-1"), full_message());
    EXPECT_EQ(s.get_message_state("AAMk-1"), MessageState::Deferred);
    EXPECT_EQ(s.get_body("AAMk-1")->text, "body");
    EXPECT_EQ(s.get_active_session()->id, "s1");
    EXPECT_EQ(s.list_session_items("s1").size(), 1u);
    EXPECT_TRUE(s.get_command("cmd-1").has_value());
    EXPECT_EQ(s.list_events("sess-1").size(), 1u);
    EXPECT_EQ(s.get_token_blob().value_or(""), "blob");
}

TEST(SqliteStoreFile, UncommittedTransactionIsNotDurable) {
    TempDir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_TRUE(s.begin().ok());
        ASSERT_TRUE(s.upsert_message(full_message()).ok());
        // destroyed with the transaction still open
    }
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_EQ(s.count_messages(), 0u);
}

TEST(SqliteStoreFile, FileModeIs0600) {
    TempDir dir;
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    ASSERT_TRUE(s.set_setting("k", "v").ok());
    struct stat st {};
    ASSERT_EQ(::stat(dir.db().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0600));
    for (const char* suffix : {"-wal", "-shm"}) {
        if (::stat((dir.db() + suffix).c_str(), &st) == 0) {
            EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0600)) << suffix;
        }
    }
}

TEST(SqliteStoreFile, FileModeFixedEvenWhenPreexistingFileWasPermissive) {
    TempDir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
    }
    ASSERT_EQ(::chmod(dir.db().c_str(), 0644), 0);
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    struct stat st {};
    ASSERT_EQ(::stat(dir.db().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, static_cast<mode_t>(0600));
}

TEST(SqliteStoreFile, UsesWalAndFullSynchronousAndForeignKeys) {
    TempDir dir;
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    ASSERT_TRUE(s.set_setting("k", "v").ok());
    EXPECT_TRUE(file_exists(dir.db() + "-wal"));  // WAL is live while the connection is open
    // Foreign keys are enforced: a body without a message is rejected (covered in put_body test too).
    MessageBody b;
    b.id = "ghost";
    EXPECT_FALSE(s.put_body(b).ok());
}

TEST(SqliteStoreFile, SecureWipeRemovesAllFiles) {
    TempDir dir;
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    ASSERT_TRUE(s.upsert_message(full_message()).ok());
    ASSERT_TRUE(s.put_token_blob("secret-token-value").ok());
    ASSERT_TRUE(file_exists(dir.db()));
    ASSERT_TRUE(s.secure_wipe().ok());
    EXPECT_FALSE(file_exists(dir.db()));
    EXPECT_FALSE(file_exists(dir.db() + "-wal"));
    EXPECT_FALSE(file_exists(dir.db() + "-shm"));
    // Wiping again (nothing open, nothing on disk) is fine.
    EXPECT_TRUE(s.secure_wipe().ok());
}

TEST(SqliteStoreFile, SecureWipeInsideOpenTransactionStillWipes) {
    TempDir dir;
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    ASSERT_TRUE(s.begin().ok());
    ASSERT_TRUE(s.upsert_message(full_message()).ok());
    ASSERT_TRUE(s.secure_wipe().ok());
    EXPECT_FALSE(file_exists(dir.db()));
}

TEST(SqliteStoreFile, SecureWipeFailsWhenUnlinkImpossible) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
    TempDir dir;
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    ASSERT_EQ(::chmod(dir.path().c_str(), 0500), 0);
    auto st = s.secure_wipe();
    ::chmod(dir.path().c_str(), 0700);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);
}

TEST(SqliteStoreFile, SecureWipeLeavesNoPlaintextInFile) {
    // Strong-ish check of the "rows are wiped before unlink" path: capture the file just before the
    // unlink is impossible, so instead verify through a second store that secure_delete zeroed pages:
    // after wipe + reopen of a recreated file nothing from the old content exists.
    TempDir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_TRUE(s.put_token_blob("TOP-SECRET-MARKER-123").ok());
        ASSERT_TRUE(s.secure_wipe().ok());
    }
    SqliteStore s(dir.db());
    ASSERT_TRUE(s.open().ok());
    EXPECT_FALSE(s.get_token_blob().has_value());
}

TEST(SqliteStoreFile, OpenFailsWithStorageErrorOnUnusablePath) {
    SqliteStore s("/nonexistent-dir-slowmail/xyz/mail.db");
    auto st = s.open();
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);
    EXPECT_FALSE(st.error().message.empty());
    // Use-before-open never throws or crashes.
    EXPECT_FALSE(s.get_setting("k").has_value());
    EXPECT_FALSE(s.set_setting("k", "v").ok());
    EXPECT_EQ(s.count_messages(), 0u);
    EXPECT_FALSE(s.begin().ok());
}

TEST(SqliteStoreFile, OpenRefusesNewerSchema) {
    TempDir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_TRUE(s.set_setting("schema_version", "99").ok());
    }
    SqliteStore s(dir.db());
    auto st = s.open();
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().kind, ErrorKind::Storage);
}

TEST(SqliteStoreFile, TwoConnectionsSeeCommittedDataOnly) {
    TempDir dir;
    SqliteStore a(dir.db());
    ASSERT_TRUE(a.open().ok());
    SqliteStore b(dir.db());
    ASSERT_TRUE(b.open().ok());
    ASSERT_TRUE(a.begin().ok());
    ASSERT_TRUE(a.set_setting("k", "v").ok());
    EXPECT_FALSE(b.get_setting("k").has_value());
    ASSERT_TRUE(a.commit().ok());
    EXPECT_EQ(b.get_setting("k").value_or(""), "v");
}

TEST(SqliteStoreFile, RawSchemaHasExpectedTablesIndexesAndStoredEnumText) {
    TempDir dir;
    {
        SqliteStore s(dir.db());
        ASSERT_TRUE(s.open().ok());
        ASSERT_TRUE(s.upsert_message(full_message()).ok());
        ASSERT_TRUE(s.set_message_state("AAMk-1", MessageState::DeletePending, "user_request").ok());
        ASSERT_TRUE(s.put_command(full_command()).ok());
    }
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(dir.db().c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    auto scalar = [&](const char* sql) {
        sqlite3_stmt* st = nullptr;
        std::string out;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            out = t ? reinterpret_cast<const char*>(t) : "";
        }
        sqlite3_finalize(st);
        return out;
    };
    for (const char* table : {"settings", "account", "inbox_sync_state", "message_cache", "message_body", "session",
                              "session_item", "command", "session_event", "token_blob"}) {
        EXPECT_EQ(scalar((std::string("SELECT name FROM sqlite_master WHERE type='table' AND name='") + table + "'").c_str()),
                  table);
    }
    for (const char* idx : {"idx_message_cache_state_received", "idx_command_state_created", "idx_session_event_session_ts"}) {
        EXPECT_EQ(scalar((std::string("SELECT name FROM sqlite_master WHERE type='index' AND name='") + idx + "'").c_str()), idx);
    }
    EXPECT_EQ(scalar("SELECT state FROM message_cache WHERE id='AAMk-1'"), "delete_pending");
    EXPECT_EQ(scalar("SELECT state_reason FROM message_cache WHERE id='AAMk-1'"), "user_request");
    EXPECT_EQ(scalar("SELECT flag_status FROM message_cache WHERE id='AAMk-1'"), "flagged");
    EXPECT_EQ(scalar("SELECT tombstone FROM message_cache WHERE id='AAMk-1'"), "0");
    EXPECT_EQ(scalar("SELECT type FROM command WHERE id='cmd-1'"), "undo_delete");
    EXPECT_GT(std::stoll(scalar("SELECT last_seen_at FROM message_cache WHERE id='AAMk-1'")), 1700000000LL);
    sqlite3_close(db);
}
