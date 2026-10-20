// Contract tests: the real GraphBackend + SqliteStore + TriageSession against tools/fake_graph/server.py.
// Test names carry the acceptance-matrix ids (handover section 11); "Gate2_" / "Gate3_" tests cover
// Stage 2/3 gate items that have no matrix id.
#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "contract_env.hpp"

namespace slowmail::contract {
namespace {

using namespace std::chrono_literals;
using core::CommandState;
using core::ErrorKind;
using core::MessageState;

// Ids of the hand-readable messages in fixtures/inbox_small.json.
const char* kPlain = "small-01-plain-text";
const char* kHtmlTracker = "small-02-html-script-tracker";
const char* kFlagged = "small-03-flagged-externally";
const char* kPdfDocxPng = "small-07-pdf-docx-png";
const char* kInlineItemRef = "small-08-inline-item-reference";
const char* kExtremes = "small-09-attachment-extremes";
const char* kInlineOnly = "small-11-inline-only";

class Contract : public ContractTest {
protected:
    // Signed-in, started stack with `fixture` loaded and one complete sync done.
    void synced(const std::string& fixture, std::optional<StackOptions> opt = std::nullopt) {
        ctl().load_fixture(fixture);
        ASSERT_NO_FATAL_FAILURE(ready_stack(std::move(opt)));
        auto rep = sync(true);
        ASSERT_TRUE(rep.complete) << rep.error.message;
    }

    // Archive/delete/defer enqueue and flush; verify through the journal, not the returned copy.
    core::Command confirmed_command(const core::Outcome<core::Command>& c) {
        EXPECT_TRUE(c.ok()) << (c.ok() ? "" : c.error().message);
        if (!c.ok()) return {};
        auto stored = stack().store().get_command(c.value().id);
        EXPECT_TRUE(stored.has_value());
        return stored.value_or(c.value());
    }
};

// ------------------------------------------------------------------------------------------------
// Sign-in (A01: first eligible sync starts with an authenticated session)
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A01_DeviceCodePollsUntilApproved) {
    make_stack();
    auto ch = stack().auth().begin();
    ASSERT_TRUE(ch.ok()) << ch.error().message;
    EXPECT_FALSE(ch.value().user_code.empty());
    EXPECT_FALSE(ch.value().device_code.empty());
    EXPECT_NE(ch.value().message.find(ch.value().user_code), std::string::npos);
    EXPECT_GE(ch.value().interval, 1);

    auto p1 = stack().auth().poll(ch.value());
    ASSERT_TRUE(p1.ok());
    EXPECT_EQ(p1.value(), graph::PollResult::Pending);
    EXPECT_FALSE(stack().auth().signed_in());

    ctl().post("approve_device", {{"user_code", ch.value().user_code}});
    std::this_thread::sleep_for(std::chrono::milliseconds(ch.value().interval * 1000 + 200));
    auto p2 = stack().auth().poll(ch.value());
    ASSERT_TRUE(p2.ok());
    EXPECT_EQ(p2.value(), graph::PollResult::Authorized);
    EXPECT_TRUE(stack().auth().signed_in());
    EXPECT_TRUE(stack().backend().account().ok());
    EXPECT_GE(ctl().counter("device_codes_issued"), 1);
}

TEST_F(Contract, A01_DeviceCodeSlowDownDeclinedAndExpired) {
    make_stack();
    auto ch = stack().auth().begin();
    ASSERT_TRUE(ch.ok());
    EXPECT_EQ(stack().auth().poll(ch.value()).value(), graph::PollResult::Pending);
    EXPECT_EQ(stack().auth().poll(ch.value()).value(), graph::PollResult::SlowDown);  // polled faster than interval

    ctl().post("decline_device", {{"user_code", ch.value().user_code}});
    EXPECT_EQ(stack().auth().poll(ch.value()).value(), graph::PollResult::Declined);

    auto ch2 = stack().auth().begin();
    ASSERT_TRUE(ch2.ok());
    ctl().post("expire_device", {{"user_code", ch2.value().user_code}});
    EXPECT_EQ(stack().auth().poll(ch2.value()).value(), graph::PollResult::Expired);
    EXPECT_FALSE(stack().auth().signed_in());
}

// ------------------------------------------------------------------------------------------------
// A01: first sync of the 1000-message dataset, restart, replay/empty-page tolerance
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A01_FirstSyncInbox1000) {
    ctl().load_fixture("inbox_1000.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    const auto before_wall = stack().clock().now_wall();

    auto rep = sync(false);  // first download: eligible without force
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_GT(rep.pages, 1u);
    EXPECT_EQ(rep.added, 1000u);
    EXPECT_EQ(stack().session().queue().size(), 1000u);
    EXPECT_EQ(stack().store().count_messages(), 1000u);
    EXPECT_EQ(queue_ids().size(), 1000u);  // no duplicates
    EXPECT_TRUE(stack().store().get_sync_state().delta_link.has_value());
    EXPECT_FALSE(stack().store().get_sync_state().resume_next_link.has_value());
    EXPECT_EQ(ctl().path_count("GET /v1.0/me/mailFolders/inbox/messages/delta"), static_cast<std::int64_t>(rep.pages));

    // next eligibility = completion + 4 h; A02 guard: the next un-forced refresh is blocked with no Graph fetch
    auto status = stack().session().status();
    ASSERT_TRUE(status.cooldown.next_eligible_wall.has_value());
    EXPECT_NEAR(static_cast<double>(*status.cooldown.next_eligible_wall), static_cast<double>(before_wall + 4 * 3600), 120.0);
    const auto requests = ctl().request_count();
    auto blocked = stack().session().refresh(false);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().kind, ErrorKind::Policy);
    EXPECT_EQ(ctl().request_count(), requests);

    // first request carries $select; links are followed verbatim afterwards
    int with_token = 0;
    for (auto& r : ctl().requests()) {
        const std::string q = r.at("query");
        if (r.at("path").get<std::string>().find("/delta") == std::string::npos) continue;
        if (q.find("$skiptoken=") != std::string::npos) {
            ++with_token;
            EXPECT_EQ(q.find("$select"), std::string::npos) << "client re-added $select to a nextLink";
        }
    }
    EXPECT_GT(with_token, 0);
    expect_prefer_immutable_on_graph_requests(true);
    EXPECT_EQ(ctl().counter("forbidden_fetch"), 0);
}

