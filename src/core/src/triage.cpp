#include "slowmail/core/triage.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "engine_util.hpp"
#include "slowmail/core/log.hpp"

namespace slowmail::core {

using detail::make_status;

struct TriageSession::Impl {
    Store& store;
    MailBackend& backend;
    std::shared_ptr<Clock> clock;
    TriageConfig config;
    SyncEngine sync;
    CommandProcessor commands;
    SessionStateMachine machine;
    SessionRecord session;
    bool opened = false;     // store.open() succeeded
    bool started = false;    // a session is active
    bool online = true;            // belief shown as StatusSnapshot::offline; never gates delivery by itself
    bool hint_offline = false;     // set ONLY by an explicit set_online(false): suppresses auto-flush in act()
    bool signed_in = false;
    std::string last_error;
    bool last_error_is_network = false;  // last_error describes a transport failure: cleared when the net is back

    Impl(Store& s, MailBackend& b, std::shared_ptr<Clock> c, TriageConfig cfg)
        : store(s),
          backend(b),
          clock(std::move(c)),
          config(cfg),
          sync(s, b, *clock, CooldownPolicy(cfg.cooldown)),
          commands(s, b, *clock, cfg.retry) {}

    // ---- session record persistence -------------------------------------------------------------
    Status persist() {
        session.state = machine.state();
        Transaction tx(store);
        SLOWMAIL_TRY(store.put_session(session));
        return tx.commit();
    }

    // Applies an event if legal; returns whether the state machine accepted it.
    bool apply(SessionEvent_ ev) {
        if (!machine.apply(ev)) return false;
        (void)persist();
        return true;
    }

    void force(SessionState s) {
        machine = SessionStateMachine(s);
        (void)persist();
    }

    void to_auth_required() {
        if (machine.state() == SessionState::AuthRequired) return;
        if (machine.state() == SessionState::Syncing) (void)apply(SessionEvent_::SyncInterrupted);
        if (!apply(SessionEvent_::AuthNeeded)) force(SessionState::AuthRequired);
    }

    // start(): commands left over from an earlier run are sent once. One bounded attempt, errors ignored (a
    // Network error leaves them Pending and marks us offline; nothing else blocks on the network).
    void flush_pending_once() {
        if (!started || !signed_in || machine.state() == SessionState::AuthRequired) return;
        if (store.list_commands(CommandState::Pending).empty()) return;
        (void)flush();
    }

    void note_error(const Error& e) {
        last_error = redact(e.message);
        if (e.kind == ErrorKind::Auth) to_auth_required();
    }

    // A network operation got a real answer from the server (sync page, confirmed command, ...): whatever we
    // believed about being offline is stale. Offline -> Active; the explicit hint and the banner are cleared.
    void network_ok() {
        online = true;
        hint_offline = false;
        if (last_error_is_network || last_error.rfind("offline", 0) == 0) {
            last_error.clear();
            last_error_is_network = false;
        }
        if (machine.state() == SessionState::Offline) (void)apply(SessionEvent_::NetworkBack);
    }

    void network_lost(const std::string& message) {
        online = false;
        last_error = message;
        last_error_is_network = true;
        (void)apply(SessionEvent_::NetworkLost);  // no-op outside Active; the offline flag still shows
    }

    ProcessReport flush() {
        ProcessReport rep;
        if (!started) return rep;
        rep = commands.process();
        if (rep.went_offline) {
            network_lost("offline: actions are queued");
        } else if (rep.reached_server) {
            network_ok();
        }
        if (rep.auth_required) {
            last_error = "sign-in required";
            to_auth_required();
        }
        return rep;
    }

    Outcome<Command> act(CommandType type, const std::string& message_id, const std::string& payload) {
        if (!started) return Outcome<Command>(make_status(ErrorKind::Invalid, "session not started").error());
        if (!SessionStateMachine::can_process_mail(machine.state())) {
            return Outcome<Command>(make_status(ErrorKind::Invalid, std::string("actions not allowed in state ") +
                                                                        to_string(machine.state()))
                                        .error());
        }
        return deliver(commands.enqueue(session.id, message_id, type, payload));
    }

