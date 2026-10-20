// Foundation tests: types, clock, log.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/log.hpp"
#include "slowmail/core/types.hpp"

using namespace slowmail::core;

// ---------------------------------------------------------------------------
// types.cpp
// ---------------------------------------------------------------------------
TEST(Types, ErrorKindNamesAreUniqueAndNonEmpty) {
    const ErrorKind all[] = {ErrorKind::None,     ErrorKind::Network,  ErrorKind::Auth,      ErrorKind::Forbidden,
                             ErrorKind::Throttled, ErrorKind::NotFound, ErrorKind::Conflict,  ErrorKind::Gone,
                             ErrorKind::Server,   ErrorKind::Malformed, ErrorKind::Storage,  ErrorKind::Policy,
                             ErrorKind::Cancelled, ErrorKind::Invalid};
    std::set<std::string> names;
    for (auto k : all) {
        std::string n = to_string(k);
        EXPECT_FALSE(n.empty());
        EXPECT_NE(n, "unknown");
        names.insert(n);
    }
    EXPECT_EQ(names.size(), sizeof(all) / sizeof(all[0]));
}

TEST(Types, ErrorRetryable) {
    auto mk = [](ErrorKind k) {
        Error e;
        e.kind = k;
        return e;
    };
    EXPECT_TRUE(mk(ErrorKind::Network).is_retryable());
    EXPECT_TRUE(mk(ErrorKind::Throttled).is_retryable());
    EXPECT_TRUE(mk(ErrorKind::Server).is_retryable());
    for (auto k : {ErrorKind::None, ErrorKind::Auth, ErrorKind::Forbidden, ErrorKind::NotFound, ErrorKind::Conflict,
                   ErrorKind::Gone, ErrorKind::Malformed, ErrorKind::Storage, ErrorKind::Policy, ErrorKind::Cancelled,
                   ErrorKind::Invalid}) {
        EXPECT_FALSE(mk(k).is_retryable()) << to_string(k);
    }
}

TEST(Types, FlagStatusRoundTripAndGraphNames) {
    EXPECT_STREQ(to_string(FlagStatus::NotFlagged), "notFlagged");
    EXPECT_STREQ(to_string(FlagStatus::Flagged), "flagged");
    EXPECT_STREQ(to_string(FlagStatus::Complete), "complete");
    for (auto f : {FlagStatus::NotFlagged, FlagStatus::Flagged, FlagStatus::Complete}) {
        EXPECT_EQ(flag_status_from_string(to_string(f)), f);
    }
    EXPECT_EQ(flag_status_from_string("bogus"), FlagStatus::NotFlagged);
    EXPECT_EQ(flag_status_from_string(""), FlagStatus::NotFlagged);
}

TEST(Types, WellKnownFolderNamesMatchGraph) {
    EXPECT_STREQ(to_string(WellKnownFolder::Inbox), "inbox");
    EXPECT_STREQ(to_string(WellKnownFolder::Archive), "archive");
    EXPECT_STREQ(to_string(WellKnownFolder::DeletedItems), "deleteditems");
}

TEST(Types, AttachmentKindAndSessionStateNames) {
    std::set<std::string> names;
    for (auto k : {AttachmentKind::File, AttachmentKind::Item, AttachmentKind::Reference, AttachmentKind::Unknown})
        names.insert(to_string(k));
    EXPECT_EQ(names.size(), 4u);
    names.clear();
    for (auto s : {SessionState::NotStarted, SessionState::AuthRequired, SessionState::ReadyCached,
                   SessionState::SyncEligible, SessionState::Syncing, SessionState::SyncPartial, SessionState::Active,
                   SessionState::Offline, SessionState::Ending, SessionState::Ended}) {
        std::string n = to_string(s);
        EXPECT_NE(n, "unknown");
        names.insert(n);
    }
    EXPECT_EQ(names.size(), 10u);
}

TEST(Types, MessageStateRoundTrip) {
    for (auto s : {MessageState::InQueue, MessageState::DoPending, MessageState::Archived, MessageState::DeletePending,
                   MessageState::Deleted, MessageState::DeferPending, MessageState::Deferred,
                   MessageState::GoneOnServer, MessageState::Failed}) {
        EXPECT_EQ(message_state_from_string(to_string(s)), s) << to_string(s);
    }
    EXPECT_EQ(message_state_from_string("nonsense"), MessageState::InQueue);
}

