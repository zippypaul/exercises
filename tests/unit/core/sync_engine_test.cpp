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
    SyncEngine sync{store, backend, clock, CooldownPolicy{}};
    CommandProcessor commands{store, backend, clock};

    Env() {
        SessionRecord s;
        s.id = "s1";
        s.started_at = clock.now_wall();
        s.state = SessionState::Active;
        store.put_session(s);
    }

    void seed(int n) {
        for (int i = 1; i <= n; ++i) {
            backend.add_message("m" + std::to_string(i), "", 1'799'990'000 - i);
        }
    }
    SyncReport first_sync(int n = 3) {
        seed(n);
        return sync.run(false);
    }
    std::vector<std::string> queue_ids() {
        std::vector<std::string> ids;
        for (const auto& m : store.list_queue()) ids.push_back(m.id);
        return ids;
    }
    Command enqueue(const std::string& id, CommandType t) {
        auto c = commands.enqueue("s1", id, t);
        EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().message);
        return c.ok() ? c.value() : Command{};
    }
};

// Wraps a backend and duplicates every changed item and removal within a page (Graph may replay items).
class DuplicatingBackend final : public MailBackend {
public:
    explicit DuplicatingBackend(MailBackend& inner) : inner_(inner) {}
    Outcome<DeltaPage> fetch_inbox_delta(const std::string& link) override {
        auto out = inner_.fetch_inbox_delta(link);
        if (!out.ok()) return out;
        DeltaPage p = out.value();
        auto changed = p.changed;
        p.changed.insert(p.changed.end(), changed.begin(), changed.end());
        auto removed = p.removed_ids;
        p.removed_ids.insert(p.removed_ids.end(), removed.begin(), removed.end());
        return p;
    }
    Outcome<MessageBody> fetch_body(const std::string& id) override { return inner_.fetch_body(id); }
    Outcome<MoveResult> move(const std::string& id, WellKnownFolder f) override { return inner_.move(id, f); }
    Status set_flag(const std::string& id, FlagStatus s) override { return inner_.set_flag(id, s); }
    Outcome<AccountInfo> account() override { return inner_.account(); }

private:
    MailBackend& inner_;
};

} // namespace

// A01
TEST(SyncEngine, FirstSyncRecordsSuccessAndStartsCooldown) {
    Env e;
    const std::int64_t t0 = e.clock.now_wall();
    SyncReport r = e.first_sync(3);
    ASSERT_TRUE(r.complete) << r.error.message;
    EXPECT_FALSE(r.resumed);
    EXPECT_EQ(r.added, 3u);
    EXPECT_EQ(r.pages, 1u);
    EXPECT_EQ(e.queue_ids().size(), 3u);

    const InboxSyncState st = e.store.get_sync_state();
    ASSERT_TRUE(st.delta_link.has_value());
    EXPECT_FALSE(st.resume_next_link.has_value());
    ASSERT_TRUE(st.last_complete_wall.has_value());
    EXPECT_EQ(*st.last_complete_wall, t0);

    const CooldownDecision d = e.sync.cooldown_decision();
    EXPECT_FALSE(d.eligible);
    ASSERT_TRUE(d.next_eligible_wall.has_value());
    EXPECT_EQ(*d.next_eligible_wall, t0 + 4 * kHour);
}

// A02
TEST(SyncEngine, RefreshAtPlusOneHourIsPolicyErrorAndQueueStaysReadable) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    const int delta_calls = e.backend.count("delta");
    e.clock.advance(kHour);

    SyncReport r = e.sync.run(false);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.error.kind, ErrorKind::Policy);
    EXPECT_EQ(e.backend.count("delta"), delta_calls) << "no inbound fetch while cooling down";
    EXPECT_EQ(e.queue_ids().size(), 3u);
}

TEST(SyncEngine, ForceBypassesCooldownAndEligibleAgainAfterFourHours) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.clock.advance(kHour);
    EXPECT_TRUE(e.sync.run(true).complete);
    e.clock.advance(4 * kHour);
    EXPECT_TRUE(e.sync.run(false).complete);
}

TEST(SyncEngine, ProgressCallbackReportsPages) {
    Env e;
    e.backend.page_size = 2;
    e.seed(5);
    std::vector<std::size_t> seen;
    SyncReport r = e.sync.run(false, [&](std::size_t n) { seen.push_back(n); });
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.pages, 3u);
    EXPECT_EQ(seen, (std::vector<std::size_t>{1, 2, 3}));
}