TEST_F(Contract, A01_RestartKeepsQueueWithoutNetwork) {
    ctl().load_fixture("inbox_1000.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    ASSERT_TRUE(sync(true).complete);
    const auto requests = ctl().request_count();
    stack_.reset();  // app restart

    auto opt = options();
    opt.graph_base = "http://127.0.0.1:1/v1.0";  // unreachable: any network use would fail loudly
    opt.authority = "http://127.0.0.1:1";
    make_stack(opt);
    auto st = stack().session().start();
    ASSERT_TRUE(st.ok()) << st.error().message;
    EXPECT_EQ(stack().session().queue().size(), 1000u);
    EXPECT_TRUE(stack().store().get_sync_state().delta_link.has_value());
    EXPECT_TRUE(stack().auth().signed_in());  // refresh token persisted
    EXPECT_EQ(ctl().request_count(), requests);
}

TEST_F(Contract, A08_DeltaReplayAndTrailingEmptyPageAreTolerated) {
    ctl().post("config", {{"delta_replay", true}, {"trailing_empty_page", true}});
    ctl().load_fixture("inbox_1000.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_EQ(rep.added, 1000u);  // replays are not double counted as new mail
    EXPECT_EQ(stack().session().queue().size(), 1000u);
    EXPECT_EQ(stack().store().count_messages(), 1000u);
}

// ------------------------------------------------------------------------------------------------
// A07: network interruption mid-delta
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A07_NetworkDropOnPage2ThenResumeWithoutConsumingCooldown) {
    ctl().load_fixture("inbox_1000.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    ctl().post("drop", {{"match", "$skiptoken"}, {"count", -1}});  // every page after the first dies

    auto first = sync(false);
    EXPECT_FALSE(first.complete);
    ctl().post("clear_faults");

    auto state = stack().store().get_sync_state();
    EXPECT_FALSE(state.delta_link.has_value());
    EXPECT_FALSE(state.last_complete_wall.has_value()) << "cooldown must not start before a complete sync";
    EXPECT_TRUE(state.resume_next_link.has_value());
    EXPECT_GE(stack().store().count_messages(), 100u);
    EXPECT_LT(stack().store().count_messages(), 1000u);

    auto second = sync(false);  // NOT forced: a failed sync consumed no cooldown
    ASSERT_TRUE(second.complete) << second.error.message;
    EXPECT_TRUE(second.resumed);
    EXPECT_EQ(stack().store().count_messages(), 1000u);
    EXPECT_EQ(stack().session().queue().size(), 1000u);
    EXPECT_EQ(queue_ids().size(), 1000u);
    EXPECT_TRUE(stack().store().get_sync_state().last_complete_wall.has_value());
}

// ------------------------------------------------------------------------------------------------
// A08: another Outlook client changes the mailbox between syncs
// ------------------------------------------------------------------------------------------------
class ExternalChanges : public Contract, public ::testing::WithParamInterface<bool> {};

TEST_P(ExternalChanges, A08_MoveDeleteFlagAddReconcileWithoutDuplicatesOrResurrection) {
    const bool partial_updates = GetParam();
    ctl().post("config", {{"delta_partial_updates", partial_updates}});
    ASSERT_NO_FATAL_FAILURE(synced("inbox_1000.json"));
    auto ids = ctl().folder_ids("inbox");
    ASSERT_EQ(ids.size(), 1000u);

    // pick messages that are not already flagged (a flag change must register as an update)
    std::vector<std::string> moved = {ids[10], ids[20], ids[30]}, deleted = {ids[40], ids[50]};
    std::string flagged;
    for (size_t i = 60; i < ids.size() && flagged.empty(); ++i)
        if (ctl().flag_of(ids[i]) == "notFlagged") flagged = ids[i];
    ASSERT_FALSE(flagged.empty());

    for (auto& id : moved) ctl().post("external", {{"op", "move"}, {"id", id}, {"to", "archive"}});
    for (auto& id : deleted) ctl().post("external", {{"op", "delete"}, {"id", id}});
    ctl().post("external", {{"op", "flag"}, {"id", flagged}});
    auto added = ctl().post("external", {{"op", "add"}, {"count", 4}, {"seed", 77}}).at("ids");
    ASSERT_EQ(added.size(), 4u);

    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_EQ(rep.removed, 5u);
    EXPECT_EQ(rep.added, 4u);
    EXPECT_EQ(rep.updated, 1u);
    EXPECT_FALSE(rep.full_resync);

    auto q = queue_ids();
    EXPECT_EQ(stack().session().queue().size(), 999u);
    EXPECT_EQ(q.size(), 999u);  // no duplicates
    for (auto& id : moved) {
        EXPECT_EQ(q.count(id), 0u);
        EXPECT_EQ(stack().store().get_message_state(id), MessageState::GoneOnServer);
    }
    for (auto& id : deleted) EXPECT_EQ(q.count(id), 0u);
    for (auto& id : added) EXPECT_EQ(q.count(id.get<std::string>()), 1u);
    EXPECT_EQ(q.count(flagged), 1u);
    EXPECT_EQ(stack().session().message(flagged)->flag_status, core::FlagStatus::Flagged);

    // second sync with no changes: nothing resurrects, nothing is added twice
    auto again = sync(true);
    ASSERT_TRUE(again.complete) << again.error.message;
    EXPECT_EQ(again.added + again.removed + again.updated, 0u);
    EXPECT_EQ(queue_ids().size(), 999u);
    for (auto& id : moved) EXPECT_EQ(queue_ids().count(id), 0u);
}
INSTANTIATE_TEST_SUITE_P(PartialAndFullUpdates, ExternalChanges, ::testing::Bool(),
                         [](const auto& info) { return info.param ? "PartialUpdates" : "FullUpdates"; });

TEST_F(Contract, A08_ExpiredDeltaTokenTriggersFullResyncAndDropsMissing) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    ASSERT_EQ(stack().session().queue().size(), 12u);
    ctl().post("external", {{"op", "delete"}, {"id", kPlain}, {"hard", true}});
    ctl().post("external", {{"op", "delete"}, {"id", kFlagged}, {"hard", true}});
    ctl().post("external", {{"op", "add"}, {"count", 1}, {"seed", 3}});
    ctl().post("expire_delta");  // 410 SyncStateNotFound on the stored deltaLink

    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_TRUE(rep.full_resync);
    EXPECT_EQ(stack().session().queue().size(), 11u);
    EXPECT_EQ(queue_ids().count(kPlain), 0u);
    EXPECT_EQ(stack().store().get_message_state(kFlagged), MessageState::GoneOnServer);
}

// ------------------------------------------------------------------------------------------------
// A03-A05: soft actions while the inbound cooldown is active
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A03_ArchiveSucceedsDuringCooldown) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    ASSERT_FALSE(stack().session().refresh(false).ok());  // cooldown is active (A02)

    auto cmd = confirmed_command(stack().session().archive(kPlain));
    EXPECT_EQ(cmd.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPlain), "archive");
    auto inbox = ctl().folder_ids("inbox");
    EXPECT_EQ(std::count(inbox.begin(), inbox.end(), kPlain), 0);
    EXPECT_EQ(queue_ids().count(kPlain), 0u);
    EXPECT_EQ(stack().store().get_message_state(kPlain), MessageState::Archived);

    bool seen = false;
    for (auto& row : stack().session().activity()) {
        if (row.event.message_id == kPlain) {
            seen = true;
            EXPECT_EQ(row.event.type, core::EventType::Archived);
            EXPECT_EQ(row.event.confirmation, core::ConfirmationState::Confirmed);
        }
    }
    EXPECT_TRUE(seen);
    expect_prefer_immutable_on_graph_requests(true);
}

