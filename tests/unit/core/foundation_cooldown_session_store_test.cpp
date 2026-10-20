// Foundation tests: cooldown, session state machine, MemoryStore, Transaction.
#include <gtest/gtest.h>

#include <set>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/cooldown.hpp"
#include "slowmail/core/memory_store.hpp"
#include "slowmail/core/session.hpp"

using namespace slowmail::core;

// ---------------------------------------------------------------------------
// Cooldown (handover Stage 4 gate, A01/A02/A07/A16)
// ---------------------------------------------------------------------------
namespace {
constexpr std::int64_t kHour = 3600;

std::int64_t at(const char* iso) {
    std::int64_t t = 0;
    EXPECT_TRUE(parse_iso8601_utc(iso, t)) << iso;
    return t;
}
} // namespace

TEST(Cooldown, FirstDownloadIsEligible) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    const auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "first_download");
    EXPECT_FALSE(d.next_eligible_wall.has_value());
    EXPECT_FALSE(d.last_success_wall.has_value());
}

TEST(Cooldown, StageFourGate_0200_0300_0600) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    ASSERT_TRUE(policy.evaluate(st, clock).eligible);  // 02:00 permitted
    CooldownPolicy::record_success(st, clock);
    EXPECT_EQ(*st.last_complete_wall, at("2026-10-20T02:00:00Z"));
    EXPECT_EQ(*st.last_complete_boot_id, "boot-1");

    clock.advance(1 * kHour);  // 03:00
    auto d = policy.evaluate(st, clock);
    EXPECT_FALSE(d.eligible);
    EXPECT_EQ(d.reason, "cooldown_active");
    ASSERT_TRUE(d.next_eligible_wall.has_value());
    EXPECT_EQ(*d.next_eligible_wall, at("2026-10-20T06:00:00Z"));
    EXPECT_EQ(*d.last_success_wall, at("2026-10-20T02:00:00Z"));

    clock.advance(3 * kHour - 1);  // 05:59:59
    EXPECT_FALSE(policy.evaluate(st, clock).eligible);

    clock.advance(1);  // 06:00:00
    d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "elapsed");
    EXPECT_FALSE(d.next_eligible_wall.has_value());
}

TEST(Cooldown, FailedSyncDoesNotStartTimer) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    // A failed/partial sync touches last_attempt_wall (and maybe resume link) but never record_success.
    st.last_attempt_wall = clock.now_wall();
    st.resume_next_link = "https://example/next";
    clock.advance(10);
    auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "first_download");
    clock.advance(3600);
    EXPECT_TRUE(policy.evaluate(st, clock).eligible);
}

TEST(Cooldown, FailedSyncAfterSuccessKeepsOriginalTimer) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.advance(4 * kHour);  // 06:00 eligible
    ASSERT_TRUE(policy.evaluate(st, clock).eligible);
    st.last_attempt_wall = clock.now_wall();  // failed attempt
    clock.advance(60);
    EXPECT_TRUE(policy.evaluate(st, clock).eligible) << "failure must not restart the timer";
    CooldownPolicy::record_success(st, clock);  // success at 06:01
    EXPECT_FALSE(policy.evaluate(st, clock).eligible);
    EXPECT_EQ(*policy.evaluate(st, clock).next_eligible_wall, clock.now_wall() + 4 * kHour);
}

TEST(Cooldown, WallClockMovedBackwardsOnSameBootUsesMono) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.advance(3600);                          // mono +1h
    clock.set_wall(at("2026-10-19T00:00:00Z"));   // user rewinds clock by 26h
    const auto d = policy.evaluate(st, clock);
    EXPECT_FALSE(d.eligible) << "monotonic clock says only 1h elapsed";
    EXPECT_EQ(d.reason, "cooldown_active");
}

TEST(Cooldown, WallClockMovedBackwardsAcrossRebootIsEligibleAnomaly) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.reboot("boot-2", 5.0);
    clock.set_wall(at("2026-10-19T00:00:00Z"));
    const auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "clock_anomaly");
}

TEST(Cooldown, WallForwardFourHoursAcrossRebootIsEligible) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.reboot("boot-2", 5.0);
    clock.set_wall(at("2026-10-20T06:00:00Z"));
    const auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "elapsed");
}

