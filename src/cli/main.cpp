// slowmail-cli: headless driver for the Slow Mail stack (offline simulator, scripted end-to-end checks).
// Plain text output. Never prints tokens or device codes. Exit codes:
//   0 success, 1 failure, 2 policy block (cooldown; next eligible time printed), 3 authentication required.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_context.hpp"
#include "slowmail/core/clock.hpp"

using namespace slowmail;
using nlohmann::json;

namespace {

constexpr int kOk = 0;
constexpr int kFail = 1;
constexpr int kPolicy = 2;
constexpr int kAuth = 3;

void usage(std::FILE* out) {
    std::fputs(
        "Usage: slowmail-cli <command> [options]\n"
        "\n"
        "Commands:\n"
        "  status                 Show session, cooldown and queue status\n"
        "  signin                 Device-code sign-in (prints code and URL, polls until done)\n"
        "  refresh [--force]      Send pending commands, then download the inbox (four-hour cooldown unless --force/pro)\n"
        "  queue [--json]         List the inbox processing queue\n"
        "  open <id>              Show sanitised body and attachment metadata\n"
        "  archive <id>           Do / Archive\n"
        "  delete <id>            Delete (move to Deleted Items)\n"
        "  defer <id>             Defer (flag, hide from queue)\n"
        "  undefer <id>           Restore a deferred message to the queue\n"
        "  undo <command-id>      Undo a confirmed archive/delete\n"
        "  flush                  Push pending outgoing commands now\n"
        "  failed [--json]        List messages whose last action failed, with the reason\n"
        "  retry <id>             Re-run the action that failed for a message\n"
        "  activity [--json]      Session activity\n"
        "  end                    End the session\n"
        "  signout [--wipe]       Sign out (optionally wipe the local cache)\n"
        "\n"
        "Environment: SLOWMAIL_DATA_DIR, SLOWMAIL_CLIENT_ID, SLOWMAIL_TENANT_ID, SLOWMAIL_GRAPH_BASE_URL,\n"
        "             SLOWMAIL_AUTHORITY, SLOWMAIL_PRO, SLOWMAIL_COOLDOWN_SECONDS, SLOWMAIL_CA_BUNDLE\n"
        "Exit codes: 0 ok, 1 error, 2 blocked by cooldown policy, 3 sign-in required\n",
        out);
}

// Set once the session exists; lets exit_code_for() recognise "refused because sign-in is required".
core::TriageSession* g_session = nullptr;

int exit_code_for(const core::Error& e) {
    if (e.kind == core::ErrorKind::Invalid && g_session != nullptr &&
        g_session->status().session_state == core::SessionState::AuthRequired) {
        return kAuth;  // the facade refuses actions/refresh in AuthRequired with an Invalid error
    }
    switch (e.kind) {
        case core::ErrorKind::Policy: return kPolicy;
        case core::ErrorKind::Auth: return kAuth;
        default: return kFail;
    }
}

void print_error(const core::Error& e) {
    std::fprintf(stderr, "error: %s%s%s\n", core::to_string(e.kind), e.message.empty() ? "" : ": ", e.message.c_str());
    if (exit_code_for(e) == kAuth) std::fprintf(stderr, "sign-in required: run `slowmail-cli signin`\n");
}

// A tenant policy (Conditional Access, app registration, consent) refused the sign-in. The AADSTS code, when the
// adapter supplied one in the expected shape, is shown to help the admin.
void print_sign_in_blocked(const core::Error& e) {
    std::string code = e.code;
    bool ok = code.rfind("AADSTS", 0) == 0 && code.size() > 6 && code.size() <= 14;
    for (std::size_t i = 6; ok && i < code.size(); ++i) ok = code[i] >= '0' && code[i] <= '9';
    std::fprintf(stderr,
                 "Your organisation blocks this sign-in method%s%s%s. Ask your admin; there is no workaround in Slow Mail.\n",
                 ok ? " (" : "", ok ? code.c_str() : "", ok ? ")" : "");
}

void print_next_eligible(core::TriageSession& s) {
    const auto st = s.status();
    if (st.cooldown.next_eligible_wall) {
        const std::int64_t t = *st.cooldown.next_eligible_wall;
        std::printf("next eligible refresh: %s local (%s)\n", app::format_local_datetime(t).c_str(),
                    core::format_iso8601_utc(t).c_str());
    }
}

bool has_flag(const std::vector<std::string>& args, const char* flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}

// First non-option argument.
std::string positional(const std::vector<std::string>& args) {
    for (const auto& a : args) if (a.rfind("--", 0) != 0) return a;
    return {};
}

std::string one_line(std::string s) {
    for (auto& c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

// ---------------------------------------------------------------------------------------------------------
int cmd_status(app::AppContext& ctx) {
    auto& s = ctx.session();
    const auto st = s.status();
    std::printf("session_state: %s\n", core::to_string(st.session_state));
    std::printf("session_id: %s\n", st.session_id.empty() ? "(none)" : st.session_id.c_str());
    std::printf("signed_in: %s\n", st.signed_in ? "yes" : "no");
    std::printf("offline: %s\n", st.offline ? "yes" : "no");
    std::printf("queue: %zu\n", st.queue_size);
    std::printf("pending: %zu\n", st.pending_commands);
    std::printf("deferred: %zu\n", st.deferred_count);
    std::printf("failed: %zu\n", st.failed_count);
    if (st.cooldown.last_success_wall) {
        std::printf("downloaded_at: %s local (%s)\n", app::format_local_hhmm(*st.cooldown.last_success_wall).c_str(),
                    core::format_iso8601_utc(*st.cooldown.last_success_wall).c_str());
    } else {
        std::printf("downloaded_at: never\n");
    }
    std::printf("refresh_eligible: %s (%s)\n", st.cooldown.eligible ? "yes" : "no", st.cooldown.reason.c_str());
    if (!st.cooldown.eligible) print_next_eligible(s);
    if (!st.last_error.empty()) std::printf("last_error: %s\n", one_line(st.last_error).c_str());
    return kOk;
}

int cmd_signin(app::AppContext& ctx) {
    auto auth = ctx.auth();
    auto begin = auth->begin();
    if (!begin.ok()) {
        if (begin.error().kind == core::ErrorKind::Forbidden) {
            print_sign_in_blocked(begin.error());
            return kFail;
        }
        print_error(begin.error());
        return kFail;
    }
    const graph::DeviceCodeChallenge ch = begin.value();
    std::printf("To sign in, open: %s\n", ch.verification_uri.c_str());
    std::printf("and enter the code: %s\n", ch.user_code.c_str());
    std::printf("(code expires in %d seconds; waiting...)\n", ch.expires_in);
    std::fflush(stdout);

    int interval = ch.interval > 0 ? ch.interval : 5;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(ch.expires_in > 0 ? ch.expires_in : 900);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        auto r = auth->poll(ch);
        if (!r.ok()) {
            if (r.error().kind == core::ErrorKind::Forbidden) {
                print_sign_in_blocked(r.error());
                return kFail;
            }
            print_error(r.error());
            return kFail;
        }
        switch (r.value()) {
            case graph::PollResult::Pending:
                break;
            case graph::PollResult::SlowDown:
                interval += 5;
                break;
            case graph::PollResult::Authorized: {
                const core::Status st = ctx.complete_sign_in();
                if (!st.ok()) {
                    print_error(st.error());
                    return kFail;
                }
                std::printf("signed in\n");
                return kOk;
            }
            case graph::PollResult::Declined:
                std::fprintf(stderr, "sign-in declined\n");
                return kFail;
            case graph::PollResult::Expired:
                std::fprintf(stderr, "sign-in code expired\n");
                return kFail;
            case graph::PollResult::BadCode:
                std::fprintf(stderr, "sign-in code rejected\n");
                return kFail;
            case graph::PollResult::Blocked:
                print_sign_in_blocked(core::Error{});
                return kFail;
            case graph::PollResult::Error:
                std::fprintf(stderr, "sign-in failed\n");
                return kFail;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "sign-in code expired\n");
            return kFail;
        }
    }
}

int cmd_refresh(app::AppContext& ctx, const std::vector<std::string>& args) {
    auto& s = ctx.session();
    const bool force = has_flag(args, "--force");
    // refresh() sends pending commands first (independent of the cooldown); say so when there was something to send.
    const auto pending_before = s.status().pending_commands;
    if (pending_before > 0) std::printf("pending commands: %zu (sending before the download)\n", pending_before);
    auto r = s.refresh(force);
    if (pending_before > 0) std::printf("pending after send: %zu\n", s.status().pending_commands);
    if (!r.ok()) {
        const core::Error& e = r.error();
        if (e.kind == core::ErrorKind::Policy) {
            std::printf("refresh blocked: %s\n", e.message.c_str());
            print_next_eligible(s);
            return kPolicy;
        }
        print_error(e);
        return exit_code_for(e);
    }
    const core::SyncReport& rep = r.value();
    std::printf("refresh %s: pages=%zu added=%zu updated=%zu removed=%zu%s%s\n", rep.complete ? "complete" : "incomplete",
                rep.pages, rep.added, rep.updated, rep.removed, rep.resumed ? " resumed" : "",
                rep.full_resync ? " full_resync" : "");
    if (rep.reconciled_pending) std::printf("reconciled_pending: %zu\n", rep.reconciled_pending);
    if (rep.restored_to_queue) std::printf("restored_to_queue: %zu\n", rep.restored_to_queue);
    if (!rep.complete) {
        print_error(rep.error);
        return exit_code_for(rep.error);
    }
    std::printf("queue: %zu\n", s.status().queue_size);
    return kOk;
}

int cmd_queue(app::AppContext& ctx, const std::vector<std::string>& args) {
    const auto q = ctx.session().queue();
    if (has_flag(args, "--json")) {
        json arr = json::array();
        for (const auto& m : q) {
            arr.push_back({{"id", m.id},
                           {"subject", m.subject},
                           {"from_name", m.from.name},
                           {"from_address", m.from.address},
                           {"received_at", m.received_at_iso},
                           {"preview", m.body_preview},
                           {"has_attachments", m.has_attachments},
                           {"is_read", m.is_read},
                           {"flagged", m.flag_status == core::FlagStatus::Flagged}});
        }
        std::printf("%s\n", arr.dump(2).c_str());
        return kOk;
    }
    if (q.empty()) {
        std::printf("queue is empty\n");
        return kOk;
    }
    for (const auto& m : q) {
        const std::string from = m.from.name.empty() ? m.from.address : m.from.name;
        std::printf("%s\t%s\t%s\t%s%s\n", m.id.c_str(), m.received_at_iso.c_str(), one_line(from).c_str(),
                    one_line(m.subject).c_str(), m.has_attachments ? "\t[attachments]" : "");
    }
    std::printf("%zu message(s)\n", q.size());
    return kOk;
}

int cmd_open(app::AppContext& ctx, const std::string& id) {
    auto& s = ctx.session();
    auto r = s.open(id);
    if (!r.ok()) {
        print_error(r.error());
        return exit_code_for(r.error());
    }
    if (auto m = s.message(id)) {
        const std::string from = m->from.name.empty() ? m->from.address
                                                       : m->from.name + " <" + m->from.address + ">";
        std::printf("From:    %s\n", one_line(from).c_str());
        std::printf("Subject: %s\n", one_line(m->subject).c_str());
        std::printf("Time:    %s\n", m->received_at_iso.c_str());
    }
    const core::MessageBody& b = r.value();
    std::printf("\n%s\n", b.text.c_str());
    if (b.truncated) std::printf("\n[message truncated]\n");
    std::printf("\nAttachments (%zu):\n", b.attachments.size());
    for (const auto& a : b.attachments) {
        std::printf("  %s \xE2\x80\x94 %s \xE2\x80\x94 %s%s\n", one_line(a.name).c_str(),
                    a.content_type.empty() ? core::to_string(a.kind) : one_line(a.content_type).c_str(),
                    app::format_size(a.size).c_str(), a.is_inline ? " (inline)" : "");
    }
    std::printf("Attachments are listed only; files are not downloaded.\n");
    return kOk;
}

int report_command(const core::Outcome<core::Command>& r) {
    if (!r.ok()) {
        print_error(r.error());
        return exit_code_for(r.error());
    }
    const core::Command& c = r.value();
    std::printf("command: %s\ntype: %s\nstate: %s\n", c.id.c_str(), core::to_string(c.type), core::to_string(c.state));
    if (!c.last_error.empty()) std::printf("last_error: %s\n", one_line(c.last_error).c_str());
    if (c.state == core::CommandState::Failed) return kFail;
    if (c.state == core::CommandState::Pending || c.state == core::CommandState::InFlight) {
        std::printf("(queued; will be sent on the next flush)\n");
    }
    return kOk;
}

int cmd_flush(app::AppContext& ctx) {
    const core::ProcessReport r = ctx.session().flush();
    std::printf("flush: confirmed=%zu failed=%zu abandoned=%zu pending=%zu\n", r.confirmed, r.failed, r.abandoned,
                r.deferred_retry);
    if (r.retry_after_seconds) std::printf("retry_after_seconds: %d\n", *r.retry_after_seconds);
    if (r.auth_required) {
        std::fprintf(stderr, "error: sign-in required\n");
        return kAuth;
    }
    if (r.went_offline) {
        std::printf("offline: commands remain pending\n");
        return kFail;
    }
    if (r.failed > 0 || r.deferred_retry > 0) return kFail;
    return kOk;
}

int cmd_failed(app::AppContext& ctx, const std::vector<std::string>& args) {
    auto& s = ctx.session();
    const auto rows = s.failed();
    if (has_flag(args, "--json")) {
        json arr = json::array();
        for (const auto& m : rows) {
            const auto c = s.failed_command(m.id);
            arr.push_back({{"id", m.id},
                           {"subject", m.subject},
                           {"from_name", m.from.name},
                           {"from_address", m.from.address},
                           {"action", c ? core::to_string(c->type) : ""},
                           {"reason", s.failure_reason(m.id)}});
        }
        std::printf("%s\n", arr.dump(2).c_str());
        return kOk;
    }
    if (rows.empty()) {
        std::printf("no failed messages\n");
        return kOk;
    }
    for (const auto& m : rows) {
        const auto c = s.failed_command(m.id);
        const std::string from = m.from.name.empty() ? m.from.address : m.from.name;
        std::printf("%s\t%s\t%s\t%s\treason: %s\n", m.id.c_str(), c ? core::to_string(c->type) : "?",
                    one_line(from).c_str(), one_line(m.subject).c_str(), one_line(s.failure_reason(m.id)).c_str());
    }
    std::printf("%zu failed message(s); `slowmail-cli retry <id>` re-runs the action\n", rows.size());
    return kOk;
}

int cmd_activity(app::AppContext& ctx, const std::vector<std::string>& args) {
    const auto rows = ctx.session().activity();
    if (has_flag(args, "--json")) {
        json arr = json::array();
        for (const auto& r : rows) {
            arr.push_back({{"id", r.event.id},
                           {"type", core::to_string(r.event.type)},
                           {"message_id", r.event.message_id},
                           {"command_id", r.event.command_id},
                           {"time", core::format_iso8601_utc(r.event.timestamp)},
                           {"confirmation", core::to_string(r.event.confirmation)},
                           {"subject", r.subject},
                           {"from_name", r.from_name},
                           {"from_address", r.from_address},
                           {"failure_reason", r.event.failure_reason}});
        }
        std::printf("%s\n", arr.dump(2).c_str());
        return kOk;
    }
    if (rows.empty()) {
        std::printf("no activity in this session\n");
        return kOk;
    }
    for (const auto& r : rows) {
        const std::string from = r.from_name.empty() ? r.from_address : r.from_name;
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s", app::format_local_hhmm(r.event.timestamp).c_str(),
                    core::to_string(r.event.type), core::to_string(r.event.confirmation),
                    r.event.command_id.c_str(), r.event.message_id.c_str(), one_line(from).c_str(),
                    one_line(r.subject).c_str());
        if (!r.event.failure_reason.empty()) std::printf("\treason: %s", one_line(r.event.failure_reason).c_str());
        std::printf("\n");
    }
    return kOk;
}