TEST_F(Contract, A04_DeleteMovesToDeletedItemsNeverPermanent) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    auto cmd = confirmed_command(stack().session().remove(kPlain));
    EXPECT_EQ(cmd.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPlain), "deleteditems");  // still exists server-side, in Deleted Items
    auto deleted = ctl().folder_ids("deleteditems");
    EXPECT_EQ(std::count(deleted.begin(), deleted.end(), kPlain), 1);
    EXPECT_EQ(ctl().counter("delete_calls"), 0);  // no DELETE verb ever used
    EXPECT_EQ(stack().store().get_message_state(kPlain), MessageState::Deleted);
    EXPECT_EQ(queue_ids().count(kPlain), 0u);
}

TEST_F(Contract, A05_DeferFlagsOnServerKeepsInboxCopyAndHidesLocally) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    ASSERT_EQ(ctl().flag_of(kPlain), "notFlagged");
    auto cmd = confirmed_command(stack().session().defer(kPlain));
    EXPECT_EQ(cmd.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().flag_of(kPlain), "flagged");
    EXPECT_EQ(ctl().parent_of(kPlain), "inbox");
    EXPECT_EQ(queue_ids().count(kPlain), 0u);
    EXPECT_EQ(stack().store().get_message_state(kPlain), MessageState::Deferred);
    EXPECT_EQ(stack().session().status().deferred_count, 1u);

    // explicit restore (Q5)
    auto un = confirmed_command(stack().session().undefer(kPlain));
    EXPECT_EQ(un.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().flag_of(kPlain), "notFlagged");
    EXPECT_EQ(queue_ids().count(kPlain), 1u);
}