TEST(Cooldown, WallForwardLessThanFourHoursAcrossRebootBlocked) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.reboot("boot-2", 5.0);
    clock.set_wall(at("2026-10-20T05:59:00Z"));
    const auto d = policy.evaluate(st, clock);
    EXPECT_FALSE(d.eligible);
    EXPECT_EQ(*d.next_eligible_wall, at("2026-10-20T06:00:00Z"));
}

TEST(Cooldown, MonoStillUsedWithinBootWhenWallJumpsForward) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.advance(1 * kHour);                      // mono +1h
    clock.set_wall(clock.now_wall() + 4 * kHour);  // user jumps wall clock: total wall +5h
    const auto d = policy.evaluate(st, clock);
    EXPECT_FALSE(d.eligible) << "set_wall +5h but mono +1h must NOT be eligible";
    EXPECT_EQ(d.reason, "cooldown_active");
    // For display, next_eligible is still derived from the stored wall time.
    EXPECT_EQ(*d.next_eligible_wall, at("2026-10-20T06:00:00Z"));
    clock.advance(3 * kHour);  // mono +4h total
    EXPECT_TRUE(policy.evaluate(st, clock).eligible);
}

TEST(Cooldown, MissingMonoFallsBackToWall) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownPolicy policy;
    InboxSyncState st;
    st.last_complete_wall = at("2026-10-20T02:00:00Z");  // legacy row without mono/boot
    clock.set_wall(at("2026-10-20T03:00:00Z"));
    EXPECT_FALSE(policy.evaluate(st, clock).eligible);
    clock.set_wall(at("2026-10-20T06:00:00Z"));
    EXPECT_TRUE(policy.evaluate(st, clock).eligible);
    clock.set_wall(at("2026-10-20T01:00:00Z"));
    const auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "clock_anomaly");
}

TEST(Cooldown, ProIsAlwaysEligible) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownConfig cfg;
    cfg.pro_entitled = true;
    CooldownPolicy policy(cfg);
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    clock.advance(60);
    auto d = policy.evaluate(st, clock);
    EXPECT_TRUE(d.eligible);
    EXPECT_EQ(d.reason, "pro");
    EXPECT_EQ(*d.last_success_wall, at("2026-10-20T02:00:00Z"));
    policy.set_pro_entitled(false);
    EXPECT_FALSE(policy.evaluate(st, clock).eligible);
}

TEST(Cooldown, CustomCooldownAndFirstDownloadBeatsPro) {
    FakeClock clock(at("2026-10-20T02:00:00Z"));
    CooldownConfig cfg;
    cfg.cooldown_seconds = 600;
    cfg.pro_entitled = true;
    CooldownPolicy policy(cfg);
    InboxSyncState st;
    EXPECT_EQ(policy.evaluate(st, clock).reason, "first_download");
    policy.set_pro_entitled(false);
    CooldownPolicy::record_success(st, clock);
    clock.advance(599);
    EXPECT_FALSE(policy.evaluate(st, clock).eligible);
    clock.advance(1);
    EXPECT_TRUE(policy.evaluate(st, clock).eligible);
}

TEST(Cooldown, RecordSuccessFillsAllFields) {
    FakeClock clock(at("2026-10-20T02:00:00Z"), 777.5, "bootX");
    InboxSyncState st;
    CooldownPolicy::record_success(st, clock);
    EXPECT_EQ(*st.last_complete_wall, clock.now_wall());
    EXPECT_DOUBLE_EQ(*st.last_complete_mono, 777.5);
    EXPECT_EQ(*st.last_complete_boot_id, "bootX");
    EXPECT_EQ(*st.last_attempt_wall, clock.now_wall());
}

