#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "fake_backend.hpp"
#include "slowmail/core/clock.hpp"
#include "slowmail/core/command_processor.hpp"
#include "slowmail/core/memory_store.hpp"
#include "slowmail/core/sync_engine.hpp"

using namespace slowmail::core;
using slowmail::testing::FakeBackend;

namespace {

constexpr std::int64_t kHour = 3600;

struct Env {
    MemoryStore store;
    FakeBackend backend;
    FakeClock clock;
    RetryPolicy policy;
    std::unique_ptr<CommandProcessor> commands;

    explicit Env(RetryPolicy p = {}) : policy(p) {
        SessionRecord s;
        s.id = "s1";
        s.started_at = clock.now_wall();
        s.state = SessionState::Active;
        store.put_session(s);
        for (int i = 1; i <= 3; ++i) backend.add_message("m" + std::to_string(i), "Subject " + std::to_string(i), 1'799'990'000 - i);
        SyncEngine sync(store, backend, clock, CooldownPolicy{});
        EXPECT_TRUE(sync.run(false).complete);
        clock.advance(kHour);  // actions happen at "03:00", one hour after the 02:00 sync
        backend.clear_calls();
        commands = std::make_unique<CommandProcessor>(store, backend, clock, policy);
    }

    Command enqueue(const std::string& id, CommandType t, const std::string& payload = "{}") {
        auto c = commands->enqueue("s1", id, t, payload);
        EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().message);
        return c.ok() ? c.value() : Command{};
    }
    Command cmd(const std::string& id) { return *store.get_command(id); }
    SessionEvent event_for(const std::string& command_id) {
        for (const auto& e : store.list_events("s1")) {
            if (e.command_id == command_id) return e;
        }
        ADD_FAILURE() << "no event for command";
        return {};
    }
    static std::int64_t not_before(const Command& c) {
        auto j = nlohmann::json::parse(c.payload_json);
        return j.value("not_before", static_cast<std::int64_t>(0));
    }
};

} // namespace

TEST(CommandProcessor, EnqueueWritesCommandStateAndPendingEventAtomically) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    EXPECT_EQ(c.state, CommandState::Pending);
    EXPECT_EQ(c.session_id, "s1");
    EXPECT_EQ(c.attempts, 0);
    EXPECT_FALSE(c.idempotency_key.empty());
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::DoPending);
    EXPECT_TRUE(e.store.list_queue().size() == 2u);

    SessionEvent ev = e.event_for(c.id);
    EXPECT_EQ(ev.type, EventType::Archived);
    EXPECT_EQ(ev.confirmation, ConfirmationState::Pending);
    auto snap = nlohmann::json::parse(ev.display_snapshot_json);
    EXPECT_EQ(snap["subject"], "Subject 1");
    EXPECT_EQ(snap["from"], "Sender m1");
    EXPECT_EQ(snap["received_at_iso"], "2026-10-20T00:00:00Z");
    ASSERT_EQ(e.store.list_session_items("s1").size(), 1u);
    EXPECT_EQ(e.store.list_session_items("s1")[0].current_state, MessageState::DoPending);
    EXPECT_EQ(e.backend.count("move"), 0) << "enqueue alone never touches the network";
}

TEST(CommandProcessor, EnqueueIsIdempotent) {
    Env e;
    Command a = e.enqueue("m1", CommandType::Archive);
    Command b = e.enqueue("m1", CommandType::Archive);
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(e.store.list_commands(CommandState::Pending).size(), 1u);
    EXPECT_EQ(e.store.list_events("s1").size(), 1u);

    ASSERT_EQ(e.commands->process().confirmed, 1u);
    Command c = e.enqueue("m1", CommandType::Archive);  // after confirmation: still the same action
    EXPECT_EQ(c.id, a.id);
    EXPECT_EQ(e.backend.count("move", "m1"), 1);
    EXPECT_EQ(CommandProcessor::make_idempotency_key("s1", "m1", CommandType::Archive), a.idempotency_key);
}

