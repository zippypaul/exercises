#include "slowmail/graph/auth.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "slowmail/core/log.hpp"
#include "slowmail/core/sanitize.hpp"
#include "url.hpp"

namespace slowmail::graph {

using nlohmann::json;

const char* to_string(PollResult r) {
    switch (r) {
    case PollResult::Pending: return "pending";
    case PollResult::SlowDown: return "slow_down";
    case PollResult::Authorized: return "authorized";
    case PollResult::Declined: return "declined";
    case PollResult::Expired: return "expired";
    case PollResult::BadCode: return "bad_code";
    case PollResult::Error: return "error";
    case PollResult::Blocked: return "blocked";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// TokenSet
// ---------------------------------------------------------------------------
std::string TokenSet::to_json() const {
    try {
        json j = {{"access_token", access_token},
                  {"refresh_token", refresh_token},
                  {"expires_at", expires_at},
                  {"scope", scope},
                  {"account_hint", account_hint}};
        return j.dump();
    } catch (...) {
        return "{}";
    }
}

std::optional<TokenSet> TokenSet::from_json(const std::string& s) {
    try {
        const json j = json::parse(s, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return std::nullopt;
        TokenSet t;
        auto str = [&](const char* k, std::string& out) {
            auto it = j.find(k);
            if (it != j.end() && it->is_string()) out = it->get<std::string>();
        };
        str("access_token", t.access_token);
        str("refresh_token", t.refresh_token);
        str("scope", t.scope);
        str("account_hint", t.account_hint);
        auto it = j.find("expires_at");
        if (it != j.end() && it->is_number_integer()) t.expires_at = it->get<std::int64_t>();
        return t;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {

constexpr std::int64_t kRefreshSkewSeconds = 300;  // contract: refresh proactively when < ~5 minutes remain
constexpr std::int64_t kMaxExpiresInSeconds = 86400LL * 365;  // a hostile/garbled expires_in must not overflow now + x

core::Error make_error(core::ErrorKind kind, std::string msg, int status = 0, std::string code = {}) {
    core::Error e;
    e.kind = kind;
    e.message = core::redact(msg);
    e.http_status = status;
    e.code = std::move(code);
    return e;
}

// OAuth error codes are short ASCII identifiers; anything else is dropped so hostile text cannot reach logs/UI.
std::string clean_code(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (url::is_unreserved(c)) out.push_back(c);
        if (out.size() >= 64) break;
    }
    return out;
}

std::optional<std::string> json_string(const json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

std::int64_t json_int(const json& j, const char* key, std::int64_t fallback) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number()) {
        const double d = it->get<double>();
        if (!(d > -1e15)) return -1'000'000'000'000'000;  // also catches NaN
        if (d > 1e15) return 1'000'000'000'000'000;
        return static_cast<std::int64_t>(d);
    }
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

std::string base64url_decode(const std::string& in) {
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = b64_value(c);
        if (v < 0) return {};
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Reads preferred_username from the id_token payload. NO signature verification is done: the token arrived over
// TLS directly from the token endpoint and is used only as a display hint, never for any authorisation decision.
std::string account_hint_from_id_token(const std::string& id_token) {
    try {
        const auto d1 = id_token.find('.');
        if (d1 == std::string::npos) return {};
        const auto d2 = id_token.find('.', d1 + 1);
        if (d2 == std::string::npos) return {};
        const std::string payload = base64url_decode(id_token.substr(d1 + 1, d2 - d1 - 1));
        if (payload.empty()) return {};
        const json j = json::parse(payload, nullptr, false);
        if (j.is_discarded()) return {};
        auto name = json_string(j, "preferred_username");
        return name ? core::sanitize_line(*name, core::Limits::kMaxAddress) : std::string();
    } catch (...) {
        return {};
    }
}

// Builds a TokenSet from a 200 token response. `previous` supplies the refresh token / hint when the response omits them.
core::Outcome<TokenSet> token_from_response(const std::string& body, std::int64_t now, const TokenSet* previous) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return make_error(core::ErrorKind::Malformed, "token response is not JSON");
    auto access = json_string(j, "access_token");
    if (!access || access->empty()) return make_error(core::ErrorKind::Malformed, "token response lacks access_token");
    TokenSet t;
    t.access_token = *access;
    auto rt = json_string(j, "refresh_token");
    if (rt && !rt->empty()) {
        t.refresh_token = *rt;
    } else if (previous) {
        t.refresh_token = previous->refresh_token;
    }
    t.expires_at = now + std::clamp<std::int64_t>(json_int(j, "expires_in", 3600), 0, kMaxExpiresInSeconds);
    t.scope = core::sanitize_line(json_string(j, "scope").value_or(previous ? previous->scope : std::string()), 1024);
    if (auto idt = json_string(j, "id_token"); idt && !idt->empty()) t.account_hint = account_hint_from_id_token(*idt);
    if (t.account_hint.empty() && previous) t.account_hint = previous->account_hint;
    return t;
}

struct OAuthError {
    std::string error;       // "error" field, cleaned
    bool blocked_by_policy = false;  // Conditional Access / flow block (AADSTS53xxxx)
    bool refresh_revoked = false;    // AADSTS530036 style flow-check revocation
    bool poll_blocked = false;       // token-endpoint policy/config block seen while polling (see is_poll_block)
    std::string aadsts;              // "AADSTS<digits>" taken from error_codes (or the description), never free text
};

bool is_poll_block_code(std::int64_t v) { return v == 53003 || v == 530032 || v == 7000218 || v == 700016; }

bool is_poll_block_error(const std::string& e) {
    return e == "blocked_by_policy" || e == "invalid_client" || e == "unauthorized_client" || e == "invalid_scope" ||
           e == "interaction_required";
}

// Extracts the first "AADSTS<digits>" token (digits only, at most 8) from a description. Only the code is kept.
std::string aadsts_from_description(const std::string& d) {
    const auto p = d.find("AADSTS");
    if (p == std::string::npos) return {};
    std::size_t i = p + 6;
    std::string digits;
    while (i < d.size() && d[i] >= '0' && d[i] <= '9' && digits.size() < 8) digits.push_back(d[i++]);
    return digits.empty() ? std::string() : "AADSTS" + digits;
}

OAuthError parse_oauth_error(const std::string& body) {
    OAuthError e;
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return e;
    if (auto s = json_string(j, "error")) e.error = clean_code(*s);
    if (is_poll_block_error(e.error)) e.poll_blocked = true;
    if (auto d = json_string(j, "error_description")) {
        e.aadsts = aadsts_from_description(*d);
        if (d->find("AADSTS530036") != std::string::npos) e.refresh_revoked = true;
        if (d->find("AADSTS53") != std::string::npos || d->find("AADSTS50005") != std::string::npos) {
            e.blocked_by_policy = true;
        }
    }
    auto codes = j.find("error_codes");
    if (codes != j.end() && codes->is_array()) {
        for (const auto& c : *codes) {
            if (c.is_number_integer()) {
                const auto v = c.get<std::int64_t>();
                if (v == 530036) e.refresh_revoked = true;
                if (v >= 530000 && v < 540000) e.blocked_by_policy = true;
                if (is_poll_block_code(v)) {
                    e.poll_blocked = true;
                    if (v > 0 && v < 100000000) e.aadsts = "AADSTS" + std::to_string(v);  // the numeric code wins
                }
            }
        }
    }
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// DeviceCodeAuth
// ---------------------------------------------------------------------------
DeviceCodeAuth::DeviceCodeAuth(AuthConfig config, std::shared_ptr<HttpTransport> transport, core::Store& store,
                               std::shared_ptr<core::Clock> clock)
    : config_(std::move(config)), transport_(std::move(transport)), store_(store), clock_(std::move(clock)) {}

std::string DeviceCodeAuth::scope_string() const {
    std::string out;
    for (const auto& s : config_.scopes) {
        if (s.empty()) continue;
        if (!out.empty()) out.push_back(' ');
        out += s;
    }
    return out;
}

namespace {
std::string endpoint(const AuthConfig& c, const char* leaf) {
    std::string a = c.authority;
    while (!a.empty() && a.back() == '/') a.pop_back();
    return a + "/" + url::percent_encode(c.tenant) + "/oauth2/v2.0/" + leaf;
}

core::Outcome<HttpResponse> post_form(HttpTransport& t, const AuthConfig& c, const std::string& u,
                                      const std::vector<std::pair<std::string, std::string>>& fields) {
    HttpRequest req;
    req.method = "POST";
    req.url = u;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Accept"] = "application/json";
    req.body = url::form_encode(fields);
    req.timeout_seconds = c.http_timeout_seconds;
    return t.send(req);
}
} // namespace

core::Outcome<DeviceCodeChallenge> DeviceCodeAuth::begin() {
    try {
        if (config_.client_id.empty()) {
            return make_error(core::ErrorKind::Invalid, "client id is not configured (SLOWMAIL_CLIENT_ID)");
        }
        auto r = post_form(*transport_, config_, endpoint(config_, "devicecode"),
                           {{"client_id", config_.client_id}, {"scope", scope_string()}});
        if (!r.ok()) return r.error();
        const HttpResponse& resp = r.value();
        if (resp.status != 200) {
            const OAuthError oe = parse_oauth_error(resp.body);
            if (oe.blocked_by_policy) {
                return make_error(core::ErrorKind::Forbidden, "device code sign-in is blocked by organisation policy",
                                  resp.status, !oe.aadsts.empty() ? oe.aadsts : oe.error);
            }
            if (resp.status >= 500) {
                return make_error(core::ErrorKind::Server, "device code endpoint unavailable", resp.status, oe.error);
            }
            return make_error(core::ErrorKind::Auth, "device code request rejected", resp.status, oe.error);
        }
        const json j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return make_error(core::ErrorKind::Malformed, "device code response is not JSON", resp.status);
        }
        DeviceCodeChallenge c;
        c.device_code = json_string(j, "device_code").value_or("");
        c.user_code = core::sanitize_line(json_string(j, "user_code").value_or(""), 64);
        c.verification_uri = core::sanitize_line(json_string(j, "verification_uri").value_or(""), 512);
        c.message = core::sanitize_line(json_string(j, "message").value_or(""), 1024);
        c.expires_in = static_cast<int>(std::clamp<std::int64_t>(json_int(j, "expires_in", 900), 0, kMaxExpiresInSeconds));
        c.interval = static_cast<int>(std::clamp<std::int64_t>(json_int(j, "interval", 5), 0, 3600));
        if (c.interval <= 0) c.interval = 5;
        if (c.device_code.empty() || c.user_code.empty() || c.verification_uri.empty()) {
            return make_error(core::ErrorKind::Malformed, "device code response is missing fields", resp.status);
        }
        return c;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "device code response could not be processed");
    }
}

core::Outcome<PollResult> DeviceCodeAuth::poll(const DeviceCodeChallenge& challenge) {
    try {
        auto r = post_form(*transport_, config_, endpoint(config_, "token"),
                           {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                            {"client_id", config_.client_id},
                            {"device_code", challenge.device_code}});
        if (!r.ok()) return r.error();
        const HttpResponse& resp = r.value();
        if (resp.status == 200) {
            auto ts = token_from_response(resp.body, clock_->now_wall(), nullptr);
            if (!ts.ok()) return ts.error();
            if (ts.value().refresh_token.empty()) {
                SLOWMAIL_LOG_WARN("token response carried no refresh token; offline_access scope missing?");
            }
            core::Status st = store_.put_token_blob(ts.value().to_json());
            if (!st.ok()) return st.error();
            cached_ = ts.value();
            return PollResult::Authorized;
        }
        if (resp.status == 429) {
            // Throttled while waiting for the user: keep polling (the caller sleeps at least `interval`); a 429 must
            // not abort a sign-in the user is still completing.
            SLOWMAIL_LOG_INFO("device code poll throttled (HTTP 429); will keep polling");
            return PollResult::Pending;
        }
        if (resp.status >= 500) {
            return make_error(core::ErrorKind::Server, "token endpoint unavailable", resp.status);
        }
        const OAuthError oe = parse_oauth_error(resp.body);
        if (oe.error == "authorization_pending") return PollResult::Pending;
        if (oe.error == "slow_down") return PollResult::SlowDown;  // RFC 8628; caller adds 5 s to its interval
        if (oe.error == "authorization_declined" || oe.error == "access_denied") return PollResult::Declined;
        if (oe.error == "expired_token") return PollResult::Expired;
        if (oe.error == "bad_verification_code") return PollResult::BadCode;
        if (oe.poll_blocked || oe.blocked_by_policy) {
            // Only the AADSTS code (or the short OAuth error identifier) is kept; never the description text.
            return make_error(core::ErrorKind::Forbidden, "sign-in is blocked by organisation policy or app configuration",
                              resp.status, !oe.aadsts.empty() ? oe.aadsts : oe.error);
        }
        SLOWMAIL_LOG_WARN("device code poll failed: http " + std::to_string(resp.status) + " code=" + oe.error);
        return PollResult::Error;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "token response could not be processed");
    }
}

bool DeviceCodeAuth::signed_in() {
    try {
        if (cached_ && !cached_->refresh_token.empty()) return true;
        auto blob = store_.get_token_blob();
        if (!blob) return false;
        auto ts = TokenSet::from_json(*blob);
        return ts && !ts->refresh_token.empty();
    } catch (...) {
        return false;
    }
}

core::Status DeviceCodeAuth::sign_out() {
    cached_.reset();
    try {
        return store_.clear_token_blob();
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Storage, "could not clear stored tokens");
    }
}

core::Outcome<TokenSet> DeviceCodeAuth::refresh(const std::string& refresh_token) {
    try {
        auto r = post_form(*transport_, config_, endpoint(config_, "token"),
                           {{"grant_type", "refresh_token"},
                            {"client_id", config_.client_id},
                            {"refresh_token", refresh_token},
                            {"scope", scope_string()}});
        if (!r.ok()) return r.error();
        const HttpResponse& resp = r.value();
        if (resp.status == 200) {
            auto ts = token_from_response(resp.body, clock_->now_wall(), cached_ ? &*cached_ : nullptr);
            if (!ts.ok()) return ts.error();
            if (ts.value().refresh_token.empty()) ts.value().refresh_token = refresh_token;
            // Persist the new (rotated) refresh token before anything else uses it.
            core::Status st = store_.put_token_blob(ts.value().to_json());
            if (!st.ok()) {
                SLOWMAIL_LOG_WARN("could not persist refreshed tokens; continuing with in-memory copy");
            }
            cached_ = ts.value();
            return ts.value();
        }
        if (resp.status >= 500) {
            return make_error(core::ErrorKind::Server, "token endpoint unavailable", resp.status);
        }
        const OAuthError oe = parse_oauth_error(resp.body);
        if (oe.error == "invalid_grant" || oe.error == "interaction_required" || oe.error == "consent_required" ||
            oe.refresh_revoked) {
            cached_.reset();
            store_.clear_token_blob();
            const std::string msg = oe.refresh_revoked ? "sign-in revoked by organisation policy; sign in again"
                                                       : "sign-in expired; sign in again";
            return make_error(core::ErrorKind::Auth, msg, resp.status, oe.error);
        }
        if (oe.blocked_by_policy) {
            return make_error(core::ErrorKind::Forbidden, "token refresh blocked by organisation policy", resp.status,
                              !oe.aadsts.empty() ? oe.aadsts : oe.error);
        }
        return make_error(core::ErrorKind::Auth, "token refresh rejected", resp.status, oe.error);
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "token response could not be processed");
    }
}

core::Outcome<std::string> DeviceCodeAuth::access_token() {
    try {
        if (!cached_) {
            auto blob = store_.get_token_blob();
            if (!blob) return make_error(core::ErrorKind::Auth, "sign-in required");
            auto ts = TokenSet::from_json(*blob);
            if (!ts) return make_error(core::ErrorKind::Auth, "stored sign-in data is unreadable; sign in again");
            cached_ = *ts;
        }
        const std::int64_t now = clock_->now_wall();
        if (!cached_->access_token.empty() && cached_->expires_at - kRefreshSkewSeconds > now) {
            return cached_->access_token;
        }
        if (cached_->refresh_token.empty()) return make_error(core::ErrorKind::Auth, "sign-in required");
        auto r = refresh(cached_->refresh_token);
        if (!r.ok()) return r.error();
        return r.value().access_token;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Auth, "token handling failed");
    }
}

void DeviceCodeAuth::invalidate() {
    try {
        if (!cached_) {
            auto blob = store_.get_token_blob();
            if (!blob) return;
            auto ts = TokenSet::from_json(*blob);
            if (!ts) return;
            cached_ = *ts;
        }
        cached_->access_token.clear();  // next access_token() is forced through a refresh
        cached_->expires_at = 0;
    } catch (...) {
    }
}

} // namespace slowmail::graph
