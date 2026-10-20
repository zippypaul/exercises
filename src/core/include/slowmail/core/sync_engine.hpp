#pragma once
#include <functional>
#include <string>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/cooldown.hpp"
#include "slowmail/core/mail_backend.hpp"
#include "slowmail/core/store.hpp"

namespace slowmail::core {

struct SyncReport {
    bool complete = false;        // deltaLink committed
    bool resumed = false;         // continued from a stored resume_next_link
    std::size_t pages = 0;
    std::size_t added = 0;        // new to cache
    std::size_t updated = 0;      // already cached, fields changed
    std::size_t removed = 0;      // tombstoned
    std::size_t reconciled_pending = 0;  // pending commands abandoned because the server already moved the message
    std::size_t restored_to_queue = 0;   // deferred items whose flag was cleared elsewhere (Q5)
    bool full_resync = false;     // delta token expired (Gone) and we started over
    Error error;                  // set when !complete
};

// Drives Inbox delta sync and reconciliation (handover §4 invariants 1, 4, 5, 7, 8; A07, A08).
// Rules:
//  * cooldown is checked by the caller (TriageSession) unless `force` is set (tests, pro)
//  * each page is committed in its own transaction together with sync_state.resume_next_link,
//    so a crash mid-sync resumes rather than restarts and never consumes cooldown
//  * last_complete_* and delta_link are written only after the final page (deltaLink) is processed
//  * reconciliation per changed message:
//      - new id                      -> cache + InQueue (unless flagged and Q6 says show anyway: still InQueue)
//      - cached, state InQueue       -> update fields
//      - cached, Deferred, flag now NotFlagged -> restore to InQueue (Q5 default), event Undeferred
//      - cached, *Pending            -> update fields only; the command processor decides
//      - cached, Archived/Deleted/GoneOnServer but still reported in the Inbox -> treat server as truth: back
//        to InQueue (someone moved it back), log once. Only when the entry's parent_folder_id equals the stored
//        setting "inbox_folder_id" (learned from the first full enumeration, majority value); with an unknown
//        inbox id or an empty parent_folder_id the entry is trusted. A stale entry from another folder is ignored.
//      - cached, Failed, still reported in the Inbox and without a Pending/InFlight command -> back to InQueue
//        (Deferred again if a failed Undefer left it flagged)
//    within a page, removals are applied BEFORE changed entries (removed and re-added = in the Inbox now)
//    per removed id:
//      - cached, any state           -> mark_gone, abandon pending commands for it (no resurrection)
//  * on ErrorKind::Gone, clear delta_link/resume, run a full sync once; tombstone anything not seen
class SyncEngine {
public:
    SyncEngine(Store& store, MailBackend& backend, const Clock& clock, CooldownPolicy policy);

    // Runs to completion or first error. Progress callback receives pages processed (for a static UI counter).
    SyncReport run(bool force, const std::function<void(std::size_t)>& progress = {});

    CooldownDecision cooldown_decision() const;

private:
    Store& store_;
    MailBackend& backend_;
    const Clock& clock_;
    CooldownPolicy policy_;
};

} // namespace slowmail::core