TEST(CommandProcessor, EnqueueRejectsInvalidRequests) {
    Env e;
    auto unknown = e.commands->enqueue("s1", "nope", CommandType::Archive);
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.error().kind, ErrorKind::Invalid);

    e.enqueue("m1", CommandType::Archive);
    auto second = e.commands->enqueue("s1", "m1", CommandType::Delete);
    ASSERT_FALSE(second.ok()) << "another command pending";
    EXPECT_EQ(second.error().kind, ErrorKind::Invalid);

    e.store.mark_gone("m2");
    auto gone = e.commands->enqueue("s1", "m2", CommandType::Archive);
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(gone.error().kind, ErrorKind::Invalid);

    EXPECT_FALSE(e.commands->enqueue("s1", "m3", CommandType::Undefer).ok()) << "m3 is not deferred";
    EXPECT_FALSE(e.commands->enqueue("s1", "m3", CommandType::UndoArchive).ok()) << "nothing to undo";
    EXPECT_FALSE(e.commands->enqueue("s1", "m3", CommandType::UndoDelete, R"({"original_command_id":"cmd-x"})").ok());
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::InQueue);
}

// A03
TEST(CommandProcessor, ArchiveConfirmsDespiteInboundCooldown) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.confirmed, 1u);
    EXPECT_FALSE(r.went_offline);
    EXPECT_TRUE(e.backend.in_archive("m1"));
    EXPECT_FALSE(e.backend.in_inbox("m1"));
    Command after = e.cmd(c.id);
    EXPECT_EQ(after.state, CommandState::Confirmed);
    EXPECT_EQ(after.attempts, 1);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Archived);
    EXPECT_EQ(e.event_for(c.id).confirmation, ConfirmationState::Confirmed);
    auto res = nlohmann::json::parse(after.server_result_json);
    EXPECT_EQ(res["new_id"], "m1");
    EXPECT_EQ(res["new_folder_id"], "archive");
}

// A04
TEST(CommandProcessor, DeleteMovesToDeletedItemsNotHardDelete) {
    Env e;
    Command c = e.enqueue("m2", CommandType::Delete);
    EXPECT_EQ(e.store.get_message_state("m2"), MessageState::DeletePending);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_TRUE(e.backend.in_deleted("m2"));
    EXPECT_FALSE(e.backend.in_archive("m2"));
    EXPECT_EQ(e.store.get_message_state("m2"), MessageState::Deleted);
    EXPECT_EQ(e.event_for(c.id).type, EventType::Deleted);
}

// A05
TEST(CommandProcessor, DeferFlagsOnServerKeepsInboxCopyAndHidesLocally) {
    Env e;
    Command c = e.enqueue("m3", CommandType::Defer);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::DeferPending);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_TRUE(e.backend.in_inbox("m3"));
    EXPECT_EQ(e.backend.flag_of("m3"), FlagStatus::Flagged);
    EXPECT_EQ(e.backend.count("move"), 0);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::Deferred);
    EXPECT_EQ(e.store.list_session_items("s1")[0].suppression_reason, "deferred_by_slowmail");
    EXPECT_EQ(e.store.list_queue().size(), 2u);
    EXPECT_EQ(e.store.get_message("m3")->flag_status, FlagStatus::Flagged);
    EXPECT_EQ(e.event_for(c.id).type, EventType::Deferred);
}

TEST(CommandProcessor, UndeferClearsFlagAndRestoresQueue) {
    Env e;
    e.enqueue("m3", CommandType::Defer);
    e.commands->process();
    Command u = e.enqueue("m3", CommandType::Undefer);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::DeferPending);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_EQ(e.backend.flag_of("m3"), FlagStatus::NotFlagged);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::InQueue);
    EXPECT_EQ(e.event_for(u.id).type, EventType::Undeferred);
    EXPECT_EQ(e.event_for(u.id).confirmation, ConfirmationState::Confirmed);
}

