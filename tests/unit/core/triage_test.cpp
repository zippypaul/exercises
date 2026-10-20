#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "fake_backend.hpp"
#include "slowmail/core/clock.hpp"
#include "slowmail/core/memory_store.hpp"
#include "slowmail/core/triage.hpp"

using namespace slowmail::core;
using slowmail::testing::FakeBackend;

namespace {

constexpr std::int64_t kHour = 3600;

struct Rig {
    MemoryStore* store = nullptr;
    FakeBackend* backend = nullptr;
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::unique_ptr<TriageSession> session;

    explicit Rig(int messages = 3, TriageConfig cfg = {}, std::unique_ptr<MemoryStore> pre = nullptr) {
        auto s = pre ? std::move(pre) : std::make_unique<MemoryStore>();
        auto b = std::make_unique<FakeBackend>();
        store = s.get();
        backend = b.get();
        for (int i = 1; i <= messages; ++i) {
            backend->add_message("m" + std::to_string(i), "Subject " + std::to_string(i), 1'799'990'000 - i);
        }
        session = std::make_unique<TriageSession>(std::move(s), std::move(b), clock, cfg);
    }
    // start + first sync at "02:00", then jump to "03:00".
    void ready() {
        ASSERT_TRUE(session->start().ok());
        auto r = session->refresh();
        ASSERT_TRUE(r.ok()) << r.error().message;
        clock->advance(kHour);
        backend->clear_calls();
    }
    std::vector<std::string> queue_ids() {
        std::vector<std::string> ids;
        for (const auto& m : session->queue()) ids.push_back(m.id);
        return ids;
    }
};

} // namespace

TEST(TriageSession, StartCreatesReadyCachedSessionWithoutTouchingTheNetwork) {
    Rig r;
    ASSERT_TRUE(r.session->start().ok());
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.session_state, SessionState::ReadyCached);
    EXPECT_TRUE(st.signed_in);
    EXPECT_FALSE(st.offline);
    EXPECT_FALSE(st.session_id.empty());
    EXPECT_TRUE(st.cooldown.eligible);
    EXPECT_EQ(st.cooldown.reason, "first_download");
    EXPECT_EQ(st.queue_size, 0u);
    EXPECT_EQ(r.backend->count("delta"), 0);
    auto rec = r.store->get_active_session();
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->id, st.session_id);
    EXPECT_EQ(rec->state, SessionState::ReadyCached);
    EXPECT_TRUE(r.session->start().ok()) << "start is idempotent";
    EXPECT_EQ(r.session->status().session_id, st.session_id);
}

TEST(TriageSession, NotSignedInStartsInAuthRequired) {
    Rig r;
    r.backend->signed_in = false;
    ASSERT_TRUE(r.session->start().ok());
    EXPECT_EQ(r.session->status().session_state, SessionState::AuthRequired);
    EXPECT_FALSE(r.session->status().signed_in);
    EXPECT_FALSE(r.session->refresh().ok());
    EXPECT_FALSE(r.session->archive("m1").ok());
    r.backend->signed_in = true;
    ASSERT_TRUE(r.session->start().ok());
    EXPECT_EQ(r.session->status().session_state, SessionState::ReadyCached);
}

TEST(TriageSession, CallsBeforeStartAreInvalid) {
    Rig r;
    EXPECT_EQ(r.session->refresh().error().kind, ErrorKind::Invalid);
    EXPECT_EQ(r.session->archive("m1").error().kind, ErrorKind::Invalid);
    EXPECT_EQ(r.session->end().error().kind, ErrorKind::Invalid);
    EXPECT_TRUE(r.session->queue().empty());
}

// A01
TEST(TriageSession, FirstRefreshSyncsAndStartsFourHourCooldown) {
    Rig r;
    const std::int64_t t0 = r.clock->now_wall();
    ASSERT_TRUE(r.session->start().ok());
    auto res = r.session->refresh();
    ASSERT_TRUE(res.ok()) << res.error().message;
    EXPECT_TRUE(res.value().complete);
    EXPECT_EQ(res.value().added, 3u);
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.session_state, SessionState::Active);
    EXPECT_EQ(st.queue_size, 3u);
    EXPECT_FALSE(st.cooldown.eligible);
    ASSERT_TRUE(st.cooldown.next_eligible_wall.has_value());
    EXPECT_EQ(*st.cooldown.next_eligible_wall, t0 + 4 * kHour);
    EXPECT_EQ(*st.cooldown.last_success_wall, t0);
    EXPECT_TRUE(r.store->get_active_session()->initial_sync_at.has_value());
}

// A02
TEST(TriageSession, RefreshAtPlusOneHourIsPolicyErrorAndQueueStaysReadable) {
    Rig r;
    r.ready();
    auto res = r.session->refresh();
    ASSERT_FALSE(res.ok());
    EXPECT_EQ(res.error().kind, ErrorKind::Policy);
    EXPECT_EQ(r.backend->count("delta"), 0);
    EXPECT_EQ(r.session->queue().size(), 3u);
    EXPECT_EQ(r.session->status().session_state, SessionState::Active);
    ASSERT_TRUE(r.session->open("m1").ok()) << "reading is never cooldown-gated";
}

