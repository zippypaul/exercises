#pragma once
#include <string>
#include <vector>

#include "slowmail/core/types.hpp"

namespace slowmail::core {

// Pure session state machine (handover §5). No I/O; TriageSession persists transitions.
//  NOT_STARTED -> AUTH_REQUIRED? -> READY_CACHED -> SYNC_ELIGIBLE -> SYNCING -> ACTIVE -> ENDING -> ENDED
//  Recoverable: AUTH_REQUIRED, OFFLINE, SYNC_PARTIAL.
enum class SessionEvent_ {
    Start, AuthNeeded, AuthOk, CooldownElapsed, CooldownActive, SyncStarted, SyncCompleted, SyncInterrupted,
    NetworkLost, NetworkBack, End, Ended
};
const char* to_string(SessionEvent_ e);

class SessionStateMachine {
public:
    explicit SessionStateMachine(SessionState initial = SessionState::NotStarted) : state_(initial) {}
    SessionState state() const { return state_; }

    // Returns false (and leaves state unchanged) for an illegal transition.
    bool apply(SessionEvent_ ev);
    static bool is_legal(SessionState from, SessionEvent_ ev, SessionState& to);

    // States in which cached mail may be read and actions may be queued.
    static bool can_process_mail(SessionState s);
    // States in which an inbound refresh may be attempted (subject to cooldown). Includes Offline: a failed
    // attempt simply leaves the session Offline, a successful one moves it back to Active.
    static bool can_attempt_sync(SessionState s);

private:
    SessionState state_;
};

std::string new_id(const std::string& prefix); // "prefix-<16 hex>" using a CSPRNG where available

} // namespace slowmail::core