TEST(CommandProcessor, ProcessesFifoOneAtATime) {
    Env e;
    e.enqueue("m2", CommandType::Archive);
    e.enqueue("m1", CommandType::Delete);
    e.enqueue("m3", CommandType::Defer);
    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.confirmed, 3u);
    ASSERT_EQ(e.backend.calls.size(), 3u);
    EXPECT_EQ(e.backend.calls[0].arg, "m2");
    EXPECT_EQ(e.backend.calls[1].arg, "m1");
    EXPECT_EQ(e.backend.calls[2].arg, "m3");
}

TEST(CommandProcessor, InFlightIsPersistedBeforeTheNetworkCall) {
    // A backend that inspects the store from inside the call.
    struct Spy final : MailBackend {
        Store* store = nullptr;
        std::optional<CommandState> seen;
        Outcome<DeltaPage> fetch_inbox_delta(const std::string&) override { return Error{}; }
        Outcome<MessageBody> fetch_body(const std::string&) override { return Error{}; }
        Outcome<MoveResult> move(const std::string& id, WellKnownFolder) override {
            auto cmds = store->list_commands_for_message(id);
            seen = cmds.empty() ? std::nullopt : std::optional<CommandState>(cmds[0].state);
            return MoveResult{id, "archive"};
        }
        Status set_flag(const std::string&, FlagStatus) override { return Status(); }
        Outcome<AccountInfo> account() override { return AccountInfo{}; }
    };
    Env e;
    Spy spy;
    spy.store = &e.store;
    CommandProcessor p(e.store, spy, e.clock);
    ASSERT_TRUE(p.enqueue("s1", "m1", CommandType::Archive).ok());
    ASSERT_EQ(p.process().confirmed, 1u);
    ASSERT_TRUE(spy.seen.has_value());
    EXPECT_EQ(*spy.seen, CommandState::InFlight);
}

// A06
TEST(CommandProcessor, CrashDuringInFlightIsRecoveredAndRetried) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    // Simulate the process dying right after InFlight was persisted.
    Command inflight = e.cmd(c.id);
    inflight.state = CommandState::InFlight;
    inflight.attempts = 1;
    ASSERT_TRUE(e.store.put_command(inflight).ok());
    EXPECT_EQ(e.event_for(c.id).confirmation, ConfirmationState::Pending) << "no false success";

    CommandProcessor restarted(e.store, e.backend, e.clock, e.policy);
    EXPECT_EQ(restarted.recover_in_flight(), 1u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Pending);
    EXPECT_EQ(e.cmd(c.id).attempts, 1);
    EXPECT_EQ(restarted.recover_in_flight(), 0u);

    ProcessReport r = restarted.process();
    EXPECT_EQ(r.confirmed, 1u);
    EXPECT_EQ(e.backend.count("move", "m1"), 1);
    EXPECT_TRUE(e.backend.in_archive("m1"));
    EXPECT_EQ(e.cmd(c.id).attempts, 2);
    EXPECT_EQ(e.store.list_commands_for_message("m1").size(), 1u) << "no duplicate command";
}

TEST(CommandProcessor, CrashRecoveryWhenServerAlreadyActedIsAbandonedNotDuplicated) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    Command inflight = e.cmd(c.id);
    inflight.state = CommandState::InFlight;
    inflight.attempts = 1;
    e.store.put_command(inflight);
    e.backend.other_client_purge("m1");  // server no longer has it

    CommandProcessor restarted(e.store, e.backend, e.clock, e.policy);
    ASSERT_EQ(restarted.recover_in_flight(), 1u);
    ProcessReport r = restarted.process();
    EXPECT_EQ(r.abandoned, 1u);
    EXPECT_EQ(r.confirmed, 0u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Abandoned);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
    SessionEvent ev = e.event_for(c.id);
    EXPECT_EQ(ev.confirmation, ConfirmationState::Failed);
    EXPECT_EQ(ev.failure_reason, "already gone");
    EXPECT_EQ(e.backend.count("move", "m1"), 1);
    // Nothing left to retry.
    e.backend.clear_calls();
    EXPECT_EQ(restarted.process().confirmed, 0u);
    EXPECT_TRUE(e.backend.calls.empty());
}