TEST(TriageSession, RefreshEligibleAgainAfterFourHoursAndPicksUpNewMail) {
    Rig r;
    r.ready();
    r.backend->add_message("m4", "Late arrival", 1'799'900'000);
    EXPECT_EQ(r.session->queue().size(), 3u) << "Q10: nothing new until the next eligible refresh";
    r.clock->advance(3 * kHour);
    auto res = r.session->refresh();
    ASSERT_TRUE(res.ok()) << res.error().message;
    EXPECT_EQ(res.value().added, 1u);
    EXPECT_EQ(r.session->queue().size(), 4u);
}

TEST(TriageSession, ForcedRefreshBypassesCooldown) {
    Rig r;
    r.ready();
    EXPECT_TRUE(r.session->refresh(true).ok());
}

// A03 / A04 / A05
TEST(TriageSession, ArchiveDeleteDeferSucceedAtPlusOneHourAndLeaveQueue) {
    Rig r;
    r.ready();
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok()) << a.error().message;
    EXPECT_EQ(a.value().state, CommandState::Confirmed);
    auto d = r.session->remove("m2");
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d.value().state, CommandState::Confirmed);
    auto f = r.session->defer("m3");
    ASSERT_TRUE(f.ok());
    EXPECT_EQ(f.value().state, CommandState::Confirmed);

    EXPECT_TRUE(r.backend->in_archive("m1"));
    EXPECT_FALSE(r.backend->in_inbox("m1"));
    EXPECT_TRUE(r.backend->in_deleted("m2"));
    EXPECT_FALSE(r.backend->in_archive("m2"));
    EXPECT_TRUE(r.backend->in_inbox("m3"));
    EXPECT_EQ(r.backend->flag_of("m3"), FlagStatus::Flagged);

    EXPECT_TRUE(r.session->queue().empty());
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.queue_size, 0u);
    EXPECT_EQ(st.deferred_count, 1u);
    EXPECT_EQ(st.pending_commands, 0u);
    EXPECT_EQ(r.backend->count("delta"), 0) << "outgoing writes never trigger an inbound fetch";
    EXPECT_TRUE(r.session->deferred().empty()) << "Q5: count only by default";
}

TEST(TriageSession, DeferredPanelShowsDeferredWhenConfigured) {
    TriageConfig cfg;
    cfg.show_deferred_panel = true;
    Rig r(3, cfg);
    r.ready();
    ASSERT_TRUE(r.session->defer("m1").ok());
    auto d = r.session->deferred();
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].id, "m1");
}

TEST(TriageSession, HidePreexistingFlaggedConfigFiltersQueue) {
    TriageConfig cfg;
    cfg.hide_preexisting_flagged = true;
    Rig r(3, cfg);
    r.backend->inbox["m2"].flag_status = FlagStatus::Flagged;
    ASSERT_TRUE(r.session->start().ok());
    ASSERT_TRUE(r.session->refresh().ok());
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m1", "m3"}));
}

TEST(TriageSession, PreexistingFlaggedStaysVisibleByDefault) {
    Rig r;
    r.backend->inbox["m2"].flag_status = FlagStatus::Flagged;
    ASSERT_TRUE(r.session->start().ok());
    ASSERT_TRUE(r.session->refresh().ok());
    EXPECT_EQ(r.queue_ids().size(), 3u);
}

TEST(TriageSession, ArchiveIsIdempotentAndRejectsGoneOrUnknown) {
    Rig r;
    r.ready();
    auto a1 = r.session->archive("m1");
    auto a2 = r.session->archive("m1");
    ASSERT_TRUE(a1.ok());
    ASSERT_TRUE(a2.ok());
    EXPECT_EQ(a1.value().id, a2.value().id);
    EXPECT_EQ(r.backend->count("move", "m1"), 1);
    EXPECT_EQ(r.session->archive("nope").error().kind, ErrorKind::Invalid);
}

TEST(TriageSession, UndoArchiveBringsMessageBack) {
    Rig r;
    r.ready();
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    auto u = r.session->undo(a.value().id);
    ASSERT_TRUE(u.ok()) << u.error().message;
    EXPECT_EQ(u.value().type, CommandType::UndoArchive);
    EXPECT_EQ(u.value().state, CommandState::Confirmed);
    EXPECT_TRUE(r.backend->in_inbox("m1"));
    EXPECT_EQ(r.session->queue().size(), 3u);
    EXPECT_FALSE(r.session->undo("unknown").ok());
}

