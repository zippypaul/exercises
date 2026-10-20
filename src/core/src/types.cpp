// String conversions for core enums and Error helpers.
// Unknown inputs to *_from_string fall back to the documented default and never throw.
#include "slowmail/core/types.hpp"

#include <cstring>

namespace slowmail::core {

namespace {

template <class E, std::size_t N>
struct Entry {
    E value;
    const char* name;
};

template <class E, std::size_t N>
const char* name_of(const Entry<E, N> (&table)[N], E v, const char* fallback) {
    for (const auto& e : table) {
        if (e.value == v) return e.name;
    }
    return fallback;
}

template <class E, std::size_t N>
E value_of(const Entry<E, N> (&table)[N], const std::string& s, E fallback) {
    for (const auto& e : table) {
        if (s == e.name) return e.value;
    }
    return fallback;
}

const Entry<ErrorKind, 14> kErrorKinds[] = {
    {ErrorKind::None, "none"},         {ErrorKind::Network, "network"},     {ErrorKind::Auth, "auth"},
    {ErrorKind::Forbidden, "forbidden"}, {ErrorKind::Throttled, "throttled"}, {ErrorKind::NotFound, "not_found"},
    {ErrorKind::Conflict, "conflict"}, {ErrorKind::Gone, "gone"},           {ErrorKind::Server, "server"},
    {ErrorKind::Malformed, "malformed"}, {ErrorKind::Storage, "storage"},   {ErrorKind::Policy, "policy"},
    {ErrorKind::Cancelled, "cancelled"}, {ErrorKind::Invalid, "invalid"},
};

const Entry<FlagStatus, 3> kFlagStatuses[] = {
    {FlagStatus::NotFlagged, "notFlagged"},
    {FlagStatus::Flagged, "flagged"},
    {FlagStatus::Complete, "complete"},
};

const Entry<WellKnownFolder, 3> kFolders[] = {
    {WellKnownFolder::Inbox, "inbox"},
    {WellKnownFolder::Archive, "archive"},
    {WellKnownFolder::DeletedItems, "deleteditems"},
};

const Entry<AttachmentKind, 4> kAttachmentKinds[] = {
    {AttachmentKind::File, "file"},
    {AttachmentKind::Item, "item"},
    {AttachmentKind::Reference, "reference"},
    {AttachmentKind::Unknown, "unknown"},
};

const Entry<SessionState, 10> kSessionStates[] = {
    {SessionState::NotStarted, "not_started"}, {SessionState::AuthRequired, "auth_required"},
    {SessionState::ReadyCached, "ready_cached"}, {SessionState::SyncEligible, "sync_eligible"},
    {SessionState::Syncing, "syncing"},         {SessionState::SyncPartial, "sync_partial"},
    {SessionState::Active, "active"},           {SessionState::Offline, "offline"},
    {SessionState::Ending, "ending"},           {SessionState::Ended, "ended"},
};

const Entry<MessageState, 9> kMessageStates[] = {
    {MessageState::InQueue, "in_queue"},           {MessageState::DoPending, "do_pending"},
    {MessageState::Archived, "archived"},          {MessageState::DeletePending, "delete_pending"},
    {MessageState::Deleted, "deleted"},            {MessageState::DeferPending, "defer_pending"},
    {MessageState::Deferred, "deferred"},          {MessageState::GoneOnServer, "gone_on_server"},
    {MessageState::Failed, "failed"},
};

const Entry<CommandType, 6> kCommandTypes[] = {
    {CommandType::Archive, "archive"},         {CommandType::Delete, "delete"},
    {CommandType::Defer, "defer"},             {CommandType::Undefer, "undefer"},
    {CommandType::UndoArchive, "undo_archive"}, {CommandType::UndoDelete, "undo_delete"},
};

const Entry<CommandState, 5> kCommandStates[] = {
    {CommandState::Pending, "pending"},     {CommandState::InFlight, "in_flight"},
    {CommandState::Confirmed, "confirmed"}, {CommandState::Failed, "failed"},
    {CommandState::Abandoned, "abandoned"},
};

const Entry<EventType, 10> kEventTypes[] = {
    {EventType::Archived, "archived"},       {EventType::Deleted, "deleted"},
    {EventType::Deferred, "deferred"},       {EventType::Undeferred, "undeferred"},
    {EventType::UndoArchive, "undo_archive"}, {EventType::UndoDelete, "undo_delete"},
    {EventType::Sent, "sent"},               {EventType::Delegated, "delegated"},
    {EventType::SyncCompleted, "sync_completed"}, {EventType::SyncFailed, "sync_failed"},
};

const Entry<ConfirmationState, 3> kConfirmations[] = {
    {ConfirmationState::Pending, "pending"},
    {ConfirmationState::Confirmed, "confirmed"},
    {ConfirmationState::Failed, "failed"},
};

} // namespace

const char* to_string(ErrorKind k) { return name_of(kErrorKinds, k, "unknown"); }

bool Error::is_retryable() const {
    switch (kind) {
    case ErrorKind::Network:
    case ErrorKind::Throttled:
    case ErrorKind::Server:
        return true;
    default:
        return false;
    }
}

const char* to_string(FlagStatus f) { return name_of(kFlagStatuses, f, "notFlagged"); }
// Unknown -> NotFlagged (the safe, non-deferred reading).
FlagStatus flag_status_from_string(const std::string& s) { return value_of(kFlagStatuses, s, FlagStatus::NotFlagged); }

const char* to_string(WellKnownFolder f) { return name_of(kFolders, f, "inbox"); }
const char* to_string(AttachmentKind k) { return name_of(kAttachmentKinds, k, "unknown"); }
const char* to_string(SessionState s) { return name_of(kSessionStates, s, "unknown"); }

const char* to_string(MessageState s) { return name_of(kMessageStates, s, "unknown"); }
// Unknown -> InQueue (matches the default of SessionItem/CachedMessage).
MessageState message_state_from_string(const std::string& s) { return value_of(kMessageStates, s, MessageState::InQueue); }

const char* to_string(CommandType t) { return name_of(kCommandTypes, t, "unknown"); }
// Unknown -> Archive (default of Command).
CommandType command_type_from_string(const std::string& s) { return value_of(kCommandTypes, s, CommandType::Archive); }

const char* to_string(CommandState s) { return name_of(kCommandStates, s, "unknown"); }
// Unknown -> Pending (default of Command).
CommandState command_state_from_string(const std::string& s) { return value_of(kCommandStates, s, CommandState::Pending); }

const char* to_string(EventType e) { return name_of(kEventTypes, e, "unknown"); }
// Unknown -> Archived (default of SessionEvent).
EventType event_type_from_string(const std::string& s) { return value_of(kEventTypes, s, EventType::Archived); }

const char* to_string(ConfirmationState c) { return name_of(kConfirmations, c, "unknown"); }
// Unknown -> Pending (default of SessionEvent).
ConfirmationState confirmation_state_from_string(const std::string& s) { return value_of(kConfirmations, s, ConfirmationState::Pending); }

} // namespace slowmail::core