TEST(CommandProcessor, NotFoundWithItemNotFoundOrEmptyCodeMeansGone) {
    Env e;
    e.enqueue("m1", CommandType::Archive);
    e.backend.fail_next(ErrorKind::NotFound, std::nullopt, "move", 1, "");
    EXPECT_EQ(e.commands->process().abandoned, 1u);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);

    e.enqueue("m2", CommandType::Delete);
    e.backend.fail_next(ErrorKind::NotFound, std::nullopt, "move", 1, "ErrorItemNotFound");
    EXPECT_EQ(e.commands->process().abandoned, 1u);
    EXPECT_EQ(e.store.get_message_state("m2"), MessageState::GoneOnServer);
}

TEST(CommandProcessor, NotFoundWithOtherCodeIsNonRetryableFailureNotGone) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    e.backend.fail_next(ErrorKind::NotFound, std::nullopt, "move", 1, "ErrorFolderNotFound");
    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.failed, 1u);
    EXPECT_EQ(r.abandoned, 0u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Failed);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Failed);
    EXPECT_NE(e.store.get_message_state("m1"), MessageState::GoneOnServer);
    EXPECT_EQ(e.store.list_session_items("s1")[0].suppression_reason, "ErrorFolderNotFound");
    SessionEvent ev = e.event_for(c.id);
    EXPECT_EQ(ev.confirmation, ConfirmationState::Failed);
    EXPECT_EQ(ev.failure_reason, "ErrorFolderNotFound");
    // Not retried.
    e.backend.clear_calls();
    e.commands->process();
    EXPECT_TRUE(e.backend.calls.empty());
    // The user may try again later: a Failed message accepts a fresh command.
    auto again = e.commands->enqueue("s1", "m1", CommandType::Archive);
    ASSERT_TRUE(again.ok());
    EXPECT_NE(again.value().id, c.id);
    EXPECT_EQ(e.commands->process().confirmed, 1u);
}

TEST(CommandProcessor, ForbiddenFailsImmediately) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Delete);
    e.backend.fail_next(ErrorKind::Forbidden, std::nullopt, "move");
    EXPECT_EQ(e.commands->process().failed, 1u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Failed);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Failed);
}

// A09 / Q8
TEST(CommandProcessor, NetworkErrorLeavesPendingStopsAndSucceedsLater) {
    Env e;
    e.enqueue("m1", CommandType::Archive);
    e.enqueue("m2", CommandType::Archive);
    e.backend.offline = true;
    ProcessReport r = e.commands->process();
    EXPECT_TRUE(r.went_offline);
    EXPECT_EQ(r.confirmed, 0u);
    EXPECT_EQ(e.backend.count("move"), 1) << "stops at the first network failure";
    EXPECT_EQ(e.store.list_commands(CommandState::Pending).size(), 2u);
    EXPECT_EQ(e.store.list_commands(CommandState::Pending)[0].attempts, 0) << "never reached the server";
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::DoPending);

    e.backend.offline = false;
    ProcessReport r2 = e.commands->process();
    EXPECT_EQ(r2.confirmed, 2u);
    EXPECT_FALSE(r2.went_offline);
}

TEST(CommandProcessor, AuthErrorLeavesPendingAndSignalsAuthRequired) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    e.backend.fail_next(ErrorKind::Auth, std::nullopt, "move");
    ProcessReport r = e.commands->process();
    EXPECT_TRUE(r.auth_required);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Pending);
    EXPECT_EQ(e.commands->process().confirmed, 1u);
}

