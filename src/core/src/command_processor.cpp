#include "slowmail/core/command_processor.hpp"

#include <algorithm>

#include "engine_util.hpp"
#include "slowmail/core/log.hpp"
#include "slowmail/core/session.hpp"

namespace slowmail::core {

namespace {

using detail::make_status;
using nlohmann::json;

constexpr const char* kGoneReason = "message gone on server before action ran";
constexpr int kMaxRetryAfterSeconds = 3600;

json parse_object(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return json::object();
    return j;
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::int64_t not_before_of(const Command& c) {
    const json j = parse_object(c.payload_json);
    auto it = j.find("not_before");
    if (it != j.end() && it->is_number_integer()) return it->get<std::int64_t>();
    return 0;
}

EventType event_type_for(CommandType t) {
    switch (t) {
    case CommandType::Archive: return EventType::Archived;
    case CommandType::Delete: return EventType::Deleted;
    case CommandType::Defer: return EventType::Deferred;
    case CommandType::Undefer: return EventType::Undeferred;
    case CommandType::UndoArchive: return EventType::UndoArchive;
    case CommandType::UndoDelete: return EventType::UndoDelete;
    }
    return EventType::Archived;
}

MessageState pending_state_for(CommandType t) {
    switch (t) {
    case CommandType::Archive:
    case CommandType::UndoArchive: return MessageState::DoPending;
    case CommandType::Delete:
    case CommandType::UndoDelete: return MessageState::DeletePending;
    case CommandType::Defer:
    case CommandType::Undefer: return MessageState::DeferPending;
    }
    return MessageState::DoPending;
}

bool is_undo(CommandType t) { return t == CommandType::UndoArchive || t == CommandType::UndoDelete; }

bool is_active(const Command& c) { return c.state == CommandState::Pending || c.state == CommandState::InFlight; }

int backoff_seconds(const RetryPolicy& p, int attempts) {
    long long wait = std::max(1, p.base_backoff_seconds);
    for (int i = 1; i < attempts && wait < p.max_backoff_seconds; ++i) wait *= 2;
    return static_cast<int>(std::min<long long>(wait, std::max(1, p.max_backoff_seconds)));
}

// Result of the network step, unified for moves and flag changes.
struct Exec {
    bool ok = false;
    MoveResult move;
    Error error;
};

// Applies a server-confirmed command to the local store in ONE transaction.
Status confirm_command(Store& store, const Clock& clock, Command& cmd, const MoveResult& mr) {
    (void)clock;
    Transaction tx(store);
    const bool moved = cmd.type != CommandType::Defer && cmd.type != CommandType::Undefer;
    json res = json::object();
    const std::string new_id = mr.new_id.empty() ? cmd.message_id : mr.new_id;
    if (moved) {
        res["new_id"] = new_id;
        res["new_folder_id"] = mr.new_folder_id;
    }
    cmd.state = CommandState::Confirmed;
    cmd.last_error.clear();
    cmd.server_result_json = dump(res);
    SLOWMAIL_TRY(store.put_command(cmd));

    MessageState next = MessageState::InQueue;
    std::string reason;
    std::optional<MessageSummary> summary = store.get_message(cmd.message_id);
    switch (cmd.type) {
    case CommandType::Archive: next = MessageState::Archived; break;
    case CommandType::Delete: next = MessageState::Deleted; break;
    case CommandType::Defer:
        next = MessageState::Deferred;
        reason = "deferred_by_slowmail";
        if (summary) {
            summary->flag_status = FlagStatus::Flagged;
            SLOWMAIL_TRY(store.upsert_message(*summary));
        }
        break;
    case CommandType::Undefer:
        next = MessageState::InQueue;
        if (summary) {
            summary->flag_status = FlagStatus::NotFlagged;
            SLOWMAIL_TRY(store.upsert_message(*summary));
        }
        break;
    case CommandType::UndoArchive:
    case CommandType::UndoDelete: next = MessageState::InQueue; break;
    }
    SLOWMAIL_TRY(store.set_message_state(cmd.message_id, next, reason));

    if (is_undo(cmd.type) && new_id != cmd.message_id && summary) {
        // Ids are not immutable on this backend: the message is back in the Inbox under a new id. Track it
        // there and tombstone the stale row so the queue never offers an id the server no longer knows.
        MessageSummary moved_back = *summary;
        moved_back.id = new_id;
        if (!mr.new_folder_id.empty()) moved_back.parent_folder_id = mr.new_folder_id;
        SLOWMAIL_TRY(store.upsert_message(moved_back));
        SLOWMAIL_TRY(store.set_message_state(new_id, MessageState::InQueue, "returned_to_inbox"));
        SLOWMAIL_TRY(store.mark_gone(cmd.message_id));
    }
    SLOWMAIL_TRY(detail::update_event_for_command(store, cmd.session_id, cmd.id, ConfirmationState::Confirmed, ""));
    SLOWMAIL_TRY(detail::touch_session_item(store, cmd.session_id, cmd.message_id, MessageState::InQueue, next, reason));
    return tx.commit();
}

} // namespace

std::string CommandProcessor::make_idempotency_key(const std::string& session_id, const std::string& message_id,
                                                   CommandType type) {
    return session_id + ":" + message_id + ":" + to_string(type);
}

CommandProcessor::CommandProcessor(Store& store, MailBackend& backend, const Clock& clock, RetryPolicy policy)
    : store_(store), backend_(backend), clock_(clock), policy_(policy) {}

Outcome<Command> CommandProcessor::enqueue(const std::string& session_id, const std::string& message_id,
                                           CommandType type, const std::string& payload_json) {
    auto invalid = [](const std::string& msg) { return Outcome<Command>(make_status(ErrorKind::Invalid, msg).error()); };
    try {
        json payload = parse_object(payload_json);
        const auto summary = store_.get_message(message_id);
        const auto state_opt = store_.get_message_state(message_id);
        if (!summary || !state_opt) return invalid("unknown message");
        const MessageState state = *state_opt;
        const std::vector<Command> history = store_.list_commands_for_message(message_id);

        // Undo needs a Confirmed original of the matching type.
        std::string original_id;
        if (is_undo(type)) {
            const CommandType want = type == CommandType::UndoArchive ? CommandType::Archive : CommandType::Delete;
            if (payload.contains("original_command_id") && payload["original_command_id"].is_string()) {
                original_id = payload["original_command_id"].get<std::string>();
            } else {
                for (auto it = history.rbegin(); it != history.rend(); ++it) {
                    if (it->type == want && it->state == CommandState::Confirmed) {
                        original_id = it->id;
                        break;
                    }
                }
            }
            const auto original = original_id.empty() ? std::nullopt : store_.get_command(original_id);
            if (!original || original->message_id != message_id || original->type != want ||
                original->state != CommandState::Confirmed) {
                return invalid("undo requires a confirmed original command");
            }
            payload["original_command_id"] = original_id;
        }

        // Idempotency: the same logical action returns the existing command while that command is still
        // meaningful (in progress, or confirmed and not superseded by a later command on the message).
        std::string base_key = make_idempotency_key(session_id, message_id, type);
        if (is_undo(type)) base_key += ":" + original_id;
        std::string key = base_key;
        for (int n = 2;; ++n) {
            const auto existing = store_.find_command_by_idempotency_key(key);
            if (!existing) break;
            const bool live = is_active(*existing) || (existing->state == CommandState::Confirmed && !history.empty() &&
                                                       history.back().id == existing->id);
            if (live) return *existing;
            key = base_key + "#" + std::to_string(n);
        }

        if (state == MessageState::GoneOnServer) return invalid("message is gone on server");
        for (const auto& h : history) {
            if (is_active(h)) return invalid("another command is already pending for this message");
        }
        bool state_ok = false;
        switch (type) {
        case CommandType::Archive:
        case CommandType::Delete:
        case CommandType::Defer: state_ok = state == MessageState::InQueue || state == MessageState::Failed; break;
        case CommandType::Undefer: state_ok = state == MessageState::Deferred; break;
        case CommandType::UndoArchive: state_ok = state == MessageState::Archived; break;
        case CommandType::UndoDelete: state_ok = state == MessageState::Deleted; break;
        }
        if (!state_ok) return invalid(std::string("action not allowed in message state ") + to_string(state));

        Command cmd;
        cmd.id = new_id("cmd");
        cmd.session_id = session_id;
        cmd.message_id = message_id;
        cmd.type = type;
        cmd.payload_json = dump(payload);
        cmd.created_at = clock_.now_wall();
        cmd.state = CommandState::Pending;
        cmd.idempotency_key = key;

        SessionEvent ev;
        ev.id = new_id("evt");
        ev.session_id = session_id;
        ev.message_id = message_id;
        ev.type = event_type_for(type);
        ev.timestamp = cmd.created_at;
        ev.confirmation = ConfirmationState::Pending;
        ev.display_snapshot_json = detail::make_snapshot_json(*summary);
        ev.command_id = cmd.id;

        const MessageState pending = pending_state_for(type);
        Transaction tx(store_);
        {
            Status s = store_.put_command(cmd);
            if (s.ok()) s = store_.set_message_state(message_id, pending, "");
            if (s.ok()) s = store_.put_event(ev);
            if (s.ok()) s = detail::touch_session_item(store_, session_id, message_id, state, pending, "");
            if (s.ok()) s = tx.commit();
            if (!s.ok()) return Outcome<Command>(s.error());
        }
        SLOWMAIL_LOG_INFO("enqueue: command=" + cmd.id + " type=" + to_string(type) + " message=" + message_id);
        return cmd;
    } catch (const std::exception&) {
        return invalid("could not enqueue command");
    }
}

std::size_t CommandProcessor::recover_in_flight() {
    std::size_t n = 0;
    Transaction tx(store_);
    for (Command c : store_.list_commands(CommandState::InFlight)) {
        c.state = CommandState::Pending;  // attempt was counted when it went InFlight
        c.last_error = "interrupted before confirmation; retrying";
        if (!store_.put_command(c).ok()) return 0;
        ++n;
    }
    if (!tx.commit().ok()) return 0;
    if (n) SLOWMAIL_LOG_INFO("recover: in-flight commands returned to pending count=" + std::to_string(n));
    return n;
}

ProcessReport CommandProcessor::process() {
    ProcessReport rep;
    const std::vector<Command> queue = store_.list_commands(CommandState::Pending);

    for (Command cmd : queue) {
        if (not_before_of(cmd) > clock_.now_wall()) {
            ++rep.deferred_retry;
            continue;
        }
        const auto summary = store_.get_message(cmd.message_id);
        if (!summary) {
            cmd.state = CommandState::Failed;
            cmd.last_error = "message not in cache";
            (void)store_.put_command(cmd);
            ++rep.failed;
            continue;
        }

        // Evidence first: InFlight is durable before the network call (A06).
        cmd.attempts += 1;
        cmd.state = CommandState::InFlight;
        if (!store_.put_command(cmd).ok()) {
            SLOWMAIL_LOG_ERROR("process: could not persist in-flight state; stopping");
            break;
        }

        Exec ex;
        switch (cmd.type) {
        case CommandType::Archive:
        case CommandType::Delete: {
            auto r = backend_.move(cmd.message_id, cmd.type == CommandType::Archive ? WellKnownFolder::Archive
                                                                                     : WellKnownFolder::DeletedItems);
            ex.ok = r.ok();
            if (r.ok()) ex.move = r.value(); else ex.error = r.error();
            break;
        }
        case CommandType::Defer:
        case CommandType::Undefer: {
            Status s = backend_.set_flag(cmd.message_id,
                                         cmd.type == CommandType::Defer ? FlagStatus::Flagged : FlagStatus::NotFlagged);
            ex.ok = s.ok();
            if (!s.ok()) ex.error = s.error();
            break;
        }
        case CommandType::UndoArchive:
        case CommandType::UndoDelete: {
            // Ids can change on move (immutable ids not honoured): use the id the original move returned.
            std::string id = cmd.message_id;
            const json payload = parse_object(cmd.payload_json);
            if (payload.contains("original_command_id") && payload["original_command_id"].is_string()) {
                if (auto orig = store_.get_command(payload["original_command_id"].get<std::string>())) {
                    const json res = parse_object(orig->server_result_json);
                    auto it = res.find("new_id");
                    if (it != res.end() && it->is_string() && !it->get<std::string>().empty()) id = it->get<std::string>();
                }
            }
            auto r = backend_.move(id, WellKnownFolder::Inbox);
            ex.ok = r.ok();
            if (r.ok()) ex.move = r.value(); else ex.error = r.error();
            break;
        }
        }

        bool stop = false;
        const Status applied = ex.ok ? confirm_command(store_, clock_, cmd, ex.move) : Status();
        if (ex.ok) {
            if (applied.ok()) {
                ++rep.confirmed;
            } else {
                SLOWMAIL_LOG_ERROR("process: confirmed on server but local store update failed");
                stop = true;
            }
            if (stop) break;
            continue;
        }

        const Error& err = ex.error;
        const std::string redacted = redact(err.message);
        const std::int64_t now = clock_.now_wall();

        auto park_pending = [&](std::int64_t wait_seconds) {
            json payload = parse_object(cmd.payload_json);
            payload["not_before"] = now + wait_seconds;
            cmd.payload_json = dump(payload);
            cmd.state = CommandState::Pending;
            cmd.last_error = redacted;
            (void)store_.put_command(cmd);
        };
        auto fail_command = [&](const std::string& reason) {
            Transaction tx(store_);
            cmd.state = CommandState::Failed;
            cmd.last_error = redacted;
            Status s = store_.put_command(cmd);
            if (s.ok()) s = store_.set_message_state(cmd.message_id, MessageState::Failed, reason);
            if (s.ok()) s = detail::update_event_for_command(store_, cmd.session_id, cmd.id, ConfirmationState::Failed, reason);
            if (s.ok()) s = detail::touch_session_item(store_, cmd.session_id, cmd.message_id, MessageState::InQueue, MessageState::Failed, reason);
            if (s.ok()) (void)tx.commit();
            ++rep.failed;
        };

        switch (err.kind) {
        case ErrorKind::NotFound:
            if (err.code.empty() || err.code == "ErrorItemNotFound") {
                Transaction tx(store_);
                cmd.state = CommandState::Abandoned;
                cmd.last_error = kGoneReason;
                Status s = store_.put_command(cmd);
                if (s.ok()) s = store_.mark_gone(cmd.message_id);
                if (s.ok()) s = detail::update_event_for_command(store_, cmd.session_id, cmd.id, ConfirmationState::Failed, "already gone");
                if (s.ok()) s = detail::touch_session_item(store_, cmd.session_id, cmd.message_id, MessageState::InQueue, MessageState::GoneOnServer, "");
                if (s.ok()) (void)tx.commit();
                ++rep.abandoned;
            } else {
                fail_command(err.code);  // e.g. ErrorFolderNotFound: a configuration problem, not a gone message
            }
            break;
        case ErrorKind::Throttled: {
            const int bo = backoff_seconds(policy_, cmd.attempts);
            const int ra = std::min(err.retry_after_seconds.value_or(0), kMaxRetryAfterSeconds);
            park_pending(std::max(ra, bo));
            ++rep.deferred_retry;
            if (err.retry_after_seconds) {
                rep.retry_after_seconds = std::max(rep.retry_after_seconds.value_or(0), *err.retry_after_seconds);
            }
            stop = true;
            break;
        }
        case ErrorKind::Network:
        case ErrorKind::Cancelled:
            cmd.attempts = std::max(0, cmd.attempts - 1);  // never reached the server
            cmd.state = CommandState::Pending;
            cmd.last_error = redacted;
            (void)store_.put_command(cmd);
            ++rep.deferred_retry;
            if (err.kind == ErrorKind::Network) rep.went_offline = true;
            stop = true;
            break;
        case ErrorKind::Auth:
            cmd.attempts = std::max(0, cmd.attempts - 1);
            cmd.state = CommandState::Pending;
            cmd.last_error = redacted;
            (void)store_.put_command(cmd);
            ++rep.deferred_retry;
            rep.auth_required = true;
            stop = true;
            break;
        case ErrorKind::Conflict:
        case ErrorKind::Server:
            if (cmd.attempts >= policy_.max_attempts) {
                fail_command(err.code.empty() ? to_string(err.kind) : err.code);
            } else {
                park_pending(backoff_seconds(policy_, cmd.attempts));
                ++rep.deferred_retry;
            }
            break;
        default:
            fail_command(err.code.empty() ? to_string(err.kind) : err.code);
            break;
        }
        if (stop) break;
    }
    return rep;
}

} // namespace slowmail::core