TEST_F(Contract, A05_PreFlaggedMessageStaysVisibleByDefault) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    EXPECT_EQ(ctl().flag_of(kFlagged), "flagged");
    EXPECT_EQ(queue_ids().count(kFlagged), 1u);  // Q6 default: flagged outside Slow Mail is not hidden
}

// ------------------------------------------------------------------------------------------------
// A06: crash with a pending archive; retries must not double-act or report false success
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A06_CrashDuringPendingArchiveRecoversSafely) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    // The server APPLIES the move, then the response is lost on every attempt.
    ctl().post("drop", {{"match", "/move"}, {"after", true}, {"count", -1}});
    auto first = stack().session().archive(kPlain);
    ASSERT_TRUE(first.ok()) << first.error().message;  // enqueue is durable even though delivery failed
    ctl().post("clear_faults");
    auto cmd = stack().store().get_command(first.value().id);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_NE(cmd->state, CommandState::Confirmed) << "no false success when the response never arrived";
    EXPECT_NE(cmd->state, CommandState::Failed);
    EXPECT_EQ(ctl().parent_of(kPlain), "archive");  // server did act
    const auto cmd_id = cmd->id;

    // "crash": drop the whole app without end(), reopen over the same database
    stack_.reset();
    make_stack();
    auto st = stack().session().start();
    ASSERT_TRUE(st.ok()) << st.error().message;
    auto report = stack().session().flush();
    EXPECT_EQ(report.failed, 0u);

    auto after = stack().store().get_command(cmd_id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->state, CommandState::Confirmed);
    EXPECT_EQ(stack().store().list_commands(CommandState::Pending).size(), 0u);
    EXPECT_EQ(stack().store().list_commands(CommandState::InFlight).size(), 0u);
    auto archive = ctl().folder_ids("archive");
    EXPECT_EQ(std::count(archive.begin(), archive.end(), kPlain), 1);  // exactly once
    EXPECT_EQ(ctl().folder_ids("inbox").size(), 11u);
    EXPECT_EQ(stack().store().get_message_state(kPlain), MessageState::Archived);
}