TEST(CommandProcessor, ThrottledWithRetryAfterStaysPendingWithNotBeforeAndStops) {
    Env e;
    Command c1 = e.enqueue("m1", CommandType::Archive);
    Command c2 = e.enqueue("m2", CommandType::Archive);
    const std::int64_t now = e.clock.now_wall();
    e.backend.fail_next(ErrorKind::Throttled, 30, "move");

    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.confirmed, 0u);
    ASSERT_TRUE(r.retry_after_seconds.has_value());
    EXPECT_EQ(*r.retry_after_seconds, 30);
    EXPECT_EQ(r.deferred_retry, 1u);
    EXPECT_EQ(e.backend.count("move"), 1) << "processing stops for this run";
    EXPECT_EQ(e.cmd(c1.id).state, CommandState::Pending);
    EXPECT_EQ(Env::not_before(e.cmd(c1.id)), now + 30);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::DoPending);

    // Before not_before the throttled command is skipped (no sleeping, no network); the other one proceeds.
    e.clock.advance(10);
    e.backend.clear_calls();
    ProcessReport r2 = e.commands->process();
    EXPECT_EQ(r2.deferred_retry, 1u);
    EXPECT_EQ(r2.confirmed, 1u);
    EXPECT_EQ(e.backend.count("move", "m1"), 0);
    EXPECT_EQ(e.cmd(c2.id).state, CommandState::Confirmed);

    e.clock.advance(21);
    EXPECT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_EQ(e.cmd(c1.id).state, CommandState::Confirmed);
}

TEST(CommandProcessor, ShortRetryAfterIsRaisedToExponentialBackoff) {
    RetryPolicy p;
    p.base_backoff_seconds = 4;
    Env e(p);
    Command c = e.enqueue("m1", CommandType::Archive);
    const std::int64_t now = e.clock.now_wall();
    e.backend.fail_next(ErrorKind::Throttled, 1, "move");
    e.commands->process();
    EXPECT_EQ(Env::not_before(e.cmd(c.id)), now + 4) << "max(retry_after=1, backoff=4)";
}

TEST(CommandProcessor, ServerErrorsBackOffExponentiallyThenFailAfterMaxAttempts) {
    RetryPolicy p;
    p.max_attempts = 3;
    p.base_backoff_seconds = 2;
    p.max_backoff_seconds = 300;
    Env e(p);
    Command c = e.enqueue("m1", CommandType::Archive);
    e.backend.fail_next(ErrorKind::Server, std::nullopt, "move", 10);

    std::int64_t t = e.clock.now_wall();
    e.commands->process();
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Pending);
    EXPECT_EQ(Env::not_before(e.cmd(c.id)), t + 2);

    e.clock.advance(2);
    t = e.clock.now_wall();
    e.commands->process();
    EXPECT_EQ(Env::not_before(e.cmd(c.id)), t + 4);

    e.clock.advance(4);
    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.failed, 1u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Failed);
    EXPECT_EQ(e.cmd(c.id).attempts, 3);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Failed);
}

TEST(CommandProcessor, ConflictStaysPendingOnceThenSucceeds) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Defer);
    e.backend.fail_next(ErrorKind::Conflict, std::nullopt, "flag");
    EXPECT_EQ(e.commands->process().deferred_retry, 1u);
    EXPECT_EQ(e.cmd(c.id).state, CommandState::Pending);
    e.clock.advance(5);
    EXPECT_EQ(e.commands->process().confirmed, 1u);
}

TEST(CommandProcessor, ErrorTextsAreRedactedNotSubjects) {
    Env e;
    Command c = e.enqueue("m1", CommandType::Archive);
    e.backend.fail_next(ErrorKind::Forbidden, std::nullopt, "move");
    e.commands->process();
    EXPECT_EQ(e.cmd(c.id).last_error.find("Subject 1"), std::string::npos);
}

// Undo
TEST(CommandProcessor, UndoArchiveMovesBackToInboxAndRestoresQueue) {
    Env e;
    Command a = e.enqueue("m1", CommandType::Archive);
    e.commands->process();
    Command u = e.enqueue("m1", CommandType::UndoArchive, R"({"original_command_id":")" + a.id + R"("})");
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::DoPending);
    EXPECT_EQ(e.event_for(u.id).type, EventType::UndoArchive);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_TRUE(e.backend.in_inbox("m1"));
    EXPECT_FALSE(e.backend.in_archive("m1"));
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
    EXPECT_EQ(e.event_for(u.id).confirmation, ConfirmationState::Confirmed);
    EXPECT_EQ(e.store.list_queue().size(), 3u);
}

