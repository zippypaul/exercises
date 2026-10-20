// Engine scenarios run against every Store implementation (MemoryStore, SqliteStore on ":memory:" and on a
// temporary file) so that store-contract divergence is caught: the core unit tests only ever see
// MemoryStore, the storage tests never see the engines.
//
// Deliberately NOT asserted here (other code is changing): offline handling details and how Failed
// messages are listed.
#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../core/fake_backend.hpp"
#include "slowmail/core/clock.hpp"
#include "slowmail/core/command_processor.hpp"
#include "slowmail/core/memory_store.hpp"
#include "slowmail/core/triage.hpp"
#include "slowmail/storage/sqlite_store.hpp"

using namespace slowmail::core;
using slowmail::storage::SqliteStore;
using slowmail::testing::FakeBackend;

namespace {

constexpr std::int64_t kHour = 3600;

enum class Kind { Memory, SqliteMemory, SqliteFile };

const char* kind_name(Kind k) {
    switch (k) {
    case Kind::Memory: return "Memory";
    case Kind::SqliteMemory: return "SqliteMemory";
    case Kind::SqliteFile: return "SqliteFile";
    }
    return "?";
}

// Owns the temp directory used by the SqliteFile kind and creates stores of the requested kind.
class StoreFactory {
public:
    explicit StoreFactory(Kind k) : kind_(k) {
        if (k == Kind::SqliteFile) {
            char tmpl[] = "/tmp/slowmail_integration_XXXXXX";
            const char* p = ::mkdtemp(tmpl);
            dir_ = p != nullptr ? p : "";
        }
    }
    ~StoreFactory() {
        if (dir_.empty()) return;
        for (const char* suffix : {"/mail.db", "/mail.db-wal", "/mail.db-shm"}) ::unlink((dir_ + suffix).c_str());
        ::rmdir(dir_.c_str());
    }
    StoreFactory(const StoreFactory&) = delete;
    StoreFactory& operator=(const StoreFactory&) = delete;

    Kind kind() const { return kind_; }
    // True when a brand-new store object sees the data written by an earlier one (a process restart).
    bool durable() const { return kind_ == Kind::SqliteFile; }

    // Not opened: TriageSession::start() opens it; raw users call open().
    std::unique_ptr<Store> make() const {
        switch (kind_) {
        case Kind::Memory: return std::make_unique<MemoryStore>();
        case Kind::SqliteMemory: return std::make_unique<SqliteStore>(":memory:");
        case Kind::SqliteFile: return std::make_unique<SqliteStore>(dir_ + "/mail.db");
        }
        return nullptr;
    }

private:
    Kind kind_;
    std::string dir_;
};

// Lets several TriageSession objects (successive "processes") talk to the same fake server.
class ProxyBackend final : public MailBackend {
public:
    explicit ProxyBackend(FakeBackend& b) : b_(b) {}
    Outcome<DeltaPage> fetch_inbox_delta(const std::string& link) override { return b_.fetch_inbox_delta(link); }
    Outcome<MessageBody> fetch_body(const std::string& id) override { return b_.fetch_body(id); }
    Outcome<MoveResult> move(const std::string& id, WellKnownFolder d) override { return b_.move(id, d); }
    Status set_flag(const std::string& id, FlagStatus f) override { return b_.set_flag(id, f); }
    Outcome<AccountInfo> account() override { return b_.account(); }

private:
    FakeBackend& b_;
};

std::vector<std::string> ids_of(const std::vector<MessageSummary>& v) {
    std::vector<std::string> out;
    for (const auto& m : v) out.push_back(m.id);
    return out;
}

class StoreScenarios : public ::testing::TestWithParam<Kind> {
protected:
    StoreScenarios() : factory(GetParam()) {}