    // After a successful enqueue: try to send right away (a Network error just leaves it Pending).
    // Only an explicit set_online(false) hint skips the attempt.
    Outcome<Command> deliver(Outcome<Command> c) {
        if (!c.ok()) return c;
        if (!hint_offline) (void)flush();
        if (auto latest = store.get_command(c.value().id)) return *latest;
        return c;
    }
};

TriageSession::TriageSession(std::unique_ptr<Store> store, std::unique_ptr<MailBackend> backend,
                             std::shared_ptr<Clock> clock, TriageConfig config)
    : store_(std::move(store)), backend_(std::move(backend)), clock_(std::move(clock)) {
    impl_ = std::make_unique<Impl>(*store_, *backend_, clock_, config);
}

TriageSession::~TriageSession() { impl_.reset(); }

Status TriageSession::start() {
    Impl& i = *impl_;
    if (i.started && i.machine.state() != SessionState::Ended) {
        // Re-entry after sign-in: AuthRequired -> ReadyCached once the account is signed in again.
        if (i.machine.state() == SessionState::AuthRequired) {
            auto acc = i.backend.account();
            if (acc.ok() && acc.value().signed_in) {
                i.signed_in = true;
                (void)i.apply(SessionEvent_::AuthOk);
                i.flush_pending_once();
            }
        }
        return Status::success();
    }
    if (!i.opened) {
        SLOWMAIL_TRY(i.store.open());
        i.opened = true;
    }
    const std::size_t recovered = i.commands.recover_in_flight();

    // Account: local call on the real backend (token presence); never required to reach the network.
    i.signed_in = false;
    if (auto acc = i.backend.account(); acc.ok()) {
        i.signed_in = acc.value().signed_in;
        if (acc.value().signed_in) {
            Transaction tx(i.store);
            SLOWMAIL_TRY(i.store.put_account(acc.value()));
            SLOWMAIL_TRY(tx.commit());
        }
    } else if (auto cached = i.store.get_account()) {
        i.signed_in = cached->signed_in;
    }

    if (auto active = i.store.get_active_session()) {
        i.session = *active;
        SessionState s = active->state;
        switch (s) {
        case SessionState::Active:
        case SessionState::SyncPartial: break;
        default: s = SessionState::ReadyCached; break;  // transient/obsolete states fall back to the cache view
        }
        i.machine = SessionStateMachine(s);
    } else {
        i.session = SessionRecord{};
        i.session.id = new_id("sess");
        i.session.started_at = i.clock->now_wall();
        i.machine = SessionStateMachine(SessionState::NotStarted);
        (void)i.machine.apply(SessionEvent_::Start);
    }
    if (!i.signed_in) {
        if (i.machine.state() == SessionState::ReadyCached) (void)i.machine.apply(SessionEvent_::AuthNeeded);
        else i.machine = SessionStateMachine(SessionState::AuthRequired);
    }
    SLOWMAIL_TRY(i.persist());
    i.started = true;
    i.online = true;
    i.hint_offline = false;
    i.last_error.clear();
    i.last_error_is_network = false;
    i.flush_pending_once();
    SLOWMAIL_LOG_INFO("session: started id=" + i.session.id + " recovered=" + std::to_string(recovered) +
                      " queue=" + std::to_string(i.store.list_queue().size()) + " deferred=" +
                      std::to_string(i.store.list_messages_in_state(MessageState::Deferred).size()));
    return Status::success();
}

Outcome<SyncReport> TriageSession::refresh(bool force) {
    Impl& i = *impl_;
    if (!i.started) return Outcome<SyncReport>(make_status(ErrorKind::Invalid, "session not started").error());
    if (!SessionStateMachine::can_attempt_sync(i.machine.state())) {
        return Outcome<SyncReport>(make_status(ErrorKind::Invalid, std::string("refresh not allowed in state ") +
                                                                       to_string(i.machine.state()))
                                       .error());
    }

    // 1. Outgoing work first. Independent of the inbound cooldown; it is also the cheapest way to find out
    //    whether the network is back.
    const ProcessReport sent = i.flush();
    if (sent.auth_required) return Outcome<SyncReport>(make_status(ErrorKind::Auth, "sign-in required").error());
    if (sent.went_offline) {
        Error e;
        e.kind = ErrorKind::Network;
        e.message = "offline: could not reach the server";
        return Outcome<SyncReport>(e);
    }
    if (sent.retry_after_seconds) {
        Error e;
        e.kind = ErrorKind::Throttled;
        e.message = "server asked us to slow down";
        e.retry_after_seconds = sent.retry_after_seconds;
        return Outcome<SyncReport>(e);
    }

    // 2. Then the inbound sync, if the cooldown allows.
    const CooldownDecision decision = i.sync.cooldown_decision();
    if (!force && !decision.eligible) {
        (void)i.apply(SessionEvent_::CooldownActive);  // no-op where not applicable; cached queue stays readable
        Error e;
        e.kind = ErrorKind::Policy;
        e.message = "refresh not yet available (cooldown active)";
        return Outcome<SyncReport>(e);
    }

    // From Offline the machine is left alone: success moves it to Active (NetworkBack), failure keeps it Offline.
    const bool from_offline = i.machine.state() == SessionState::Offline;
    if (!from_offline) {
        if (i.machine.state() != SessionState::SyncEligible && i.machine.state() != SessionState::SyncPartial) {
            (void)i.apply(SessionEvent_::CooldownElapsed);
        }
        if (!i.apply(SessionEvent_::SyncStarted)) {
            return Outcome<SyncReport>(make_status(ErrorKind::Invalid, "cannot start sync from current state").error());
        }
    }

    SyncReport rep = i.sync.run(force);
    if (rep.complete) {
        if (!from_offline) (void)i.apply(SessionEvent_::SyncCompleted);
        i.network_ok();
        if (!i.session.initial_sync_at) {
            i.session.initial_sync_at = i.clock->now_wall();
            (void)i.persist();
        }
        i.last_error.clear();
        i.last_error_is_network = false;
        return rep;
    }
    if (!from_offline) (void)i.apply(SessionEvent_::SyncInterrupted);
    if (rep.error.kind == ErrorKind::Network) {
        i.note_error(rep.error);
        i.online = false;
        i.last_error_is_network = true;
    } else {
        if (rep.error.kind != ErrorKind::Policy && rep.error.kind != ErrorKind::Storage) i.network_ok();  // we did get an answer from the server
        i.note_error(rep.error);
    }
    return Outcome<SyncReport>(rep.error);
}

ProcessReport TriageSession::flush() { return impl_->flush(); }

std::vector<MessageSummary> TriageSession::queue() const {
    Impl& i = *impl_;
    if (!i.opened) return {};
    std::vector<MessageSummary> q = i.store.list_queue();
    if (i.config.hide_preexisting_flagged) {
        q.erase(std::remove_if(q.begin(), q.end(),
                               [](const MessageSummary& m) { return m.flag_status != FlagStatus::NotFlagged; }),
                q.end());
    }
    return q;
}

Outcome<MessageBody> TriageSession::open(const std::string& message_id) {
    Impl& i = *impl_;
    if (!i.opened) return Outcome<MessageBody>(make_status(ErrorKind::Invalid, "session not started").error());
    if (!i.store.get_message(message_id)) {
        return Outcome<MessageBody>(make_status(ErrorKind::Invalid, "unknown message").error());
    }
    if (auto cached = i.store.get_body(message_id)) return *cached;
    Outcome<MessageBody> fetched = i.backend.fetch_body(message_id);
    if (!fetched.ok()) {
        if (fetched.error().kind == ErrorKind::Network) i.last_error = "offline: message body not cached";
        else i.note_error(fetched.error());
        return fetched;
    }
    MessageBody body = fetched.value();
    body.id = message_id;
    Transaction tx(i.store);
    {
        Status s = i.store.put_body(body);
        if (s.ok()) s = tx.commit();
        if (!s.ok()) return Outcome<MessageBody>(s.error());
    }
    return body;
}

std::optional<MessageSummary> TriageSession::message(const std::string& message_id) const {
    Impl& i = *impl_;
    if (!i.opened) return std::nullopt;
    return i.store.get_message(message_id);
}

Outcome<Command> TriageSession::archive(const std::string& id) { return impl_->act(CommandType::Archive, id, "{}"); }
Outcome<Command> TriageSession::remove(const std::string& id) { return impl_->act(CommandType::Delete, id, "{}"); }
Outcome<Command> TriageSession::defer(const std::string& id) { return impl_->act(CommandType::Defer, id, "{}"); }
Outcome<Command> TriageSession::undefer(const std::string& id) { return impl_->act(CommandType::Undefer, id, "{}"); }

Outcome<Command> TriageSession::undo(const std::string& command_id) {
    Impl& i = *impl_;
    if (!i.started) return Outcome<Command>(make_status(ErrorKind::Invalid, "session not started").error());
    const auto original = i.store.get_command(command_id);
    if (!original) return Outcome<Command>(make_status(ErrorKind::Invalid, "unknown command").error());
    CommandType undo_type;
    if (original->type == CommandType::Archive) undo_type = CommandType::UndoArchive;
    else if (original->type == CommandType::Delete) undo_type = CommandType::UndoDelete;
    else return Outcome<Command>(make_status(ErrorKind::Invalid, "only archive and delete can be undone").error());
    if (original->state != CommandState::Confirmed) {
        return Outcome<Command>(make_status(ErrorKind::Invalid, "only confirmed actions can be undone").error());
    }
    nlohmann::json payload;
    payload["original_command_id"] = command_id;
    return i.act(undo_type, original->message_id, payload.dump());
}

std::vector<MessageSummary> TriageSession::failed() const {
    Impl& i = *impl_;
    if (!i.opened) return {};
    return i.store.list_messages_in_state(MessageState::Failed);
}

std::optional<Command> TriageSession::failed_command(const std::string& message_id) const {
    Impl& i = *impl_;
    if (!i.opened) return std::nullopt;
    const std::vector<Command> history = i.store.list_commands_for_message(message_id);
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->state == CommandState::Failed) return *it;
    }
    return std::nullopt;
}