TEST(TriageSession, UndoRejectedForDeferAndUnconfirmed) {
    Rig r;
    r.ready();
    auto f = r.session->defer("m1");
    ASSERT_TRUE(f.ok());
    EXPECT_EQ(r.session->undo(f.value().id).error().kind, ErrorKind::Invalid);

    r.session->set_online(false);
    auto d = r.session->remove("m2");
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d.value().state, CommandState::Pending);
    EXPECT_EQ(r.session->undo(d.value().id).error().kind, ErrorKind::Invalid);
}

TEST(TriageSession, UndeferRestoresQueueAndClearsFlag) {
    Rig r;
    r.ready();
    ASSERT_TRUE(r.session->defer("m1").ok());
    auto u = r.session->undefer("m1");
    ASSERT_TRUE(u.ok()) << u.error().message;
    EXPECT_EQ(u.value().state, CommandState::Confirmed);
    EXPECT_EQ(r.backend->flag_of("m1"), FlagStatus::NotFlagged);
    EXPECT_EQ(r.session->status().deferred_count, 0u);
    EXPECT_EQ(r.session->queue().size(), 3u);
}

// Q5 through the facade
TEST(TriageSession, UnflaggedElsewhereRestoresDeferredAtNextRefreshWithEvent) {
    Rig r;
    r.ready();
    ASSERT_TRUE(r.session->defer("m1").ok());
    r.backend->other_client_unflag("m1");
    r.clock->advance(3 * kHour);
    auto res = r.session->refresh();
    ASSERT_TRUE(res.ok());
    EXPECT_EQ(res.value().restored_to_queue, 1u);
    EXPECT_EQ(r.session->status().deferred_count, 0u);
    bool undeferred = false;
    for (const auto& row : r.session->activity()) {
        if (row.event.type == EventType::Undeferred && row.event.message_id == "m1") undeferred = true;
    }
    EXPECT_TRUE(undeferred);
}

// A08 through the facade. The pending archive is delivered first (refresh flushes before it syncs); a message that
// another client already moved is simply archived, and is never resurrected into the queue.
TEST(TriageSession, MessageMovedElsewhereIsNotResurrected) {
    Rig r;
    r.ready();
    r.session->set_online(false);
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    r.backend->other_client_move("m1", WellKnownFolder::Archive);
    r.session->set_online(true);
    r.clock->advance(4 * kHour);
    ASSERT_TRUE(r.session->refresh().ok());
    auto cmd = r.store->get_command(a.value().id);
    EXPECT_EQ(cmd->state, CommandState::Confirmed);
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m2", "m3"}));
    EXPECT_EQ(r.store->get_message_state("m1"), MessageState::Archived);
    EXPECT_EQ(r.session->status().pending_commands, 0u);
}

TEST(TriageSession, MessageRemovedElsewhereIsAbandonedAndNotResurrected) {
    Rig r;
    r.ready();
    r.session->set_online(false);
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    r.backend->other_client_purge("m1");
    r.session->set_online(true);
    r.clock->advance(4 * kHour);
    ASSERT_TRUE(r.session->refresh().ok());
    auto cmd = r.store->get_command(a.value().id);
    EXPECT_EQ(cmd->state, CommandState::Abandoned);
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m2", "m3"}));
    EXPECT_EQ(r.session->status().pending_commands, 0u);
    r.backend->clear_calls();
    r.session->flush();
    EXPECT_EQ(r.backend->count("move"), 0);
    for (const auto& row : r.session->activity()) {
        if (row.event.message_id == "m1") {
            EXPECT_EQ(row.event.confirmation, ConfirmationState::Failed);
        }
    }
}

// A07
TEST(TriageSession, NetworkFailureMidSyncIsPartialResumableAndConsumesNoCooldown) {
    Rig r;
    r.backend->page_size = 1;
    ASSERT_TRUE(r.session->start().ok());
    r.backend->fail_delta_at_call(2, ErrorKind::Network);
    auto res = r.session->refresh();
    ASSERT_FALSE(res.ok());
    EXPECT_EQ(res.error().kind, ErrorKind::Network);
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.session_state, SessionState::SyncPartial);
    EXPECT_TRUE(st.cooldown.eligible);
    EXPECT_FALSE(st.cooldown.last_success_wall.has_value());
    EXPECT_FALSE(st.last_error.empty());
    EXPECT_EQ(r.session->queue().size(), 1u) << "page 1 is already usable";
    ASSERT_TRUE(r.store->get_sync_state().resume_next_link.has_value());

    auto again = r.session->refresh();
    ASSERT_TRUE(again.ok()) << again.error().message;
    EXPECT_TRUE(again.value().resumed);
    EXPECT_EQ(again.value().pages, 2u);
    EXPECT_EQ(r.session->status().session_state, SessionState::Active);
    EXPECT_EQ(r.session->queue().size(), 3u);
    EXPECT_FALSE(r.session->status().cooldown.eligible);
    EXPECT_TRUE(r.session->status().last_error.empty());
}

