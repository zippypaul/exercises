// In-memory MailBackend for unit tests. Header-only. Not thread-safe.
//
// Delta model: a change log with a cursor.
//   ""                       -> full sync: current inbox snapshot, pages of `page_size`
//   "full:<gen>:<off>:<cur>"  -> next page of a full sync (cur = log cursor when the sync started)
//   "delta:<gen>:<cur>"       -> changes logged since cursor `cur`
//   "inc:<gen>:<cur>:<end>:<off>" -> next page of an incremental sync
// A link whose generation differs from the current one yields ErrorKind::Gone (expire_delta_links()).
#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "slowmail/core/mail_backend.hpp"

namespace slowmail::testing {

using namespace slowmail::core;

class FakeBackend final : public MailBackend {
public:
    struct Call {
        std::string op;      // "delta", "body", "move", "flag", "account"
        std::string arg;     // message id, link, ...
        std::string extra;   // destination folder / flag value
        bool ok = true;
    };

    // ---- observable state ---------------------------------------------------------------------
    std::map<std::string, MessageSummary> inbox;
    std::map<std::string, MessageSummary> archive;
    std::map<std::string, MessageSummary> deleted;
    std::map<std::string, std::string> bodies;  // optional body text overrides
    std::vector<Call> calls;

    // ---- knobs --------------------------------------------------------------------------------
    bool offline = false;          // every call fails with Network
    bool signed_in = true;
    bool immutable_ids = true;     // false: move() assigns a new id
    std::size_t page_size = 50;
    // Test hook: runs right after a move()/set_flag() succeeded on the "server", before the caller sees the result.
    std::function<void()> after_success;
    // Test hook: entries appended to the NEXT incremental delta page, then cleared (stale / out-of-folder / removed
    // entries that a real server may include).
    std::vector<MessageSummary> extra_changed;
    std::vector<std::string> extra_removed;
    // Test hook: runs when a scripted failure (fail_next) is delivered, before the caller sees the error.
    std::function<void()> on_injected_failure;

    // ---- fixtures -----------------------------------------------------------------------------
    static MessageSummary make_message(const std::string& id, const std::string& subject = "",
                                       std::int64_t received_epoch = 1'799'990'000) {
        MessageSummary m;
        m.id = id;
        m.conversation_id = "conv-" + id;
        m.subject = subject.empty() ? "Subject " + id : subject;
        m.from.name = "Sender " + id;
        m.from.address = id + "@example.test";
        m.received_at_epoch = received_epoch;
        m.received_at_iso = "2026-10-20T00:00:00Z";
        m.body_preview = "Preview " + id;
        m.change_key = "ck-1";
        m.parent_folder_id = "inbox";
        return m;
    }

    // Adds a message to the server inbox (as if it arrived) and logs the change.
    MessageSummary& add_message(const std::string& id, const std::string& subject = "",
                                std::int64_t received_epoch = 1'799'990'000) {
        MessageSummary m = make_message(id, subject, received_epoch);
        inbox[id] = m;
        log_upsert(id);
        return inbox[id];
    }

    // ---- scripted failures --------------------------------------------------------------------
    // Fail the next `times` matching calls. `op` empty = any operation.
    void fail_next(ErrorKind kind, std::optional<int> retry_after = std::nullopt, const std::string& op = "",
                   int times = 1, const std::string& code = "") {
        failures_.push_back({kind, retry_after, op, times, code});
    }
    // Fail the K-th (1-based) fetch_inbox_delta call counted from now; one-shot.
    void fail_delta_at_call(int k, ErrorKind kind, std::optional<int> retry_after = std::nullopt) {
        delta_calls_ = 0;
        delta_fail_at_ = k;
        delta_fail_kind_ = kind;
        delta_fail_retry_ = retry_after;
    }
    // All previously issued delta/next links now answer Gone.
    void expire_delta_links() { ++generation_; }