TEST(Types, CommandTypeRoundTrip) {
    for (auto t : {CommandType::Archive, CommandType::Delete, CommandType::Defer, CommandType::Undefer,
                   CommandType::UndoArchive, CommandType::UndoDelete}) {
        EXPECT_EQ(command_type_from_string(to_string(t)), t) << to_string(t);
    }
    EXPECT_EQ(command_type_from_string("nonsense"), CommandType::Archive);
}

TEST(Types, CommandStateRoundTrip) {
    for (auto s : {CommandState::Pending, CommandState::InFlight, CommandState::Confirmed, CommandState::Failed,
                   CommandState::Abandoned}) {
        EXPECT_EQ(command_state_from_string(to_string(s)), s) << to_string(s);
    }
    EXPECT_EQ(command_state_from_string(""), CommandState::Pending);
}

TEST(Types, EventTypeRoundTrip) {
    for (auto e : {EventType::Archived, EventType::Deleted, EventType::Deferred, EventType::Undeferred,
                   EventType::UndoArchive, EventType::UndoDelete, EventType::Sent, EventType::Delegated,
                   EventType::SyncCompleted, EventType::SyncFailed}) {
        EXPECT_EQ(event_type_from_string(to_string(e)), e) << to_string(e);
    }
    EXPECT_EQ(event_type_from_string("nonsense"), EventType::Archived);
}

TEST(Types, ConfirmationStateRoundTrip) {
    for (auto c : {ConfirmationState::Pending, ConfirmationState::Confirmed, ConfirmationState::Failed}) {
        EXPECT_EQ(confirmation_state_from_string(to_string(c)), c) << to_string(c);
    }
    EXPECT_EQ(confirmation_state_from_string("???"), ConfirmationState::Pending);
}

TEST(Types, OutcomeAndStatus) {
    Outcome<int> ok(5);
    EXPECT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), 5);
    Error e;
    e.kind = ErrorKind::Network;
    Outcome<int> bad(e);
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().kind, ErrorKind::Network);
    EXPECT_TRUE(Status::success().ok());
    EXPECT_FALSE(Status(e).ok());
}

// ---------------------------------------------------------------------------
// clock.cpp
// ---------------------------------------------------------------------------
TEST(Clock, ParseBasicAndKnownEpoch) {
    std::int64_t t = 0;
    ASSERT_TRUE(parse_iso8601_utc("1970-01-01T00:00:00Z", t));
    EXPECT_EQ(t, 0);
    ASSERT_TRUE(parse_iso8601_utc("2000-03-01T00:00:00Z", t));
    EXPECT_EQ(t, 951868800);
    ASSERT_TRUE(parse_iso8601_utc("2026-10-20T01:23:45Z", t));
    EXPECT_EQ(t, 1792459425);  // date -u -d 2026-10-20T01:23:45Z +%s
    EXPECT_EQ(format_iso8601_utc(t), "2026-10-20T01:23:45Z");
}

TEST(Clock, ParseFractionalAndOffset) {
    std::int64_t a = 0, b = 0, c = 0, d = 0;
    ASSERT_TRUE(parse_iso8601_utc("2026-10-20T01:23:45Z", a));
    ASSERT_TRUE(parse_iso8601_utc("2026-10-20T01:23:45.678Z", b));
    ASSERT_TRUE(parse_iso8601_utc("2026-10-20T01:23:45+00:00", c));
    ASSERT_TRUE(parse_iso8601_utc("2026-10-20T01:23:45.1234567+00:00", d));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(a, d);
}

TEST(Clock, ParseLeapDay) {
    std::int64_t t = 0;
    EXPECT_TRUE(parse_iso8601_utc("2028-02-29T12:00:00Z", t));
    EXPECT_FALSE(parse_iso8601_utc("2027-02-29T12:00:00Z", t));
    EXPECT_FALSE(parse_iso8601_utc("2100-02-29T12:00:00Z", t));
    EXPECT_TRUE(parse_iso8601_utc("2000-02-29T12:00:00Z", t));
}