TEST(TriageSession, AuthErrorDuringRefreshMovesToAuthRequired) {
    Rig r;
    ASSERT_TRUE(r.session->start().ok());
    r.backend->fail_next(ErrorKind::Auth, std::nullopt, "delta");
    auto res = r.session->refresh();
    ASSERT_FALSE(res.ok());
    EXPECT_EQ(res.error().kind, ErrorKind::Auth);
    EXPECT_EQ(r.session->status().session_state, SessionState::AuthRequired);
    ASSERT_TRUE(r.session->start().ok());  // re-entry after a new sign-in
    EXPECT_EQ(r.session->status().session_state, SessionState::ReadyCached);
    EXPECT_TRUE(r.session->refresh().ok());
}

TEST(TriageSession, AuthErrorDuringFlushKeepsCommandPendingAndRecoversAfterSignIn) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Auth, std::nullopt, "move");
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    EXPECT_EQ(r.session->status().session_state, SessionState::AuthRequired);
    EXPECT_EQ(r.session->archive("m2").error().kind, ErrorKind::Invalid);
    ASSERT_TRUE(r.session->start().ok());  // start() sends the leftover command once
    EXPECT_EQ(r.store->list_commands(CommandState::Pending).size(), 0u);
    ProcessReport pr = r.session->flush();
    EXPECT_EQ(pr.confirmed, 0u);
    EXPECT_TRUE(r.backend->in_archive("m1"));
}

// A09
TEST(TriageSession, OfflineActionsQueueAndConfirmAfterReconnect) {
    Rig r;
    r.ready();
    r.session->set_online(false);
    EXPECT_TRUE(r.session->status().offline);
    EXPECT_EQ(r.session->status().session_state, SessionState::Offline);

    auto a = r.session->archive("m1");
    auto d = r.session->defer("m2");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    EXPECT_EQ(d.value().state, CommandState::Pending);
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.pending_commands, 2u);
    EXPECT_EQ(st.queue_size, 1u);
    EXPECT_EQ(r.backend->count("move") + r.backend->count("flag"), 0);
    EXPECT_EQ(r.session->queue().size(), 1u);  // cached mail stays readable while offline

    r.session->set_online(true);
    EXPECT_FALSE(r.session->status().offline);
    ProcessReport pr = r.session->flush();
    EXPECT_EQ(pr.confirmed, 2u);
    EXPECT_EQ(r.session->status().pending_commands, 0u);
    EXPECT_TRUE(r.backend->in_archive("m1"));
    EXPECT_EQ(r.backend->flag_of("m2"), FlagStatus::Flagged);
    EXPECT_EQ(r.session->status().deferred_count, 1u);
}

TEST(TriageSession, NetworkErrorOnFlushMarksOfflineAndKeepsCommand) {
    Rig r;
    r.ready();
    r.backend->offline = true;
    auto a = r.session->archive("m1");  // believes it is online, discovers it is not
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    StatusSnapshot st = r.session->status();
    EXPECT_TRUE(st.offline);
    EXPECT_EQ(st.session_state, SessionState::Offline);
    EXPECT_EQ(st.pending_commands, 1u);

    // F1: offline is not sticky. Further actions still attempt delivery (and stay Pending while the net is down).
    const int calls = r.backend->count("move");
    ASSERT_TRUE(r.session->archive("m2").ok());
    EXPECT_EQ(r.backend->count("move"), calls + 1) << "one attempt per action, stopping at the first Network error";

    r.backend->offline = false;
    ProcessReport pr = r.session->flush();  // no set_online() needed
    EXPECT_EQ(pr.confirmed, 2u);
    EXPECT_FALSE(r.session->status().offline);
    EXPECT_EQ(r.session->status().session_state, SessionState::Active);
}

// F1: after one network blip the NEXT archive is delivered with no flush(), set_online() or refresh() in between.
TEST(TriageSession, NextActionIsDeliveredAfterOneNetworkBlip) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Network, std::nullopt, "move");
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    EXPECT_TRUE(r.session->status().offline);
    EXPECT_EQ(r.session->status().session_state, SessionState::Offline);

    auto b = r.session->archive("m2");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b.value().state, CommandState::Confirmed);
    EXPECT_TRUE(r.backend->in_archive("m2"));
    EXPECT_TRUE(r.backend->in_archive("m1")) << "the older pending command went out first (FIFO)";
    StatusSnapshot st = r.session->status();
    EXPECT_FALSE(st.offline) << "status.offline clears after a confirmed command";
    EXPECT_EQ(st.session_state, SessionState::Active);
    EXPECT_EQ(st.pending_commands, 0u);
    EXPECT_TRUE(st.last_error.empty());
}

