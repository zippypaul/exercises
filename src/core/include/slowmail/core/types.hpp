// Slow Mail core types. Pure C++17, no Qt, no I/O.
// Every string is UTF-8. Every timestamp is UTC unless named *_mono (monotonic seconds).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace slowmail::core {

// ---------------------------------------------------------------------------
// Errors and outcomes
// ---------------------------------------------------------------------------
enum class ErrorKind {
    None,
    Network,     // transport failure, DNS, TLS, timeout
    Auth,        // 401 / token invalid / sign-in required
    Forbidden,   // 403 / consent or conditional access denied
    Throttled,   // 429 or 503 with Retry-After
    NotFound,    // 404 ErrorItemNotFound (message moved/deleted elsewhere)
    Conflict,    // 409 / 412 precondition failed
    Gone,        // 410 delta token expired; full resync required
    Server,      // other 5xx
    Malformed,   // response could not be parsed / violated contract
    Storage,     // local database failure
    Policy,      // blocked by local policy (e.g. cooldown)
    Cancelled,
    Invalid      // caller error (bad state transition, bad argument)
};

const char* to_string(ErrorKind k);

struct Error {
    ErrorKind kind = ErrorKind::None;
    std::string message;                    // human readable, MUST NOT contain tokens/subjects/bodies
    int http_status = 0;                    // 0 if not HTTP
    std::optional<int> retry_after_seconds; // from Retry-After when Throttled
    std::string code;                       // Graph error code e.g. "ErrorItemNotFound", or OAuth "authorization_pending"

    bool is_retryable() const;              // Network, Throttled, Server(5xx) -> true
};

