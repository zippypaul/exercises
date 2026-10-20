// Production object graph for Slow Mail, shared by the CLI and the Qt UI.
// Plain C++17: no Qt in this header or its .cpp.
//
// Lifetime contract
// -----------------
//   * core::TriageSession owns the storage::SqliteStore (unique_ptr<Store>) and the graph::GraphBackend.
//   * graph::DeviceCodeAuth holds a core::Store& (the same SqliteStore) and is also held, as a TokenProvider,
//     by GraphBackend. AppContext keeps one extra shared_ptr to it so the UI/CLI can drive sign-in.
//   * Therefore DeviceCodeAuth must never be used (or kept alive by callers) after the AppContext, and hence the
//     TriageSession, has been destroyed. AppContext declares its members so that destruction order is:
//     auth handle -> TriageSession (backend, then store). Do not copy `auth()` beyond the AppContext lifetime.
//   * Nothing here ever prints or stores a token; tokens live only in the Store token blob.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "slowmail/core/triage.hpp"
#include "slowmail/core/types.hpp"
#include "slowmail/graph/auth.hpp"

namespace slowmail::app {

struct AppConfig {
    std::string data_dir;                  // SLOWMAIL_DATA_DIR or platform default
    std::string client_id;                 // SLOWMAIL_CLIENT_ID (not printed by describe())
    std::string tenant_id = "organizations";                           // SLOWMAIL_TENANT_ID
    std::string graph_base_url = "https://graph.microsoft.com/v1.0";   // SLOWMAIL_GRAPH_BASE_URL
    std::string authority = "https://login.microsoftonline.com";       // SLOWMAIL_AUTHORITY
    bool graph_base_url_overridden = false; // true when SLOWMAIL_GRAPH_BASE_URL was set (fake server)
    bool pro = false;                      // SLOWMAIL_PRO=1
    std::int64_t cooldown_seconds = -1;    // SLOWMAIL_COOLDOWN_SECONDS; <0 means "use the default"
    std::string ca_bundle;                 // SLOWMAIL_CA_BUNDLE

    static AppConfig from_environment();

    // Empty string when the configuration is usable, otherwise a human readable problem.
    std::string validate() const;

    // Multi-line, secret-free description (client id shown only as set/unset).
    std::string describe() const;
};

class AppContext {
public:
    // Builds SqliteStore, CurlTransport, DeviceCodeAuth, GraphBackend, SystemClock and TriageSession.
    // Does not open the store and does not touch the network: call session().start() next.
    // Returns nullptr and fills `error` on invalid configuration or if the data dir cannot be created.
    static std::unique_ptr<AppContext> create(const AppConfig& config, std::string& error);

    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    core::TriageSession& session() { return *session_; }
    const std::shared_ptr<graph::DeviceCodeAuth>& auth() const { return auth_; }
    const AppConfig& config() const { return config_; }

    // After DeviceCodeAuth::poll() returned Authorized: re-enters TriageSession::start() so the session leaves
    // AuthRequired. Safe to call repeatedly.
    core::Status complete_sign_in();

    // Clears the stored token (auth), then asks the session to sign out (optionally wiping the cache).
    core::Status sign_out(bool wipe_cache);

private:
    AppContext() = default;

    AppConfig config_;
    // Declaration order matters: members are destroyed in reverse, so auth_ goes before session_.
    std::unique_ptr<core::TriageSession> session_;
    std::shared_ptr<graph::DeviceCodeAuth> auth_;
};

// ---- small formatting helpers shared by CLI and UI (no Qt) -----------------------------------------------
std::string format_local_hhmm(std::int64_t epoch_seconds);       // "14:05" in the local time zone
std::string format_local_datetime(std::int64_t epoch_seconds);   // "2026-10-20 14:05" in the local time zone
std::string format_size(std::int64_t bytes);                     // "12.3 KB", "unknown size" for < 0

} // namespace slowmail::app