// ---------------------------------------------------------------------------
// Session state machine
// ---------------------------------------------------------------------------
namespace {
struct T {
    SessionState from;
    SessionEvent_ ev;
    SessionState to;
};
const T kLegal[] = {
    {SessionState::NotStarted, SessionEvent_::Start, SessionState::ReadyCached},
    {SessionState::ReadyCached, SessionEvent_::AuthNeeded, SessionState::AuthRequired},
    {SessionState::AuthRequired, SessionEvent_::AuthOk, SessionState::ReadyCached},
    {SessionState::ReadyCached, SessionEvent_::CooldownElapsed, SessionState::SyncEligible},
    {SessionState::ReadyCached, SessionEvent_::CooldownActive, SessionState::Active},
    {SessionState::SyncEligible, SessionEvent_::SyncStarted, SessionState::Syncing},
    {SessionState::SyncEligible, SessionEvent_::CooldownActive, SessionState::Active},
    {SessionState::Syncing, SessionEvent_::SyncCompleted, SessionState::Active},
    {SessionState::Syncing, SessionEvent_::SyncInterrupted, SessionState::SyncPartial},
    {SessionState::SyncPartial, SessionEvent_::SyncStarted, SessionState::Syncing},
    {SessionState::SyncPartial, SessionEvent_::CooldownActive, SessionState::Active},
    {SessionState::Active, SessionEvent_::NetworkLost, SessionState::Offline},
    {SessionState::Offline, SessionEvent_::NetworkBack, SessionState::Active},
    {SessionState::Active, SessionEvent_::AuthNeeded, SessionState::AuthRequired},
    {SessionState::Offline, SessionEvent_::AuthNeeded, SessionState::AuthRequired},
    {SessionState::SyncPartial, SessionEvent_::AuthNeeded, SessionState::AuthRequired},
    {SessionState::Active, SessionEvent_::End, SessionState::Ending},
    {SessionState::Offline, SessionEvent_::End, SessionState::Ending},
    {SessionState::SyncPartial, SessionEvent_::End, SessionState::Ending},
    {SessionState::ReadyCached, SessionEvent_::End, SessionState::Ending},
    {SessionState::Ending, SessionEvent_::Ended, SessionState::Ended},
    {SessionState::Active, SessionEvent_::CooldownElapsed, SessionState::SyncEligible},
};
const SessionState kAllStates[] = {SessionState::NotStarted, SessionState::AuthRequired, SessionState::ReadyCached,
                                   SessionState::SyncEligible, SessionState::Syncing, SessionState::SyncPartial,
                                   SessionState::Active, SessionState::Offline, SessionState::Ending, SessionState::Ended};
const SessionEvent_ kAllEvents[] = {SessionEvent_::Start, SessionEvent_::AuthNeeded, SessionEvent_::AuthOk,
                                    SessionEvent_::CooldownElapsed, SessionEvent_::CooldownActive,
                                    SessionEvent_::SyncStarted, SessionEvent_::SyncCompleted,
                                    SessionEvent_::SyncInterrupted, SessionEvent_::NetworkLost,
                                    SessionEvent_::NetworkBack, SessionEvent_::End, SessionEvent_::Ended};
} // namespace

TEST(SessionMachine, EveryLegalTransition) {
    for (const auto& t : kLegal) {
        SessionState to = SessionState::NotStarted;
        ASSERT_TRUE(SessionStateMachine::is_legal(t.from, t.ev, to)) << to_string(t.from) << " " << to_string(t.ev);
        EXPECT_EQ(to, t.to) << to_string(t.from) << " " << to_string(t.ev);
        SessionStateMachine m(t.from);
        EXPECT_TRUE(m.apply(t.ev));
        EXPECT_EQ(m.state(), t.to);
    }
}

TEST(SessionMachine, EverythingElseIsIllegalAndLeavesStateUnchanged) {
    std::size_t illegal = 0;
    for (SessionState s : kAllStates) {
        for (SessionEvent_ e : kAllEvents) {
            bool listed = false;
            for (const auto& t : kLegal) listed |= (t.from == s && t.ev == e);
            SessionState to = SessionState::Ending;
            SessionStateMachine m(s);
            if (!listed) {
                EXPECT_FALSE(SessionStateMachine::is_legal(s, e, to)) << to_string(s) << " " << to_string(e);
                EXPECT_FALSE(m.apply(e));
                EXPECT_EQ(m.state(), s);
                ++illegal;
            }
        }
    }
    EXPECT_EQ(illegal, 10u * 12u - sizeof(kLegal) / sizeof(kLegal[0]));
}