    // ---- another client ------------------------------------------------------------------------
    // These do not record calls and cannot be failed.
    void other_client_move(const std::string& id, WellKnownFolder dest) { relocate(id, dest); }
    void other_client_delete(const std::string& id) { other_client_move(id, WellKnownFolder::DeletedItems); }
    void other_client_set_flag(const std::string& id, FlagStatus f) { apply_flag(id, f); }
    void other_client_unflag(const std::string& id) { apply_flag(id, FlagStatus::NotFlagged); }
    void other_client_flag(const std::string& id) { apply_flag(id, FlagStatus::Flagged); }
    // Hard removal from every folder.
    void other_client_purge(const std::string& id) {
        if (inbox.erase(id)) log_remove(id);
        archive.erase(id);
        deleted.erase(id);
    }
    void other_client_update(const std::string& id, const std::string& subject) {
        if (auto it = inbox.find(id); it != inbox.end()) {
            it->second.subject = subject;
            it->second.change_key += "+";
            log_upsert(id);
        }
    }

    // ---- inspection helpers --------------------------------------------------------------------
    int count(const std::string& op, const std::string& arg = "") const {
        int n = 0;
        for (const auto& c : calls) {
            if (c.op == op && (arg.empty() || c.arg == arg)) ++n;
        }
        return n;
    }
    bool in_inbox(const std::string& id) const { return inbox.count(id) != 0; }
    bool in_archive(const std::string& id) const { return archive.count(id) != 0; }
    bool in_deleted(const std::string& id) const { return deleted.count(id) != 0; }
    FlagStatus flag_of(const std::string& id) const {
        for (const auto* m : {&inbox, &archive, &deleted}) {
            auto it = m->find(id);
            if (it != m->end()) return it->second.flag_status;
        }
        return FlagStatus::NotFlagged;
    }
    void clear_calls() { calls.clear(); }

    // ---- MailBackend --------------------------------------------------------------------------
    Outcome<DeltaPage> fetch_inbox_delta(const std::string& link) override {
        ++delta_calls_;
        if (auto e = injected("delta", link)) return *e;
        if (delta_fail_at_ > 0 && delta_calls_ == delta_fail_at_) {
            delta_fail_at_ = 0;
            return fail_<DeltaPage>("delta", link, "", make_error(delta_fail_kind_, delta_fail_retry_, ""));
        }
        if (offline) return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Network, std::nullopt, ""));

        std::vector<std::string> p = split(link);
        DeltaPage page;
        const std::size_t n = std::max<std::size_t>(1, page_size);

