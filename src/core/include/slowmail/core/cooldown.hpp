#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/types.hpp"

namespace slowmail::core {

// Four-hour inbound refresh policy (handover §6).
//   can_refresh = first_download || pro_entitled || elapsed_since_success >= cooldown
// The user is eligible when EITHER clock says the cooldown has elapsed (fail open, never lock out):
//   * monotonic elapsed (same boot, readings present) >= cooldown. SystemClock's monotonic reading includes
//     suspended time (CLOCK_BOOTTIME), so a sleeping device does not stretch the cooldown.
//   * OR wall elapsed >= cooldown. A forward wall-clock change therefore bypasses the cooldown (accepted, D23;
//     it also covers a monotonic source that stops during suspend, and reboots).
// When the monotonic reading is unusable (other boot, missing, went backwards) only the wall clock decides, and
// a NEGATIVE wall elapsed (clock moved backwards) resolves to eligible ("clock_anomaly").
// With a usable monotonic reading a backwards wall clock does NOT unlock early: mono still has to elapse.
// Accepted MVP limitation (RISKS.md R-COOLDOWN-CLOCK); server time is not trusted here.
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