// F1: refresh is allowed from Offline, flushes first, and a success brings the session back to Active.
TEST(TriageSession, RefreshFromOfflineFlushesThenSyncsAndClearsOffline) {
    Rig r;
    r.ready();
    r.backend->offline = true;
    ASSERT_TRUE(r.session->archive("m1").ok());
    ASSERT_EQ(r.session->status().session_state, SessionState::Offline);
    r.backend->add_message("m4", "New mail", 1'799'999'000);

    // Still down: the refresh fails, the session stays Offline, nothing is lost.
    r.clock->advance(4 * kHour);
    auto down = r.session->refresh();
    ASSERT_FALSE(down.ok());
    EXPECT_EQ(down.error().kind, ErrorKind::Network);
    EXPECT_EQ(r.session->status().session_state, SessionState::Offline);
    EXPECT_TRUE(r.session->status().offline);
    EXPECT_EQ(r.session->status().pending_commands, 1u);

    r.backend->offline = false;
    auto up = r.session->refresh();
    ASSERT_TRUE(up.ok()) << up.error().message;
    EXPECT_TRUE(up.value().complete);
    EXPECT_TRUE(r.backend->in_archive("m1")) << "pending commands are flushed before the sync";
    StatusSnapshot st = r.session->status();
    EXPECT_EQ(st.session_state, SessionState::Active);
    EXPECT_FALSE(st.offline);
    EXPECT_EQ(st.pending_commands, 0u);
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m4", "m2", "m3"}));
}

TEST(TriageSession, RefreshFlushesPendingEvenWhenCooldownBlocksTheSync) {
    Rig r;
    r.ready();  // cooldown active: only one hour since the first sync
    r.backend->fail_next(ErrorKind::Network, std::nullopt, "move");
    ASSERT_TRUE(r.session->archive("m1").ok());
    ASSERT_EQ(r.session->status().pending_commands, 1u);
    auto res = r.session->refresh();
    ASSERT_FALSE(res.ok());
    EXPECT_EQ(res.error().kind, ErrorKind::Policy);
    EXPECT_TRUE(r.backend->in_archive("m1"));
    EXPECT_EQ(r.backend->count("delta"), 0);
    EXPECT_EQ(r.session->status().session_state, SessionState::Active);
    EXPECT_FALSE(r.session->status().offline);
}

TEST(TriageSession, FailedSyncWithNetworkErrorMarksOfflineAndASuccessfulFlushClearsIt) {
    Rig r;
    r.ready();
    r.clock->advance(4 * kHour);
    r.backend->fail_next(ErrorKind::Network, std::nullopt, "delta");
    ASSERT_FALSE(r.session->refresh().ok());
    EXPECT_TRUE(r.session->status().offline);
    ASSERT_TRUE(r.session->archive("m1").ok());  // delivered: the net is fine
    EXPECT_FALSE(r.session->status().offline);
    EXPECT_TRUE(r.session->status().last_error.empty());
}

// Documented behaviour: start() makes one delivery attempt for leftover Pending commands and never blocks on
// the network otherwise.
TEST(TriageSession, StartFlushesLeftoverPendingOnceAndIgnoresNetworkErrors) {
    auto store = std::make_unique<MemoryStore>();
    MemoryStore* raw = store.get();
    raw->upsert_message(FakeBackend::make_message("m1"));
    raw->set_message_state("m1", MessageState::DoPending, "");
    Command c;
    c.id = "cmd-1";
    c.session_id = "s";
    c.message_id = "m1";
    c.type = CommandType::Archive;
    c.payload_json = "{}";
    c.state = CommandState::Pending;
    raw->put_command(c);
    Rig r(0, {}, std::move(store));
    r.backend->inbox["m1"] = FakeBackend::make_message("m1");
    r.backend->offline = true;
    ASSERT_TRUE(r.session->start().ok());
    EXPECT_EQ(r.backend->count("move"), 1) << "exactly one attempt";
    EXPECT_EQ(r.store->get_command("cmd-1")->state, CommandState::Pending);
    EXPECT_TRUE(r.session->status().offline);

    r.backend->offline = false;
    r.backend->clear_calls();
    ASSERT_TRUE(r.session->refresh().ok());  // refresh flushes first
    EXPECT_EQ(r.store->get_command("cmd-1")->state, CommandState::Confirmed);
}

TEST(TriageSession, StartWithNothingPendingMakesNoNetworkCallBeyondAccount) {
    Rig r;
    ASSERT_TRUE(r.session->start().ok());
    EXPECT_EQ(r.backend->count("move") + r.backend->count("flag") + r.backend->count("delta") + r.backend->count("body"), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// F2: failed messages are listed, counted and retryable
// ---------------------------------------------------------------------------------------------------------------
TEST(TriageSession, FailedMessageIsListedCountedAndRetryable) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::NotFound, std::nullopt, "move", 1, "ErrorFolderNotFound");
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Failed);
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m2", "m3"})) << "hidden from the queue ...";
    ASSERT_EQ(r.session->failed().size(), 1u) << "... but listed under failed()";
    EXPECT_EQ(r.session->failed()[0].id, "m1");
    EXPECT_EQ(r.session->failure_reason("m1"), "fake backend: not_found");
    ASSERT_TRUE(r.session->failed_command("m1").has_value());
    EXPECT_EQ(r.session->failed_command("m1")->type, CommandType::Archive);
    EXPECT_EQ(r.session->status().failed_count, 1u);
    EXPECT_EQ(r.session->failure_reason("m2"), "");

    auto again = r.session->retry("m1");
    ASSERT_TRUE(again.ok()) << again.error().message;
    EXPECT_EQ(again.value().type, CommandType::Archive);
    EXPECT_EQ(again.value().state, CommandState::Confirmed);
    EXPECT_TRUE(r.backend->in_archive("m1"));
    EXPECT_TRUE(r.session->failed().empty());
    EXPECT_EQ(r.session->status().failed_count, 0u);
}