        if (link.empty() || (!p.empty() && p[0] == "full")) {
            std::size_t offset = 0;
            std::size_t cursor0 = log_.size();
            if (!link.empty()) {
                if (p.size() != 4) return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Malformed, std::nullopt, ""));
                if (stoi_(p[1]) != generation_) return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Gone, std::nullopt, "SyncStateNotFound"));
                offset = static_cast<std::size_t>(stoi_(p[2]));
                cursor0 = static_cast<std::size_t>(stoi_(p[3]));
            }
            std::vector<const MessageSummary*> items;
            for (const auto& kv : inbox) items.push_back(&kv.second);
            const std::size_t end = std::min(items.size(), offset + n);
            for (std::size_t i = offset; i < end; ++i) page.changed.push_back(*items[i]);
            if (end < items.size()) page.next_link = "full:" + std::to_string(generation_) + ":" + std::to_string(end) + ":" + std::to_string(cursor0);
            else page.delta_link = "delta:" + std::to_string(generation_) + ":" + std::to_string(cursor0);
            calls.push_back({"delta", link, "", true});
            return page;
        }

        std::size_t start = 0, end_cursor = 0, offset = 0;
        if (p[0] == "delta" && p.size() == 3) {
            if (stoi_(p[1]) != generation_) return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Gone, std::nullopt, "SyncStateNotFound"));
            start = static_cast<std::size_t>(stoi_(p[2]));
            end_cursor = log_.size();
        } else if (p[0] == "inc" && p.size() == 5) {
            if (stoi_(p[1]) != generation_) return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Gone, std::nullopt, "SyncStateNotFound"));
            start = static_cast<std::size_t>(stoi_(p[2]));
            end_cursor = static_cast<std::size_t>(stoi_(p[3]));
            offset = static_cast<std::size_t>(stoi_(p[4]));
        } else {
            return fail_<DeltaPage>("delta", link, "", make_error(ErrorKind::Malformed, std::nullopt, ""));
        }

        // Net effect per id, in first-appearance order.
        std::vector<std::string> order;
        std::map<std::string, bool> removed;  // id -> last op was a removal
        for (std::size_t i = start; i < end_cursor && i < log_.size(); ++i) {
            if (!removed.count(log_[i].id)) order.push_back(log_[i].id);
            removed[log_[i].id] = log_[i].remove;
        }
        const std::size_t end = std::min(order.size(), offset + n);
        for (std::size_t i = offset; i < end; ++i) {
            const std::string& id = order[i];
            auto it = inbox.find(id);
            if (removed[id] || it == inbox.end()) page.removed_ids.push_back(id);
            else page.changed.push_back(it->second);
        }
        for (auto& m : extra_changed) page.changed.push_back(m);
        for (auto& id : extra_removed) page.removed_ids.push_back(id);
        extra_changed.clear();
        extra_removed.clear();
        if (end < order.size()) page.next_link = "inc:" + std::to_string(generation_) + ":" + std::to_string(start) + ":" + std::to_string(end_cursor) + ":" + std::to_string(end);
        else page.delta_link = "delta:" + std::to_string(generation_) + ":" + std::to_string(end_cursor);
        calls.push_back({"delta", link, "", true});
        return page;
    }

    Outcome<MessageBody> fetch_body(const std::string& message_id) override {
        if (auto e = injected("body", message_id)) return *e;
        if (offline) return fail_<MessageBody>("body", message_id, "", make_error(ErrorKind::Network, std::nullopt, ""));
        if (!find(message_id)) return fail_<MessageBody>("body", message_id, "", make_error(ErrorKind::NotFound, std::nullopt, "ErrorItemNotFound"));
        MessageBody b;
        b.id = message_id;
        auto it = bodies.find(message_id);
        b.text = it != bodies.end() ? it->second : "Body of " + message_id;
        calls.push_back({"body", message_id, "", true});
        return b;
    }

    Outcome<MoveResult> move(const std::string& message_id, WellKnownFolder destination) override {
        const std::string dest = to_string(destination);
        if (auto e = injected("move", message_id, dest)) return *e;
        if (offline) return fail_<MoveResult>("move", message_id, dest, make_error(ErrorKind::Network, std::nullopt, ""));
        if (!find(message_id)) return fail_<MoveResult>("move", message_id, dest, make_error(ErrorKind::NotFound, std::nullopt, "ErrorItemNotFound"));
        MoveResult r = relocate(message_id, destination);
        calls.push_back({"move", message_id, dest, true});
        if (after_success) after_success();
        return r;
    }

    Status set_flag(const std::string& message_id, FlagStatus status) override {
        const std::string val = to_string(status);
        if (auto e = injected("flag", message_id, val)) return Status(e->error());
        if (offline) { Error e = make_error(ErrorKind::Network, std::nullopt, ""); calls.push_back({"flag", message_id, val, false}); return Status(e); }
        if (!find(message_id)) { Error e = make_error(ErrorKind::NotFound, std::nullopt, "ErrorItemNotFound"); calls.push_back({"flag", message_id, val, false}); return Status(e); }
        apply_flag(message_id, status);
        calls.push_back({"flag", message_id, val, true});
        if (after_success) after_success();
        return Status::success();
    }

    Outcome<AccountInfo> account() override {
        calls.push_back({"account", "", "", true});
        AccountInfo a;
        a.tenant_id = "tenant";
        a.user_id = "user";
        a.display_address = "user@example.test";
        a.signed_in = signed_in;
        return a;
    }

