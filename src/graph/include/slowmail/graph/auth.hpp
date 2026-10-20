#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/store.hpp"
#include "slowmail/core/types.hpp"
#include "slowmail/graph/http_transport.hpp"

namespace slowmail::graph {

// Supplies a bearer token for Graph calls. GraphBackend only sees this interface.
class TokenProvider {
public:
    virtual ~TokenProvider() = default;
    // Returns a valid access token, refreshing silently if needed. ErrorKind::Auth => interactive sign-in required.
    virtual core::Outcome<std::string> access_token() = 0;
    // Called by the client on a 401 so the provider can drop a cached token and refresh once.
    virtual void invalidate() = 0;
};

struct AuthConfig {
    std::string authority = "https://login.microsoftonline.com";  // overridable for the fake server
    std::string tenant = "organizations";                          // tenant id or "organizations"
    std::string client_id;                                         // from env SLOWMAIL_CLIENT_ID; never committed
    std::vector<std::string> scopes = {"offline_access", "User.Read", "Mail.ReadWrite"};
    int http_timeout_seconds = 30;
};

struct DeviceCodeChallenge {
    std::string device_code;      // secret; never logged or displayed
    std::string user_code;        // shown to the user
    std::string verification_uri; // shown to the user
    std::string message;          // server-provided instruction text
    int expires_in = 0;
    int interval = 5;
};

enum class PollResult { Pending, SlowDown, Authorized, Declined, Expired, BadCode, Error };
const char* to_string(PollResult r);

// Persisted as an opaque JSON blob via Store::put_token_blob. Fields are the OAuth token response subset.
struct TokenSet {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0;   // unix seconds
    std::string scope;
    std::string account_hint;      // preferred_username from id token claims if present; sensitive

    std::string to_json() const;
    static std::optional<TokenSet> from_json(const std::string& s);
};

// OAuth 2.0 device authorization grant against the Microsoft identity platform (v2.0 endpoints).
//   POST {authority}/{tenant}/oauth2/v2.0/devicecode   client_id, scope
//   POST {authority}/{tenant}/oauth2/v2.0/token        grant_type=urn:ietf:params:oauth:grant-type:device_code
//   POST {authority}/{tenant}/oauth2/v2.0/token        grant_type=refresh_token
// Tokens are stored through the Store (token blob). Implements TokenProvider.
class DeviceCodeAuth final : public TokenProvider {
public:
    DeviceCodeAuth(AuthConfig config, std::shared_ptr<HttpTransport> transport, core::Store& store,
                   std::shared_ptr<core::Clock> clock);

    core::Outcome<DeviceCodeChallenge> begin();
    // One poll. The caller sleeps `interval` seconds between polls (and increases on SlowDown).
    core::Outcome<PollResult> poll(const DeviceCodeChallenge& challenge);

    bool signed_in();                       // a refresh token is stored
    core::Status sign_out();                // clears the stored token blob (does not revoke server-side)

    core::Outcome<std::string> access_token() override;
    void invalidate() override;

    const AuthConfig& config() const { return config_; }

private:
    AuthConfig config_;
    std::shared_ptr<HttpTransport> transport_;
    core::Store& store_;
    std::shared_ptr<core::Clock> clock_;
    std::optional<TokenSet> cached_;
    std::string scope_string() const;
    core::Outcome<TokenSet> refresh(const std::string& refresh_token);
};

// Static token for tests and the fake server.
class StaticTokenProvider final : public TokenProvider {
public:
    explicit StaticTokenProvider(std::string token) : token_(std::move(token)) {}
    core::Outcome<std::string> access_token() override { return token_; }
    void invalidate() override {}
private:
    std::string token_;
};

} // namespace slowmail::graph