TEST(Clock, ParseRejects) {
    std::int64_t t = 12345;
    const char* bad[] = {"",
                         "garbage",
                         "2026-10-20",
                         "2026-10-20T01:23:45",           // no zone
                         "2026-10-20 01:23:45Z",          // space instead of T
                         "2026-10-20T01:23:45+02:00",     // non-UTC offset
                         "2026-10-20T01:23:45-05:00",
                         "2026-10-20T01:23:45+0000",      // no colon
                         "2026-10-20T01:23:45Zjunk",
                         "2026-10-20T01:23:45Z ",
                         " 2026-10-20T01:23:45Z",
                         "2026-13-20T01:23:45Z",          // month
                         "2026-00-20T01:23:45Z",
                         "2026-10-32T01:23:45Z",          // day
                         "2026-10-00T01:23:45Z",
                         "2026-10-20T24:00:00Z",          // hour
                         "2026-10-20T01:60:00Z",          // minute
                         "2026-10-20T01:23:60Z",          // second
                         "2026-10-20T01:23:45.Z",         // empty fraction
                         "2026-10-20t01:23:45z",          // lower case
                         "26-10-20T01:23:45Z",
                         "2026-10-20T1:23:45Z",
                         "2026-10-20T01:23:45+",
                         "1969-12-31T23:59:59Z",
                         "+2026-10-20T01:23:45Z"};
    for (const char* s : bad) {
        EXPECT_FALSE(parse_iso8601_utc(s, t)) << "accepted: '" << s << "'";
    }
    EXPECT_EQ(t, 12345) << "output must be untouched on failure";
    std::string with_nul("2026-10-20T01:23:45Z");
    with_nul.push_back('\0');
    EXPECT_FALSE(parse_iso8601_utc(with_nul, t));
}

TEST(Clock, FormatRoundTripManyValues) {
    for (std::int64_t e : {std::int64_t{0}, std::int64_t{1}, std::int64_t{86399}, std::int64_t{86400},
                           std::int64_t{951782400}, std::int64_t{1792459425}, std::int64_t{4102444799}}) {
        std::int64_t back = -1;
        ASSERT_TRUE(parse_iso8601_utc(format_iso8601_utc(e), back)) << e;
        EXPECT_EQ(back, e);
    }
    EXPECT_EQ(format_iso8601_utc(0), "1970-01-01T00:00:00Z");
    EXPECT_EQ(format_iso8601_utc(86399), "1970-01-01T23:59:59Z");
    EXPECT_EQ(format_iso8601_utc(951782400), "2000-02-29T00:00:00Z");
}

