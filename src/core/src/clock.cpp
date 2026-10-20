#include "slowmail/core/clock.hpp"

#include <time.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

namespace slowmail::core {

std::int64_t SystemClock::now_wall() const {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
}

double SystemClock::now_mono() const {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        const auto d = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(d).count();
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

namespace {

std::string read_boot_id() {
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string line;
    if (f && std::getline(f, line)) {
        // Keep printable ASCII only, trim whitespace.
        std::string out;
        for (char c : line) {
            if (c > 0x20 && c < 0x7f) out.push_back(c);
        }
        if (!out.empty() && out.size() <= 128) return out;
    }
    std::random_device rd;
    std::uint64_t a = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    std::uint64_t b = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    char buf[48];
    std::snprintf(buf, sizeof buf, "proc-%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return buf;
}

// Days from civil date (Howard Hinnant's algorithm); proleptic Gregorian.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool read_digits(const std::string& s, std::size_t& pos, std::size_t count, int& out) {
    if (pos + count > s.size()) return false;
    int v = 0;
    for (std::size_t i = 0; i < count; ++i) {
        char c = s[pos + i];
        if (!is_digit(c)) return false;
        v = v * 10 + (c - '0');
    }
    pos += count;
    out = v;
    return true;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap(y)) ? 29 : dm[m - 1];
}

} // namespace

std::string SystemClock::boot_id() const {
    // Computed once per process; the fallback id must stay stable for the process lifetime.
    static const std::string id = read_boot_id();
    return id;
}

bool parse_iso8601_utc(const std::string& iso, std::int64_t& epoch_out) {
    // Accepted: YYYY-MM-DDTHH:MM:SS[.fraction](Z|+00:00)
    std::size_t p = 0;
    int Y, M, D, h, mi, s;
    if (!read_digits(iso, p, 4, Y)) return false;
    if (p >= iso.size() || iso[p++] != '-') return false;
    if (!read_digits(iso, p, 2, M)) return false;
    if (p >= iso.size() || iso[p++] != '-') return false;
    if (!read_digits(iso, p, 2, D)) return false;
    if (p >= iso.size() || iso[p++] != 'T') return false;
    if (!read_digits(iso, p, 2, h)) return false;
    if (p >= iso.size() || iso[p++] != ':') return false;
    if (!read_digits(iso, p, 2, mi)) return false;
    if (p >= iso.size() || iso[p++] != ':') return false;
    if (!read_digits(iso, p, 2, s)) return false;
    if (p < iso.size() && iso[p] == '.') {
        ++p;
        std::size_t start = p;
        while (p < iso.size() && is_digit(iso[p])) ++p;
        if (p == start) return false;
    }
    if (p >= iso.size()) return false;
    if (iso[p] == 'Z') {
        ++p;
    } else if (iso[p] == '+') {
        ++p;
        int oh, om;
        if (!read_digits(iso, p, 2, oh)) return false;
        if (p >= iso.size() || iso[p++] != ':') return false;
        if (!read_digits(iso, p, 2, om)) return false;
        if (oh != 0 || om != 0) return false; // only a zero offset is accepted
    } else {
        return false;
    }
    if (p != iso.size()) return false;

    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > days_in_month(Y, M)) return false;
    if (h > 23 || mi > 59 || s > 59) return false;

    const std::int64_t days = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D));
    epoch_out = days * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

std::string format_iso8601_utc(std::int64_t epoch) {
    std::int64_t days = epoch / 86400;
    std::int64_t rem = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    std::int64_t y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02u:%02u:%02uZ", static_cast<long long>(y), m, d,
                  static_cast<unsigned>(rem / 3600), static_cast<unsigned>((rem % 3600) / 60),
                  static_cast<unsigned>(rem % 60));
    return buf;
}

} // namespace slowmail::core
