// Test environment for the contract tests: spawns tools/fake_graph/server.py, talks to its
// /__control/ API, and builds a real GraphBackend + SqliteStore + TriageSession stack against it.
#pragma once

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/triage.hpp"
#include "slowmail/graph/auth.hpp"
#include "slowmail/graph/graph_backend.hpp"
#include "slowmail/graph/http_transport.hpp"
#include "slowmail/storage/sqlite_store.hpp"

extern char** environ;

#ifndef SLOWMAIL_FAKE_GRAPH_SERVER
#error "SLOWMAIL_FAKE_GRAPH_SERVER must be defined by CMake"
#endif
#ifndef SLOWMAIL_FIXTURE_DIR
#error "SLOWMAIL_FIXTURE_DIR must be defined by CMake"
#endif

namespace slowmail::contract {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// Child process running server.py --port 0. Reads the "LISTENING <port>" line from its stdout.
// The child's stdin is a pipe we hold: if this test binary dies, the server sees EOF and exits.
// ---------------------------------------------------------------------------------------------
class FakeGraphProcess {
public:
    explicit FakeGraphProcess(const std::vector<std::string>& extra_args = {}) {
        int out_pipe[2], in_pipe[2];
        if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) throw std::runtime_error("pipe() failed");

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
        for (int fd : {out_pipe[0], out_pipe[1], in_pipe[0], in_pipe[1]}) posix_spawn_file_actions_addclose(&fa, fd);

        const char* py = std::getenv("SLOWMAIL_PYTHON");
        std::vector<std::string> args = {py && *py ? py : "python3", "-I", SLOWMAIL_FAKE_GRAPH_SERVER,
                                         "--port", "0", "--device-interval", "1", "--exit-on-stdin-eof"};
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        int rc = posix_spawnp(&pid_, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(out_pipe[1]);
        close(in_pipe[0]);
        stdin_fd_ = in_pipe[1];
        if (rc != 0) {
            close(out_pipe[0]);
            throw std::runtime_error("posix_spawnp(python3) failed: " + std::to_string(rc));
        }
        std::string line = read_line(out_pipe[0], 15000);
        close(out_pipe[0]);
        if (line.rfind("LISTENING ", 0) != 0) {
            stop();
            throw std::runtime_error("fake server did not print LISTENING; got: '" + line + "'");
        }
        port_ = std::stoi(line.substr(10));
    }

    ~FakeGraphProcess() { stop(); }
    FakeGraphProcess(const FakeGraphProcess&) = delete;
    FakeGraphProcess& operator=(const FakeGraphProcess&) = delete;

    int port() const { return port_; }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }   // identity authority
    std::string graph_base() const { return base() + "/v1.0"; }                          // GraphConfig::base_url

    bool running() const {
        if (pid_ <= 0) return false;
        int status = 0;
        return waitpid(pid_, &status, WNOHANG) == 0;
    }

