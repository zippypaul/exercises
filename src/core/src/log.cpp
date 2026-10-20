#include "slowmail/core/log.hpp"

#include <cstdio>
#include <mutex>

namespace slowmail::core {

namespace {

std::mutex& sink_mutex() {
    static std::mutex m;
    return m;
}

LogSink& sink_storage() {
    static LogSink s;
    return s;
}

const char* level_name(LogLevel l) {
    switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

void default_sink(LogLevel level, const std::string& msg) {
    std::fprintf(stderr, "[slowmail] %s %s\n", level_name(level), msg.c_str());
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_b64url(char c) { return is_alnum(c) || c == '-' || c == '_'; }
bool is_local_char(char c) { return is_alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
bool is_domain_char(char c) { return is_alnum(c) || c == '.' || c == '-'; }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool starts_with_ci(const std::string& s, std::size_t pos, const char* lit) {
    std::size_t i = 0;
    for (; lit[i]; ++i) {
        if (pos + i >= s.size() || lower(s[pos + i]) != lit[i]) return false;
    }
    return true;
}

// Mask the value following key names such as access_token=..., "refresh_token":"...".
std::string redact_keyed(const std::string& s) {
    static const char* const kKeys[] = {"access_token", "refresh_token", "device_code", "id_token"};
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const char* matched = nullptr;
        for (const char* k : kKeys) {
            if (starts_with_ci(s, i, k)) {
                matched = k;
                break;
            }
        }
        if (!matched) {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t klen = 0;
        while (matched[klen]) ++klen;
        std::size_t j = i + klen;
        out.append(s, i, klen);
        // Optional closing quote, spaces, then a mandatory ':' or '='.
        std::size_t sep_start = j;
        while (j < s.size() && (s[j] == '"' || s[j] == '\'' || is_space(s[j]))) ++j;
        if (j >= s.size() || (s[j] != ':' && s[j] != '=')) {
            i = sep_start;
            continue;
        }
        ++j;
        while (j < s.size() && (s[j] == '"' || s[j] == '\'' || is_space(s[j]))) ++j;
        out.append(s, sep_start, j - sep_start);
        std::size_t v = j;
        while (v < s.size() && !is_space(s[v]) && s[v] != '"' && s[v] != '\'' && s[v] != '&' && s[v] != ',' &&
               s[v] != '}' && s[v] != ';')
            ++v;
        if (v > j) out += "[REDACTED]";
        i = v;
    }
    return out;
}

std::string redact_bearer(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const bool boundary = (i == 0) || !is_alnum(s[i - 1]);
        if (boundary && starts_with_ci(s, i, "bearer") && i + 6 < s.size() && is_space(s[i + 6])) {
            std::size_t j = i + 6;
            while (j < s.size() && is_space(s[j])) ++j;
            std::size_t v = j;
            while (v < s.size() && (is_alnum(s[v]) || s[v] == '.' || s[v] == '_' || s[v] == '~' || s[v] == '+' ||
                                    s[v] == '/' || s[v] == '=' || s[v] == '-'))
                ++v;
            if (v > j) {
                out += "Bearer [REDACTED]";
                i = v;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string redact_jwt(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (!is_b64url(s[i])) {
            out.push_back(s[i++]);
            continue;
        }
        // Take a maximal run of base64url characters and dots.
        std::size_t j = i;
        while (j < s.size() && (is_b64url(s[j]) || s[j] == '.')) ++j;
        while (j > i && s[j - 1] == '.') --j;  // trailing sentence punctuation is not part of a token
        // Split into segments; JWT-looking = exactly three, each >= 8 chars.
        std::size_t segs = 0;
        bool ok = true;
        std::size_t seg_start = i;
        for (std::size_t k = i; k <= j; ++k) {
            if (k == j || s[k] == '.') {
                if (k - seg_start < 8) ok = false;
                ++segs;
                seg_start = k + 1;
            }
        }
        if (ok && segs == 3) {
            out += "[REDACTED_JWT]";
        } else {
            out.append(s, i, j - i);
        }
        i = j;
    }
    return out;
}

std::string redact_email(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;  // everything before pos has been emitted
    for (std::size_t at = 0; at < s.size(); ++at) {
        if (s[at] != '@' || at < pos) continue;
        std::size_t ls = at;
        while (ls > pos && is_local_char(s[ls - 1])) --ls;
        std::size_t de = at + 1;
        while (de < s.size() && is_domain_char(s[de])) ++de;
        while (de > at + 1 && (s[de - 1] == '.' || s[de - 1] == '-')) --de;
        if (ls == at || de == at + 1) continue;
        out.append(s, pos, ls - pos);
        out.push_back(s[ls]);
        out += "***@***";
        std::size_t dot = std::string::npos;
        for (std::size_t k = de; k > at + 1; --k) {
            if (s[k - 1] == '.') {
                dot = k - 1;
                break;
            }
        }
        if (dot != std::string::npos && dot + 1 < de) out.append(s, dot, de - dot);
        pos = de;
        at = de - 1;
    }
    out.append(s, pos, std::string::npos);
    return out;
}

} // namespace

std::string redact(const std::string& s) {
    return redact_jwt(redact_email(redact_bearer(redact_keyed(s))));
}

void set_log_sink(LogSink sink) {
    std::lock_guard<std::mutex> lock(sink_mutex());
    sink_storage() = std::move(sink);
}

void log(LogLevel level, const std::string& message) {
    const std::string safe = redact(message);
    LogSink sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex());
        sink = sink_storage();
    }
    if (sink) {
        sink(level, safe);
    } else {
        default_sink(level, safe);
    }
}

} // namespace slowmail::core
