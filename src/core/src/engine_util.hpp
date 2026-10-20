// Private helpers shared by sync_engine.cpp, command_processor.cpp and triage.cpp. Not installed.
#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "slowmail/core/store.hpp"
#include "slowmail/core/types.hpp"

namespace slowmail::core::detail {

#define SLOWMAIL_TRY(expr)                        \
    do {                                          \
        ::slowmail::core::Status try_st_ = (expr); \
        if (!try_st_.ok()) return try_st_;        \
    } while (0)

inline Status make_status(ErrorKind kind, const std::string& message) {
    Error e;
    e.kind = kind;
    e.message = message;
    return Status(e);
}

// {"subject":..., "from":<name>, "from_address":..., "received_at_iso":...}; never logged.
inline std::string make_snapshot_json(const MessageSummary& m) {
    nlohmann::json j;
    j["subject"] = m.subject;
    j["from"] = m.from.name.empty() ? m.from.address : m.from.name;
    j["from_address"] = m.from.address;
    j["received_at_iso"] = m.received_at_iso;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Records the per-session state of a message. initial_if_new is used when no item exists yet.
inline Status touch_session_item(Store& store, const std::string& session_id, const std::string& message_id,
                                 MessageState initial_if_new, MessageState current, const std::string& reason) {
    if (session_id.empty()) return Status::success();
    SessionItem item;
    item.session_id = session_id;
    item.message_id = message_id;
    item.initial_state = initial_if_new;
    for (const auto& existing : store.list_session_items(session_id)) {
        if (existing.message_id == message_id) {
            item.initial_state = existing.initial_state;
            break;
        }
    }
    item.current_state = current;
    item.suppression_reason = reason;
    return store.put_session_item(item);
}

// Updates the activity event that belongs to a command, if there is one.
inline Status update_event_for_command(Store& store, const std::string& session_id, const std::string& command_id,
                                       ConfirmationState confirmation, const std::string& failure_reason) {
    if (command_id.empty()) return Status::success();
    for (SessionEvent e : store.list_events(session_id)) {
        if (e.command_id == command_id) {
            e.confirmation = confirmation;
            e.failure_reason = failure_reason;
            return store.put_event(e);
        }
    }
    return Status::success();
}

} // namespace slowmail::core::detail