int cmd_end(app::AppContext& ctx) {
    const core::Status st = ctx.session().end();
    if (!st.ok()) {
        print_error(st.error());
        return exit_code_for(st.error());
    }
    std::printf("session ended\n");
    return kOk;
}

int cmd_signout(app::AppContext& ctx, const std::vector<std::string>& args) {
    const bool wipe = has_flag(args, "--wipe");
    const core::Status st = ctx.sign_out(wipe);
    if (!st.ok()) {
        print_error(st.error());
        return exit_code_for(st.error());
    }
    std::printf("signed out%s\n", wipe ? " (local cache wiped)" : "");
    return kOk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(stderr);
        return kFail;
    }
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage(stdout);
        return kOk;
    }
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

    static const char* const kKnown[] = {"status", "signin", "refresh", "queue", "open", "archive", "delete",
                                         "defer", "undefer", "undo", "flush", "failed", "retry", "activity", "end", "signout"};
    bool known = false;
    for (const char* k : kKnown) if (cmd == k) known = true;
    if (!known) {
        std::fprintf(stderr, "unknown command: %s\n\n", cmd.c_str());
        usage(stderr);
        return kFail;
    }
    const bool needs_id = cmd == "open" || cmd == "archive" || cmd == "delete" || cmd == "defer" ||
                          cmd == "undefer" || cmd == "undo" || cmd == "retry";
    const std::string id = positional(args);
    if (needs_id && id.empty()) {
        std::fprintf(stderr, "%s: missing %s argument\n", cmd.c_str(), cmd == "undo" ? "<command-id>" : "<id>");
        return kFail;
    }

    std::string error;
    auto ctx = app::AppContext::create(app::AppConfig::from_environment(), error);
    if (!ctx) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return kFail;
    }
    g_session = &ctx->session();
    const core::Status started = ctx->session().start();
    if (!started.ok()) {
        print_error(started.error());
        return exit_code_for(started.error());
    }

    int rc = kFail;
    if (cmd == "status") rc = cmd_status(*ctx);
    else if (cmd == "signin") rc = cmd_signin(*ctx);
    else if (cmd == "refresh") rc = cmd_refresh(*ctx, args);
    else if (cmd == "queue") rc = cmd_queue(*ctx, args);
    else if (cmd == "open") rc = cmd_open(*ctx, id);
    else if (cmd == "archive") rc = report_command(ctx->session().archive(id));
    else if (cmd == "delete") rc = report_command(ctx->session().remove(id));
    else if (cmd == "defer") rc = report_command(ctx->session().defer(id));
    else if (cmd == "undefer") rc = report_command(ctx->session().undefer(id));
    else if (cmd == "undo") rc = report_command(ctx->session().undo(id));
    else if (cmd == "flush") rc = cmd_flush(*ctx);
    else if (cmd == "failed") rc = cmd_failed(*ctx, args);
    else if (cmd == "retry") rc = report_command(ctx->session().retry(id));
    else if (cmd == "activity") rc = cmd_activity(*ctx, args);
    else if (cmd == "end") rc = cmd_end(*ctx);
    else if (cmd == "signout") rc = cmd_signout(*ctx, args);
    g_session = nullptr;
    return rc;
}
