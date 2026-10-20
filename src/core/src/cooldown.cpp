#include "slowmail/core/cooldown.hpp"

namespace slowmail::core {

CooldownDecision CooldownPolicy::evaluate(const InboxSyncState& state, const Clock& clock) const {
    CooldownDecision d;
    d.last_success_wall = state.last_complete_wall;

    if (!state.last_complete_wall) {
        d.eligible = true;
        d.reason = "first_download";
        return d;
    }
    if (config_.pro_entitled) {
        d.eligible = true;
        d.reason = "pro";
        return d;
    }

    const std::int64_t last_wall = *state.last_complete_wall;
    const std::int64_t cooldown = config_.cooldown_seconds;
    const std::int64_t next_wall = last_wall + cooldown;  // display value, even when mono decides

    const bool mono_usable = state.last_complete_mono.has_value() && state.last_complete_boot_id.has_value() &&
                             *state.last_complete_boot_id == clock.boot_id();
    if (mono_usable) {
        const double elapsed = clock.now_mono() - *state.last_complete_mono;
        if (elapsed >= 0.0) {
            if (elapsed >= static_cast<double>(cooldown)) {
                d.eligible = true;
                d.reason = "elapsed";
            } else {
                d.eligible = false;
                d.reason = "cooldown_active";
                d.next_eligible_wall = next_wall;
            }
            return d;
        }
        // A monotonic clock that went backwards is not trustworthy; fall through to wall time.
    }

    const std::int64_t wall_elapsed = clock.now_wall() - last_wall;
    if (wall_elapsed < 0) {
        d.eligible = true;
        d.reason = "clock_anomaly";
        return d;
    }
    if (wall_elapsed >= cooldown) {
        d.eligible = true;
        d.reason = "elapsed";
        return d;
    }
    d.eligible = false;
    d.reason = "cooldown_active";
    d.next_eligible_wall = next_wall;
    return d;
}

void CooldownPolicy::record_success(InboxSyncState& state, const Clock& clock) {
    const std::int64_t now = clock.now_wall();
    state.last_complete_wall = now;
    state.last_complete_mono = clock.now_mono();
    state.last_complete_boot_id = clock.boot_id();
    state.last_attempt_wall = now;
}

} // namespace slowmail::core