// ------------------------------------------------------------------------------------------------
// A09: offline use
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A09_OfflineCachedReadablePendingActionsVisibleThenFlush) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    auto body = stack().session().open(kPlain);  // caches the body while online
    ASSERT_TRUE(body.ok()) << body.error().message;
    EXPECT_NE(body.value().text.find("plain text message"), std::string::npos);

    stack().session().set_online(false);
    const auto requests = ctl().request_count();

    auto cached = stack().session().open(kPlain);
    ASSERT_TRUE(cached.ok());
    EXPECT_EQ(cached.value().text, body.value().text);
    EXPECT_EQ(stack().session().queue().size(), 12u);

    auto queued = stack().session().archive(kHtmlTracker);
    ASSERT_TRUE(queued.ok()) << queued.error().message;
    auto cmd = stack().store().get_command(queued.value().id);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->state, CommandState::Pending);
    EXPECT_EQ(stack().session().status().pending_commands, 1u);
    EXPECT_EQ(stack().store().get_message_state(kHtmlTracker), MessageState::DoPending);
    EXPECT_EQ(ctl().request_count(), requests) << "offline mode must not touch the network";
    EXPECT_EQ(ctl().parent_of(kHtmlTracker), "inbox");

    stack().session().set_online(true);
    auto report = stack().session().flush();
    EXPECT_EQ(report.confirmed, 1u);
    EXPECT_EQ(ctl().parent_of(kHtmlTracker), "archive");
    EXPECT_EQ(stack().session().status().pending_commands, 0u);
}