    void stop() {
        if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
        if (pid_ > 0) {
            for (int i = 0; i < 50; ++i) {
                int status = 0;
                if (waitpid(pid_, &status, WNOHANG) != 0) { pid_ = -1; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

private:
    static std::string read_line(int fd, int timeout_ms) {
        std::string line;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 200) <= 0) continue;
            char c;
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            if (c == '\n') return line;
            line.push_back(c);
        }
        return line;
    }

    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int port_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Control API client (libcurl through the production CurlTransport).
// ---------------------------------------------------------------------------------------------
class Control {
public:
    explicit Control(std::string base) : base_(std::move(base)) {}

    json post(const std::string& name, const json& body = json::object()) {
        graph::HttpRequest r;
        r.method = "POST";
        r.url = base_ + "/__control/" + name;
        r.headers["Content-Type"] = "application/json";
        r.body = body.dump();
        return send(r, name);
    }
    json get(const std::string& name) {
        graph::HttpRequest r;
        r.method = "GET";
        r.url = base_ + "/__control/" + name;
        return send(r, name);
    }

    json state() { return get("state"); }
    json counters() { return state().at("counters"); }
    std::int64_t request_count() { return counters().at("requests").get<std::int64_t>(); }
    std::int64_t counter(const std::string& name) { return counters().at(name).get<std::int64_t>(); }
    std::int64_t path_count(const std::string& key) {
        const json by = counters().at("by_path");
        return by.contains(key) ? by.at(key).get<std::int64_t>() : 0;
    }
    json requests() { return get("requests"); }

    void load_fixture(const std::string& file_name) {
        auto r = post("load", {{"path", std::string(SLOWMAIL_FIXTURE_DIR) + "/" + file_name}});
        if (!r.value("ok", false)) throw std::runtime_error("fixture load failed: " + r.dump());
    }
    // Message ids currently in a server folder, newest first.
    std::vector<std::string> folder_ids(const std::string& folder) {
        std::vector<std::string> out;
        const json s = state();  // keep alive: ranging over a temporary's member would dangle
        for (auto& v : s.at("folders").at(folder)) out.push_back(v.get<std::string>());
        return out;
    }
    std::string parent_of(const std::string& id) {
        auto s = state();
        return s.at("messages").contains(id) ? s.at("messages").at(id).at("parentFolder").get<std::string>() : "";
    }
    std::string flag_of(const std::string& id) {
        auto s = state();
        return s.at("messages").contains(id) ? s.at("messages").at(id).at("flagStatus").get<std::string>() : "";
    }
    std::string mutable_id(const std::string& id) { return state().at("messages").at(id).at("mutableId"); }

private:
    json send(const graph::HttpRequest& req, const std::string& what) {
        auto out = transport_.send(req);
        if (!out.ok()) throw std::runtime_error("control " + what + " failed: " + out.error().message);
        if (out.value().status != 200) throw std::runtime_error("control " + what + " -> " + std::to_string(out.value().status) + " " + out.value().body);
        return json::parse(out.value().body);
    }
    std::string base_;
    graph::CurlTransport transport_;
};

// ---------------------------------------------------------------------------------------------
// One "app instance": store + auth + backend + TriageSession over a database file.
// Destroying it and creating another over the same path simulates an app restart / crash.
// ---------------------------------------------------------------------------------------------
struct StackOptions {
    std::string authority;                 // identity base (e.g. http://127.0.0.1:PORT)
    std::string graph_base;                // GraphConfig::base_url
    fs::path db_path;
    graph::GraphConfig graph;              // base_url is overwritten from graph_base
    core::TriageConfig triage;
};

class Stack {
public:
    explicit Stack(StackOptions opt) : options_(std::move(opt)) {
        options_.graph.base_url = options_.graph_base;
        transport_ = std::make_shared<graph::CurlTransport>();
        clock_ = std::make_shared<core::SystemClock>();

        // DeviceCodeAuth needs a Store&; TriageSession owns its own. Two connections to one WAL database.
        auth_store_ = std::make_unique<storage::SqliteStore>(options_.db_path.string());
        auto st = auth_store_->open();
        if (!st.ok()) throw std::runtime_error("auth store open failed: " + st.error().message);

        graph::AuthConfig ac;
        ac.authority = options_.authority;
        ac.tenant = "common";
        ac.client_id = "contract-test-client";
        auth_ = std::make_shared<graph::DeviceCodeAuth>(ac, transport_, *auth_store_, clock_);

        auto backend = std::make_unique<graph::GraphBackend>(options_.graph, transport_, auth_, clock_);
        backend_ = backend.get();
        auto store = std::make_unique<storage::SqliteStore>(options_.db_path.string());
        store_ = store.get();
        session_ = std::make_unique<core::TriageSession>(std::move(store), std::move(backend), clock_, options_.triage);
    }

    core::TriageSession& session() { return *session_; }
    graph::DeviceCodeAuth& auth() { return *auth_; }
    graph::GraphBackend& backend() { return *backend_; }
    core::Store& store() { return *store_; }
    core::Clock& clock() { return *clock_; }
    const StackOptions& options() const { return options_; }

    // A second, independent backend sharing this stack's auth (for direct backend-level assertions).
    std::unique_ptr<graph::GraphBackend> extra_backend(graph::GraphConfig cfg) {
        cfg.base_url = options_.graph_base;
        return std::make_unique<graph::GraphBackend>(cfg, transport_, auth_, clock_);
    }

private:
    StackOptions options_;
    std::shared_ptr<graph::CurlTransport> transport_;
    std::shared_ptr<core::SystemClock> clock_;
    std::unique_ptr<storage::SqliteStore> auth_store_;   // declared before auth_: destroyed after it
    std::shared_ptr<graph::DeviceCodeAuth> auth_;
    core::Store* store_ = nullptr;                       // owned by session_
    graph::GraphBackend* backend_ = nullptr;             // owned by session_
    std::unique_ptr<core::TriageSession> session_;
};

// Shared across the whole suite: one server process, reset between tests via /__control/reset.
class ContractTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        try {
            server_ = std::make_unique<FakeGraphProcess>();
        } catch (const std::exception& e) {
            server_.reset();
            startup_error_ = e.what();
        }
    }
    static void TearDownTestSuite() { server_.reset(); }