TEST(SessionMachine, SpecificIllegalExamples) {
    SessionStateMachine m;
    EXPECT_EQ(m.state(), SessionState::NotStarted);
    EXPECT_FALSE(m.apply(SessionEvent_::SyncStarted));
    EXPECT_FALSE(m.apply(SessionEvent_::End));
    EXPECT_FALSE(m.apply(SessionEvent_::Ended));
    EXPECT_EQ(m.state(), SessionState::NotStarted);
    SessionStateMachine ended(SessionState::Ended);
    for (SessionEvent_ e : kAllEvents) EXPECT_FALSE(ended.apply(e));
    SessionStateMachine syncing(SessionState::Syncing);
    EXPECT_FALSE(syncing.apply(SessionEvent_::End)) << "must finish or interrupt a sync before ending";
    EXPECT_FALSE(syncing.apply(SessionEvent_::SyncStarted));
}

TEST(SessionMachine, HappyPathFirstDownload) {
    SessionStateMachine m;
    for (auto e : {SessionEvent_::Start, SessionEvent_::CooldownElapsed, SessionEvent_::SyncStarted,
                   SessionEvent_::SyncCompleted, SessionEvent_::End, SessionEvent_::Ended}) {
        ASSERT_TRUE(m.apply(e)) << to_string(e);
    }
    EXPECT_EQ(m.state(), SessionState::Ended);
}

TEST(SessionMachine, InterruptedSyncResumesWithoutLosingUserWork) {
    SessionStateMachine m;
    for (auto e : {SessionEvent_::Start, SessionEvent_::CooldownElapsed, SessionEvent_::SyncStarted,
                   SessionEvent_::SyncInterrupted})
        ASSERT_TRUE(m.apply(e));
    EXPECT_EQ(m.state(), SessionState::SyncPartial);
    EXPECT_TRUE(SessionStateMachine::can_process_mail(m.state()));
    EXPECT_TRUE(m.apply(SessionEvent_::SyncStarted));
    EXPECT_EQ(m.state(), SessionState::Syncing);
}

TEST(SessionMachine, CanProcessMailAndCanAttemptSync) {
    const std::set<SessionState> process = {SessionState::ReadyCached, SessionState::SyncEligible, SessionState::Syncing,
                                            SessionState::SyncPartial, SessionState::Active, SessionState::Offline};
    const std::set<SessionState> sync = {SessionState::ReadyCached, SessionState::SyncEligible,
                                         SessionState::SyncPartial, SessionState::Active};
    for (SessionState s : kAllStates) {
        EXPECT_EQ(SessionStateMachine::can_process_mail(s), process.count(s) == 1) << to_string(s);
        EXPECT_EQ(SessionStateMachine::can_attempt_sync(s), sync.count(s) == 1) << to_string(s);
    }
}

TEST(SessionMachine, EventNames) {
    std::set<std::string> names;
    for (SessionEvent_ e : kAllEvents) names.insert(to_string(e));
    EXPECT_EQ(names.size(), 12u);
}

TEST(NewId, FormatAndUniqueness) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string id = new_id("cmd");
        ASSERT_EQ(id.size(), 3u + 1u + 16u) << id;
        EXPECT_EQ(id.substr(0, 4), "cmd-");
        for (std::size_t k = 4; k < id.size(); ++k) {
            const char c = id[k];
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << id;
        }
        seen.insert(id);
    }
    EXPECT_EQ(seen.size(), 1000u);
    EXPECT_EQ(new_id("").substr(0, 1), "-");
}

// ---------------------------------------------------------------------------
// MemoryStore
// ---------------------------------------------------------------------------
namespace {
MessageSummary msg(const std::string& id, std::int64_t epoch, const std::string& subject = "s") {
    MessageSummary m;
    m.id = id;
    m.subject = subject;
    m.received_at_epoch = epoch;
    return m;
}
} // namespace

