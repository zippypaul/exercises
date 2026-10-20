#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/command_processor.hpp"
#include "slowmail/core/cooldown.hpp"
#include "slowmail/core/mail_backend.hpp"
#include "slowmail/core/session.hpp"
#include "slowmail/core/store.hpp"
#include "slowmail/core/sync_engine.hpp"

namespace slowmail::core {

// What the UI shows in the top bar.
struct StatusSnapshot {
    SessionState session_state = SessionState::NotStarted;
    std::string session_id;
    bool signed_in = false;
    bool offline = false;
    CooldownDecision cooldown;
    std::size_t queue_size = 0;
    std::size_t pending_commands = 0;   // "Pending" indicator (Q8)
    std::size_t deferred_count = 0;     // Q5: count only, no queue by default
    std::string last_error;             // redacted
};

// One row in the activity view.
struct ActivityRow {
    SessionEvent event;
    std::string subject;      // from display_snapshot
    std::string from_name;
    std::string from_address;
    std::string received_at_iso;
};

struct TriageConfig {
    CooldownConfig cooldown;
    RetryPolicy retry;
    bool hide_preexisting_flagged = false;  // Q6 default: do NOT hide messages flagged outside Slow Mail
    bool show_deferred_panel = false;       // Q5 default: count only
};

// Facade used by UI and CLI. Single-threaded: callers serialise access (UI uses a worker thread + queue).
// Every method is synchronous and returns quickly except refresh() and flush(), which hit the network.
class TriageSession {
public:
    TriageSession(std::unique_ptr<Store> store, std::unique_ptr<MailBackend> backend,
                  std::shared_ptr<Clock> clock, TriageConfig config = {});
    ~TriageSession();

    // Opens the store, recovers in-flight commands, resumes an active session or starts a new one (Q7).
    // Never touches the network.
    Status start();

    // Attempts a manual inbound refresh if eligible. Returns Policy error with next_eligible when blocked (A02).
    // On success the cooldown clock starts (A01). Partial failures are resumable and consume no cooldown (A07).
    Outcome<SyncReport> refresh(bool force = false);

    // Pushes pending outgoing commands now. Independent of the inbound cooldown (A03-A05).
    ProcessReport flush();

    // Queue and detail
    std::vector<MessageSummary> queue() const;
    Outcome<MessageBody> open(const std::string& message_id);   // cached body or fetch (not cooldown-limited: it is one message on demand)
    std::optional<MessageSummary> message(const std::string& message_id) const;

    // DDD actions. Each enqueues durably and, if online, flushes immediately. Returns the command.
    Outcome<Command> archive(const std::string& message_id);     // Do
    Outcome<Command> remove(const std::string& message_id);      // Delete (soft)
    Outcome<Command> defer(const std::string& message_id);       // Defer
    Outcome<Command> undefer(const std::string& message_id);     // restore (Q5 explicit mechanism)
    Outcome<Command> undo(const std::string& command_id);        // compensating move for Confirmed archive/delete

    // Session activity (Q12)
    std::vector<ActivityRow> activity() const;
    std::vector<MessageSummary> deferred() const;               // only if config.show_deferred_panel; else empty

    // Explicit end (Q7). Flushes if possible; pending commands survive for the next session.
    Status end();

    // Sign-out: clears tokens and, per policy, the cache (A10).
    Status sign_out(bool wipe_cache);

    StatusSnapshot status() const;
    void set_online(bool online);   // platform informs connectivity; affects whether actions auto-flush

    Store& store() { return *store_; }
    MailBackend& backend() { return *backend_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<MailBackend> backend_;
    std::shared_ptr<Clock> clock_;
};

} // namespace slowmail::core
