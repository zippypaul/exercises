#include "slowmail/core/memory_store.hpp"

#include <algorithm>

namespace slowmail::core {

// Everything begin() must be able to restore. Copies the maps so rollback is real.
struct MemoryStore::Snapshot {
    std::map<std::string, std::string> settings;
    std::optional<AccountInfo> account;
    InboxSyncState sync;
    std::map<std::string, CachedMessage> messages;
    std::map<std::string, MessageBody> bodies;
    std::map<std::string, SessionRecord> sessions;
    std::vector<SessionItem> items;
    std::map<std::string, Command> commands;
    std::vector<std::string> command_order;
    std::vector<SessionEvent> events;
    std::optional<std::string> token_blob;
};

namespace {

Status make_error(ErrorKind kind, const char* msg) {
    Error e;
    e.kind = kind;
    e.message = msg;
    return Status(e);
}

void sort_newest_first(std::vector<MessageSummary>& v) {
    std::sort(v.begin(), v.end(), [](const MessageSummary& a, const MessageSummary& b) {
        if (a.received_at_epoch != b.received_at_epoch) return a.received_at_epoch > b.received_at_epoch;
        return a.id < b.id;
    });
}

} // namespace

// Test hook: data-writing methods consume fail_next_. begin/commit/rollback/open never fail on it.
Status MemoryStore::maybe_fail() {
    if (fail_next_) {
        ErrorKind k = *fail_next_;
        fail_next_.reset();
        return make_error(k, "injected storage failure");
    }
    return Status::success();
}

Status MemoryStore::open() { return Status::success(); }

Status MemoryStore::secure_wipe() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    settings_.clear();
    account_.reset();
    sync_ = InboxSyncState{};
    messages_.clear();
    bodies_.clear();
    sessions_.clear();
    items_.clear();
    commands_.clear();
    command_order_.clear();
    events_.clear();
    token_blob_.reset();
    snapshot_.reset();
    fail_next_.reset();
    return Status::success();
}

Status MemoryStore::begin() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (snapshot_) return make_error(ErrorKind::Invalid, "nested transactions are not supported");
    auto s = std::make_shared<Snapshot>();
    s->settings = settings_;
    s->account = account_;
    s->sync = sync_;
    s->messages = messages_;
    s->bodies = bodies_;
    s->sessions = sessions_;
    s->items = items_;
    s->commands = commands_;
    s->command_order = command_order_;
    s->events = events_;
    s->token_blob = token_blob_;
    snapshot_ = std::move(s);
    return Status::success();
}

Status MemoryStore::commit() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!snapshot_) return make_error(ErrorKind::Invalid, "no active transaction");
    snapshot_.reset();
    return Status::success();
}

Status MemoryStore::rollback() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!snapshot_) return Status::success();  // nothing to undo
    Snapshot& s = *snapshot_;
    settings_ = std::move(s.settings);
    account_ = std::move(s.account);
    sync_ = std::move(s.sync);
    messages_ = std::move(s.messages);
    bodies_ = std::move(s.bodies);
    sessions_ = std::move(s.sessions);
    items_ = std::move(s.items);
    commands_ = std::move(s.commands);
    command_order_ = std::move(s.command_order);
    events_ = std::move(s.events);
    token_blob_ = std::move(s.token_blob);
    snapshot_.reset();
    return Status::success();
}

std::optional<std::string> MemoryStore::get_setting(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = settings_.find(key);
    if (it == settings_.end()) return std::nullopt;
    return it->second;
}

Status MemoryStore::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    settings_[key] = value;
    return Status::success();
}

std::optional<AccountInfo> MemoryStore::get_account() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return account_;
}

Status MemoryStore::put_account(const AccountInfo& a) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    account_ = a;
    return Status::success();
}

InboxSyncState MemoryStore::get_sync_state() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return sync_;
}

Status MemoryStore::put_sync_state(const InboxSyncState& s) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    sync_ = s;
    return Status::success();
}

Status MemoryStore::upsert_message(const MessageSummary& m) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    if (m.id.empty()) return make_error(ErrorKind::Invalid, "message id is empty");
    auto it = messages_.find(m.id);
    if (it == messages_.end()) {
        CachedMessage c;
        c.summary = m;
        messages_.emplace(m.id, std::move(c));
    } else {
        it->second.summary = m;  // state and reason are local-only and preserved
    }
    return Status::success();
}

Status MemoryStore::set_message_state(const std::string& id, MessageState state, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    auto it = messages_.find(id);
    if (it == messages_.end()) return make_error(ErrorKind::NotFound, "message not in cache");
    it->second.state = state;
    it->second.reason = reason;
    return Status::success();
}