    // Backend with m1..m<n>, a TriageSession over a fresh store, started and synced once, clock +5h.
    void ready(int messages = 3) {
        for (int i = 1; i <= messages; ++i) {
            backend.add_message("m" + std::to_string(i), "Subject " + std::to_string(i), 1'799'990'000 - i);
        }
        build_session();
        Status started = session->start();
        ASSERT_TRUE(started.ok()) << started.error().message;
        auto r = session->refresh();
        ASSERT_TRUE(r.ok()) << r.error().message;
        clock->advance(5 * kHour);
        backend.clear_calls();
    }
    void build_session() {
        auto s = factory.make();
        store = s.get();
        session = std::make_unique<TriageSession>(std::move(s), std::make_unique<ProxyBackend>(backend), clock, cfg);
    }
    // New process: drop everything in memory, keep only what the store persisted. Only meaningful when
    // the store is durable.
    void restart_process() {
        session.reset();
        store = nullptr;
        build_session();
    }
    void sync_now() {
        clock->advance(5 * kHour);
        auto r = session->refresh();
        ASSERT_TRUE(r.ok()) << r.error().message;
    }
    std::vector<std::string> queue_ids() { return ids_of(session->queue()); }

    StoreFactory factory;
    FakeBackend backend;
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    TriageConfig cfg;
    Store* store = nullptr;  // owned by `session`
    std::unique_ptr<TriageSession> session;
};

std::string param_name(const ::testing::TestParamInfo<Kind>& i) { return kind_name(i.param); }

} // namespace

// (1) F4: a message another client moves out and back must be listed again and must survive session end.
TEST_P(StoreScenarios, MovedOutAndBackByAnotherClientIsRestoredAndSurvivesEnd) {
    ready();
    ASSERT_EQ(queue_ids().size(), 3u);

    backend.other_client_move("m2", WellKnownFolder::Archive);
    sync_now();
    EXPECT_EQ(store->get_message_state("m2"), MessageState::GoneOnServer);
    for (const auto& id : queue_ids()) EXPECT_NE(id, "m2");
    EXPECT_EQ(queue_ids().size(), 2u);

    backend.other_client_move("m2", WellKnownFolder::Inbox);
    sync_now();
    EXPECT_EQ(store->get_message_state("m2"), MessageState::InQueue);
    auto q = queue_ids();
    EXPECT_EQ(q.size(), 3u);
    EXPECT_NE(std::find(q.begin(), q.end(), "m2"), q.end()) << "restored message is listed again";

    ASSERT_TRUE(session->end().ok());  // purges tombstones
    EXPECT_EQ(store->get_message_state("m2"), MessageState::InQueue) << "restored message is not purged";
    EXPECT_TRUE(store->get_message("m2").has_value());
    EXPECT_EQ(store->list_queue().size(), 3u);
}

TEST_P(StoreScenarios, TrulyGoneMessageIsPurgedAtEndWhileRestoredOneStays) {
    ready();
    backend.other_client_move("m1", WellKnownFolder::Archive);
    backend.other_client_purge("m3");
    sync_now();
    EXPECT_EQ(store->get_message_state("m1"), MessageState::GoneOnServer);
    EXPECT_EQ(store->get_message_state("m3"), MessageState::GoneOnServer);
    backend.other_client_move("m1", WellKnownFolder::Inbox);
    sync_now();
    ASSERT_TRUE(session->end().ok());
    EXPECT_EQ(store->get_message_state("m1"), MessageState::InQueue);
    EXPECT_FALSE(store->get_message_state("m3").has_value()) << "tombstone purged";
    EXPECT_TRUE(store->get_message_state("m2").has_value());
}

// (2) F9: commands created in the same second are processed in enqueue order.
TEST_P(StoreScenarios, SameSecondArchivesReachTheBackendInEnqueueOrder) {
    ready(3);
    session->set_online(false);
    const std::vector<std::string> order = {"m3", "m1", "m2"};
    for (const auto& id : order) ASSERT_TRUE(session->archive(id).ok()) << id;
    session->set_online(true);
    ProcessReport pr = session->flush();
    EXPECT_EQ(pr.confirmed, 3u);

    std::vector<std::string> moved;
    for (const auto& c : backend.calls) {
        if (c.op == "move" && c.ok) moved.push_back(c.arg);
    }
    EXPECT_EQ(moved, order);
}

TEST_P(StoreScenarios, ManySameSecondArchivesKeepFifoOrder) {
    ready(12);
    session->set_online(false);
    std::vector<std::string> order = {"m7", "m2", "m12", "m1", "m9", "m4", "m11", "m3", "m8", "m5", "m10", "m6"};
    for (const auto& id : order) ASSERT_TRUE(session->archive(id).ok()) << id;
    // Store-level FIFO too, independent of the processor.
    std::vector<std::string> pending_msgs;
    for (const auto& c : store->list_commands(CommandState::Pending)) pending_msgs.push_back(c.message_id);
    EXPECT_EQ(pending_msgs, order);

    session->set_online(true);
    EXPECT_EQ(session->flush().confirmed, 12u);
    std::vector<std::string> moved;
    for (const auto& c : backend.calls) {
        if (c.op == "move" && c.ok) moved.push_back(c.arg);
    }
    EXPECT_EQ(moved, order);
}

// (3) A06: in-flight command found by a second processor over the same store (same file for SQLite).
TEST_P(StoreScenarios, CrashRecoveryByASecondProcessorOverTheSameStore) {
    backend.add_message("m1", "Subject 1");
    std::unique_ptr<Store> first = factory.make();
    ASSERT_TRUE(first->open().ok());
    ASSERT_TRUE(first->upsert_message(FakeBackend::make_message("m1", "Subject 1")).ok());
    SessionRecord rec;
    rec.id = "sess-a06";
    rec.started_at = clock->now_wall();
    rec.state = SessionState::Active;
    ASSERT_TRUE(first->put_session(rec).ok());

    std::string cmd_id;
    {
        CommandProcessor p1(*first, backend, *clock);
        auto c = p1.enqueue(rec.id, "m1", CommandType::Archive);
        ASSERT_TRUE(c.ok()) << c.error().message;
        cmd_id = c.value().id;
        // The process dies right after InFlight was persisted and before the move reached the server.
        Command inflight = *first->get_command(cmd_id);
        inflight.state = CommandState::InFlight;
        inflight.attempts = 1;
        ASSERT_TRUE(first->put_command(inflight).ok());
    }

    Store* second = first.get();
    std::unique_ptr<Store> reopened;
    if (factory.durable()) {
        first.reset();  // closes the first connection
        reopened = factory.make();
        ASSERT_TRUE(reopened->open().ok());
        second = reopened.get();
    }
    ASSERT_TRUE(second->get_command(cmd_id).has_value());
    EXPECT_EQ(second->get_command(cmd_id)->state, CommandState::InFlight);
    EXPECT_EQ(second->get_message_state("m1"), MessageState::DoPending);

    CommandProcessor p2(*second, backend, *clock);
    EXPECT_EQ(p2.recover_in_flight(), 1u);
    EXPECT_EQ(second->get_command(cmd_id)->state, CommandState::Pending);
    EXPECT_EQ(p2.recover_in_flight(), 0u);
    ProcessReport r = p2.process();
    EXPECT_EQ(r.confirmed, 1u);
    EXPECT_TRUE(backend.in_archive("m1"));
    EXPECT_EQ(backend.count("move", "m1"), 1);
    EXPECT_EQ(second->get_command(cmd_id)->state, CommandState::Confirmed);
    EXPECT_EQ(second->get_command(cmd_id)->attempts, 2);
    EXPECT_EQ(second->get_message_state("m1"), MessageState::Archived);
    EXPECT_EQ(second->list_commands_for_message("m1").size(), 1u) << "no duplicate command";
}

// (4) F9: defer -> undefer -> defer inside one second yields three commands in that order.
TEST_P(StoreScenarios, DeferUndeferDeferWithinOneSecondKeepsOrderAndEndsDeferred) {
    ready();
    ASSERT_TRUE(session->defer("m1").ok());
    ASSERT_TRUE(session->undefer("m1").ok());
    auto last = session->defer("m1");
    ASSERT_TRUE(last.ok()) << last.error().message;

    const auto cmds = store->list_commands_for_message("m1");
    ASSERT_EQ(cmds.size(), 3u);
    EXPECT_EQ(cmds[0].type, CommandType::Defer);
    EXPECT_EQ(cmds[1].type, CommandType::Undefer);
    EXPECT_EQ(cmds[2].type, CommandType::Defer);
    EXPECT_EQ(cmds[2].id, last.value().id);
    EXPECT_EQ(cmds[0].created_at, cmds[2].created_at) << "all three share one second";
    EXPECT_TRUE(cmds[2].state == CommandState::Pending || cmds[2].state == CommandState::Confirmed);

    session->flush();  // no-op if everything was confirmed already
    EXPECT_EQ(store->get_command(last.value().id)->state, CommandState::Confirmed);
    EXPECT_EQ(store->get_message_state("m1"), MessageState::Deferred);
    EXPECT_EQ(backend.flag_of("m1"), FlagStatus::Flagged);
    const auto again = store->list_commands_for_message("m1");
    ASSERT_EQ(again.size(), 3u);
    EXPECT_EQ(again.back().type, CommandType::Defer);
}

// (5) A13: end()/start() keeps deferred suppression and scopes the activity view to the session.
TEST_P(StoreScenarios, EndThenStartKeepsDeferredSuppressionAndScopesActivity) {
    ready();
    ASSERT_TRUE(session->defer("m1").ok());
    ASSERT_TRUE(session->archive("m2").ok());
    const std::string first = session->status().session_id;
    ASSERT_EQ(session->activity().size(), 2u);

    ASSERT_TRUE(session->end().ok());
    clock->advance(kHour);
    ASSERT_TRUE(session->start().ok());
    const std::string second = session->status().session_id;
    EXPECT_NE(second, first);
    EXPECT_TRUE(session->activity().empty()) << "activity is scoped to the current session";
    EXPECT_EQ(store->list_events(first).size(), 2u);
    EXPECT_EQ(session->status().deferred_count, 1u);
    EXPECT_EQ(queue_ids(), (std::vector<std::string>{"m3"}));

    // A later sync still reports the flagged m1 as changed; it must stay suppressed.
    sync_now();
    EXPECT_EQ(queue_ids(), (std::vector<std::string>{"m3"}));
    EXPECT_EQ(store->get_message_state("m1"), MessageState::Deferred);

    ASSERT_TRUE(session->archive("m3").ok());
    EXPECT_EQ(session->activity().size(), 1u);
    EXPECT_EQ(store->list_events(first).size(), 2u);
}

// Only for the file-backed store: a new store object (new process) must see what the old one persisted.
using DurableScenarios = StoreScenarios;

TEST_P(DurableScenarios, SessionRestartOverTheSameFileKeepsDeferredSuppressionAndActivityScoping) {
    ASSERT_TRUE(factory.durable());
    ready();
    const bool eligible_before = session->status().cooldown.eligible;
    ASSERT_TRUE(session->defer("m1").ok());
    ASSERT_TRUE(session->archive("m2").ok());
    const std::string first = session->status().session_id;
    ASSERT_TRUE(session->end().ok());

    restart_process();
    clock->advance(kHour);
    ASSERT_TRUE(session->start().ok());
    const std::string second = session->status().session_id;
    EXPECT_NE(second, first);
    EXPECT_TRUE(session->activity().empty());
    EXPECT_EQ(store->list_events(first).size(), 2u);
    EXPECT_EQ(session->status().deferred_count, 1u);
    EXPECT_EQ(store->get_message_state("m1"), MessageState::Deferred);
    EXPECT_EQ(store->get_message_state("m2"), MessageState::Archived);
    EXPECT_EQ(queue_ids(), (std::vector<std::string>{"m3"}));
    EXPECT_EQ(session->status().cooldown.eligible, eligible_before) << "the inbound clock belongs to the account";

    // The activity order is newest first, same-second events by reverse insertion order.
    ASSERT_TRUE(session->remove("m3").ok());
    EXPECT_EQ(session->activity().size(), 1u);

    ASSERT_TRUE(session->end().ok());
    restart_process();
    ASSERT_TRUE(session->start().ok());
    EXPECT_NE(session->status().session_id, second);
    EXPECT_TRUE(session->activity().empty());
    EXPECT_EQ(store->list_events(second).size(), 1u);
    EXPECT_EQ(store->list_events(first).size(), 2u);
    EXPECT_EQ(session->status().deferred_count, 1u);
}

INSTANTIATE_TEST_SUITE_P(Stores, StoreScenarios,
                         ::testing::Values(Kind::Memory, Kind::SqliteMemory, Kind::SqliteFile), param_name);
INSTANTIATE_TEST_SUITE_P(Stores, DurableScenarios, ::testing::Values(Kind::SqliteFile), param_name);
