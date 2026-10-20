#include "slowmail/core/session.hpp"

#include <cstdint>
#include <cstdio>
#include <random>

namespace slowmail::core {

const char* to_string(SessionEvent_ e) {
    switch (e) {
    case SessionEvent_::Start: return "start";
    case SessionEvent_::AuthNeeded: return "auth_needed";
    case SessionEvent_::AuthOk: return "auth_ok";
    case SessionEvent_::CooldownElapsed: return "cooldown_elapsed";
    case SessionEvent_::CooldownActive: return "cooldown_active";
    case SessionEvent_::SyncStarted: return "sync_started";
    case SessionEvent_::SyncCompleted: return "sync_completed";
    case SessionEvent_::SyncInterrupted: return "sync_interrupted";
    case SessionEvent_::NetworkLost: return "network_lost";
    case SessionEvent_::NetworkBack: return "network_back";
    case SessionEvent_::End: return "end";
    case SessionEvent_::Ended: return "ended";
    }
    return "unknown";
}

namespace {

struct Transition {
    SessionState from;
    SessionEvent_ ev;
    SessionState to;
};

using S = SessionState;
using E = SessionEvent_;

// The complete machine (handover section 5). Anything not listed is illegal.
constexpr Transition kTransitions[] = {
    {S::NotStarted, E::Start, S::ReadyCached},
    {S::ReadyCached, E::AuthNeeded, S::AuthRequired},
    {S::AuthRequired, E::AuthOk, S::ReadyCached},
    {S::ReadyCached, E::CooldownElapsed, S::SyncEligible},
    {S::ReadyCached, E::CooldownActive, S::Active},
    {S::SyncEligible, E::SyncStarted, S::Syncing},
    {S::SyncEligible, E::CooldownActive, S::Active},
    {S::Syncing, E::SyncCompleted, S::Active},
    {S::Syncing, E::SyncInterrupted, S::SyncPartial},
    {S::SyncPartial, E::SyncStarted, S::Syncing},
    {S::SyncPartial, E::CooldownActive, S::Active},
    {S::Active, E::NetworkLost, S::Offline},
    {S::Offline, E::NetworkBack, S::Active},
    {S::Active, E::AuthNeeded, S::AuthRequired},
    {S::Offline, E::AuthNeeded, S::AuthRequired},
    {S::SyncPartial, E::AuthNeeded, S::AuthRequired},
    {S::Active, E::End, S::Ending},
    {S::Offline, E::End, S::Ending},
    {S::SyncPartial, E::End, S::Ending},
    {S::ReadyCached, E::End, S::Ending},
    {S::Ending, E::Ended, S::Ended},
    {S::Active, E::CooldownElapsed, S::SyncEligible},
};

} // namespace

bool SessionStateMachine::is_legal(SessionState from, SessionEvent_ ev, SessionState& to) {
    for (const auto& t : kTransitions) {
        if (t.from == from && t.ev == ev) {
            to = t.to;
            return true;
        }
    }
    return false;
}

bool SessionStateMachine::apply(SessionEvent_ ev) {
    SessionState next = state_;
    if (!is_legal(state_, ev, next)) return false;
    state_ = next;
    return true;
}

bool SessionStateMachine::can_process_mail(SessionState s) {
    switch (s) {
    case S::ReadyCached:
    case S::SyncEligible:
    case S::Syncing:
    case S::SyncPartial:
    case S::Active:
    case S::Offline:
        return true;
    default:
        return false;
    }
}

bool SessionStateMachine::can_attempt_sync(SessionState s) {
    switch (s) {
    case S::ReadyCached:
    case S::SyncEligible:
    case S::SyncPartial:
    case S::Active:
        return true;
    default:
        return false;
    }
}

std::string new_id(const std::string& prefix) {
    std::uint64_t v = 0;
    bool ok = false;
    if (std::FILE* f = std::fopen("/dev/urandom", "rb")) {
        ok = std::fread(&v, sizeof v, 1, f) == 1;
        std::fclose(f);
    }
    if (!ok) {
        std::random_device rd;
        v = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return prefix + "-" + buf;
}

} // namespace slowmail::core