// ------------------------------------------------------------------------------------------------
// Stage 2/3 gate items without matrix ids: tokens, throttling, 401
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, Gate2_TokenExpiryRefreshesTransparently) {
    ctl().post("config", {{"token_ttl", 1}});
    ctl().load_fixture("inbox_small.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    std::this_thread::sleep_for(2200ms);  // access token is now expired
    const auto refreshes = ctl().counter("refreshes");

    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_EQ(stack().session().queue().size(), 12u);
    EXPECT_GT(ctl().counter("refreshes"), refreshes);  // silent refresh, no user interaction

    std::this_thread::sleep_for(2200ms);
    auto body = stack().session().open(kPlain);
    EXPECT_TRUE(body.ok()) << (body.ok() ? "" : body.error().message);
    EXPECT_EQ(ctl().counter("device_codes_issued"), 1);
}

TEST_F(Contract, Gate2_Server401InvalidatesTokenAndRefreshesOnce) {
    ctl().load_fixture("inbox_small.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    ctl().post("expire_tokens");  // the server forgets all access tokens; the client still believes its token is valid
    const auto refreshes = ctl().counter("refreshes");
    const auto unauthorised = ctl().counter("graph_401");

    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_EQ(ctl().counter("graph_401") - unauthorised, 1);   // one rejected call...
    EXPECT_EQ(ctl().counter("refreshes") - refreshes, 1);      // ...one refresh, then the retry succeeds
}

TEST_F(Contract, Gate2_RevokedRefreshTokenRequiresInteractiveSignIn) {
    ctl().post("config", {{"token_ttl", 1}});
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    ctl().post("revoke_refresh");
    std::this_thread::sleep_for(2200ms);
    auto page = stack().backend().fetch_inbox_delta("");
    ASSERT_FALSE(page.ok());
    EXPECT_EQ(page.error().kind, ErrorKind::Auth);
}

TEST_F(Contract, Gate2_Throttle429RetryAfterHonoured) {
    ctl().load_fixture("inbox_small.json");
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    auto opt = options();
    opt.graph.max_retry_after_wait_seconds = 5;
    opt.graph.sleep_on_retry = true;
    auto backend = stack().extra_backend(opt.graph);

    ctl().post("clear_log");
    ctl().post("fault", {{"match", "/v1.0/me"}, {"method", "GET"}, {"status", 429}, {"retry_after", 1}, {"count", 1}});
    const auto t0 = std::chrono::steady_clock::now();
    auto acct = backend->account();
    const auto waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(acct.ok()) << acct.error().message;
    EXPECT_GE(waited, 0.9) << "Retry-After: 1 must be waited out";
    EXPECT_EQ(ctl().path_count("GET /v1.0/me"), 2);  // exactly one retry, no hammering

    // Retry-After longer than the cap is returned to the caller, without further requests
    ctl().post("clear_log");
    ctl().post("fault", {{"match", "/v1.0/me"}, {"method", "GET"}, {"status", 429}, {"retry_after", 30}, {"count", 1}});
    auto cfg = opt.graph;
    cfg.max_retry_after_wait_seconds = 1;
    auto strict = stack().extra_backend(cfg);
    auto throttled = strict->account();
    ASSERT_FALSE(throttled.ok());
    EXPECT_EQ(throttled.error().kind, ErrorKind::Throttled);
    ASSERT_TRUE(throttled.error().retry_after_seconds.has_value());
    EXPECT_EQ(*throttled.error().retry_after_seconds, 30);
    EXPECT_EQ(ctl().path_count("GET /v1.0/me"), 1);
}

TEST_F(Contract, Gate2_Throttle429WithoutRetryAfterHeaderBacksOff) {
    ASSERT_NO_FATAL_FAILURE(ready_stack());
    auto cfg = options().graph;
    cfg.sleep_on_retry = true;
    auto backend = stack().extra_backend(cfg);
    ctl().post("clear_log");
    ctl().post("fault", {{"match", "/v1.0/me"}, {"method", "GET"}, {"status", 429}, {"retry_after", nullptr}, {"count", 1}});
    auto acct = backend->account();
    EXPECT_TRUE(acct.ok() || acct.error().kind == ErrorKind::Throttled);  // exponential backoff, never a hang or crash
    EXPECT_LE(ctl().path_count("GET /v1.0/me"), cfg.max_retries_on_throttle + 1);
}

TEST_F(Contract, A03_ThrottledArchiveStaysPendingThenConfirmsExactlyOnce) {
    auto opt = options();
    opt.graph.max_retry_after_wait_seconds = 1;  // do not sleep 30 s in a test
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json", opt));
    ctl().post("fault", {{"match", "/move"}, {"status", 429}, {"retry_after", 2}, {"count", 1}});

    auto queued = stack().session().archive(kPlain);
    ASSERT_TRUE(queued.ok()) << queued.error().message;
    auto cmd = stack().store().get_command(queued.value().id);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->state, CommandState::Pending);
    EXPECT_EQ(ctl().parent_of(kPlain), "inbox");
    EXPECT_EQ(ctl().path_count("POST /v1.0/me/messages/{id}/move"), 1);
    EXPECT_EQ(stack().session().flush().confirmed, 0u);  // inside the Retry-After window nothing is hammered
    EXPECT_EQ(ctl().path_count("POST /v1.0/me/messages/{id}/move"), 1);

    std::this_thread::sleep_for(2200ms);
    auto report = stack().session().flush();
    EXPECT_EQ(report.confirmed, 1u);
    EXPECT_EQ(ctl().parent_of(kPlain), "archive");
    EXPECT_EQ(ctl().path_count("POST /v1.0/me/messages/{id}/move"), 2);  // throttled attempt + the retry
}

// ------------------------------------------------------------------------------------------------
// Non-immutable ids: move returns a new id, the old one 404s, and undo still works
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A03_NonImmutableMoveArchiveAndUndoStillWork) {
    auto opt = options();
    opt.graph.prefer_immutable_ids = false;
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json", opt));
    expect_prefer_immutable_on_graph_requests(false);

    // The client only knows mutable ids now; map the plain-text message by subject.
    std::string client_id;
    for (auto& m : stack().session().queue())
        if (m.subject == "Plain text hello") client_id = m.id;
    ASSERT_FALSE(client_id.empty());
    EXPECT_EQ(client_id, ctl().mutable_id(kPlain));
    EXPECT_NE(client_id, kPlain);

    auto cmd = confirmed_command(stack().session().archive(client_id));
    ASSERT_EQ(cmd.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPlain), "archive");
    EXPECT_NE(ctl().mutable_id(kPlain), client_id);  // the id changed on the server

    auto undone = stack().session().undo(cmd.id);
    ASSERT_TRUE(undone.ok()) << undone.error().message;
    auto undo_cmd = stack().store().get_command(undone.value().id);
    ASSERT_TRUE(undo_cmd.has_value());
    EXPECT_EQ(undo_cmd->state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPlain), "inbox");

    // reconcile: the message shows up exactly once under its newest id
    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    int count = 0;
    for (auto& m : stack().session().queue())
        if (m.subject == "Plain text hello") ++count;
    EXPECT_EQ(count, 1);
    EXPECT_EQ(stack().session().queue().size(), 12u);
    expect_prefer_immutable_on_graph_requests(false);
}