// A07
TEST(SyncEngine, ErrorOnPageTwoOfThreeResumesAndDoesNotConsumeCooldown) {
    Env e;
    e.backend.page_size = 1;
    e.seed(3);
    e.backend.fail_delta_at_call(2, ErrorKind::Network);

    SyncReport r1 = e.sync.run(false);
    EXPECT_FALSE(r1.complete);
    EXPECT_EQ(r1.error.kind, ErrorKind::Network);
    EXPECT_EQ(r1.pages, 1u);
    const InboxSyncState st = e.store.get_sync_state();
    ASSERT_TRUE(st.resume_next_link.has_value());
    EXPECT_FALSE(st.delta_link.has_value());
    EXPECT_FALSE(st.last_complete_wall.has_value()) << "cooldown must not be consumed";
    EXPECT_TRUE(st.last_attempt_wall.has_value());
    EXPECT_TRUE(e.sync.cooldown_decision().eligible);
    EXPECT_EQ(e.store.count_messages(), 1u) << "page 1 was committed";

    SyncReport r2 = e.sync.run(false);
    ASSERT_TRUE(r2.complete) << r2.error.message;
    EXPECT_TRUE(r2.resumed);
    EXPECT_EQ(r2.pages, 2u);
    EXPECT_EQ(e.store.count_messages(), 3u);
    const InboxSyncState done = e.store.get_sync_state();
    EXPECT_FALSE(done.resume_next_link.has_value());
    EXPECT_TRUE(done.delta_link.has_value());
    EXPECT_TRUE(done.last_complete_wall.has_value());
    EXPECT_FALSE(e.sync.cooldown_decision().eligible);
}

TEST(SyncEngine, ErrorWhenStartingFromDeltaLinkLeavesDeltaLinkAndNoResume) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    const std::string delta = *e.store.get_sync_state().delta_link;
    e.backend.add_message("m9");
    e.backend.fail_next(ErrorKind::Server, std::nullopt, "delta");

    SyncReport r = e.sync.run(true);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.error.kind, ErrorKind::Server);
    const InboxSyncState st = e.store.get_sync_state();
    EXPECT_EQ(st.delta_link, delta);
    EXPECT_FALSE(st.resume_next_link.has_value());
    EXPECT_TRUE(st.last_attempt_wall.has_value());

    SyncReport r2 = e.sync.run(true);
    ASSERT_TRUE(r2.complete);
    EXPECT_FALSE(r2.resumed);
    EXPECT_EQ(r2.added, 1u);
}

TEST(SyncEngine, IncrementalSyncAddsUpdatesAndRemoves) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    e.backend.add_message("m4");
    e.backend.other_client_update("m1", "New subject for m1");
    e.backend.other_client_delete("m3");
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.added, 1u);
    EXPECT_EQ(r.updated, 1u);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_EQ(e.store.get_message("m1")->subject, "New subject for m1");
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::GoneOnServer);
    EXPECT_EQ(e.queue_ids().size(), 3u);  // m1, m2, m4
}

// A08 (pending command)
TEST(SyncEngine, AnotherClientMovedMessageWithPendingCommandAbandonsCommand) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    Command c = e.enqueue("m1", CommandType::Archive);
    e.backend.other_client_move("m1", WellKnownFolder::DeletedItems);

    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_EQ(r.reconciled_pending, 1u);

    auto after = e.store.get_command(c.id);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->state, CommandState::Abandoned);
    EXPECT_EQ(after->last_error, "message gone on server before action ran");
    auto events = e.store.list_events("s1");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].confirmation, ConfirmationState::Failed);
    EXPECT_EQ(events[0].failure_reason, "already gone");
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
    EXPECT_EQ(e.queue_ids(), (std::vector<std::string>{"m2", "m3"}));

    // Never resurrected by the processor either: nothing is sent to the server.
    e.backend.clear_calls();
    ProcessReport pr = e.commands.process();
    EXPECT_EQ(pr.confirmed, 0u);
    EXPECT_EQ(e.backend.count("move"), 0);
}

// A08 (no command)
TEST(SyncEngine, AnotherClientMovedMessageWithoutCommandLeavesQueue) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    e.backend.other_client_move("m2", WellKnownFolder::Archive);
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_EQ(r.reconciled_pending, 0u);
    EXPECT_EQ(e.queue_ids(), (std::vector<std::string>{"m1", "m3"}));
}

TEST(SyncEngine, PendingStateOnlyUpdatesFieldsWhenChangedElsewhere) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.enqueue("m1", CommandType::Archive);
    e.backend.other_client_update("m1", "edited elsewhere");
    ASSERT_TRUE(e.sync.run(true).complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::DoPending);
    EXPECT_EQ(e.store.get_message("m1")->subject, "edited elsewhere");
}