std::string TriageSession::failure_reason(const std::string& message_id) const {
    if (const auto c = failed_command(message_id)) return c->last_error;
    return {};
}

Outcome<Command> TriageSession::retry(const std::string& message_id) {
    Impl& i = *impl_;
    if (!i.started) return Outcome<Command>(make_status(ErrorKind::Invalid, "session not started").error());
    if (!SessionStateMachine::can_process_mail(i.machine.state())) {
        return Outcome<Command>(make_status(ErrorKind::Invalid, std::string("actions not allowed in state ") +
                                                                    to_string(i.machine.state()))
                                    .error());
    }
    return i.deliver(i.commands.retry(i.session.id, message_id));
}

std::vector<ActivityRow> TriageSession::activity() const {
    Impl& i = *impl_;
    std::vector<ActivityRow> rows;
    if (!i.opened || i.session.id.empty()) return rows;
    for (const SessionEvent& e : i.store.list_events(i.session.id)) {
        ActivityRow row;
        row.event = e;
        const nlohmann::json j = nlohmann::json::parse(e.display_snapshot_json, nullptr, false);
        if (j.is_object()) {
            auto str = [&j](const char* key) {
                auto it = j.find(key);
                return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
            };
            row.subject = str("subject");
            row.from_name = str("from");
            row.from_address = str("from_address");
            row.received_at_iso = str("received_at_iso");
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<MessageSummary> TriageSession::deferred() const {
    Impl& i = *impl_;
    if (!i.opened || !i.config.show_deferred_panel) return {};
    return i.store.list_messages_in_state(MessageState::Deferred);
}

Status TriageSession::end() {
    Impl& i = *impl_;
    if (!i.started) return make_status(ErrorKind::Invalid, "session not started");
    if (!i.apply(SessionEvent_::End)) i.force(SessionState::Ending);
    if (!i.hint_offline) (void)i.flush();  // pending commands survive for the next session if this fails
    if (!i.machine.apply(SessionEvent_::Ended)) i.machine = SessionStateMachine(SessionState::Ended);
    i.session.ended_at = i.clock->now_wall();
    i.session.state = SessionState::Ended;
    Transaction tx(i.store);
    SLOWMAIL_TRY(i.store.put_session(i.session));
    SLOWMAIL_TRY(i.store.purge_tombstones());
    SLOWMAIL_TRY(tx.commit());
    i.started = false;
    SLOWMAIL_LOG_INFO("session: ended id=" + i.session.id);
    return Status::success();
}

Status TriageSession::sign_out(bool wipe_cache) {
    Impl& i = *impl_;
    SLOWMAIL_TRY(i.store.clear_token_blob());
    i.signed_in = false;
    if (wipe_cache) {
        SLOWMAIL_TRY(i.store.secure_wipe());
        i.opened = false;  // the store must be opened (and a session started) again
        i.started = false;
        i.machine = SessionStateMachine(SessionState::AuthRequired);
        i.session = SessionRecord{};
        i.last_error.clear();
        return Status::success();
    }
    if (i.started) {
        Transaction tx(i.store);
        AccountInfo acc = i.store.get_account().value_or(AccountInfo{});
        acc.signed_in = false;
        SLOWMAIL_TRY(i.store.put_account(acc));
        SLOWMAIL_TRY(tx.commit());
        i.to_auth_required();
    }
    return Status::success();
}

StatusSnapshot TriageSession::status() const {
    Impl& i = *impl_;
    StatusSnapshot s;
    s.session_state = i.machine.state();
    s.session_id = i.session.id;
    s.signed_in = i.signed_in;
    s.offline = !i.online;
    s.last_error = i.last_error;
    if (!i.opened) return s;
    s.cooldown = i.sync.cooldown_decision();
    s.queue_size = queue().size();
    s.pending_commands = i.store.list_commands(CommandState::Pending).size() +
                         i.store.list_commands(CommandState::InFlight).size();
    s.deferred_count = i.store.list_messages_in_state(MessageState::Deferred).size();
    s.failed_count = i.store.list_messages_in_state(MessageState::Failed).size();
    return s;
}

void TriageSession::set_online(bool online) {
    Impl& i = *impl_;
    i.online = online;
    i.hint_offline = !online;
    if (!i.started) return;
    if (online) {
        if (i.machine.state() == SessionState::Offline) (void)i.apply(SessionEvent_::NetworkBack);
        if (i.last_error_is_network || i.last_error.rfind("offline", 0) == 0) {
            i.last_error.clear();
            i.last_error_is_network = false;
        }
    } else {
        (void)i.apply(SessionEvent_::NetworkLost);
    }
}

} // namespace slowmail::core
