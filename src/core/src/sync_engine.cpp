#include "slowmail/core/sync_engine.hpp"

#include <set>

#include "engine_util.hpp"
#include "slowmail/core/log.hpp"
#include "slowmail/core/session.hpp"

namespace slowmail::core {

namespace {

using detail::make_status;

constexpr const char* kFullResyncKey = "sync.full_resync_pending";
constexpr const char* kGoneReason = "message gone on server before action ran";

bool same_summary(const MessageSummary& a, const MessageSummary& b) {
    return a.id == b.id && a.conversation_id == b.conversation_id && a.subject == b.subject &&
           a.from.name == b.from.name && a.from.address == b.from.address && a.received_at_iso == b.received_at_iso &&
           a.received_at_epoch == b.received_at_epoch && a.is_read == b.is_read && a.flag_status == b.flag_status &&
           a.body_preview == b.body_preview && a.has_attachments == b.has_attachments &&
           a.change_key == b.change_key && a.parent_folder_id == b.parent_folder_id;
}

struct Ctx {
    Store& store;
    const Clock& clock;
    SyncReport& report;
    std::string session_id;  // active session, may be empty
};

Status record_undeferred(Ctx& c, const MessageSummary& m) {
    SessionEvent ev;
    ev.id = new_id("evt");
    ev.session_id = c.session_id;
    ev.message_id = m.id;
    ev.type = EventType::Undeferred;
    ev.timestamp = c.clock.now_wall();
    ev.confirmation = ConfirmationState::Confirmed;
    ev.display_snapshot_json = detail::make_snapshot_json(m);
    SLOWMAIL_TRY(c.store.put_event(ev));
    return detail::touch_session_item(c.store, c.session_id, m.id, MessageState::Deferred, MessageState::InQueue, "");
}

// One changed item. Idempotent: applying the same item twice leaves the same store state.
Status apply_changed(Ctx& c, const MessageSummary& m) {
    if (m.id.empty()) return Status::success();  // adapter contract violation; ignore rather than poison the page
    const auto state = c.store.get_message_state(m.id);
    if (!state) {
        SLOWMAIL_TRY(c.store.upsert_message(m));
        SLOWMAIL_TRY(c.store.set_message_state(m.id, MessageState::InQueue, ""));
        ++c.report.added;
        return Status::success();
    }
    const auto old = c.store.get_message(m.id);
    const bool fields_changed = !old || !same_summary(*old, m);

    switch (*state) {
    case MessageState::Deferred:
        if (m.flag_status == FlagStatus::NotFlagged) {
            // Q5: someone cleared the flag elsewhere -> back to the queue.
            SLOWMAIL_TRY(c.store.upsert_message(m));
            SLOWMAIL_TRY(c.store.set_message_state(m.id, MessageState::InQueue, ""));
            SLOWMAIL_TRY(record_undeferred(c, m));
            ++c.report.restored_to_queue;
            return Status::success();
        }
        break;
    case MessageState::Archived:
    case MessageState::Deleted:
    case MessageState::GoneOnServer:
        // The server reports it in the Inbox: server is truth (moved back / returned).
        SLOWMAIL_TRY(c.store.upsert_message(m));
        SLOWMAIL_TRY(c.store.set_message_state(m.id, MessageState::InQueue, "returned_to_inbox"));
        SLOWMAIL_TRY(detail::touch_session_item(c.store, c.session_id, m.id, *state, MessageState::InQueue, ""));
        ++c.report.updated;
        SLOWMAIL_LOG_INFO("sync: message returned to inbox id=" + m.id);
        return Status::success();
    default:
        break;  // InQueue, *Pending, Failed: fields only
    }
    if (fields_changed) {
        SLOWMAIL_TRY(c.store.upsert_message(m));
        ++c.report.updated;
    }
    return Status::success();
}

Status abandon_pending_for(Ctx& c, const std::string& id) {
    for (Command cmd : c.store.list_commands_for_message(id)) {
        if (cmd.state != CommandState::Pending && cmd.state != CommandState::InFlight) continue;
        cmd.state = CommandState::Abandoned;
        cmd.last_error = kGoneReason;
        SLOWMAIL_TRY(c.store.put_command(cmd));
        SLOWMAIL_TRY(detail::update_event_for_command(c.store, cmd.session_id, cmd.id, ConfirmationState::Failed,
                                                      "already gone"));
        ++c.report.reconciled_pending;
    }
    return Status::success();
}

// `resync` = tombstoning of everything unseen after a full resync (also covers Archived/Deleted rows).
Status apply_removed(Ctx& c, const std::string& id, bool resync) {
    const auto state = c.store.get_message_state(id);
    if (!state || *state == MessageState::GoneOnServer) return Status::success();
    // Our own confirmed archive/delete shows up as a removal; that is the expected confirmation, not a
    // conflict. Keep the row so the session can still offer Undo.
    if (!resync && (*state == MessageState::Archived || *state == MessageState::Deleted)) return Status::success();
    SLOWMAIL_TRY(c.store.mark_gone(id));
    ++c.report.removed;
    return abandon_pending_for(c, id);
}

Status apply_page(Ctx& c, const DeltaPage& page, std::set<std::string>& seen) {
    for (const auto& m : page.changed) {
        SLOWMAIL_TRY(apply_changed(c, m));
        seen.insert(m.id);
    }
    for (const auto& id : page.removed_ids) {
        SLOWMAIL_TRY(apply_removed(c, id, false));
    }
    return Status::success();
}

Status tombstone_unseen(Ctx& c, const std::set<std::string>& seen) {
    static const MessageState kStates[] = {MessageState::InQueue,    MessageState::DoPending, MessageState::Archived,
                                           MessageState::DeletePending, MessageState::Deleted,   MessageState::DeferPending,
                                           MessageState::Deferred,   MessageState::Failed};
    for (MessageState s : kStates) {
        for (const auto& m : c.store.list_messages_in_state(s)) {
            if (seen.count(m.id)) continue;
            SLOWMAIL_TRY(apply_removed(c, m.id, true));
        }
    }
    return Status::success();
}

} // namespace

SyncEngine::SyncEngine(Store& store, MailBackend& backend, const Clock& clock, CooldownPolicy policy)
    : store_(store), backend_(backend), clock_(clock), policy_(policy) {}

CooldownDecision SyncEngine::cooldown_decision() const { return policy_.evaluate(store_.get_sync_state(), clock_); }

SyncReport SyncEngine::run(bool force, const std::function<void(std::size_t)>& progress) {
    SyncReport rep;
    if (!force) {
        const CooldownDecision d = cooldown_decision();
        if (!d.eligible) {
            rep.error.kind = ErrorKind::Policy;
            rep.error.message = "refresh not yet available (cooldown active)";
            return rep;
        }
    }

    InboxSyncState st = store_.get_sync_state();
    const bool resync_pending = store_.get_setting(kFullResyncKey).value_or("") == "1";

    // Choose the starting link. An interrupted full resync is restarted from scratch because the set of ids
    // seen so far is not persisted (needed to tombstone vanished messages).
    std::string link;
    bool from_delta = false;
    if (resync_pending) {
        link.clear();
        rep.full_resync = true;
        if (st.resume_next_link) {
            st.resume_next_link.reset();
            Transaction tx(store_);
            if (Status s = store_.put_sync_state(st); !s.ok() || !tx.commit().ok()) {
                rep.error = s.ok() ? make_status(ErrorKind::Storage, "could not persist sync state").error() : s.error();
                return rep;
            }
        }
    } else if (st.resume_next_link && !st.resume_next_link->empty()) {
        link = *st.resume_next_link;
        rep.resumed = true;
    } else if (st.delta_link && !st.delta_link->empty()) {
        link = *st.delta_link;
        from_delta = true;
    }

    Ctx ctx{store_, clock_, rep, ""};
    if (auto active = store_.get_active_session()) ctx.session_id = active->id;

    std::set<std::string> seen;
    bool gone_once = false;

    auto fail = [&](const Error& err) -> SyncReport {
        rep.complete = false;
        rep.error = err;
        // Error path: keep what we have so a retry resumes; cooldown is NOT consumed (A07).
        InboxSyncState cur = store_.get_sync_state();
        cur.last_attempt_wall = clock_.now_wall();
        if (!from_delta && !link.empty()) cur.resume_next_link = link;
        Transaction tx(store_);
        if (store_.put_sync_state(cur).ok()) (void)tx.commit();
        SLOWMAIL_LOG_WARN(std::string("sync: stopped kind=") + to_string(err.kind) + " pages=" +
                          std::to_string(rep.pages));
        return rep;
    };

    for (;;) {
        Outcome<DeltaPage> out = backend_.fetch_inbox_delta(link);
        if (!out.ok()) {
            const Error& err = out.error();
            if (err.kind == ErrorKind::Gone && !gone_once) {
                gone_once = true;
                st = store_.get_sync_state();
                st.delta_link.reset();
                st.resume_next_link.reset();
                Transaction tx(store_);
                Status s = store_.put_sync_state(st);
                if (s.ok()) s = store_.set_setting(kFullResyncKey, "1");
                if (s.ok()) s = tx.commit();
                if (!s.ok()) return fail(s.error());
                rep.full_resync = true;
                rep.resumed = false;
                link.clear();
                from_delta = false;
                seen.clear();
                SLOWMAIL_LOG_INFO("sync: delta link expired, full resync");
                continue;
            }
            return fail(err);
        }

        const DeltaPage& page = out.value();
        const bool final_page = page.delta_link.has_value();
        if (!final_page && (!page.next_link || page.next_link->empty() || *page.next_link == link)) {
            return fail(make_status(ErrorKind::Malformed, "delta page has no usable next or delta link").error());
        }

        const SyncReport before = rep;
        Status s = [&]() -> Status {
            Transaction tx(store_);  // one transaction per page, incl. the resume pointer
            SLOWMAIL_TRY(apply_page(ctx, page, seen));
            st = store_.get_sync_state();
            if (final_page) {
                st.delta_link = page.delta_link;
                st.resume_next_link.reset();
                CooldownPolicy::record_success(st, clock_);
                SLOWMAIL_TRY(store_.put_sync_state(st));
                if (rep.full_resync) {
                    SLOWMAIL_TRY(tombstone_unseen(ctx, seen));
                    SLOWMAIL_TRY(store_.set_setting(kFullResyncKey, "0"));
                }
            } else {
                st.resume_next_link = page.next_link;
                st.last_attempt_wall = clock_.now_wall();
                SLOWMAIL_TRY(store_.put_sync_state(st));
            }
            return tx.commit();
        }();
        if (!s.ok()) {
            // The page was rolled back; restore the counters and keep the link so a retry resumes here.
            const Error serr = s.error();
            rep = before;
            return fail(serr);
        }

        ++rep.pages;
        if (progress) progress(rep.pages);

        if (final_page) {
            rep.complete = true;
            SLOWMAIL_LOG_INFO("sync: complete pages=" + std::to_string(rep.pages) + " added=" +
                              std::to_string(rep.added) + " updated=" + std::to_string(rep.updated) + " removed=" +
                              std::to_string(rep.removed));
            return rep;
        }
        link = *page.next_link;
        from_delta = false;
    }
}

} // namespace slowmail::core