TEST_F(Contract, A03_MoveToMissingArchiveFolderFailsCleanlyMessageStaysInInbox) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    ctl().post("folders", {{"archive", false}});

    auto queued = stack().session().archive(kPlain);
    ASSERT_TRUE(queued.ok()) << queued.error().message;
    auto cmd = stack().store().get_command(queued.value().id);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_NE(cmd->state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPlain), "inbox");
    // ErrorFolderNotFound is NOT "the message is gone": the message must not be tombstoned or hidden as archived
    auto state = stack().store().get_message_state(kPlain);
    ASSERT_TRUE(state.has_value());
    EXPECT_NE(*state, MessageState::GoneOnServer);
    EXPECT_NE(*state, MessageState::Archived);
    EXPECT_TRUE(*state == MessageState::Failed || *state == MessageState::InQueue || *state == MessageState::DoPending);
    EXPECT_FALSE(cmd->last_error.empty() && cmd->state == CommandState::Failed);
    EXPECT_EQ(ctl().folder_ids("archive").size(), 0u);
}

// ------------------------------------------------------------------------------------------------
// A17-A19: attachments are metadata only
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, A17_PdfDocxPngListedWithoutFetchingBytesAndActionsStillWork) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    auto body = stack().session().open(kPdfDocxPng);
    ASSERT_TRUE(body.ok()) << body.error().message;
    ASSERT_EQ(body.value().attachments.size(), 3u);
    const auto& a = body.value().attachments;
    EXPECT_EQ(a[0].name, "report.pdf");
    EXPECT_EQ(a[0].content_type, "application/pdf");
    EXPECT_EQ(a[0].size, 183421);
    EXPECT_EQ(a[1].name, "minutes.docx");
    EXPECT_EQ(a[1].content_type, "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    EXPECT_EQ(a[1].size, 52311);
    EXPECT_EQ(a[2].name, "photo.png");
    EXPECT_EQ(a[2].content_type, "image/png");
    EXPECT_EQ(a[2].size, 2048576);
    for (auto& att : a) {
        EXPECT_EQ(att.kind, core::AttachmentKind::File);
        EXPECT_FALSE(att.is_inline);
    }
    EXPECT_NE(body.value().text.find("See attached"), std::string::npos);

    // never fetched bytes: no $value, no contentBytes served, and the list request used $select
    EXPECT_EQ(ctl().counter("forbidden_fetch"), 0);
    EXPECT_EQ(ctl().counter("content_bytes_served"), 0);
    int list_calls = 0;
    for (auto& r : ctl().requests()) {
        const std::string path = r.at("path"), query = r.at("query");
        EXPECT_EQ(path.find("$value"), std::string::npos);
        if (path.size() >= 12 && path.compare(path.size() - 12, 12, "/attachments") == 0) {
            ++list_calls;
            EXPECT_NE(query.find("$select="), std::string::npos);
            EXPECT_EQ(query.find("contentBytes"), std::string::npos);
        }
    }
    EXPECT_EQ(list_calls, 1);

    // reading and the DDD actions keep working
    auto cmd = confirmed_command(stack().session().archive(kPdfDocxPng));
    EXPECT_EQ(cmd.state, CommandState::Confirmed);
    EXPECT_EQ(ctl().parent_of(kPdfDocxPng), "archive");
}

TEST_F(Contract, A18_InlineItemAndReferenceAttachmentsAreGracefulMetadata) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    auto body = stack().session().open(kInlineItemRef);
    ASSERT_TRUE(body.ok()) << body.error().message;
    ASSERT_EQ(body.value().attachments.size(), 3u);
    const auto& a = body.value().attachments;
    EXPECT_EQ(a[0].kind, core::AttachmentKind::File);
    EXPECT_TRUE(a[0].is_inline);
    EXPECT_EQ(a[1].kind, core::AttachmentKind::Item);
    EXPECT_EQ(a[2].kind, core::AttachmentKind::Reference);
    // contentType is null on the wire for item/reference attachments; the adapter may default it
    EXPECT_TRUE(a[1].content_type.empty() || a[1].content_type == "application/octet-stream");
    EXPECT_EQ(a[1].name, "Re: budget review");
    EXPECT_EQ(a[2].name, "plan.docx");

    // inline-only message: hasAttachments=false per Graph; opening it must still be safe
    auto summary = stack().session().message(kInlineOnly);
    ASSERT_TRUE(summary.has_value());
    EXPECT_FALSE(summary->has_attachments);
    auto inline_only = stack().session().open(kInlineOnly);
    EXPECT_TRUE(inline_only.ok());

    // tracking / script content never reaches the reading text
    auto html = stack().session().open(kHtmlTracker);
    ASSERT_TRUE(html.ok()) << html.error().message;
    EXPECT_NE(html.value().text.find("Big news"), std::string::npos);
    EXPECT_EQ(html.value().text.find("<script"), std::string::npos);
    EXPECT_EQ(html.value().text.find("alert("), std::string::npos);
    EXPECT_EQ(html.value().text.find("track.example.test"), std::string::npos);

    EXPECT_EQ(ctl().counter("forbidden_fetch"), 0);
    EXPECT_EQ(ctl().counter("content_bytes_served"), 0);
}