TEST(TriageSession, RetryKeepsTheFailedActionTypeAndRejectsHealthyMessages) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Malformed, std::nullopt, "move");
    ASSERT_EQ(r.session->remove("m1").value().state, CommandState::Failed);
    EXPECT_EQ(r.session->retry("m2").error().kind, ErrorKind::Invalid);
    auto again = r.session->retry("m1");
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().type, CommandType::Delete);
    EXPECT_TRUE(r.backend->in_deleted("m1"));
}

TEST(TriageSession, RetryWhilePendingBehindNetworkFailureStaysPending) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Malformed, std::nullopt, "move");
    ASSERT_EQ(r.session->archive("m1").value().state, CommandState::Failed);
    r.backend->offline = true;
    auto again = r.session->retry("m1");
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().state, CommandState::Pending);
    EXPECT_EQ(r.session->status().failed_count, 0u) << "a retry in progress is Pending, not Failed";
    EXPECT_EQ(r.session->status().pending_commands, 1u);
}

// F2(c) at the facade: a 5-minute Graph outage hides nothing and fails nothing.
TEST(TriageSession, ServerOutageKeepsActionPendingUntilItClears) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Server, std::nullopt, "move", 4);
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    for (int i = 0; i < 3; ++i) {
        r.clock->advance(30);  // each flush is past the backoff, and hits the outage again
        r.session->flush();
        EXPECT_EQ(r.session->status().failed_count, 0u);
        EXPECT_EQ(r.session->status().pending_commands, 1u);
    }
    EXPECT_EQ(r.backend->count("move"), 4);
    r.clock->advance(30);  // outage over
    EXPECT_EQ(r.session->flush().confirmed, 1u);
    EXPECT_TRUE(r.backend->in_archive("m1"));
}

// F2(d): a Failed row the server still reports in the Inbox comes back at the next sync.
TEST(TriageSession, RefreshUnhidesFailedRowStillInTheInbox) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Malformed, std::nullopt, "move");
    ASSERT_EQ(r.session->archive("m1").value().state, CommandState::Failed);
    ASSERT_EQ(r.session->status().failed_count, 1u);
    r.backend->other_client_update("m1", "Edited elsewhere");  // the server reports it again
    r.clock->advance(4 * kHour);
    ASSERT_TRUE(r.session->refresh().ok());
    EXPECT_EQ(r.store->get_message_state("m1"), MessageState::InQueue);
    EXPECT_EQ(r.session->status().failed_count, 0u);
    EXPECT_EQ(r.queue_ids().size(), 3u);
}

TEST(TriageSession, RefreshDoesNotUnhideFailedRowWithAPendingCommand) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Malformed, std::nullopt, "move");
    ASSERT_EQ(r.session->archive("m1").value().state, CommandState::Failed);
    r.session->set_online(false);  // hint: queue the retry without sending it
    ASSERT_TRUE(r.session->retry("m1").ok());
    r.session->set_online(true);
    r.backend->other_client_update("m1", "Edited elsewhere");
    r.backend->fail_next(ErrorKind::Network, std::nullopt, "move");  // keep the retry from going out in refresh's flush
    r.clock->advance(4 * kHour);
    auto res = r.session->refresh();
    ASSERT_FALSE(res.ok());  // flush hit the network error first
    EXPECT_EQ(r.store->get_message_state("m1"), MessageState::DoPending);
}

TEST(TriageSession, ThrottledActionStaysPendingAndFlushLaterConfirms) {
    Rig r;
    r.ready();
    r.backend->fail_next(ErrorKind::Throttled, 20, "move");
    auto a = r.session->archive("m1");
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().state, CommandState::Pending);
    EXPECT_EQ(r.session->status().pending_commands, 1u);
    EXPECT_EQ(r.session->flush().confirmed, 0u);
    r.clock->advance(21);
    EXPECT_EQ(r.session->flush().confirmed, 1u);
}