    void SetUp() override {
        ASSERT_TRUE(server_ != nullptr) << "could not start fake server: " << startup_error_;
        ASSERT_TRUE(server_->running()) << "fake server process died";
        ctl_ = std::make_unique<Control>(server_->base());
        ctl_->post("reset");
        char tmpl[] = "/tmp/slowmail-contract-XXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        tmp_ = tmpl;
    }
    void TearDown() override {
        stack_.reset();
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    Control& ctl() { return *ctl_; }
    FakeGraphProcess& server() { return *server_; }
    Stack& stack() { return *stack_; }

    StackOptions options() const {
        StackOptions o;
        o.authority = server_->base();
        o.graph_base = server_->graph_base();
        o.db_path = tmp_ / "slowmail.db";
        o.graph.page_size = 100;
        o.graph.sleep_on_retry = true;
        o.triage.cooldown.cooldown_seconds = 4 * 3600;
        return o;
    }

    void make_stack(std::optional<StackOptions> opt = std::nullopt) {
        stack_.reset();
        stack_ = std::make_unique<Stack>(opt ? *opt : options());
    }

    // Device-code sign-in with immediate approval through the control API.
    void sign_in() {
        auto challenge = stack_->auth().begin();
        ASSERT_TRUE(challenge.ok()) << challenge.error().message;
        ctl().post("approve_device", {{"user_code", challenge.value().user_code}});
        auto polled = stack_->auth().poll(challenge.value());
        ASSERT_TRUE(polled.ok()) << polled.error().message;
        ASSERT_EQ(polled.value(), graph::PollResult::Authorized);
        ASSERT_TRUE(stack_->auth().signed_in());
    }

    // make_stack + sign_in + start(): the common prologue.
    void ready_stack(std::optional<StackOptions> opt = std::nullopt) {
        make_stack(std::move(opt));
        ASSERT_NO_FATAL_FAILURE(sign_in());
        auto st = stack_->session().start();
        ASSERT_TRUE(st.ok()) << st.error().message;
    }

    // refresh() that folds an error Outcome into the report (partial syncs may surface either way).
    core::SyncReport sync(bool force = true, bool* outcome_ok = nullptr) {
        auto r = stack_->session().refresh(force);
        if (outcome_ok) *outcome_ok = r.ok();
        if (r.ok()) return r.value();
        core::SyncReport rep;
        rep.complete = false;
        rep.error = r.error();
        return rep;
    }

    std::set<std::string> queue_ids() {
        std::set<std::string> ids;
        for (auto& m : stack_->session().queue()) ids.insert(m.id);
        return ids;
    }

    // Every Graph call must carry Prefer: IdType="ImmutableId" when immutable ids are on (and never otherwise).
    void expect_prefer_immutable_on_graph_requests(bool expected) {
        for (auto& r : ctl().requests()) {
            const std::string path = r.at("path");
            if (path.rfind("/v1.0/", 0) != 0) continue;
            std::string prefer = r.at("headers").value("Prefer", "");
            EXPECT_EQ(prefer.find("ImmutableId") != std::string::npos, expected) << r.at("method") << " " << path;
        }
    }

    static inline std::unique_ptr<FakeGraphProcess> server_;
    static inline std::string startup_error_;
    std::unique_ptr<Control> ctl_;
    std::unique_ptr<Stack> stack_;
    fs::path tmp_;
};

}  // namespace slowmail::contract