// Q5
TEST(SyncEngine, UnflaggedElsewhereRestoresDeferredMessageWithUndeferredEvent) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.enqueue("m1", CommandType::Defer);
    ASSERT_EQ(e.commands.process().confirmed, 1u);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Deferred);

    // Still flagged: stays hidden.
    e.backend.other_client_update("m1", "tweak");
    ASSERT_TRUE(e.sync.run(true).complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Deferred);

    e.backend.other_client_unflag("m1");
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.restored_to_queue, 1u);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
    bool found = false;
    for (const auto& ev : e.store.list_events("s1")) {
        if (ev.type == EventType::Undeferred && ev.command_id.empty()) {
            found = true;
            EXPECT_EQ(ev.confirmation, ConfirmationState::Confirmed);
            EXPECT_EQ(ev.message_id, "m1");
        }
    }
    EXPECT_TRUE(found);
}

TEST(SyncEngine, DeferredMessageRemovedElsewhereIsTombstoned) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.enqueue("m1", CommandType::Defer);
    e.commands.process();
    e.backend.other_client_delete("m1");
    ASSERT_TRUE(e.sync.run(true).complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
}

TEST(SyncEngine, OurOwnArchiveShowingAsRemovedKeepsArchivedStateForUndo) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.enqueue("m1", CommandType::Archive);
    ASSERT_EQ(e.commands.process().confirmed, 1u);
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::Archived);
    // ... so Undo still works after a refresh.
    auto undo = e.commands.enqueue("s1", "m1", CommandType::UndoArchive);
    EXPECT_TRUE(undo.ok());
}

TEST(SyncEngine, ArchivedMessageReportedBackInInboxIsServerTruth) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.enqueue("m1", CommandType::Archive);
    e.commands.process();
    e.backend.other_client_move("m1", WellKnownFolder::Inbox);
    ASSERT_TRUE(e.sync.run(true).complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
}

// Graph contract: moved out then back = @removed then a normal changed item.
TEST(SyncEngine, MovedOutThenBackUnTombstones) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.backend.other_client_move("m1", WellKnownFolder::Archive);
    ASSERT_TRUE(e.sync.run(true).complete);
    ASSERT_EQ(e.store.get_message_state("m1"), MessageState::GoneOnServer);
    EXPECT_EQ(e.queue_ids(), (std::vector<std::string>{"m2"}));

    e.backend.other_client_move("m1", WellKnownFolder::Inbox);
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
    EXPECT_EQ(e.queue_ids().size(), 2u);
}

TEST(SyncEngine, RemovedThenChangedAcrossPagesEndsInQueue) {
    Env e;
    e.backend.page_size = 1;
    ASSERT_TRUE(e.first_sync(2).complete);
    // Net effect delivered in one incremental sync, split over pages: m1 is gone, m2 changed.
    e.backend.other_client_move("m1", WellKnownFolder::Archive);
    e.backend.other_client_move("m1", WellKnownFolder::Inbox);
    e.backend.other_client_update("m2", "changed");
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
    EXPECT_EQ(e.store.get_message("m2")->subject, "changed");
}

// Graph contract: duplicates / replays must be idempotent.
TEST(SyncEngine, DuplicateItemsInOnePageAreIdempotent) {
    MemoryStore store;
    FakeBackend fake;
    DuplicatingBackend dup(fake);
    FakeClock clock;
    SyncEngine sync(store, dup, clock, CooldownPolicy{});
    for (int i = 1; i <= 3; ++i) fake.add_message("m" + std::to_string(i));

    SyncReport r = sync.run(false);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.added, 3u);
    EXPECT_EQ(r.updated, 0u);
    EXPECT_EQ(store.count_messages(), 3u);

    fake.other_client_delete("m3");
    fake.other_client_update("m1", "x");
    SyncReport r2 = sync.run(true);
    ASSERT_TRUE(r2.complete);
    EXPECT_EQ(r2.removed, 1u);
    EXPECT_EQ(r2.updated, 1u);
}

TEST(SyncEngine, ReplayingTheSamePageTwiceYieldsSameStoreState) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    e.backend.other_client_update("m1", "same");
    // Run twice from the same delta link by restoring the sync state between runs.
    const InboxSyncState before = e.store.get_sync_state();
    ASSERT_TRUE(e.sync.run(true).complete);
    auto q1 = e.store.list_queue();
    const std::size_t count1 = e.store.count_messages();
    e.store.put_sync_state(before);
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.added, 0u);
    EXPECT_EQ(r.updated, 0u) << "item identical to the cache is a no-op";
    EXPECT_EQ(r.removed, 0u);
    auto q2 = e.store.list_queue();
    ASSERT_EQ(q1.size(), q2.size());
    for (std::size_t i = 0; i < q1.size(); ++i) {
        EXPECT_EQ(q1[i].id, q2[i].id);
        EXPECT_EQ(q1[i].subject, q2[i].subject);
    }
    EXPECT_EQ(e.store.count_messages(), count1);
}