TEST(MemoryStore, SettingsAccountSyncStateToken) {
    MemoryStore s;
    ASSERT_TRUE(s.open().ok());
    EXPECT_FALSE(s.get_setting("k").has_value());
    ASSERT_TRUE(s.set_setting("k", "v").ok());
    EXPECT_EQ(*s.get_setting("k"), "v");
    ASSERT_TRUE(s.set_setting("k", "v2").ok());
    EXPECT_EQ(*s.get_setting("k"), "v2");

    EXPECT_FALSE(s.get_account().has_value());
    AccountInfo a;
    a.tenant_id = "t";
    a.signed_in = true;
    ASSERT_TRUE(s.put_account(a).ok());
    EXPECT_EQ(s.get_account()->tenant_id, "t");

    InboxSyncState st = s.get_sync_state();
    EXPECT_FALSE(st.delta_link.has_value());
    st.delta_link = "dl";
    st.last_complete_wall = 5;
    ASSERT_TRUE(s.put_sync_state(st).ok());
    EXPECT_EQ(*s.get_sync_state().delta_link, "dl");
    EXPECT_EQ(*s.get_sync_state().last_complete_wall, 5);

    EXPECT_FALSE(s.get_token_blob().has_value());
    ASSERT_TRUE(s.put_token_blob("blob").ok());
    EXPECT_EQ(*s.get_token_blob(), "blob");
    ASSERT_TRUE(s.clear_token_blob().ok());
    EXPECT_FALSE(s.get_token_blob().has_value());
}

TEST(MemoryStore, MessageCrudAndOrdering) {
    MemoryStore s;
    ASSERT_TRUE(s.upsert_message(msg("b", 100)).ok());
    ASSERT_TRUE(s.upsert_message(msg("a", 100)).ok());
    ASSERT_TRUE(s.upsert_message(msg("c", 300)).ok());
    ASSERT_TRUE(s.upsert_message(msg("d", 200)).ok());
    EXPECT_EQ(s.count_messages(), 4u);
    const auto q = s.list_queue();
    ASSERT_EQ(q.size(), 4u);
    EXPECT_EQ(q[0].id, "c");  // newest first
    EXPECT_EQ(q[1].id, "d");
    EXPECT_EQ(q[2].id, "a");  // tie broken by id
    EXPECT_EQ(q[3].id, "b");

    ASSERT_TRUE(s.set_message_state("d", MessageState::Archived, "").ok());
    ASSERT_TRUE(s.set_message_state("a", MessageState::Archived, "").ok());
    ASSERT_TRUE(s.set_message_state("c", MessageState::Deferred, "deferred_by_slowmail").ok());
    EXPECT_EQ(s.list_queue().size(), 1u);
    const auto arch = s.list_messages_in_state(MessageState::Archived);
    ASSERT_EQ(arch.size(), 2u);
    EXPECT_EQ(arch[0].id, "d");
    EXPECT_EQ(arch[1].id, "a");
    EXPECT_EQ(*s.get_message_state("c"), MessageState::Deferred);
    EXPECT_FALSE(s.get_message_state("zzz").has_value());
    EXPECT_FALSE(s.get_message("zzz").has_value());
    EXPECT_EQ(s.get_message("c")->received_at_epoch, 300);
    EXPECT_EQ(s.set_message_state("zzz", MessageState::Archived, "").error().kind, ErrorKind::NotFound);
}

TEST(MemoryStore, UpsertPreservesStateAndUpdatesSummary) {
    MemoryStore s;
    ASSERT_TRUE(s.upsert_message(msg("m", 10, "old")).ok());
    ASSERT_TRUE(s.set_message_state("m", MessageState::Deferred, "deferred_by_slowmail").ok());
    ASSERT_TRUE(s.upsert_message(msg("m", 10, "new subject")).ok());
    EXPECT_EQ(*s.get_message_state("m"), MessageState::Deferred);
    EXPECT_EQ(s.get_message("m")->subject, "new subject");
    EXPECT_EQ(s.count_messages(), 1u);
    EXPECT_TRUE(s.list_queue().empty());
    EXPECT_EQ(s.upsert_message(msg("", 1)).error().kind, ErrorKind::Invalid);
}

TEST(MemoryStore, MarkGoneTombstonesAndNeverResurrects) {
    MemoryStore s;
    ASSERT_TRUE(s.upsert_message(msg("m", 10)).ok());
    MessageBody b;
    b.id = "m";
    b.text = "body";
    ASSERT_TRUE(s.put_body(b).ok());
    ASSERT_TRUE(s.mark_gone("m").ok());
    EXPECT_EQ(*s.get_message_state("m"), MessageState::GoneOnServer);
    EXPECT_TRUE(s.list_queue().empty());
    EXPECT_EQ(s.count_messages(), 1u);
    ASSERT_TRUE(s.upsert_message(msg("m", 10, "again")).ok());
    EXPECT_EQ(*s.get_message_state("m"), MessageState::GoneOnServer);
    EXPECT_TRUE(s.mark_gone("never-seen").ok());
    EXPECT_EQ(s.count_messages(), 1u);
    ASSERT_TRUE(s.purge_tombstones().ok());
    EXPECT_EQ(s.count_messages(), 0u);
    EXPECT_FALSE(s.get_body("m").has_value());
}