Status MemoryStore::mark_gone(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    auto it = messages_.find(id);
    if (it == messages_.end()) return Status::success();  // never cached: nothing to tombstone
    it->second.state = MessageState::GoneOnServer;
    return Status::success();
}

std::optional<MessageSummary> MemoryStore::get_message(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = messages_.find(id);
    if (it == messages_.end()) return std::nullopt;
    return it->second.summary;
}

std::optional<MessageState> MemoryStore::get_message_state(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = messages_.find(id);
    if (it == messages_.end()) return std::nullopt;
    return it->second.state;
}

std::vector<MessageSummary> MemoryStore::list_messages_in_state(MessageState state) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<MessageSummary> out;
    for (const auto& kv : messages_) {
        if (kv.second.state == state) out.push_back(kv.second.summary);
    }
    sort_newest_first(out);
    return out;
}

std::vector<MessageSummary> MemoryStore::list_queue() { return list_messages_in_state(MessageState::InQueue); }

Status MemoryStore::put_body(const MessageBody& b) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    bodies_[b.id] = b;
    return Status::success();
}

std::optional<MessageBody> MemoryStore::get_body(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = bodies_.find(id);
    if (it == bodies_.end()) return std::nullopt;
    return it->second;
}

Status MemoryStore::purge_tombstones() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    for (auto it = messages_.begin(); it != messages_.end();) {
        if (it->second.state == MessageState::GoneOnServer) {
            bodies_.erase(it->first);
            it = messages_.erase(it);
        } else {
            ++it;
        }
    }
    return Status::success();
}

std::size_t MemoryStore::count_messages() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return messages_.size();
}

Status MemoryStore::put_session(const SessionRecord& s) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    sessions_[s.id] = s;
    return Status::success();
}

std::optional<SessionRecord> MemoryStore::get_active_session() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const SessionRecord* best = nullptr;
    for (const auto& kv : sessions_) {
        if (kv.second.state == SessionState::Ended) continue;
        if (!best || kv.second.started_at > best->started_at) best = &kv.second;
    }
    if (!best) return std::nullopt;
    return *best;
}

std::optional<SessionRecord> MemoryStore::get_session(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}

Status MemoryStore::put_session_item(const SessionItem& item) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    for (auto& existing : items_) {
        if (existing.session_id == item.session_id && existing.message_id == item.message_id) {
            existing = item;
            return Status::success();
        }
    }
    items_.push_back(item);
    return Status::success();
}

std::vector<SessionItem> MemoryStore::list_session_items(const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<SessionItem> out;
    for (const auto& it : items_) {
        if (it.session_id == session_id) out.push_back(it);
    }
    return out;
}

Status MemoryStore::put_command(const Command& c) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    if (c.id.empty()) return make_error(ErrorKind::Invalid, "command id is empty");
    if (commands_.find(c.id) == commands_.end()) command_order_.push_back(c.id);
    commands_[c.id] = c;
    return Status::success();
}

std::optional<Command> MemoryStore::get_command(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = commands_.find(id);
    if (it == commands_.end()) return std::nullopt;
    return it->second;
}

std::vector<Command> MemoryStore::list_commands(CommandState state) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<Command> out;
    for (const auto& id : command_order_) {
        const Command& c = commands_.at(id);
        if (c.state == state) out.push_back(c);
    }
    return out;
}

std::vector<Command> MemoryStore::list_commands_for_message(const std::string& message_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<Command> out;
    for (const auto& id : command_order_) {
        const Command& c = commands_.at(id);
        if (c.message_id == message_id) out.push_back(c);
    }
    return out;
}

std::optional<Command> MemoryStore::find_command_by_idempotency_key(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (key.empty()) return std::nullopt;
    for (const auto& id : command_order_) {
        const Command& c = commands_.at(id);
        if (c.idempotency_key == key) return c;
    }
    return std::nullopt;
}

Status MemoryStore::put_event(const SessionEvent& e) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    for (auto& existing : events_) {
        if (!e.id.empty() && existing.id == e.id) {
            existing = e;
            return Status::success();
        }
    }
    events_.push_back(e);
    return Status::success();
}

std::vector<SessionEvent> MemoryStore::list_events(const std::string& session_id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<SessionEvent> out;
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {  // later insertions first on ties
        if (it->session_id == session_id) out.push_back(*it);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SessionEvent& a, const SessionEvent& b) { return a.timestamp > b.timestamp; });
    return out;
}

std::optional<std::string> MemoryStore::get_token_blob() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return token_blob_;
}

Status MemoryStore::put_token_blob(const std::string& blob) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (Status f = maybe_fail(); !f.ok()) return f;
    token_blob_ = blob;
    return Status::success();
}

Status MemoryStore::clear_token_blob() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    token_blob_.reset();
    return Status::success();
}

} // namespace slowmail::core
