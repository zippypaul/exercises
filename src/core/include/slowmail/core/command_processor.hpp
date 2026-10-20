#pragma once
#include <string>
#include <vector>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/mail_backend.hpp"
#include "slowmail/core/store.hpp"

namespace slowmail::core {

struct ProcessReport {
    std::size_t confirmed = 0;
    std::size_t failed = 0;
    std::size_t abandoned = 0;
    std::size_t deferred_retry = 0;    // still pending (retryable error / throttled)
    std::optional<int> retry_after_seconds;  // largest Retry-After seen
    bool went_offline = false;         // a Network error stopped processing
    bool auth_required = false;        // an Auth error stopped processing; caller moves session to AuthRequired
    bool reached_server = false;       // at least one backend call got a (non-transport) answer: the network works
};

struct RetryPolicy {
    // Legacy knob, no longer a retry limit: retryable errors (Server, Conflict, Forbidden, Throttled, an
    // empty-code NotFound) keep the command Pending with exponential backoff capped at max_backoff_seconds, so a
    // long Graph outage never hides mail. Kept so existing aggregate initialisers still compile.
    int max_attempts = 8;
    int base_backoff_seconds = 2;      // exponential, capped
    int max_backoff_seconds = 300;
};

// Durable outgoing command queue (handover invariants 2, 3, 10; A03-A06, A09).
// Rules:
//  * enqueue() is idempotent on idempotency_key: a second enqueue of the same logical action returns the existing command
//  * enqueue writes command + message state (*Pending) + session event (Pending) in one transaction
//  * process() drains Pending commands FIFO, one at a time, synchronously:
//      InFlight is persisted BEFORE the network call so a crash leaves evidence (A06)
//      Confirmed -> message state Archived/Deleted/Deferred, event Confirmed
//      NotFound with code "ErrorItemNotFound" -> message may have been moved elsewhere already: mark message
//                   GoneOnServer, command Abandoned, event Failed("already gone"); never retried (no duplicate harm)
//      NotFound with ANY OTHER code (including an empty one, e.g. a code-less 404 from a proxy) -> a code-less
//                   404 is a transient: stays Pending with backoff; a coded one (ErrorFolderNotFound) is Failed
//      Throttled -> stays Pending, report retry_after, processing stops for this run
//      Network   -> stays Pending, went_offline, processing stops
//      Auth      -> stays Pending, stops; caller moves session to AuthRequired
//      Conflict / Server / Forbidden -> stay Pending with exponential backoff (never Failed by attempt count)
//      other non-retryable (Malformed, Invalid, Policy, NotFound with another code, ...) -> Failed immediately,
//                   message state Failed with reason; TriageSession::failed()/retry() surface and re-run it
//      Confirmed on the server but the local write failed -> the write is retried once, then the command goes
//                   back to Pending (the retry is idempotent); recover_in_flight() runs at the top of process()
//      not_before further in the future than max_backoff + 1 h is ignored (clock moved backwards)
//  * Undo* commands are compensating moves back to Inbox; they are enqueued only if the original is Confirmed
//  * Defer == set_flag(Flagged); Undefer == set_flag(NotFlagged). Both idempotent.
class CommandProcessor {
public:
    CommandProcessor(Store& store, MailBackend& backend, const Clock& clock, RetryPolicy policy = {});

    Outcome<Command> enqueue(const std::string& session_id, const std::string& message_id, CommandType type,
                             const std::string& payload_json = "{}");

    // Re-enqueues the command that last failed for `message_id` (same type and payload minus scheduling data).
    // The message must be in state Failed. Returns the new Pending command.
    Outcome<Command> retry(const std::string& session_id, const std::string& message_id);

    ProcessReport process();

    // Crash recovery: any InFlight command found at startup goes back to Pending (attempt counted) so it
    // is retried; the retry is safe because NotFound is treated as "already done elsewhere".
    std::size_t recover_in_flight();

    static std::string make_idempotency_key(const std::string& session_id, const std::string& message_id, CommandType type);

private:
    Outcome<Command> enqueue_impl(const std::string& session_id, const std::string& message_id, CommandType type,
                                  const std::string& payload_json, bool allow_failed_state);

    Store& store_;
    MailBackend& backend_;
    const Clock& clock_;
    RetryPolicy policy_;
};

} // namespace slowmail::core
