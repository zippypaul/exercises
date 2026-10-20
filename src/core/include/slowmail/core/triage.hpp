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
    std::size_t failed_count = 0;       // messages whose last action failed non-retryably (see failed()/retry())
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
    // It does NOT wait on the network: the only backend call is `backend.account()`, which for the Graph backend
    // is a local token check (no request). If commands are Pending from an earlier run, start() makes ONE
    // delivery attempt (flush) and ignores its errors; a Network error leaves them Pending.
    Status start();

    // Manual inbound refresh. First flushes pending outgoing commands (independent of the cooldown), then, if
    // the cooldown allows, syncs. Returns a Policy error with next_eligible when blocked (A02). On success the
    // cooldown clock starts (A01); partial failures are resumable and consume no cooldown (A07).
    // Allowed from Offline: "offline" is only a belief. A success clears it (Offline -> Active), a failure
    // keeps it. A Network/Auth/Throttled error from the flush step is returned without attempting the sync.
    Outcome<SyncReport> refresh(bool force = false);

    // Pushes pending outgoing commands now. Independent of the inbound cooldown (A03-A05). Always attempts
    // delivery; a Network error leaves commands Pending and reports went_offline. Any server answer clears the
    // offline flag.
    ProcessReport flush();

    // Queue and detail
    std::vector<MessageSummary> queue() const;
    Outcome<MessageBody> open(const std::string& message_id);   // cached body or fetch (not cooldown-limited: it is one message on demand)
    std::optional<MessageSummary> message(const std::string& message_id) const;

    // DDD actions. Each enqueues durably and flushes immediately (a Network error just leaves the command Pending;
    // only an explicit set_online(false) hint suppresses the attempt). Returns the command.
    Outcome<Command> archive(const std::string& message_id);     // Do
    Outcome<Command> remove(const std::string& message_id);      // Delete (soft)
    Outcome<Command> defer(const std::string& message_id);       // Defer
    Outcome<Command> undefer(const std::string& message_id);     // restore (Q5 explicit mechanism)
    Outcome<Command> undo(const std::string& command_id);        // compensating move for Confirmed archive/delete

    // Failed actions. A message is Failed after a non-retryable error (Malformed, Invalid, NotFound with a Graph
    // code other than ErrorItemNotFound, Policy). Retryable errors (Network, Server, Conflict, Forbidden,
    // Throttled, a code-less 404) never fail a command: they stay Pending with capped exponential backoff.
    std::vector<MessageSummary> failed() const;                      // newest first
    std::optional<Command> failed_command(const std::string& message_id) const;  // most recent Failed command
    std::string failure_reason(const std::string& message_id) const;  // its last_error (redacted), "" if none
    // Re-enqueues the same CommandType as the last Failed command for the message (allowed from Failed) and
    // tries to deliver it. Invalid if the message is not Failed.
    Outcome<Command> retry(const std::string& message_id);

    // Session activity (Q12)
    std::vector<ActivityRow> activity() const;
    std::vector<MessageSummary> deferred() const;               // only if config.show_deferred_panel; else empty

    // Explicit end (Q7). Flushes if possible; pending commands survive for the next session.
    Status end();

    // Sign-out: clears tokens and, per policy, the cache (A10).
    Status sign_out(bool wipe_cache);

    StatusSnapshot status() const;
    // Explicit connectivity hint from the platform (nothing in production calls it). set_online(false) shows
    // "offline" and stops act() from auto-flushing until set_online(true) or any successful network operation
    // (flush/refresh); an explicit flush() or refresh() always attempts delivery. A Network error discovered by
    // the app itself sets the offline FLAG but never gates delivery.
    void set_online(bool online);

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
