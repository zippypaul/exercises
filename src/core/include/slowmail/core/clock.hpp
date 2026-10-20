#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace slowmail::core {

// Two clocks are needed: wall (persisted across reboots, manipulable) and monotonic
// (trustworthy within a boot, resets on reboot; counts suspended time on SystemClock). boot_id lets us know if a stored
// monotonic reading is comparable with the current one.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t now_wall() const = 0;      // unix seconds UTC
    // Seconds since an arbitrary origin, never decreasing within a boot. On the real clock this INCLUDES time spent
    // suspended (CLOCK_BOOTTIME, falling back to CLOCK_MONOTONIC where unavailable), so cooldowns keep running
    // while the device sleeps.
    virtual double now_mono() const = 0;
    virtual std::string boot_id() const = 0;        // stable for the lifetime of this boot
};

class SystemClock final : public Clock {
public:
    std::int64_t now_wall() const override;
    double now_mono() const override;
    std::string boot_id() const override;           // /proc/sys/kernel/random/boot_id or process-start fallback
};

// Deterministic clock for tests. Fully controllable.
class FakeClock final : public Clock {
public:
    explicit FakeClock(std::int64_t wall = 1'800'000'000, double mono = 1000.0, std::string boot = "boot-1")
        : wall_(wall), mono_(mono), boot_(std::move(boot)) {}
    std::int64_t now_wall() const override { return wall_; }
    double now_mono() const override { return mono_; }
    std::string boot_id() const override { return boot_; }

    void advance(double seconds) { wall_ += static_cast<std::int64_t>(seconds); mono_ += seconds; }
    void set_wall(std::int64_t wall) { wall_ = wall; }         // simulate user changing the clock
    void reboot(const std::string& new_boot_id, double new_mono = 10.0) { boot_ = new_boot_id; mono_ = new_mono; }

private:
    std::int64_t wall_;
    double mono_;
    std::string boot_;
};

// ISO-8601 helpers (UTC, "Z" suffix). parse returns false on malformed input.
bool parse_iso8601_utc(const std::string& iso, std::int64_t& epoch_out);
std::string format_iso8601_utc(std::int64_t epoch);

} // namespace slowmail::core