TEST(MemoryStore, Bodies) {
    MemoryStore s;
    EXPECT_FALSE(s.get_body("x").has_value());
    MessageBody b;
    b.id = "x";
    b.text = "hello";
    b.truncated = true;
    AttachmentMeta a;
    a.name = "f.pdf";
    b.attachments.push_back(a);
    ASSERT_TRUE(s.put_body(b).ok());
    const auto got = s.get_body("x");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->text, "hello");
    EXPECT_TRUE(got->truncated);
    ASSERT_EQ(got->attachments.size(), 1u);
    EXPECT_EQ(got->attachments[0].name, "f.pdf");
}

TEST(MemoryStore, SessionsAndItems) {
    MemoryStore s;
    EXPECT_FALSE(s.get_active_session().has_value());
    SessionRecord r1;
    r1.id = "s1";
    r1.started_at = 10;
    r1.state = SessionState::Active;
    ASSERT_TRUE(s.put_session(r1).ok());
    EXPECT_EQ(s.get_active_session()->id, "s1");
    r1.state = SessionState::Ended;
    r1.ended_at = 20;
    ASSERT_TRUE(s.put_session(r1).ok());
    EXPECT_FALSE(s.get_active_session().has_value());
    EXPECT_EQ(s.get_session("s1")->state, SessionState::Ended);
    EXPECT_FALSE(s.get_session("nope").has_value());

    SessionItem it;
    it.session_id = "s1";
    it.message_id = "m1";
    ASSERT_TRUE(s.put_session_item(it).ok());
    it.current_state = MessageState::Archived;
    ASSERT_TRUE(s.put_session_item(it).ok());  // replace, not duplicate
    it.session_id = "s2";
    ASSERT_TRUE(s.put_session_item(it).ok());
    const auto items = s.list_session_items("s1");
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].current_state, MessageState::Archived);
    EXPECT_EQ(s.list_session_items("s2").size(), 1u);
    EXPECT_TRUE(s.list_session_items("s3").empty());
}

TEST(MemoryStore, CommandsFifoAndIdempotencyLookup) {
    MemoryStore s;
    auto mk = [](const std::string& id, const std::string& msgid, CommandState st, const std::string& key) {
        Command c;
        c.id = id;
        c.message_id = msgid;
        c.state = st;
        c.idempotency_key = key;
        return c;
    };
    ASSERT_TRUE(s.put_command(mk("c1", "m1", CommandState::Pending, "k1")).ok());
    ASSERT_TRUE(s.put_command(mk("c2", "m2", CommandState::Pending, "k2")).ok());
    ASSERT_TRUE(s.put_command(mk("c3", "m1", CommandState::Failed, "k3")).ok());
    auto pending = s.list_commands(CommandState::Pending);
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0].id, "c1");
    EXPECT_EQ(pending[1].id, "c2");
    // replace keeps FIFO position
    ASSERT_TRUE(s.put_command(mk("c1", "m1", CommandState::InFlight, "k1")).ok());
    EXPECT_EQ(s.list_commands(CommandState::Pending).size(), 1u);
    EXPECT_EQ(s.list_commands(CommandState::InFlight)[0].id, "c1");
    EXPECT_EQ(s.get_command("c1")->state, CommandState::InFlight);
    EXPECT_FALSE(s.get_command("nope").has_value());
    const auto for_m1 = s.list_commands_for_message("m1");
    ASSERT_EQ(for_m1.size(), 2u);
    EXPECT_EQ(for_m1[0].id, "c1");
    EXPECT_EQ(for_m1[1].id, "c3");
    EXPECT_EQ(s.find_command_by_idempotency_key("k2")->id, "c2");
    EXPECT_FALSE(s.find_command_by_idempotency_key("missing").has_value());
    EXPECT_FALSE(s.find_command_by_idempotency_key("").has_value());
}