TEST_F(Contract, A19_HugeAttachmentMetadataIsBoundedAndSanitised) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    auto body = stack().session().open(kExtremes);
    ASSERT_TRUE(body.ok()) << body.error().message;
    ASSERT_EQ(body.value().attachments.size(), 2u);
    for (auto& att : body.value().attachments) EXPECT_LE(att.name.size(), core::Limits::kMaxAttachmentName + 3);
    EXPECT_EQ(body.value().attachments[1].size, -1);

    // external message: 3000-char subject and 300 attachments
    json atts = json::array();
    for (int i = 0; i < 300; ++i)
        atts.push_back({{"@odata.type", "#microsoft.graph.fileAttachment"}, {"id", "bulk-att-" + std::to_string(i)},
                        {"name", "file-" + std::to_string(i) + ".pdf"}, {"contentType", "application/pdf"},
                        {"size", 1000 + i}, {"isInline", false}});
    json msg = {{"subject", std::string(3000, 'S')},
                {"from", {{"emailAddress", {{"name", "Bulk"}, {"address", "bulk@example.test"}}}}},
                {"body", {{"contentType", "text"}, {"content", "many attachments"}}},
                {"attachments", atts}};
    auto added = ctl().post("external", {{"op", "add"}, {"message", msg}}).at("ids");
    auto rep = sync(true);
    ASSERT_TRUE(rep.complete) << rep.error.message;
    EXPECT_EQ(rep.added, 1u);

    const std::string id = added.at(0);
    auto summary = stack().session().message(id);
    ASSERT_TRUE(summary.has_value());
    EXPECT_LE(summary->subject.size(), core::Limits::kMaxSubject + 3);
    auto big = stack().session().open(id);
    ASSERT_TRUE(big.ok()) << big.error().message;
    EXPECT_LE(big.value().attachments.size(), core::Limits::kMaxAttachmentsListed);
    EXPECT_GT(big.value().attachments.size(), 0u);
    EXPECT_EQ(ctl().counter("forbidden_fetch"), 0);
    EXPECT_EQ(ctl().counter("content_bytes_served"), 0);
}

// ------------------------------------------------------------------------------------------------
// Request-shape invariants over a mixed session (handover checklist 11.9)
// ------------------------------------------------------------------------------------------------
TEST_F(Contract, Gate2_RequestShapeInvariants) {
    ASSERT_NO_FATAL_FAILURE(synced("inbox_small.json"));
    ASSERT_TRUE(stack().session().open(kPdfDocxPng).ok());
    EXPECT_EQ(confirmed_command(stack().session().archive(kPlain)).state, CommandState::Confirmed);
    EXPECT_EQ(confirmed_command(stack().session().defer(kHtmlTracker)).state, CommandState::Confirmed);
    EXPECT_EQ(confirmed_command(stack().session().remove(kFlagged)).state, CommandState::Confirmed);

    expect_prefer_immutable_on_graph_requests(true);
    for (auto& r : ctl().requests()) {
        const std::string path = r.at("path");
        if (path.rfind("/v1.0/", 0) != 0) continue;
        EXPECT_EQ(path.find("$value"), std::string::npos);
        EXPECT_EQ(r.at("headers").at("Authorization").get<std::string>(), "Bearer <redacted>");
        EXPECT_NE(r.at("status").get<int>(), 401);
    }
    EXPECT_EQ(ctl().counter("forbidden_fetch"), 0);
    EXPECT_EQ(ctl().counter("content_bytes_served"), 0);
    EXPECT_EQ(ctl().counter("delete_calls"), 0);
}

}  // namespace
}  // namespace slowmail::contract