TEST(CommandProcessor, UndoFindsLatestConfirmedOriginalWhenPayloadOmitted) {
    Env e;
    Command d = e.enqueue("m2", CommandType::Delete);
    e.commands->process();
    Command u = e.enqueue("m2", CommandType::UndoDelete);
    EXPECT_NE(u.payload_json.find(d.id), std::string::npos);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_TRUE(e.backend.in_inbox("m2"));
    EXPECT_EQ(e.store.get_message_state("m2"), MessageState::InQueue);
    // Undo is itself idempotent.
    EXPECT_EQ(e.enqueue("m2", CommandType::UndoDelete).id, u.id);
}

TEST(CommandProcessor, ArchiveUndoArchiveCreatesDistinctCommands) {
    Env e;
    Command a1 = e.enqueue("m1", CommandType::Archive);
    e.commands->process();
    e.enqueue("m1", CommandType::UndoArchive);
    e.commands->process();
    Command a2 = e.enqueue("m1", CommandType::Archive);
    EXPECT_NE(a1.id, a2.id);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    EXPECT_TRUE(e.backend.in_archive("m1"));
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Archived);
}

TEST(CommandProcessor, UndoOnlyForConfirmedOriginal) {
    Env e;
    Command a = e.enqueue("m1", CommandType::Archive);  // still Pending
    auto u = e.commands->enqueue("s1", "m1", CommandType::UndoArchive, R"({"original_command_id":")" + a.id + R"("})");
    ASSERT_FALSE(u.ok());
    EXPECT_EQ(u.error().kind, ErrorKind::Invalid);
}

TEST(CommandProcessor, UndoWithMismatchedTypeIsRejected) {
    Env e;
    Command a = e.enqueue("m1", CommandType::Archive);
    e.commands->process();
    auto u = e.commands->enqueue("s1", "m1", CommandType::UndoDelete, R"({"original_command_id":")" + a.id + R"("})");
    EXPECT_FALSE(u.ok());
}

TEST(CommandProcessor, UndoWhenMovedAwayElsewhereIsAbandonedAsGone) {
    Env e;
    e.enqueue("m1", CommandType::Archive);
    e.commands->process();
    e.enqueue("m1", CommandType::UndoArchive);
    e.backend.other_client_purge("m1");
    EXPECT_EQ(e.commands->process().abandoned, 1u);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
}

// immutable=false: ids change on move.
TEST(CommandProcessor, UndoUsesNewIdFromOriginalCommandWhenIdsAreNotImmutable) {
    Env e;
    e.backend.immutable_ids = false;
    Command a = e.enqueue("m1", CommandType::Archive);
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    auto res = nlohmann::json::parse(e.cmd(a.id).server_result_json);
    const std::string archived_id = res["new_id"];
    ASSERT_NE(archived_id, "m1");
    EXPECT_TRUE(e.backend.in_archive(archived_id));
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Archived) << "cache row stays under the original id";

    Command u = e.enqueue("m1", CommandType::UndoArchive);
    e.backend.clear_calls();
    ASSERT_EQ(e.commands->process().confirmed, 1u);
    ASSERT_EQ(e.backend.calls.size(), 1u);
    EXPECT_EQ(e.backend.calls[0].arg, archived_id) << "undo must move the CURRENT server id";
    EXPECT_EQ(e.backend.calls[0].extra, "inbox");
    auto ures = nlohmann::json::parse(e.cmd(u.id).server_result_json);
    const std::string inbox_id = ures["new_id"];
    EXPECT_TRUE(e.backend.in_inbox(inbox_id));

    // The queue offers the id the server now knows; the stale row is tombstoned.
    EXPECT_EQ(e.store.get_message_state(inbox_id), MessageState::InQueue);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
    // And it can be archived again under that id.
    auto again = e.commands->enqueue("s1", inbox_id, CommandType::Archive);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(e.commands->process().confirmed, 1u);
}

TEST(CommandProcessor, ProcessWithNothingPendingIsANoop) {
    Env e;
    ProcessReport r = e.commands->process();
    EXPECT_EQ(r.confirmed + r.failed + r.abandoned + r.deferred_retry, 0u);
    EXPECT_FALSE(r.went_offline);
    EXPECT_TRUE(e.backend.calls.empty());
}