TEST(MemoryStore, EventsNewestFirst) {
    MemoryStore s;
    auto mk = [](const std::string& id, const std::string& sess, std::int64_t ts) {
        SessionEvent e;
        e.id = id;
        e.session_id = sess;
        e.timestamp = ts;
        return e;
    };
    ASSERT_TRUE(s.put_event(mk("e1", "s", 10)).ok());
    ASSERT_TRUE(s.put_event(mk("e2", "s", 30)).ok());
    ASSERT_TRUE(s.put_event(mk("e3", "s", 20)).ok());
    ASSERT_TRUE(s.put_event(mk("e4", "other", 99)).ok());
    ASSERT_TRUE(s.put_event(mk("e5", "s", 30)).ok());
    auto ev = s.list_events("s");
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[0].id, "e5");  // tie: later insertion first
    EXPECT_EQ(ev[1].id, "e2");
    EXPECT_EQ(ev[2].id, "e3");
    EXPECT_EQ(ev[3].id, "e1");
    // same id replaces
    SessionEvent upd = mk("e1", "s", 10);
    upd.confirmation = ConfirmationState::Confirmed;
    ASSERT_TRUE(s.put_event(upd).ok());
    ev = s.list_events("s");
    EXPECT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev.back().confirmation, ConfirmationState::Confirmed);
}

TEST(MemoryStore, CommitKeepsChanges) {
    MemoryStore s;
    ASSERT_TRUE(s.begin().ok());
    ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
    ASSERT_TRUE(s.commit().ok());
    EXPECT_EQ(s.count_messages(), 1u);
    EXPECT_FALSE(s.commit().ok()) << "no active transaction";
    EXPECT_TRUE(s.rollback().ok()) << "rollback with nothing to undo is a no-op";
    EXPECT_EQ(s.count_messages(), 1u);
}

TEST(MemoryStore, RollbackRestoresEverything) {
    MemoryStore s;
    ASSERT_TRUE(s.set_setting("k", "before").ok());
    ASSERT_TRUE(s.upsert_message(msg("keep", 1)).ok());
    ASSERT_TRUE(s.put_token_blob("t0").ok());
    InboxSyncState st0;
    st0.delta_link = "d0";
    ASSERT_TRUE(s.put_sync_state(st0).ok());

    ASSERT_TRUE(s.begin().ok());
    ASSERT_TRUE(s.set_setting("k", "after").ok());
    ASSERT_TRUE(s.set_setting("new", "x").ok());
    ASSERT_TRUE(s.upsert_message(msg("added", 2)).ok());
    ASSERT_TRUE(s.set_message_state("keep", MessageState::Archived, "").ok());
    ASSERT_TRUE(s.mark_gone("keep").ok());
    Command c;
    c.id = "c1";
    c.idempotency_key = "k";
    ASSERT_TRUE(s.put_command(c).ok());
    SessionEvent e;
    e.id = "e1";
    e.session_id = "s";
    ASSERT_TRUE(s.put_event(e).ok());
    SessionRecord r;
    r.id = "s";
    ASSERT_TRUE(s.put_session(r).ok());
    SessionItem it;
    it.session_id = "s";
    it.message_id = "keep";
    ASSERT_TRUE(s.put_session_item(it).ok());
    MessageBody b;
    b.id = "keep";
    ASSERT_TRUE(s.put_body(b).ok());
    ASSERT_TRUE(s.put_token_blob("t1").ok());
    InboxSyncState st1;
    st1.delta_link = "d1";
    ASSERT_TRUE(s.put_sync_state(st1).ok());
    AccountInfo a;
    a.tenant_id = "T";
    ASSERT_TRUE(s.put_account(a).ok());
    ASSERT_TRUE(s.purge_tombstones().ok());

    ASSERT_TRUE(s.rollback().ok());
    EXPECT_EQ(*s.get_setting("k"), "before");
    EXPECT_FALSE(s.get_setting("new").has_value());
    EXPECT_EQ(s.count_messages(), 1u);
    EXPECT_EQ(*s.get_message_state("keep"), MessageState::InQueue);
    EXPECT_FALSE(s.get_message("added").has_value());
    EXPECT_FALSE(s.get_command("c1").has_value());
    EXPECT_FALSE(s.find_command_by_idempotency_key("k").has_value());
    EXPECT_TRUE(s.list_events("s").empty());
    EXPECT_FALSE(s.get_session("s").has_value());
    EXPECT_TRUE(s.list_session_items("s").empty());
    EXPECT_FALSE(s.get_body("keep").has_value());
    EXPECT_EQ(*s.get_token_blob(), "t0");
    EXPECT_EQ(*s.get_sync_state().delta_link, "d0");
    EXPECT_FALSE(s.get_account().has_value());
    // store is usable and not in a transaction afterwards
    ASSERT_TRUE(s.begin().ok());
    ASSERT_TRUE(s.commit().ok());
}