TEST(TriageSession, OpenCachesBodyAndDoesNotRefetch) {
    Rig r;
    r.backend->bodies["m1"] = "Hello body";
    r.ready();
    auto b = r.session->open("m1");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b.value().text, "Hello body");
    EXPECT_EQ(r.backend->count("body", "m1"), 1);
    ASSERT_TRUE(r.session->open("m1").ok());
    EXPECT_EQ(r.backend->count("body", "m1"), 1);
    EXPECT_TRUE(r.store->get_body("m1").has_value());
    EXPECT_EQ(r.session->open("zzz").error().kind, ErrorKind::Invalid);
}

TEST(TriageSession, OpenUncachedWhileOfflineFailsButQueueRemainsUsable) {
    Rig r;
    r.ready();
    r.backend->offline = true;
    auto b = r.session->open("m1");
    ASSERT_FALSE(b.ok());
    EXPECT_EQ(b.error().kind, ErrorKind::Network);
    EXPECT_EQ(r.session->queue().size(), 3u);
    ASSERT_TRUE(r.session->message("m1").has_value());
    EXPECT_EQ(r.session->message("m1")->subject, "Subject 1");
}

TEST(TriageSession, ActivityListsSessionEventsWithSnapshots) {
    Rig r;
    r.ready();
    ASSERT_TRUE(r.session->archive("m1").ok());
    ASSERT_TRUE(r.session->remove("m2").ok());
    auto rows = r.session->activity();
    ASSERT_EQ(rows.size(), 2u);
    bool saw_archive = false, saw_delete = false;
    for (const auto& row : rows) {
        EXPECT_EQ(row.event.confirmation, ConfirmationState::Confirmed);
        if (row.event.type == EventType::Archived) {
            saw_archive = true;
            EXPECT_EQ(row.subject, "Subject 1");
            EXPECT_EQ(row.from_name, "Sender m1");
            EXPECT_EQ(row.from_address, "m1@example.test");
            EXPECT_EQ(row.received_at_iso, "2026-10-20T00:00:00Z");
        }
        if (row.event.type == EventType::Deleted) {
            saw_delete = true;
            EXPECT_EQ(row.subject, "Subject 2");
        }
    }
    EXPECT_TRUE(saw_archive && saw_delete);
}

// A13
TEST(TriageSession, EndThenStartCreatesNewSessionAndDeferredSuppressionPersists) {
    Rig r;
    r.ready();
    ASSERT_TRUE(r.session->defer("m1").ok());
    ASSERT_TRUE(r.session->archive("m2").ok());
    const std::string first = r.session->status().session_id;
    ASSERT_EQ(r.session->activity().size(), 2u);

    ASSERT_TRUE(r.session->end().ok());
    EXPECT_EQ(r.session->status().session_state, SessionState::Ended);
    auto ended = r.store->get_session(first);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->state, SessionState::Ended);
    EXPECT_TRUE(ended->ended_at.has_value());
    EXPECT_FALSE(r.store->get_active_session().has_value());

    r.clock->advance(kHour);
    ASSERT_TRUE(r.session->start().ok());
    StatusSnapshot st = r.session->status();
    EXPECT_NE(st.session_id, first);
    EXPECT_EQ(st.session_state, SessionState::ReadyCached);
    EXPECT_TRUE(r.session->activity().empty()) << "activity is scoped to the current session";
    EXPECT_EQ(r.store->list_events(first).size(), 2u);
    EXPECT_EQ(st.deferred_count, 1u);
    EXPECT_EQ(r.store->get_message_state("m1"), MessageState::Deferred);
    EXPECT_EQ(r.queue_ids(), (std::vector<std::string>{"m3"}));
    EXPECT_FALSE(st.cooldown.eligible) << "the inbound clock belongs to the account, not the session";
}

TEST(TriageSession, EndFlushesWhenOnlineAndPurgesTombstones) {
    Rig r;
    r.ready();
    r.backend->other_client_delete("m3");
    r.clock->advance(3 * kHour);
    ASSERT_TRUE(r.session->refresh().ok());
    EXPECT_EQ(r.store->get_message_state("m3"), MessageState::GoneOnServer);
    r.session->set_online(false);
    ASSERT_TRUE(r.session->archive("m1").ok());
    r.session->set_online(true);
    ASSERT_TRUE(r.session->end().ok());
    EXPECT_TRUE(r.backend->in_archive("m1")) << "end() flushes when online";
    EXPECT_FALSE(r.store->get_message_state("m3").has_value()) << "tombstones purged at session end";
}

TEST(TriageSession, PendingCommandsSurviveEndWhenOffline) {
    Rig r;
    r.ready();
    r.session->set_online(false);
    ASSERT_TRUE(r.session->archive("m1").ok());
    ASSERT_TRUE(r.session->end().ok());
    EXPECT_FALSE(r.backend->in_archive("m1"));
    EXPECT_EQ(r.store->list_commands(CommandState::Pending).size(), 1u);

    r.backend->offline = true;  // still down when the next session starts: start() tries once and moves on
    ASSERT_TRUE(r.session->start().ok());
    r.backend->offline = false;
    EXPECT_EQ(r.session->status().pending_commands, 1u);
    EXPECT_EQ(r.session->flush().confirmed, 1u);
    EXPECT_TRUE(r.backend->in_archive("m1"));
}