template <class T>
class Outcome {
public:
    Outcome(T value) : value_(std::move(value)) {}
    Outcome(Error error) : error_(std::move(error)) {}

    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    const Error& error() const { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

// Outcome for operations without a payload.
class Status {
public:
    Status() = default;                                  // success
    Status(Error error) : error_(std::move(error)) {}    // failure
    static Status success() { return Status(); }

    bool ok() const { return error_.kind == ErrorKind::None; }
    explicit operator bool() const { return ok(); }
    const Error& error() const { return error_; }

private:
    Error error_;
};

// ---------------------------------------------------------------------------
// Mail domain
// ---------------------------------------------------------------------------
enum class FlagStatus { NotFlagged, Flagged, Complete };
const char* to_string(FlagStatus f);
FlagStatus flag_status_from_string(const std::string& s); // Graph values: notFlagged/flagged/complete

enum class WellKnownFolder { Inbox, Archive, DeletedItems };
const char* to_string(WellKnownFolder f);               // Graph well-known names: inbox/archive/deleteditems

struct EmailAddress {
    std::string name;     // display name, may be empty
    std::string address;  // may be empty for malformed senders
};

// What the queue list needs. Comes from delta sync ($select on delta).
struct MessageSummary {
    std::string id;                 // Graph id (immutable if the backend honoured IdType=ImmutableId)
    std::string conversation_id;
    std::string subject;            // sanitised: control chars stripped, length-bounded
    EmailAddress from;
    std::string received_at_iso;    // as received from server, e.g. 2026-10-20T01:23:45Z
    std::int64_t received_at_epoch = 0;
    bool is_read = false;
    FlagStatus flag_status = FlagStatus::NotFlagged;
    std::string body_preview;       // sanitised, <= 255 chars
    bool has_attachments = false;
    std::string change_key;         // Graph changeKey, used for conflict detection
    std::string parent_folder_id;   // raw folder id as reported by Graph
};

enum class AttachmentKind { File, Item, Reference, Unknown };
const char* to_string(AttachmentKind k);

// Metadata only. We NEVER download attachment bytes in MVP.
struct AttachmentMeta {
    std::string id;
    std::string name;          // sanitised, length-bounded
    std::string content_type;  // MIME, sanitised
    std::int64_t size = 0;     // bytes; -1 if unknown
    bool is_inline = false;
    AttachmentKind kind = AttachmentKind::Unknown;
};

struct MessageBody {
    std::string id;
    std::string text;                        // plain text, already sanitised (no HTML, no remote refs)
    bool was_html = false;                   // original content type was html
    bool truncated = false;                  // body exceeded local bound
    std::vector<AttachmentMeta> attachments; // may be empty even if has_attachments
};

// One page of a delta sync.
struct DeltaPage {
    std::vector<MessageSummary> changed;     // added or updated
    std::vector<std::string> removed_ids;    // @removed entries (deleted or moved out of folder)
    std::optional<std::string> next_link;    // follow to continue this sync
    std::optional<std::string> delta_link;   // present on the final page only
};

struct MoveResult {
    std::string new_id;        // id after move; equals old id when immutable ids are in effect
    std::string new_folder_id;
};

// ---------------------------------------------------------------------------
// Session / action model
// ---------------------------------------------------------------------------
enum class SessionState {
    NotStarted,
    AuthRequired,
    ReadyCached,     // cache loaded, no sync yet this session
    SyncEligible,    // cooldown elapsed (or first download / pro)
    Syncing,
    SyncPartial,     // network died mid-delta; resumable, cooldown NOT consumed
    Active,
    Offline,         // actions queue locally
    Ending,
    Ended
};
const char* to_string(SessionState s);

enum class MessageState {
    InQueue,
    DoPending,
    Archived,
    DeletePending,
    Deleted,
    DeferPending,
    Deferred,       // flagged on server, hidden locally
    GoneOnServer,   // removed by another client; never resurrected
    Failed          // last action failed non-retryably; shown with reason
};
const char* to_string(MessageState s);
MessageState message_state_from_string(const std::string& s);

enum class CommandType { Archive, Delete, Defer, Undefer, UndoArchive, UndoDelete };
const char* to_string(CommandType t);
CommandType command_type_from_string(const std::string& s);

enum class CommandState { Pending, InFlight, Confirmed, Failed, Abandoned };
const char* to_string(CommandState s);
CommandState command_state_from_string(const std::string& s);

struct Command {
    std::string id;               // uuid-ish, generated locally
    std::string session_id;
    std::string message_id;
    CommandType type = CommandType::Archive;
    std::string payload_json;     // type specific; "{}" when none
    std::int64_t created_at = 0;  // unix seconds (wall)
    CommandState state = CommandState::Pending;
    int attempts = 0;
    std::string last_error;       // redacted, human readable
    std::string server_result_json; // e.g. {"new_id": "..."} after confirmation
    std::string idempotency_key;  // stable across retries of the same logical action
};

struct SessionRecord {
    std::string id;
    std::int64_t started_at = 0;
    std::optional<std::int64_t> ended_at;
    std::optional<std::int64_t> initial_sync_at;
    SessionState state = SessionState::NotStarted;
};

struct SessionItem {
    std::string session_id;
    std::string message_id;
    MessageState initial_state = MessageState::InQueue;
    MessageState current_state = MessageState::InQueue;
    std::string suppression_reason;  // "deferred_by_slowmail" etc.; empty when visible
};

enum class EventType { Archived, Deleted, Deferred, Undeferred, UndoArchive, UndoDelete, Sent, Delegated, SyncCompleted, SyncFailed };
const char* to_string(EventType e);
EventType event_type_from_string(const std::string& s);

enum class ConfirmationState { Pending, Confirmed, Failed };
const char* to_string(ConfirmationState c);
ConfirmationState confirmation_state_from_string(const std::string& s);

// Session activity entry. display_snapshot captures subject/sender at action time so the
// activity view never needs to read Sent/Deleted folders from the server.
struct SessionEvent {
    std::string id;
    std::string session_id;
    std::string message_id;
    EventType type = EventType::Archived;
    std::int64_t timestamp = 0;
    ConfirmationState confirmation = ConfirmationState::Pending;
    std::string display_snapshot_json;  // {"subject":..., "from":..., "received_at_iso":...}
    std::string command_id;             // links back to the command
    std::string failure_reason;         // redacted
};

struct InboxSyncState {
    std::optional<std::string> delta_link;      // committed only after a COMPLETE sync
    std::optional<std::string> resume_next_link;// set while a sync is in progress (SyncPartial)
    std::optional<std::int64_t> last_complete_wall;  // unix seconds of last complete sync
    std::optional<double> last_complete_mono;        // monotonic seconds at that moment
    std::optional<std::string> last_complete_boot_id;// to know whether mono clock is comparable
    std::optional<std::int64_t> last_attempt_wall;
    int schema_version = 1;
};

struct AccountInfo {
    std::string tenant_id;
    std::string user_id;
    std::string display_address;  // treated as sensitive; never logged
    bool signed_in = false;
};

// Limits applied by sanitisation (see sanitize.hpp). Exposed so UI and tests agree.
struct Limits {
    static constexpr std::size_t kMaxSubject = 512;
    static constexpr std::size_t kMaxPreview = 255;
    static constexpr std::size_t kMaxBodyChars = 200000;
    static constexpr std::size_t kMaxAttachmentName = 255;
    static constexpr std::size_t kMaxAttachmentsListed = 100;
    static constexpr std::size_t kMaxAddress = 320;
};

} // namespace slowmail::core