private:
    struct Change { std::string id; bool remove = false; };
    struct Failure { ErrorKind kind; std::optional<int> retry_after; std::string op; int times; std::string code; };

    std::vector<Change> log_;
    int generation_ = 1;
    int moves_ = 0;
    int delta_calls_ = 0;
    int delta_fail_at_ = 0;
    ErrorKind delta_fail_kind_ = ErrorKind::Network;
    std::optional<int> delta_fail_retry_;
    std::vector<Failure> failures_;

    static int stoi_(const std::string& s) { try { return std::stoi(s); } catch (...) { return -1; } }
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, ':')) out.push_back(part);
        return out;
    }
    static Error make_error(ErrorKind k, std::optional<int> retry, const std::string& code) {
        Error e;
        e.kind = k;
        e.retry_after_seconds = retry;
        e.code = code;
        e.message = std::string("fake backend: ") + to_string(k);
        return e;
    }
    template <class T>
    Outcome<T> fail_(const std::string& op, const std::string& arg, const std::string& extra, Error e) {
        calls.push_back({op, arg, extra, false});
        return Outcome<T>(std::move(e));
    }

    // Returns an error outcome (convertible to any Outcome<T> via Error) when a scripted failure matches.
    struct InjectedError {
        Error error_;
        operator Outcome<DeltaPage>() const { return Outcome<DeltaPage>(error_); }
        operator Outcome<MessageBody>() const { return Outcome<MessageBody>(error_); }
        operator Outcome<MoveResult>() const { return Outcome<MoveResult>(error_); }
        const Error& error() const { return error_; }
    };
    std::optional<InjectedError> injected(const std::string& op, const std::string& arg, const std::string& extra = "") {
        for (auto it = failures_.begin(); it != failures_.end(); ++it) {
            if (!it->op.empty() && it->op != op) continue;
            Error e = make_error(it->kind, it->retry_after, it->code);
            if (--it->times <= 0) failures_.erase(it);
            calls.push_back({op, arg, extra, false});
            if (on_injected_failure) on_injected_failure();
            return InjectedError{e};
        }
        return std::nullopt;
    }

    MessageSummary* find(const std::string& id) {
        for (auto* m : {&inbox, &archive, &deleted}) {
            auto it = m->find(id);
            if (it != m->end()) return &it->second;
        }
        return nullptr;
    }
    void log_upsert(const std::string& id) { log_.push_back({id, false}); }
    void log_remove(const std::string& id) { log_.push_back({id, true}); }

    void apply_flag(const std::string& id, FlagStatus f) {
        MessageSummary* m = find(id);
        if (!m) return;
        m->flag_status = f;
        m->change_key += "+";
        if (inbox.count(id)) log_upsert(id);
    }

    MoveResult relocate(const std::string& id, WellKnownFolder dest) {
        MessageSummary m;
        bool was_inbox = false;
        if (auto it = inbox.find(id); it != inbox.end()) { m = it->second; inbox.erase(it); was_inbox = true; }
        else if (auto it2 = archive.find(id); it2 != archive.end()) { m = it2->second; archive.erase(it2); }
        else if (auto it3 = deleted.find(id); it3 != deleted.end()) { m = it3->second; deleted.erase(it3); }
        else return MoveResult{id, to_string(dest)};
        const std::string new_id = immutable_ids ? id : id + "~" + std::to_string(++moves_);
        m.id = new_id;
        m.parent_folder_id = to_string(dest);
        m.change_key += "+";
        if (was_inbox) log_remove(id);
        switch (dest) {
        case WellKnownFolder::Inbox: inbox[new_id] = m; log_upsert(new_id); break;
        case WellKnownFolder::Archive: archive[new_id] = m; break;
        case WellKnownFolder::DeletedItems: deleted[new_id] = m; break;
        }
        return MoveResult{new_id, to_string(dest)};
    }
};

} // namespace slowmail::testing