// A06 through the facade: new process over the same store
TEST(TriageSession, StartRecoversInFlightCommandsAndResumesActiveSession) {
    auto store = std::make_unique<MemoryStore>();
    MemoryStore* raw = store.get();
    MessageSummary m = FakeBackend::make_message("m1", "Subject 1");
    raw->upsert_message(m);
    raw->set_message_state("m1", MessageState::DoPending, "");
    SessionRecord rec;
    rec.id = "old-session";
    rec.started_at = 1'799'999'000;
    rec.state = SessionState::Active;
    raw->put_session(rec);
    Command c;
    c.id = "cmd-1";
    c.session_id = "old-session";
    c.message_id = "m1";
    c.type = CommandType::Archive;
    c.payload_json = "{}";
    c.state = CommandState::InFlight;
    c.attempts = 1;
    c.idempotency_key = "old-session:m1:archive";
    raw->put_command(c);

    Rig r(0, {}, std::move(store));
    r.backend->inbox["m1"] = m;
    r.backend->offline = true;  // keep start()'s single delivery attempt from sending it
    ASSERT_TRUE(r.session->start().ok());
    r.backend->offline = false;
    EXPECT_EQ(r.session->status().session_id, "old-session");
    EXPECT_EQ(r.store->get_command("cmd-1")->state, CommandState::Pending);
    EXPECT_EQ(r.session->status().pending_commands, 1u);
    ProcessReport pr = r.session->flush();
    EXPECT_EQ(pr.confirmed, 1u);
    EXPECT_TRUE(r.backend->in_archive("m1"));
    EXPECT_EQ(r.store->get_command("cmd-1")->state, CommandState::Confirmed);
}

TEST(TriageSession, StartRecoveredCommandForMessageGoneOnServerIsAbandoned) {
    auto store = std::make_unique<MemoryStore>();
    MemoryStore* raw = store.get();
    raw->upsert_message(FakeBackend::make_message("m1"));
    raw->set_message_state("m1", MessageState::DoPending, "");
    Command c;
    c.id = "cmd-1";
    c.session_id = "s";
    c.message_id = "m1";
    c.type = CommandType::Archive;
    c.payload_json = "{}";
    c.state = CommandState::InFlight;
    c.attempts = 1;
    raw->put_command(c);
    Rig r(0, {}, std::move(store));  // server has no m1
    ASSERT_TRUE(r.session->start().ok());  // start() sends the recovered command once: gone on the server
    EXPECT_EQ(r.store->get_command("cmd-1")->state, CommandState::Abandoned);
    EXPECT_EQ(r.store->get_message_state("m1"), MessageState::GoneOnServer);
    EXPECT_EQ(r.session->status().pending_commands, 0u);
}

// A10
TEST(TriageSession, SignOutClearsTokensAndOptionallyWipesCache) {
    {
        Rig r;
        r.ready();
        r.store->put_token_blob("secret");
        ASSERT_TRUE(r.session->sign_out(false).ok());
        EXPECT_FALSE(r.store->get_token_blob().has_value());
        EXPECT_GT(r.store->count_messages(), 0u) << "cache kept per policy";
        EXPECT_EQ(r.session->status().session_state, SessionState::AuthRequired);
        EXPECT_FALSE(r.session->status().signed_in);
    }
    {
        Rig r;
        r.ready();
        r.store->put_token_blob("secret");
        ASSERT_TRUE(r.session->sign_out(true).ok());
        EXPECT_FALSE(r.store->get_token_blob().has_value());
        EXPECT_EQ(r.store->count_messages(), 0u);
        EXPECT_TRUE(r.session->queue().empty());
        EXPECT_FALSE(r.session->archive("m1").ok());
    }
}

// A16 at the facade level
TEST(TriageSession, ClockMovedBackwardNeverLocksTheUserOut) {
    Rig r;
    r.ready();
    r.clock->reboot("boot-2");       // monotonic origin no longer comparable
    r.clock->set_wall(r.clock->now_wall() - 10 * kHour);
    auto res = r.session->refresh();
    EXPECT_TRUE(res.ok()) << (res.ok() ? "" : res.error().message);
}

TEST(TriageSession, RefreshWhileOfflineStateIsAllowedFromTheHint) {
    Rig r;
    r.ready();
    r.session->set_online(false);
    EXPECT_EQ(r.session->status().session_state, SessionState::Offline);
    r.clock->advance(4 * kHour);
    auto res = r.session->refresh();
    EXPECT_TRUE(res.ok()) << (res.ok() ? "" : res.error().message);
    EXPECT_EQ(r.session->status().session_state, SessionState::Active);
    EXPECT_FALSE(r.session->status().offline);
}
