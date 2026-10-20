#pragma once
#include <string>
#include <vector>

#include "slowmail/core/types.hpp"

namespace slowmail::core {

// Abstract mailbox backend. The only implementation in MVP is slowmail::graph::GraphBackend.
// Tests use an in-memory fake. All calls are synchronous and blocking; the UI runs them off-thread.
class MailBackend {
public:
    virtual ~MailBackend() = default;

    // Delta sync of the Inbox. Pass "" to start a fresh full sync; otherwise pass the exact
    // next_link or delta_link previously returned. Must request stable ids (ImmutableId) where supported.
    // ErrorKind::Gone means the link expired and the caller must restart with "".
    virtual Outcome<DeltaPage> fetch_inbox_delta(const std::string& link) = 0;

    // Fetch readable body text (plain) + attachment metadata. Never downloads attachment bytes.
    virtual Outcome<MessageBody> fetch_body(const std::string& message_id) = 0;

    // Soft actions. Delete == move to DeletedItems. Archive == move to Archive well-known folder.
    virtual Outcome<MoveResult> move(const std::string& message_id, WellKnownFolder destination) = 0;

    // PATCH flag. Idempotent.
    virtual Status set_flag(const std::string& message_id, FlagStatus status) = 0;

    // Current signed-in account (may be unauthenticated -> signed_in=false, no error).
    virtual Outcome<AccountInfo> account() = 0;
};

} // namespace slowmail::core