TEST(SyncEngine, GoneTriggersFullResyncAndTombstonesUnseenMessages) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    e.backend.other_client_purge("m3");
    e.backend.add_message("m4", "", 1'799'900'000);
    e.backend.expire_delta_links();

    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete) << r.error.message;
    EXPECT_TRUE(r.full_resync);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::GoneOnServer);
    EXPECT_EQ(e.store.get_message_state("m4"), MessageState::InQueue);
    EXPECT_EQ(e.queue_ids(), (std::vector<std::string>{"m1", "m2", "m4"}));
    EXPECT_TRUE(e.store.get_sync_state().delta_link.has_value());

    // And the next sync is a plain incremental one again.
    SyncReport r2 = e.sync.run(true);
    ASSERT_TRUE(r2.complete);
    EXPECT_FALSE(r2.full_resync);
}

TEST(SyncEngine, GoneOnResumeLinkAlsoFullResyncs) {
    Env e;
    e.backend.page_size = 1;
    e.seed(3);
    e.backend.fail_delta_at_call(2, ErrorKind::Network);
    ASSERT_FALSE(e.sync.run(false).complete);
    e.backend.expire_delta_links();
    SyncReport r = e.sync.run(false);
    ASSERT_TRUE(r.complete) << r.error.message;
    EXPECT_TRUE(r.full_resync);
    EXPECT_FALSE(r.resumed);
    EXPECT_EQ(e.queue_ids().size(), 3u);
}

TEST(SyncEngine, FullResyncTombstoneAbandonsPendingCommands) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    Command c = e.enqueue("m2", CommandType::Delete);
    e.backend.other_client_purge("m2");
    e.backend.expire_delta_links();
    SyncReport r = e.sync.run(true);
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(r.reconciled_pending, 1u);
    EXPECT_EQ(e.store.get_command(c.id)->state, CommandState::Abandoned);
}

TEST(SyncEngine, SecondGoneIsAnError) {
    Env e;
    ASSERT_TRUE(e.first_sync(2).complete);
    e.backend.fail_next(ErrorKind::Gone, std::nullopt, "delta", 2);
    SyncReport r = e.sync.run(true);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.error.kind, ErrorKind::Gone);
    EXPECT_TRUE(r.full_resync);
    // Nothing was tombstoned and the cooldown stays at the earlier successful sync.
    EXPECT_EQ(e.queue_ids().size(), 2u);
    // A later run recovers with a clean full resync.
    SyncReport r2 = e.sync.run(true);
    ASSERT_TRUE(r2.complete) << r2.error.message;
    EXPECT_TRUE(r2.full_resync);
}

TEST(SyncEngine, InterruptedFullResyncRestartsSoUnseenMessagesAreStillTombstoned) {
    Env e;
    ASSERT_TRUE(e.first_sync(3).complete);
    e.backend.page_size = 1;
    e.backend.other_client_purge("m3");
    e.backend.expire_delta_links();
    // call 1: expired delta link -> Gone; call 2: full resync page 1; call 3: page 2 fails.
    e.backend.fail_delta_at_call(3, ErrorKind::Network);
    SyncReport r = e.sync.run(true);
    ASSERT_FALSE(r.complete);
    EXPECT_TRUE(r.full_resync);
    SyncReport r2 = e.sync.run(true);
    ASSERT_TRUE(r2.complete) << r2.error.message;
    EXPECT_TRUE(r2.full_resync) << "interrupted resync is restarted, not resumed";
    EXPECT_FALSE(r2.resumed);
    EXPECT_EQ(e.store.get_message_state("m3"), MessageState::GoneOnServer);
    EXPECT_EQ(e.queue_ids().size(), 2u);
}

TEST(SyncEngine, StorageFailureMidPageRollsBackAndRetrySucceeds) {
    Env e;
    e.seed(3);
    e.store.fail_next_write();
    SyncReport r = e.sync.run(false);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.error.kind, ErrorKind::Storage);
    EXPECT_EQ(e.store.count_messages(), 0u) << "page applied atomically";
    EXPECT_FALSE(e.store.get_sync_state().last_complete_wall.has_value());
    SyncReport r2 = e.sync.run(false);
    ASSERT_TRUE(r2.complete);
    EXPECT_EQ(e.store.count_messages(), 3u);
}

TEST(SyncEngine, NewFlaggedMessageStillEntersQueue) {
    Env e;
    e.backend.add_message("m1").flag_status = FlagStatus::Flagged;
    ASSERT_TRUE(e.sync.run(false).complete);
    EXPECT_EQ(e.store.get_message_state("m1"), MessageState::InQueue);
}
