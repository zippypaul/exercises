#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/types.hpp"

namespace slowmail::core {

// Four-hour inbound refresh policy (handover §6).
//   can_refresh = first_download || pro_entitled || elapsed_since_success >= cooldown
// elapsed is computed defensively:
//   * if boot_id matches and mono readings are present -> use monotonic elapsed (clock-change proof)
//   * else use wall elapsed; a NEGATIVE wall elapsed (clock moved backwards) is treated as
//     "unknown" and, to avoid permanently locking the user, resolves to eligible once
//     the wall clock is at least `cooldown` past the EARLIER of (last_complete_wall, now).
//     In practice: backwards clock change => eligible. Forward clock change => eligible early.
//     This is an accepted MVP limitation (RISKS.md R-COOLDOWN-CLOCK); server time is not trusted here.
//   * failed or partial syncs never update last_complete_* and so never consume the cooldown.
struct CooldownConfig {
    std::int64_t cooldown_seconds = 4 * 60 * 60;
    bool pro_entitled = false;   // Pro concept: unlimited manual refresh. Behind this flag, no payment code.
};

struct CooldownDecision {
    bool eligible = false;
    std::string reason;                          // "first_download", "pro", "elapsed", "cooldown_active", "clock_anomaly"
    std::optional<std::int64_t> next_eligible_wall;  // for display: "next refresh at HH:MM"; nullopt when eligible
    std::optional<std::int64_t> last_success_wall;   // for display: "Downloaded at HH:MM"
};

class CooldownPolicy {
public:
    explicit CooldownPolicy(CooldownConfig config = {}) : config_(config) {}

    CooldownDecision evaluate(const InboxSyncState& state, const Clock& clock) const;

    // Called by the sync engine after a COMPLETE sync; returns the updated state fields.
    static void record_success(InboxSyncState& state, const Clock& clock);

    const CooldownConfig& config() const { return config_; }
    void set_pro_entitled(bool v) { config_.pro_entitled = v; }

private:
    CooldownConfig config_;
};

} // namespace slowmail::core
