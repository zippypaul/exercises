#include "app_context.hpp"

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "slowmail/core/clock.hpp"
#include "slowmail/graph/graph_backend.hpp"
#include "slowmail/graph/http_transport.hpp"
#include "slowmail/platform/paths.hpp"
#include "slowmail/storage/sqlite_store.hpp"

namespace slowmail::app {

namespace {

std::string env_str(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

} // namespace

AppConfig AppConfig::from_environment() {
    AppConfig c;
    c.data_dir = platform::data_dir();
    c.client_id = env_str("SLOWMAIL_CLIENT_ID");
    const std::string tenant = env_str("SLOWMAIL_TENANT_ID");
    if (!tenant.empty()) c.tenant_id = tenant;
    const std::string base = env_str("SLOWMAIL_GRAPH_BASE_URL");
    if (!base.empty()) {
        c.graph_base_url = base;
        c.graph_base_url_overridden = true;
    }
    const std::string authority = env_str("SLOWMAIL_AUTHORITY");
    if (!authority.empty()) c.authority = authority;
    const std::string pro = env_str("SLOWMAIL_PRO");
    c.pro = (pro == "1" || pro == "true" || pro == "yes");
    const std::string cd = env_str("SLOWMAIL_COOLDOWN_SECONDS");
    if (!cd.empty()) {
        char* end = nullptr;
        const long long v = std::strtoll(cd.c_str(), &end, 10);
        if (end != cd.c_str() && *end == '\0' && v >= 0) c.cooldown_seconds = v;
    }
    c.ca_bundle = env_str("SLOWMAIL_CA_BUNDLE");
    return c;
}

std::string AppConfig::validate() const {
    if (client_id.empty() && !graph_base_url_overridden) {
        return "SLOWMAIL_CLIENT_ID is not set. Register an Entra ID application and export its client id "
               "(or set SLOWMAIL_GRAPH_BASE_URL to use a fake Graph server).";
    }
    if (data_dir.empty()) return "No data directory could be determined.";
    return {};
}

std::string AppConfig::describe() const {
    std::ostringstream o;
    o << "data_dir:        " << data_dir << "\n"
      << "client_id:       " << (client_id.empty() ? "(unset)" : "(set)") << "\n"
      << "tenant:          " << tenant_id << "\n"
      << "graph_base_url:  " << graph_base_url << (graph_base_url_overridden ? " (override)" : "") << "\n"
      << "authority:       " << authority << "\n"
      << "pro:             " << (pro ? "yes" : "no") << "\n"
      << "cooldown_seconds:" << ' ';
    if (cooldown_seconds < 0) o << "default"; else o << cooldown_seconds;
    o << "\n"
      << "ca_bundle:       " << (ca_bundle.empty() ? "(system default)" : ca_bundle) << "\n";
    return o.str();
}

std::unique_ptr<AppContext> AppContext::create(const AppConfig& config, std::string& error) {
    error.clear();
    const std::string problem = config.validate();
    if (!problem.empty()) {
        error = problem;
        return nullptr;
    }
    if (!platform::ensure_dir(config.data_dir, 0700)) {
        error = "Cannot create data directory: " + config.data_dir;
        return nullptr;
    }

    std::unique_ptr<AppContext> ctx(new AppContext());
    ctx->config_ = config;

    auto clock = std::make_shared<core::SystemClock>();

    auto store = std::make_unique<storage::SqliteStore>(config.data_dir + "/slowmail.sqlite3");

    auto transport = std::make_shared<graph::CurlTransport>();
    if (!config.ca_bundle.empty()) transport->set_ca_bundle(config.ca_bundle);

    graph::AuthConfig auth_cfg;
    auth_cfg.authority = config.authority;
    auth_cfg.tenant = config.tenant_id;
    auth_cfg.client_id = config.client_id.empty() ? std::string("slowmail-simulator") : config.client_id;
    // DeviceCodeAuth keeps a Store& to the SqliteStore which TriageSession is about to own.
    ctx->auth_ = std::make_shared<graph::DeviceCodeAuth>(auth_cfg, transport, *store, clock);

    graph::GraphConfig graph_cfg;
    graph_cfg.base_url = config.graph_base_url;
    auto backend = std::make_unique<graph::GraphBackend>(graph_cfg, transport, ctx->auth_, clock);

    core::TriageConfig triage_cfg;
    triage_cfg.cooldown.pro_entitled = config.pro;
    if (config.cooldown_seconds >= 0) triage_cfg.cooldown.cooldown_seconds = config.cooldown_seconds;

    ctx->session_ = std::make_unique<core::TriageSession>(std::move(store), std::move(backend), clock, triage_cfg);
    return ctx;
}

AppContext::~AppContext() {
    // Drop our extra handle first; the backend inside session_ then releases the last one before the store dies.
    auth_.reset();
    session_.reset();
}

core::Status AppContext::complete_sign_in() {
    // TriageSession::start() is re-entrant: from AuthRequired it re-checks the account via the backend
    // (a /me call with the freshly stored token) and moves the session on to ReadyCached.
    return session_->start();
}

core::Status AppContext::sign_out(bool wipe_cache) {
    if (auth_) {
        core::Status s = auth_->sign_out();
        if (!s.ok()) return s;
    }
    return session_->sign_out(wipe_cache);
}

std::string format_local_hhmm(std::int64_t epoch_seconds) {
    const std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm tm_buf {};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M", &tm_buf);
    return buf;
}

std::string format_local_datetime(std::int64_t epoch_seconds) {
    const std::time_t t = static_cast<std::time_t>(epoch_seconds);
    std::tm tm_buf {};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm_buf);
    return buf;
}

std::string format_size(std::int64_t bytes) {
    if (bytes < 0) return "unknown size";
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof buf, "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

} // namespace slowmail::app