TEST(Clock, SystemClockIsSane) {
    SystemClock c;
    const auto w = c.now_wall();
    EXPECT_GT(w, 1'700'000'000);
    const double m1 = c.now_mono();
    const double m2 = c.now_mono();
    EXPECT_GE(m2, m1);
    const std::string b = c.boot_id();
    EXPECT_FALSE(b.empty());
    EXPECT_EQ(b, c.boot_id());
    SystemClock other;
    EXPECT_EQ(b, other.boot_id());
}

TEST(Clock, FakeClockControls) {
    FakeClock c(1000, 50.0, "b1");
    c.advance(10);
    EXPECT_EQ(c.now_wall(), 1010);
    EXPECT_DOUBLE_EQ(c.now_mono(), 60.0);
    c.set_wall(5);
    EXPECT_EQ(c.now_wall(), 5);
    EXPECT_DOUBLE_EQ(c.now_mono(), 60.0);
    c.reboot("b2", 1.0);
    EXPECT_EQ(c.boot_id(), "b2");
    EXPECT_DOUBLE_EQ(c.now_mono(), 1.0);
}

// ---------------------------------------------------------------------------
// log.cpp
// ---------------------------------------------------------------------------
TEST(Log, RedactEmailKeepsFirstCharAndTld) {
    EXPECT_EQ(redact("mail from paul@example.com today"), "mail from p***@***.com today");
    EXPECT_EQ(redact("first.last+tag@sub.corp.co.uk."), "f***@***.uk.");
    EXPECT_EQ(redact("<alice@x.org>"), "<a***@***.org>");
    EXPECT_EQ(redact("a@b.io and c@d.net"), "a***@***.io and c***@***.net");
    EXPECT_EQ(redact("no address here, 5 @ 6"), "no address here, 5 @ 6");
    EXPECT_EQ(redact("@"), "@");
    EXPECT_EQ(redact("foo@"), "foo@");
    EXPECT_EQ(redact("@bar.com"), "@bar.com");
}

TEST(Log, RedactBearerTokens) {
    EXPECT_EQ(redact("Authorization: Bearer abc123.def-456_ghi"), "Authorization: Bearer [REDACTED]");
    EXPECT_EQ(redact("bearer   SECRETVALUE== done"), "Bearer [REDACTED] done");
    EXPECT_EQ(redact("BEARER tok"), "Bearer [REDACTED]");
    EXPECT_EQ(redact("Bearer"), "Bearer");
}

TEST(Log, RedactJwt) {
    const std::string jwt = "eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dGhpc2lzYXNpZ25hdHVyZQ";
    std::string r = redact("token=" + jwt + " end");
    EXPECT_EQ(r.find("eyJ"), std::string::npos) << r;
    EXPECT_NE(r.find("[REDACTED_JWT]"), std::string::npos) << r;
    EXPECT_EQ(redact("see " + jwt + "."), "see [REDACTED_JWT].");
    // hostnames and versions must survive
    EXPECT_EQ(redact("GET graph.microsoft.com/v1.0/me"), "GET graph.microsoft.com/v1.0/me");
    EXPECT_EQ(redact("login.microsoftonline.com"), "login.microsoftonline.com");
    EXPECT_EQ(redact("version 1.2.3"), "version 1.2.3");
}

TEST(Log, RedactKeyedSecrets) {
    EXPECT_EQ(redact("access_token=abc123&expires=5"), "access_token=[REDACTED]&expires=5");
    EXPECT_EQ(redact("{\"access_token\":\"AbCdEf\",\"x\":1}"), "{\"access_token\":\"[REDACTED]\",\"x\":1}");
    EXPECT_EQ(redact("{\"refresh_token\": \"0.AAAA\"}"), "{\"refresh_token\": \"[REDACTED]\"}");
    EXPECT_EQ(redact("device_code=GMMhmHCXhWEzkobqIHGG_EnNYYsAkukHspeYUk9E8 more"),
              "device_code=[REDACTED] more");
    EXPECT_EQ(redact("ACCESS_TOKEN: xyz"), "ACCESS_TOKEN: [REDACTED]");
    EXPECT_EQ(redact("id_token=eyJ.a.b"), "id_token=[REDACTED]");
    EXPECT_EQ(redact("the device_code expired"), "the device_code expired");  // no separator, no value
    EXPECT_EQ(redact("refresh_token="), "refresh_token=");
}

TEST(Log, RedactLeavesPlainTextAndIsLinear) {
    EXPECT_EQ(redact(""), "");
    EXPECT_EQ(redact("synced 12 messages in 3 pages"), "synced 12 messages in 3 pages");
    EXPECT_EQ(redact("id=AAMkAGI2THVSAAA="), "id=AAMkAGI2THVSAAA=");
    std::string big(2'000'000, '@');
    EXPECT_EQ(redact(big), big);
    std::string dots(1'000'000, '.');
    EXPECT_EQ(redact(dots), dots);
}

TEST(Log, SinkReceivesRedactedMessage) {
    std::vector<std::pair<LogLevel, std::string>> got;
    set_log_sink([&](LogLevel l, const std::string& m) { got.emplace_back(l, m); });
    SLOWMAIL_LOG_INFO("user paul@example.com signed in, access_token=SECRET1");
    SLOWMAIL_LOG_WARN("plain");
    set_log_sink(nullptr);  // restore default stderr sink
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].first, LogLevel::Info);
    EXPECT_EQ(got[0].second, "user p***@***.com signed in, access_token=[REDACTED]");
    EXPECT_EQ(got[1].first, LogLevel::Warn);
    EXPECT_EQ(got[1].second, "plain");
}