TEST(MemoryStore, NestedBeginRejected) {
    MemoryStore s;
    ASSERT_TRUE(s.begin().ok());
    EXPECT_EQ(s.begin().error().kind, ErrorKind::Invalid);
    ASSERT_TRUE(s.rollback().ok());
}

TEST(MemoryStore, FailNextWriteConsumedOnce) {
    MemoryStore s;
    s.fail_next_write();
    const Status f = s.upsert_message(msg("a", 1));
    ASSERT_FALSE(f.ok());
    EXPECT_EQ(f.error().kind, ErrorKind::Storage);
    EXPECT_EQ(s.count_messages(), 0u);
    EXPECT_TRUE(s.upsert_message(msg("a", 1)).ok());
    s.fail_next_write(ErrorKind::Invalid);
    EXPECT_EQ(s.set_setting("k", "v").error().kind, ErrorKind::Invalid);
    EXPECT_TRUE(s.set_setting("k", "v").ok());
}

TEST(MemoryStore, SecureWipeClearsEverything) {
    MemoryStore s;
    ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
    ASSERT_TRUE(s.put_token_blob("t").ok());
    ASSERT_TRUE(s.set_setting("k", "v").ok());
    Command c;
    c.id = "c";
    ASSERT_TRUE(s.put_command(c).ok());
    ASSERT_TRUE(s.secure_wipe().ok());
    EXPECT_EQ(s.count_messages(), 0u);
    EXPECT_FALSE(s.get_token_blob().has_value());
    EXPECT_FALSE(s.get_setting("k").has_value());
    EXPECT_FALSE(s.get_command("c").has_value());
}

// ---------------------------------------------------------------------------
// Transaction RAII
// ---------------------------------------------------------------------------
TEST(Transaction, CommitPersists) {
    MemoryStore s;
    {
        Transaction tx(s);
        ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
        ASSERT_TRUE(tx.commit().ok());
    }
    EXPECT_EQ(s.count_messages(), 1u);
}

TEST(Transaction, DestructionWithoutCommitRollsBack) {
    MemoryStore s;
    ASSERT_TRUE(s.upsert_message(msg("pre", 1)).ok());
    {
        Transaction tx(s);
        ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
        ASSERT_TRUE(s.set_setting("k", "v").ok());
        EXPECT_EQ(s.count_messages(), 2u);
    }
    EXPECT_EQ(s.count_messages(), 1u);
    EXPECT_FALSE(s.get_setting("k").has_value());
}

TEST(Transaction, MidTransactionFailureIsAtomic) {
    MemoryStore s;
    {
        Transaction tx(s);
        ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
        s.fail_next_write();
        Command c;
        c.id = "c";
        if (s.put_command(c).ok()) {
            ASSERT_TRUE(tx.commit().ok());
        }
    }
    EXPECT_EQ(s.count_messages(), 0u) << "partial work must not survive";
    EXPECT_FALSE(s.get_command("c").has_value());
}

TEST(Transaction, DoubleCommitReportsInvalidAndNestedDoesNotClobberOuter) {
    MemoryStore s;
    Transaction outer(s);
    ASSERT_TRUE(s.upsert_message(msg("a", 1)).ok());
    {
        Transaction inner(s);  // begin() fails (no nesting); inner must not roll back the outer one
        ASSERT_TRUE(s.upsert_message(msg("b", 2)).ok());
        EXPECT_EQ(inner.commit().error().kind, ErrorKind::Invalid);
    }
    EXPECT_EQ(s.count_messages(), 2u);
    ASSERT_TRUE(outer.commit().ok());
    EXPECT_EQ(outer.commit().error().kind, ErrorKind::Invalid);
    EXPECT_EQ(s.count_messages(), 2u);
}
