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
};

struct RetryPolicy {
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
//      NotFound  -> message may have been moved elsewhere already: mark message GoneOnServer,
//                   command Abandoned, event Failed("already gone"); never retried (no duplicate harm)
//      Throttled -> stays Pending, report retry_after, processing stops for this run
//      Network   -> stays Pending, went_offline, processing stops
//      Auth      -> stays Pending, stops; caller moves session to AuthRequired
//      Conflict  -> stays Pending once, then Failed after max_attempts
//      other non-retryable -> Failed, message state Failed with reason
//  * Undo* commands are compensating moves back to Inbox; they are enqueued only if the original is Confirmed
//  * Defer == set_flag(Flagged); Undefer == set_flag(NotFlagged). Both idempotent.
class CommandProcessor {
public:
    CommandProcessor(Store& store, MailBackend& backend, const Clock& clock, RetryPolicy policy = {});

    Outcome<Command> enqueue(const std::string& session_id, const std::string& message_id, CommandType type,
                             const std::string& payload_json = "{}");

    ProcessReport process();

    // Crash recovery: any InFlight command found at startup goes back to Pending (attempt counted) so it
    // is retried; the retry is safe because NotFound is treated as "already done elsewhere".
    std::size_t recover_in_flight();

    static std::string make_idempotency_key(const std::string& session_id, const std::string& message_id, CommandType type);

private:
    Store& store_;
    MailBackend& backend_;
    const Clock& clock_;
    RetryPolicy policy_;
};

} // namespace slowmail::core
